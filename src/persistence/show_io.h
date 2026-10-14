// ShowIO (MOD-13): atomic save with retained generations, validated load with fallback, migration with
// backup-first, debounced autosave, crash-recovery marker, local (non-portable) settings. REQ-X01/S13/S14.
#pragma once
#include "core/show/model.h"
#include "persistence/file_ops.h"
#include "persistence/show_json.h"
#include <map>
#include <string>
#include <vector>

namespace ss {
namespace persist {

struct LoadInfo {
  DecodeInfo decode;
  bool recoveredFromBackup = false;    // primary was missing/corrupt; an earlier valid generation was loaded
  std::string loadedFrom;
  bool migratedOnDisk = false;
  std::string migrationBackup;         // path of the pre-migration copy
};

class ShowIO {
 public:
  explicit ShowIO(IFileOps& ops = defaultFileOps()) : ops_(ops) {}
  // Atomic save: validate -> temp file in the same directory -> flush -> read-back verify -> rotate prior valid
  // generations (<path>.bak1..bak3) -> rename over <path>. On any failure the existing file is untouched.
  Status save(const std::string& path, const show::ShowDocument& doc, int retain = 3);
  // Loads and fully validates; 'out' is only assigned on success (old state stays live otherwise).
  // Newer-major files: SchemaUnsupported, source never modified. Older majors are migrated in memory.
  Status load(const std::string& path, show::ShowDocument& out, LoadInfo* info = nullptr);
  // Same, but if the primary fails, tries bak1..bak3 (REQ-X01 recovery).
  Status loadWithFallback(const std::string& path, show::ShowDocument& out, LoadInfo* info = nullptr);
  // Migrates an older-schema file on disk: backs it up first (<path>.premigrate-v<maj>.<min>), then saves the
  // migrated document atomically. Never touches files written by a newer major.
  Status migrateFileInPlace(const std::string& path, LoadInfo* info = nullptr);
  // Removes stale "<path>.tmp" left by a crash.
  Status cleanupTemps(const std::string& path);
  static std::string backupPath(const std::string& path, int n) { return path + ".bak" + std::to_string(n); }
  static std::string tempPath(const std::string& path) { return path + ".tmp"; }
 private:
  IFileOps& ops_;
};

// Debounced autosave: first change opens a window; the document is written once the window reaches 'interval'
// (30 s default) even under continuous edits. Writes ONLY the show JSON (never media) to the autosave path.
class Autosave {
 public:
  Autosave(IFileOps& ops, std::string path, Ns interval = 30 * kNsPerSec) : io_(ops), path_(std::move(path)), interval_(interval) {}
  void markDirty(Ns now) { if (!dirty_) { dirty_ = true; since_ = now; } }
  void markClean() { dirty_ = false; }
  bool dirty() const { return dirty_; }
  // Call periodically (e.g. 1 Hz). On failure stays dirty and retries after another interval.
  Status tick(Ns now, const show::ShowDocument& doc, bool* saved = nullptr);
  int saves() const { return saves_; }
  const Status& lastError() const { return err_; }
  const std::string& path() const { return path_; }
 private:
  ShowIO io_; std::string path_; Ns interval_; bool dirty_ = false; Ns since_ = 0; int saves_ = 0; Status err_;
};

struct RecoveryInfo {
  bool crashed = false;              // marker from a previous session still present
  bool autosaveValid = false;
  std::string autosavePath;
  std::string showName;
  int64_t markerWallMs = 0;
};
struct RestoredShow {
  show::ShowDocument doc;
  std::string savedPath;             // explicit restored copy (original is never overwritten)
  bool startDisarmed = true;         // lighting/broadcast/recording are disarmed after recovery
  bool startIdle = true;             // engine should be created with Options::startIdle
};
class CrashRecovery {
 public:
  CrashRecovery(IFileOps& ops, std::string dir) : ops_(ops), io_(ops), dir_(std::move(dir)) {}
  RecoveryInfo detect();                                         // call BEFORE beginSession()
  Status beginSession(int64_t wallMs);                           // writes marker
  Status endSession();                                           // clean shutdown removes marker
  Status restoreCopy(const std::string& desiredPath, RestoredShow& out);   // picks a unique name if desiredPath exists
  Status discard();                                              // user declined recovery
  std::string autosavePath() const { return joinPath(dir_, "autosave.stagesignal.json"); }
  std::string markerPath() const { return joinPath(dir_, "session.marker"); }
 private:
  IFileOps& ops_; ShowIO io_; std::string dir_;
};

// Machine-local settings (never exported): physical devices and secret *references* only.
struct LocalSettings {
  int version = 1;
  std::map<std::string, std::string> devices;        // role -> stable device id (audioInput, midiIn, midiOut, camera, lightingNic ...)
  std::map<std::string, std::string> outputDisplays; // logical output id -> display stable id
  std::map<std::string, std::string> secretRefs;     // label -> reference key into ISecretStore (never the secret)
  std::string recordingFolder;
};
Status validateSettings(const LocalSettings& s);
Status saveSettings(IFileOps& ops, const std::string& path, const LocalSettings& s);
Status loadSettings(IFileOps& ops, const std::string& path, LocalSettings& out);

} // namespace persist
} // namespace ss
