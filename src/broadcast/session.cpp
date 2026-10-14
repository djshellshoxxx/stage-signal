#include "broadcast/session.h"
#include "broadcast/redact.h"
#include <algorithm>
#include <cmath>
#include <atomic>
namespace ss {

const int kReconnectDelaysSec[5] = {1, 2, 4, 8, 15};
const char* broadcastStateName(BroadcastState s) {
  switch (s) { case BroadcastState::Off: return "Off"; case BroadcastState::Preparing: return "Preparing"; case BroadcastState::Connecting: return "Connecting"; case BroadcastState::Live: return "Live";
    case BroadcastState::Reconnecting: return "Reconnecting"; case BroadcastState::Stopping: return "Stopping"; case BroadcastState::Error: return "Error"; }
  return "?";
}

Status UserActionGate::issue(ActionSource src, UserStartToken& out) {
  if (!sourceMayStart(src)) return Status::Fail(Err::PermissionDenied, "Broadcast can be started only by an explicit operator action (button, keyboard or the operator's own controller); show cues, show load, restart, remote and benchmark requests are refused.", false);
  out = UserStartToken(next_++, clock_->nowNs(), src); return Status::Ok();
}

namespace {
struct EventBox {
  struct Ev { uint64_t gen; TransportEvent e; };
  std::mutex m; std::deque<Ev> q;
};
}

struct BroadcastSession::Impl {
  BroadcastDeps d; mutable std::mutex ctl;
  BroadcastState state = BroadcastState::Off; BroadcastSettings settings; PublicationInputs pub;
  uint64_t sessionGen = 0, connGen = 0, lastNonce = 0;
  std::shared_ptr<EventBox> box = std::make_shared<EventBox>();
  std::unique_ptr<ITransport> transport, preMade; PacketQueue queue; MediaPacketPtr headers;
  bool connected = false, flowing = false, inFlight = false, needHeaders = false;
  Ns attemptDeadline = 0, nextAttempt = 0, outageStart = 0; int attempt = 0; uint64_t reconnects = 0, lateIgnored = 0;
  uint64_t sent = 0, bytes = 0, lastBytes = 0; Ns lastRate = 0; double kbps = 0; uint64_t lastDropped = 0; Ns lastDropNs = -1000 * kNsPerSec;
  Status err; bool terminal = false; std::vector<std::string> warnings; std::string scrub; uint32_t rng;
  std::function<void(BroadcastState)> cb; std::vector<BroadcastState> notes;

  explicit Impl(const BroadcastDeps& dd) : d(dd), rng(dd.rngSeed ? dd.rngSeed : 1) {}
  Ns now() const { return d.clock->nowNs(); }
  std::string clean(const std::string& s) const { return cast::redactWithSecret(s, scrub); }
  void log(const std::string& s) { if (d.log) d.log(clean(s)); }
  void setState(BroadcastState s) { if (state == s) return; log(std::string("broadcast ") + broadcastStateName(state) + " -> " + broadcastStateName(s)); state = s; notes.push_back(s); }
  double jitter() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 65535.0 * 0.4 - 0.2; }   // [-0.2, +0.2]
  void closeTransport() { if (preMade) { preMade.reset(); } if (transport) { transport->close(); transport.reset(); } connected = false; inFlight = false; flowing = false; }
  void toError(const Status& s, bool term) {
    closeTransport(); queue.setOpen(false); queue.clear();
    err = Status::Fail(s.code, clean(s.detail), !term); terminal = term; setState(BroadcastState::Error);
    cast::wipe(scrub); log("broadcast error: " + err.detail);
  }

  Status fetchSecrets(std::string& key, std::string& pass) {
    const Endpoint& e = settings.endpoint;
    auto fetch = [&](const std::string& ref, const char* what, std::string& out) -> Status {
      if (!d.secrets) return Status::Fail(Err::PermissionDenied, std::string("No credential store is available to read the ") + what + ".", false);
      Status s = d.secrets->get(ref, out);
      if (!s.ok() || out.empty()) { cast::wipe(out); return Status::Fail(s.ok() ? Err::PermissionDenied : s.code, std::string("The ") + what + " '" + ref + "' could not be read from the credential store" + (s.ok() ? " (empty)" : " (" + cast::redactText(s.detail) + ")") + ". Re-enter it in Broadcast settings.", false); }
      return Status::Ok();
    };
    if (e.protocol == BroadcastProtocol::RTMP || e.protocol == BroadcastProtocol::RTMPS) { Status s = fetch(e.streamKeyRef, "stream key", key); if (!s.ok()) return s; }
    else if (e.protocol == BroadcastProtocol::SRT && !e.streamKeyRef.empty()) { Status s = fetch(e.streamKeyRef, "stream id/key", key); if (!s.ok()) return s; }
    if (e.protocol == BroadcastProtocol::SRT && settings.srtEncryption != SrtEncryption::None) { Status s = fetch(e.passphraseRef, "SRT passphrase", pass); if (!s.ok()) return s; }
    return Status::Ok();
  }

