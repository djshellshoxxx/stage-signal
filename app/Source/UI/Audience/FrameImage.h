// Wraps a composed PixelBuffer (RGBA8 premultiplied) in a reusable juce::Image (premultiplied ARGB).
// Owner: ui-live. The Image is reallocated only when the size changes, so a steady stream of frames costs
// one pass over the pixels and no allocation. Used by audience windows and the Live preview.
#pragma once
#include <juce_graphics/juce_graphics.h>
#include "ss/base/types.hpp"

namespace ss::app::audience {

inline void copyFrame(const ss::PixelBuffer& px, juce::Image& dst) {
    if (px.empty()) return;
    const int w = static_cast<int>(px.width), h = static_cast<int>(px.height);
    if (dst.isNull() || dst.getWidth() != w || dst.getHeight() != h)
        dst = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Image::BitmapData bd(dst, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<juce::PixelARGB*>(bd.getLinePointer(y));
        const std::uint8_t* src = px.row(static_cast<std::uint32_t>(y));
        for (int x = 0; x < w; ++x, src += 4) line[x].setARGB(src[3], src[0], src[1], src[2]);
    }
}

}  // namespace ss::app::audience
