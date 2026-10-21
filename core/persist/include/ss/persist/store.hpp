// StageSignal persist: atomic show save / load (REQ-X01, spec section 17).
//
// Save file = one header line + the show JSON:
//   STAGESIGNAL-SAVE 1 kind=<save|autosave> saved=<unix ms> bytes=<n> sha256=<hex>\n{...show json...}
// so a torn or corrupted file is always detectable. Raw JSON (no header) is accepted on load only.
//
// save() order of operations (every step leaves a loadable file behind):
//   1 serialise + validate        2 refuse to overwrite a NEWER-format file   3 write <path>.tmp (flushed)
//   4 read back, verify checksum and re-validate     5 rotate <path>.bak1..3 (copy of the previous valid save)
//   6 atomic rename tmp -> path   7 drop the autosave recovery file
// Any failure before step 6 leaves the previous file untouched and returns the error.
#pragma once
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/persist/fs.hpp"
#include "ss/persist/migration.hpp"
#include "ss/persist/show_json.hpp"

namespace ss::persist {

constexpr int kKeepBackups = 3;
constexpr std::size_t kEnvelopeSlackBytes = 1024;

struct EnvelopeInfo {
    bool hasEnvelope = false;
    std::string kind;                  // "save" | "autosave"
    std::int64_t savedWallUnixMs = 0;
    std::uint64_t bytes = 0;
    std::string sha256;
};

std::string makeEnvelope(const std::string& jsonText, const std::string& kind, std::int64_t savedWallUnixMs);
// Verifies length and checksum. Raw JSON (first byte '{') passes with hasEnvelope=false.
Result<std::string> openEnvelope(const std::string& fileBytes, EnvelopeInfo* info = nullptr);

// Write `content` to `path` via `<path>.tmp`: flush, read back and compare checksum, then rename over `path`.
// On any failure the temp file is removed (best effort) and `path` is untouched.
Status atomicWriteFile(IFileSystem& fs, const std::string& path, const std::string& content);

enum class LoadSource : std::uint8_t { Primary, Backup1, Backup2, Backup3 };

struct LoadedShow {
    ParsedShow parsed;
    LoadSource source = LoadSource::Primary;
    EnvelopeInfo info;
    bool migrated = false;
    std::string migrationBackupPath;
    std::vector<std::string> notes;    // operator-visible, e.g. "primary file was damaged; restored from backup 1"
};

class ShowStore {
public:
    ShowStore(IFileSystem& fs, const IClock& clock, std::string path);

    Status save(const model::ShowDocument& doc, const JsonWriteOptions& opt = JsonWriteOptions{});
    // Autosave target: atomic, own file, never touches the user's file or its backups.
    Status writeRecovery(const model::ShowDocument& doc, const JsonWriteOptions& opt = JsonWriteOptions{});
    // Primary first, then newest valid backup. A primary from a newer format is reported, never skipped.
    Result<LoadedShow> load(const MigrationRegistry* migrations = nullptr);
    Result<LoadedShow> loadFile(const std::string& path, const MigrationRegistry* migrations = nullptr);

    void removeStaleTemps();

    const std::string& path() const { return path_; }
    std::string backupPath(int n) const { return path_ + ".bak" + std::to_string(n); }
    std::string recoveryPath() const { return path_ + ".recovery"; }
    std::string sessionLockPath() const { return path_ + ".session"; }
    IFileSystem& fs() { return fs_; }
    const IClock& clock() const { return clock_; }

private:
    Status rotateBackups(const std::string& previousBytes);
    IFileSystem& fs_;
    const IClock& clock_;
    std::string path_;
};

}  // namespace ss::persist
