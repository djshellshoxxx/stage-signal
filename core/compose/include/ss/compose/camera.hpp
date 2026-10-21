// Camera layer (REQ-S09/S10/S11). Pixel buffers are handed in by the app; no device access here.
//  * Treatments are applied once per pushed camera frame, ONLY inside this layer (so they can affect nothing
//    but the routed camera layer). Order: mirror -> posterize -> threshold -> tint -> motion trail.
//  * Publication is an explicit flag: a layer that is not published is never composited (preview != publish);
//    the processed frame stays readable via preview() for the operator UI.
//  * Loss (REQ-S11): freeze last processed frame for <= 500 ms from the moment loss is established, then hide the
//    layer or show a placeholder. The layer is bound to ONE source id; frames from another id are rejected and
//    counted. There is deliberately no API to select an alternate camera.
#pragma once
#include <cstdint>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/compose/pixel.hpp"

namespace ss::compose {

struct CameraTreatments {
    bool mirror = false;                 // horizontal flip
    bool posterizeOn = false; int posterizeLevels = 4;   // 2..16
    bool thresholdOn = false; float thresholdCutoff = 0.5f;  // luminance cutoff 0..1
    bool tintOn = false; Rgb tintColor{255, 255, 255}; float tintAmount = 0.5f;  // blend toward tintColor, 0..1
    int trailFrames = 0;                 // 0 = off, else 2..8 (accumulation decay length)
};
CameraTreatments sanitize(CameraTreatments t);   // clamps every field into its documented range

enum class CameraPlacement : std::uint8_t { Full, Pip, Split };
struct SplitConfig {
    bool vertical = true;        // true: side by side (camera left/right); false: stacked (camera top/bottom)
    bool cameraFirst = true;     // camera on the left / top
    float ratio = 0.5f;          // camera share of the canvas, 0.1..0.9
    FitMode fit = FitMode::Crop; // how the camera fills its pane
};
enum class LossPolicy : std::uint8_t { Hide, Placeholder };
enum class CameraState : std::uint8_t {
    NoSignal,      // no frame ever received (nothing to show; layer hidden)
    Live,          // frames arriving
    Frozen,        // lost <= freeze window: last processed frame held
    Hidden,        // lost past the window, policy Hide
    Placeholder    // lost past the window, policy Placeholder
};

struct CameraLayerConfig {
    std::uint32_t sourceId = 0;            // the routed camera; never changed automatically
    bool published = false;
    CameraPlacement placement = CameraPlacement::Full;
    FitMode fit = FitMode::Fit;            // Full and Pip
    RectF pip{0.68f, 0.68f, 0.30f, 0.30f}; // normalised canvas rectangle
    SplitConfig split;
    CameraTreatments treatments;
    LossPolicy lossPolicy = LossPolicy::Hide;
    Rgb placeholderColor{32, 32, 32};
    std::int64_t freezeMs = 500;           // clamped to 0..500
    std::int64_t staleMs = 250;            // no frame for this long => loss established (clamped 50..2000)
};

class CameraLayer {
public:
    CameraLayer() = default;
    CameraLayer(const CameraLayer&) = delete;            // buffers are reserved once; copying would drop that
    CameraLayer& operator=(const CameraLayer&) = delete;
    CameraLayer(CameraLayer&&) = default;
    CameraLayer& operator=(CameraLayer&&) = default;
    // Allocates the three working buffers once (max camera frame size). Control path.
    ss::Status prepare(std::uint32_t maxW, std::uint32_t maxH);
    void configure(const CameraLayerConfig& cfg);   // no allocation
    const CameraLayerConfig& config() const { return cfg_; }
    void setPublished(bool p) { cfg_.published = p; }
    void setTrailShed(bool shed);                   // adaptive quality: trails are the first thing shed

    // Camera frame arrival. Returns false (frame ignored, counter bumped) for a foreign source id, an empty frame,
    // or a frame larger than prepare(); never allocates.
    bool pushFrame(std::uint32_t sourceId, const PixelBuffer& frame, ss::Ns now);
    void markLost(ss::Ns now);                       // device-lost event from the app
    CameraState update(ss::Ns now);                  // evaluates the loss state machine at `now`
    CameraState state() const { return state_; }

    bool visible() const { return cfg_.published && (state_ == CameraState::Live || state_ == CameraState::Frozen || state_ == CameraState::Placeholder); }
    const PixelBuffer* frame() const { return hasFrame_ ? &processed_ : nullptr; }  // processed (treated) frame
    const PixelBuffer* preview() const { return frame(); }   // operator preview works regardless of publication
    std::uint64_t rejectedFrames() const { return rejected_; }
    std::uint64_t acceptedFrames() const { return accepted_; }

private:
    void process(const PixelBuffer& in);
    CameraLayerConfig cfg_;
    PixelBuffer processed_, acc_;
    bool hasFrame_ = false, lost_ = false, trailShed_ = false, accValid_ = false;
    ss::Ns lastFrame_ = 0, lossStart_ = 0;
    CameraState state_ = CameraState::NoSignal;
    std::uint64_t rejected_ = 0, accepted_ = 0;
    std::uint32_t capW_ = 0, capH_ = 0;
};

// Pure per-pixel treatment steps (exposed for tests and for the app's preview thumbnail path).
void mirrorHorizontal(PixelBuffer& b);
void posterize(PixelBuffer& b, int levels);
void threshold(PixelBuffer& b, float cutoff01);
void tint(PixelBuffer& b, Rgb colour, float amount01);

}  // namespace ss::compose
