#include "App/ShellState.h"
#include <cstring>
#include "nlohmann/json.hpp"
#include "ss/base/sha256.hpp"
#include "ss/persist/show_json.hpp"

namespace ss::app {

// ---- prefs ------------------------------------------------------------------------------------------------
bool PrefsStore::validKey(const std::string& k) {
    if (k.empty() || k.size() > 64) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

bool PrefsStore::validValue(const std::string& v) {
    if (v.size() > 512) return false;
    for (unsigned char c : v) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

ss::Status PrefsStore::set(const std::string& key, const std::string& value) {
    if (!validKey(key)) return ss::makeError(ss::ErrorCategory::InvalidArgument, "bad preference name");
    if (!validValue(value)) return ss::makeError(ss::ErrorCategory::InvalidArgument, "bad preference value");
    if (map_.count(key) == 0 && map_.size() >= kMaxEntries)
        return ss::makeError(ss::ErrorCategory::ResourceExhausted, "too many preferences");
    map_[key] = value;
    return ss::okStatus();
}

std::string PrefsStore::get(const std::string& key, const std::string& fallback) const {
    auto it = map_.find(key);
    return it == map_.end() ? fallback : it->second;
}

std::string PrefsStore::serialise() const {
    std::string o;
    for (const auto& kv : map_) { o += kv.first; o += '='; o += kv.second; o += '\n'; }
    return o;
}

PrefsStore PrefsStore::parse(const std::string& text) {
    PrefsStore p;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        (void) p.set(line.substr(0, eq), line.substr(eq + 1));
    }
    return p;
}

// ---- lock ---------------------------------------------------------------------------------------------------
std::string lockToText(const LockInfo& l) {
    nlohmann::json j;
    j["pid"] = l.pid; j["startUnixMs"] = l.startUnixMs; j["build"] = l.build; j["mode"] = l.mode;
    return j.dump();
}

bool lockFromText(const std::string& text, LockInfo& out) {
    try {
        const auto j = nlohmann::json::parse(text);
        if (!j.is_object() || !j.contains("pid") || !j["pid"].is_number_integer()) return false;
        out.pid = j["pid"].get<long long>();
        out.startUnixMs = j.value("startUnixMs", 0LL);
        out.build = j.value("build", std::string());
        out.mode = j.value("mode", std::string());
        return true;
    } catch (...) { return false; }
}

// ---- state blob ---------------------------------------------------------------------------------------------
namespace {
void putLe(std::string& s, std::uint64_t v, int bytes) { for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff)); }
std::uint64_t getLe(const unsigned char* p, int bytes) { std::uint64_t v = 0; for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i); return v; }
constexpr std::size_t kHeader = 4 + 2 + 4, kHash = 32;
}  // namespace

ss::Result<std::string> encodeStateBlob(const StateBlobContent& c) {
    nlohmann::json j;
    j["format"] = "stagesignal-state";
    nlohmann::json prefs = nlohmann::json::object();
    for (const auto& kv : c.prefs)
        if (kv.first.rfind("ui.", 0) == 0 && PrefsStore::validKey(kv.first) && PrefsStore::validValue(kv.second)) prefs[kv.first] = kv.second;
    j["prefs"] = prefs;
    bool ref = c.referenceOnly;
    std::string showText;
    if (!ref) {
        auto r = ss::persist::showToJson(c.doc, ss::persist::JsonWriteOptions{ss::persist::ExportMode::Portable, {}});
        if (!r.ok()) return r.error();
        showText = r.value();
        j["name"] = c.doc.name; j["uuid"] = c.doc.uuid;
        if (showText.size() + 4096 > kStateBlobMaxBytes) ref = true;
    }
    if (ref) {
        j["reference"] = true;
        if (!c.referenceOnly) {
            j["sha256"] = ss::Sha256::hashHex(showText.data(), showText.size());
        } else {
            j["name"] = c.showName; j["uuid"] = c.showUuid; j["sha256"] = c.showHash;
        }
    } else {
        j["showJson"] = showText;
    }
    const std::string payload = j.dump();
    if (payload.size() + kHeader + kHash > kStateBlobMaxBytes)
        return ss::makeError(ss::ErrorCategory::ResourceExhausted, "state too large");
    std::string out("SSST", 4);
    putLe(out, kStateBlobVersion, 2);
    putLe(out, payload.size(), 4);
    out += payload;
    Sha256 h; h.update(payload);
    const auto d = h.finish();
    out.append(reinterpret_cast<const char*>(d.data()), d.size());
    return out;
}

ss::Result<StateBlobContent> decodeStateBlob(const void* data, std::size_t size) {
    using ss::ErrorCategory;
    if (!data || size < kHeader + kHash) return ss::makeError(ErrorCategory::InvalidArgument, "state is too short");
    if (size > kStateBlobMaxBytes) return ss::makeError(ErrorCategory::ResourceExhausted, "state is too large");
    const auto* p = static_cast<const unsigned char*>(data);
    if (std::memcmp(p, "SSST", 4) != 0) return ss::makeError(ErrorCategory::InvalidArgument, "state is not from StageSignal");
    const auto ver = static_cast<std::uint16_t>(getLe(p + 4, 2));
    if (ver == 0 || ver > kStateBlobVersion) return ss::makeError(ErrorCategory::SchemaUnsupported, "state is from a newer version");
    const std::uint64_t len = getLe(p + 6, 4);
    if (len != size - kHeader - kHash) return ss::makeError(ErrorCategory::InvalidArgument, "state length does not match");
    const char* payload = reinterpret_cast<const char*>(p + kHeader);
    Sha256 h; h.update(payload, static_cast<std::size_t>(len));
    const auto d = h.finish();
    if (std::memcmp(d.data(), p + kHeader + len, kHash) != 0) return ss::makeError(ErrorCategory::InvalidArgument, "state checksum failed");
    StateBlobContent out;
    try {
        const auto j = nlohmann::json::parse(payload, payload + len);
        if (!j.is_object() || j.value("format", std::string()) != "stagesignal-state")
            return ss::makeError(ErrorCategory::InvalidArgument, "state has the wrong format");
        if (j.contains("prefs") && j["prefs"].is_object()) {
            for (auto it = j["prefs"].begin(); it != j["prefs"].end(); ++it) {
                if (!it.value().is_string()) continue;
                const std::string k = it.key(), v = it.value().get<std::string>();
                if (k.rfind("ui.", 0) == 0 && PrefsStore::validKey(k) && PrefsStore::validValue(v)) out.prefs.emplace_back(k, v);
            }
        }
        if (j.value("reference", false)) {
            out.referenceOnly = true;
            out.showName = j.value("name", std::string()); out.showUuid = j.value("uuid", std::string());
            out.showHash = j.value("sha256", std::string());
            return out;
        }
        if (!j.contains("showJson") || !j["showJson"].is_string())
            return ss::makeError(ErrorCategory::InvalidArgument, "state has no show");
        auto parsed = ss::persist::showFromJson(j["showJson"].get<std::string>());
        if (!parsed.ok()) return parsed.error();
        out.doc = std::move(parsed.value().doc);
        return out;
    } catch (...) {
        return ss::makeError(ErrorCategory::InvalidArgument, "state could not be read");
    }
}

}  // namespace ss::app
