// StageSignal media: decoder-worker isolation (REQ-S08, REQ-X03). Decoding happens in a separate process or
// thread that implements IMediaWorker. Every call on this interface is NON-BLOCKING by contract; the control
// thread only ever submits, polls and (on timeout) kills. A worker crash, hang or garbage reply costs one media
// item, never the show, audio or UI. The app supplies the real process-backed worker; FakeMediaWorker drives tests.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/media/format.hpp"
#include "ss/media/validate.hpp"

namespace ss::media {

using RequestId = std::uint64_t;

struct DecodeRequest {
    RequestId id = 0;
    std::uint32_t mediaId = 0;
    Format format = Format::Unknown;
    std::string relativePath;                  // inside the show bundle; the worker resolves and re-validates it
    std::uint32_t targetWidth = 0, targetHeight = 0;   // validated plan target; worker must not exceed it
};

enum class WorkerEventKind : std::uint8_t { FrameReady, DecodeFailed, WorkerDied };
struct WorkerEvent {
    WorkerEventKind kind = WorkerEventKind::DecodeFailed;
    RequestId id = 0;
    std::uint32_t mediaId = 0;
    PixelBuffer frame;                         // FrameReady: first/poster frame (RGBA8 premultiplied)
    Error error;                               // DecodeFailed / WorkerDied (sanitized)
};

class IMediaWorker {
public:
    virtual ~IMediaWorker() = default;
    virtual Status submit(const DecodeRequest& req) = 0;      // enqueue only; error when full or dead
    virtual bool poll(WorkerEvent& out) = 0;                  // false when nothing is ready
    virtual void kill() = 0;                                  // forceful, returns immediately
    virtual Status restart() = 0;                             // start a fresh worker, returns immediately
    virtual bool alive() const = 0;
};

// Scripted worker for fault tests. No threads, no sleeping.
class FakeMediaWorker final : public IMediaWorker {
public:
    enum class Mode : std::uint8_t { Succeed, Fail, Hang, Crash, WrongSize };
    void setMode(std::uint32_t mediaId, Mode m, std::uint32_t w = 0, std::uint32_t h = 0);
    void setDefault(Mode m) { default_ = m; }
    Status submit(const DecodeRequest& req) override;
    bool poll(WorkerEvent& out) override;
    void kill() override { alive_ = false; ++kills; events_.clear(); }
    Status restart() override;
    bool alive() const override { return alive_; }
    void failRestart(bool v) { restartFails_ = v; }
    int kills = 0, restarts = 0, submits = 0;
    std::vector<DecodeRequest> received;
private:
    struct Script { Mode mode = Mode::Succeed; std::uint32_t w = 0, h = 0; };
    std::map<std::uint32_t, Script> scripts_;
    Mode default_ = Mode::Succeed;
    std::deque<WorkerEvent> events_;
    bool alive_ = true, restartFails_ = false;
};

enum class MediaState : std::uint8_t { Unknown, Pending, Ready, Failed };

struct SupervisorConfig {
    Ns decodeTimeoutNs = secToNs(5);           // a pending decode older than this is declared hung
    Ns restartBackoffNs = msToNs(500);
    std::uint32_t maxPending = 8;
    std::uint32_t maxConsecutiveRestarts = 5;  // then the decoder stays down until retryWorker()
    std::uint32_t maxEventsPerPump = 16;       // bounds control-thread work per call
};

struct MediaRequest {
    std::uint32_t mediaId = 0;
    Format format = Format::Unknown;
    std::string relativePath;
    std::uint32_t targetWidth = 0, targetHeight = 0;   // from StillPlan/GifPlan/VideoPlan
    std::uint64_t reserveBytes = 0;                    // decoded cache bytes to reserve (plan.decodedBytes etc.)
};

// What a scene should show for a media reference right now.
struct MediaView {
    enum class Kind : std::uint8_t { Real, Placeholder } kind = Kind::Placeholder;
    const PixelBuffer* pixels = nullptr;       // never null; valid until the next pump()/unload()
    MediaState state = MediaState::Unknown;
    std::string reason;                        // sanitized failure text for the operator, empty when Real
};

enum class SwitchDecision : std::uint8_t {
    Proceed,            // the next scene's media is ready (or it needs none)
    KeepCurrent,        // next media failed/pending and the current scene is valid: stay on it
    UsePlaceholder      // nothing valid to keep: show the documented placeholder for the next scene
};

class MediaSupervisor {
public:
    MediaSupervisor(IMediaWorker& worker, const IClock& clock, DecodedCacheBudget budget, SupervisorConfig cfg = {});

    // Never blocks. Failure is recorded as MediaState::Failed (and returned) instead of thrown.
    Status request(const MediaRequest& req);
    // Drains worker events (bounded), enforces timeouts, restarts a dead worker with backoff.
    void pump();
    void unload(std::uint32_t mediaId);                    // frees the cache reservation
    void retryWorker() { consecutiveRestarts_ = 0; }       // operator action after maxConsecutiveRestarts

    MediaState state(std::uint32_t mediaId) const;
    std::string failureReason(std::uint32_t mediaId) const;
    MediaView view(std::uint32_t mediaId) const;           // Real when Ready, otherwise the placeholder
    // mediaId 0 means "no media needed" (procedural scene) and is always valid.
    SwitchDecision decideSwitch(std::uint32_t currentMediaId, std::uint32_t nextMediaId) const;

    const PixelBuffer& placeholder() const { return placeholder_; }
    const DecodedCacheBudget& budget() const { return budget_; }
    std::uint32_t pendingCount() const;
    std::uint32_t workerRestarts() const { return restarts_; }

    // Documented placeholder: 128x128 RGBA, 16 px checker of two dark greys with a 4 px red frame.
    static PixelBuffer makePlaceholder();

private:
    struct Slot {
        MediaState state = MediaState::Pending;
        RequestId reqId = 0;
        Ns deadline = 0;
        std::uint64_t reserved = 0;
        std::uint32_t targetW = 0, targetH = 0;
        PixelBuffer frame;
        std::string reason;
    };
    void fail(std::uint32_t mediaId, Slot& s, const std::string& reason);
    void failAllPending(const std::string& reason);
    void maintainWorker(Ns now);

    IMediaWorker& worker_;
    const IClock& clock_;
    DecodedCacheBudget budget_;
    SupervisorConfig cfg_;
    std::map<std::uint32_t, Slot> slots_;
    PixelBuffer placeholder_;
    RequestId nextReq_ = 1;
    Ns restartAt_ = -1;
    std::uint32_t consecutiveRestarts_ = 0, restarts_ = 0;
};

}  // namespace ss::media
