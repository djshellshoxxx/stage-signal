// MOD-12 broadcast tests: validation, explicit-start gate, state machine, reconnect schedule, stop/generation, terminal errors,
// honest unsupported transports, bounded queues, secret hygiene, local recording independence.
#include "test.h"
#include "broadcast/session.h"
#include "broadcast/rtp.h"
#include "broadcast/redact.h"
#include "recording/session.h"
#include <map>
#include <set>
#include <cmath>
using namespace ss;

static const char* kSecret = "sk_live_TOPSECRET_9f3a77";

struct FakeSecrets : ISecretStore {
  std::map<std::string, std::string> m; int gets = 0; bool fail = false;
  Status put(const std::string& r, const std::string& s) override { m[r] = s; return Status::Ok(); }
  Status get(const std::string& r, std::string& out) override { ++gets; if (fail) return Status::Fail(Err::InternalFault, "store locked while reading " + r + " secret=" + kSecret); auto it = m.find(r); if (it == m.end()) return Status::Fail(Err::InvalidArgument, "no such reference"); out = it->second; return Status::Ok(); }
  Status remove(const std::string& r) override { m.erase(r); return Status::Ok(); }
};

// Captures the transport callback so tests can fire it LATE (after stop / new generation).
struct CapturingTransport : FaultyTransport {
  std::function<void(const TransportEvent&)> captured;
  Status connect(const TransportParams& p, std::function<void(const TransportEvent&)> cb) override { captured = cb; return FaultyTransport::connect(p, cb); }
};

struct Rig {
  FakeClock clk; FakeSecrets secrets; TransportFactory factory; std::vector<FaultyTransport*> made; std::vector<std::string> logs; int keyframeReqs = 0;
  std::vector<BroadcastState> states; std::function<void(FaultyTransport&)> prepare; bool capturing = false;
  std::unique_ptr<BroadcastSession> s; UserActionGate gate{&clk};
  EncoderRegistry reg = EncoderRegistry::withBuiltins();
  Rig() {
    clk.t = 100 * kNsPerSec; clk.wall = 1767225600000LL; secrets.put("key-main", kSecret); secrets.put("pass-srt", "srt-pass-phrase-123");
    factory.setTestOverride([this]() -> std::unique_ptr<ITransport> {
      FaultyTransport* ft; std::unique_ptr<ITransport> u;
      if (capturing) { auto* c = new CapturingTransport(); ft = c; u.reset(c); } else { ft = new FaultyTransport(); u.reset(ft); }
      if (prepare) prepare(*ft); made.push_back(ft); return u; });
    BroadcastDeps d; d.secrets = &secrets; d.clock = &clk; d.factory = &factory; d.log = [this](const std::string& l) { logs.push_back(l); }; d.requestKeyframe = [this] { ++keyframeReqs; }; d.rngSeed = 7;
    s.reset(new BroadcastSession(d)); s->setStateCallback([this](BroadcastState st) { states.push_back(st); });
  }
  BroadcastSettings settings(BroadcastProtocol p = BroadcastProtocol::RTMPS) {
    BroadcastSettings b = defaultBroadcastSettings(); b.endpoint.name = "Studio ingest"; b.endpoint.protocol = p; b.endpoint.host = "ingest.example.net"; b.endpoint.path = "/live"; b.endpoint.streamKeyRef = "key-main";
    if (p == BroadcastProtocol::SRT) { b.endpoint.port = 9000; b.endpoint.path = ""; }
    return b;
  }
  Status start(const BroadcastSettings& b, ActionSource src = ActionSource::UI) { UserStartToken t; Status g = gate.issue(src, t); if (!g.ok()) return g; return s->start(std::move(t), b); }
  void step(Ns ms, int n = 1) { for (int i = 0; i < n; ++i) { clk.advance(ms * kNsPerMs); s->tick(); } }
  MediaPacketPtr pkt(PacketKind k, bool key = false, size_t n = 100, Ns pts = 0) { auto p = std::make_shared<MediaPacket>(); p->kind = k; p->key = key; p->pts = pts; p->data.assign(n, 0x42); p->codec = k == PacketKind::Audio ? PacketCodec::Aac : PacketCodec::H264; return p; }
};
static bool leaks(const std::string& s) { return s.find("TOPSECRET") != std::string::npos || s.find(kSecret) != std::string::npos; }

