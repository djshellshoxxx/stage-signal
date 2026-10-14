#include "persistence/show_io.h"
#include "nlohmann/json.hpp"
#include "persistence/hash.h"
#include <set>

namespace ss {
namespace persist {
using json = nlohmann::json;

namespace {
bool validBytes(const std::vector<uint8_t>& b) {
  show::ShowDocument d;
  return decodeShow(b.data(), b.size(), d, nullptr).ok();
}
Status loadBytes(IFileOps& ops, const std::string& path, show::ShowDocument& out, DecodeInfo* di) {
  std::vector<uint8_t> bytes;
  Status s = readFileAll(ops, path, show::limits::kMaxJsonBytes + 4096, bytes);
  if (!s.ok()) return s;
  return decodeShow(bytes.data(), bytes.size(), out, di);
}
} // namespace

Status ShowIO::cleanupTemps(const std::string& path) { return ops_.remove(tempPath(path)); }

Status ShowIO::save(const std::string& path, const show::ShowDocument& doc, int retain) {
  Status v = show::validateShow(doc);
  if (!v.ok()) return v;                                  // never write an invalid document
  std::string text = encodeShow(doc);
  if (text.size() > show::limits::kMaxJsonBytes + 4096) return Status::Fail(Err::ResourceExhausted, "show exceeds 8 MiB limit");
  const std::string tmp = tempPath(path);
  Status s = writeFileAll(ops_, tmp, text.data(), text.size());
  if (!s.ok()) { ops_.remove(tmp); return s; }
  std::vector<uint8_t> back;
  s = readFileAll(ops_, tmp, text.size() + 16, back);
  if (!s.ok() || back.size() != text.size() || !validBytes(back)) { ops_.remove(tmp); return s.ok() ? Status::Fail(Err::InternalFault, "read-back verification failed") : s; }
  if (retain < 0) retain = 0;
  if (retain > 9) retain = 9;
  if (retain > 0 && ops_.exists(path)) {
    std::vector<uint8_t> cur;
    if (readFileAll(ops_, path, show::limits::kMaxJsonBytes + 4096, cur).ok() && validBytes(cur)) {   // only valid saves enter the history
      ops_.remove(backupPath(path, retain));
      for (int i = retain - 1; i >= 1; --i)
        if (ops_.exists(backupPath(path, i))) { s = ops_.rename(backupPath(path, i), backupPath(path, i + 1)); if (!s.ok()) { ops_.remove(tmp); return s; } }
      s = writeFileAll(ops_, backupPath(path, 1) + ".tmp", cur.data(), cur.size());   // copy (primary stays in place until the final rename)
      if (s.ok()) s = ops_.rename(backupPath(path, 1) + ".tmp", backupPath(path, 1));
      if (!s.ok()) { ops_.remove(tmp); return s; }
    }
  }
  s = ops_.rename(tmp, path);
  if (!s.ok()) { ops_.remove(tmp); return s; }
  return Status::Ok();
}

Status ShowIO::load(const std::string& path, show::ShowDocument& out, LoadInfo* info) {
  LoadInfo local; LoadInfo& li = info ? *info : local;
  li = LoadInfo();
  show::ShowDocument d;
  Status s = loadBytes(ops_, path, d, &li.decode);
  if (!s.ok()) return s;                                   // 'out' untouched
  out = std::move(d); li.loadedFrom = path;
  return Status::Ok();
}

Status ShowIO::loadWithFallback(const std::string& path, show::ShowDocument& out, LoadInfo* info) {
  LoadInfo local; LoadInfo& li = info ? *info : local;
  Status first = load(path, out, &li);
  if (first.ok()) return first;
  if (first.code == Err::SchemaUnsupported) return first;     // a newer file is not "corrupt"; do not silently fall back
  for (int i = 1; i <= 9; ++i) {
    std::string b = backupPath(path, i);
    if (!ops_.exists(b)) continue;
    LoadInfo bi;
    if (load(b, out, &bi).ok()) { li = bi; li.recoveredFromBackup = true; li.loadedFrom = b; return Status::Ok(); }
  }
  return first;
}

Status ShowIO::migrateFileInPlace(const std::string& path, LoadInfo* info) {
  LoadInfo local; LoadInfo& li = info ? *info : local;
  std::vector<uint8_t> bytes;
  Status s = readFileAll(ops_, path, show::limits::kMaxJsonBytes + 4096, bytes);
  if (!s.ok()) return s;
  int maj = 0, min = 0;
  if (!peekSchema(bytes.data(), bytes.size(), maj, min)) return Status::Fail(Err::InvalidArgument, "unreadable schema version");
  if (maj > show::kSchemaMajor) return Status::Fail(Err::SchemaUnsupported, "file is newer than this build; refusing to touch it", false);
  show::ShowDocument d;
  s = decodeShow(bytes.data(), bytes.size(), d, &li.decode);
  if (!s.ok()) return s;
  if (!li.decode.migrated) { li.loadedFrom = path; return Status::Ok(); }
  std::string bak = path + ".premigrate-v" + std::to_string(maj) + "." + std::to_string(min);
  s = writeFileAll(ops_, bak, bytes.data(), bytes.size());   // backup FIRST; failure aborts before anything else changes
  if (!s.ok()) return s;
  s = save(path, d, 3);
  if (!s.ok()) return s;
  li.migratedOnDisk = true; li.migrationBackup = bak; li.loadedFrom = path;
  return Status::Ok();
}

// ---- autosave ------------------------------------------------------------------------------------
Status Autosave::tick(Ns now, const show::ShowDocument& doc, bool* saved) {
  if (saved) *saved = false;
  if (!dirty_ || now - since_ < interval_) return Status::Ok();
  Status s = io_.save(path_, doc, 1);
  if (!s.ok()) { err_ = s; since_ = now; return s; }     // stays dirty; retry after another interval
  err_ = Status::Ok(); dirty_ = false; ++saves_;
  if (saved) *saved = true;
  return Status::Ok();
}

// ---- crash recovery ----------------------------------------------------------------------------------
RecoveryInfo CrashRecovery::detect() {
  RecoveryInfo r;
  if (!ops_.exists(markerPath())) return r;
  r.crashed = true; r.autosavePath = autosavePath();
  std::vector<uint8_t> b;
  if (readFileAll(ops_, markerPath(), 4096, b).ok()) {
    json j = json::parse(b.begin(), b.end(), nullptr, false);
    if (j.is_object() && j.contains("wallMs") && j["wallMs"].is_number_integer()) r.markerWallMs = j["wallMs"].get<int64_t>();
  }
  show::ShowDocument d;
  if (io_.loadWithFallback(autosavePath(), d).ok()) { r.autosaveValid = true; r.showName = d.name; }
  return r;
}
Status CrashRecovery::beginSession(int64_t wallMs) {
  Status s = ops_.createDirs(dir_);
  if (!s.ok()) return s;
  std::string t = json{{"wallMs", wallMs}, {"v", 1}}.dump();
  return writeFileAll(ops_, markerPath(), t.data(), t.size());
}
Status CrashRecovery::endSession() { return ops_.remove(markerPath()); }
Status CrashRecovery::discard() {
  ops_.remove(autosavePath());
  for (int i = 1; i <= 3; ++i) ops_.remove(ShowIO::backupPath(autosavePath(), i));
  ops_.remove(ShowIO::tempPath(autosavePath()));
  return ops_.remove(markerPath());
}
Status CrashRecovery::restoreCopy(const std::string& desired, RestoredShow& out) {
  show::ShowDocument d;
  Status s = io_.loadWithFallback(autosavePath(), d);
  if (!s.ok()) return s;
  std::string target = desired;
  for (int n = 1; ops_.exists(target); ++n) {
    size_t dot = desired.find_last_of('.'), slash = desired.find_last_of("/\\");
    std::string suffix = " (recovered " + std::to_string(n) + ")";
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) target = desired + suffix;
    else target = desired.substr(0, dot) + suffix + desired.substr(dot);
    if (n > 1000) return Status::Fail(Err::InvalidArgument, "cannot choose a restore file name");
  }
  s = io_.save(target, d, 0);                     // explicit copy; original file is never overwritten
  if (!s.ok()) return s;
  out.doc = std::move(d); out.savedPath = target; out.startDisarmed = true; out.startIdle = true;
  return Status::Ok();
}

// ---- local settings ----------------------------------------------------------------------------------
namespace {
bool refOk(const std::string& r) {
  if (r.empty() || r.size() > 128) return false;
  for (unsigned char c : r) if (!(isalnum(c) || c == '_' || c == '.' || c == ':' || c == '-' || c == '/')) return false;
  return r.find("://") == std::string::npos;
}
}
Status validateSettings(const LocalSettings& s) {
  if (s.version != 1) return Status::Fail(Err::SchemaUnsupported, "settings version unsupported", false);
  if (s.devices.size() > 64 || s.outputDisplays.size() > 64 || s.secretRefs.size() > 64) return Status::Fail(Err::InvalidArgument, "settings: too many entries");
  for (const auto& kv : s.secretRefs) {
    if (!show::validId(kv.first)) return Status::Fail(Err::InvalidArgument, "settings: invalid secret label");
    if (!refOk(kv.second)) return Status::Fail(Err::InvalidArgument, "settings: secretRefs must hold a store reference, not a secret or URL");
  }
  for (const auto& kv : s.devices) if (kv.first.size() > 64 || kv.second.size() > 512) return Status::Fail(Err::InvalidArgument, "settings: device entry too long");
  for (const auto& kv : s.outputDisplays) if (kv.first.size() > 64 || kv.second.size() > 512) return Status::Fail(Err::InvalidArgument, "settings: display entry too long");
  if (s.recordingFolder.size() > 1024) return Status::Fail(Err::InvalidArgument, "settings: path too long");
  return Status::Ok();
}
Status saveSettings(IFileOps& ops, const std::string& path, const LocalSettings& s) {
  Status v = validateSettings(s);
  if (!v.ok()) return v;
  json body = {{"version", s.version}, {"devices", s.devices}, {"outputDisplays", s.outputDisplays}, {"secretRefs", s.secretRefs}, {"recordingFolder", s.recordingFolder}};
  std::string compact = body.dump();
  json env = {{"format", "stagesignal.settings"}, {"checksum", "sha256:" + Sha256::hex(compact.data(), compact.size())}, {"settings", body}};
  std::string text = env.dump();
  std::string tmp = path + ".tmp";
  Status w = writeFileAll(ops, tmp, text.data(), text.size());
  if (!w.ok()) { ops.remove(tmp); return w; }
  w = ops.rename(tmp, path);
  if (!w.ok()) ops.remove(tmp);
  return w;
}
Status loadSettings(IFileOps& ops, const std::string& path, LocalSettings& out) {
  std::vector<uint8_t> b;
  Status s = readFileAll(ops, path, 1u << 20, b);
  if (!s.ok()) return s;
  json env = json::parse(b.begin(), b.end(), nullptr, false);
  if (env.is_discarded() || !env.is_object() || env.value("format", "") != "stagesignal.settings" || !env.contains("settings") || !env["settings"].is_object() || !env.contains("checksum") || !env["checksum"].is_string())
    return Status::Fail(Err::InvalidArgument, "settings file invalid");
  std::string compact = env["settings"].dump();
  if (env["checksum"].get<std::string>() != "sha256:" + Sha256::hex(compact.data(), compact.size())) return Status::Fail(Err::InvalidArgument, "settings checksum mismatch");
  const json& j = env["settings"];
  LocalSettings r;
  auto strMap = [&](const char* k, std::map<std::string, std::string>& m) -> bool {
    if (!j.contains(k)) return true;
    if (!j[k].is_object()) return false;
    for (auto it = j[k].begin(); it != j[k].end(); ++it) { if (!it.value().is_string()) return false; m[it.key()] = it.value().get<std::string>(); }
    return true;
  };
  if (!j.contains("version") || !j["version"].is_number_integer()) return Status::Fail(Err::InvalidArgument, "settings version missing");
  r.version = j["version"].get<int>();
  if (!strMap("devices", r.devices) || !strMap("outputDisplays", r.outputDisplays) || !strMap("secretRefs", r.secretRefs)) return Status::Fail(Err::InvalidArgument, "settings malformed");
  if (j.contains("recordingFolder") && j["recordingFolder"].is_string()) r.recordingFolder = j["recordingFolder"].get<std::string>();
  Status v = validateSettings(r);
  if (!v.ok()) return v;
  out = std::move(r);
  return Status::Ok();
}

} // namespace persist
} // namespace ss
