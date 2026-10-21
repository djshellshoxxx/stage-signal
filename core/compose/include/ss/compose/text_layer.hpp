// Published-text compositing (REQ-S06). The show engine owns the draft/preview/publish data model; here only
// pre-rasterised bitmaps (the app rasterises Unicode with system fonts) are composited with placement, size,
// colour, duration, fade and bounded scroll.
//  * Bitmap: premultiplied RGBA. Default = coverage mask (alpha only) tinted with `color`; colorBitmap=true uses
//    the bitmap's own colour (emoji / pre-coloured text) and ignores `color`.
//  * `utf8` is the message text that accompanies the bitmap; it must be well-formed and <= 4096 code points.
//  * Scroll is one bounded right-to-left pass (no wrap, no repeats), lasting clamp(duration, 2 s, 120 s).
//  * Non-scrolling text that is wider than the canvas is scaled down to fit (never silently clipped).
#pragma once
#include <array>
#include <memory>
#include <string>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/compose/limits.hpp"
#include "ss/compose/pixel.hpp"

namespace ss::compose {

struct TextItem {
    std::uint32_t id = 0;
    std::string utf8;                                   // accompanying message text (validated)
    std::shared_ptr<const PixelBuffer> bitmap;          // required, premultiplied RGBA, <= 4096 x 4096 / 64 MiB
    bool colorBitmap = false;
    float x = 0.5f, y = 0.5f;                           // normalised centre (clamped 0..1)
    float size = 0.12f;                                 // rendered bitmap height / canvas height (0.01..1)
    Rgb color{255, 255, 255};
    double durationSec = 5.0;                           // 0 = hold until cleared; else 0.1..3600
    double fadeSec = 0.5;                               // fade in and out; clamped to duration/2
    bool scroll = false;
};

// Opacity envelope 0..256 at `elapsed` for a static item; 0 at/after the end. Exposed for tests.
std::uint32_t textEnvelope256(Ns elapsed, double durationSec, double fadeSec);
// Scroll progress 0..1 (>=1 means the pass is finished).
double scrollProgress(Ns elapsed, double durationSec);

class TextLayer {
public:
    // Control path (may allocate). Replaces an active item with the same id; otherwise needs a free slot.
    ss::Status publish(const TextItem& item, Ns now);
    void clear();
    void clearId(std::uint32_t id);
    void purgeExpired(Ns now);                // releases finished items (control path)
    std::size_t activeCount(Ns now) const;    // items not yet expired at `now`
    // Render path: no allocation, no locks. Composites onto the canvas.
    void draw(PixelBuffer& canvas, Ns now) const;

private:
    struct Slot { bool used = false; TextItem item; Ns start = 0; };
    std::array<Slot, Limits::kMaxActiveText> slots_{};
    static bool expired(const Slot& s, Ns now);
};

}  // namespace ss::compose
