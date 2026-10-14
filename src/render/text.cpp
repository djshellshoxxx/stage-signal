#include "render/text.h"
#include "render/font.h"
#include "render/fixmath.h"
#include <algorithm>

namespace ss {

int TextEngine::builtinPitch(int px) {
  int s = px * 256 / font::kCellH;
  int p = (font::kCellW * s + 128) >> 8;
  return p < 1 ? 1 : p;
}

TextBitmap TextEngine::buildBuiltin(const std::u32string& in, int px, int maxW, HAlign align, bool bold) {
  TextBitmap bm;
  if (px < 6) px = 6;
  if (maxW < 1) maxW = 1;
  std::u32string text = in;
  if (text.size() > kMaxCodePoints) { text.resize(kMaxCodePoints); bm.truncated = true; }
  std::u32string clean; clean.reserve(text.size());
  for (char32_t c : text) { if (c == '\r') continue; if (c == '\t') c = ' '; if (c < 32 && c != '\n') continue; clean.push_back(c); }
  const int pitch = builtinPitch(px), lineH = px;
  const int s = px * 256 / font::kCellH;
  int maxCols = maxW / pitch; if (maxCols < 1) maxCols = 1;
  size_t maxLines = kMaxMaskBytes / ((size_t)maxW * (size_t)lineH); if (maxLines < 1) maxLines = 1;

  struct Ln { size_t b, e; };
  std::vector<Ln> lines;
  size_t i = 0, n = clean.size();
  while (i <= n) {
    size_t b = i, lastSpace = (size_t)-1, col = 0, j = b;
    for (; j < n; ++j) {
      if (clean[j] == '\n') break;
      if (clean[j] == ' ') lastSpace = j;
      if (col == (size_t)maxCols) {
        if (lastSpace != (size_t)-1 && lastSpace > b) { lines.push_back({b, lastSpace}); i = lastSpace + 1; goto nextLine; }
        lines.push_back({b, j}); i = j; goto nextLine;
      }
      ++col;
    }
    lines.push_back({b, j});
    i = j + 1;                                   // skip '\n' (or end)
    if (j >= n) break;
    nextLine:;
    if (lines.size() > maxLines + 1) break;
  }
  if (lines.size() > maxLines) { lines.resize(maxLines); bm.truncated = true; }
  // trim trailing spaces; apply ellipsis on truncated last line
  std::vector<std::u32string> ls;
  for (auto& l : lines) { size_t e = l.e; while (e > l.b && clean[e - 1] == ' ') --e; ls.emplace_back(clean.substr(l.b, e - l.b)); }
  while (ls.size() > 1 && ls.back().empty() && !bm.truncated) ls.pop_back();
  if (bm.truncated && !ls.empty()) {
    std::u32string& L = ls.back();
    if ((int)L.size() + 3 > maxCols) L.resize(maxCols > 3 ? maxCols - 3 : 0);
    L += U"...";
  }
  size_t widest = 1;
  for (auto& l : ls) widest = std::max(widest, l.size());
  bm.w = (int)std::min<size_t>(widest * (size_t)pitch, (size_t)maxW); if (bm.w < 1) bm.w = 1;
  bm.lines = (int)ls.size();
  bm.h = (int)ls.size() * lineH;
  bm.a.assign((size_t)bm.w * bm.h, 0);
  std::vector<int> colmap(pitch), rowmap(lineH);
  for (int d = 0; d < pitch; ++d) colmap[d] = d * 256 / s;           // 0..5 (5 = spacing)
  for (int d = 0; d < lineH; ++d) rowmap[d] = d * 256 / s;           // 0..11 (11 = spacing)
  for (size_t li = 0; li < ls.size(); ++li) {
    int lw = (int)ls[li].size() * pitch;
    int ox = align == HAlign::Center ? (bm.w - lw) / 2 : (align == HAlign::Right ? bm.w - lw : 0);
    if (ox < 0) ox = 0;
    for (size_t ci = 0; ci < ls[li].size(); ++ci) {
      bool box; const uint8_t* g = font::glyph(ls[li][ci], box);
      int x0 = ox + (int)ci * pitch;
      for (int dy = 0; dy < lineH; ++dy) {
        int sr = rowmap[dy]; if (sr >= font::kGlyphRows) continue;
        uint8_t bits = g[sr]; if (!bits) continue;
        uint8_t* row = &bm.a[(size_t)(li * lineH + dy) * bm.w];
        for (int dx = 0; dx < pitch && x0 + dx < bm.w; ++dx) {
          int sc = colmap[dx];
          if (sc < font::kGlyphCols && (bits & (1u << (4 - sc)))) row[x0 + dx] = 255;
        }
      }
    }
  }
  if (bold) {
    int k = std::max(1, pitch / 6);
    for (int y = 0; y < bm.h; ++y) {
      uint8_t* row = &bm.a[(size_t)y * bm.w];
      for (int x = bm.w - 1; x >= 1; --x) for (int q = 1; q <= k && x - q >= 0; ++q) if (row[x - q] > row[x]) row[x] = row[x - q];
    }
  }
  return bm;
}

TextBitmap TextEngine::build(const std::u32string& text, int px, int maxW, HAlign align, bool bold) {
  if (ras_) {
    std::u32string t = text; bool trunc = false;
    if (t.size() > kMaxCodePoints) { t.resize(kMaxCodePoints); trunc = true; }
    TextStyle st; st.px = px; st.color = 0xFFFFFF; st.bold = bold;
    int ow = 0, oh = 0; std::vector<uint32_t> argb;
    if (ras_->rasterize(t, st, maxW, ow, oh, argb) && ow >= 1 && oh >= 1 && ow <= maxW && oh <= 16384 &&
        argb.size() >= (size_t)ow * (size_t)oh) {
      TextBitmap bm; bm.w = ow; bm.truncated = trunc;
      size_t maxRows = kMaxMaskBytes / (size_t)ow;
      if ((size_t)oh > maxRows) { oh = (int)maxRows; bm.truncated = true; }
      bm.h = oh; bm.a.resize((size_t)ow * oh);
      for (size_t i = 0; i < bm.a.size(); ++i) bm.a[i] = (uint8_t)(argb[i] >> 24);
      bm.lines = std::max(1, oh / std::max(1, px));
      return bm;
    }
  }
  return buildBuiltin(text, px, maxW, align, bold);
}

const TextBitmap* TextEngine::get(const std::u32string& text, int px, int maxW, HAlign align, bool bold) {
  ++tick_;
  for (auto& e : entries_)
    if (e.px == px && e.maxW == maxW && e.align == (uint8_t)align && e.bold == (uint8_t)bold && e.text == text) { e.used = tick_; return &e.bm; }
  Entry e; e.text = text; if (e.text.size() > kMaxCodePoints) e.text.resize(kMaxCodePoints);
  e.px = px; e.maxW = maxW; e.align = (uint8_t)align; e.bold = bold; e.used = tick_;
  e.bm = build(text, px, maxW, align, bold);
  e.cost = e.bm.a.size() + e.text.size() * sizeof(char32_t) + 128;
  while (!entries_.empty() && bytes_ + e.cost > kBudgetBytes) {
    auto victim = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) if (it->used < victim->used) victim = it;
    bytes_ -= victim->cost; entries_.erase(victim);
  }
  bytes_ += e.cost;
  entries_.push_back(std::move(e));
  return &entries_.back().bm;
}