TEST(cast_defaults_and_validation) {
  BroadcastSettings d = defaultBroadcastSettings();
  CHECK(d.width == 1280 && d.height == 720 && d.fps == 30 && d.videoKbps == 2500 && d.audioKbps == 128 && d.keyframeSec == 2 && d.videoCodec == VideoCodec::H264 && d.audioRate == 48000 && d.audioChannels == 2 && d.reconnect);
  Rig r; BroadcastSettings ok = r.settings(); CHECK(validateSettings(ok).ok());
  auto bad = [&](std::function<void(BroadcastSettings&)> f) { BroadcastSettings b = r.settings(); f(b); return validateSettings(b); };
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.host = "user:pw@ingest.example.net"; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.host = "a b"; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.host = ""; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.path = "/live?key=abc"; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.path = "/live/abcdef"; }).code == Err::InvalidArgument);   // key must not ride in the path
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.path = "/../x"; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.streamKeyRef = ""; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.streamKeyRef = "has space"; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.width = 1281; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.fps = 120; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.keyframeSec = 0; }).code == Err::InvalidArgument);
  CHECK(bad([](BroadcastSettings& b) { b.videoCodec = VideoCodec::MJPEG; }).code == Err::UnsupportedCapability);
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.protocol = BroadcastProtocol::SRT; b.endpoint.path = ""; }).code == Err::InvalidArgument);   // SRT needs port
  CHECK(bad([](BroadcastSettings& b) { b.endpoint.protocol = BroadcastProtocol::SRT; b.endpoint.path = ""; b.endpoint.port = 9000; b.srtEncryption = SrtEncryption::Aes128; }).code == Err::InvalidArgument);   // no passphrase ref
  { BroadcastSettings s = r.settings(BroadcastProtocol::SRT); s.srtEncryption = SrtEncryption::Aes256; s.endpoint.passphraseRef = "pass-srt"; CHECK(validateSettings(s).ok()); CHECK(encryptionLabel(s).find("AES-256") != std::string::npos); }
  // RTP: even port, LAN only unless opted in, unencrypted label
  BroadcastSettings rtp = r.settings(BroadcastProtocol::RTP); rtp.videoCodec = VideoCodec::MJPEG; rtp.endpoint.path = ""; rtp.endpoint.streamKeyRef = ""; rtp.endpoint.host = "192.168.1.50"; rtp.endpoint.port = 5004; rtp.width = 640; rtp.height = 480; rtp.fps = 20;
  std::vector<std::string> w; CHECK(validateSettings(rtp, &w).ok()); bool unenc = false; for (auto& x : w) if (x.find("UNENCRYPTED") != std::string::npos) unenc = true; CHECK(unenc);
  CHECK(encryptionLabel(rtp).find("UNENCRYPTED") != std::string::npos); CHECK(!isEncrypted(rtp));
  { BroadcastSettings b = rtp; b.endpoint.port = 5005; CHECK(!validateSettings(b).ok()); }
  { BroadcastSettings b = rtp; b.endpoint.host = "203.0.113.9"; CHECK(validateSettings(b).code == Err::InvalidArgument); b.allowNonLanRtp = true; CHECK(validateSettings(b).ok()); }
  { BroadcastSettings b = rtp; b.endpoint.host = "239.1.2.3"; CHECK(validateSettings(b).ok()); }
  { BroadcastSettings b = rtp; b.videoCodec = VideoCodec::H264; CHECK(validateSettings(b).code == Err::UnsupportedCapability); }
  // BASE fallback profile
  CHECK(validateSettings([&] { BroadcastSettings b = baseFallbackSettings(false); b.endpoint = ok.endpoint; return b; }()).ok());
  CHECK(validateSettings([&] { BroadcastSettings b = baseFallbackSettings(true); b.endpoint = ok.endpoint; b.fps = 30; return b; }()).code == Err::InvalidArgument);
  // URL parsing
  Endpoint e; CHECK(parseEndpointUrl("rtmps://ingest.example.net:443/live", e).ok() && e.host == "ingest.example.net" && e.port == 443 && e.path == "/live" && e.protocol == BroadcastProtocol::RTMPS);
  CHECK(!parseEndpointUrl("rtmps://u:p@h/live", e).ok()); CHECK(!parseEndpointUrl("rtmps://h/live?key=1", e).ok()); CHECK(!parseEndpointUrl("http://h/", e).ok()); CHECK(!parseEndpointUrl("rtmp://h:70000/x", e).ok());
  CHECK(parseEndpointUrl("srt://[::1]:9000", e).ok() && e.host == "::1" && e.port == 9000);
  CHECK(endpointUrl(r.settings().endpoint) == "rtmps://ingest.example.net:443/live");
  // export/import round trip carries references only
  std::string js = exportSettingsJson(ok); CHECK(!leaks(js)); BroadcastSettings back; CHECK(importSettingsJson(js, back).ok()); CHECK(back.endpoint.streamKeyRef == "key-main" && back.width == 1280);
  CHECK(!importSettingsJson("{\"endpoint\":{\"protocol\":\"rtmps\",\"host\":\"a@b\"}}", back).ok());
}

