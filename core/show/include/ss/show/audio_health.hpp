// StageSignal show engine: audio-loss detector for REQ-D09. Pure state machine, no clock or I/O of its own.
// The app reports "is the capture device delivering audio" (IAudioSource health); this class debounces it so a
// brief glitch never flickers the audience output and a real loss is confirmed within the spec's 2 s.
#pragma once
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"

namespace ss::show {

// What the show does once audio loss is confirmed. Continue = keep the show running and only report it.
enum class AudioLossPolicy : std::uint8_t { Continue, Idle };

struct AudioHealthConfig {
    Ns detectNs = 1000 * kNsPerMs;       // loss must persist this long to be confirmed (hard max kMaxDetectNs)
    Ns restoreHoldNs = 500 * kNsPerMs;   // audio must stay good this long before loss is cleared
    static constexpr Ns kMaxDetectNs = 2000 * kNsPerMs;      // REQ-D09: policy applies within 2 s
    static constexpr Ns kMaxRestoreHoldNs = 10 * kNsPerSec;
};

// InvalidArgument when a value is out of range. An invalid config given to the monitor is replaced by the
// defaults, so detection can never be configured away.
Status validate(const AudioHealthConfig& c);

struct AudioHealthState {
    bool lost = false;      // loss confirmed (stays true through short recoveries until the restore hold passes)
    bool changed = false;   // `lost` flipped on this update
};

class AudioHealthMonitor {
public:
    explicit AudioHealthMonitor(const AudioHealthConfig& c = {});
    // `available`: the capture device is present and delivering. Call at the tick rate with a monotonic time;
    // a time that moves backwards re-bases the timers instead of confirming or clearing loss early.
    AudioHealthState update(Ns now, bool available);
    bool lost() const { return lost_; }
    const AudioHealthConfig& config() const { return cfg_; }

private:
    AudioHealthConfig cfg_;
    bool lost_ = false, started_ = false, bad_ = false, good_ = false;
    Ns last_ = 0, badSince_ = 0, goodSince_ = 0;
};

}  // namespace ss::show