void blitMask(uint32_t* dst, int dstW, int dstH, const TextBitmap& bm, int sx, int sy, int cw, int ch, int dx, int dy,
              uint32_t color, int alphaQ8) {
  if (alphaQ8 <= 0) return;
  if (sx < 0) { dx -= sx; cw += sx; sx = 0; }
  if (sy < 0) { dy -= sy; ch += sy; sy = 0; }
  if (sx + cw > bm.w) cw = bm.w - sx;
  if (sy + ch > bm.h) ch = bm.h - sy;
  if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
  if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
  if (dx + cw > dstW) cw = dstW - dx;
  if (dy + ch > dstH) ch = dstH - dy;
  if (cw <= 0 || ch <= 0) return;
  if (alphaQ8 > 256) alphaQ8 = 256;
  for (int y = 0; y < ch; ++y) {
    const uint8_t* m = &bm.a[(size_t)(sy + y) * bm.w + sx];
    uint32_t* d = dst + (size_t)(dy + y) * dstW + dx;
    for (int x = 0; x < cw; ++x) {
      if (!m[x]) continue;
      uint32_t q = ((uint32_t)m[x] * (uint32_t)alphaQ8) >> 8;     // 0..255
      q += q >> 7;
      d[x] = rfx::lerpQ8(d[x], color, q);
    }
  }
}

} // namespace ss