TEST(cast_summary_before_start) {
  Rig r; PublicationInputs in; in.cameraInProgram = true; in.cameraLabel = "Stage cam"; in.localRecordingActive = true;
  std::string t = BroadcastSession::summary(r.settings(), in);
  CHECK(t.find("1280x720 @ 30") != std::string::npos); CHECK(t.find("INCLUDING the camera") != std::string::npos); CHECK(t.find("TLS") != std::string::npos);
  CHECK(t.find("key-main") != std::string::npos); CHECK(!leaks(t)); CHECK(t.find("Nothing is sent until you press Start") != std::string::npos);
  in.cameraInProgram = false; CHECK(BroadcastSession::summary(r.settings(), in).find("camera NOT included") != std::string::npos);
  BroadcastSettings rtmp = r.settings(BroadcastProtocol::RTMP); CHECK(BroadcastSession::summary(rtmp, in).find("UNENCRYPTED") != std::string::npos);
}

TEST(cast_start_requires_explicit_user_token) {
  Rig r; BroadcastSettings b = r.settings();
  UserStartToken t;
  CHECK(r.gate.issue(ActionSource::Show, t).code == Err::PermissionDenied); CHECK(!t.valid());
  CHECK(r.gate.issue(ActionSource::System, t).code == Err::PermissionDenied);
  CHECK(r.gate.issue(ActionSource::Remote, t).code == Err::PermissionDenied);
  CHECK(r.s->start(UserStartToken(), b).code == Err::PermissionDenied);                 // no token at all (e.g. show load / restart / benchmark)
  CHECK(r.s->state() == BroadcastState::Off); CHECK(r.made.empty()); CHECK(r.secrets.gets == 0);
  CHECK(r.gate.issue(ActionSource::UI, t).ok() && t.valid());
  uint64_t nonce = t.nonce(); Status s = r.s->start(std::move(t), b); CHECK(s.ok()); CHECK(!t.valid());
  CHECK(r.s->state() == BroadcastState::Connecting || r.s->state() == BroadcastState::Live);
  r.s->stop();
  // replay of a spent nonce / expired token cannot start
  UserStartToken t2; r.gate.issue(ActionSource::Keyboard, t2); r.clk.advance(31 * kNsPerSec);
  CHECK(r.s->start(std::move(t2), b).code == Err::PermissionDenied); CHECK(r.s->state() == BroadcastState::Off);
  UserStartToken t3; r.gate.issue(ActionSource::Midi, t3); CHECK(t3.nonce() > nonce); CHECK(r.s->start(std::move(t3), b).ok()); r.s->stop();
  CHECK(UserActionGate::sourceMayStart(ActionSource::UI) && !UserActionGate::sourceMayStart(ActionSource::Show));
  // the session never auto-starts: a fresh object is Off regardless of anything persisted
  Rig fresh; CHECK(fresh.s->state() == BroadcastState::Off); fresh.step(1000, 50); CHECK(fresh.made.empty());
}

