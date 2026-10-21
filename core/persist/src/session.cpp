#include "ss/persist/session.hpp"

namespace ss::persist {

void AutosaveController::noteChange() {
    if (!dirty_) { dirty_ = true; deadline_ = clock_.nowMonotonic() + debounce_; }
}

Ns AutosaveController::nsUntilDue() const {
    if (!dirty_) return 0;
    const Ns d = deadline_ - clock_.nowMonotonic();
    return d > 0 ? d : 0;
}

bool AutosaveController::tick(const std::function<Status()>& write) {
    if (!due()) return false;
    const Status st = write();
    if (st.ok()) markClean();
    else { noteFailure(st.error()); deadline_ = clock_.nowMonotonic() + debounce_; }   // stay dirty, retry next window
    return true;
}

ShowSession::ShowSession(IFileSystem& fs, const IClock& clock, std::string path, Ns debounce)
    : store_(fs, clock, std::move(path)), autosave_(clock, debounce) {}

Result<std::optional<RestoreProposal>> ShowSession::detectRecovery() {
    IFileSystem& fs = store_.fs();
    const bool locked = fs.exists(store_.sessionLockPath());
    if (!fs.exists(store_.recoveryPath())) return std::optional<RestoreProposal>{};
    auto bytes = fs.readFile(store_.recoveryPath(), model::Limits::kMaxJsonBytes + kEnvelopeSlackBytes);
    if (!bytes) return std::optional<RestoreProposal>{};
    EnvelopeInfo info;
    auto body = openEnvelope(bytes.value(), &info);
    if (!body || !info.hasEnvelope) return std::optional<RestoreProposal>{};          // torn autosave: ignore, saved file stays authoritative
    auto parsed = showFromJson(body.value());
    if (!parsed) return std::optional<RestoreProposal>{};

    RestoreProposal p;
    p.showName = parsed.value().doc.name;
    p.recoveryWallUnixMs = info.savedWallUnixMs;
    p.uncleanShutdown = locked;
    if (fs.exists(store_.path())) {
        auto saved = fs.readFile(store_.path(), model::Limits::kMaxJsonBytes + kEnvelopeSlackBytes);
        EnvelopeInfo sinfo;
        if (saved && openEnvelope(saved.value(), &sinfo).ok()) p.savedWallUnixMs = sinfo.savedWallUnixMs;
    }
    p.newerThanSavedFile = p.savedWallUnixMs == 0 || p.recoveryWallUnixMs > p.savedWallUnixMs;
    // An explicit save removes the recovery file, so a leftover is either newer work or a crash artefact.
    if (!p.newerThanSavedFile && !locked) return std::optional<RestoreProposal>{};
    for (int n = 1; n < 1000; ++n) {
        p.restoredCopyPath = store_.path() + ".restored" + (n > 1 ? "-" + std::to_string(n) : std::string());
        if (!fs.exists(p.restoredCopyPath)) break;
    }
    p.doc = std::move(parsed.value().doc);
    p.extras = std::move(parsed.value().extras);
    return std::optional<RestoreProposal>{std::move(p)};
}

Result<ParsedShow> ShowSession::acceptRestore(const RestoreProposal& p) {
    ShowStore copy(store_.fs(), store_.clock(), p.restoredCopyPath);
    JsonWriteOptions opt;
    opt.extras = p.extras;
    if (auto st = copy.save(p.doc, opt); !st.ok()) return st.error();
    store_.fs().remove(store_.recoveryPath());
    store_.fs().remove(store_.sessionLockPath());
    ParsedShow out;
    out.doc = p.doc;
    out.extras = p.extras;
    return out;
}

Status ShowSession::discardRestore() {
    store_.fs().remove(store_.recoveryPath());
    store_.fs().remove(store_.sessionLockPath());
    return okStatus();
}

Status ShowSession::open() {
    return store_.fs().writeFile(store_.sessionLockPath(), "open " + std::to_string(store_.clock().wallUnixNs() / kNsPerMs), true);
}

Status ShowSession::saveNow(const model::ShowDocument& doc, const JsonWriteOptions& opt) {
    const Status st = store_.save(doc, opt);
    if (st.ok()) { unsaved_ = false; lastError_.reset(); autosave_.markClean(); }
    else lastError_ = st.error();   // dirty is untouched: a failed save never clears it
    return st;
}

bool ShowSession::tickAutosave(const std::function<void(model::ShowDocument&, JsonWriteOptions&)>& snapshot) {
    const bool ran = autosave_.tick([&]() -> Status {
        model::ShowDocument d;
        JsonWriteOptions opt;
        snapshot(d, opt);
        return store_.writeRecovery(d, opt);
    });
    if (ran) { if (autosave_.lastError()) lastError_ = autosave_.lastError(); else lastError_.reset(); }
    return ran;
}

Status ShowSession::closeClean() {
    store_.fs().remove(store_.recoveryPath());
    return store_.fs().remove(store_.sessionLockPath());
}

}  // namespace ss::persist
