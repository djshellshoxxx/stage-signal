// Animation timing on the MEDIA clock (spec §13): elapsed nanoseconds -> frame index, never render-tick counts.
#pragma once
#include "core/types.h"
#include <vector>

namespace ss { namespace media {

constexpr double kGifSpeedMin = 0.25, kGifSpeedMax = 4.0;
constexpr double kVideoSpeedMin = 0.25, kVideoSpeedMax = 2.0;
double clampSpeed(double s, double lo, double hi);   // NaN/<=0 -> 1.0 (neutral), otherwise clamped

// cumEndNs[i] = end time of frame i (strictly increasing). mediaNs is already speed-scaled media time.
// loop: wraps modulo total; no loop: holds the last frame. Negative time -> frame 0.
size_t animFrameIndex(const std::vector<Ns>& cumEndNs, Ns mediaNs, bool loop);
std::vector<Ns> cumulativeEnds(const std::vector<int>& delayMs);

// Converts wall-of-media-clock elapsed time into speed-scaled media time, correct across speed changes.
struct MediaCursor {
  Ns lastElapsed = 0; double mediaNs = 0; bool started = false;
  Ns advance(Ns elapsedNs, double speed) {     // elapsed is monotonic per source; going backwards restarts from it
    if (!started || elapsedNs < lastElapsed) { started = true; mediaNs = (double)elapsedNs * speed; }
    else mediaNs += (double)(elapsedNs - lastElapsed) * speed;
    lastElapsed = elapsedNs; return (Ns)mediaNs;
  }
};

}} // namespace ss::media
