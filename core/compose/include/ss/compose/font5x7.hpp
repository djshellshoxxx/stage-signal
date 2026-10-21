// Built-in 5x7 ASCII bitmap font (printable 0x20..0x7E) so core can draw labels without system fonts.
// Glyph storage: 5 column bytes, bit0 = top row. Non-ASCII / control characters render as '?'.
#pragma once
#include <cstdint>
#include "ss/compose/pixel.hpp"

namespace ss::compose {

constexpr int kGlyphW = 5, kGlyphH = 7, kGlyphAdvance = 6;  // 1 column spacing

const std::uint8_t* glyphColumns(char c);                    // 5 bytes; '?' for unsupported characters
bool glyphPixel(char c, int col, int row);                   // col 0..4, row 0..6
int textWidth(const char* s, int scale);                     // pixels, no trailing gap; 0 for empty
int textHeight(int scale);
// Draw ASCII text with top-left at (x,y); glyph pixels are scale x scale squares; clipped to dst.
void drawText(PixelBuffer& dst, int x, int y, const char* s, int scale, Rgb colour, std::uint32_t alpha255 = 255);

}  // namespace ss::compose