  void startAttempt() {
    ++connGen; connected = false; flowing = false; needHeaders = true;
    queue.clear(); queue.setOpen(true); queue.requireKeyframe(true);
    if (transport) { transport->close(); transport.reset(); }
    std::unique_ptr<ITransport> t; Status cs = Status::Ok();
    if (preMade) t = std::move(preMade); else cs = d.factory ? d.factory->create(settings, t) : Status::Fail(Err::UnsupportedCapability, "no transport factory");
    if (!cs.ok()) { toError(cs, true); return; }
    TransportParams p; p.settings = settings; p.connectTimeoutNs = kConnectTimeoutNs;
    Status ss_ = fetchSecrets(p.secret, p.passphrase);
    if (!ss_.ok()) { toError(ss_, true); return; }
    scrub = p.secret.empty() ? p.passphrase : p.secret;
    transport = std::move(t); inFlight = true; attemptDeadline = now() + kConnectTimeoutNs;
    if (d.requestKeyframe) d.requestKeyframe();
    auto bx = box; uint64_t g = connGen;
    Status s = transport->connect(p, [bx, g](const TransportEvent& e) { std::lock_guard<std::mutex> l(bx->m); bx->q.push_back({g, e}); });
    cast::wipe(p.secret); cast::wipe(p.passphrase);
    log("connecting (attempt " + std::to_string(attempt + 1) + ") to " + endpointUrl(settings.endpoint) + " via " + transport->describe());
    if (!s.ok()) onDisconnect(TransportErrorClass::Network, s.detail);
  }

  void onDisconnect(TransportErrorClass cls, const std::string& detail) {
    std::string det = clean(detail);
    closeTransport();
    if (isTerminal(cls)) {
      Err code = cls == TransportErrorClass::Auth ? Err::PermissionDenied : (cls == TransportErrorClass::InvalidConfig ? Err::InvalidArgument : Err::TransportFailure);
      toError(Status::Fail(code, std::string("Receiver rejected the stream (") + errorClassName(cls) + "): " + det + ". This will not be retried; fix the setting and press Start again."), true); return;
    }
    if (!settings.reconnect) { toError(Status::Fail(Err::TransportFailure, "Connection lost: " + det + " (reconnect is off)"), false); return; }
    if (state != BroadcastState::Reconnecting) { outageStart = now(); attempt = 0; }
    queue.setOpen(false); queue.clear();
    ++attempt;
    double base = kReconnectDelaysSec[std::min(attempt - 1, 4)];
    Ns delay = (Ns)(base * (1.0 + jitter()) * 1e9);
    nextAttempt = now() + delay;
    Ns deadlineAt = outageStart + kReconnectDeadlineNs;
    if (nextAttempt > deadlineAt) nextAttempt = deadlineAt;
    err = Status::Fail(Err::TransportFailure, "Connection problem: " + det, true);
    setState(BroadcastState::Reconnecting);
  }

  void handleEvents() {
    std::deque<EventBox::Ev> evs; { std::lock_guard<std::mutex> l(box->m); evs.swap(box->q); }
    for (auto& ev : evs) {
      if (ev.gen != connGen || !(state == BroadcastState::Connecting || state == BroadcastState::Live || state == BroadcastState::Reconnecting) || (!inFlight && !connected)) { ++lateIgnored; continue; }
      if (ev.e.type == TransportEvent::Type::Connected) { connected = true; inFlight = false; needHeaders = true; log("transport connected"); }
      else onDisconnect(ev.e.cls, ev.e.detail);
    }
  }

  void pump() {
    if (!connected || !transport) return;
    if (needHeaders && headers) {
      Status s = transport->send(*headers);
      if (s.ok()) needHeaders = false; else if (s.code != Err::ResourceExhausted) { onDisconnect(TransportErrorClass::Network, s.detail); return; } else return;
    } else if (needHeaders) needHeaders = false;
    for (int n = 0; n < 256; ++n) {
      MediaPacketPtr p = queue.peek(); if (!p) break;
      Status s = transport->send(*p);
      if (s.ok()) {
        queue.popFront(); ++sent; bytes += p->data.size();
        if (!flowing) { flowing = true; if (state == BroadcastState::Connecting || state == BroadcastState::Reconnecting) { if (state == BroadcastState::Reconnecting) ++reconnects; attempt = 0; err = Status(); setState(BroadcastState::Live); } }
        if (!connected || !transport) return;
      } else if (s.code == Err::ResourceExhausted) break;
      else { onDisconnect(TransportErrorClass::Network, s.detail); return; }
    }
  }