TEST(cast_state_machine_live_means_media_delivered) {
  Rig r; r.s->setHeaders(r.pkt(PacketKind::Headers, true, 40));
  CHECK(r.start(r.settings()).ok());
  CHECK(r.keyframeReqs == 1);
  std::vector<BroadcastState> want = {BroadcastState::Preparing, BroadcastState::Connecting}; CHECK(r.states == want);
  r.step(20); auto st = r.s->status(); CHECK(st.state == BroadcastState::Connecting); CHECK(st.connected); CHECK(!st.live); CHECK(st.text.find("waiting for media") != std::string::npos);   // socket/handshake is not "Live"
  CHECK(r.made.size() == 1 && r.made[0]->lastSecretSeen() == kSecret);                       // secret reached the transport, in memory
  CHECK(r.s->submit(r.pkt(PacketKind::Video, false, 100, 1)));                                // non-key before first key frame is discarded
  r.s->submit(r.pkt(PacketKind::Video, true, 500, 2)); r.s->submit(r.pkt(PacketKind::Audio, false, 60, 2)); r.step(20);
  st = r.s->status(); CHECK(st.state == BroadcastState::Live); CHECK(st.live && st.mediaFlowing); CHECK(st.packetsSent == 2); CHECK(st.text.find("not prove") != std::string::npos);
  auto got = r.made[0]->received(); CHECK(got.size() == 3); CHECK(got[0].kind == PacketKind::Headers); CHECK(got[1].kind == PacketKind::Video && got[1].key);   // headers first, then key frame
  r.step(1000, 3); st = r.s->status(); CHECK(st.encrypted && st.encryption.find("TLS") != std::string::npos); CHECK(st.endpoint == "rtmps://ingest.example.net:443/live");
  r.s->stop(); CHECK(r.s->state() == BroadcastState::Off); CHECK(r.made[0]->closed());
  want = {BroadcastState::Preparing, BroadcastState::Connecting, BroadcastState::Live, BroadcastState::Stopping, BroadcastState::Off}; CHECK(r.states == want);
  r.s->stop(); CHECK(r.states == want);                                                       // idempotent
}

TEST(cast_reconnect_schedule_and_deadline) {
  Rig r; r.prepare = [](FaultyTransport& t) { for (int i = 0; i < 100; ++i) t.script.push_back({ConnectOutcome::Kind::Fail, TransportErrorClass::Network, "connection refused"}); };
  // each new transport gets its own failing script
  CHECK(r.start(r.settings()).ok() || true);
  std::vector<Ns> attemptAt; size_t seen = 0; Ns t0 = r.clk.t; int64_t maxCountdown = 0;
  for (int i = 0; i < 1400 && r.s->state() != BroadcastState::Error; ++i) {
    r.step(100);
    if (r.made.size() > seen) { seen = r.made.size(); attemptAt.push_back(r.clk.t); }
    auto st = r.s->status(); if (st.state == BroadcastState::Reconnecting) maxCountdown = std::max(maxCountdown, st.reconnectInMs);
  }
  auto st = r.s->status();
  CHECK(st.state == BroadcastState::Error); CHECK(st.error.detail.find("120 s") != std::string::npos); CHECK(!st.errorTerminal);
  Ns total = r.clk.t - t0; CHECK(total >= 119 * kNsPerSec && total <= 123 * kNsPerSec);
  // delays between consecutive attempts follow 1,2,4,8,15,15,... within +-20% (+ one 100 ms tick)
  std::vector<double> nominal = {1, 2, 4, 8, 15, 15, 15, 15, 15, 15};
  CHECK(attemptAt.size() >= 9);
  Ns prev = attemptAt.empty() ? 0 : attemptAt[0]; std::set<Ns> distinct;
  for (size_t i = 1; i < attemptAt.size() && i <= nominal.size(); ++i) {
    double d = nsToSec(attemptAt[i] - prev); prev = attemptAt[i]; distinct.insert(attemptAt[i] - attemptAt[i - 1]);
    CHECK(d >= nominal[i - 1] * 0.8 - 0.01 && d <= nominal[i - 1] * 1.2 + 0.11);
  }
  CHECK(distinct.size() >= 3);                                                                // jitter actually varies
  CHECK(attemptAt.size() <= 14);                                                              // bounded: 1+2+4+8+15*... within 120 s
  CHECK(maxCountdown >= 1000 && maxCountdown <= 18000);                                       // UI countdown was exposed
  for (auto& l : r.logs) CHECK(!leaks(l));
  r.s->stop(); CHECK(r.s->state() == BroadcastState::Off);
}

