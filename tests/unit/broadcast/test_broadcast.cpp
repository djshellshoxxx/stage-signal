// Broadcast tests (T-CAST, T-FAULT). Fast and deterministic: no real ffmpeg, no network, no threads, no sleeping.
// Every test name starts with the REQ id it covers.
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "doctest/doctest.h"
#include "nlohmann/json.hpp"
#include "ss/base/rng.hpp"
#include "ss/base/time.hpp"
#include "ss/broadcast/command.hpp"
#include "ss/broadcast/ffmpeg_io.hpp"
#include "ss/broadcast/process.hpp"
#include "ss/broadcast/redact.hpp"
#include "ss/broadcast/session.hpp"
#include "ss/broadcast/settings.hpp"

using namespace ss;
using namespace ss::broadcast;

namespace {

const char* kKey = "live_abc123SECRETKEY";
const char* kPass = "correct horse battery";

struct MapSecrets final : ISecretResolver {
    std::map<std::string, std::string> m{{"ref:stream-key", kKey}, {"ref:srt-pass", kPass}};
    bool exists(const SecretRef& r) const override { return m.count(r.handle) != 0; }
    Result<std::string> resolve(const SecretRef& r) const override {
        auto it = m.find(r.handle);
        if (it == m.end()) return makeError(ErrorCategory::PermissionDenied, "missing");
        return it->second;
    }
};

BroadcastSettings rtmps() {
    BroadcastSettings s;
    s.profile = Profile::Rtmps;
    s.host = "live.example.com";
    s.path = "live";
    s.streamKey.handle = "ref:stream-key";
    return s;
}
BroadcastSettings rtmp() {
    BroadcastSettings s = rtmps();
    s.profile = Profile::Rtmp;
    s.acknowledgeUnencrypted = true;
    return s;
}
BroadcastSettings srt(bool pass = true) {
    BroadcastSettings s;
    s.profile = Profile::SrtCaller;
    s.host = "srt.example.com";
    s.port = 9000;
    if (pass) s.srtPassphrase.handle = "ref:srt-pass"; else s.acknowledgeUnencrypted = true;
    return s;
}

BroadcastCommandInput mediaFor(const BroadcastSettings& s) {
    BroadcastCommandInput in;
    in.settings = s;
    in.encoder.name = "h264_nvenc";
    in.encoder.codec = record::VideoCodec::H264;
    in.encoder.hardware = true;
    in.video.endpoint = "/run/ss/video.fifo";
    in.video.width = s.video.width; in.video.height = s.video.height; in.video.fpsNum = s.video.fps; in.video.fpsDen = 1;
    in.audio.endpoint = "/run/ss/audio.fifo";
    in.audio.rateHz = s.audio.rateHz; in.audio.channels = s.audio.channels;
    return in;
}

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool adjacent(const std::vector<std::string>& v, const std::string& a, const std::string& b) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i) if (v[i] == a && v[i + 1] == b) return true;
    return false;
}
bool fieldIssue(const std::vector<SettingIssue>& v, const std::string& f, bool blocking = true) {
    return std::any_of(v.begin(), v.end(), [&](const SettingIssue& i) { return i.field == f && i.blocking == blocking; });
}
bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

PublicationFacts facts(bool camera = false) {
    PublicationFacts f;
    f.cameraPublished = camera;
    f.audioSourceLabel = "Master bus (after limiter)";
    f.programOutputLabel = "Output 1 - Main";
    f.recordingActive = true;
    return f;
}

struct Rig {
    FakeClock clock{secToNs(1000)};
    Rng rng;
    FakeProcessFactory procs;
    FakeFeed feed;
    MapSecrets secrets;
    BroadcastSession session;
    BroadcastSettings settings;
    std::uint64_t total = 0, outUs = 0;

    explicit Rig(BroadcastSettings s = rtmps(), std::uint64_t seed = 7, SessionConfig cfg = {})
        : rng(seed), session(s, clock, rng, procs, feed, secrets, cfg), settings(s) {}

    Status startNow(ActionSource src = ActionSource::Ui) {
        auto t = session.confirmPublication(src, facts());
        if (!t.ok()) return t.error();
        return session.start(t.value(), facts(), mediaFor(settings));
    }
    FakeProcessData& proc() { return *procs.last(); }
    void advanceTick(Ns d) { clock.advance(d); session.tick(); }
    void goLive() {
        REQUIRE(startNow().ok());
        proc().emitProgress(1000, 100000);
        proc().emitProgress(5000, 200000);
        total = 5000; outUs = 200000;
        session.tick();
        REQUIRE(session.state() == State::Live);
    }
    void feedLive(int seconds) {                     // healthy traffic, one progress sample per second
        for (int i = 0; i < seconds; ++i) {
            clock.advance(secToNs(1));
            total += 50000; outUs += 1000000;
            proc().emitProgress(total, outUs);
            session.tick();
        }
    }
    void crash(const std::string& text = "Connection to tcp://203.0.113.9:443 failed: Connection refused\n") {
        proc().crash(1, text);
        session.tick();
    }
    // Waits for the scheduled reconnect and returns the delay that was announced.
    double reconnect() {
        REQUIRE(session.state() == State::Reconnecting);
        const Ns d = session.status().nextAttemptIn;
        clock.advance(d);
        session.tick();
        return nsToSec(d);
    }
};

}  // namespace

// ================================================================================================================
// REQ-B01: profiles, settings, command lines
// ================================================================================================================

TEST_CASE("REQ-B01 defaults are 720p30, 2.5 Mb/s, AAC stereo 128 kb/s, 2 s keyframes") {
    const BroadcastSettings d = defaultSettings();
    CHECK(d.video.width == 1280);
    CHECK(d.video.height == 720);
    CHECK(d.video.fps == 30);
    CHECK(d.video.bitrateKbps == 2500);
    CHECK(d.video.keyframeSeconds == 2);
    CHECK(d.audio.rateHz == 48000);
    CHECK(d.audio.channels == 2);
    CHECK(d.audio.bitrateKbps == 128);
    CHECK(d.profile == Profile::Rtmps);
    CHECK(d.reconnectEnabled);
    CHECK_FALSE(d.acknowledgeUnencrypted);
}

TEST_CASE("REQ-B01 profile availability: RTMPS and SRT beta, RTMP optional, RTP later, WebRTC deferred") {
    CHECK(availability(Profile::Rtmps) == Availability::Beta);
    CHECK(availability(Profile::SrtCaller) == Availability::Beta);
    CHECK(availability(Profile::Rtmp) == Availability::Optional);
    CHECK(availability(Profile::Rtp) == Availability::Later);
    CHECK(availability(Profile::WebRtc) == Availability::Deferred);
}

