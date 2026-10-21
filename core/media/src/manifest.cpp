#include "ss/media/manifest.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include "nlohmann/json.hpp"

namespace ss::media {
namespace {
using json = nlohmann::json;

std::string getStr(const json& o, const char* k) {
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
bool getBool(const json& o, const char* k, bool def = false) {
    auto it = o.find(k);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : def;
}
Permission getPerm(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || !it->is_boolean()) return Permission::Unset;
    return it->get<bool>() ? Permission::Yes : Permission::No;
}
double getNum(const json& o, const char* k) {
    auto it = o.find(k);
    return (it != o.end() && it->is_number()) ? it->get<double>() : 0.0;
}
std::uint64_t getU64(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end()) return 0;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    return 0;
}

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

bool validDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i = 0; i < 10; ++i) if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    const int m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// ---- SPDX expression: expr = term {("AND"|"OR") term}; term = "(" expr ")" | idstr["+"] ["WITH" idstr] ----
struct SpdxParser {
    std::vector<std::string> tok;
    std::size_t i = 0;
    static bool idChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':'; }
    bool lex(const std::string& s) {
        std::size_t p = 0;
        while (p < s.size()) {
            const char c = s[p];
            if (c == ' ' || c == '\t') { ++p; continue; }
            if (c == '(' || c == ')') { tok.emplace_back(1, c); ++p; continue; }
            if (c == '+') { tok.emplace_back("+"); ++p; continue; }
            if (!idChar(c)) return false;
            std::size_t q = p;
            while (q < s.size() && idChar(s[q])) ++q;
            tok.push_back(s.substr(p, q - p)); p = q;
        }
        return !tok.empty();
    }
    static bool isOp(const std::string& t) { return t == "AND" || t == "OR" || t == "WITH"; }
    bool idstr() {
        if (i >= tok.size()) return false;
        const std::string& t = tok[i];
        if (t == "(" || t == ")" || t == "+" || isOp(t)) return false;
        if (t == "NONE" || t == "NOASSERTION") return false;           // "unknown license" is not a license
        ++i; return true;
    }
    bool term() {
        if (i < tok.size() && tok[i] == "(") { ++i; if (!expr()) return false; if (i >= tok.size() || tok[i] != ")") return false; ++i; return true; }
        if (!idstr()) return false;
        if (i < tok.size() && tok[i] == "+") ++i;
        if (i < tok.size() && tok[i] == "WITH") { ++i; if (!idstr()) return false; }
        return true;
    }
    bool expr() {
        if (!term()) return false;
        while (i < tok.size() && (tok[i] == "AND" || tok[i] == "OR")) { ++i; if (!term()) return false; }
        return true;
    }
};

void add(std::vector<Finding>& v, const std::string& id, const char* field, const std::string& msg) {
    v.push_back(Finding{id, field, msg});
}
}  // namespace

bool isValidSpdxExpression(const std::string& expr) {
    SpdxParser p;
    if (!p.lex(expr)) return false;
    return p.expr() && p.i == p.tok.size();
}

bool isSafeRelativePath(const std::string& p) {
    if (p.empty() || p.size() > 260 || p[0] == '/' || p[0] == '\\') return false;
    if (p.find('\\') != std::string::npos || p.find('\0') != std::string::npos) return false;
    if (p.size() >= 2 && p[1] == ':') return false;                    // drive letter
    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        const std::string seg = p.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        start = end + 1;
    }
    return true;
}

