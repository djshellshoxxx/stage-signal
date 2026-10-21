// StageSignal broadcast: the seams to the outside world. The session never spawns anything itself: the app supplies an
// IProcessFactory (posix_spawn / CreateProcess of the external ffmpeg, stdout = progress, stderr = diagnostics) and an
// IMediaFeed (the bounded network queue that feeds the process). Fakes here make start/stop/crash/retry testable
// without ffmpeg or a network (T-FAULT).
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/record/fanout.hpp"

namespace ss::broadcast {

// Non-blocking. All reads return what has arrived since the last call.
class IProcess {
public:
    virtual ~IProcess() = default;
    virtual Status start(const std::vector<std::string>& argv) = 0;   // argv may contain secrets: do not retain or log it
    virtual bool running() = 0;
    virtual int exitCode() const = 0;                                 // valid when !running() after a start
    virtual std::string readProgress() = 0;                           // stdout (`-progress pipe:1`)
    virtual std::string readStderr() = 0;
    virtual void requestStop() = 0;                                   // graceful (close stdin / SIGINT / 'q'); never blocks
    virtual void kill() = 0;                                          // forceful; never blocks
};
class IProcessFactory {
public:
    virtual ~IProcessFactory() = default;
    virtual std::unique_ptr<IProcess> create() = 0;
};

// The producer side of the stream. abandonQueued() drops media that was captured before a disconnect so that a
// reconnect does not replay stale content; requestKeyframe() asks the producer to resync (a restarted ffmpeg also
// begins with a fresh GOP because the encoder is new).
class IMediaFeed {
public:
    virtual ~IMediaFeed() = default;
    virtual void setActive(bool on) = 0;
    virtual void abandonQueued() = 0;
    virtual void requestKeyframe() = 0;
    virtual std::uint64_t droppedChunks() const = 0;
};

// Adapter over the record module's SinkFanout (network queue only). Never touches the disk queue.
class FanoutFeed final : public IMediaFeed {
public:
    explicit FanoutFeed(record::SinkFanout& f) : f_(f) {}
    void setActive(bool on) override { f_.setNetworkEnabled(on); }
    void abandonQueued() override { f_.network().clear(); }
    void requestKeyframe() override { ++keyframeRequests_; }
    std::uint64_t droppedChunks() const override { return f_.network().stats().dropped; }
    std::uint64_t keyframeRequests() const { return keyframeRequests_; }
private:
    record::SinkFanout& f_;
    std::uint64_t keyframeRequests_ = 0;
};

// ---- fakes (used by tests and by the app's simulation mode) -----------------------------------------------------
class FakeFeed final : public IMediaFeed {
public:
    void setActive(bool on) override { active = on; }
    void abandonQueued() override { ++abandons; }
    void requestKeyframe() override { ++keyframes; }
    std::uint64_t droppedChunks() const override { return dropped; }
    bool active = false;
    int abandons = 0, keyframes = 0;
    std::uint64_t dropped = 0;
};

// State of a fake process, shared between the session (which owns the IProcess) and the test (which scripts it), so
// a test can still inspect a process after the session has released it.
struct FakeProcessData {
    bool failStart = false, exitOnStop = true, started = false, alive = false;
    int code = 0, stopRequests = 0, kills = 0;
    std::vector<std::string> argv;
    std::string progressOut, stderrOut;
    void emitProgress(std::uint64_t totalSize, std::uint64_t outTimeUs, std::uint64_t drops = 0) {
        progressOut += "frame=1\ntotal_size=" + std::to_string(totalSize) + "\nout_time_us=" + std::to_string(outTimeUs) +
                       "\ndrop_frames=" + std::to_string(drops) + "\nprogress=continue\n";
    }
    void crash(int exitCode, const std::string& stderrText) { stderrOut += stderrText; alive = false; code = exitCode; }
};

class FakeProcess final : public IProcess {
public:
    explicit FakeProcess(std::shared_ptr<FakeProcessData> d) : d_(std::move(d)) {}
    Status start(const std::vector<std::string>& a) override {
        if (d_->failStart) return makeError(ErrorCategory::TransportFailure, "cannot start ffmpeg", Recoverability::OperatorAction);
        d_->argv = a; d_->started = true; d_->alive = true; return okStatus();
    }
    bool running() override { return d_->started && d_->alive; }
    int exitCode() const override { return d_->code; }
    std::string readProgress() override { std::string s; s.swap(d_->progressOut); return s; }
    std::string readStderr() override { std::string s; s.swap(d_->stderrOut); return s; }
    void requestStop() override { ++d_->stopRequests; if (d_->exitOnStop && d_->alive) { d_->alive = false; d_->code = 0; } }
    void kill() override { ++d_->kills; if (d_->alive) { d_->alive = false; d_->code = 137; } }
private:
    std::shared_ptr<FakeProcessData> d_;
};

class FakeProcessFactory final : public IProcessFactory {
public:
    std::unique_ptr<IProcess> create() override {
        auto d = std::make_shared<FakeProcessData>();
        d->failStart = nextFailsToStart; d->exitOnStop = nextExitOnStop;
        nextFailsToStart = false;
        created.push_back(d);
        return std::make_unique<FakeProcess>(d);
    }
    std::shared_ptr<FakeProcessData> last() const { return created.empty() ? nullptr : created.back(); }
    std::vector<std::shared_ptr<FakeProcessData>> created;
    bool nextFailsToStart = false, nextExitOnStop = true;
};

}  // namespace ss::broadcast
