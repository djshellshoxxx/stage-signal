#include "ss/persist/store.hpp"

#include <cstdlib>
#include "ss/base/sha256.hpp"

namespace ss::persist {
namespace {
constexpr const char* kMagic = "STAGESIGNAL-SAVE 1 ";
constexpr std::uint64_t kMaxFileBytes = model::Limits::kMaxJsonBytes + kEnvelopeSlackBytes;

bool parseField(const std::string& line, const std::string& key, std::string& out) {
    const std::string k = " " + key + "=";
    const auto p = line.find(k);
    if (p == std::string::npos) return false;
    const auto s = p + k.size();
    const auto e = line.find(' ', s);
    out = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
    return true;
}
Error corrupt() { return makeError(ErrorCategory::DecodeFailure, "save file is damaged or incomplete", Recoverability::OperatorAction); }
}  // namespace

std::string makeEnvelope(const std::string& jsonText, const std::string& kind, std::int64_t savedWallUnixMs) {
    return std::string(kMagic) + "kind=" + kind + " saved=" + std::to_string(savedWallUnixMs) + " bytes=" + std::to_string(jsonText.size()) +
           " sha256=" + Sha256::hashHex(jsonText) + "\n" + jsonText;
}

Result<std::string> openEnvelope(const std::string& bytes, EnvelopeInfo* info) {
    EnvelopeInfo local;
    EnvelopeInfo& inf = info ? *info : local;
    inf = EnvelopeInfo{};
    if (bytes.empty()) return corrupt();
    if (bytes[0] == '{') return bytes;   // raw JSON, validated by the parser
    if (bytes.compare(0, std::char_traits<char>::length(kMagic), kMagic) != 0) return corrupt();
    const auto nl = bytes.find('\n');
    if (nl == std::string::npos || nl > 512) return corrupt();
    const std::string line = bytes.substr(0, nl);
    std::string kind, saved, len, sha;
    if (!parseField(line, "kind", kind) || !parseField(line, "saved", saved) || !parseField(line, "bytes", len) || !parseField(line, "sha256", sha)) return corrupt();
    if (saved.empty() || saved.size() > 19 || len.empty() || len.size() > 12) return corrupt();
    const std::string body = bytes.substr(nl + 1);
    char* end = nullptr;
    const unsigned long long n = std::strtoull(len.c_str(), &end, 10);
    if (*end != '\0' || n != body.size()) return corrupt();
    if (Sha256::hashHex(body) != sha) return corrupt();
    inf.hasEnvelope = true; inf.kind = kind; inf.bytes = n; inf.sha256 = sha;
    inf.savedWallUnixMs = std::strtoll(saved.c_str(), nullptr, 10);
    return body;
}

Status atomicWriteFile(IFileSystem& fs, const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    if (auto st = fs.writeFile(tmp, content, true); !st.ok()) { fs.remove(tmp); return st; }
    auto back = fs.readFile(tmp, content.size() + 1);
    if (!back || back.value() != content) { fs.remove(tmp); return makeError(ErrorCategory::InternalFault, "write verification failed", Recoverability::Retryable); }
    if (auto st = fs.rename(tmp, path); !st.ok()) { fs.remove(tmp); return st; }
    return okStatus();
}

ShowStore::ShowStore(IFileSystem& fs, const IClock& clock, std::string path) : fs_(fs), clock_(clock), path_(std::move(path)) {}

void ShowStore::removeStaleTemps() {
    for (const auto& p : {path_ + ".tmp", backupPath(1) + ".tmp", recoveryPath() + ".tmp"}) fs_.remove(p);
}

Status ShowStore::rotateBackups(const std::string& previousBytes) {
    if (auto st = fs_.remove(backupPath(kKeepBackups)); !st.ok()) return st;
    for (int n = kKeepBackups - 1; n >= 1; --n)
        if (fs_.exists(backupPath(n)))
            if (auto st = fs_.rename(backupPath(n), backupPath(n + 1)); !st.ok()) return st;
    return atomicWriteFile(fs_, backupPath(1), previousBytes);
}

Status ShowStore::save(const model::ShowDocument& doc, const JsonWriteOptions& opt) {
    auto json = showToJson(doc, opt);
    if (!json) return json.error();
    removeStaleTemps();

    std::string previous;
    bool previousValid = false;
    if (fs_.exists(path_)) {
        auto prev = fs_.readFile(path_, kMaxFileBytes);
        if (prev) {
            auto body = openEnvelope(prev.value());
            if (body) {
                auto ver = peekSchema(body.value());
                if (ver) {
                    if (ver.value().major > model::kSchemaMajor || (ver.value().major == model::kSchemaMajor && ver.value().minor > model::kSchemaMinor))
                        return makeError(ErrorCategory::SchemaUnsupported, "existing file uses a newer format and will not be overwritten; use Save As", Recoverability::OperatorAction);
                    previous = std::move(prev.value());
                    previousValid = showFromJson(body.value()).ok() || ver.value().major < model::kSchemaMajor;
                }
            }
        }
    }

    const std::string bytes = makeEnvelope(json.value(), "save", clock_.wallUnixNs() / kNsPerMs);
    const std::string tmp = path_ + ".tmp";
    if (auto st = fs_.writeFile(tmp, bytes, true); !st.ok()) { fs_.remove(tmp); return st; }
    {   // validate what is actually on disk
        auto back = fs_.readFile(tmp, kMaxFileBytes);
        bool ok = back.ok() && back.value() == bytes;
        if (ok) { auto body = openEnvelope(back.value()); ok = body.ok() && showFromJson(body.value()).ok(); }
        if (!ok) { fs_.remove(tmp); return makeError(ErrorCategory::InternalFault, "save verification failed; previous file kept", Recoverability::Retryable); }
    }
    if (previousValid) {
        if (auto st = rotateBackups(previous); !st.ok()) { fs_.remove(tmp); return st; }
    }
    if (auto st = fs_.rename(tmp, path_); !st.ok()) { fs_.remove(tmp); return st; }
    fs_.remove(recoveryPath());   // the explicit save supersedes the autosave
    return okStatus();
}

Status ShowStore::writeRecovery(const model::ShowDocument& doc, const JsonWriteOptions& opt) {
    auto json = showToJson(doc, opt);
    if (!json) return json.error();
    return atomicWriteFile(fs_, recoveryPath(), makeEnvelope(json.value(), "autosave", clock_.wallUnixNs() / kNsPerMs));
}

Result<LoadedShow> ShowStore::loadFile(const std::string& path, const MigrationRegistry* migrations) {
    auto bytes = fs_.readFile(path, kMaxFileBytes);
    if (!bytes) return bytes.error();
    LoadedShow out;
    auto body = openEnvelope(bytes.value(), &out.info);
    if (!body) return body.error();
    auto ver = peekSchema(body.value());
    if (!ver) return ver.error();
    MigrationRegistry empty;
    auto plan = planMigration(path, ver.value(), migrations ? *migrations : empty, fs_);
    if (!plan) return plan.error();
    auto res = applyMigration(fs_, plan.value(), bytes.value(), body.value(), migrations ? *migrations : empty);
    if (!res) return res.error();
    out.parsed = std::move(res.value().parsed);
    out.migrated = res.value().migrated;
    out.migrationBackupPath = res.value().backupPath;
    if (out.migrated) out.notes.push_back("show was migrated in memory from an older format; the original was backed up; save to keep the new format");
    if (!out.parsed.warnings.empty()) out.notes.push_back("show contains fields this version does not understand; they will be ignored");
    return out;
}

Result<LoadedShow> ShowStore::load(const MigrationRegistry* migrations) {
    const bool havePrimary = fs_.exists(path_);
    Error firstError = makeError(ErrorCategory::InvalidArgument, "show file not found", Recoverability::OperatorAction);
    if (havePrimary) {
        auto r = loadFile(path_, migrations);
        if (r) { r.value().source = LoadSource::Primary; return r; }
        // A newer-format primary must be reported, not silently replaced by an older backup.
        if (r.error().category == ErrorCategory::SchemaUnsupported || r.error().category == ErrorCategory::PermissionDenied) return r.error();
        firstError = r.error();
    }
    for (int n = 1; n <= kKeepBackups; ++n) {
        if (!fs_.exists(backupPath(n))) continue;
        auto r = loadFile(backupPath(n), migrations);
        if (!r) continue;
        r.value().source = static_cast<LoadSource>(n);
        r.value().notes.insert(r.value().notes.begin(),
            std::string(havePrimary ? "the show file was damaged; restored from backup " : "the show file was missing; restored from backup ") + std::to_string(n));
        return r;
    }
    return firstError;
}

}  // namespace ss::persist