  void tick() {
    handleEvents();
    if (state == BroadcastState::Off || state == BroadcastState::Error || state == BroadcastState::Stopping || state == BroadcastState::Preparing) return;
    Ns t = now();
    if (state == BroadcastState::Reconnecting && !inFlight && !connected) {
      if (t - outageStart >= kReconnectDeadlineNs) { toError(Status::Fail(Err::TransportFailure, "Could not reconnect within 120 s; giving up. Local recording (if running) is unaffected. Check the network/receiver, then press Start again."), false); return; }
      if (t >= nextAttempt) startAttempt();
    }
    if ((state == BroadcastState::Connecting || state == BroadcastState::Reconnecting) && inFlight && !connected && t >= attemptDeadline) onDisconnect(TransportErrorClass::Network, "connection attempt timed out after 15 s");
    if (state == BroadcastState::Reconnecting && (inFlight || connected) && t - outageStart >= kReconnectDeadlineNs && !flowing) { toError(Status::Fail(Err::TransportFailure, "Could not reconnect within 120 s; giving up."), false); return; }
    pump();
    // bitrate (1 s windows) and sustained-drop warning
    { PacketQueueStats qs = queue.stats(); if (qs.dropped > lastDropped) { lastDropNs = t; lastDropped = qs.dropped; } }
    if (t - lastRate >= kNsPerSec) { double dt = nsToSec(t - lastRate); kbps = lastRate ? (double)(bytes - lastBytes) * 8.0 / 1000.0 / dt : 0; lastBytes = bytes; lastRate = t; }
  }
};

BroadcastSession::BroadcastSession(const BroadcastDeps& d) : p_(new Impl(d)) {}
BroadcastSession::~BroadcastSession() { stop(); }
void BroadcastSession::setStateCallback(std::function<void(BroadcastState)> cb) { std::lock_guard<std::mutex> l(p_->ctl); p_->cb = std::move(cb); }
BroadcastState BroadcastSession::state() const { std::lock_guard<std::mutex> l(p_->ctl); return p_->state; }

// run `f` under the control lock, then deliver queued state notifications without it
template <class F> static auto locked(BroadcastSession::Impl* p, std::mutex& m, F f) -> decltype(f()) {
  std::vector<BroadcastState> notes; std::function<void(BroadcastState)> cb;
  std::unique_lock<std::mutex> l(m);
  auto r = f();
  notes.swap(p->notes); cb = p->cb; l.unlock();
  if (cb) for (auto s : notes) cb(s);
  return r;
}

Status BroadcastSession::start(UserStartToken&& token, const BroadcastSettings& s, const PublicationInputs& pub) {
  return locked(p_.get(), p_->ctl, [&]() -> Status {
    Impl& I = *p_;
    if (I.state != BroadcastState::Off && I.state != BroadcastState::Error)
      return Status::Fail(Err::InvalidArgument, std::string("A broadcast is already active (") + broadcastStateName(I.state) + "); stop it first. Only one endpoint is supported.");
    // explicit user action check (REQ-B02)
    if (!token.valid()) return Status::Fail(Err::PermissionDenied, "Broadcast start requires an explicit operator action; no start token was supplied.", false);
    uint64_t nonce = token.nonce(); Ns issued = token.issuedNs(); ActionSource src = token.source(); token = UserStartToken();   // consume
    if (!UserActionGate::sourceMayStart(src)) return Status::Fail(Err::PermissionDenied, "Start token does not originate from an operator action.", false);
    if (nonce <= I.lastNonce) return Status::Fail(Err::PermissionDenied, "Start token was already used; press Start again.", false);
    if (I.now() - issued > kTokenTtlNs || I.now() < issued) return Status::Fail(Err::PermissionDenied, "Start token expired; press Start again.", false);
    I.lastNonce = nonce;
    I.settings = s; I.pub = pub; I.err = Status(); I.terminal = false; I.warnings.clear(); cast::wipe(I.scrub);
    I.sent = I.bytes = I.lastBytes = 0; I.kbps = 0; I.lastRate = 0; I.attempt = 0; I.reconnects = 0; I.lateIgnored = 0; I.lastDropped = 0; I.lastDropNs = -1000 * kNsPerSec;
    ++I.sessionGen; ++I.connGen; I.connected = I.flowing = I.inFlight = false;
    { std::lock_guard<std::mutex> l(I.box->m); I.box->q.clear(); }
    I.setState(BroadcastState::Preparing);
    Status v = validateSettings(s, &I.warnings);
    if (!v.ok()) { I.toError(v, true); return I.err; }
    if (I.d.encoders) {
      EncoderAvailability a = I.d.encoders->availability(s.videoCodec);
      if (!a.available) { I.toError(Status::Fail(Err::EncoderUnavailable, std::string(codecName(s.videoCodec)) + " encoder unavailable for broadcast: " + a.reason), true); return I.err; }
    }
    I.queue.reset();
    // transport availability is checked BEFORE touching the secret store
    { std::unique_ptr<ITransport> probe; Status cs = I.d.factory ? I.d.factory->create(s, probe) : Status::Fail(Err::UnsupportedCapability, "no transport factory");
      if (!cs.ok()) { I.toError(cs, true); return I.err; }
      I.preMade = std::move(probe); }
    I.setState(BroadcastState::Connecting);
    I.startAttempt();
    I.handleEvents();                           // transports that answer synchronously (config errors) surface in the return value
    return I.state == BroadcastState::Error ? I.err : Status::Ok();
  });
}

