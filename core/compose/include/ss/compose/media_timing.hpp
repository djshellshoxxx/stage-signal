// Media-clock timing helpers for already-decoded GIF / video / still layers (decoding is NOT here).
// Animation position is a pure function of elapsed media time, never of the render tick count (section 13).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/compose/pixel.hpp"

namespace ss::compose {

// Zero / sub-20 ms GIF delays clamp to 20 ms and raise a warning flag.
struct GifDelay { Ns effective = 0; bool clamped = false; };
GifDelay clampGifDelay(Ns declared);

double clampGifSpeed(double s);    // 0.25 .. 4   (NaN -> 1)
double clampVideoSpeed(double s);  // 0.25 .. 2   (NaN -> 1)

// Normalised crop rectangle sanitiser: clamps into 0..1 and guarantees a non-empty rectangle.
RectF sanitizeCrop(RectF r);

struct GifPosition {
    std::size_t frame = 0;
    bool finished = false;   // non-looping GIF past its last frame (frame stays on the last one)
    std::uint32_t loop = 0;  // completed loops
};

class GifTimeline {
public:
    // delays are the declared per-frame delays; 1..1000 frames. loops: 0 = infinite, N = play N times.
    static ss::Result<GifTimeline> create(const std::vector<Ns>& declaredDelays, double speed = 1.0,
                                          std::uint32_t loops = 0);
    GifPosition at(Ns elapsedMedia) const;     // elapsedMedia since layer start on the media clock
    std::size_t frameCount() const { return ends_.size(); }
    Ns totalDuration() const { return ends_.empty() ? 0 : ends_.back(); }   // unscaled
    bool anyDelayClamped() const { return clampedAny_; }                     // warning flag
    double speed() const { return speedMilli_ / 1000.0; }
private:
    std::vector<Ns> ends_;   // cumulative end time of each frame (effective delays)
    std::int64_t speedMilli_ = 1000;
    std::uint32_t loops_ = 0;
    bool clampedAny_ = false;
};

struct VideoPosition {
    Ns mediaPts = 0;         // presentation time the layer should show
    bool finished = false;   // non-looping video past its end (mediaPts == duration)
    std::uint32_t loop = 0;
};
class VideoTimeline {
public:
    static ss::Result<VideoTimeline> create(Ns duration, double speed = 1.0, bool loop = false);
    VideoPosition at(Ns elapsedMedia) const;
private:
    Ns duration_ = 0;
    std::int64_t speedMilli_ = 1000;
    bool loop_ = false;
};

}  // namespace ss::compose
