#include "ss/media/worker.hpp"

namespace ss::media {

// ---- FakeMediaWorker -----------------------------------------------------------------------------------------
void FakeMediaWorker::setMode(std::uint32_t mediaId, Mode m, std::uint32_t w, std::uint32_t h) {
    scripts_[mediaId] = Script{m, w, h};
}

Status FakeMediaWorker::submit(const DecodeRequest& req) {
    if (!alive_) return makeError(ErrorCategory::DeviceLost, "decoder worker is not running", Recoverability::Retryable);
    ++submits;
    received.push_back(req);
    Script s;
    auto it = scripts_.find(req.mediaId);
    if (it != scripts_.end()) s = it->second; else s.mode = default_;
    WorkerEvent ev;
    ev.id = req.id; ev.mediaId = req.mediaId;
    switch (s.mode) {
        case Mode::Succeed:
            ev.kind = WorkerEventKind::FrameReady;
            ev.frame = PixelBuffer(req.targetWidth ? req.targetWidth : 16, req.targetHeight ? req.targetHeight : 16);
            events_.push_back(std::move(ev));
            break;
        case Mode::WrongSize:
            ev.kind = WorkerEventKind::FrameReady;
            ev.frame = PixelBuffer(s.w ? s.w : req.targetWidth + 1, s.h ? s.h : req.targetHeight + 1);
            events_.push_back(std::move(ev));
            break;
        case Mode::Fail:
            ev.kind = WorkerEventKind::DecodeFailed;
            ev.error = makeError(ErrorCategory::DecodeFailure, "decoder rejected the media", Recoverability::OperatorAction);
            events_.push_back(std::move(ev));
            break;
        case Mode::Hang:
            break;
        case Mode::Crash:
            alive_ = false;
            events_.clear();
            ev.kind = WorkerEventKind::WorkerDied;
            ev.error = makeError(ErrorCategory::InternalFault, "decoder worker terminated unexpectedly", Recoverability::Retryable);
            events_.push_back(std::move(ev));
            break;
    }
    return okStatus();
}

bool FakeMediaWorker::poll(WorkerEvent& out) {
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

Status FakeMediaWorker::restart() {
    if (restartFails_) return makeError(ErrorCategory::InternalFault, "decoder worker could not be started", Recoverability::Retryable);
    ++restarts;
    alive_ = true;
    events_.clear();
    return okStatus();
}

// ---- MediaSupervisor -----------------------------------------------------------------------------------------
PixelBuffer MediaSupervisor::makePlaceholder() {
    PixelBuffer p(128, 128);
    for (std::uint32_t y = 0; y < 128; ++y) {
        std::uint8_t* row = p.row(y);
        for (std::uint32_t x = 0; x < 128; ++x) {
            const bool frame = x < 4 || y < 4 || x >= 124 || y >= 124;
            std::uint8_t r, g, b;
            if (frame) { r = 200; g = 40; b = 40; }
            else { const bool light = (((x / 16) + (y / 16)) & 1) != 0; r = g = b = light ? 64 : 32; }
            row[x * 4 + 0] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255;
        }
    }
    return p;
}

MediaSupervisor::MediaSupervisor(IMediaWorker& worker, const IClock& clock, DecodedCacheBudget budget, SupervisorConfig cfg)
    : worker_(worker), clock_(clock), budget_(budget), cfg_(cfg), placeholder_(makePlaceholder()) {}

void MediaSupervisor::fail(std::uint32_t, Slot& s, const std::string& reason) {
    if (s.reserved) { budget_.release(s.reserved); s.reserved = 0; }
    s.state = MediaState::Failed;
    s.reason = reason;
    s.frame = PixelBuffer();
}

void MediaSupervisor::failAllPending(const std::string& reason) {
    for (auto& kv : slots_) if (kv.second.state == MediaState::Pending) fail(kv.first, kv.second, reason);
}

std::uint32_t MediaSupervisor::pendingCount() const {
    std::uint32_t n = 0;
    for (const auto& kv : slots_) if (kv.second.state == MediaState::Pending) ++n;
    return n;
}

Status MediaSupervisor::request(const MediaRequest& req) {
    auto it = slots_.find(req.mediaId);
    if (it != slots_.end()) {
        if (it->second.state == MediaState::Ready || it->second.state == MediaState::Pending) return okStatus();
        slots_.erase(it);                                         // Failed: explicit re-request is a retry
    }
    if (pendingCount() >= cfg_.maxPending)
        return makeError(ErrorCategory::ResourceExhausted, "too many media decodes in flight", Recoverability::Retryable);
    Slot s;
    s.targetW = req.targetWidth; s.targetH = req.targetHeight;
    if (!budget_.tryReserve(req.reserveBytes)) {
        s.state = MediaState::Failed; s.reason = "decoded media cache budget exceeded";
        slots_[req.mediaId] = std::move(s);
        return makeError(ErrorCategory::ResourceExhausted, "decoded media cache budget exceeded", Recoverability::OperatorAction);
    }
    s.reserved = req.reserveBytes;
    if (!worker_.alive()) {
        fail(req.mediaId, s, "decoder is unavailable");
        slots_[req.mediaId] = std::move(s);
        return makeError(ErrorCategory::DeviceLost, "decoder is unavailable", Recoverability::Retryable);
    }
    DecodeRequest d;
    d.id = nextReq_++; d.mediaId = req.mediaId; d.format = req.format; d.relativePath = req.relativePath;
    d.targetWidth = req.targetWidth; d.targetHeight = req.targetHeight;
    s.reqId = d.id;
    s.deadline = clock_.nowMonotonic() + cfg_.decodeTimeoutNs;
    s.state = MediaState::Pending;
    Status st = worker_.submit(d);
    if (!st.ok()) {
        fail(req.mediaId, s, "decoder rejected the request");
        slots_[req.mediaId] = std::move(s);
        return st;
    }
    slots_[req.mediaId] = std::move(s);
    return okStatus();
}

void MediaSupervisor::maintainWorker(Ns now) {
    if (worker_.alive()) { restartAt_ = -1; return; }
    if (consecutiveRestarts_ >= cfg_.maxConsecutiveRestarts) return;       // stays down until retryWorker()
    if (restartAt_ < 0) restartAt_ = now + cfg_.restartBackoffNs;
    if (now >= restartAt_) {
        ++consecutiveRestarts_;
        ++restarts_;
        restartAt_ = -1;
        (void)worker_.restart();          // failure leaves alive()==false; the next pump schedules another attempt
    }
}

void MediaSupervisor::pump() {
    const Ns now = clock_.nowMonotonic();
    WorkerEvent ev;
    for (std::uint32_t n = 0; n < cfg_.maxEventsPerPump && worker_.poll(ev); ++n) {
        if (ev.kind == WorkerEventKind::WorkerDied) {
            failAllPending("decoder worker terminated");
            continue;
        }
        auto it = slots_.find(ev.mediaId);
        if (it == slots_.end() || it->second.state != MediaState::Pending || it->second.reqId != ev.id) continue;   // stale
        Slot& s = it->second;
        if (ev.kind == WorkerEventKind::DecodeFailed) { fail(ev.mediaId, s, "media could not be decoded"); continue; }
        const std::uint64_t bytes = static_cast<std::uint64_t>(ev.frame.width) * ev.frame.height * 4;
        const bool sane = !ev.frame.empty() && ev.frame.rgba.size() == bytes &&
                          ev.frame.width <= s.targetW && ev.frame.height <= s.targetH && bytes <= s.reserved;
        if (!sane) { fail(ev.mediaId, s, "decoder returned a frame outside the validated size"); continue; }
        s.frame = std::move(ev.frame);
        s.state = MediaState::Ready;
        s.reason.clear();
        consecutiveRestarts_ = 0;
    }
    // Hung decodes: kill the worker (the only way to reclaim it) and fail what was in flight.
    bool timedOut = false;
    for (auto& kv : slots_)
        if (kv.second.state == MediaState::Pending && now >= kv.second.deadline) { timedOut = true; break; }
    if (timedOut) {
        worker_.kill();
        failAllPending("decode timed out");
    }
    maintainWorker(now);
}

void MediaSupervisor::unload(std::uint32_t mediaId) {
    auto it = slots_.find(mediaId);
    if (it == slots_.end()) return;
    if (it->second.reserved) budget_.release(it->second.reserved);
    slots_.erase(it);
}

MediaState MediaSupervisor::state(std::uint32_t mediaId) const {
    auto it = slots_.find(mediaId);
    return it == slots_.end() ? MediaState::Unknown : it->second.state;
}
std::string MediaSupervisor::failureReason(std::uint32_t mediaId) const {
    auto it = slots_.find(mediaId);
    return it == slots_.end() ? std::string() : it->second.reason;
}

MediaView MediaSupervisor::view(std::uint32_t mediaId) const {
    MediaView v;
    auto it = slots_.find(mediaId);
    if (it != slots_.end() && it->second.state == MediaState::Ready) {
        v.kind = MediaView::Kind::Real; v.pixels = &it->second.frame; v.state = MediaState::Ready;
        return v;
    }
    v.kind = MediaView::Kind::Placeholder; v.pixels = &placeholder_;
    v.state = it == slots_.end() ? MediaState::Unknown : it->second.state;
    v.reason = it == slots_.end() ? "media not loaded" : it->second.reason;
    return v;
}

SwitchDecision MediaSupervisor::decideSwitch(std::uint32_t currentMediaId, std::uint32_t nextMediaId) const {
    if (nextMediaId == 0 || state(nextMediaId) == MediaState::Ready) return SwitchDecision::Proceed;
    const bool currentValid = currentMediaId == 0 || state(currentMediaId) == MediaState::Ready;
    return currentValid ? SwitchDecision::KeepCurrent : SwitchDecision::UsePlaceholder;
}

}  // namespace ss::media
