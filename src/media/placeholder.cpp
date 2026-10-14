#include "media/placeholder.h"
#include <cstring>
#include <algorithm>

namespace ss { namespace media {

namespace {
struct Glyph { char c; uint8_t rows[7]; };
const Glyph kGlyphs[] = {
  {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
  {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
  {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
};
const uint8_t* glyph(char c) { for (const Glyph& g : kGlyphs) if (g.c == c) return g.rows; return nullptr; }
}

Frame makePlaceholderFrame(int w, int h) {
  w = std::min(4096, std::max(16, w)); h = std::min(4096, std::max(16, h));
  Frame f; f.resize(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      f.px[(size_t)y * w + x] = (((x / kPlaceholderCell) + (y / kPlaceholderCell)) & 1) ? kPlaceholderA : kPlaceholderB;
  const char* text = "MISSING MEDIA";
  const int n = (int)std::strlen(text);
  int scale = std::max(1, std::min((w - 8) / (n * 6), h / 24));
  int tw = n * 6 * scale - scale, th = 7 * scale;
  int ox = (w - tw) / 2, oy = (h - th) / 2;
  if (ox < 0 || oy < 0) return f;
  // opaque black plate behind the text keeps it legible on the checkerboard
  for (int y = oy - scale; y < oy + th + scale && y < h; ++y)
    for (int x = ox - scale; x < ox + tw + scale && x < w; ++x) if (x >= 0 && y >= 0) f.px[(size_t)y * w + x] = 0x000000;
  for (int i = 0; i < n; ++i) {
    const uint8_t* g = glyph(text[i]);
    if (!g) continue;
    for (int r = 0; r < 7; ++r)
      for (int c = 0; c < 5; ++c)
        if (g[r] & (0x10 >> c))
          for (int sy = 0; sy < scale; ++sy)
            for (int sx = 0; sx < scale; ++sx)
              f.px[(size_t)(oy + r * scale + sy) * w + (ox + (i * 6 + c) * scale + sx)] = kPlaceholderText;
  }
  return f;
}
std::shared_ptr<const Frame> sharedPlaceholder(int w, int h) { return std::make_shared<const Frame>(makePlaceholderFrame(w, h)); }

}} // namespace ss::media