bool isSha256Hex(const std::string& s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

Result<Manifest> parseManifestJson(const std::string& text) {
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return makeError(ErrorCategory::InvalidArgument, "manifest is not valid JSON", Recoverability::OperatorAction);
    Manifest m;
    auto sch = doc.find("schema");
    if (sch != doc.end() && sch->is_number_unsigned()) m.schema = sch->get<std::uint32_t>();
    if (m.schema != 1) return makeError(ErrorCategory::SchemaUnsupported, "manifest schema version is not supported");
    auto arr = doc.find("assets");
    if (arr == doc.end() || !arr->is_array())
        return makeError(ErrorCategory::InvalidArgument, "manifest has no 'assets' array", Recoverability::OperatorAction);
    for (const auto& o : *arr) {
        if (!o.is_object()) return makeError(ErrorCategory::InvalidArgument, "manifest asset entry is not an object");
        AssetEntry e;
        e.id = getStr(o, "id"); e.version = getStr(o, "version"); e.type = getStr(o, "type"); e.path = getStr(o, "path");
        e.creator = getStr(o, "creator"); e.originalUrl = getStr(o, "originalUrl"); e.retrievalDate = getStr(o, "retrievalDate");
        e.isOriginalWork = getBool(o, "isOriginalWork");
        e.licenseText = getStr(o, "licenseText"); e.spdx = getStr(o, "spdx");
        e.copyright = getStr(o, "copyright"); e.attributionText = getStr(o, "attributionText");
        e.attributionLocation = getStr(o, "attributionLocation");
        e.commercialUse = getPerm(o, "commercialUse"); e.modificationAllowed = getPerm(o, "modificationAllowed");
        e.modified = getBool(o, "modified"); e.derivativeProvenance = getStr(o, "derivativeProvenance");
        e.originalSha256 = lower(getStr(o, "originalSha256")); e.distributedSha256 = lower(getStr(o, "distributedSha256"));
        e.width = static_cast<std::uint32_t>(getU64(o, "width")); e.height = static_cast<std::uint32_t>(getU64(o, "height"));
        e.durationSec = getNum(o, "durationSec"); e.sizeBytes = getU64(o, "sizeBytes");
        auto fl = o.find("flash");
        if (fl != o.end() && fl->is_object()) {
            e.flash.declared = fl->contains("containsFlashOrStrobe") && (*fl)["containsFlashOrStrobe"].is_boolean();
            e.flash.containsFlashOrStrobe = getBool(*fl, "containsFlashOrStrobe");
            e.flash.maxFlashesPerSecond = getNum(*fl, "maxFlashesPerSecond");
        }
        auto tiers = o.find("permittedTiers");
        if (tiers != o.end() && tiers->is_array())
            for (const auto& t : *tiers) if (t.is_string()) e.permittedTiers.push_back(t.get<std::string>());
        m.assets.push_back(std::move(e));
        if (m.assets.size() > 100000) return makeError(ErrorCategory::ResourceExhausted, "manifest has too many entries");
    }
    return m;
}

std::vector<Finding> validateEntry(const AssetEntry& e) {
    std::vector<Finding> f;
    const std::string& id = e.id;
    auto idOk = [](const std::string& s) {
        return !s.empty() && s.size() <= 64 &&
               std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
    };
    if (!idOk(id)) add(f, id, "id", "id is required (1-64 chars of letters, digits, . _ -)");
    if (e.version.empty()) add(f, id, "version", "version is required");
    if (e.type.empty()) add(f, id, "type", "type is required");
    if (!isSafeRelativePath(e.path)) add(f, id, "path", "path must be a safe relative path (no absolute, drive, backslash or '..')");
    if (e.creator.empty()) add(f, id, "creator", "creator is required");
    if (e.originalUrl.empty()) {
        if (!e.isOriginalWork) add(f, id, "originalUrl", "original URL is required unless the asset is an original work");
    } else if (e.originalUrl.rfind("https://", 0) != 0 && e.originalUrl.rfind("http://", 0) != 0) {
        add(f, id, "originalUrl", "original URL must start with http:// or https://");
    }
    if (e.retrievalDate.empty() ? !e.isOriginalWork : !validDate(e.retrievalDate))
        add(f, id, "retrievalDate", "retrieval date must be YYYY-MM-DD");
    if (e.spdx.empty() && e.licenseText.empty()) add(f, id, "license", "license is missing: provide an SPDX expression or the full license text");
    if (!e.spdx.empty() && !isValidSpdxExpression(e.spdx)) add(f, id, "spdx", "SPDX expression is not valid");
    if (e.copyright.empty()) add(f, id, "copyright", "copyright notice is required");
    if (!e.isOriginalWork && (e.attributionText.empty() || e.attributionLocation.empty()))
        add(f, id, "attribution", "attribution text and location are required for third-party assets");
    if (e.commercialUse == Permission::Unset) add(f, id, "commercialUse", "commercial-use permission must be stated");
    if (e.modificationAllowed == Permission::Unset) add(f, id, "modificationAllowed", "modification permission must be stated");
    if (e.derivativeProvenance.empty()) add(f, id, "derivativeProvenance", "derivative provenance is required (use \"original\" when unmodified)");
    if (!isSha256Hex(e.originalSha256)) add(f, id, "originalSha256", "original SHA-256 must be 64 lower-case hex characters");
    if (!isSha256Hex(e.distributedSha256)) add(f, id, "distributedSha256", "distributed SHA-256 must be 64 lower-case hex characters");
    if (!e.modified && isSha256Hex(e.originalSha256) && isSha256Hex(e.distributedSha256) && e.originalSha256 != e.distributedSha256)
        add(f, id, "originalSha256", "asset is marked unmodified but original and distributed hashes differ");
    if (e.modified && e.modificationAllowed == Permission::No)
        add(f, id, "modificationAllowed", "asset is modified but its license does not allow modification");
    if (e.sizeBytes == 0) add(f, id, "sizeBytes", "size in bytes is required");
    const bool visual = e.type == "image" || e.type == "gif" || e.type == "video";
    if (visual && (e.width == 0 || e.height == 0)) add(f, id, "dimensions", "width and height are required for image/gif/video");
    if ((e.type == "gif" || e.type == "video") && !(e.durationSec > 0)) add(f, id, "durationSec", "duration is required for gif/video");
    if (!e.flash.declared) add(f, id, "flash", "flash/strobe metadata must be stated (containsFlashOrStrobe true or false)");
    else if (e.flash.containsFlashOrStrobe && !(e.flash.maxFlashesPerSecond > 0))
        add(f, id, "flash", "assets containing flash/strobe need maxFlashesPerSecond > 0");
    if (e.permittedTiers.empty()) add(f, id, "permittedTiers", "permitted package tiers are required");
    for (const auto& t : e.permittedTiers)
        if (t != "base" && t != "standard" && t != "enhanced") add(f, id, "permittedTiers", "unknown tier '" + t + "'");
    return f;
}

std::vector<Finding> validateManifest(const Manifest& m) {
    std::vector<Finding> f;
    std::map<std::string, int> ids, paths;
    for (const auto& e : m.assets) {
        auto v = validateEntry(e);
        f.insert(f.end(), v.begin(), v.end());
        if (++ids[e.id] == 2) add(f, e.id, "id", "duplicate asset id");
        if (++paths[e.path] == 2) add(f, e.id, "path", "duplicate asset path");
    }
    return f;
}

VerifyReport verifyManifest(const Manifest& m, IAssetSource& files, const VerifyOptions& opt) {
    VerifyReport r;
    r.findings = validateManifest(m);
    std::set<std::string> listed;
    for (const auto& e : m.assets) {
        listed.insert(e.path);
        if (!isSafeRelativePath(e.path) || !isSha256Hex(e.distributedSha256)) continue;   // already reported
        ++r.entriesChecked;
        std::uint64_t size = 0;
        auto h = files.sha256Hex(e.path, &size);
        if (!h.ok()) { add(r.findings, e.id, "file", "file is absent or unreadable"); continue; }
        if (h.value() != e.distributedSha256) add(r.findings, e.id, "distributedSha256", "SHA-256 of the file does not match the manifest");
        if (e.sizeBytes != 0 && size != e.sizeBytes) add(r.findings, e.id, "sizeBytes", "file size does not match the manifest");
    }
    if (opt.requireAllFilesListed)
        for (const auto& p : files.listFiles())
            if (!listed.count(p) && !opt.ignorePaths.count(p)) add(r.findings, "", "file", "shipped file has no manifest entry: " + p);
    return r;
}

}  // namespace ss::media
