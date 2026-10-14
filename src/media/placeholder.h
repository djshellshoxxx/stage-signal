// Documented placeholder for missing/invalid/failed media (REQ-S08/X03): a magenta/black checkerboard with the
// text "MISSING MEDIA" in a built-in 5x7 font. Deterministic, procedural, no allocation beyond the frame itself.
#pragma once
#include "core/types.h"
#include <memory>

namespace ss { namespace media {

constexpr int kPlaceholderCell = 16;                 // checker cell size in pixels
constexpr uint32_t kPlaceholderA = 0xFF00FF;         // magenta
constexpr uint32_t kPlaceholderB = 0x101010;         // near black
constexpr uint32_t kPlaceholderText = 0xFFFFFF;
Frame makePlaceholderFrame(int w, int h);            // w,h clamped to [16, 4096]
std::shared_ptr<const Frame> sharedPlaceholder(int w, int h);

}} // namespace ss::media
