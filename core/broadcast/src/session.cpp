#include "ss/broadcast/session.hpp"
#include <algorithm>
#include "ss/broadcast/redact.hpp"

namespace ss::broadcast {

const char* stateName(State s) {
    switch (s) {
        case State::Off: return "Off";
        case State::Preparing: return "Preparing";
        case State::Connecting: return "Connecting";
        case State::Live: return "Live";
        case State::Reconnecting: return "Reconnecting";
        case State::Stopping: return "Stopping";
        case State::Error: return "Error";
    }
    return "?";
}
bool isUserAction(ActionSource s) { return s == ActionSource::Ui || s == ActionSource::Keyboard || s == ActionSource::Midi; }

const std::vector<Ns>& reconnectSchedule() {
    static const std::vector<Ns> k{secToNs(1), secToNs(2), secToNs(4), secToNs(8), secToNs(15)};
    return k;
}

namespace {
constexpr std::size_t kMaxErrors = 10;
constexpr std::size_t kMaxLog = 500;
constexpr std::size_t kMaxStderrTail = 8192;
Error denied(std::string d) { return makeError(ErrorCategory::PermissionDenied, std::move(d), Recoverability::OperatorAction); }
Error invalid(std::string d) { return makeError(ErrorCategory::InvalidArgument, std::move(d), Recoverability::OperatorAction); }
}  // namespace

BroadcastSession::BroadcastSession(BroadcastSettings settings, const IClock& clock, Rng& rng, IProcessFactory& processes, IMediaFeed& feed,
                                   const ISecretResolver& secrets, SessionConfig cfg)
    : settings_(std::move(settings)), clock_(clock), rng_(rng), factory_(processes), feed_(feed), secrets_(secrets), cfg_(cfg) {
    connection_ = "Off";
}

void BroadcastSession::note(const std::string& line) {
    log_.push_back(redactText(line));
    if (log_.size() > kMaxLog) log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - kMaxLog));
}
void BroadcastSession::setState(State s, const std::string& why) {
    note(std::string("state ") + stateName(state_) + " -> " + stateName(s) + (why.empty() ? "" : ": " + why));
    state_ = s;
    stateSince_ = clock_.nowMonotonic();
}
void BroadcastSession::recordError(const Failure& f, bool terminal) {
    errors_.push_front(StatusError{clock_.nowMonotonic(), f.kind, redactText(f.message), terminal});
    while (errors_.size() > kMaxErrors) errors_.pop_back();
    note(std::string("error (") + failureName(f.kind) + (terminal ? ", terminal" : "") + "): " + f.message);
}

void BroadcastSession::updateSettings(const BroadcastSettings& s) {
    if (state_ == State::Off || state_ == State::Error) settings_ = s;
}

Result<StartToken> BroadcastSession::confirmPublication(ActionSource source, const PublicationFacts& facts) {
    if (!isUserAction(source)) {
        note("start refused: the request did not come from a person");
        return denied("Streaming can only be started by a person using a button, key or MIDI control. Loading, restarting or benchmarking a show never starts it.");
    }
    if (hasBlocking(validateSettings(settings_))) return invalid("The stream settings are not valid yet.");
    activeTokenHash_ = makeSummary(settings_, facts).hash();
    activeToken_ = nextTokenId_++;
    activeTokenExpires_ = clock_.nowMonotonic() + cfg_.tokenTtl;
    return StartToken(activeToken_, activeTokenHash_, activeTokenExpires_);
}

