#include "App/MediaWorker.h"
#include <algorithm>
#include <chrono>
#include "ss/base/actions.hpp"

namespace ss::app {

namespace {
bool isUser(ss::ActionSource s) { return s == ss::ActionSource::Ui || s == ss::ActionSource::Keyboard || s == ss::ActionSource::Midi; }
ss::ActionResult reject(const std::string& why) { ss::ActionResult r; r.accepted = false; r.reason = why; return r; }
ss::ActionResult accept() { ss::ActionResult r; r.accepted = true; return r; }
constexpr std::size_t kAudioQueueMax = 400;     // ~4 s of 512-frame blocks at 48 kHz
}  // namespace

ss::Status MediaWorker::start() {
    if (started_.exchange(true)) return ss::okStatus();
    if (adapters_.processes) adapters_.processes->start();
    quit_.store(false);
    record_ = std::make_unique<RecordRunner>(*adapters_.clock, *adapters_.processes);
    broadcast_ = std::make_unique<BroadcastRunner>(*adapters_.clock, *adapters_.processes, adapters_.secrets.get());
    thread_ = std::thread([this] { run(); });
    return ss::okStatus();
}

void MediaWorker::stop() {
    std::lock_guard<std::mutex> g(stopMutex_);
    unpublishCamera();
    camera_.close();
    if (started_.load()) {
        quit_.store(true);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        started_.store(false);
    }
}

void MediaWorker::refreshCameras() {
    auto devices = camera_.devices();
    std::lock_guard<std::mutex> lock(viewMutex_);
    mediaView_.cameraDevices = std::move(devices);
}

bool MediaWorker::unpublishCamera() { camera_.unpublish(); return true; }

void MediaWorker::configure(const MediaConfig& cfg) { std::lock_guard<std::mutex> l(cfgMutex_); cfg_ = cfg; }

ss::Status MediaWorker::importImage(std::size_t slot, const std::string& path, std::string* note) {
    return layers_.setImage(slot, path, ss::media::Tier::Standard, note);
}
ss::Status MediaWorker::setTextBanner(const std::string& utf8, std::uint32_t w, std::uint32_t h) { return layers_.setText(utf8, w, h); }

bool MediaWorker::wantsAudio() const { return wantAudio_.load(std::memory_order_relaxed); }

void MediaWorker::onProcessedBlock(const AudioBlockView& p, const AudioBlockView&) {
    tapBlocks_.fetch_add(1, std::memory_order_relaxed);
    if (!p.interleaved || p.frames == 0 || p.channels == 0) return;
    lastRate_.store(p.rateHz, std::memory_order_relaxed);
    lastCh_.store(p.channels, std::memory_order_relaxed);
    lastBlockNs_.store(adapters_.clock ? adapters_.clock->nowMonotonic() : 0, std::memory_order_relaxed);
    if (!wantsAudio()) return;
    std::lock_guard<std::mutex> l(audioMutex_);
    if (audioQ_.size() >= kAudioQueueMax) { ++audioOverflow_; return; }
    AudioChunk c;
    if (!audioFree_.empty()) { c = std::move(audioFree_.back()); audioFree_.pop_back(); }
    c.s.assign(p.interleaved, p.interleaved + static_cast<std::size_t>(p.frames) * p.channels);
    c.frames = p.frames; c.channels = p.channels; c.rateHz = p.rateHz; c.flags = p.flags;
    c.firstSampleIndex = p.firstSampleIndex; c.captureNs = p.captureNs; c.generation = p.generation;
    audioQ_.push_back(std::move(c));
}

void MediaWorker::push(Cmd c) {
    { std::lock_guard<std::mutex> l(cmdMutex_); cmds_.push_back(c); }
    cv_.notify_all();
}

ss::ActionResult MediaWorker::handleAction(const ss::Action& a) {
    if (!started_.load()) return reject("Media is not running");
    const auto id = static_cast<ss::ActionId>(a.actionId);
    if (id == ss::ActionId::StartRecording) {
        if (!isUser(a.source)) return reject("Recording starts only from an operator action");
        { std::lock_guard<std::mutex> l(viewMutex_);
          if (recPending_.load() || (recView_.state != RecordUiState::Off && recView_.state != RecordUiState::Failed && recView_.state != RecordUiState::Recoverable))
              return reject("Recording is already running"); }
        recPending_.store(true);
        wantAudio_.store(true);
        push({Cmd::StartRec, a.source});
        return accept();
    }
    if (id == ss::ActionId::StopRecording) { push({Cmd::StopRec, a.source}); return accept(); }
    if (id == ss::ActionId::AddMarker) { push({Cmd::Marker, a.source}); return accept(); }
    if (id == ss::ActionId::StartBroadcast) {
        if (!isUser(a.source)) return reject("Broadcast starts only from an operator action");
        push({a.value >= 0.5 ? Cmd::ConfirmBc : Cmd::StageBc, a.source});
        return accept();
    }
    if (id == ss::ActionId::StopBroadcast) { push({Cmd::StopBc, a.source}); return accept(); }
    return reject("Not a media action");
}

const FfmpegProbe& MediaWorker::probe(bool force) {
    std::string path;
    { std::lock_guard<std::mutex> l(cfgMutex_); path = cfg_.ffmpegPath; }
    if (force || !probed_ || path != probedPath_) {
        probe_ = adapters_.processes ? adapters_.processes->probeFfmpeg(path) : FfmpegProbe{};
        if (!probe_.found && probe_.reason.empty()) probe_.reason = "ffmpeg was not found";
        probedPath_ = path; probed_ = true;
        std::lock_guard<std::mutex> l(viewMutex_);
        mediaView_.ffmpegFound = probe_.found;
        mediaView_.ffmpegNote = probe_.found ? probe_.versionLine : probe_.reason;
    }
    return probe_;
}

void MediaWorker::doCommand(const Cmd& c) {
    MediaConfig cfg;
    { std::lock_guard<std::mutex> l(cfgMutex_); cfg = cfg_; }
    const FrameProvider& fp = frames_;
    auto frame = [&]() -> std::shared_ptr<const SharedFrame> { return fp ? fp(cfg.programOutput) : nullptr; };
    switch (c.kind) {
        case Cmd::Rescan: probe(true); break;
        case Cmd::StartRec: {
            const FfmpegProbe* pr = cfg.recordVideo ? &probe(false) : nullptr;
            auto f = frame();
            const bool fresh = (adapters_.clock->nowMonotonic() - lastBlockNs_.load()) < ss::secToNs(2);
            record_->begin(cfg, pr, fresh ? lastRate_.load() : 0, fresh ? lastCh_.load() : 0, f ? &f->pixels : nullptr);
            recPending_.store(false);
            nextRecFrame_ = adapters_.clock->nowMonotonic();
            break;
        }
        case Cmd::StopRec: record_->requestStop(ss::record::StopReason::User); break;
        case Cmd::Marker: record_->addMarker(); break;
        case Cmd::StageBc: {
            ss::broadcast::PublicationFacts facts;
            facts.cameraPublished = camera_.state() == CameraState::Published;
            facts.audioSourceLabel = "Master bus (after compressor/limiter)";
            facts.programOutputLabel = "Output " + std::to_string(cfg.programOutput);
            facts.recordingActive = record_->active();
            const bool fresh = (adapters_.clock->nowMonotonic() - lastBlockNs_.load()) < ss::secToNs(2);
            auto st = broadcast_->stage(cfg.broadcast, facts, fresh ? lastRate_.load() : 0);
            bcFail_ = st.ok() ? std::string() : st.error().detail;
            break;
        }
        case Cmd::ConfirmBc: {
            const FfmpegProbe& pr = probe(false);
            wantAudio_.store(true);
            auto st = broadcast_->confirmAndStart(c.src, pr);
            nextBcFrame_ = adapters_.clock->nowMonotonic();
            bcFail_ = st.ok() ? std::string() : st.error().detail;
            break;
        }
        case Cmd::StopBc: broadcast_->stop(); break;
    }
}

void MediaWorker::drainAudio() {
    for (;;) {
        AudioChunk c;
        {
            std::lock_guard<std::mutex> l(audioMutex_);
            if (audioQ_.empty()) break;
            c = std::move(audioQ_.front());
            audioQ_.pop_front();
        }
        record_->onAudio(c);
        broadcast_->onAudio(c);
        std::lock_guard<std::mutex> l(audioMutex_);
        if (audioFree_.size() < 32) audioFree_.push_back(std::move(c));
    }
}

void MediaWorker::feedVideo() {
    const bool rv = record_->wantsVideo(), bv = broadcast_->wantsVideo();
    if (!rv && !bv) return;
    MediaConfig cfg;
    { std::lock_guard<std::mutex> l(cfgMutex_); cfg = cfg_; }
    const ss::Ns now = adapters_.clock->nowMonotonic();
    if (!frames_) return;
    auto emit = [&](bool want, ss::Ns& next, std::uint32_t fps, auto&& sink) {
        if (!want || now < next) return;
        const ss::Ns period = ss::kNsPerSec / std::max<std::uint32_t>(1, fps);
        next = (now - next > 2 * period) ? now + period : next + period;
        if (auto f = frames_(cfg.programOutput)) { lastFrame_ = f; sink(f->pixels); }
        else if (lastFrame_) sink(lastFrame_->pixels);
    };
    emit(rv, nextRecFrame_, record_->fps(), [&](const ss::PixelBuffer& p) { record_->onVideo(p); });
    emit(bv, nextBcFrame_, broadcast_->fps(), [&](const ss::PixelBuffer& p) { broadcast_->onVideo(p); });
}

void MediaWorker::publish() {
    RecordView r = record_->view();
    if (recPending_.load() && r.state == RecordUiState::Off) { r.state = RecordUiState::Preparing; r.note = "Checking ffmpeg and the recordings folder"; }
    BroadcastView b = broadcast_->view();
    if (b.state == BroadcastUiState::Off && !b.awaitingConfirm && !bcFail_.empty()) { b.state = BroadcastUiState::Failed; b.note = bcFail_; }
    std::lock_guard<std::mutex> l(viewMutex_);
    recView_ = r;
    bcView_ = b;
    mediaView_.workerAvailable = true;
    mediaView_.loadedItems = layers_.loaded();
    mediaView_.cameraPreview = camera_.preview();
    if (adapters_.camera) mediaView_.cameraState = static_cast<unsigned char>(camera_.state());
    mediaView_.note = layers_.lastNote();
}

void MediaWorker::run() {
    refreshCameras();
    probe(false);
    publish();
    while (!quit_.load()) {
        for (;;) {
            Cmd c;
            { std::lock_guard<std::mutex> l(cmdMutex_); if (cmds_.empty()) break; c = cmds_.front(); cmds_.pop_front(); }
            doCommand(c);
        }
        camera_.poll(adapters_.clock->nowMonotonic());
        drainAudio();
        feedVideo();
        record_->tick();
        broadcast_->tick();
        wantAudio_.store(record_->active() || broadcast_->active() || recPending_.load() || broadcast_->staged(), std::memory_order_relaxed);
        publish();
        std::unique_lock<std::mutex> l(cmdMutex_);
        cv_.wait_for(l, std::chrono::milliseconds(4), [&] { return quit_.load() || !cmds_.empty(); });
    }
    // Shutdown: finalise within the stage budget (graceful 5 s, then kill).
    broadcast_->stop();
    record_->requestStop(ss::record::StopReason::Shutdown);
    const auto t0 = std::chrono::steady_clock::now();
    while ((record_->active() || broadcast_->active()) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        drainAudio();
        record_->tick();
        broadcast_->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    publish();
}

void MediaWorker::fillView(MediaView& m, RecordView& r, BroadcastView& b) const {
    std::lock_guard<std::mutex> l(viewMutex_);
    m = mediaView_;
    r = recView_;
    b = bcView_;
    if (!started_.load()) { m.workerAvailable = false; m.note = "Media worker is not running"; }
    if (m.cameraState == 0 && adapters_.camera && adapters_.camera->cameraState() == CameraState::Unsupported)
        m.cameraState = static_cast<unsigned char>(CameraState::Unsupported);
}

}  // namespace ss::app
