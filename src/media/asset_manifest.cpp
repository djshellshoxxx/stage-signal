#include "media/asset_manifest.h"
#include "media/import.h"
#include "media/sha256.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace ss { namespace media {

using nlohmann::json;

static std::string str(const json& j, const char* k) { auto it = j.find(k); return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string(); }
static std::vector<std::string> strs(const json& j, const char* k) {
  std::vector<std::string> v; auto it = j.find(k);
  if (it != j.end() && it->is_array()) for (const auto& e : *it) if (e.is_string()) v.push_back(e.get<std::string>());
  return v;
}
static int tri(const json& j, const char* k) { auto it = j.find(k); return (it != j.end() && it->is_boolean()) ? (it->get<bool>() ? 1 : 0) : -1; }

Status parseManifest(const std::string& text, Manifest& m) {
  m = Manifest();
  try {
    json j = json::parse(text);
    if (!j.is_object()) return Status::Fail(Err::SchemaUnsupported, "manifest must be a JSON object", false);
    m.schemaVersion = j.value("schemaVersion", 0);
    if (m.schemaVersion != 1) return Status::Fail(Err::SchemaUnsupported, "unsupported manifest schemaVersion", false);
    m.generator = str(j, "generator");
    if (j.contains("assets")) {
      if (!j["assets"].is_array()) return Status::Fail(Err::SchemaUnsupported, "assets must be an array", false);
      for (const auto& a : j["assets"]) {
        if (!a.is_object()) return Status::Fail(Err::SchemaUnsupported, "asset entry must be an object", false);
        AssetEntry e;
        e.id = str(a, "id"); e.version = str(a, "version"); e.type = str(a, "type"); e.path = str(a, "path");
        e.creator = str(a, "creator"); e.originalUrl = str(a, "originalUrl"); e.retrievalDate = str(a, "retrievalDate");
        e.license = str(a, "license"); e.copyright = str(a, "copyright");
        if (a.contains("attribution") && a["attribution"].is_object()) { e.attributionText = str(a["attribution"], "text"); e.attributionLocation = str(a["attribution"], "location"); }
        e.commercialUse = tri(a, "commercialUse"); e.modification = tri(a, "modification");
        e.provenanceKind = str(a, "provenanceKind"); e.provenanceNotes = str(a, "provenanceNotes"); e.derivedFrom = strs(a, "derivedFrom");
        e.sha256Original = str(a, "sha256Original"); e.sha256Distributed = str(a, "sha256Distributed");
        e.width = a.value("width", 0); e.height = a.value("height", 0); e.durationMs = a.value("durationMs", (int64_t)0);
        e.sizeBytes = a.value("sizeBytes", (uint64_t)0);
        if (a.contains("flash") && a["flash"].is_object()) {
          const json& f = a["flash"]; e.flash.hasFlashing = f.value("hasFlashing", false); e.flash.hasStrobe = f.value("hasStrobe", false);
          e.flash.maxFlashHz = f.value("maxFlashHz", 0.0); e.flash.notes = str(f, "notes");
          if (!f.contains("hasFlashing") || !f.contains("hasStrobe")) e.flash.notes += "";   // completeness checked in verify
        }
        e.tiers = strs(a, "tiers");
        // remember whether flash metadata was present at all
        if (!a.contains("flash")) e.flash.maxFlashHz = -1;
        m.assets.push_back(std::move(e));
      }
    }
    if (j.contains("decoders") && j["decoders"].is_array())
      for (const auto& d : j["decoders"]) {
        if (!d.is_object()) continue;
        DecoderDecl x; x.id = str(d, "id"); x.enabled = d.value("enabled", false); x.containers = strs(d, "containers"); x.codecs = strs(d, "codecs"); x.notes = str(d, "notes");
        m.decoders.push_back(x);
      }
    return Status::Ok();
  } catch (...) {
    m = Manifest();
    return Status::Fail(Err::SchemaUnsupported, "manifest is not valid JSON of the expected shape", false);
  }
}