Status BroadcastSession::start(const StartToken& token, const PublicationFacts& facts, BroadcastCommandInput media) {
    if (state_ != State::Off && state_ != State::Error) return invalid("A stream is already running.");
    if (!token.valid() || token.id_ != activeToken_) { note("start refused: no valid confirmation"); return denied("Confirm the publication summary before starting the stream."); }
    activeToken_ = 0;                                                       // single use
    if (clock_.nowMonotonic() > token.expires_) { note("start refused: confirmation expired"); return denied("The confirmation expired. Confirm the summary again."); }
    if (makeSummary(settings_, facts).hash() != token.hash_) { note("start refused: summary changed"); return denied("The stream settings changed after you confirmed them. Confirm again."); }

    setState(State::Preparing, "validating settings and secrets");
    facts_ = facts;
    media.settings = settings_;
    auto cmd = FfmpegCommandBuilder::buildBroadcast(media);
    if (!cmd.ok()) {
        recordError(Failure{FailureKind::InvalidConfig, cmd.error().detail}, true);
        connection_ = "Could not prepare: " + cmd.error().detail;
        setState(State::Error, cmd.error().detail);
        return cmd.error();
    }
    const bool rtmp = settings_.profile == Profile::Rtmps || settings_.profile == Profile::Rtmp;
    if ((rtmp && !secrets_.exists(settings_.streamKey)) || (!settings_.srtPassphrase.empty() && !secrets_.exists(settings_.srtPassphrase))) {
        const Failure f{FailureKind::Auth, "A stored secret (stream key or passphrase) is missing. Enter it in the secret store."};
        recordError(f, true);
        connection_ = f.message;
        setState(State::Error, "missing secret");
        return denied(f.message);
    }
    command_ = cmd.value();
    note("command: " + command_.displayLine());
    attempt_ = 0; totalReconnects_ = 0; outageStart_ = -1; dropsBase_ = 0; dropsCur_ = 0; bytesBase_ = 0; lastTotal_ = 0; liveSince_ = -1;
    errors_.clear();
    ++gen_;
    feed_.abandonQueued();
    feed_.requestKeyframe();
    feed_.setActive(true);
    Status st = spawn();
    if (!st.ok()) {
        feed_.setActive(false);
        const Failure f{FailureKind::InvalidConfig, st.error().detail};
        recordError(f, true);
        connection_ = "Could not start ffmpeg: " + st.error().detail;
        setState(State::Error, st.error().detail);
        return st;
    }
    return okStatus();
}

Status BroadcastSession::spawn() {
    auto argv = command_.materialize(secrets_);
    if (!argv.ok()) return argv.error();
    proc_ = factory_.create();
    Status st = proc_->start(argv.value());
    wipe(argv.value());
    if (!st.ok()) { proc_.reset(); return st; }
    parser_ = ProgressParser{};
    stderrTail_.clear();
    lastTotal_ = 0; growthSamples_ = 0; dropsCur_ = 0; lastSampleAt_ = -1; lastGrowthAt_ = clock_.nowMonotonic(); bitrate_ = 0;
    connection_ = "Connecting to " + settings_.host;
    setState(State::Connecting, "process started");
    return okStatus();
}

bool BroadcastSession::outageExpired() const {
    return outageStart_ >= 0 && clock_.nowMonotonic() - outageStart_ >= cfg_.reconnectDeadline;
}

void BroadcastSession::scheduleReconnect() {
    const auto& sched = reconnectSchedule();
    const Ns base = sched[std::min<std::size_t>(attempt_, sched.size() - 1)];
    const double factor = 1.0 + cfg_.jitter * (2.0 * static_cast<double>(rng_.unit()) - 1.0);
    const Ns delay = static_cast<Ns>(static_cast<double>(base) * factor);
    nextAttemptAt_ = clock_.nowMonotonic() + delay;
    feed_.abandonQueued();                          // do not replay media captured before the disconnect
    feed_.requestKeyframe();
    connection_ = "Connection lost. Reconnecting (attempt " + std::to_string(attempt_ + 1) + ")";
    setState(State::Reconnecting, "retry in " + std::to_string(delay / kNsPerMs) + " ms");
}