TEST(cast_stop_during_reconnect_cancels_and_late_callbacks_ignored) {
  Rig r; r.capturing = true; r.prepare = [](FaultyTransport& t) { for (int i = 0; i < 50; ++i) t.script.push_back({ConnectOutcome::Kind::Fail, TransportErrorClass::Network, "reset"}); };
  CHECK(r.start(r.settings()).ok() || true);
  r.step(100, 5); CHECK(r.s->state() == BroadcastState::Reconnecting);
  auto* first = dynamic_cast<CapturingTransport*>(r.made[0]); auto staleCb = first->captured; CHECK((bool)staleCb);
  size_t madeBefore = r.made.size();
  auto t0 = std::chrono::steady_clock::now(); r.s->stop(); double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 1000.0); CHECK(r.s->state() == BroadcastState::Off);
  // stale callbacks from earlier attempts, even a "Connected" one, cannot restart or resurrect the stream
  TransportEvent c; c.type = TransportEvent::Type::Connected; staleCb(c);
  r.step(1000, 200);                                                                          // 200 s of ticks
  CHECK(r.s->state() == BroadcastState::Off); CHECK(r.made.size() == madeBefore);             // no further attempts
  CHECK(r.s->status().lateCallbacksIgnored >= 1);
  // a connection that succeeds but whose Disconnected callback arrives after stop() is ignored too
  Rig r2; r2.capturing = true; CHECK(r2.start(r2.settings()).ok()); r2.step(20); auto cb2 = dynamic_cast<CapturingTransport*>(r2.made[0])->captured;
  r2.s->stop(); TransportEvent dis; dis.type = TransportEvent::Type::Disconnected; dis.detail = "late"; cb2(dis); r2.step(1000, 30);
  CHECK(r2.s->state() == BroadcastState::Off); CHECK(r2.made.size() == 1);
}

TEST(cast_terminal_errors_are_not_retried) {
  struct Case { TransportErrorClass c; Err e; } cases[] = {{TransportErrorClass::Auth, Err::PermissionDenied}, {TransportErrorClass::Certificate, Err::TransportFailure}, {TransportErrorClass::InvalidConfig, Err::InvalidArgument}};
  for (auto& k : cases) {
    Rig r; auto cls = k.c; r.prepare = [cls](FaultyTransport& t) { t.script.push_back({ConnectOutcome::Kind::Fail, cls, std::string("handshake failed; rtmps://u:pw@ingest.example.net/live/") + kSecret + " key=" + kSecret}); };
    Status s = r.start(r.settings()); CHECK(!s.ok());
    auto st = r.s->status(); CHECK(st.state == BroadcastState::Error); CHECK(st.errorTerminal); CHECK(st.error.code == k.e);
    r.step(1000, 200); CHECK(r.made.size() == 1); CHECK(r.made[0]->connectCalls() == 1);   // never retried
    CHECK(!leaks(st.error.detail) && !leaks(st.text)); CHECK(st.error.detail.find("will not be retried") != std::string::npos);
    for (auto& l : r.logs) CHECK(!leaks(l));
    r.s->stop(); CHECK(r.s->state() == BroadcastState::Off);   // acknowledging the error
  }
  { Rig r; r.secrets.m.erase("key-main"); Status s = r.start(r.settings()); CHECK(!s.ok()); CHECK(r.s->state() == BroadcastState::Error); CHECK(s.detail.find("credential store") != std::string::npos); CHECK(s.detail.find("key-main") != std::string::npos); }
  { Rig r; r.secrets.fail = true; Status s = r.start(r.settings()); CHECK(!s.ok()); CHECK(!leaks(s.detail)); for (auto& l : r.logs) CHECK(!leaks(l)); }
  { Rig r; BroadcastSettings b = r.settings(); b.endpoint.host = "bad host"; Status s = r.start(b); CHECK(s.code == Err::InvalidArgument); CHECK(r.s->state() == BroadcastState::Error); CHECK(r.made.empty() && r.secrets.gets == 0); }
  { Rig r; BroadcastSettings b = r.settings(); b.reconnect = false; r.prepare = [](FaultyTransport& t) { t.script.push_back({ConnectOutcome::Kind::Fail, TransportErrorClass::Network, "refused"}); }; r.start(b); CHECK(r.s->state() == BroadcastState::Error); CHECK(!r.s->status().errorTerminal); }
}

