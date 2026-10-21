// Scene transitions between two same-sized opaque buffers, driven by a progress value 0..1 (REQ-S/section 13).
//  Cut:              progress <= 0 -> A, progress > 0 -> B.
//  Crossfade:        per-channel premultiplied mix; progress 0 == A and 1 == B bit-exactly.
//  FadeThroughBlack: 0..0.5 dims A to black (RGB only, alpha kept), 0.5..1 brightens B from black.
#pragma once
#include "ss/compose/pixel.hpp"

namespace ss::compose {

// Progress quantised to 0..255 (NaN -> 0). Deterministic.
std::uint32_t progressWeight255(float progress);

// In-place: `a` becomes the transitioned result. No allocation. Returns false (a untouched) on size mismatch.
bool applyTransitionInPlace(PixelBuffer& a, const PixelBuffer& b, ss::model::TransitionKind kind, float progress);

// Convenience for tests/off-path use: out = transition(a, b). May allocate `out`.
bool blendTransition(const PixelBuffer& a, const PixelBuffer& b, ss::model::TransitionKind kind, float progress,
                     PixelBuffer& out);

}  // namespace ss::compose
