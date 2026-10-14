// Internal: text layout + bitmap cache (spec §13 limits: <=4096 code points; cache <=8 MiB shared budget).
// Bitmaps are 8-bit coverage masks (color/alpha applied at blit time so fades never invalidate the cache).
#pragma once
#include "core/ports.h"
#include "render/render_state.h"
#include <list>

namespace ss {

struct TextBitmap {
  int w = 0, h = 0;
  std::vector<uint8_t> a;          // w*h coverage
  bool truncated = false;          // code-point limit or height budget hit ("..." appended on last visible line)
  int lines = 0;
};

class TextEngine {
public:
  static constexpr size_t kBudgetBytes = 8u << 20;       // shared cache budget
  static constexpr size_t kMaxMaskBytes = 4u << 20;      // one bitmap never exceeds half the budget
  static constexpr size_t kMaxCodePoints = 4096;
  void setRasterizer(ITextRasterizer* r) { ras_ = r; clear(); }
  // Returns the (cached) mask for `text` wrapped to maxW pixels at line height px. Pointer valid until the next get()/clear().
  const TextBitmap* get(const std::u32string& text, int px, int maxW, HAlign align, bool bold);
  void clear() { entries_.clear(); bytes_ = 0; }
  size_t bytes() const { return bytes_; }
  size_t entries() const { return entries_.size(); }
  // Deterministic built-in layout (also used by tests): wraps at spaces, hard-breaks long words, honours '\n', truncates.
  static TextBitmap buildBuiltin(const std::u32string& text, int px, int maxW, HAlign align, bool bold);
  static int builtinPitch(int px);                       // advance of one glyph in pixels
private:
  struct Entry { std::u32string text; int px, maxW; uint8_t align, bold; uint64_t used; TextBitmap bm; size_t cost; };
  ITextRasterizer* ras_ = nullptr;
  std::list<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t tick_ = 0;
  TextBitmap build(const std::u32string& text, int px, int maxW, HAlign align, bool bold);
};

// dst = lerp(dst, color, mask*alpha) for mask pixels in src rect [sx,sx+cw)x[sy,sy+ch) placed at (dx,dy); clipped to dst.
void blitMask(uint32_t* dst, int dstW, int dstH, const TextBitmap& bm, int sx, int sy, int cw, int ch, int dx, int dy,
              uint32_t color, int alphaQ8);

} // namespace ss