std::string serializeManifest(const Manifest& m) {
  json j; j["schemaVersion"] = m.schemaVersion; j["generator"] = m.generator;
  j["assets"] = json::array();
  for (const AssetEntry& e : m.assets) {
    json a;
    a["id"] = e.id; a["version"] = e.version; a["type"] = e.type; a["path"] = e.path; a["creator"] = e.creator;
    a["originalUrl"] = e.originalUrl; a["retrievalDate"] = e.retrievalDate; a["license"] = e.license; a["copyright"] = e.copyright;
    a["attribution"] = {{"text", e.attributionText}, {"location", e.attributionLocation}};
    if (e.commercialUse >= 0) a["commercialUse"] = e.commercialUse == 1;
    if (e.modification >= 0) a["modification"] = e.modification == 1;
    a["provenanceKind"] = e.provenanceKind; a["derivedFrom"] = e.derivedFrom; a["provenanceNotes"] = e.provenanceNotes;
    a["sha256Original"] = e.sha256Original; a["sha256Distributed"] = e.sha256Distributed;
    a["width"] = e.width; a["height"] = e.height; a["durationMs"] = e.durationMs; a["sizeBytes"] = e.sizeBytes;
    a["flash"] = {{"hasFlashing", e.flash.hasFlashing}, {"hasStrobe", e.flash.hasStrobe}, {"maxFlashHz", e.flash.maxFlashHz}, {"notes", e.flash.notes}};
    a["tiers"] = e.tiers;
    j["assets"].push_back(a);
  }
  j["decoders"] = json::array();
  for (const DecoderDecl& d : m.decoders) j["decoders"].push_back({{"id", d.id}, {"enabled", d.enabled}, {"containers", d.containers}, {"codecs", d.codecs}, {"notes", d.notes}});
  return j.dump(2) + "\n";
}

// ---- SPDX expression syntax ----------------------------------------------------------------------------------------
namespace {
struct SpdxParser {
  std::vector<std::string> t; size_t i = 0;
  static bool idChar(char c) { return std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':'; }
  static bool isId(const std::string& s) {
    if (s.empty()) return false;
    std::string b = s; if (b.back() == '+') b.pop_back();
    if (b.empty()) return false;
    for (char c : b) if (!idChar(c)) return false;
    if (b == "AND" || b == "OR" || b == "WITH" || b == "NONE" || b == "NOASSERTION") return false;
    return true;
  }
  bool expr() { if (!term()) return false; while (i < t.size() && (t[i] == "AND" || t[i] == "OR")) { ++i; if (!term()) return false; } return true; }
  bool term() {
    if (i >= t.size()) return false;
    if (t[i] == "(") { ++i; if (!expr()) return false; if (i >= t.size() || t[i] != ")") return false; ++i; return true; }
    if (!isId(t[i])) return false;
    ++i;
    if (i < t.size() && t[i] == "WITH") { ++i; if (i >= t.size() || !isId(t[i])) return false; ++i; }
    return true;
  }
};
}
bool isValidSpdxExpression(const std::string& e) {
  SpdxParser p; std::string cur;
  for (char c : e) {
    if (c == '(' || c == ')') { if (!cur.empty()) { p.t.push_back(cur); cur.clear(); } p.t.push_back(std::string(1, c)); }
    else if (std::isspace((unsigned char)c)) { if (!cur.empty()) { p.t.push_back(cur); cur.clear(); } }
    else cur.push_back(c);
  }
  if (!cur.empty()) p.t.push_back(cur);
  if (p.t.empty()) return false;
  return p.expr() && p.i == p.t.size();
}

// ---- verification ---------------------------------------------------------------------------------------------------
std::string ManifestReport::summary() const {
  std::string s = ok ? "MANIFEST OK" : "MANIFEST FAILED";
  s += " (" + std::to_string(entriesChecked) + " entries, " + std::to_string(filesScanned) + " files)\n";
  for (const auto& i : issues) s += "  [" + i.code + "] " + (i.assetId.empty() ? std::string("-") : i.assetId) + ": " + i.message + "\n";
  return s;
}