TEST(cast_unsupported_transports_are_honest) {
  for (auto p : {BroadcastProtocol::RTMPS, BroadcastProtocol::RTMP, BroadcastProtocol::SRT}) {
    FakeClock clk; clk.t = kNsPerSec; FakeSecrets sec; sec.put("key-main", kSecret); sec.put("pass-srt", "x"); TransportFactory f;   // no override, no media worker
    BroadcastDeps d; d.secrets = &sec; d.clock = &clk; d.factory = &f; std::vector<std::string> logs; d.log = [&](const std::string& l) { logs.push_back(l); };
    BroadcastSession s(d); UserActionGate g(&clk); UserStartToken t; g.issue(ActionSource::UI, t);
    BroadcastSettings b = defaultBroadcastSettings(); b.endpoint.protocol = p; b.endpoint.host = "ingest.example.net"; b.endpoint.name = "x";
    if (p == BroadcastProtocol::SRT) b.endpoint.port = 9000; else { b.endpoint.path = "/live"; b.endpoint.streamKeyRef = "key-main"; }
    Status st = s.start(std::move(t), b);
    CHECK(st.code == Err::UnsupportedCapability); CHECK(st.detail.find("media worker") != std::string::npos); CHECK(st.detail.find("not bundled") != std::string::npos || st.detail.find("not available") != std::string::npos);
    CHECK(s.state() == BroadcastState::Error); CHECK(sec.gets == 0);       // never even read the key for an impossible transport
    CHECK(s.status().errorTerminal);
  }
  { // a registered media worker is honoured
    FakeClock clk; clk.t = kNsPerSec; FakeSecrets sec; sec.put("key-main", kSecret); TransportFactory f; int made = 0; f.registerMediaWorker(BroadcastProtocol::RTMPS, [&] { ++made; return std::unique_ptr<ITransport>(new FaultyTransport()); });
    BroadcastDeps d; d.secrets = &sec; d.clock = &clk; d.factory = &f; BroadcastSession s(d); UserActionGate g(&clk); UserStartToken t; g.issue(ActionSource::UI, t);
    BroadcastSettings b = defaultBroadcastSettings(); b.endpoint.host = "h.example.net"; b.endpoint.path = "/live"; b.endpoint.streamKeyRef = "key-main"; b.endpoint.name = "x";
    CHECK(s.start(std::move(t), b).ok()); CHECK(made >= 1); s.stop(); }
  { // encoder registry without H.264 is reported before any network activity
    Rig r; BroadcastDeps d; d.secrets = &r.secrets; d.clock = &r.clk; d.factory = &r.factory; d.encoders = &r.reg; BroadcastSession s(d);
    UserStartToken t; r.gate.issue(ActionSource::UI, t); Status st = s.start(std::move(t), r.settings()); CHECK(st.code == Err::EncoderUnavailable); CHECK(r.made.empty()); }
  { // RTP without a socket adapter
    FakeClock clk; clk.t = kNsPerSec; TransportFactory f; BroadcastSettings b = defaultBroadcastSettings(); b.endpoint.protocol = BroadcastProtocol::RTP; b.endpoint.host = "192.168.0.2"; b.endpoint.port = 5004; b.endpoint.path = ""; b.endpoint.streamKeyRef = ""; b.videoCodec = VideoCodec::MJPEG; b.width = 640; b.height = 480;
    std::unique_ptr<ITransport> t; Status st = f.create(b, t); CHECK(st.code == Err::UnsupportedCapability); CHECK(st.detail.find("socket") != std::string::npos); }
}

