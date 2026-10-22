// Broadcast driver around ss::broadcast::BroadcastSession (broadcast.md). Owner: engine-media-record. WORKER THREAD ONLY.
// Two-step start: stage() builds the publication summary for the confirmation dialog; confirmAndStart() needs a user
// action source (Ui/Keyboard/Midi) and turns it into the single-use start token. Nothing starts by itself.
#pragma once
#include <memory>
#include <string>
#include "App/Media/MediaTypes.h"
#include "App/Views/BroadcastView.h"
#include "Engine/ISecretStoreAdapter.h"
#include "Engine/IProcessSupervisor.h"
#include "ss/base/rng.hpp"
#include "ss/broadcast/session.hpp"
#include "ss/record/fanout.hpp"

namespace ss::app {

class BroadcastRunner {
public:
    BroadcastRunner(const ss::IClock& clock, IProcessSupervisor& procs, ISecretStoreAdapter* secrets);
    ~BroadcastRunner();

    ss::Status stage(const ss::broadcast::BroadcastSettings& s, const ss::broadcast::PublicationFacts& facts,
                     std::uint32_t audioRate);
    void cancelStage();
    // probe must describe a usable H.264 ffmpeg; src is the origin of the confirming action.
    ss::Status confirmAndStart(ss::ActionSource src, const FfmpegProbe& probe);
    void stop();
    void onAudio(const AudioChunk& c);
    void onVideo(const ss::PixelBuffer& frame);
    void tick();

    bool staged() const { return session_ && !started_; }
    bool active() const;                                // from start until Off/Error
    bool wantsVideo() const { return active() && videoOn_; }
    std::uint32_t fps() const { return fps_; }
    BroadcastView view() const;

private:
    struct Feed;
    struct Factory;
    void pump();

    const ss::IClock& clock_;
    IProcessSupervisor& procs_;
    ISecretStoreAdapter* secrets_;
    ss::Rng rng_;
    std::unique_ptr<Feed> feed_;
    std::unique_ptr<Factory> factory_;
    std::unique_ptr<ss::broadcast::ISecretResolver> resolver_;
    std::unique_ptr<ss::broadcast::BroadcastSession> session_;
    ss::broadcast::BroadcastSettings settings_;
    ss::broadcast::PublicationFacts facts_;
    std::string summary_, note_, encryption_;
    bool started_ = false, videoOn_ = false, stopRequested_ = false;
    std::uint32_t fps_ = 30, w_ = 0, h_ = 0, ach_ = 2;
    std::uint64_t pumpDrops_ = 0;
    ss::Ns stagedAt_ = 0, lastTick_ = 0;
    std::vector<std::uint8_t> vbuf_;
    std::vector<float> abuf_;
    std::string key_;
};

}  // namespace ss::app