static bool safeRelPath(const std::string& p) {
  if (p.empty() || p.size() > 512 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos || p.find('\\') != std::string::npos) return false;
  size_t i = 0;
  while (i <= p.size()) { size_t j = p.find('/', i); if (j == std::string::npos) j = p.size(); std::string c = p.substr(i, j - i); if (c.empty() || c == "." || c == "..") return false; i = j + 1; }
  for (unsigned char ch : p) if (ch < 0x20) return false;
  return true;
}
static bool validDate(const std::string& d) {
  if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
  for (size_t i = 0; i < 10; ++i) if (i != 4 && i != 7 && !std::isdigit((unsigned char)d[i])) return false;
  int m = std::stoi(d.substr(5, 2)), dd = std::stoi(d.substr(8, 2));
  return m >= 1 && m <= 12 && dd >= 1 && dd <= 31;
}

ManifestReport verifyManifestText(const std::string& dir, const std::string& text, const VerifyOptions& opt) {
  ManifestReport r;
  auto add = [&](const std::string& id, const char* code, const std::string& msg) { r.issues.push_back({id, code, msg}); };
  try {
    Manifest m; Status ps = parseManifest(text, m);
    if (!ps.ok()) { add("", "manifest-parse", ps.detail); return r; }
    static const char* kTypes[] = {"still", "animation", "video", "font", "preset"};
    static const char* kTiers[] = {"BASE", "STANDARD", "ENHANCED", "SHOW"};
    std::set<std::string> ids, paths;
    for (const AssetEntry& e : m.assets) {
      ++r.entriesChecked;
      const std::string id = e.id.empty() ? "(no id)" : e.id;
      auto req = [&](const std::string& v, const char* field) { if (v.empty()) add(id, "missing-field", std::string("required field empty: ") + field); };
      req(e.id, "id"); req(e.version, "version"); req(e.type, "type"); req(e.path, "path"); req(e.creator, "creator");
      req(e.retrievalDate, "retrievalDate"); req(e.license, "license"); req(e.copyright, "copyright");
      req(e.attributionText, "attribution.text"); req(e.attributionLocation, "attribution.location"); req(e.provenanceKind, "provenanceKind");
      if (e.commercialUse < 0) add(id, "missing-field", "commercialUse must be an explicit boolean");
      if (e.modification < 0) add(id, "missing-field", "modification must be an explicit boolean");
      if (!e.id.empty() && !ids.insert(e.id).second) add(id, "duplicate-id", "duplicate asset id");
      if (!e.type.empty() && std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char* t) { return e.type == t; }) == std::end(kTypes)) add(id, "bad-type", "unknown asset type: " + e.type);
      if (!e.retrievalDate.empty() && !validDate(e.retrievalDate)) add(id, "bad-date", "retrievalDate must be YYYY-MM-DD");
      if (!e.license.empty() && !isValidSpdxExpression(e.license)) add(id, "bad-license", "license is not a valid SPDX expression: " + e.license);
      if (e.provenanceKind != "" && e.provenanceKind != "original" && e.provenanceKind != "derivative" && e.provenanceKind != "third-party") add(id, "bad-provenance", "provenanceKind must be original|derivative|third-party");
      if (e.provenanceKind == "derivative" && e.derivedFrom.empty()) add(id, "missing-field", "derivative asset must list derivedFrom");
      if (e.provenanceKind != "original" && e.originalUrl.empty()) add(id, "missing-field", "originalUrl required for non-original assets");
      if (!isHex64(e.sha256Distributed)) add(id, "missing-field", "sha256Distributed must be 64 lowercase hex chars");
      if (!isHex64(e.sha256Original)) add(id, "missing-field", "sha256Original must be 64 lowercase hex chars");
      if (e.provenanceKind == "original" && isHex64(e.sha256Original) && e.sha256Original != e.sha256Distributed) add(id, "provenance-mismatch", "original asset must have sha256Original == sha256Distributed");
      if (e.flash.maxFlashHz < 0) add(id, "missing-field", "flash/strobe metadata object is required");
      if (e.tiers.empty()) add(id, "missing-field", "permitted tiers list is empty");
      for (const std::string& t : e.tiers) if (std::find_if(std::begin(kTiers), std::end(kTiers), [&](const char* k) { return t == k; }) == std::end(kTiers)) add(id, "bad-tier", "unknown tier: " + t);
      if (e.type != "font" && e.type != "preset" && (e.width <= 0 || e.height <= 0)) add(id, "missing-field", "width/height required for visual assets");
      if (e.sizeBytes == 0) add(id, "missing-field", "sizeBytes required");
      if (!opt.requireTier.empty() && std::find(e.tiers.begin(), e.tiers.end(), opt.requireTier) == e.tiers.end()) continue;   // not shipped on this tier
      if (!safeRelPath(e.path)) { add(id, "bad-path", "path must be relative, '/'-separated, no '..' or drive/URL: " + e.path); continue; }
      if (!paths.insert(e.path).second) add(id, "duplicate-path", "two entries share path " + e.path);
      std::string full = dir + "/" + e.path;
      std::string hex; uint64_t sz = 0;
      Status hs = sha256FileHex(full, hex, &sz);
      if (!hs.ok()) { add(id, "file-missing", "bundled file absent or unreadable: " + e.path); continue; }
      if (e.sizeBytes && sz != e.sizeBytes) add(id, "size-mismatch", "size " + std::to_string(sz) + " != manifest " + std::to_string(e.sizeBytes));
      if (isHex64(e.sha256Distributed) && hex != e.sha256Distributed) add(id, "checksum-mismatch", "SHA-256 of " + e.path + " is " + hex + ", manifest says " + e.sha256Distributed);
      if (e.type == "still" || e.type == "animation") {
        std::vector<uint8_t> bytes;
        if (readFileBounded(full, 64ull << 20, bytes).ok()) {
          ImageProbe p; Status s = probeImage(bytes.data(), bytes.size(), p);
          if (!s.ok()) add(id, "unreadable-image", s.detail);
          else if ((e.width && p.w != e.width) || (e.height && p.h != e.height)) add(id, "dimension-mismatch", "image is " + std::to_string(p.w) + "x" + std::to_string(p.h) + ", manifest says " + std::to_string(e.width) + "x" + std::to_string(e.height));
          else if (e.type == "animation" && p.fmt == Container::Gif) {
            DecodedGif g; ImportLimits lim = limitsFor(Tier::Standard);
            if (validateAndDecodeGif(bytes.data(), bytes.size(), e.path, lim, g).ok() && e.durationMs && g.totalMs() != e.durationMs)
              add(id, "duration-mismatch", "GIF duration " + std::to_string(g.totalMs()) + " ms, manifest says " + std::to_string(e.durationMs));
          }
        }
      }
    }
    if (opt.failOnUnlistedFiles) {
      namespace fs = std::filesystem;
      std::error_code ec;
      fs::path root(dir);
      for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        ++r.filesScanned;
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (rel == "manifest.json" || rel == "README.md") continue;
        if (!paths.count(rel)) {
          bool listedButOtherTier = false;
          for (const AssetEntry& e : m.assets) if (e.path == rel) listedButOtherTier = true;
          if (!listedButOtherTier) add("", "unlisted-file", "file has no manifest entry: " + rel);
        }
      }
    }
  } catch (...) {
    add("", "internal", "verification aborted by an unexpected error");
  }
  r.ok = r.issues.empty();
  return r;
}

ManifestReport verifyManifest(const std::string& dir, const VerifyOptions& opt) {
  std::vector<uint8_t> bytes;
  Status s = readFileBounded(dir + "/manifest.json", 4u << 20, bytes);
  if (!s.ok()) { ManifestReport r; r.issues.push_back({"", "manifest-missing", "cannot read " + dir + "/manifest.json: " + s.detail}); return r; }
  return verifyManifestText(dir, std::string(bytes.begin(), bytes.end()), opt);
}

}} // namespace ss::media
