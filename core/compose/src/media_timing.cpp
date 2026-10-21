#include "ss/compose/media_timing.hpp"
#include <algorithm>
#include <cmath>
#include "ss/compose/limits.hpp"

namespace ss::compose {

GifDelay clampGifDelay(Ns declared) {
    const Ns minNs = msToNs(Limits::kMinGifDelayMs);
    if (declared < minNs) return GifDelay{minNs, true};
    return GifDelay{declared, false};
}

static double clampSpeed(double s, double lo, double hi) {
    if (!(s == s)) return 1.0;
    return std::min(hi, std::max(lo, s));
}
double clampGifSpeed(double s) { return clampSpeed(s, Limits::kGifSpeedMin, Limits::kGifSpeedMax); }
double clampVideoSpeed(double s) { return clampSpeed(s, Limits::kVideoSpeedMin, Limits::kVideoSpeedMax); }

RectF sanitizeCrop(RectF r) {
    auto c01 = [](float v) { return v != v ? 0.f : std::min(1.f, std::max(0.f, v)); };
    const float x = c01(r.x), y = c01(r.y);
    float w = c01(r.w), h = c01(r.h);
    if (w <= 0.f) w = 1.f - x > 0.f ? 1.f - x : 1.f;
    if (h <= 0.f) h = 1.f - y > 0.f ? 1.f - y : 1.f;
    w = std::min(w, 1.f - x);
    h = std::min(h, 1.f - y);
    if (w <= 0.f || h <= 0.f) return RectF{0.f, 0.f, 1.f, 1.f};
    return RectF{x, y, w, h};
}

ss::Result<GifTimeline> GifTimeline::create(const std::vector<Ns>& delays, double speed, std::uint32_t loops) {
    if (delays.empty() || delays.size() > Limits::kMaxGifFrames)
        return ss::makeError(ss::ErrorCategory::InvalidArgument, "gif frame count out of range");
    GifTimeline t;
    t.ends_.reserve(delays.size());
    Ns acc = 0;
    for (Ns d : delays) {
        const GifDelay g = clampGifDelay(d);
        t.clampedAny_ = t.clampedAny_ || g.clamped;
        acc += g.effective;
        t.ends_.push_back(acc);
    }
    t.speedMilli_ = std::llround(clampGifSpeed(speed) * 1000.0);
    t.loops_ = loops;
    return t;
}

GifPosition GifTimeline::at(Ns elapsed) const {
    GifPosition p;
    if (ends_.empty()) return p;
    if (elapsed < 0) elapsed = 0;
    const Ns total = ends_.back();
    // scaled = elapsed * speed ; elapsed bounded well below overflow for show-length runtimes (< 1e15 ns * 4000)
    const Ns scaled = elapsed * speedMilli_ / 1000;
    const std::uint64_t loopIdx = static_cast<std::uint64_t>(scaled / total);
    if (loops_ != 0 && loopIdx >= loops_) {
        p.finished = true;
        p.loop = loops_;
        p.frame = ends_.size() - 1;
        return p;
    }
    const Ns inLoop = scaled % total;
    p.loop = static_cast<std::uint32_t>(std::min<std::uint64_t>(loopIdx, 0xFFFFFFFFu));
    p.frame = static_cast<std::size_t>(std::upper_bound(ends_.begin(), ends_.end(), inLoop) - ends_.begin());
    if (p.frame >= ends_.size()) p.frame = ends_.size() - 1;
    return p;
}

ss::Result<VideoTimeline> VideoTimeline::create(Ns duration, double speed, bool loop) {
    if (duration <= 0) return ss::makeError(ss::ErrorCategory::InvalidArgument, "video duration must be positive");
    VideoTimeline t;
    t.duration_ = duration;
    t.speedMilli_ = std::llround(clampVideoSpeed(speed) * 1000.0);
    t.loop_ = loop;
    return t;
}

VideoPosition VideoTimeline::at(Ns elapsed) const {
    VideoPosition p;
    if (elapsed < 0) elapsed = 0;
    const Ns scaled = elapsed * speedMilli_ / 1000;
    if (loop_) {
        p.loop = static_cast<std::uint32_t>(std::min<Ns>(scaled / duration_, 0xFFFFFFFFLL));
        p.mediaPts = scaled % duration_;
    } else if (scaled >= duration_) {
        p.mediaPts = duration_;
        p.finished = true;
    } else {
        p.mediaPts = scaled;
    }
    return p;
}

}  // namespace ss::compose
