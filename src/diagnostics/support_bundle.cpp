#include "diagnostics/support_bundle.h"
#include "diagnostics/time_util.h"
#include "diagnostics/zip_store.h"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace ss {
using json = nlohmann::json;

SupportBundle::SupportBundle(IFileOps& ops, IClock& clk, BundleLimits lim) : ops_(ops), clk_(clk), lim_(lim) {}

static std::string lowerS(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }
static std::string baseName(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
static std::string dirName(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? std::string() : p.substr(0, s); }

bool SupportBundle::isExcludedArtifactName(const std::string& name) {
  static const char* ext[] = {".wav", ".flac", ".mp3", ".aac", ".ogg", ".opus", ".m4a", ".aif", ".aiff", ".raw", ".pcm", ".mp4", ".mkv", ".mov", ".avi",
    ".webm", ".ts", ".gif", ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".dmp", ".mdmp", ".hdmp", ".core", ".key", ".pem", ".pfx", ".p12", ".crt",
    ".cer", ".der", ".stagesignal", ".zip", nullptr};
  std::string b = lowerS(baseName(name));
  size_t d = b.find_last_of('.');
  if (d != std::string::npos) for (int i = 0; ext[i]; ++i) if (b.compare(d, std::string::npos, ext[i]) == 0) return true;
  static const char* words[] = {"secret", "streamkey", "stream_key", "password", "credential", "privatekey", "crash", nullptr};
  for (int i = 0; words[i]; ++i) if (b.find(words[i]) != std::string::npos) return true;
  return false;
}
std::vector<std::string> SupportBundle::alwaysExcluded() {
  return {"raw audio", "camera frames or video", "stream keys and other secrets", "show media files", "crash memory dumps", "full show files"};
}

Status SupportBundle::addTextSection(const std::string& id, const std::string& title, const std::string& file, const std::string& content) {
  Status v = sec::validateRelPath(file); if (!v.ok()) return v;
  std::string l = lowerS(file);
  bool okExt = false; for (const char* e : {".txt", ".json", ".jsonl", ".log", ".csv"}) { size_t n = std::string(e).size(); if (l.size() > n && l.compare(l.size() - n, n, e) == 0) okExt = true; }
  if (!okExt || isExcludedArtifactName(file)) return Status::Fail(Err::PermissionDenied, "section file name not allowed in a support bundle");
  if (id == "logs" || id == "journal" || id == "system") return Status::Fail(Err::InvalidArgument, "reserved category id");
  for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return Status::Fail(Err::InvalidArgument, "bad category id");
  secs_.push_back({id, title, file, content}); previewed_ = false; return Status::Ok();
}

std::vector<BundleCategory> SupportBundle::preview() {
  std::vector<BundleCategory> v;
  { BundleCategory c; c.id = "logs"; c.title = "Application logs"; c.description = "Rotated text logs, sanitized: URL credentials/queries/stream keys, usernames and secret-like values removed.";
    for (auto* f : logs_) { for (auto& p : f->existingPaths()) { ++c.files; int64_t s = ops_.fileSize(p); if (s > 0) c.approxBytes += (uint64_t)s; } }
    v.push_back(c); }
  { BundleCategory c; c.id = "journal"; c.title = "Event journal"; c.description = "Timeline of scene/cue/mode changes, text IDs, tempo/key snapshots, markers, device, adaptation, record/broadcast events (monotonic + wall-clock). No keys, no audio.";
    bool any = false;
    if (journalFile_) for (auto& p : journalFile_->existingPaths()) { any = true; ++c.files; int64_t s = ops_.fileSize(p); if (s > 0) c.approxBytes += (uint64_t)s; }
    if (!any && journal_) { c.files = 1; c.approxBytes = journal_->size() * 220; }
    v.push_back(c); }
  { BundleCategory c; c.id = "system"; c.title = "System information"; c.description = "OS, CPU/GPU, memory, app version and benchmark profile fields supplied by the application, plus bundle manifest.";
    c.files = 2; for (auto& kv : sys_) c.approxBytes += kv.first.size() + kv.second.size() + 8; v.push_back(c); }
  for (auto& s : secs_) {
    bool found = false; for (auto& c : v) if (c.id == s.id) { ++c.files; c.approxBytes += s.content.size(); found = true; }
    if (!found) { BundleCategory c; c.id = s.id; c.title = s.title; c.description = "Sanitized text: " + s.file; c.files = 1; c.approxBytes = s.content.size(); v.push_back(c); }
  }
  previewIds_.clear(); for (auto& c : v) previewIds_.insert(c.id);
  previewed_ = true; return v;
}

bool SupportBundle::sanitizeText(const std::string& in, std::string& out) const {
  out.clear(); out.reserve(in.size());
  size_t pos = 0;
  while (pos < in.size()) {
    size_t e = in.find('\n', pos); bool nl = e != std::string::npos; if (!nl) e = in.size();
    std::string line = in.substr(pos, e - pos); pos = nl ? e + 1 : e;
    if (line.size() > lim_.maxLineBytes) line = line.substr(0, lim_.maxLineBytes) + "...";
    for (auto& c : line) if (((unsigned char)c < 0x20 && c != '\t') || c == 0x7f) c = '?';
    std::string cur = line, prev;
    int pass = 0;
    do { prev = cur; cur = red_ ? red_->apply(prev) : sec::redact(prev); } while (cur != prev && ++pass < 4);
    if (cur != prev) return false;      // not stable: refuse rather than risk a leak
    out += cur; if (nl) out += '\n';
  }
  return true;
}

static bool cleanString(const sec::Redactor* r, const std::string& in, std::string& out) {
  std::string cur = in, prev; int pass = 0;
  do { prev = cur; cur = r ? r->apply(prev) : sec::redact(prev); } while (cur != prev && ++pass < 4);
  out = cur; return cur == prev;
}
static bool cleanJson(const sec::Redactor* r, json& j) {
  if (j.is_string()) { std::string o; if (!cleanString(r, j.get<std::string>(), o)) return false; j = o; return true; }
  if (j.is_array()) { for (auto& e : j) if (!cleanJson(r, e)) return false; return true; }
  if (j.is_object()) {
    json n = json::object();
    for (auto it = j.begin(); it != j.end(); ++it) { std::string k; if (!cleanString(r, it.key(), k)) return false; json v = it.value();
      if (sec::isSecretKeyName(it.key())) v = sec::kRedacted; else if (!cleanJson(r, v)) return false;
      n[k] = v; }
    j = n; return true;
  }
  return true;
}
bool SupportBundle::sanitizeJournal(const std::string& in, std::string& out) const {
  out.clear(); size_t pos = 0;
  while (pos < in.size()) {
    size_t e = in.find('\n', pos); bool nl = e != std::string::npos; if (!nl) e = in.size();
    std::string line = in.substr(pos, e - pos); pos = nl ? e + 1 : e;
    if (line.empty()) continue;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { json w; w["raw"] = line.substr(0, lim_.maxLineBytes); j = w; }
    if (!cleanJson(red_, j)) return false;
    out += j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
  }
  return true;
}

std::vector<SupportBundle::Ent> SupportBundle::plan(const std::set<std::string>& ok) {
  std::vector<Ent> v;
  if (ok.count("logs")) {
    for (size_t i = 0; i < logs_.size(); ++i) for (auto& p : logs_[i]->existingPaths()) {
      std::string nm = "logs/" + (logs_.size() > 1 ? std::to_string(i) + "-" : std::string()) + baseName(p);
      std::string path = p;
      v.push_back({"logs", nm, [this, path](std::string& o) { std::string raw; if (!ops_.readAll(path, raw, lim_.maxFileBytes)) return false; return sanitizeText(raw, o); }});
    }
  }
  if (ok.count("journal")) {
    bool fromFile = false;
    if (journal_) journal_->flush();
    if (journalFile_) for (auto& p : journalFile_->existingPaths()) {
      fromFile = true; std::string path = p;
      v.push_back({"journal", "journal/" + baseName(p), [this, path](std::string& o) { std::string raw; if (!ops_.readAll(path, raw, lim_.maxFileBytes)) return false; return sanitizeJournal(raw, o); }});
    }
    if (!fromFile && journal_) {
      v.push_back({"journal", "journal/journal.jsonl", [this](std::string& o) {
        std::string raw; for (auto& e : journal_->snapshot()) raw += EventJournal::toJsonLine(e); return sanitizeJournal(raw, o); }});
    }
  }
  if (ok.count("system")) {
    v.push_back({"system", "system/system.json", [this](std::string& o) {
      json j = json::object(); j["bundle_format"] = 1; j["generated_wall_ms"] = clk_.wallUnixMs(); j["generated_iso"] = isoUtc(clk_.wallUnixMs());
      json info = json::object(); for (auto& kv : sys_) info[kv.first] = kv.second; j["info"] = info;
      if (!cleanJson(red_, j)) return false;
      o = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n"; return true; }});
  }
  for (auto& s : secs_) if (ok.count(s.id)) {
    std::string content = s.content;
    v.push_back({s.id, "extra/" + s.file, [this, content](std::string& o) { return sanitizeText(content, o); }});
  }
  return v;
}

Status SupportBundle::buildEntries(const std::set<std::string>& approved, std::function<Status(const std::string&, const std::string&)> emit, BundleReport* rep) {
  if (!previewed_) return Status::Fail(Err::InvalidArgument, "preview the bundle categories before exporting");
  for (auto& a : approved) if (!previewIds_.count(a)) return Status::Fail(Err::InvalidArgument, "category was not offered in the preview: " + sec::sanitizeLibraryError(a, 32));
  if (approved.empty()) return Status::Fail(Err::InvalidArgument, "no categories approved");
  json manifest = json::object(); manifest["bundle_format"] = 1; manifest["generated_wall_ms"] = clk_.wallUnixMs(); manifest["generated_iso"] = isoUtc(clk_.wallUnixMs());
  manifest["monotonic_ns"] = clk_.nowNs();
  json cats = json::array(); for (auto& a : approved) cats.push_back(a); manifest["categories"] = cats;
  json excluded = json::array(); for (auto& e : alwaysExcluded()) excluded.push_back(e); manifest["excluded"] = excluded;
  manifest["redaction"] = "URL userinfo/query/stream-key segments, usernames and secret-like option values removed; output verified stable under re-redaction";
  json files = json::array(), skipped = json::array();
  BundleReport local; BundleReport& r = rep ? *rep : local; r = BundleReport();
  for (auto& e : plan(approved)) {
    if (isExcludedArtifactName(e.name)) return Status::Fail(Err::PermissionDenied, "excluded artifact type in bundle plan");
    std::string data;
    if (!e.load(data)) { skipped.push_back({{"name", e.name}, {"reason", "unreadable, oversized or failed sanitization self-check"}}); continue; }
    Status s = emit(e.name, data); if (!s.ok()) return s;
    json f; f["name"] = e.name; f["category"] = e.category; f["bytes"] = data.size(); f["crc32"] = crc32(data.data(), data.size()); files.push_back(f);
    ++r.files; r.bytes += data.size(); r.names.push_back(e.name);
  }
  manifest["files"] = files; manifest["skipped"] = skipped;
  std::string m = manifest.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
  Status s = emit("manifest.json", m); if (!s.ok()) return s;
  ++r.files; r.bytes += m.size(); r.names.push_back("manifest.json");
  return Status::Ok();
}

Status SupportBundle::exportZip(const std::string& dest, const std::set<std::string>& approved, BundleReport* rep) {
  if (!sec::validateRelPath(baseName(dest)).ok()) return Status::Fail(Err::InvalidArgument, "bad destination file name");
  if (ops_.fileSize(dest) >= 0) return Status::Fail(Err::InvalidArgument, "destination already exists");
  std::string part = dest + ".part";
  std::string d = dirName(dest); if (!d.empty()) ops_.createDirs(d);
  ops_.remove(part);
  ZipStoreWriter w([&](const char* p, size_t n) { return ops_.append(part, p, n); });
  int64_t wall = clk_.wallUnixMs();
  Status s = buildEntries(approved, [&](const std::string& n, const std::string& data) { return w.addFile(n, data, wall); }, rep);
  if (s.ok()) s = w.finish();
  if (!s.ok()) { ops_.remove(part); return s; }
  if (!ops_.rename(part, dest)) { ops_.remove(part); return Status::Fail(Err::DiskFull, "could not finalize bundle file"); }
  return Status::Ok();
}

Status SupportBundle::exportFolder(const std::string& dest, const std::set<std::string>& approved, BundleReport* rep) {
  if (!ops_.listDir(dest).empty()) return Status::Fail(Err::InvalidArgument, "destination folder is not empty");
  std::vector<std::string> written;
  Status s = buildEntries(approved, [&](const std::string& n, const std::string& data) {
    std::string full = joinPath(dest, n); std::string d = dirName(full);
    if (!ops_.createDirs(d) || !ops_.writeAll(full, data)) return Status::Fail(Err::DiskFull, "write failed");
    written.push_back(full); return Status::Ok(); }, rep);
  if (!s.ok()) for (auto& w : written) ops_.remove(w);
  return s;
}

} // namespace ss
