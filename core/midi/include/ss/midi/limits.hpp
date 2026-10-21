// StageSignal midi: bounds and policy constants (spec sections 8, 14; REQ-M01..M04).
#pragma once
#include <cstddef>
#include <cstdint>
#include "ss/base/time.hpp"

namespace ss::midi {

constexpr std::size_t kEventQueueCapacity = 512;     // input pending-action queue (section 8)
constexpr std::size_t kOutputQueueCapacity = 512;    // ordinary scheduled output
constexpr std::size_t kOutputHardCapacity = 2048;    // ceiling including must-deliver items (note-off, transport)
constexpr std::size_t kMaxSysexBytes = 1024;         // including F0 and F7; disabled by default
constexpr std::uint32_t kRateCeilingPerSec = 1000;   // supported messages/s/port
constexpr Ns kRateWindowNs = kNsPerSec;
constexpr Ns kSceneDebounceNs = 50 * kNsPerMs;       // scene triggers
constexpr Ns kLossMarkDeadlineNs = 1 * kNsPerSec;    // REQ-M04: mapping unavailable within 1 s of detected loss
constexpr Ns kDevicePollPeriodNs = 500 * kNsPerMs;   // recommended adapter enumeration period (loss detection)
constexpr int kFeedbackTriggerLimit = 20;            // more than this many identical trigger events ...
constexpr Ns kFeedbackWindowNs = 1 * kNsPerSec;      // ... inside this window latches the alarm
constexpr Ns kEchoWindowNs = 250 * kNsPerMs;         // inbound message matching a recent output = echo
constexpr std::size_t kMaxTrackedStormKeys = 128;
constexpr std::size_t kEchoHistory = 128;
constexpr int kClockPpqn = 24;
constexpr double kMinBpm = 20.0, kMaxBpm = 300.0;
constexpr Ns kClockStaleNs = 50 * kNsPerMs;          // late clock pulses beyond this are discarded, never burst
constexpr Ns kNoteOnStaleNs = 250 * kNsPerMs;        // late note-on / CC beyond this are discarded
constexpr std::size_t kMaxPorts = 64;

}  // namespace ss::midi
