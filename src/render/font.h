// Internal: built-in 5x11 bitmap font access (6x12 cell with spacing).
#pragma once
#include <cstdint>
namespace ss { namespace font {
constexpr int kGlyphCols = 5, kGlyphRows = 11, kCellW = 6, kCellH = 12;
// Returns 11 row bytes (bit 4 = leftmost column). Unknown code points yield the box glyph (isBox = true).
const uint8_t* glyph(char32_t cp, bool& isBox);
}}
