#include "App/Media/BroadcastRunner.h"
#include <algorithm>
#include "ss/record/profile.hpp"
#include "ss/security/secret_store.hpp"

namespace ss::app {
namespace bc = ss::broadcast;

namespace {
// Broadcast handles are "ref:<id>", the secret store uses "secret:<label>".
std::string storeRef(const bc::SecretRef& r) {
    if (r.handle.compare(0, 4, "ref:") != 0) return {};
    return "secret:" + r.handle.substr(4);
}

class StoreResolver final : public bc::ISecretResolver {
public:
    explicit StoreResolver(ISecretStoreAdapter* s) : s_(s) {}
    bool exists(const bc::SecretRef& r) const override { return s_ && s_->exists(storeRef(r)); }
    ss::Result<std::string> resolve(const bc::SecretRef& r) const override {
        if (!s_) return ss::makeError(ss::ErrorCategory::PermissionDenied, "No secret store is available");
        auto v = s_->get(storeRef(r));
        if (!v.ok()) return ss::makeError(ss::ErrorCategory::PermissionDenied, "The stored stream key could not be read");
        return v;
    }
private:
    ISecretStoreAdapter* s_;
};

// Forwards to a shared inner process so the runner can keep writing to it while the session owns the proxy.
class ProxyProcess final : public bc::IProcess {
public:
    explicit ProxyProcess(std::shared_ptr<bc::IProcess> p) : p_(std::move(p)) {}
    ss::Status start(const std::vector<std::string>& a) override { return p_->start(a); }
    bool running() override { return p_->running(); }
    int exitCode() const override { return p_->exitCode(); }
    std::string readProgress() override { return p_->readProgress(); }
    std::string readStderr() override { return p_->readStderr(); }
    void requestStop() override { p_->requestStop(); }
    void kill() override { p_->kill(); }
private:
    std::shared_ptr<bc::IProcess> p_;
};

ss::broadcast::ActionSource mapSource(ss::ActionSource s) {
    switch (s) {
        case ss::ActionSource::Ui: return bc::ActionSource::Ui;
        case ss::ActionSource::Keyboard: return bc::ActionSource::Keyboard;
        case ss::ActionSource::Midi: return bc::ActionSource::Midi;
        case ss::ActionSource::Show: return bc::ActionSource::Show;
        case ss::ActionSource::Remote: return bc::ActionSource::Remote;
        case ss::ActionSource::System: return bc::ActionSource::System;
    }
    return bc::ActionSource::System;
}
}  // namespace

struct BroadcastRunner::Feed final : bc::IMediaFeed {
    ss::record::ConsumerQueue video{"bc-video", 1u << 20, ss::record::OverflowPolicy::DropOldest};
    ss::record::ConsumerQueue audio{"bc-audio", 2u << 20, ss::record::OverflowPolicy::DropOldest};
    bool on = false;
    std::uint64_t keyframes = 0, seq = 0;
    void setActive(bool v) override { on = v; if (!v) { video.clear(); audio.clear(); } }
    void abandonQueued() override { video.clear(); audio.clear(); }
    void requestKeyframe() override { ++keyframes; }
    std::uint64_t droppedChunks() const override { return video.stats().dropped + audio.stats().dropped; }
};

struct BroadcastRunner::Factory final : bc::IProcessFactory {
    explicit Factory(IProcessSupervisor& s) : sup(s) {}
    std::unique_ptr<bc::IProcess> create() override {
        std::shared_ptr<bc::IProcess> inner(sup.create().release());
        live = inner;
        return std::make_unique<ProxyProcess>(inner);
    }
    IProcessSupervisor& sup;
    std::weak_ptr<bc::IProcess> live;
};

BroadcastRunner::BroadcastRunner(const ss::IClock& clock, IProcessSupervisor& procs, ISecretStoreAdapter* secrets)
    : clock_(clock), procs_(procs), secrets_(secrets), rng_(static_cast<std::uint64_t>(clock.wallUnixNs()) | 1u),
      factory_(std::make_unique<Factory>(procs)) {}
BroadcastRunner::~BroadcastRunner() = default;

bool BroadcastRunner::active() const {
    if (!session_ || !started_) return false;
    const auto s = session_->state();
    return s != bc::State::Off && s != bc::State::Error;
}

ss::Status BroadcastRunner::stage(const bc::BroadcastSettings& in, const bc::PublicationFacts& facts, std::uint32_t audioRate) {
    if (active()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "A broadcast is already running");
    if (!procs_.auxSupported())
        return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "Broadcast is not available on this system in this build yet");
    if (audioRate == 0) return ss::makeError(ss::ErrorCategory::DeviceLost, "No audio is arriving. Choose an audio input in Setup first");
    bc::BroadcastSettings s = in;
    s.audio.rateHz = audioRate;
    s.audio.channels = 2;
    for (const auto& i : bc::validateSettings(s))
        if (i.blocking) return ss::makeError(ss::ErrorCategory::InvalidArgument, i.message);
    session_.reset();
    feed_ = std::make_unique<Feed>();
    feed_->video = ss::record::ConsumerQueue("bc-video", std::max<std::size_t>(1u << 20, static_cast<std::size_t>(s.video.width) * s.video.height * 4 * 3),
                                             ss::record::OverflowPolicy::DropOldest);
    resolver_ = std::make_unique<StoreResolver>(secrets_);
    settings_ = s; facts_ = facts;
    session_ = std::make_unique<bc::BroadcastSession>(s, clock_, rng_, *factory_, *feed_, *resolver_);
    summary_ = bc::makeSummary(s, facts).text();
    encryption_ = bc::encryptionLabel(s);
    stagedAt_ = clock_.nowMonotonic();
    started_ = false; stopRequested_ = false; note_.clear();
    return ss::okStatus();
}

