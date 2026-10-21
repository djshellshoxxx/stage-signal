// MOD-06 procedural scenes (spec section 13 default pack). Contract used by compose and the app.
#pragma once
#include <cstdint>
#include <memory>
#include "ss/base/types.hpp"
#include "ss/model/show_model.hpp"

namespace ss::render {

struct AudioVisualInput {
    float spectrum[32] = {};    // 0..1, log-spaced bins, low to high; filled by the app from ss::audio
    float waveform[256] = {};   // -1..1 samples, one analysis window
};

struct SceneInput {
    Ns timeNs = 0;                              // elapsed show/scene time; ALL animation derives from this
    double dtSec = 0.0;                         // informational (scenes never integrate it)
    const ss::FeatureSnapshot* features = nullptr;   // may be null: bands/onset treated as 0
    const AudioVisualInput* audio = nullptr;         // may be null: a gentle time-based idle animation is shown
    const model::SceneParams* params = nullptr;      // may be null: defaults
    std::uint64_t seed = 0;                     // 0 or equal to the prepared seed: no effect; else state is reseeded
    bool strobingDisabled = false;              // REQ-S12: scenes that flash must render the non-flashing fallback
};

class SceneRenderer {
public:
    virtual ~SceneRenderer();
    // Allocates every buffer the scene will ever need. Not real-time safe. Output frames must be w x h.
    virtual void prepare(std::uint32_t w, std::uint32_t h, std::uint64_t seed) = 0;
    // Real-time safe: no allocation, no locks, no I/O. `out` must be prepared size (else it is cleared to black).
    // Output is an opaque (alpha 255) premultiplied RGBA8 sRGB frame.
    virtual void render(const SceneInput&, ss::PixelBuffer& out) = 0;
    virtual const char* name() const = 0;
    virtual float costEstimate() const = 0;   // relative CPU cost per pixel, ColorField = 1.0
    virtual bool usesStrobe() const = 0;      // true only if the scene can produce full-frame flashes
};

// Factory for the 12 procedural kinds (SceneKind::SpectrumBars .. LissajousScope). Image/Gif/Video/Count -> nullptr.
std::unique_ptr<SceneRenderer> makeScene(model::SceneKind kind);

}  // namespace ss::render