void BroadcastSession::stop() {
  locked(p_.get(), p_->ctl, [&]() -> int {
    Impl& I = *p_;
    if (I.state == BroadcastState::Off) return 0;
    if (I.state == BroadcastState::Error) { I.setState(BroadcastState::Off); cast::wipe(I.scrub); return 0; }
    I.setState(BroadcastState::Stopping);
    ++I.sessionGen; ++I.connGen;                 // every callback from any earlier attempt is now stale
    I.closeTransport(); I.queue.setOpen(false); I.queue.clear(); cast::wipe(I.scrub);
    I.err = Status(); I.attempt = 0; I.nextAttempt = 0;
    I.setState(BroadcastState::Off);
    return 0;
  });
}

void BroadcastSession::tick() { locked(p_.get(), p_->ctl, [&]() -> int { p_->tick(); return 0; }); }
bool BroadcastSession::submit(const MediaPacketPtr& pk) { return pk && p_->queue.push(pk); }
void BroadcastSession::setHeaders(const MediaPacketPtr& h) { std::lock_guard<std::mutex> l(p_->ctl); p_->headers = h; }

BroadcastStatus BroadcastSession::status() const {
  std::lock_guard<std::mutex> l(p_->ctl);
  const Impl& I = *p_; BroadcastStatus s; s.state = I.state; s.connected = I.connected; s.mediaFlowing = I.flowing; s.live = I.state == BroadcastState::Live;
  s.protocol = I.settings.endpoint.protocol; s.endpoint = cast::redactUrl(endpointUrl(I.settings.endpoint)); s.encryption = encryptionLabel(I.settings); s.encrypted = isEncrypted(I.settings);
  s.attempt = I.attempt; s.reconnects = I.reconnects; s.targetKbps = I.settings.videoKbps + I.settings.audioKbps; s.bitrateKbps = I.kbps;
  Ns t = I.d.clock->nowNs();
  if (I.state == BroadcastState::Reconnecting) { s.outageMs = (t - I.outageStart) / kNsPerMs; if (!I.inFlight && !I.connected) s.reconnectInMs = std::max<int64_t>(0, (I.nextAttempt - t) / kNsPerMs); }
  PacketQueueStats qs = I.queue.stats();
  s.packetsSent = I.sent; s.bytesSent = I.bytes; s.packetsDropped = qs.dropped; s.queueDepth = qs.depth; s.droppedWhileOffline = qs.rejectedWhileClosed; s.lateCallbacksIgnored = I.lateIgnored;
  s.error = I.err; s.errorTerminal = I.terminal; s.warnings = I.warnings; s.generation = I.connGen;
  if (t - I.lastDropNs < 10 * kNsPerSec && I.state != BroadcastState::Off) s.warnings.push_back("Upload cannot keep up with " + std::to_string(s.targetKbps) + " kb/s: packets are being dropped. Local recording is unaffected; lower the stream bitrate or check the network.");
  std::string txt = broadcastStateName(I.state);
  switch (I.state) {
    case BroadcastState::Connecting: txt += I.connected ? " - connected, waiting for media to be delivered" : " - opening connection"; break;
    case BroadcastState::Live: txt += " - connected and delivering media (this does not prove a public service is displaying it)"; break;
    case BroadcastState::Reconnecting: txt += " - attempt " + std::to_string(I.attempt) + (s.reconnectInMs >= 0 ? ", retrying in " + std::to_string((s.reconnectInMs + 999) / 1000) + " s" : ", connecting now") + "; outage " + std::to_string(s.outageMs / 1000) + " s of 120 s; local recording unaffected"; break;
    case BroadcastState::Error: txt += " - " + I.err.detail; break;
    default: break;
  }
  s.text = cast::redactWithSecret(txt, I.scrub);
  return s;
}

} // namespace ss
