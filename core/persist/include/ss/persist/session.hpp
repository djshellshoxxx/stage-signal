// StageSignal persist: dirty tracking, 30 s debounced autosave, crash-recovery proposal (spec section 17).
//  - Autosave runs at most once per debounce window while the document is dirty, writes only the recovery file
//    (never the user's file, never media), and is driven by IClock so tests need no sleeps.
//  - A failed save or autosave keeps the document dirty, keeps the old file, and exposes lastError() for a visible banner.
//  - After a crash, detectRecovery() returns a proposal; nothing is applied automatically. Accepting writes an explicit
//    restored copy next to the original (never over it) and the restored session must start disarmed.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include "ss/persist/store.hpp"

namespace ss::persist {

constexpr Ns kAutosaveDebounceNs = 30 * kNsPerSec;

class AutosaveController {
public:
    explicit AutosaveController(const IClock& clock, Ns debounce = kAutosaveDebounceNs) : clock_(clock), debounce_(debounce) {}
    void noteChange();                               // arms the deadline on the first change of a dirty period
    bool dirty() const { return dirty_; }
    bool due() const { return dirty_ && clock_.nowMonotonic() >= deadline_; }
    Ns nsUntilDue() const;                           // 0 when due or clean
    // Runs `write` when due. Failure keeps dirty and retries after another debounce window.
    // Returns true when a write was attempted.
    bool tick(const std::function<Status()>& write);
    void markClean() { dirty_ = false; failures_ = 0; error_.reset(); }
    void noteFailure(const Error& e) { error_ = e; ++failures_; }
    const std::optional<Error>& lastError() const { return error_; }
    std::uint32_t consecutiveFailures() const { return failures_; }
private:
    const IClock& clock_;
    Ns debounce_;
    bool dirty_ = false;
    Ns deadline_ = 0;
    std::optional<Error> error_;
    std::uint32_t failures_ = 0;
};

struct RestoreProposal {
    std::string showName;
    std::int64_t recoveryWallUnixMs = 0;
    std::int64_t savedWallUnixMs = 0;               // 0 when the user's file is missing or unreadable
    bool uncleanShutdown = false;                   // session lock was left behind
    bool newerThanSavedFile = false;
    std::string restoredCopyPath;                   // where an accepted restore is written
    // Safety contract for the app: a restored session starts with every output disarmed.
    bool startDisarmed = true;
    bool lightingArmed = false, broadcastLive = false, recordingActive = false;
    model::ShowDocument doc;
    ShowExtras extras;
};

class ShowSession {
public:
    ShowSession(IFileSystem& fs, const IClock& clock, std::string path, Ns debounce = kAutosaveDebounceNs);

    // Call before open(): looks for an autosave newer than the saved file (or left by an unclean exit).
    Result<std::optional<RestoreProposal>> detectRecovery();
    Result<ParsedShow> acceptRestore(const RestoreProposal& p);   // writes restoredCopyPath, drops recovery, clears the lock
    Status discardRestore();

    Status open();                                   // writes the session lock
    void noteChanged() { unsaved_ = true; autosave_.noteChange(); }
    Status saveNow(const model::ShowDocument& doc, const JsonWriteOptions& opt = JsonWriteOptions{});
    // Owner's timer calls this (any cadence <= 1 s). `snapshot` fills the current document; it is only called when due.
    bool tickAutosave(const std::function<void(model::ShowDocument&, JsonWriteOptions&)>& snapshot);
    Status closeClean();                             // clean exit (operator resolved unsaved work): removes lock and recovery file

    bool dirty() const { return unsaved_; }                  // unsaved relative to the user's file
    bool autosavePending() const { return autosave_.dirty(); }
    const std::optional<Error>& lastError() const { return lastError_ ; }
    ShowStore& store() { return store_; }

private:
    ShowStore store_;
    AutosaveController autosave_;
    bool unsaved_ = false;
    std::optional<Error> lastError_;
};

}  // namespace ss::persist