TEST_CASE("REQ-B01 a complete RTMPS, RTMP and SRT configuration validates without blocking issues") {
    CHECK_FALSE(hasBlocking(validateSettings(rtmps())));
    CHECK_FALSE(hasBlocking(validateSettings(rtmp())));
    CHECK_FALSE(hasBlocking(validateSettings(srt(true))));
    CHECK_FALSE(hasBlocking(validateSettings(srt(false))));
}

TEST_CASE("REQ-B01 host, port, path and secret fields are validated") {
    auto s = rtmps();
    s.host = "rtmps://live.example.com/live";
    CHECK(fieldIssue(validateSettings(s), "host"));
    s = rtmps(); s.host = "user@live.example.com";
    CHECK(fieldIssue(validateSettings(s), "host"));
    s = rtmps(); s.host = "";
    CHECK(fieldIssue(validateSettings(s), "host"));
    s = rtmps(); s.host = "[2001:db8::1]";
    CHECK_FALSE(fieldIssue(validateSettings(s), "host"));
    s = rtmps(); s.port = 70000;
    CHECK(fieldIssue(validateSettings(s), "port"));
    s = rtmps(); s.path = "../etc";
    CHECK(fieldIssue(validateSettings(s), "path"));
    s = rtmps(); s.path = "";
    CHECK(fieldIssue(validateSettings(s), "path"));
    s = rtmps(); s.path = "live2/app";
    CHECK_FALSE(fieldIssue(validateSettings(s), "path"));
    s = rtmps(); s.streamKey.handle = kKey;                      // a pasted key instead of a reference
    CHECK(fieldIssue(validateSettings(s), "streamKey"));
    s = rtmps(); s.streamKey.handle = "";
    CHECK(fieldIssue(validateSettings(s), "streamKey"));
    s = srt(); s.port = 0;
    CHECK(fieldIssue(validateSettings(s), "port"));
    s = srt(); s.srtPassphrase.handle = "plain passphrase";
    CHECK(fieldIssue(validateSettings(s), "srtPassphrase"));
}

TEST_CASE("REQ-B01 encoder and transport ranges are validated") {
    auto s = rtmps();
    s.video.width = 1281;
    CHECK(fieldIssue(validateSettings(s), "video.size"));
    s = rtmps(); s.video.fps = 120;
    CHECK(fieldIssue(validateSettings(s), "video.fps"));
    s = rtmps(); s.video.bitrateKbps = 100;
    CHECK(fieldIssue(validateSettings(s), "video.bitrate"));
    s = rtmps(); s.video.keyframeSeconds = 0;
    CHECK(fieldIssue(validateSettings(s), "video.keyframe"));
    s = rtmps(); s.audio.bitrateKbps = 8;
    CHECK(fieldIssue(validateSettings(s), "audio.bitrate"));
    s = rtmps(); s.audio.rateHz = 22050;
    CHECK(fieldIssue(validateSettings(s), "audio.rate"));
    s = srt(); s.srtLatencyMs = 5;
    CHECK(fieldIssue(validateSettings(s), "srtLatencyMs"));
    s = srt(); s.srtKeyLenBytes = 20;
    CHECK(fieldIssue(validateSettings(s), "srtKeyLenBytes"));
}

TEST_CASE("REQ-B01 unencrypted transports need acknowledgement and are labelled") {
    auto s = rtmp();
    s.acknowledgeUnencrypted = false;
    CHECK(fieldIssue(validateSettings(s), "acknowledgeUnencrypted"));
    CHECK(encryptionLabel(rtmp()).find("NOT encrypted") != std::string::npos);
    CHECK(encryptionState(rtmp()) == EncryptionState::Unencrypted);

    auto u = srt(false);
    u.acknowledgeUnencrypted = false;
    CHECK(fieldIssue(validateSettings(u), "acknowledgeUnencrypted"));
    CHECK(encryptionLabel(srt(false)).find("NOT encrypted") != std::string::npos);
    CHECK(encryptionState(srt(true)) == EncryptionState::SrtEncrypted);
    CHECK(encryptionLabel(srt(true)).find("AES-128") != std::string::npos);

    CHECK(encryptionState(rtmps()) == EncryptionState::TlsVerified);
    CHECK(encryptionLabel(rtmps()).find("certificate") != std::string::npos);
}

TEST_CASE("REQ-B01 BASE tier fallbacks 800x600 / 640x480 at 15-25 FPS apply only when the receiver accepts them") {
    const auto s = rtmps();
    CHECK(fieldIssue(validateSettings(s, true), "video.size", false));       // BASE advises, never blocks
    CHECK_FALSE(hasBlocking(validateSettings(s, true)));

    auto refused = applyBaseFallback(s, true, 15, false);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().category == ErrorCategory::UnsupportedCapability);

    auto a = applyBaseFallback(s, true, 15, true);
    REQUIRE(a.ok());
    CHECK(a.value().video.width == 640);
    CHECK(a.value().video.height == 480);
    CHECK(a.value().video.fps == 15);
    CHECK(a.value().video.bitrateKbps <= 1200);
    CHECK_FALSE(fieldIssue(validateSettings(a.value(), true), "video.size", false));

    auto b = applyBaseFallback(s, false, 25, true);
    REQUIRE(b.ok());
    CHECK(b.value().video.width == 800);
    CHECK(b.value().video.height == 600);

    CHECK_FALSE(applyBaseFallback(s, false, 30, true).ok());                 // 30 FPS is not a BASE fallback
}

TEST_CASE("REQ-B01 upload capacity warning below the stream bitrate") {
    const auto s = rtmps();
    CHECK(requiredUploadKbps(s) > s.video.bitrateKbps + s.audio.bitrateKbps);
    CHECK(uploadWarning(s, 10000).empty());
    const std::string w = uploadWarning(s, 1500);
    CHECK_FALSE(w.empty());
    CHECK(w.find("Recording is not affected") != std::string::npos);
}

TEST_CASE("REQ-B01 RTMPS command: certificate and host validation forced on, FLV, H.264 + AAC, 2 s keyframes") {
    MapSecrets sec;
    auto cmd = FfmpegCommandBuilder::buildBroadcast(mediaFor(rtmps()));
    REQUIRE(cmd.ok());
    auto argv = cmd.value().materialize(sec);
    REQUIRE(argv.ok());
    const auto& a = argv.value();
    CHECK(a[0] == "ffmpeg");
    CHECK(adjacent(a, "-tls_verify", "1"));
    CHECK(adjacent(a, "-verifyhost", "live.example.com"));
    CHECK(adjacent(a, "-f", "flv"));
    CHECK(adjacent(a, "-c:v", "h264_nvenc"));
    CHECK(adjacent(a, "-b:v", "2500k"));
    CHECK(adjacent(a, "-g", "60"));
    CHECK(adjacent(a, "-force_key_frames", "expr:gte(t,n_forced*2)"));
    CHECK(adjacent(a, "-c:a", "aac"));
    CHECK(adjacent(a, "-b:a", "128k"));
    CHECK(adjacent(a, "-ar", "48000"));
    CHECK(adjacent(a, "-progress", "pipe:1"));
    CHECK(a.back() == std::string("rtmps://live.example.com:443/live/") + kKey);
    CHECK_FALSE(has(a, "libx264"));
    // tls_verify must come before the output URL it applies to
    const auto tls = std::find(a.begin(), a.end(), "-tls_verify") - a.begin();
    CHECK(tls < static_cast<long>(a.size()) - 1);
    CHECK(cmd.value().container == "flv");
}