TEST(cast_reconnect_recovers_with_fresh_headers_and_keyframe) {
  Rig r; r.s->setHeaders(r.pkt(PacketKind::Headers, true, 40));
  r.prepare = [&r](FaultyTransport& t) { if (r.made.empty()) t.dropLinkAfterPackets = 3; };
  CHECK(r.start(r.settings()).ok()); r.step(20);
  r.s->submit(r.pkt(PacketKind::Video, true, 300, 1)); r.s->submit(r.pkt(PacketKind::Audio, false, 50, 1)); r.step(20);
  CHECK(r.s->state() == BroadcastState::Reconnecting || r.s->state() == BroadcastState::Live);   // headers + video + audio = 3 packets => link drops
  r.step(20); CHECK(r.s->state() == BroadcastState::Reconnecting);
  // packets submitted during the outage are not accumulated (bounded, no stale queue)
  for (int i = 0; i < 200; ++i) r.s->submit(r.pkt(PacketKind::Video, i % 30 == 0, 200, i));
  CHECK(r.s->status().queueDepth == 0); CHECK(r.s->status().droppedWhileOffline >= 200); CHECK(r.s->status().text.find("local recording unaffected") != std::string::npos);
  int reqBefore = r.keyframeReqs;
  r.step(100, 25);                                                                            // past the 1 s (+-20%) delay
  CHECK(r.made.size() == 2); CHECK(r.keyframeReqs > reqBefore);                               // fresh transport + key frame requested
  r.s->submit(r.pkt(PacketKind::Video, false, 200, 300)); r.step(20); CHECK(r.s->state() != BroadcastState::Live);   // P-frame alone cannot start the new stream
  r.s->submit(r.pkt(PacketKind::Video, true, 400, 301)); r.s->submit(r.pkt(PacketKind::Audio, false, 50, 301)); r.step(20);
  CHECK(r.s->state() == BroadcastState::Live); CHECK(r.s->status().reconnects == 1); CHECK(r.s->status().attempt == 0);
  auto got = r.made[1]->received(); CHECK(got.size() >= 2); CHECK(got[0].kind == PacketKind::Headers); CHECK(got[1].kind == PacketKind::Video && got[1].key);
  r.s->stop();
}

TEST(cast_packet_queue_bounded_and_gop_aware) {
  PacketQueueLimits lim; lim.maxPackets = 40; lim.maxBytes = 1u << 20; PacketQueue q(lim);
  uint64_t seq = 0; auto mk = [&](PacketKind k, bool key) { auto p = std::make_shared<MediaPacket>(); p->kind = k; p->key = key; p->data.assign(1000, 1); p->seq = seq++; return MediaPacketPtr(p); };
  for (int g = 0; g < 50; ++g) { q.push(mk(PacketKind::Video, true)); for (int i = 0; i < 9; ++i) { q.push(mk(i % 2 ? PacketKind::Audio : PacketKind::Video, false)); } }
  auto st = q.stats(); CHECK(st.depth <= 40); CHECK(st.dropped > 400); CHECK(st.gopsDropped > 0); CHECK(st.highWater <= 40);
  auto first = q.peek(); CHECK(first && first->kind == PacketKind::Video && first->key);        // stream stays decodable after drops
  // byte budget
  PacketQueueLimits l2; l2.maxPackets = 10000; l2.maxBytes = 50000; PacketQueue q2(l2);
  for (int i = 0; i < 1000; ++i) q2.push(mk(PacketKind::Video, i % 10 == 0)); CHECK(q2.stats().bytes <= 50000);
  // multiple consumers are independent
  PacketFanout fan; size_t a = fan.addConsumer("net", lim), b = fan.addConsumer("preview", lim);
  for (int i = 0; i < 30; ++i) fan.publish(mk(PacketKind::Video, i % 5 == 0));
  while (fan.consumer(a).peek()) fan.consumer(a).popFront();                                  // one consumer drains, the other stalls
  CHECK(fan.consumer(a).stats().depth == 0); CHECK(fan.consumer(b).stats().depth == 30);
  for (int i = 0; i < 300; ++i) fan.publish(mk(PacketKind::Video, i % 5 == 0)); CHECK(fan.consumer(b).stats().depth <= 40); CHECK(fan.consumer(a).stats().depth <= 40);
  // through the session with a stalled transport
  Rig r; r.prepare = [](FaultyTransport& t) { t.acceptLimit = 0; }; CHECK(r.start(r.settings()).ok()); r.step(20);
  for (int i = 0; i < 5000; ++i) r.s->submit(r.pkt(PacketKind::Video, i % 30 == 0, 2000, i)); r.step(20);
  auto ss_ = r.s->status(); CHECK(ss_.queueDepth <= 512); CHECK(ss_.packetsDropped > 4000); CHECK(ss_.state == BroadcastState::Connecting);
  bool warned = false; for (auto& w : ss_.warnings) if (w.find("Upload cannot keep up") != std::string::npos) warned = true; CHECK(warned);
  r.s->stop();
}

