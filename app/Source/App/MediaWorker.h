// Contract: MEDIA worker host (app-shell 4.1 MEDIA row, step 12). Owner: engine-media-record.
// STUB in the contract commit: it exists so the controller graph, start/stop order, UiSnapshot fields and the
// ownership map are real. It decodes nothing, records nothing, broadcasts nothing, and reports that honestly.
// engine-media-record replaces the bodies (media::MediaSupervisor, camera mailbox, record::RecordSession,
// broadcast::BroadcastSession, ffmpeg supervision) in MediaWorker.cpp and may add files under App/Media/.
// Rules that stay true: nothing starts at startup, stop() finalises and disarms within the shutdown timeouts,
// and a fault here never stops UI, audio ingress or the show engine (APP-SH-12).
#pragma once
#include "App/AnalysisWorker.h"
#include "App/RenderHost.h"
#include "App/Views/BroadcastView.h"
#include "App/Views/MediaView.h"
#include "App/Views/RecordView.h"
#include "Engine/AdapterSet.h"

namespace ss::app {

class MediaWorker final : public IProcessedAudioConsumer, public IRenderLayerSource {
public:
    explicit MediaWorker(AdapterSet& adapters) : adapters_(adapters) {}

    ss::Status start();       // step 12: idle; camera stays closed, publication off
    void stop();              // shutdown stages 1, 2, 4(camera), 6: unpublish, stop broadcast, finalise recorder
    bool unpublishCamera();   // stage 1 helper

    // IProcessedAudioConsumer (ANALYSIS thread): recorder/broadcast tap. STUB: counts blocks only.
    void onProcessedBlock(const AudioBlockView& processed, const AudioBlockView& raw) override;
    // IRenderLayerSource (RENDER thread): media/camera/text layers. STUB: adds none.
    void fillLayers(OutputId, ss::Ns, ss::compose::FrameInputs&) override {}

    // CONTROL thread: StartRecording/StopRecording/StartBroadcast/StopBroadcast/AddMarker routed here.
    ss::ActionResult handleAction(const ss::Action& a);
    void fillView(MediaView& m, RecordView& r, BroadcastView& b) const;
    std::uint64_t tapBlocks() const { return tapBlocks_.load(std::memory_order_relaxed); }

private:
    AdapterSet& adapters_;
    std::atomic<std::uint64_t> tapBlocks_{0};
    std::atomic<bool> started_{false};
};

}  // namespace ss::app