TEST_CASE("REQ-B01 RTMP command is plain rtmp:// without TLS options and says so") {
    MapSecrets sec;
    auto cmd = FfmpegCommandBuilder::buildBroadcast(mediaFor(rtmp()));
    REQUIRE(cmd.ok());
    auto argv = cmd.value().materialize(sec);
    REQUIRE(argv.ok());
    CHECK_FALSE(has(argv.value(), "-tls_verify"));
    CHECK(argv.value().back().rfind("rtmp://live.example.com:1935/live/", 0) == 0);
    CHECK(adjacent(argv.value(), "-f", "flv"));
}

TEST_CASE("REQ-B01 SRT caller command: mpegts, latency in microseconds, passphrase option, key length, enforced encryption") {
    MapSecrets sec;
    auto cmd = FfmpegCommandBuilder::buildBroadcast(mediaFor(srt(true)));
    REQUIRE(cmd.ok());
    auto argv = cmd.value().materialize(sec);
    REQUIRE(argv.ok());
    const auto& a = argv.value();
    CHECK(adjacent(a, "-mode", "caller"));
    CHECK(adjacent(a, "-latency", "200000"));
    CHECK(adjacent(a, "-connect_timeout", "3000"));
    CHECK(adjacent(a, "-passphrase", kPass));
    CHECK(adjacent(a, "-pbkeylen", "16"));
    CHECK(adjacent(a, "-enforced_encryption", "1"));
    CHECK(adjacent(a, "-f", "mpegts"));
    CHECK(a.back() == "srt://srt.example.com:9000");
    CHECK(cmd.value().container == "mpegts");

    auto plain = FfmpegCommandBuilder::buildBroadcast(mediaFor(srt(false)));
    REQUIRE(plain.ok());
    auto pa = plain.value().materialize(sec);
    REQUIRE(pa.ok());
    CHECK_FALSE(has(pa.value(), "-passphrase"));
    CHECK_FALSE(plain.value().containsSecrets());
}

TEST_CASE("REQ-B01 SRT passphrase outside 10..64 characters is rejected when the command is materialised") {
    MapSecrets sec;
    sec.m["ref:srt-pass"] = "short";
    auto cmd = FfmpegCommandBuilder::buildBroadcast(mediaFor(srt(true)));
    REQUIRE(cmd.ok());
    CHECK_FALSE(cmd.value().materialize(sec).ok());
    sec.m["ref:srt-pass"] = std::string(65, 'x');
    CHECK_FALSE(cmd.value().materialize(sec).ok());
    sec.m["ref:srt-pass"] = "line\nbreak is not ok";
    CHECK_FALSE(cmd.value().materialize(sec).ok());
}

TEST_CASE("REQ-B01 the builder refuses MJPEG, libx264, mismatched inputs and unsupported profiles") {
    auto in = mediaFor(rtmps());
    in.encoder.name = "mjpeg"; in.encoder.codec = record::VideoCodec::Mjpeg;
    auto r = FfmpegCommandBuilder::buildBroadcast(in);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::EncoderUnavailable);

    in = mediaFor(rtmps());
    in.encoder.name = "libx264";
    CHECK_FALSE(FfmpegCommandBuilder::buildBroadcast(in).ok());

    in = mediaFor(rtmps());
    in.video.width = 1920; in.video.height = 1080;
    CHECK_FALSE(FfmpegCommandBuilder::buildBroadcast(in).ok());

    in = mediaFor(rtmps());
    in.audio.channels = 1;
    CHECK_FALSE(FfmpegCommandBuilder::buildBroadcast(in).ok());

    in = mediaFor(rtmps());
    in.video.endpoint = "rtmp://evil/x";                      // inputs must be local pipes, never URLs
    CHECK_FALSE(FfmpegCommandBuilder::buildBroadcast(in).ok());

    auto s = rtmps(); s.host = "-evil";
    CHECK_FALSE(FfmpegCommandBuilder::buildBroadcast(mediaFor(s)).ok());
}

TEST_CASE("REQ-B01 broadcast shares the record input pieces: audio delay compensation shifts the video input offset") {
    auto in = mediaFor(rtmps());
    in.sync.videoStartHostNs = secToNs(5);
    in.sync.audioStartHostNs = secToNs(5);
    in.sync.audioDelayCompNs = msToNs(5);                        // processed audio arrives 5 ms late relative to capture
    auto cmd = FfmpegCommandBuilder::buildBroadcast(in);
    REQUIRE(cmd.ok());
    CHECK(cmd.value().offsets.video == msToNs(5));
    CHECK(cmd.value().offsets.audio == 0);
    MapSecrets sec;
    auto argv = cmd.value().materialize(sec).value();
    CHECK(adjacent(argv, "-itsoffset", "0.005000"));
    CHECK(adjacent(argv, "-itsoffset", "0.000000"));
}

TEST_CASE("REQ-B01 publication summary lists destination, protection, camera and audio source; hash binds the facts") {
    const auto s = rtmps();
    auto a = makeSummary(s, facts(false));
    auto b = makeSummary(s, facts(true));
    CHECK(a.text().find("live.example.com") != std::string::npos);
    CHECK(a.text().find("certificate") != std::string::npos);
    CHECK(a.text().find("Master bus") != std::string::npos);
    CHECK(a.text().find("Camera: not included") != std::string::npos);
    CHECK(b.text().find("INCLUDED") != std::string::npos);
    CHECK(a.hash() != b.hash());
    CHECK(a.hash() == makeSummary(s, facts(false)).hash());
    CHECK(a.hash().size() == 64);
}

TEST_CASE("REQ-B01 settings round trip locally with references and export without them") {
    auto s = srt(true);
    s.name = "Venue SRT";
    s.srtLatencyMs = 400;
    auto back = parseSettingsJson(serializeSettingsJson(s));
    REQUIRE(back.ok());
    CHECK(back.value().host == s.host);
    CHECK(back.value().profile == Profile::SrtCaller);
    CHECK(back.value().srtLatencyMs == 400);
    CHECK(back.value().srtPassphrase.handle == "ref:srt-pass");
    CHECK(back.value().video.bitrateKbps == 2500);

    CHECK_FALSE(parseSettingsJson("not json").ok());
    CHECK_FALSE(parseSettingsJson("{\"profile\": 99}").ok());
}

