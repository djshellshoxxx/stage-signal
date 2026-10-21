// Layer compositor (MOD-06 compositing half). Composition order is fixed (spec section 13):
//   background -> scene/media layer(s) -> camera layer -> published text -> approved overlays -> master
//   brightness / visual blackout, then rotation into the physical output frame.
// Everything up to the camera layer is drawn at the adaptive INTERNAL resolution; the result is upscaled
// (nearest/bilinear) to the logical output size, and text/overlays/brightness are drawn at full output resolution.
// Threading: compose() and every setter run on the render thread (or are externally serialised), except
// setVisualBlackout()/visualBlackout() (any thread, lock-free) and FramePool release (present thread).
#pragma once
#include <atomic>
#include <cstdint>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/compose/camera.hpp"
#include "ss/compose/frame_pool.hpp"
#include "ss/compose/media_timing.hpp"
#include "ss/compose/overlay.hpp"
#include "ss/compose/raster.hpp"
#include "ss/compose/text_layer.hpp"
#include "ss/compose/transition.hpp"

namespace ss::compose {

struct IdleConfig {
    Rgb color{16, 16, 24};
    bool showLogo = true;                              // placeholder logo (outlined box + label) when no bitmap
    std::shared_ptr<const PixelBuffer> logo;           // optional approved branding bitmap (premultiplied)
    std::array<char, 24> label{{'S', 'T', 'A', 'G', 'E', 'S', 'I', 'G', 'N', 'A', 'L', 0}};
};

struct CompositorConfig {
    Rgb background{0, 0, 0};
    FitMode sceneFit = FitMode::Fit;                   // REQ-D04 aspect handling for scene + split pane
    Sampling sceneSampling = Sampling::Bilinear;
    Sampling upscaleSampling = Sampling::Bilinear;     // internal -> output
    OutputPolicy policy;
    IdleConfig idle;
};

struct SceneInput {
    const PixelBuffer* a = nullptr;                    // current scene (any size; fitted into the canvas)
    const PixelBuffer* b = nullptr;                    // incoming scene during a transition
    ss::model::TransitionKind kind = ss::model::TransitionKind::Crossfade;
    float progress = 0.f;                              // 0 = A, 1 = B
};
struct MediaLayerInput {
    const PixelBuffer* frame = nullptr;                // already decoded still / GIF frame / video frame
    FitMode fit = FitMode::Fit;
    RectF region{0.f, 0.f, 1.f, 1.f};                  // normalised destination
    RectF crop{0.f, 0.f, 1.f, 1.f};                    // normalised source crop
    float opacity = 1.f;                               // 0..1
};
struct FrameInputs {
    ss::Ns nowNs = 0;                                  // monotonic/media time for text and camera state
    SceneInput scene;
    std::array<MediaLayerInput, Limits::kMaxMediaLayers> media{};
    int mediaCount = 0;
    CameraLayer* camera = nullptr;                     // composited only if published and visible
    const TextLayer* text = nullptr;
    const OverlayInfo* overlay = nullptr;
    float masterBrightness = 1.f;                      // 0..1
    bool idle = false;                                 // audio-loss policy: idle screen replaces scene/media/camera
};

struct ComposedFrame {
    int slot = -1;                                     // -1 = pool exhausted, nothing produced
    const PixelBuffer* pixels = nullptr;               // output-sized, opaque, premultiplied RGBA8
    bool blackout = false;
    std::uint64_t sequence = 0;
    bool ok() const { return pixels != nullptr; }
};

class Compositor {
public:
    // outW/outH = physical viewport. internalScale in [0.25, 1]. Allocates everything once.
    ss::Status prepare(std::uint32_t outW, std::uint32_t outH, float internalScale = 1.0f, std::size_t poolFrames = 3,
                       int rotationDeg = 0);
    void setConfig(const CompositorConfig& c);
    const CompositorConfig& config() const { return cfg_; }

    // REQ-D07: rotation / internal scale change recompute the composition from normalised coordinates; show
    // content is untouched and nothing is allocated.
    ss::Status setRotation(int rotationDeg);
    ss::Status setInternalScale(float scale);
    // Adaptive quality ladder (section 7): 0 full; 1 sheds camera trails; 2..4 lower scale to 75/50/35 %.
    void setQualityStep(int step);
    int qualityStep() const { return step_; }

    // REQ-D08: control path flips the flag; the next compose() call (next admitted frame) honours it. Scene,
    // text, camera and trail state keep advancing, so clearing the flag restores the exact prior composition.
    void setVisualBlackout(bool on) { blackout_.store(on, std::memory_order_release); }
    bool visualBlackout() const { return blackout_.load(std::memory_order_acquire); }

    ComposedFrame compose(const FrameInputs& in);
    void release(const ComposedFrame& f) { pool_.release(f.slot); }

    std::uint32_t outputWidth() const { return outW_; }
    std::uint32_t outputHeight() const { return outH_; }
    std::uint32_t logicalWidth() const { return lw_; }
    std::uint32_t logicalHeight() const { return lh_; }
    std::uint32_t internalWidth() const { return iw_; }
    std::uint32_t internalHeight() const { return ih_; }
    int rotation() const { return rot_; }
    float internalScale() const { return scale_; }
    std::uint64_t droppedFrames() const { return dropped_; }   // pool exhausted
    std::size_t freeFrames() const { return pool_.freeCount(); }

private:
    void layoutBuffers();
    void drawIdle(PixelBuffer& c) const;
    void drawScene(PixelBuffer& c, const PixelBuffer* scene, Rect region) const;
    void drawMedia(PixelBuffer& c, const FrameInputs& in) const;
    void drawCamera(PixelBuffer& c, const CameraLayer& cam, CameraState st, Rect cameraRect) const;

    CompositorConfig cfg_;
    FramePool pool_;
    PixelBuffer stage_, scratch_, logical_;
    std::atomic<bool> blackout_{false};
    std::uint32_t outW_ = 0, outH_ = 0, lw_ = 0, lh_ = 0, iw_ = 0, ih_ = 0;
    int rot_ = 0, step_ = 0;
    float scale_ = 1.f, userScale_ = 1.f;
    bool trailShed_ = false, prepared_ = false;
    std::uint64_t seq_ = 0, dropped_ = 0;
};

// Quality ladder helper: scale for a step (0..4).
float scaleForQualityStep(int step);

}  // namespace ss::compose
