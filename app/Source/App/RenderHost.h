// Contract: RENDER thread body (app-shell 4.1, step 6). Owner: shell-integration (engine-media-record adds media
// layers through the hooks below, ui-live consumes the per-output FrameExchange through UiModel::latestFrame).
// Renders scenes (render core) and composes them (compose core) once per output, then publishes the RGBA frame to
// that output's FrameExchange. Independent compositor per output in this commit: duplicate-sharing is an
// optimisation left to a later PR. renderFrame() is thread-free so tests can drive it.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "App/Mailboxes.h"
#include "ss/compose/compositor.hpp"
#include "ss/model/show_model.hpp"
#include "ss/render/effects.hpp"
#include "ss/render/scene.hpp"
#include "ss/show/types.hpp"

namespace ss::app {

// Scene id -> definition, published by the CONTROL thread whenever a show is (re)loaded.
struct SceneTable {
    std::map<std::uint32_t, model::SceneDef> scenes;
    std::vector<model::OutputDef> outputs;
};

struct OutputSpec { OutputId id = 0; std::uint32_t width = 800, height = 600; int rotationDeg = 0; };

struct RenderStats {
    std::uint64_t framesRendered = 0, framesDropped = 0;
    double fpsMeasured = 0.0, lastFrameMs = 0.0, worstFrameMs = 0.0;
};

// Optional hook for later workers: fill media/camera/text layers of a FrameInputs before compose().
// Called on the RENDER thread, must not block.
class IRenderLayerSource {
public:
    virtual ~IRenderLayerSource() = default;
    virtual void fillLayers(OutputId output, ss::Ns nowNs, ss::compose::FrameInputs& in) = 0;
};

class RenderHost {
public:
    RenderHost(const ss::IClock& clock, BlackoutLatch& latch,
               const LatestShared<ss::show::ShowSnapshot>& show, const LatestShared<SceneTable>& scenes,
               const ss::LatestMailbox<ss::FeatureSnapshot>& features,
               const ss::LatestMailbox<ss::render::AudioVisualInput>& visual);

    // Any thread. Applied by the render thread at the next frame boundary (allocates there, not per frame).
    void setOutputs(std::vector<OutputSpec> specs);
    void setLayerSource(IRenderLayerSource* s) { layers_ = s; }       // before the thread starts

    // RENDER thread. Renders and publishes one frame per output. Returns the number of frames published.
    std::size_t renderFrame();

    // Any thread.
    std::shared_ptr<const SharedFrame> latestFrame(OutputId id) const;
    RenderStats stats() const;
    double targetFps() const { return targetFps_.load(std::memory_order_relaxed); }
    void setTargetFps(double fps) { targetFps_.store(fps, std::memory_order_relaxed); }   // any thread (perf adaptation)
    // Optional palette tint from the key colour (FEAT-002): a gentle multiply on the scene frames, off by default.
    void setPaletteTint(bool on, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        tint_.store(on ? (0x01000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b) : 0u, std::memory_order_relaxed);
    }

private:
    struct OutputRender {
        OutputSpec spec;
        ss::compose::Compositor comp;
        FrameExchange exchange;
        std::unique_ptr<ss::render::SceneRenderer> sceneA, sceneB;
        std::uint32_t idA = ~0u, idB = ~0u;
        PixelBuffer frameA, frameB;
        ss::render::EffectScratch fx;
        Ns sceneStartNs = 0;
        std::uint64_t sequence = 0;
    };
    void applyStaged();
    void ensureScene(OutputRender& o, std::unique_ptr<ss::render::SceneRenderer>& r, std::uint32_t& cur,
                     std::uint32_t wantId, const SceneTable& table, PixelBuffer& frame);

    const ss::IClock& clock_;
    BlackoutLatch& latch_;
    const LatestShared<ss::show::ShowSnapshot>& show_;
    const LatestShared<SceneTable>& scenes_;
    const ss::LatestMailbox<ss::FeatureSnapshot>& features_;
    const ss::LatestMailbox<ss::render::AudioVisualInput>& visual_;
    IRenderLayerSource* layers_ = nullptr;
    std::atomic<double> targetFps_{25.0};
    std::atomic<std::uint32_t> tint_{0};

    std::mutex stageMutex_;
    std::vector<OutputSpec> staged_;
    bool haveStaged_ = false;

    mutable std::mutex mapMutex_;      // guards the output list for latestFrame() lookups
    std::vector<std::shared_ptr<OutputRender>> outputs_;
    Ns lastFrameNs_ = 0;
    std::atomic<std::uint64_t> rendered_{0}, dropped_{0};
    std::atomic<double> lastMs_{0.0}, worstMs_{0.0}, fps_{0.0};
};

}  // namespace ss::app