// ================================================================================================================
// REQ-B02: a stream starts only from an explicit user action
// ================================================================================================================

TEST_CASE("REQ-B02 show load, restart, benchmark and remote sources can never produce a start token") {
    Rig r;
    for (ActionSource src : {ActionSource::Show, ActionSource::System, ActionSource::Remote}) {
        auto t = r.session.confirmPublication(src, facts());
        CHECK_FALSE(t.ok());
        CHECK(t.error().category == ErrorCategory::PermissionDenied);
    }
    CHECK(r.session.state() == State::Off);
    CHECK(r.procs.created.empty());
    CHECK_FALSE(r.feed.active);
}

TEST_CASE("REQ-B02 start without a confirmation token is refused and spawns nothing") {
    Rig r;
    auto st = r.session.start(StartToken{}, facts(), mediaFor(r.settings));
    CHECK_FALSE(st.ok());
    CHECK(r.session.state() == State::Off);
    CHECK(r.procs.created.empty());
}

TEST_CASE("REQ-B02 UI, keyboard and MIDI confirmations may start a stream") {
    for (ActionSource src : {ActionSource::Ui, ActionSource::Keyboard, ActionSource::Midi}) {
        Rig r;
        REQUIRE(r.startNow(src).ok());
        CHECK(r.session.state() == State::Connecting);
        CHECK(r.procs.created.size() == 1);
    }
}

