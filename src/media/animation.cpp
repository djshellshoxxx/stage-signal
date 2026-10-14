#include "media/animation.h"
#include <cmath>
#include <algorithm>

namespace ss { namespace media {

double clampSpeed(double s, double lo, double hi) {
  if (!(s > 0) || std::isnan(s)) return 1.0;
  return std::min(hi, std::max(lo, s));
}
std::vector<Ns> cumulativeEnds(const std::vector<int>& delayMs) {
  std::vector<Ns> v; v.reserve(delayMs.size()); Ns t = 0;
  for (int d : delayMs) { t += (Ns)d * kNsPerMs; v.push_back(t); }
  return v;
}
size_t animFrameIndex(const std::vector<Ns>& ends, Ns t, bool loop) {
  if (ends.empty()) return 0;
  Ns total = ends.back();
  if (t < 0 || total <= 0) return 0;
  if (t >= total) { if (!loop) return ends.size() - 1; t %= total; }
  return (size_t)(std::upper_bound(ends.begin(), ends.end(), t) - ends.begin());
}

}} // namespace ss::media
