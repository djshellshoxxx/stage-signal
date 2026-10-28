// MEDIA worker host (app-shell 4.1 MEDIA row, step 12). Owner: engine-media-record.
// One worker thread owns recording, broadcast, ffmpeg probing and the frame/audio feeds, so the CONTROL, ANALYSIS and
// RENDER threads never block on a child process or the disk. Start/Stop actions are queued to it; results appear in
// fillView(). Rules that stay true: nothing starts at startup, stop() finalises and disarms within the shutdown
// timeouts, and a fault here never stops UI, audio ingress or the show engine (APP-SH-12).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "App/AnalysisWorker.h"
#include "App/Media/BroadcastRunner.h"
#include "App/Media/CameraFeed.h"
#include "Engine/Media/ClipDecoder.h"
#include "App/Media/MediaLayers.h"
#include "App/Media/MediaTypes.h"
#include "App/Media/RecordRunner.h"
#include "App/RenderHost.h"
#include "App/Views/BroadcastView.h"
#include "App/Views/MediaView.h"
#include "App/Views/RecordView.h"
#include "Engine/AdapterSet.h"

namespace ss::app {

class MediaWorker final : public IProcessedAudioConsumer, public IRenderLayerSource {
public:
    using FrameProvider = std::function<std::shared_ptr<const SharedFrame>(ss::OutputId)>;

    explicit MediaWorker(AdapterSet& adapters) : adapters_(adapters), camera_(adapters.camera) {}
    ~MediaWorker() override { stop(); }

    ss::Status start();       // step 12: starts the worker thread; idle, camera closed, nothing published
    void stop();              // shutdown stages 1, 2, 4(camera), 6: unpublish, stop broadcast, finalise recorder
    void refreshCameras();
    CameraFeed& camera() { return camera_; }
    bool unpublishCamera();   // stage 1 helper

    // Any thread. Settings used by the NEXT session; a running session keeps the settings it started with.
    void configure(const MediaConfig& cfg);
    // Before start(): source of the rendered program frames (e.g. [this](OutputId id){ return render_.latestFrame(id); }).
    void setFrameProvider(FrameProvider p) { frames_ = std::move(p); }

    // Media import (any non-RT thread). Stills only; GIF shows its first frame. Slot 0..2.
    ss::Status importImage(std::size_t slot, const std::string& path, std::string* note = nullptr);
    ss::Status setTextBanner(const std::string& utf8, std::uint32_t outW = 1280, std::uint32_t outH = 720);
    ss::Status setMediaPlayback(std::size_t slot, double speed, bool loop);
    ss::Status pauseMedia(std::size_t slot, bool paused);
    void removeMedia(std::size_t slot);
    void clearMedia();
    MediaLayers& layers() { return layers_; }

    // IProcessedAudioConsumer (ANALYSIS thread): copies into a bounded queue only while a session needs audio.
    void onProcessedBlock(const AudioBlockView& processed, const AudioBlockView& raw) override;
    // IRenderLayerSource (RENDER thread): still and text layers.
    void fillLayers(OutputId o, ss::Ns now, ss::compose::FrameInputs& in) override { layers_.fillLayers(o, now, in); camera_.fillLayers(o, now, in); }

    // CONTROL thread: StartRecording/StopRecording/StartBroadcast/StopBroadcast/AddMarker.
    //   StartRecording / StartBroadcast need a user source (Ui, Keyboard, Midi).
    //   StartBroadcast value < 0.5 asks for the confirmation summary (BroadcastView::confirmText); the same action with
    //   value >= 0.5 within 15 s confirms it. Stop actions are always accepted.
    ss::ActionResult handleAction(const ss::Action& a);
    void fillView(MediaView& m, RecordView& r, BroadcastView& b) const;
    std::uint64_t tapBlocks() const { return tapBlocks_.load(std::memory_order_relaxed); }

private:
    struct Cmd { enum Kind { StartRec, StopRec, StageBc, ConfirmBc, StopBc, Marker, Rescan } kind; ss::ActionSource src; };
    void run();
    void push(Cmd c);
    void doCommand(const Cmd& c);
    const FfmpegProbe& probe(bool force);
    void drainAudio();
    void feedVideo();
    void publish();
    bool wantsAudio() const;

    AdapterSet& adapters_;
    FrameProvider frames_;
    MediaLayers layers_;
    CameraFeed camera_;
    std::mutex clipsMutex_;
    std::array<std::unique_ptr<ClipDecoder>, 3> clips_;
    std::array<std::shared_ptr<const PixelBuffer>, 3> clipFrames_;
    std::array<std::string, 3> clipPaths_;
    std::array<FfmpegProbe, 3> clipFfmpeg_;
    std::atomic<std::uint64_t> tapBlocks_{0};
    std::atomic<bool> started_{false}, quit_{false}, wantAudio_{false}, recPending_{false};
    std::atomic<std::uint32_t> lastRate_{0}, lastCh_{0};
    std::atomic<std::int64_t> lastBlockNs_{0};
    std::thread thread_;
    std::mutex stopMutex_;

    mutable std::mutex cfgMutex_;
    MediaConfig cfg_;

    std::mutex cmdMutex_;
    std::condition_variable cv_;
    std::deque<Cmd> cmds_;

    std::mutex audioMutex_;
    std::deque<AudioChunk> audioQ_, audioFree_;
    std::uint64_t audioOverflow_ = 0;

    mutable std::mutex viewMutex_;
    MediaView mediaView_;
    RecordView recView_;
    BroadcastView bcView_;

    // Worker-thread state.
    std::unique_ptr<RecordRunner> record_;
    std::unique_ptr<BroadcastRunner> broadcast_;
    FfmpegProbe probe_;
    std::string probedPath_;
    bool probed_ = false;
    std::string bcFail_;
    ss::Ns nextRecFrame_ = 0, nextBcFrame_ = 0;
    std::uint64_t lastFrameSeq_ = 0;
    std::shared_ptr<const SharedFrame> lastFrame_;
};

}  // namespace ss::app