TEST_CASE("REQ-B02 a token is single use") {
    Rig r;
    auto t = r.session.confirmPublication(ActionSource::Ui, facts());
    REQUIRE(t.ok());
    REQUIRE(r.session.start(t.value(), facts(), mediaFor(r.settings)).ok());
    r.session.stop();
    REQUIRE(r.session.state() == State::Off);
    CHECK_FALSE(r.session.start(t.value(), facts(), mediaFor(r.settings)).ok());
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("REQ-B02 a token expires") {
    Rig r;
    auto t = r.session.confirmPublication(ActionSource::Ui, facts());
    REQUIRE(t.ok());
    r.clock.advance(secToNs(16));
    CHECK_FALSE(r.session.start(t.value(), facts(), mediaFor(r.settings)).ok());
    CHECK(r.procs.created.empty());
    CHECK(r.session.state() == State::Off);
}

TEST_CASE("REQ-B02 a token is bound to the summary the operator saw") {
    Rig r;
    auto t = r.session.confirmPublication(ActionSource::Ui, facts(false));
    REQUIRE(t.ok());
    // The camera was switched on after the confirmation dialog: the old confirmation no longer applies.
    CHECK_FALSE(r.session.start(t.value(), facts(true), mediaFor(r.settings)).ok());
    CHECK(r.procs.created.empty());

    auto t2 = r.session.confirmPublication(ActionSource::Ui, facts(false));
    REQUIRE(t2.ok());
    auto changed = r.settings;
    changed.video.bitrateKbps = 4000;
    r.session.updateSettings(changed);
    CHECK_FALSE(r.session.start(t2.value(), facts(false), mediaFor(changed)).ok());
    CHECK(r.procs.created.empty());
}

TEST_CASE("REQ-B02 a newer confirmation replaces the older token; invalid settings give no token") {
    Rig r;
    auto t1 = r.session.confirmPublication(ActionSource::Ui, facts());
    auto t2 = r.session.confirmPublication(ActionSource::Ui, facts());
    REQUIRE(t1.ok());
    REQUIRE(t2.ok());
    CHECK_FALSE(r.session.start(t1.value(), facts(), mediaFor(r.settings)).ok());
    CHECK(r.session.start(t2.value(), facts(), mediaFor(r.settings)).ok());

    auto bad = rtmps();
    bad.host = "";
    Rig r2(bad);
    CHECK_FALSE(r2.session.confirmPublication(ActionSource::Ui, facts()).ok());
}

TEST_CASE("REQ-B02 a running stream cannot be started a second time") {
    Rig r;
    REQUIRE(r.startNow().ok());
    CHECK_FALSE(r.startNow().ok());
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("REQ-B02 isUserAction accepts only UI, keyboard and MIDI") {
    CHECK(isUserAction(ActionSource::Ui));
    CHECK(isUserAction(ActionSource::Keyboard));
    CHECK(isUserAction(ActionSource::Midi));
    CHECK_FALSE(isUserAction(ActionSource::Show));
    CHECK_FALSE(isUserAction(ActionSource::System));
    CHECK_FALSE(isUserAction(ActionSource::Remote));
}

// ================================================================================================================
// REQ-B03: secrets and redaction
// ================================================================================================================

TEST_CASE("REQ-B03 redaction removes URL userinfo, path, query and known secrets") {
    CHECK(redactUrl("rtmps://user:pw@live.example.com:443/live/KEY123?token=abc") == "rtmps://live.example.com:443/<redacted>?<redacted>");
    CHECK(redactUrl("srt://host:9000") == "srt://host:9000");
    CHECK(redactUrl("not a url") == "not a url");
    const std::string t = redactText("Error opening output rtmps://h.example.com/live/SECRETKEY99: Input/output error", {});
    CHECK(t.find("SECRETKEY99") == std::string::npos);
    CHECK(t.find("h.example.com") != std::string::npos);
    CHECK(t.find("Input/output error") != std::string::npos);
    CHECK(redactText("the passphrase is hunter2hunter2", {"hunter2hunter2"}).find("hunter2") == std::string::npos);
}

TEST_CASE("REQ-B03 portable exports contain no secret, no reference and no key material") {
    auto s = rtmps();
    const std::string e = exportSettingsJson(s);
    CHECK(e.find("ref:") == std::string::npos);
    CHECK(e.find(kKey) == std::string::npos);
    CHECK(e.find("streamKeySet") != std::string::npos);
    CHECK(e.find("live.example.com") != std::string::npos);
    CHECK(e.find("RTMPS") != std::string::npos);

    auto p = srt(true);
    const std::string pe = exportSettingsJson(p);
    CHECK(pe.find("ref:") == std::string::npos);
    CHECK(pe.find(kPass) == std::string::npos);
    CHECK(pe.find("AES-128") != std::string::npos);
}

TEST_CASE("REQ-B03 the displayed command line never contains the stream key or the passphrase") {
    auto c = FfmpegCommandBuilder::buildBroadcast(mediaFor(rtmps()));
    REQUIRE(c.ok());
    const std::string line = c.value().displayLine();
    CHECK(line.find("SECRETKEY") == std::string::npos);
    CHECK(line.find("live.example.com") != std::string::npos);
    CHECK(c.value().containsSecrets());

    auto s = FfmpegCommandBuilder::buildBroadcast(mediaFor(srt(true)));
    REQUIRE(s.ok());
    const std::string sl = s.value().displayLine();
    CHECK(sl.find("correct horse") == std::string::npos);
    CHECK(sl.find("<secret>") != std::string::npos);
}

TEST_CASE("REQ-B03 ffmpeg errors that echo the output URL are redacted in status and log") {
    Rig r;
    REQUIRE(r.startNow().ok());
    r.crash(std::string("[rtmps @ 0x1] Cannot open connection tls://live.example.com:443\nError opening output rtmps://live.example.com:443/live/") +
            kKey + ": Input/output error\n");
    const auto st = r.session.status();
    REQUIRE_FALSE(st.errors.empty());
    for (const auto& e : st.errors) CHECK(e.message.find("SECRETKEY") == std::string::npos);
    CHECK_FALSE(anyContains(r.session.log(), "SECRETKEY"));
    CHECK_FALSE(anyContains(r.session.log(), kPass));
    CHECK(st.connection.find("SECRETKEY") == std::string::npos);
}

TEST_CASE("REQ-B03 the process receives the resolved key; log and status never do") {
    Rig r;
    REQUIRE(r.startNow().ok());
    REQUIRE(r.procs.created.size() == 1);
    CHECK(r.proc().argv.back() == std::string("rtmps://live.example.com:443/live/") + kKey);   // spawn is the only place it exists
    CHECK_FALSE(anyContains(r.session.log(), "SECRETKEY"));
    CHECK(anyContains(r.session.log(), "command:"));
}

TEST_CASE("REQ-B03 status reports secret handling as PARTIAL and says why (argv exposure is not claimed solved)") {
    Rig r;
    const auto st = r.session.status();
    CHECK(st.secrets.level == SecretHandling::Partial);
    CHECK(st.secrets.note.find("PARTIAL") != std::string::npos);
    CHECK(st.secrets.note.find("command line") != std::string::npos);
    CHECK(st.secrets.note.find("not solved") != std::string::npos);
}

TEST_CASE("REQ-B03 a missing secret blocks the start with an actionable message and spawns nothing") {
    Rig r;
    r.secrets.m.erase("ref:stream-key");
    auto t = r.session.confirmPublication(ActionSource::Ui, facts());
    REQUIRE(t.ok());
    auto st = r.session.start(t.value(), facts(), mediaFor(r.settings));
    REQUIRE_FALSE(st.ok());
    CHECK(r.procs.created.empty());
    CHECK(r.session.state() == State::Error);
    CHECK(st.error().detail.find("secret store") != std::string::npos);
}

TEST_CASE("REQ-B03 a secret that disappears before a reconnect ends the stream instead of retrying blindly") {
    Rig r;
    r.goLive();
    r.crash();
    REQUIRE(r.session.state() == State::Reconnecting);
    r.secrets.m.erase("ref:stream-key");
    r.reconnect();
    CHECK(r.session.state() == State::Error);
    CHECK(r.procs.created.size() == 1);
}

// ================================================================================================================
// REQ-B04: Live semantics, reconnect, terminal errors
// ================================================================================================================

TEST_CASE("REQ-B04 Live is claimed only after outbound media is observed, not when the process starts") {
    Rig r;
    REQUIRE(r.startNow().ok());
    CHECK(r.session.state() == State::Connecting);
    r.proc().emitProgress(0, 0);                                  // ffmpeg's first block: nothing written yet
    r.session.tick();
    CHECK(r.session.state() == State::Connecting);
    r.proc().emitProgress(1000, 100000);                          // one sample with growth is not enough
    r.session.tick();
    CHECK(r.session.state() == State::Connecting);
    r.proc().emitProgress(9000, 300000);
    r.session.tick();
    CHECK(r.session.state() == State::Live);
    CHECK(r.session.status().connection == "Live");
}

TEST_CASE("REQ-B04 the state sequence is Off, Preparing, Connecting, Live") {
    Rig r;
    r.goLive();
    CHECK(anyContains(r.session.log(), "Off -> Preparing"));
    CHECK(anyContains(r.session.log(), "Preparing -> Connecting"));
    CHECK(anyContains(r.session.log(), "Connecting -> Live"));
}

TEST_CASE("REQ-B04 status reports measured bitrate, bytes, drops and the encryption state") {
    Rig r;
    REQUIRE(r.startNow().ok());
    r.proc().emitProgress(10000, 1000000);
    r.session.tick();
    r.clock.advance(secToNs(1));
    r.proc().emitProgress(135000, 2000000, 3);                    // +125000 bytes in one second = 1000 kb/s
    r.session.tick();
    REQUIRE(r.session.state() == State::Live);
    r.feed.dropped = 4;
    const auto st = r.session.status();
    CHECK(st.bitrateKbps == doctest::Approx(1000.0).epsilon(0.01));
    CHECK(st.bytesSent == 135000);
    CHECK(st.droppedFrames == 7);                                 // 3 reported by ffmpeg + 4 dropped by the feed
    CHECK(st.encryption == EncryptionState::TlsVerified);
    CHECK(st.encryptionText.find("encrypted") != std::string::npos);
    CHECK(st.errors.empty());
}

TEST_CASE("REQ-B04 reconnect delays follow 1, 2, 4, 8, 15 s with +-20% jitter and stop at the 120 s deadline") {
    Rig r;
    const Ns t0 = r.clock.nowMonotonic();
    REQUIRE(r.startNow().ok());
    std::vector<double> delays;
    for (int i = 0; i < 40 && r.session.state() != State::Error; ++i) {
        r.crash();
        if (r.session.state() != State::Reconnecting) break;
        delays.push_back(r.reconnect());
    }
    REQUIRE(delays.size() >= 8);
    const double base[] = {1, 2, 4, 8, 15, 15, 15, 15};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(delays[i] >= base[i] * 0.8 - 1e-9);
        CHECK(delays[i] <= base[i] * 1.2 + 1e-9);
    }
    CHECK(r.session.state() == State::Error);
    const double elapsed = nsToSec(r.clock.nowMonotonic() - t0);
    CHECK(elapsed >= 120.0);
    CHECK(elapsed < 140.0);
    CHECK(r.feed.abandons >= static_cast<int>(delays.size()));    // queued media dropped at each reconnect
    CHECK(r.feed.keyframes >= static_cast<int>(delays.size()));   // and a fresh keyframe requested
    CHECK_FALSE(r.feed.active);
    const auto before = r.procs.created.size();
    r.advanceTick(secToNs(500));
    CHECK(r.procs.created.size() == before);                      // no retries after the deadline
    CHECK(r.session.status().connection.find("120 s") != std::string::npos);
}

TEST_CASE("REQ-B04 jitter comes from the injected rng: same seed, same delays; different seed, different delays") {
    auto run = [](std::uint64_t seed) {
        Rig r(rtmps(), seed);
        std::vector<double> d;
        REQUIRE(r.startNow().ok());
        for (int i = 0; i < 4; ++i) { r.crash(); d.push_back(r.reconnect()); }
        return d;
    };
    CHECK(run(11) == run(11));
    CHECK(run(11) != run(12));
}

TEST_CASE("REQ-B04 every reconnect attempt starts a new process with a fresh queue and keyframe") {
    Rig r;
    r.goLive();
    const int abandons = r.feed.abandons, keys = r.feed.keyframes;
    r.crash();
    CHECK(r.session.state() == State::Reconnecting);
    CHECK(r.feed.abandons > abandons);
    CHECK(r.feed.keyframes > keys);
    r.reconnect();
    CHECK(r.session.state() == State::Connecting);
    CHECK(r.procs.created.size() == 2);
    CHECK(r.session.status().totalReconnects == 1);
    r.proc().emitProgress(500, 50000);
    r.proc().emitProgress(9000, 150000);
    r.session.tick();
    CHECK(r.session.state() == State::Live);
}

TEST_CASE("REQ-B04 certificate, authentication and configuration failures are terminal and never retried") {
    const std::vector<std::string> terminal = {
        "[tls @ 0x1] Peer certificate failed verification\n[rtmps @ 0x1] Cannot open connection tls://live.example.com:443\n",
        "[rtmp @ 0x1] Server error: NetStream.Publish.BadName\n",
        "HTTP error 403 Forbidden\n",
        "Unrecognized option 'bogus_opt'.\nError splitting the argument list: Option not found\n",
        "[flv @ 0x1] Video codec mjpeg not compatible with flv\n"};
    for (const auto& text : terminal) {
        INFO(text);
        Rig r;
        r.goLive();
        r.crash(text);
        CHECK(r.session.state() == State::Error);
        const auto st = r.session.status();
        REQUIRE_FALSE(st.errors.empty());
        CHECK(st.errors.front().terminal);
        r.advanceTick(secToNs(600));
        CHECK(r.session.state() == State::Error);
        CHECK(r.procs.created.size() == 1);
        CHECK_FALSE(r.feed.active);
    }
}

TEST_CASE("REQ-B04 retryable failures are retried: refused, DNS, timeout, reset, unknown") {
    const std::vector<std::string> retry = {
        "[tcp @ 0x1] Connection to tcp://127.0.0.1:1935 failed: Connection refused\n",
        "[tcp @ 0x1] Failed to resolve hostname live.example.com: Name or service not known\n",
        "[tcp @ 0x1] Connection to tcp://203.0.113.5:1935 failed: Connection timed out\n",
        "av_interleaved_write_frame(): Broken pipe\n",
        "something odd happened: error 17\n"};
    for (const auto& text : retry) {
        INFO(text);
        Rig r;
        r.goLive();
        r.crash(text);
        CHECK(r.session.state() == State::Reconnecting);
        REQUIRE_FALSE(r.session.status().errors.empty());
        CHECK_FALSE(r.session.status().errors.front().terminal);
    }
}

TEST_CASE("REQ-B04 classifier matches real ffmpeg 6.1 messages") {
    CHECK(classifyStderr("[tls @ 0x55] Peer certificate failed verification\n").kind == FailureKind::Certificate);
    CHECK(classifyStderr("[tcp @ 0x55] Connection to tcp://127.0.0.1:19461?tcp_nodelay=0 failed: Connection refused\n").kind == FailureKind::Refused);
    CHECK(classifyStderr("[tcp @ 0x55] Failed to resolve hostname no-such-host.invalid: Name or service not known\n").kind == FailureKind::Dns);
    CHECK(classifyStderr("[rtmps @ 0x55] Cannot read RTMP handshake response\n").kind == FailureKind::ConnectionLost);
    CHECK(classifyStderr("[srt @ 0x55] Connection to srt://127.0.0.1:19462 failed: Input/output error\n").kind == FailureKind::ConnectionLost);
    CHECK(classifyStderr("Unrecognized option 'bogus_opt'.\n").kind == FailureKind::InvalidConfig);
    CHECK(classifyStderr("").kind == FailureKind::None);
    CHECK(classifyStderr("frame=1 fps=30\n").kind == FailureKind::None);
    CHECK(classifyExit(8, "").kind == FailureKind::InvalidConfig);
    CHECK(classifyExit(0, "").kind == FailureKind::None);
    CHECK(classifyExit(137, "").kind == FailureKind::Unknown);
    CHECK(isTerminal(FailureKind::Certificate));
    CHECK(isTerminal(FailureKind::Auth));
    CHECK(isTerminal(FailureKind::InvalidConfig));
    CHECK_FALSE(isTerminal(FailureKind::Refused));
    CHECK_FALSE(isTerminal(FailureKind::Unknown));
    // A certificate failure wins over the generic "cannot open" lines around it.
    CHECK(classifyStderr("[tls @ 1] Peer certificate failed verification\n[rtmps @ 1] Cannot open connection tls://h:443\nError opening output: Input/output error\n").kind ==
          FailureKind::Certificate);
}

TEST_CASE("REQ-B04 with reconnect disabled a lost connection ends in Error") {
    auto s = rtmps();
    s.reconnectEnabled = false;
    Rig r(s);
    r.goLive();
    r.crash();
    CHECK(r.session.state() == State::Error);
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("REQ-B04 a stable connection clears the outage; a flapping one keeps escalating the delay") {
    {   // stable for >= 30 s: next outage starts again at 1 s
        Rig r;
        r.goLive();
        r.crash();
        r.reconnect();
        r.crash();
        const double second = nsToSec(r.session.status().nextAttemptIn);
        CHECK(second >= 1.6);                                     // second failure of one outage: 2 s base
        r.reconnect();
        r.proc().emitProgress(500, 50000);
        r.proc().emitProgress(9000, 150000);
        r.total = 9000; r.outUs = 150000;
        r.session.tick();
        REQUIRE(r.session.state() == State::Live);
        r.feedLive(31);
        REQUIRE(r.session.state() == State::Live);
        r.crash();
        CHECK(nsToSec(r.session.status().nextAttemptIn) <= 1.2 + 1e-9);
        CHECK(r.session.status().reconnectAttempts == 0);
    }
    {   // Live for only 5 s: the outage clock keeps running
        Rig r;
        r.goLive();
        r.crash();
        r.reconnect();
        r.proc().emitProgress(500, 50000);
        r.proc().emitProgress(9000, 150000);
        r.total = 9000; r.outUs = 150000;
        r.session.tick();
        REQUIRE(r.session.state() == State::Live);
        r.feedLive(5);
        r.crash();
        CHECK(nsToSec(r.session.status().nextAttemptIn) >= 1.6);
    }
}

TEST_CASE("REQ-B04 a Live stream whose bytes stop growing is treated as lost (stall)") {
    Rig r;
    r.goLive();
    r.advanceTick(secToNs(9));
    CHECK(r.session.state() == State::Live);
    r.advanceTick(secToNs(2));
    CHECK(r.session.state() == State::Reconnecting);
    REQUIRE_FALSE(r.session.status().errors.empty());
    CHECK(r.session.status().errors.front().kind == FailureKind::Stalled);
    CHECK(r.procs.created[0]->kills == 1);
}

TEST_CASE("REQ-B04 connecting longer than the connect timeout counts as a failure and kills the process") {
    Rig r;
    REQUIRE(r.startNow().ok());
    r.advanceTick(secToNs(19));
    CHECK(r.session.state() == State::Connecting);
    r.advanceTick(secToNs(2));
    CHECK(r.session.state() == State::Reconnecting);
    CHECK(r.procs.created[0]->kills == 1);
    CHECK(r.session.status().errors.front().kind == FailureKind::Timeout);
}

TEST_CASE("REQ-B04 a process that exits cleanly without being asked to is treated as a lost connection") {
    Rig r;
    r.goLive();
    r.proc().alive = false;
    r.proc().code = 0;
    r.session.tick();
    CHECK(r.session.state() == State::Reconnecting);
}

TEST_CASE("REQ-B04 ffmpeg progress parser handles chunked input, N/A values and the end block") {
    ProgressParser p;
    auto a = p.feed("frame=0\nfps=0.00\nbitrate=N/A\ntotal_size=0\nout_time_us=N/A\ndup_frames=0\ndrop_frames=0\nspeed=N/A\nprogress=continue\nfra");
    REQUIRE(a.size() == 1);
    CHECK(a[0].outTime == -1);
    CHECK(a[0].bitrateKbps < 0);
    CHECK_FALSE(a[0].end);
    auto b = p.feed("me=10\nbitrate= 102.8kbits/s\ntotal_size=23124\nout_time_us=1800000\ndrop_frames=2\nprogress=end\n");
    REQUIRE(b.size() == 1);
    CHECK(b[0].frame == 10);
    CHECK(b[0].totalSize == 23124);
    CHECK(b[0].outTime == 1800000 * 1000LL);
    CHECK(b[0].bitrateKbps == doctest::Approx(102.8));
    CHECK(b[0].dropFrames == 2);
    CHECK(b[0].end);
    CHECK(p.feed("garbage without newline").empty());
}

// ================================================================================================================
// REQ-B05: Stop within 1 s, stale callbacks cannot restart
// ================================================================================================================

TEST_CASE("REQ-B05 Stop with a cooperative encoder reaches Off immediately") {
    Rig r;
    r.goLive();
    r.session.stop();
    CHECK(r.session.state() == State::Off);
    CHECK(r.proc().stopRequests == 1);
    CHECK(r.proc().kills == 0);
    CHECK_FALSE(r.feed.active);
}

TEST_CASE("REQ-B05 Stop with a hung encoder is forced Off inside one second") {
    Rig r;
    r.procs.nextExitOnStop = false;
    r.goLive();
    const Ns t0 = r.clock.nowMonotonic();
    r.session.stop();
    CHECK(r.session.state() == State::Stopping);
    r.advanceTick(msToNs(500));
    CHECK(r.session.state() == State::Stopping);
    CHECK(r.proc().kills == 0);
    r.advanceTick(msToNs(300));
    CHECK(r.session.state() == State::Off);
    CHECK(r.proc().kills == 1);
    CHECK(r.clock.nowMonotonic() - t0 <= secToNs(1));
}

TEST_CASE("REQ-B05 Stop while Reconnecting cancels the pending retry at once") {
    Rig r;
    r.goLive();
    r.crash();
    REQUIRE(r.session.state() == State::Reconnecting);
    r.session.stop();
    CHECK(r.session.state() == State::Off);
    r.advanceTick(secToNs(1000));
    CHECK(r.session.state() == State::Off);
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("REQ-B05 a late reconnect timer from before Stop cannot restart the stream") {
    Rig r;
    r.goLive();
    r.crash();
    REQUIRE(r.session.state() == State::Reconnecting);
    const auto gen = r.session.generation();
    r.session.stop();
    r.session.onReconnectTimer(gen);                              // the timer callback was already in flight
    CHECK(r.session.state() == State::Off);
    CHECK(r.procs.created.size() == 1);
    CHECK(r.session.generation() > gen);
}

TEST_CASE("REQ-B05 a late exit report from a previous process generation is ignored") {
    Rig r;
    r.goLive();
    const auto oldGen = r.session.generation();
    r.crash();
    r.reconnect();                                                // new process, new generation
    REQUIRE(r.session.state() == State::Connecting);
    CHECK(r.session.generation() > oldGen);
    r.session.onProcessExited(oldGen, 1, "Connection refused");   // stale callback
    CHECK(r.session.state() == State::Connecting);
    CHECK(r.procs.created.size() == 2);
    CHECK(anyContains(r.session.log(), "ignored stale"));

    r.session.onProcessExited(r.session.generation(), 1, "Connection refused");   // current generation is honoured
    CHECK(r.session.state() == State::Reconnecting);
}

TEST_CASE("REQ-B05 after Stop a stale exit report does not resurrect or retry") {
    Rig r;
    r.goLive();
    const auto gen = r.session.generation();
    r.session.stop();
    r.session.onProcessExited(gen, 1, "Connection refused");
    r.session.onReconnectTimer(gen);
    CHECK(r.session.state() == State::Off);
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("REQ-B05 Stop is idempotent, clears Error, and a new start needs a new confirmation") {
    Rig r;
    r.session.stop();
    CHECK(r.session.state() == State::Off);
    r.goLive();
    r.crash("Peer certificate failed verification\n");
    REQUIRE(r.session.state() == State::Error);
    r.session.stop();
    CHECK(r.session.state() == State::Off);
    r.session.stop();
    CHECK(r.session.state() == State::Off);
    CHECK_FALSE(r.session.start(StartToken{}, facts(), mediaFor(r.settings)).ok());
    CHECK(r.startNow().ok());
}

// ================================================================================================================
// REQ-B06: offline operation and recording independence
// ================================================================================================================

TEST_CASE("REQ-B06 starting while offline keeps retrying visibly and never blocks anything else") {
    Rig r;
    REQUIRE(r.startNow().ok());
    r.crash("[tcp @ 0x1] Failed to resolve hostname live.example.com: Name or service not known\n");
    CHECK(r.session.state() == State::Reconnecting);
    const auto st = r.session.status();
    CHECK(st.connection.find("Reconnecting") != std::string::npos);
    CHECK(st.errors.front().kind == FailureKind::Dns);
    CHECK(st.nextAttemptIn > 0);
    // The network comes back: the next attempt reaches Live.
    r.reconnect();
    r.proc().emitProgress(500, 50000);
    r.proc().emitProgress(9000, 150000);
    r.session.tick();
    CHECK(r.session.state() == State::Live);
}

TEST_CASE("REQ-B06 a stalled network never stalls the disk consumer of the shared fan-out") {
    record::SinkFanout fan(64u << 20, 4096);                      // disk queue large, network queue tiny
    FanoutFeed feed(fan);
    FakeClock clock(secToNs(1000));
    Rng rng(3);
    FakeProcessFactory procs;
    MapSecrets secrets;
    BroadcastSession s(rtmps(), clock, rng, procs, feed, secrets);
    auto t = s.confirmPublication(ActionSource::Ui, facts());
    REQUIRE(t.ok());
    REQUIRE(s.start(t.value(), facts(), mediaFor(rtmps())).ok());
    CHECK(fan.networkEnabled());

    // 1000 chunks are published; nothing ever drains the network queue (the broadcast process is stuck).
    for (std::uint64_t i = 0; i < 1000; ++i) {
        record::MediaChunk c;
        c.seq = i; c.ptsNs = static_cast<Ns>(i) * msToNs(33); c.durationNs = msToNs(33);
        c.data = std::make_shared<const std::vector<std::uint8_t>>(1000, std::uint8_t{1});
        fan.publish(c);
    }
    std::size_t diskGot = 0;
    record::MediaChunk out;
    while (fan.disk().pop(out)) ++diskGot;
    CHECK(diskGot == 1000);                                       // recording lost nothing
    CHECK(fan.disk().stats().dropped == 0);
    CHECK(fan.network().stats().dropped > 0);                     // only the network queue overflowed
    CHECK(fan.network().stats().depthBytes <= 4096);
    CHECK(s.status().droppedFrames == fan.network().stats().dropped);

    s.stop();
    CHECK_FALSE(fan.networkEnabled());                            // stopping the stream stops queueing for it
    record::MediaChunk more;
    more.data = std::make_shared<const std::vector<std::uint8_t>>(1000, std::uint8_t{1});
    fan.publish(more);
    CHECK(fan.disk().pop(out));                                   // the recording still receives media
}

TEST_CASE("REQ-B06 a terminal broadcast error does not touch the recording path") {
    record::SinkFanout fan(1u << 20, 1u << 16);
    FanoutFeed feed(fan);
    FakeClock clock(secToNs(1000));
    Rng rng(3);
    FakeProcessFactory procs;
    MapSecrets secrets;
    BroadcastSession s(rtmps(), clock, rng, procs, feed, secrets);
    auto t = s.confirmPublication(ActionSource::Keyboard, facts());
    REQUIRE(s.start(t.value(), facts(), mediaFor(rtmps())).ok());
    procs.last()->crash(1, "Peer certificate failed verification\n");
    s.tick();
    REQUIRE(s.state() == State::Error);
    CHECK_FALSE(fan.networkEnabled());
    record::MediaChunk c;
    c.data = std::make_shared<const std::vector<std::uint8_t>>(100, std::uint8_t{1});
    fan.publish(c);
    record::MediaChunk out;
    CHECK(fan.disk().pop(out));
    CHECK(fan.network().size() == 0);
}

// ================================================================================================================
// REQ-B07: RTP later, WebRTC deferred
// ================================================================================================================

TEST_CASE("REQ-B07 RTP and WebRTC are documented but unavailable and cannot be started") {
    for (Profile p : {Profile::Rtp, Profile::WebRtc}) {
        auto s = rtmps();
        s.profile = p;
        CHECK(fieldIssue(validateSettings(s), "profile"));
        auto cmd = FfmpegCommandBuilder::buildBroadcast(mediaFor(s));
        REQUIRE_FALSE(cmd.ok());
        CHECK(cmd.error().category == ErrorCategory::UnsupportedCapability);
        Rig r(s);
        CHECK_FALSE(r.session.confirmPublication(ActionSource::Ui, facts()).ok());
        CHECK(std::string(availabilityNote(p)).size() > 20);
    }
    CHECK(std::string(availabilityNote(Profile::Rtp)).find("unencrypted") != std::string::npos);
    CHECK(std::string(availabilityNote(Profile::WebRtc)).find("out of scope") != std::string::npos);
}

// ================================================================================================================
// T-FAULT: process start failure, crash and retry through the fake process
// ================================================================================================================

TEST_CASE("T-FAULT ffmpeg missing or not startable is a terminal operator error, not a retry loop") {
    Rig r;
    r.procs.nextFailsToStart = true;
    auto st = r.startNow();
    CHECK_FALSE(st.ok());
    CHECK(r.session.state() == State::Error);
    CHECK(r.procs.created.size() == 1);
    CHECK_FALSE(r.feed.active);
    r.advanceTick(secToNs(300));
    CHECK(r.procs.created.size() == 1);
}

TEST_CASE("T-FAULT encoder crash mid-stream then successful retry; counters accumulate across processes") {
    Rig r;
    REQUIRE(r.startNow().ok());
    r.proc().emitProgress(1000, 100000, 2);
    r.proc().emitProgress(90000, 200000, 5);
    r.session.tick();
    REQUIRE(r.session.state() == State::Live);
    r.crash("[out#0/flv @ 0x1] Error muxing a packet\nav_interleaved_write_frame(): Broken pipe\n");
    REQUIRE(r.session.state() == State::Reconnecting);
    r.reconnect();
    r.proc().emitProgress(1000, 100000, 1);
    r.proc().emitProgress(20000, 200000, 1);
    r.session.tick();
    REQUIRE(r.session.state() == State::Live);
    const auto st = r.session.status();
    CHECK(st.droppedFrames == 6);                                 // 5 from the first process + 1 from the second
    CHECK(st.bytesSent == 90000 + 20000);
    CHECK(st.totalReconnects == 1);
    CHECK(st.errors.size() == 1);
}

TEST_CASE("T-FAULT the status error list is bounded") {
    Rig r;
    REQUIRE(r.startNow().ok());
    for (int i = 0; i < 6; ++i) {
        r.crash();
        if (r.session.state() != State::Reconnecting) break;
        r.reconnect();
    }
    CHECK(r.session.status().errors.size() <= 10);
    CHECK(r.session.log().size() <= 500);
}

TEST_CASE("T-FAULT the fake process records a stop request and a kill") {
    FakeProcessFactory f;
    f.nextExitOnStop = false;
    auto p = f.create();
    REQUIRE(p->start({"ffmpeg"}).ok());
    CHECK(p->running());
    p->requestStop();
    CHECK(p->running());
    p->kill();
    CHECK_FALSE(p->running());
    CHECK(p->exitCode() == 137);
    CHECK(f.last()->stopRequests == 1);
    CHECK(f.last()->kills == 1);
}
