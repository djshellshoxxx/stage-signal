// Transport seam. The core never opens sockets: the app layer implements IDatagramSink over JUCE UDP.
// MockSink is the ONLY sink used for preview, setup, benchmark and dry-run (REQ-L03); it also doubles as the
// socket-style instrumentation double in tests (counts every datagram and byte handed to it).
#pragma once
#include <cstdint>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/lighting/addressing.hpp"

namespace ss::lighting {

struct Datagram {
    Endpoint dst;
    std::vector<std::uint8_t> bytes;
    Generation generation = 0;        // transport generation the frame was built for (stale ones are discarded)
    std::uint64_t armToken = 0;       // identifies the Arm that authorised this transmission (0 = none)
    bool priority = false;            // blackout / safe frames: the sink must send these ahead of any queue
};

class IDatagramSink {
public:
    virtual ~IDatagramSink() = default;
    // Must not block the lighting worker. Returns TransportFailure/DeviceLost on error.
    virtual Status send(const Datagram& d) = 0;
    virtual bool connected() const = 0;
};

class MockSink final : public IDatagramSink {
public:
    Status send(const Datagram& d) override {
        ++attempts_;
        if (!connected_) { ++failures_; return makeError(ErrorCategory::DeviceLost, "mock transport disconnected", Recoverability::OperatorAction); }
        if (failAt_ >= 0 && sent_ >= static_cast<std::uint64_t>(failAt_)) {
            ++failures_; return makeError(ErrorCategory::TransportFailure, "mock transport send failed", Recoverability::Retryable);
        }
        if (expectGeneration_ != 0 && d.generation != expectGeneration_) { ++stale_; return okStatus(); }   // discarded, not sent
        ++sent_; bytes_ += d.bytes.size();
        if (log_.size() < kMaxLog) log_.push_back(d);
        return okStatus();
    }
    bool connected() const override { return connected_; }

    // ---- instrumentation ----
    std::uint64_t datagrams() const { return sent_; }
    std::uint64_t bytes() const { return bytes_; }
    std::uint64_t attempts() const { return attempts_; }
    std::uint64_t failures() const { return failures_; }
    std::uint64_t staleDiscarded() const { return stale_; }
    const std::vector<Datagram>& log() const { return log_; }
    void clearLog() { log_.clear(); }
    // ---- fault injection ----
    void setConnected(bool c) { connected_ = c; }
    void failAfter(int n) { failAt_ = n < 0 ? -1 : static_cast<std::int64_t>(sent_) + n; }   // n < 0 = never; else n more succeed, then fail
    void setExpectedGeneration(Generation g) { expectGeneration_ = g; }

private:
    static constexpr std::size_t kMaxLog = 4096;
    bool connected_ = true;
    std::int64_t failAt_ = -1;
    Generation expectGeneration_ = 0;
    std::uint64_t sent_ = 0, bytes_ = 0, attempts_ = 0, failures_ = 0, stale_ = 0;
    std::vector<Datagram> log_;
};

}  // namespace ss::lighting
