// Contract: sample index -> monotonic ns (app-adapters 3.2, APP-AD-03). Owner: engine-adapters-desktop.
// Real-time safe: map() allocates nothing and takes no lock. Only a converged mapper reports valid=true.
#pragma once
#include <cstdint>
#include "ss/base/time.hpp"

namespace ss::app {

enum class MapperState : unsigned char { Unconverged, Converged, Degraded };

struct MappedTime {
    ss::Ns captureNs = 0;
    bool valid = false;      // becomes kAudioTimestampValid; never invented when unknown
};

class ITimestampMapper {
public:
    virtual ~ITimestampMapper() = default;
    virtual MappedTime map(std::uint64_t firstSampleIndex, std::uint32_t rateHz, ss::Ns callbackEntryNs) = 0;  // audio thread
    virtual void reset() = 0;                 // new stream generation
    virtual MapperState state() const = 0;
};

// Honest default: reports callback entry time but never claims validity.
class UnknownTimestampMapper final : public ITimestampMapper {
public:
    MappedTime map(std::uint64_t, std::uint32_t, ss::Ns entry) override { return {entry, false}; }
    void reset() override {}
    MapperState state() const override { return MapperState::Unconverged; }
};

// Deterministic mapper for tests: capture = entry - fixed latency, valid immediately.
class FixedLatencyTimestampMapper final : public ITimestampMapper {
public:
    explicit FixedLatencyTimestampMapper(ss::Ns latency = 0) : latency_(latency) {}
    MappedTime map(std::uint64_t, std::uint32_t, ss::Ns entry) override { return {entry - latency_, true}; }
    void reset() override {}
    MapperState state() const override { return MapperState::Converged; }
private:
    ss::Ns latency_;
};

}  // namespace ss::app