TEST(cast_secret_never_in_any_string) {
  Rig r; std::vector<std::string> strings;
  auto grab = [&] { auto st = r.s->status(); strings.push_back(st.text); strings.push_back(st.error.detail); strings.push_back(st.endpoint); strings.push_back(st.encryption); for (auto& w : st.warnings) strings.push_back(w); };
  r.prepare = [](FaultyTransport& t) {
    t.script.push_back({ConnectOutcome::Kind::Fail, TransportErrorClass::Network, std::string("connect rtmps://pub:pw@ingest.example.net:443/live/") + kSecret + "?token=" + kSecret + " failed streamkey=" + kSecret});
    t.script.push_back({ConnectOutcome::Kind::Succeed, TransportErrorClass::Network, ""});
    t.breakDetail = std::string("write to rtmps://ingest.example.net/live/") + kSecret + " reset; Authorization: Bearer " + kSecret;
    t.dropLinkAfterPackets = 5;
  };
  BroadcastSettings b = r.settings(); std::string summary = BroadcastSession::summary(b, PublicationInputs());
  r.s->setHeaders(r.pkt(PacketKind::Headers, true, 40));
  CHECK(r.start(b).ok() || true); grab();
  for (int i = 0; i < 400; ++i) { r.step(100); if (i % 7 == 0) { r.s->submit(r.pkt(PacketKind::Video, i % 3 == 0, 300, i)); r.s->submit(r.pkt(PacketKind::Audio, false, 40, i)); } grab(); }
  strings.push_back(summary); strings.push_back(exportSettingsJson(b)); strings.push_back(cast::redactText(std::string("x ") + kSecret)); 
  for (auto& l : r.logs) strings.push_back(l);
  r.s->stop(); grab();
  // the secret DID reach the transport (otherwise this scan would prove nothing)
  bool reached = false; for (auto* t : r.made) if (t->lastSecretSeen() == kSecret) reached = true; CHECK(reached);
  size_t scanned = 0; for (auto& s : strings) { ++scanned; if (leaks(s)) { std::printf("   LEAK in: %s\n", s.c_str()); CHECK(false); } }
  CHECK(scanned > 100); CHECK(!r.logs.empty());
  // redactor behaviour
  CHECK(cast::redactUrl("rtmps://user:pw@host:443/live/abcdef?x=1#f") == "rtmps://host:443/live/<redacted>?<redacted>");
  CHECK(cast::redactText("see srt://h:9000?passphrase=hunter22&x=1 now").find("hunter22") == std::string::npos);
  CHECK(cast::redactText("password=abc123 other").find("abc123") == std::string::npos);
  CHECK(cast::redactText("plain text without secrets") == "plain text without secrets");
  CHECK(cast::redactWithSecret("raw tokenvalue-99999 appears", "tokenvalue-99999").find("tokenvalue") == std::string::npos);
}

TEST(cast_local_recording_unaffected_by_receiver_outage) {
  Rig r; r.prepare = [](FaultyTransport& t) { for (int i = 0; i < 100; ++i) t.script.push_back({ConnectOutcome::Kind::Fail, TransportErrorClass::Network, "unreachable"}); };
  MemFileSystem fs; FakeDiskInfo disk; disk.free = 50ull << 30; FakeClock rclk; rclk.wall = 1767225600000LL;
  SessionDeps d; d.fs = &fs; d.disk = &disk; d.clock = &rclk; d.manualPump = true; RecordingSession rec(d);
  RecordingRequest q; q.dir = "rec"; q.sessionId = "s"; q.showName = "x"; q.sources.audioPresent = true; q.sources.audioRate = 48000; q.sources.audioChannels = 2; q.profile.driftCorrection = false;
  CHECK(rec.start(q).ok());
  r.start(r.settings());
  for (int k = 0; k < 3000; ++k) {                                                            // 30 s outage
    auto b = std::make_shared<AudioBlock>(); b->rate = 48000; b->channels = 2; b->firstSample = k * 480ull; b->captureNs = rec::samplesToNs(k * 480ull, 48000) + kNsPerSec; b->flags = AF_TimestampValid; b->samples.assign(960, 0.1f);
    rclk.t = b->captureNs; rec.pushAudio(b); rec.pump(); r.step(10);
  }
  CHECK(r.s->state() == BroadcastState::Reconnecting); CHECK(rec.state() == RecState::Recording); CHECK(rec.status().audioFramesWritten == 3000 * 480ull); CHECK(rec.status().gapCount == 0);
  r.s->stop(); CHECK(rec.stop().ok()); CHECK(rec.state() == RecState::Completed);
}

int main() { return ss_test::runAll(); }