void BroadcastRunner::cancelStage() { if (!started_) { session_.reset(); summary_.clear(); } }

ss::Status BroadcastRunner::confirmAndStart(ss::ActionSource src, const FfmpegProbe& probe) {
    if (!session_ || started_) return ss::makeError(ss::ErrorCategory::InvalidArgument, "Request the broadcast first, then confirm it");
    if (clock_.nowMonotonic() - stagedAt_ > ss::secToNs(15)) { session_.reset(); return ss::makeError(ss::ErrorCategory::Cancelled, "The confirmation timed out. Start the broadcast again"); }
    auto enc = ss::record::selectVideoEncoder(toProbeResults(probe), ss::record::VideoCodecPolicy::H264Only);
    if (!probe.found || !enc.ok()) {
        session_.reset();
        return ss::makeError(ss::ErrorCategory::EncoderUnavailable, probe.found ? "ffmpeg has no usable H.264 encoder (a hardware encoder or libopenh264 is needed), which broadcast requires"
                                                                              : "ffmpeg was not found, which broadcast needs");
    }
    auto tok = session_->confirmPublication(mapSource(src), facts_);
    if (!tok.ok()) { session_.reset(); return tok.error(); }
    bc::BroadcastCommandInput ci;
    ci.ffmpegPath = probe.path;
    ci.settings = settings_;
    ci.encoder = enc.value();
    fps_ = std::max<std::uint32_t>(1, settings_.video.fps);
    w_ = settings_.video.width; h_ = settings_.video.height; ach_ = settings_.audio.channels;
    ci.video = {"pipe:0", w_, h_, fps_, 1, ss::record::PixelFormat::Rgba};
    ci.audio = {"pipe:3", settings_.audio.rateHz, settings_.audio.channels};
    ci.sync.videoStartHostNs = ci.sync.audioStartHostNs = clock_.nowMonotonic();
    auto st = session_->start(tok.value(), facts_, ci);
    if (!st.ok()) { note_ = st.error().detail; return st; }
    started_ = true; videoOn_ = true; pumpDrops_ = 0; lastTick_ = 0;
    return ss::okStatus();
}

void BroadcastRunner::stop() {
    if (!session_ || !started_) { cancelStage(); return; }
    stopRequested_ = true;
    session_->stop();
}

void BroadcastRunner::onAudio(const AudioChunk& c) {
    if (!active() || !feed_ || !feed_->on) return;
    const float* src = c.s.data();
    std::size_t n = c.s.size();
    if (c.channels != ach_) { remapChannels(c, ach_, abuf_); src = abuf_.data(); n = abuf_.size(); }
    auto data = std::make_shared<std::vector<std::uint8_t>>(reinterpret_cast<const std::uint8_t*>(src), reinterpret_cast<const std::uint8_t*>(src) + n * sizeof(float));
    ss::record::MediaChunk m;
    m.seq = ++feed_->seq; m.data = std::move(data);
    feed_->audio.push(m);
}

void BroadcastRunner::onVideo(const ss::PixelBuffer& frame) {
    if (!active() || !feed_ || !feed_->on) return;
    fitRgba(frame, w_, h_, vbuf_);
    ss::record::MediaChunk m;
    m.seq = ++feed_->seq;
    m.data = std::make_shared<std::vector<std::uint8_t>>(vbuf_);
    feed_->video.push(m);
}

void BroadcastRunner::pump() {
    auto p = factory_->live.lock();
    if (!p) return;
    procs_.pumpWrites(*p);
    if (!p->running()) return;
    ss::record::MediaChunk m;
    while (feed_->audio.pop(m)) if (!procs_.writeAux(*p, m.data->data(), m.data->size())) ++pumpDrops_;
    while (feed_->video.pop(m)) if (!procs_.writeStdin(*p, m.data->data(), m.data->size())) ++pumpDrops_;
}

void BroadcastRunner::tick() {
    if (session_ && !started_ && clock_.nowMonotonic() - stagedAt_ > ss::secToNs(15)) { cancelStage(); return; }
    if (!session_ || !started_) return;
    if (feed_) pump();
    const ss::Ns now = clock_.nowMonotonic();
    if (now - lastTick_ >= ss::msToNs(50) || session_->state() == bc::State::Stopping) { lastTick_ = now; session_->tick(); }
    if (stopRequested_ && session_->state() == bc::State::Off) { note_ = "Broadcast stopped"; stopRequested_ = false; videoOn_ = false; started_ = false; session_.reset(); }
}

BroadcastView BroadcastRunner::view() const {
    BroadcastView v;
    v.note = note_;
    if (!session_) return v;
    if (!started_) { v.awaitingConfirm = true; v.confirmText = summary_; v.encryption = encryption_; return v; }
    const auto st = session_->status();
    v.encryption = st.encryptionText;
    v.bitrateKbps = static_cast<std::uint32_t>(st.bitrateKbps);
    v.droppedFrames = st.droppedFrames + pumpDrops_;
    v.note = st.connection;
    switch (st.state) {
        case bc::State::Off: v.state = BroadcastUiState::Off; break;
        case bc::State::Preparing: case bc::State::Connecting: v.state = BroadcastUiState::Preparing; break;
        case bc::State::Live: v.state = BroadcastUiState::Live; break;
        case bc::State::Reconnecting: v.state = BroadcastUiState::Reconnecting; break;
        case bc::State::Stopping: v.state = BroadcastUiState::Stopping; break;
        case bc::State::Error: v.state = BroadcastUiState::Failed; if (!st.errors.empty()) v.note = st.errors.front().message; break;
    }
    return v;
}

}  // namespace ss::app
