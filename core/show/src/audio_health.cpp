#include "ss/show/audio_health.hpp"

namespace ss::show {

Status validate(const AudioHealthConfig& c) {
    if (c.detectNs <= 0 || c.detectNs > AudioHealthConfig::kMaxDetectNs)
        return makeError(ErrorCategory::InvalidArgument, "audio loss detect time must be 1 ns to 2 s");
    if (c.restoreHoldNs < 0 || c.restoreHoldNs > AudioHealthConfig::kMaxRestoreHoldNs)
        return makeError(ErrorCategory::InvalidArgument, "audio restore hold must be 0 to 10 s");
    return okStatus();
}

AudioHealthMonitor::AudioHealthMonitor(const AudioHealthConfig& c) : cfg_(validate(c).ok() ? c : AudioHealthConfig{}) {}

AudioHealthState AudioHealthMonitor::update(Ns now, bool available) {
    if (started_ && now < last_) {              // non-monotonic input: restart the running timers from here
        if (bad_) badSince_ = now;
        if (good_) goodSince_ = now;
    }
    started_ = true;
    last_ = now;
    AudioHealthState s;
    s.lost = lost_;
    if (available) {
        bad_ = false;
        if (!good_) { good_ = true; goodSince_ = now; }
        if (lost_ && now - goodSince_ >= cfg_.restoreHoldNs) { lost_ = false; s.changed = true; }
    } else {
        good_ = false;
        if (!bad_) { bad_ = true; badSince_ = now; }
        if (!lost_ && now - badSince_ >= cfg_.detectNs) { lost_ = true; s.changed = true; }
    }
    s.lost = lost_;
    return s;
}

}  // namespace ss::show
