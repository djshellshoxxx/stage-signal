#include "ss/persist/bundle.hpp"

#include <algorithm>
#include <map>
#include "nlohmann/json.hpp"
#include "ss/base/sha256.hpp"
#include "ss/security/redact.hpp"

namespace ss::persist {
using json = nlohmann::json;
namespace {

Error bad(const std::string& why) { return makeError(ErrorCategory::InvalidArgument, "bundle invalid: " + why); }

std::string mediaMemberPath(const model::MediaRef& m) { return "media/" + m.sha256 + "." + m.format; }

std::string sizeText(std::uint64_t b) {
    const char* unit = "B";
    double v = static_cast<double>(b);
    if (b >= (1u << 30)) { v /= (1u << 30); unit = "GiB"; }
    else if (b >= (1u << 20)) { v /= (1u << 20); unit = "MiB"; }
    else if (b >= 1024) { v /= 1024; unit = "KiB"; }
    const long tenths = static_cast<long>(v * 10 + 0.5);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " " + unit;
}

security::ArchiveLimits bundleLimits() {
    security::ArchiveLimits l;
    l.maxEntries = model::Limits::kMaxArchiveEntries;
    l.maxTotalUncompressed = model::Limits::kMaxDecompressedBytes;
    l.maxEntryUncompressed = 1ull << 30;
    return l;
}

bool allowedExt(const std::string& e) {
    return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "gif" || e == "mp4" || e == "mkv";
}

}  // namespace

std::vector<MediaInclusionPrompt> mediaPrompts(const model::ShowDocument& doc) {
    std::vector<MediaInclusionPrompt> out;
    for (const auto& m : doc.media) {
        MediaInclusionPrompt p;
        p.mediaId = m.id; p.format = m.format; p.byteSize = m.byteSize; p.sha256 = m.sha256; p.flashRisk = m.flashRisk;
        std::string base = m.relativePath.substr(m.relativePath.find_last_of('/') == std::string::npos ? 0 : m.relativePath.find_last_of('/') + 1);
        p.fileLabel = security::sanitizeForLog(base.empty() ? m.sha256.substr(0, 12) + "." + m.format : base, 64);
        for (const auto& s : doc.scenes) if (s.mediaId == m.id) p.usedByScenes.push_back(s.id);
        for (const auto& c : doc.cues) if (c.mediaId == m.id) p.usedByCues.push_back(c.id);
        p.defaultInclude = !m.relativePath.empty();
        p.question = "Include '" + p.fileLabel + "' (" + sizeText(m.byteSize) + ") in the bundle?";
        if (m.relativePath.empty()) p.question += " The file is not available on this machine.";
        out.push_back(std::move(p));
    }
    return out;
}

Result<BundlePlan> planBundle(const model::ShowDocument& src, const BundleChoices& choices, const std::string& appVersion) {
    if (auto st = validateDocument(src); !st.ok()) return st.error();
    model::ShowDocument doc = src;
    BundlePlan plan;
    std::map<std::string, BundleMember> media;   // by member path, deduplicated by content
    for (auto& m : doc.media) {
        const bool want = choices.includeAllMedia || choices.includeMedia.count(m.id) != 0;
        const bool available = !m.relativePath.empty();
        if (want && available) {
            const std::string path = mediaMemberPath(m);
            m.relativePath = path;
            auto it = media.find(path);
            if (it == media.end()) {
                BundleMember bm;
                bm.path = path; bm.kind = MemberKind::Media; bm.size = m.byteSize; bm.sha256 = m.sha256; bm.mediaId = m.id;
                media.emplace(path, std::move(bm));
            }
            if (m.flashRisk) plan.warnings.push_back("media " + std::to_string(m.id) + " is flagged as flash risk");
        } else {
            m.relativePath.clear();
            plan.excludedMedia.push_back(m.id);
            for (const auto& s : doc.scenes) if (s.mediaId == m.id) plan.warnings.push_back("scene " + std::to_string(s.id) + " uses media " + std::to_string(m.id) + " that is not in the bundle");
            for (const auto& c : doc.cues) if (c.mediaId == m.id) plan.warnings.push_back("cue " + std::to_string(c.id) + " uses media " + std::to_string(m.id) + " that is not in the bundle");
        }
    }
    for (const auto& hit : scanForSecrets(doc)) plan.warnings.push_back("field " + hit + " looks like it contains a secret; review before sharing");

    JsonWriteOptions opt;
    opt.mode = ExportMode::Portable;
    opt.extras = choices.extras;
    auto showJson = showToJson(doc, opt);
    if (!showJson) return showJson.error();

    BundleMember show;
    show.path = "show.json"; show.kind = MemberKind::Show; show.size = showJson.value().size(); show.sha256 = Sha256::hashHex(showJson.value());
    show.content = showJson.value();

    json man;
    man["format"] = kBundleFormatTag;
    man["bundleVersion"] = kBundleVersion;
    if (!appVersion.empty()) man["app"] = security::sanitizeForLog(appVersion, 64);
    man["show"] = {{"uuid", doc.uuid}, {"name", doc.name}, {"schema", {{"major", model::kSchemaMajor}, {"minor", model::kSchemaMinor}}}};
    json members = json::array();
    members.push_back({{"path", show.path}, {"kind", "show"}, {"size", show.size}, {"sha256", show.sha256}});
    std::uint64_t total = show.size;
    for (const auto& kv : media) {
        members.push_back({{"path", kv.second.path}, {"kind", "media"}, {"size", kv.second.size}, {"sha256", kv.second.sha256}});
        if (kv.second.size > UINT64_MAX - total) return makeError(ErrorCategory::ResourceExhausted, "bundle too large");
        total += kv.second.size;
    }
    man["members"] = members;
    man["excludedMedia"] = plan.excludedMedia;
    BundleMember manifest;
    manifest.path = "manifest.json"; manifest.kind = MemberKind::Manifest; manifest.content = man.dump();
    manifest.size = manifest.content.size(); manifest.sha256 = Sha256::hashHex(manifest.content);
    total += manifest.size;
    if (media.size() + 2 > model::Limits::kMaxArchiveEntries) return makeError(ErrorCategory::ResourceExhausted, "bundle has too many members");
    if (total > model::Limits::kMaxDecompressedBytes) return makeError(ErrorCategory::ResourceExhausted, "bundle larger than the 2 GiB export limit");

    plan.members.push_back(std::move(manifest));
    plan.members.push_back(std::move(show));
    for (auto& kv : media) plan.members.push_back(std::move(kv.second));
    plan.totalBytes = total;
    return plan;
}

// ---- import ------------------------------------------------------------------------------------
const BundleManifestEntry* BundleManifest::find(const std::string& path) const {
    for (const auto& m : members) if (m.path == path) return &m;
    return nullptr;
}

bool isAllowedBundlePath(const std::string& p) {
    if (p == "show.json" || p == "manifest.json" || p == "media") return true;
    if (p.compare(0, 6, "media/") != 0) return false;
    const std::string leaf = p.substr(6);
    const auto dot = leaf.find('.');
    if (dot != 64 || leaf.find('/') != std::string::npos) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = leaf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return allowedExt(leaf.substr(65));
}

Result<std::vector<security::ValidatedEntry>> validateBundleListing(const std::vector<security::ArchiveEntryInfo>& listing) {
    auto v = security::validateArchive(listing, bundleLimits());
    if (!v) return v.error();
    bool haveShow = false, haveManifest = false;
    for (const auto& e : v.value()) {
        if (!isAllowedBundlePath(e.path)) return bad("unexpected member");
        if (e.path == "show.json") {
            if (e.isDirectory) return bad("show.json is a directory");
            if (e.uncompressedSize > model::Limits::kMaxJsonBytes) return makeError(ErrorCategory::ResourceExhausted, "show.json larger than 8 MiB limit");
            haveShow = true;
        } else if (e.path == "manifest.json") {
            if (e.isDirectory) return bad("manifest.json is a directory");
            if (e.uncompressedSize > (1u << 20)) return makeError(ErrorCategory::ResourceExhausted, "manifest.json too large");
            haveManifest = true;
        } else if (e.path == "media") {
            if (!e.isDirectory) return bad("media must be a directory");
        } else if (e.isDirectory) {
            return bad("unexpected directory");
        }
    }
    if (!haveShow || !haveManifest) return bad("missing show.json or manifest.json");
    return v;
}

Result<BundleManifest> parseManifest(const std::string& text) {
    if (text.size() > (1u << 20)) return makeError(ErrorCategory::ResourceExhausted, "manifest too large");
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError(ErrorCategory::DecodeFailure, "manifest is not valid JSON");
    BundleManifest m;
    try {
        if (j.value("format", std::string()) != kBundleFormatTag) return bad("not a StageSignal bundle");
        m.bundleVersion = j.at("bundleVersion").get<std::uint32_t>();
        if (m.bundleVersion != kBundleVersion)
            return makeError(ErrorCategory::SchemaUnsupported, "bundle version not supported", Recoverability::OperatorAction);
        const json& s = j.at("show");
        m.showUuid = s.value("uuid", std::string()); m.showName = security::sanitizeForLog(s.value("name", std::string()), 256);
        m.schema.major = s.at("schema").at("major").get<std::uint32_t>(); m.schema.minor = s.at("schema").at("minor").get<std::uint32_t>();
        const json& arr = j.at("members");
        if (!arr.is_array() || arr.size() > model::Limits::kMaxArchiveEntries) return makeError(ErrorCategory::ResourceExhausted, "manifest member count");
        for (const auto& e : arr) {
            BundleManifestEntry me;
            me.path = e.at("path").get<std::string>(); me.kind = e.at("kind").get<std::string>();
            me.size = e.at("size").get<std::uint64_t>(); me.sha256 = e.at("sha256").get<std::string>();
            if (!security::canonicalMemberPath(me.path).ok() || !isAllowedBundlePath(me.path) || me.sha256.size() != 64) return bad("manifest member");
            m.members.push_back(std::move(me));
        }
        if (j.contains("excludedMedia")) for (const auto& x : j.at("excludedMedia")) m.excludedMedia.push_back(x.get<std::uint32_t>());
    } catch (const std::exception&) {
        return makeError(ErrorCategory::DecodeFailure, "manifest is malformed");
    }
    if (!m.find("show.json")) return bad("manifest does not list show.json");
    return m;
}

Status checkMemberAgainstManifest(const BundleManifest& m, const std::string& path, std::uint64_t size, const std::string& sha) {
    const auto* e = m.find(path);
    if (!e) return bad("member not listed in manifest");
    if (e->size != size) return bad("member size differs from manifest");
    if (e->sha256 != sha) return bad("member checksum differs from manifest");
    return okStatus();
}

Result<ParsedShow> importBundleShow(const std::string& showJson, const BundleManifest& manifest) {
    auto parsed = showFromJson(showJson);
    if (!parsed) return parsed.error();
    if (!manifest.showUuid.empty() && parsed.value().doc.uuid != manifest.showUuid) return bad("show.json does not match manifest");
    for (const auto& m : parsed.value().doc.media) {
        if (m.relativePath.empty()) {
            parsed.value().warnings.push_back("media " + std::to_string(m.id) + " was not included in the bundle");
            continue;
        }
        if (m.relativePath != mediaMemberPath(m)) return bad("media reference does not point at its content-addressed member");
        const auto* e = manifest.find(m.relativePath);
        if (!e || e->sha256 != m.sha256) return bad("media reference missing from manifest");
    }
    return parsed;
}

Status verifyMediaBytes(const model::MediaRef& ref, const void* data, std::size_t size) {
    if (ref.byteSize != 0 && ref.byteSize != size) return bad("media size differs from the show record");
    if (Sha256::hashHex(data, size) != ref.sha256) return bad("media content hash differs from the show record");
    return okStatus();
}

}  // namespace ss::persist