void BroadcastSession::handleFailure(const Failure& f) {
    if (proc_) { if (proc_->running()) proc_->kill(); proc_.reset(); }
    dropsBase_ += dropsCur_; dropsCur_ = 0;
    bytesBase_ += lastTotal_; lastTotal_ = 0;
    liveSince_ = -1;
    const bool terminal = isTerminal(f.kind);
    recordError(f, terminal);
    if (terminal) {
        feed_.setActive(false); feed_.abandonQueued();
        connection_ = std::string("Stopped (") + failureName(f.kind) + "): " + f.message + " Fix the setting and start again.";
        ++gen_;
        setState(State::Error, failureName(f.kind));
        return;
    }
    if (!settings_.reconnectEnabled) {
        feed_.setActive(false); feed_.abandonQueued();
        connection_ = "Connection lost; automatic reconnect is off.";
        ++gen_;
        setState(State::Error, "reconnect disabled");
        return;
    }
    if (outageStart_ < 0) outageStart_ = clock_.nowMonotonic();
    if (outageExpired()) {
        feed_.setActive(false); feed_.abandonQueued();
        connection_ = "Could not reconnect within " + std::to_string(cfg_.reconnectDeadline / kNsPerSec) + " s. The stream has stopped.";
        ++gen_;
        setState(State::Error, "reconnect deadline");
        return;
    }
    scheduleReconnect();
}

void BroadcastSession::pollProcess() {
    if (!proc_) return;
    const Ns now = clock_.nowMonotonic();
    for (const auto& smp : parser_.feed(proc_->readProgress())) {
        if (smp.totalSize > lastTotal_ && smp.outTime > 0) {
            ++growthSamples_;
            lastGrowthAt_ = now;
            if (lastSampleAt_ >= 0 && now > lastSampleAt_)
                bitrate_ = static_cast<double>(smp.totalSize - lastTotal_) * 8.0 / 1000.0 / nsToSec(now - lastSampleAt_);
        }
        if (smp.totalSize != lastTotal_ || lastSampleAt_ < 0) lastSampleAt_ = now;
        lastTotal_ = std::max<std::uint64_t>(lastTotal_, smp.totalSize);
        dropsCur_ = smp.dropFrames;
        if (state_ == State::Connecting && growthSamples_ >= 2) {
            connection_ = "Live";
            liveSince_ = now;
            setState(State::Live, "outbound media confirmed");
        }
    }
    stderrTail_ += proc_->readStderr();
    if (stderrTail_.size() > kMaxStderrTail) stderrTail_.erase(0, stderrTail_.size() - kMaxStderrTail);

    if (!proc_->running()) {
        stderrTail_ += proc_->readStderr();
        Failure f = classifyExit(proc_->exitCode(), stderrTail_);
        if (f.kind == FailureKind::None) f = Failure{FailureKind::ConnectionLost, "The encoder ended unexpectedly."};
        handleFailure(f);
        return;
    }
    if (state_ == State::Connecting) {
        if (outageExpired()) { handleFailure(Failure{FailureKind::Timeout, "The connection could not be restored in time."}); return; }
        if (now - stateSince_ > cfg_.connectTimeout) { handleFailure(Failure{FailureKind::Timeout, "No media was delivered before the connect timeout."}); return; }
    } else if (state_ == State::Live) {
        if (now - lastGrowthAt_ > cfg_.stallTimeout) { handleFailure(Failure{FailureKind::Stalled, "No data reached the server for several seconds."}); return; }
        if (liveSince_ >= 0 && now - liveSince_ >= cfg_.stableAfter && outageStart_ >= 0) { outageStart_ = -1; attempt_ = 0; note("connection stable; outage cleared"); }
    }
}

