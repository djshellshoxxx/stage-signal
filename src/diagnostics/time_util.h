// MOD-16 small UTC time formatting helpers (no locale, no localtime).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace ss {
struct CivilTime { int y, mo, d, h, mi, s, ms; };
inline CivilTime civilFromUnixMs(int64_t unixMs) {
  int64_t ms = unixMs % 1000; int64_t secs = unixMs / 1000; if (ms < 0) { ms += 1000; --secs; }
  int64_t days = secs / 86400; int64_t rem = secs % 86400; if (rem < 0) { rem += 86400; --days; }
  int64_t z = days + 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1; int64_t m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) ++y;
  return {(int)y, (int)m, (int)d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60), (int)ms};
}
inline std::string isoUtc(int64_t unixMs) {
  CivilTime c = civilFromUnixMs(unixMs); char b[40];
  std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", c.y, c.mo, c.d, c.h, c.mi, c.s, c.ms); return b;
}
}
