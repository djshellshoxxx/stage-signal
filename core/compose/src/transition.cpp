#include "ss/compose/transition.hpp"
#include <cmath>
#include <cstring>
#include "ss/compose/raster.hpp"

namespace ss::compose {

std::uint32_t progressWeight255(float p) {
    if (!(p > 0.f)) return 0;
    if (p >= 1.f) return 255;
    return static_cast<std::uint32_t>(std::lround(double(p) * 255.0));
}

bool applyTransitionInPlace(PixelBuffer& a, const PixelBuffer& b, ss::model::TransitionKind kind, float progress) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) return false;
    using TK = ss::model::TransitionKind;
    switch (kind) {
        case TK::Cut:
            if (progress > 0.f && !a.rgba.empty()) std::memcpy(a.rgba.data(), b.rgba.data(), a.rgba.size());
            return true;
        case TK::Crossfade: {
            const std::uint32_t w = progressWeight255(progress);
            if (w == 0) return true;
            if (w == 255) {
                if (!a.rgba.empty()) std::memcpy(a.rgba.data(), b.rgba.data(), a.rgba.size());
                return true;
            }
            mixInto(a, b, w);
            return true;
        }
        case TK::FadeThroughBlack: {
            // k in 0..510: 0..255 dims A, 255 = black, 255..510 brightens B
            const std::uint32_t k = !(progress > 0.f) ? 0u : (progress >= 1.f ? 510u : static_cast<std::uint32_t>(std::lround(double(progress) * 510.0)));
            if (k <= 255) {
                scaleRgb256(a, 256 - ((k * 256 + 127) / 255));  // k=0 -> 256 (identity), k=255 -> 0
            } else {
                if (!a.rgba.empty()) std::memcpy(a.rgba.data(), b.rgba.data(), a.rgba.size());
                const std::uint32_t up = k - 255;  // 0..255 -> 0..256 brightness
                scaleRgb256(a, (up * 256 + 127) / 255);
            }
            return true;
        }
    }
    return false;
}

bool blendTransition(const PixelBuffer& a, const PixelBuffer& b, ss::model::TransitionKind kind, float progress,
                     PixelBuffer& out) {
    if (a.width != b.width || a.height != b.height) return false;
    out = a;
    return applyTransitionInPlace(out, b, kind, progress);
}

}  // namespace ss::compose