void BroadcastSession::tick() {
    const Ns now = clock_.nowMonotonic();
    switch (state_) {
        case State::Connecting:
        case State::Live: pollProcess(); break;
        case State::Reconnecting:
            if (outageExpired()) {
                feed_.setActive(false); feed_.abandonQueued();
                recordError(Failure{FailureKind::Timeout, "Could not reconnect within the deadline."}, true);
                connection_ = "Could not reconnect within " + std::to_string(cfg_.reconnectDeadline / kNsPerSec) + " s. The stream has stopped.";
                ++gen_;
                setState(State::Error, "reconnect deadline");
            } else if (now >= nextAttemptAt_) {
                onReconnectTimer(gen_);
            }
            break;
        case State::Stopping:
            if (!proc_ || !proc_->running()) finishStop();
            else if (now >= stopDeadline_) { proc_->kill(); if (!proc_->running()) finishStop(); }
            break;
        default: break;
    }
}

void BroadcastSession::onReconnectTimer(std::uint64_t generation) {
    if (generation != gen_ || state_ != State::Reconnecting) { note("ignored stale reconnect timer"); return; }
    if (outageExpired()) { tick(); return; }
    ++gen_;                                          // late exit callbacks of the previous process are now stale
    ++attempt_; ++totalReconnects_;
    feed_.abandonQueued();
    feed_.requestKeyframe();
    Status st = spawn();
    if (!st.ok()) handleFailure(Failure{FailureKind::InvalidConfig, st.error().detail});
}

void BroadcastSession::onProcessExited(std::uint64_t generation, int exitCode, const std::string& stderrText) {
    if (generation != gen_ || (state_ != State::Connecting && state_ != State::Live)) { note("ignored stale process exit"); return; }
    Failure f = classifyExit(exitCode, stderrTail_ + stderrText);
    if (f.kind == FailureKind::None) f = Failure{FailureKind::ConnectionLost, "The encoder ended unexpectedly."};
    handleFailure(f);
}

void BroadcastSession::stop() {
    if (state_ == State::Off || state_ == State::Stopping) return;
    ++gen_;                                          // every pending callback of the old session is now stale
    activeToken_ = 0;
    feed_.setActive(false);
    feed_.abandonQueued();
    if (state_ == State::Error || !proc_ || !proc_->running()) { finishStop(); return; }
    proc_->requestStop();
    stopDeadline_ = clock_.nowMonotonic() + cfg_.stopGrace;
    connection_ = "Stopping";
    setState(State::Stopping, "operator stop");
    tick();                                          // a process that exits at once leaves Stopping immediately
}

void BroadcastSession::finishStop() {
    proc_.reset();
    feed_.setActive(false);
    outageStart_ = -1; attempt_ = 0; liveSince_ = -1;
    connection_ = "Off";
    setState(State::Off, "stopped");
}

BroadcastStatus BroadcastSession::status() const {
    BroadcastStatus s;
    const Ns now = clock_.nowMonotonic();
    s.state = state_;
    s.connection = connection_;
    s.encryption = encryptionState(settings_);
    s.encryptionText = encryptionLabel(settings_);
    s.bitrateKbps = state_ == State::Live ? bitrate_ : 0;
    s.bytesSent = bytesBase_ + lastTotal_;
    s.droppedFrames = dropsBase_ + dropsCur_ + feed_.droppedChunks();
    s.reconnectAttempts = attempt_;
    s.totalReconnects = totalReconnects_;
    s.outageElapsed = outageStart_ >= 0 ? now - outageStart_ : 0;
    s.nextAttemptIn = state_ == State::Reconnecting ? std::max<Ns>(0, nextAttemptAt_ - now) : 0;
    s.generation = gen_;
    s.errors.assign(errors_.begin(), errors_.end());
    s.secrets.level = SecretHandling::Partial;
    s.secrets.note =
        "Stream key and SRT passphrase are kept only as references, resolved when ffmpeg starts, and never written to settings "
        "exports, logs or this status. PARTIAL: ffmpeg receives them on its command line, where other processes of the same user "
        "and administrators can read them while the stream runs. Run the show machine under a dedicated OS account; this "
        "limitation is not solved by the application.";
    return s;
}

}  // namespace ss::broadcast
