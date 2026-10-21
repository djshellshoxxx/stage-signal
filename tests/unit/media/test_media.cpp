#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "doctest/doctest.h"
#include "media_fixtures.hpp"
#include "ss/base/sha256.hpp"
#include "ss/media/manifest.hpp"
#include "ss/media/probe.hpp"
#include "ss/media/validate.hpp"
#include "ss/media/worker.hpp"

using namespace ss;
using namespace ss::media;

// ---------------------------------------------------------------------------------------------------------------
// Header sniffing (REQ-S07, section 13/16)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-S07: sniff accepts the declared allowlist by magic bytes") {
    CHECK(sniff(fx::png(10, 10)).format == Format::Png);
    CHECK(sniff(fx::jpeg(10, 10)).format == Format::Jpeg);
    CHECK(sniff(fx::bmp(10, 10)).format == Format::Bmp);
    CHECK(sniff(fx::gif(10, 10, {5})).format == Format::Gif);
    CHECK(sniff(fx::mp4("isom")).format == Format::Mp4);
    CHECK(sniff(fx::mp4("mp42")).format == Format::Mp4);
    CHECK(sniff(fx::mp4("qt  ")).format == Format::Mov);
    CHECK(sniff(fx::mkv()).format == Format::Mkv);
}

TEST_CASE("REQ-S07: sniff rejects SVG, HTML, script and unknown content") {
    for (const char* bad : {"<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>",
                            "<?xml version='1.0'?><svg/>", "  \n<!DOCTYPE html><html></html>", "<html><body>",
                            "\xEF\xBB\xBF<script>x</script>", "var a = 1; <svg onload=x>", "javascript:alert(1)"}) {
        const auto r = sniff(std::string(bad));
        CHECK_FALSE(r.ok());
        CHECK(r.reject == SniffReject::ActiveContent);
    }
    CHECK(sniff(std::string("plain text file, nothing special")).reject == SniffReject::UnknownSignature);
    CHECK(sniff(std::string("abc")).reject == SniffReject::TooShort);
    CHECK(sniff(fx::mp4("heic")).reject == SniffReject::UnsupportedBrand);
    CHECK(sniff(fx::mkv("webm")).reject == SniffReject::UnsupportedBrand);
    CHECK(sniff(std::string("BMxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")).reject == SniffReject::UnknownSignature);
}

TEST_CASE("REQ-S07: a renamed file is judged by content, never by its declared name") {
    const auto svg = sniff(std::string("<svg></svg>"));
    CHECK_FALSE(checkDeclaredFormat("png", svg).ok());                       // "evil.png" that is really SVG
    CHECK(checkDeclaredFormat("png", sniff(fx::png(4, 4))).ok());
    CHECK_FALSE(checkDeclaredFormat("png", sniff(fx::jpeg(4, 4))).ok());     // jpeg bytes declared png
    CHECK_FALSE(checkDeclaredFormat("svg", sniff(fx::png(4, 4))).ok());      // svg is not an allowed declaration
    CHECK(checkDeclaredFormat("jpg", sniff(fx::jpeg(4, 4))).ok());
}

TEST_CASE("REQ-S07: image headers give dimensions without decoding pixels") {
    auto p = fx::png(640, 480);
    auto h = readImageHeader(reinterpret_cast<const std::uint8_t*>(p.data()), p.size(), Format::Png);
    REQUIRE(h.ok()); CHECK(h.value().width == 640); CHECK(h.value().height == 480);
    auto j = fx::jpeg(1920, 1080);
    auto hj = readImageHeader(reinterpret_cast<const std::uint8_t*>(j.data()), j.size(), Format::Jpeg);
    REQUIRE(hj.ok()); CHECK(hj.value().width == 1920); CHECK(hj.value().height == 1080);
    auto b = fx::bmp(320, -240);   // negative height = top-down
    auto hb = readImageHeader(reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), Format::Bmp);
    REQUIRE(hb.ok()); CHECK(hb.value().width == 320); CHECK(hb.value().height == 240);
    auto huge = fx::png(0xFFFFFFFFu, 0xFFFFFFFFu);
    auto hh = readImageHeader(reinterpret_cast<const std::uint8_t*>(huge.data()), huge.size(), Format::Png);
    REQUIRE(hh.ok());                                              // parse is fine; the validator rejects it
    CHECK_FALSE(validateStill(hh.value().width, hh.value().height, Tier::Standard).ok());
    auto zero = fx::png(0, 10);
    CHECK_FALSE(readImageHeader(reinterpret_cast<const std::uint8_t*>(zero.data()), zero.size(), Format::Png).ok());
    CHECK_FALSE(readImageHeader(reinterpret_cast<const std::uint8_t*>(p.data()), 10, Format::Png).ok());   // truncated
    std::string jt = fx::jpeg(10, 10).substr(0, 24);
    CHECK_FALSE(readImageHeader(reinterpret_cast<const std::uint8_t*>(jt.data()), jt.size(), Format::Jpeg).ok());
}

// ---------------------------------------------------------------------------------------------------------------
// Still limits (validators run before allocation)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-S07: still limits 4096 per side and 64 MiB decoded, checked from numbers only") {
    CHECK(validateStill(4096, 4096, Tier::Standard).ok());                   // exactly 64 MiB RGBA
    CHECK(validateStill(4096, 4096, Tier::Standard).value().decodedBytes == (64ull << 20));
    CHECK_FALSE(validateStill(4097, 100, Tier::Standard).ok());
    CHECK_FALSE(validateStill(100, 4097, Tier::Enhanced).ok());
    CHECK_FALSE(validateStill(0, 100, Tier::Standard).ok());
    CHECK_FALSE(validateStill(4294967295u, 4294967295u, Tier::Enhanced).ok());   // no overflow, no allocation
    CHECK(validateStill(4097, 100, Tier::Standard).error().category == ErrorCategory::ResourceExhausted);
}

TEST_CASE("REQ-S07: BASE stills fit 800x600 (preprocessed off the live path) and share an 8 MiB cache") {
    auto small = validateStill(800, 600, Tier::Base);
    REQUIRE(small.ok());
    CHECK_FALSE(small.value().needsDownscale);
    CHECK(small.value().decodedBytes == 800u * 600u * 4u);
    auto big = validateStill(4000, 3000, Tier::Base);
    REQUIRE(big.ok());
    CHECK(big.value().needsDownscale);
    CHECK(big.value().targetWidth == 800);
    CHECK(big.value().targetHeight == 600);
    auto tall = validateStill(1000, 4000, Tier::Base);
    REQUIRE(tall.ok());
    CHECK(tall.value().targetHeight == 600);
    CHECK(tall.value().targetWidth == 150);
    auto cache = DecodedCacheBudget::forTier(Tier::Base);
    int admitted = 0;
    while (true) {
        auto p = validateStill(800, 600, Tier::Base, &cache);
        if (!p.ok()) break;
        REQUIRE(cache.tryReserve(p.value().decodedBytes));
        ++admitted;
    }
    CHECK(admitted == 4);                                       // 4 x 1.92 MB = 7.68 MB <= 8 MiB, a 5th does not fit
    CHECK(cache.used() <= Limits::kBaseDecodedCacheBytes);
    CHECK(validateStill(800, 600, Tier::Base, &cache).error().category == ErrorCategory::ResourceExhausted);
    // Higher tiers have no 8 MiB cache
    auto hi = DecodedCacheBudget::forTier(Tier::Standard);
    CHECK(hi.tryReserve(60ull << 20));
}

// ---------------------------------------------------------------------------------------------------------------
// GIF: frames, timing, speed, media clock
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-S07: GIF parser reads frames, delays and loop count from container blocks") {
    auto g = fx::gif(64, 48, {10, 0, 1, 20}, 3);
    auto info = parseGif(reinterpret_cast<const std::uint8_t*>(g.data()), g.size());
    REQUIRE(info.ok());
    CHECK(info.value().width == 64); CHECK(info.value().height == 48);
    REQUIRE(info.value().frames.size() == 4);
    CHECK(info.value().frames[0].rawDelayMs == 100);
    CHECK(info.value().frames[1].rawDelayMs == 0);
    CHECK(info.value().frames[2].rawDelayMs == 10);
    CHECK(info.value().loopCount == 3);
    auto none = fx::gif(8, 8, {5});
    CHECK(parseGif(reinterpret_cast<const std::uint8_t*>(none.data()), none.size()).value().loopCount == -1);
    auto trunc = g.substr(0, g.size() - 7);
    CHECK_FALSE(parseGif(reinterpret_cast<const std::uint8_t*>(trunc.data()), 10).ok());
}

TEST_CASE("REQ-S07: GIF frame count is capped at 1000 before anything is allocated per frame") {
    auto ok1000 = fx::gif(8, 8, std::vector<int>(1000, 5));
    auto i1 = parseGif(reinterpret_cast<const std::uint8_t*>(ok1000.data()), ok1000.size());
    REQUIRE(i1.ok());
    CHECK(i1.value().frames.size() == 1000);
    CHECK_FALSE(i1.value().frameCapExceeded);
    CHECK(validateGif(i1.value(), Tier::Standard).ok());
    auto too = fx::gif(8, 8, std::vector<int>(1001, 5));
    auto i2 = parseGif(reinterpret_cast<const std::uint8_t*>(too.data()), too.size());
    REQUIRE(i2.ok());
    CHECK(i2.value().frames.size() <= 1000);                    // metadata growth is bounded
    CHECK(i2.value().frameCapExceeded);
    auto r = validateGif(i2.value(), Tier::Enhanced);
    CHECK_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::ResourceExhausted);
}

TEST_CASE("REQ-S07: GIF streamed cache is bounded to 64 MiB and BASE shares the 8 MiB cache") {
    GifInfo info;
    info.frames.assign(500, GifFrameInfo{});
    info.width = 1024; info.height = 1024;                      // 4 MiB per frame -> 16 frames fit in 64 MiB
    auto p = validateGif(info, Tier::Standard);
    REQUIRE(p.ok());
    CHECK(p.value().cacheFrames == 16);
    CHECK(p.value().cacheBytes <= Limits::kGifStreamCacheBytes);
    info.width = 4096; info.height = 4096;                      // 64 MiB per frame: cannot hold 3 working frames
    CHECK_FALSE(validateGif(info, Tier::Enhanced).ok());
    GifInfo base;
    base.width = 800; base.height = 600; base.frames.assign(50, GifFrameInfo{});
    auto b = validateGif(base, Tier::Base);
    REQUIRE(b.ok());
    CHECK(b.value().cacheBytes <= Limits::kBaseDecodedCacheBytes);
    CHECK(b.value().cacheFrames == 4);
    base.width = 801;
    CHECK_FALSE(validateGif(base, Tier::Base).ok());
    DecodedCacheBudget busy(Limits::kBaseDecodedCacheBytes);
    REQUIRE(busy.tryReserve(Limits::kBaseDecodedCacheBytes - 1000));
    base.width = 100; base.height = 100;
    CHECK_FALSE(validateGif(base, Tier::Base, &busy).ok());
}

TEST_CASE("REQ-S07: zero and sub-20 ms GIF delays clamp to 20 ms with a warning") {
    const auto t = normalizeGifDelays({0, 10, 19, 20, 21, 100});
    REQUIRE(t.delaysMs.size() == 6);
    CHECK(t.delaysMs[0] == 20); CHECK(t.delaysMs[1] == 20); CHECK(t.delaysMs[2] == 20);
    CHECK(t.delaysMs[3] == 20); CHECK(t.delaysMs[4] == 21); CHECK(t.delaysMs[5] == 100);
    CHECK(t.clampedCount == 3);
    REQUIRE(t.warnings.size() == 1);
    CHECK(t.warnings[0].find("20 ms") != std::string::npos);
    CHECK(normalizeGifDelays({20, 30}).warnings.empty());
    auto g = fx::gif(8, 8, {0, 1, 5});
    auto info = parseGif(reinterpret_cast<const std::uint8_t*>(g.data()), g.size());
    auto plan = validateGif(info.value(), Tier::Standard);
    REQUIRE(plan.ok());
    CHECK(plan.value().timing.clampedCount == 2);               // 0 ms and 10 ms; 50 ms kept
    CHECK_FALSE(plan.value().timing.warnings.empty());
}

TEST_CASE("REQ-S07: GIF animation follows the media clock, speed and loop count") {
    const GifTimeline t({100, 100, 200}, 0);                     // 400 ms cycle, infinite loop
    CHECK(t.frameAt(0, 1.0) == 0);
    CHECK(t.frameAt(msToNs(99), 1.0) == 0);
    CHECK(t.frameAt(msToNs(100), 1.0) == 1);
    CHECK(t.frameAt(msToNs(250), 1.0) == 2);
    CHECK(t.frameAt(msToNs(400), 1.0) == 0);                     // wrapped
}
TEST_CASE("REQ-S07: GIF timeline speed scaling and finite loops") {
    const GifTimeline t({100, 100, 200}, 0);
    CHECK(t.frameAt(msToNs(250), 2.0) == 1);                     // 500 ms media time -> 100 ms into cycle -> frame 1
    CHECK(t.frameAt(msToNs(250), 0.25) == 0);                    // 62 ms media time
    CHECK(t.frameAt(msToNs(250), 4.0) == 2);                     // 1000 ms media time -> 200 ms into the cycle = start of frame 2
    const GifTimeline once({100, 100}, -1);                      // no NETSCAPE block: play once
    bool fin = false;
    CHECK(once.frameAt(msToNs(150), 1.0, &fin) == 1); CHECK_FALSE(fin);
    CHECK(once.frameAt(msToNs(250), 1.0, &fin) == 1); CHECK(fin);
    const GifTimeline twice({100, 100}, 1);                      // loop count 1 = one extra play
    CHECK(twice.frameAt(msToNs(250), 1.0, &fin) == 0); CHECK_FALSE(fin);   // second play, 50 ms in
    CHECK(twice.frameAt(msToNs(350), 1.0, &fin) == 1); CHECK_FALSE(fin);
    CHECK(twice.frameAt(msToNs(450), 1.0, &fin) == 1); CHECK(fin);
    // sub-20 ms delays inside the timeline are clamped as well
    const GifTimeline fast({0, 0}, 0);
    CHECK(fast.cycleNs() == msToNs(40));
}

TEST_CASE("REQ-S07: speed limits video 0.25-2x and GIF 0.25-4x") {
    CHECK(validateSpeed(SpeedKind::Video, 0.25).ok());
    CHECK(validateSpeed(SpeedKind::Video, 2.0).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Video, 0.24).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Video, 2.01).ok());
    CHECK(validateSpeed(SpeedKind::Gif, 4.0).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Gif, 4.5).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Gif, 0.0).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Gif, std::nan("")).ok());
    CHECK_FALSE(validateSpeed(SpeedKind::Video, INFINITY).ok());
    CHECK(clampSpeed(SpeedKind::Video, 9.0) == 2.0);
    CHECK(clampSpeed(SpeedKind::Gif, 9.0) == 4.0);
    CHECK(clampSpeed(SpeedKind::Gif, 0.0) == 0.25);
    CHECK(clampSpeed(SpeedKind::Gif, std::nan("")) == 1.0);
}

TEST_CASE("REQ-S07: video loop position uses the media clock") {
    bool ended = false;
    CHECK(loopPosition(secToNs(25), 1.0, secToNs(10), true, &ended) == secToNs(5)); CHECK_FALSE(ended);
    CHECK(loopPosition(secToNs(5), 2.0, secToNs(8), true) == secToNs(2));
    CHECK(loopPosition(secToNs(25), 1.0, secToNs(10), false, &ended) == secToNs(10)); CHECK(ended);
    CHECK(loopPosition(secToNs(4), 0.5, secToNs(10), false, &ended) == secToNs(2)); CHECK_FALSE(ended);
}

TEST_CASE("REQ-S07: video import accepts declared profiles on STANDARD+ only") {
    VideoInfo v; v.container = Format::Mp4; v.videoCodec = "h264"; v.width = 1280; v.height = 720; v.fps = 30; v.durationSec = 12;
    CHECK(validateVideo(v, Tier::Standard).ok());
    CHECK_FALSE(validateVideo(v, Tier::Base).ok());
    CHECK(validateVideo(v, Tier::Base).error().category == ErrorCategory::UnsupportedCapability);
    v.videoCodec = "hevc";
    CHECK_FALSE(validateVideo(v, Tier::Enhanced).ok());
    v.videoCodec = "h264"; v.width = 8192;
    CHECK_FALSE(validateVideo(v, Tier::Enhanced).ok());
    v.width = 1280; v.container = Format::Png;
    CHECK_FALSE(validateVideo(v, Tier::Standard).ok());
}

TEST_CASE("REQ-S06: text limit is 4096 code points, valid UTF-8, 256 messages") {
    CHECK(validateText(std::string(4096, 'a')).ok());
    CHECK_FALSE(validateText(std::string(4097, 'a')).ok());
    std::string uni; for (int i = 0; i < 4096; ++i) uni += "\xE2\x82\xAC";       // 4096 euro signs = 12288 bytes
    CHECK(validateText(uni).ok());
    uni += "\xE2\x82\xAC";
    CHECK_FALSE(validateText(uni).ok());
    CHECK_FALSE(validateText(std::string("bad \xC0\xAF utf8")).ok());           // overlong
    CHECK_FALSE(validateText(std::string("nul\0inside", 10)).ok());
    CHECK(validateText("").ok());
    CHECK(validateMessageCount(256).ok());
    CHECK_FALSE(validateMessageCount(257).ok());
}

TEST_CASE("REQ-S07: admission allows one video and one camera on STANDARD+, BASE needs qualification") {
    AdmissionRequest q; q.videos = 2; q.cameras = 2; q.gifs = 3;
    auto d = admitSources(Tier::Standard, q);
    CHECK(d.videosAdmitted == 1); CHECK(d.camerasAdmitted == 1); CHECK(d.gifsAdmitted == 3);
    CHECK(d.reasons.size() == 2);
    CHECK_FALSE(d.all(q));
    q.videos = 1; q.cameras = 1; q.gifs = 1;
    CHECK(admitSources(Tier::Enhanced, q).all(q));
    CHECK(admitSources(Tier::Enhanced, q).reasons.empty());
    auto b = admitSources(Tier::Base, q);
    CHECK(b.videosAdmitted == 0); CHECK(b.camerasAdmitted == 0); CHECK(b.gifsAdmitted == 0);
    CHECK(b.reasons.size() == 3);
    q.baseCameraQualified = true; q.baseGifQualified = true;
    auto b2 = admitSources(Tier::Base, q);
    CHECK(b2.camerasAdmitted == 1); CHECK(b2.gifsAdmitted == 1); CHECK(b2.videosAdmitted == 0);
}

TEST_CASE("REQ-S07: crop validation and fit/crop/stretch geometry") {
    CHECK(validateCrop({0.1f, 0.1f, 0.5f, 0.5f}).ok());
    CHECK(validateCrop({0, 0, 1, 1}).ok());
    CHECK_FALSE(validateCrop({0.6f, 0, 0.5f, 1}).ok());
    CHECK_FALSE(validateCrop({0, 0, 0, 1}).ok());
    CHECK_FALSE(validateCrop({-0.1f, 0, 0.5f, 0.5f}).ok());
    CHECK_FALSE(validateCrop({0, 0, std::nanf(""), 1}).ok());
    auto fit = computeFit(1920, 1080, 800, 600, FitMode::Fit);                 // 16:9 into 4:3 -> letterbox
    CHECK(fit.dst.w == 800); CHECK(fit.dst.h == 450); CHECK(fit.dst.y == 75);
    auto crop = computeFit(1920, 1080, 800, 600, FitMode::Crop);               // trim left/right
    CHECK(crop.src.h == 1080); CHECK(crop.src.w == 1440); CHECK(crop.src.x == 240);
    auto st = computeFit(100, 50, 640, 480, FitMode::Stretch);
    CHECK(st.dst.w == 640); CHECK(st.src.w == 100);
    auto pillar = computeFit(480, 640, 800, 600, FitMode::Fit);                // portrait into 4:3 -> pillarbox
    CHECK(pillar.dst.h == 600); CHECK(pillar.dst.w == 450); CHECK(pillar.dst.x == 175);
}

// ---------------------------------------------------------------------------------------------------------------
// Worker isolation (REQ-S08, REQ-X03)
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct WorkerRig {
    FakeClock clock;
    FakeMediaWorker worker;
    MediaSupervisor sup;
    explicit WorkerRig(SupervisorConfig cfg = {}) : sup(worker, clock, DecodedCacheBudget(Limits::kBaseDecodedCacheBytes), cfg) {}
    MediaRequest req(std::uint32_t id, std::uint32_t w = 32, std::uint32_t h = 32) {
        MediaRequest r; r.mediaId = id; r.format = Format::Png; r.relativePath = "media/a.png";
        r.targetWidth = w; r.targetHeight = h; r.reserveBytes = std::uint64_t(w) * h * 4; return r;
    }
};
}  // namespace

TEST_CASE("REQ-S08: successful decode becomes Ready without blocking the caller") {
    WorkerRig r;
    REQUIRE(r.sup.request(r.req(7)).ok());
    CHECK(r.sup.state(7) == MediaState::Pending);               // request() returned before any frame existed
    CHECK(r.sup.view(7).kind == MediaView::Kind::Placeholder);  // until ready, the placeholder is what is shown
    r.sup.pump();
    CHECK(r.sup.state(7) == MediaState::Ready);
    auto v = r.sup.view(7);
    CHECK(v.kind == MediaView::Kind::Real);
    CHECK(v.pixels->width == 32);
    CHECK(r.sup.budget().used() == 32u * 32u * 4u);
    r.sup.unload(7);
    CHECK(r.sup.budget().used() == 0);
    CHECK(r.sup.state(7) == MediaState::Unknown);
}

TEST_CASE("REQ-S08: decode failure keeps the valid current scene") {
    WorkerRig r;
    r.worker.setMode(2, FakeMediaWorker::Mode::Fail);
    REQUIRE(r.sup.request(r.req(1)).ok());
    r.sup.pump();
    REQUIRE(r.sup.state(1) == MediaState::Ready);
    REQUIRE(r.sup.request(r.req(2)).ok());
    r.sup.pump();
    CHECK(r.sup.state(2) == MediaState::Failed);
    CHECK_FALSE(r.sup.failureReason(2).empty());
    CHECK(r.sup.decideSwitch(1, 2) == SwitchDecision::KeepCurrent);       // current scene (media 1) is valid
    CHECK(r.sup.decideSwitch(0, 2) == SwitchDecision::KeepCurrent);       // current procedural scene is valid
    CHECK(r.sup.decideSwitch(1, 1) == SwitchDecision::Proceed);
    CHECK(r.sup.decideSwitch(1, 0) == SwitchDecision::Proceed);           // next scene needs no media
    CHECK(r.sup.budget().used() == 32u * 32u * 4u);                       // failed item released its reservation
}

TEST_CASE("REQ-S08: with nothing valid to keep a documented placeholder is substituted") {
    WorkerRig r;
    r.worker.setDefault(FakeMediaWorker::Mode::Fail);
    REQUIRE(r.sup.request(r.req(1)).ok());
    REQUIRE(r.sup.request(r.req(2)).ok());
    r.sup.pump();
    CHECK(r.sup.decideSwitch(1, 2) == SwitchDecision::UsePlaceholder);    // current media also failed
    CHECK(r.sup.decideSwitch(99, 2) == SwitchDecision::UsePlaceholder);   // current media unknown
    auto v = r.sup.view(2);
    CHECK(v.kind == MediaView::Kind::Placeholder);
    REQUIRE(v.pixels != nullptr);
    CHECK(v.pixels->width == 128); CHECK(v.pixels->height == 128);
    CHECK(v.pixels->row(0)[0] == 200);                                    // red frame
    CHECK(v.pixels->row(64)[3] == 255);                                   // opaque checker
    CHECK(r.sup.view(12345).kind == MediaView::Kind::Placeholder);        // missing media (never requested)
    CHECK(r.sup.view(12345).state == MediaState::Unknown);
}

TEST_CASE("REQ-X03: worker crash fails only in-flight media and the worker is restarted with backoff") {
    WorkerRig r;
    REQUIRE(r.sup.request(r.req(1)).ok());
    r.sup.pump();
    REQUIRE(r.sup.state(1) == MediaState::Ready);
    r.worker.setMode(2, FakeMediaWorker::Mode::Crash);
    REQUIRE(r.sup.request(r.req(3)).ok());                                // in flight when the worker dies
    REQUIRE(r.sup.request(r.req(2)).ok());                                // this one crashes the worker
    r.sup.pump();
    CHECK(r.sup.state(2) == MediaState::Failed);
    CHECK(r.sup.state(3) == MediaState::Failed);
    CHECK(r.sup.state(1) == MediaState::Ready);                           // earlier decoded media is untouched
    CHECK(r.sup.decideSwitch(1, 2) == SwitchDecision::KeepCurrent);
    CHECK_FALSE(r.worker.alive());
    CHECK(r.worker.restarts == 0);                                        // backoff not elapsed
    r.clock.advance(msToNs(499)); r.sup.pump();
    CHECK(r.worker.restarts == 0);
    r.clock.advance(msToNs(2)); r.sup.pump();
    CHECK(r.worker.restarts == 1);
    CHECK(r.worker.alive());
    r.worker.setMode(4, FakeMediaWorker::Mode::Succeed);
    REQUIRE(r.sup.request(r.req(4)).ok());
    r.sup.pump();
    CHECK(r.sup.state(4) == MediaState::Ready);                           // decoding works again
}

TEST_CASE("REQ-X03: a request while the worker is down fails fast instead of waiting") {
    WorkerRig r;
    r.worker.kill();
    auto st = r.sup.request(r.req(1));
    CHECK_FALSE(st.ok());
    CHECK(st.error().recoverability == Recoverability::Retryable);
    CHECK(r.sup.state(1) == MediaState::Failed);
    CHECK(r.sup.budget().used() == 0);
}

TEST_CASE("REQ-S08: a hung decoder is killed on timeout; control thread never waits") {
    SupervisorConfig cfg; cfg.decodeTimeoutNs = secToNs(2);
    WorkerRig r(cfg);
    r.worker.setMode(5, FakeMediaWorker::Mode::Hang);
    REQUIRE(r.sup.request(r.req(5)).ok());
    r.sup.pump();
    CHECK(r.sup.state(5) == MediaState::Pending);
    r.clock.advance(secToNs(1)); r.sup.pump();
    CHECK(r.sup.state(5) == MediaState::Pending);
    CHECK(r.worker.kills == 0);
    r.clock.advance(secToNs(2)); r.sup.pump();
    CHECK(r.sup.state(5) == MediaState::Failed);
    CHECK(r.sup.failureReason(5).find("timed out") != std::string::npos);
    CHECK(r.worker.kills == 1);
    CHECK(r.sup.budget().used() == 0);
}

TEST_CASE("REQ-S08: a worker reply larger than the validated plan is rejected") {
    WorkerRig r;
    r.worker.setMode(9, FakeMediaWorker::Mode::WrongSize, 64, 64);
    REQUIRE(r.sup.request(r.req(9, 32, 32)).ok());
    r.sup.pump();
    CHECK(r.sup.state(9) == MediaState::Failed);
    CHECK(r.sup.budget().used() == 0);
}

TEST_CASE("REQ-P05: media decode queue and cache stay bounded") {
    SupervisorConfig cfg; cfg.maxPending = 3;
    WorkerRig r(cfg);
    r.worker.setDefault(FakeMediaWorker::Mode::Hang);
    for (std::uint32_t i = 1; i <= 3; ++i) REQUIRE(r.sup.request(r.req(i)).ok());
    auto st = r.sup.request(r.req(4));
    CHECK_FALSE(st.ok());
    CHECK(st.error().category == ErrorCategory::ResourceExhausted);
    CHECK(r.worker.submits == 3);
    // cache budget exhaustion (BASE 8 MiB)
    WorkerRig r2;
    r2.worker.setDefault(FakeMediaWorker::Mode::Succeed);
    CHECK(r2.sup.request(r2.req(1, 800, 600)).ok());
    CHECK(r2.sup.request(r2.req(2, 800, 600)).ok());
    CHECK(r2.sup.request(r2.req(3, 800, 600)).ok());
    CHECK(r2.sup.request(r2.req(4, 800, 600)).ok());
    CHECK_FALSE(r2.sup.request(r2.req(5, 800, 600)).ok());                // would exceed 8 MiB
    CHECK(r2.sup.state(5) == MediaState::Failed);
}

TEST_CASE("REQ-X03: repeated worker restarts stop after the configured limit until the operator retries") {
    SupervisorConfig cfg; cfg.maxConsecutiveRestarts = 2;
    WorkerRig r(cfg);
    r.worker.failRestart(true);
    r.worker.kill();
    for (int i = 0; i < 10; ++i) { r.clock.advance(secToNs(1)); r.sup.pump(); }
    CHECK(r.sup.workerRestarts() == 2);
    r.worker.failRestart(false);
    r.sup.retryWorker();
    r.clock.advance(secToNs(1)); r.sup.pump(); r.clock.advance(secToNs(1)); r.sup.pump();
    CHECK(r.worker.alive());
}

// ---------------------------------------------------------------------------------------------------------------
// Asset manifest (REQ-S15)
// ---------------------------------------------------------------------------------------------------------------
namespace {
class MemSource final : public IAssetSource {
public:
    std::map<std::string, std::string> files;
    Result<std::string> sha256Hex(const std::string& rel, std::uint64_t* size) override {
        auto it = files.find(rel);
        if (it == files.end()) return makeError(ErrorCategory::InvalidArgument, "absent");
        if (size) *size = it->second.size();
        return Sha256::hashHex(it->second);
    }
    std::vector<std::string> listFiles() override {
        std::vector<std::string> v; for (auto& kv : files) v.push_back(kv.first); return v;
    }
};

AssetEntry goodEntry(const std::string& id, const std::string& path, const std::string& content) {
    AssetEntry e;
    e.id = id; e.version = "1.0"; e.type = "image"; e.path = path; e.creator = "Test Author";
    e.originalUrl = "https://example.org/a.png"; e.retrievalDate = "2026-10-01";
    e.spdx = "CC-BY-4.0"; e.copyright = "(c) 2026 Test Author"; e.attributionText = "Art by Test Author";
    e.attributionLocation = "About > Credits";
    e.commercialUse = Permission::Yes; e.modificationAllowed = Permission::Yes;
    e.derivativeProvenance = "original";
    e.originalSha256 = e.distributedSha256 = Sha256::hashHex(content);
    e.width = 16; e.height = 16; e.sizeBytes = content.size();
    e.flash.declared = true; e.flash.containsFlashOrStrobe = false;
    e.permittedTiers = {"base", "standard", "enhanced"};
    return e;
}
bool hasField(const std::vector<Finding>& f, const std::string& field) {
    for (auto& x : f) if (x.field == field) return true;
    return false;
}
}  // namespace

TEST_CASE("REQ-S15: a complete manifest entry validates and verifies against its file") {
    MemSource src; src.files["img/a.png"] = "PNGDATA";
    Manifest m; m.assets.push_back(goodEntry("a", "img/a.png", "PNGDATA"));
    CHECK(validateManifest(m).empty());
    auto rep = verifyManifest(m, src);
    CHECK(rep.ok());
    CHECK(rep.entriesChecked == 1);
}

TEST_CASE("REQ-S15: absent file, mismatched hash and unlisted shipped file all fail the gate") {
    MemSource src; src.files["img/a.png"] = "PNGDATA";
    Manifest m; m.assets.push_back(goodEntry("a", "img/a.png", "PNGDATA"));
    m.assets.push_back(goodEntry("b", "img/b.png", "OTHER"));                  // never shipped
    auto rep = verifyManifest(m, src);
    CHECK_FALSE(rep.ok());
    CHECK(hasField(rep.findings, "file"));
    src.files["img/b.png"] = "TAMPERED";
    CHECK(hasField(verifyManifest(m, src).findings, "distributedSha256"));
    src.files["img/b.png"] = "OTHER";
    CHECK(verifyManifest(m, src).ok());
    src.files["img/c.png"] = "EXTRA";                                         // shipped but no entry
    auto extra = verifyManifest(m, src);
    CHECK_FALSE(extra.ok());
    VerifyOptions lax; lax.requireAllFilesListed = false;
    CHECK(verifyManifest(m, src, lax).ok());
    VerifyOptions ign; ign.ignorePaths.insert("img/c.png");
    CHECK(verifyManifest(m, src, ign).ok());
}

TEST_CASE("REQ-S15: every required manifest field is enforced") {
    auto base = goodEntry("a", "img/a.png", "X");
    auto check = [&](const char* field, auto mutate) {
        AssetEntry e = base; mutate(e);
        INFO("field " << field);
        CHECK(hasField(validateEntry(e), field));
    };
    check("id", [](AssetEntry& e) { e.id.clear(); });
    check("version", [](AssetEntry& e) { e.version.clear(); });
    check("type", [](AssetEntry& e) { e.type.clear(); });
    check("path", [](AssetEntry& e) { e.path = "../escape.png"; });
    check("path", [](AssetEntry& e) { e.path = "/abs/a.png"; });
    check("path", [](AssetEntry& e) { e.path = "C:/a.png"; });
    check("creator", [](AssetEntry& e) { e.creator.clear(); });
    check("originalUrl", [](AssetEntry& e) { e.originalUrl.clear(); });
    check("originalUrl", [](AssetEntry& e) { e.originalUrl = "ftp://x/y"; });
    check("retrievalDate", [](AssetEntry& e) { e.retrievalDate = "10/01/2026"; });
    check("license", [](AssetEntry& e) { e.spdx.clear(); e.licenseText.clear(); });
    check("spdx", [](AssetEntry& e) { e.spdx = "NOASSERTION"; });
    check("spdx", [](AssetEntry& e) { e.spdx = "MIT AND"; });
    check("copyright", [](AssetEntry& e) { e.copyright.clear(); });
    check("attribution", [](AssetEntry& e) { e.attributionLocation.clear(); });
    check("commercialUse", [](AssetEntry& e) { e.commercialUse = Permission::Unset; });
    check("modificationAllowed", [](AssetEntry& e) { e.modificationAllowed = Permission::Unset; });
    check("derivativeProvenance", [](AssetEntry& e) { e.derivativeProvenance.clear(); });
    check("originalSha256", [](AssetEntry& e) { e.originalSha256 = "abc"; });
    check("distributedSha256", [](AssetEntry& e) { e.distributedSha256.clear(); });
    check("originalSha256", [](AssetEntry& e) { e.modified = false; e.originalSha256 = std::string(64, 'a'); });
    check("modificationAllowed", [](AssetEntry& e) { e.modified = true; e.modificationAllowed = Permission::No; });
    check("sizeBytes", [](AssetEntry& e) { e.sizeBytes = 0; });
    check("dimensions", [](AssetEntry& e) { e.width = 0; });
    check("durationSec", [](AssetEntry& e) { e.type = "gif"; e.durationSec = 0; });
    check("flash", [](AssetEntry& e) { e.flash.declared = false; });
    check("flash", [](AssetEntry& e) { e.flash.containsFlashOrStrobe = true; e.flash.maxFlashesPerSecond = 0; });
    check("permittedTiers", [](AssetEntry& e) { e.permittedTiers.clear(); });
    check("permittedTiers", [](AssetEntry& e) { e.permittedTiers = {"ultra"}; });
    AssetEntry orig = base; orig.isOriginalWork = true; orig.originalUrl.clear(); orig.retrievalDate.clear();
    orig.attributionText.clear(); orig.attributionLocation.clear();
    CHECK(validateEntry(orig).empty());                                         // original works may omit upstream fields
    AssetEntry strobe = base; strobe.flash.containsFlashOrStrobe = true; strobe.flash.maxFlashesPerSecond = 2;
    CHECK(validateEntry(strobe).empty());
    Manifest dup; dup.assets = {base, base};
    CHECK(hasField(validateManifest(dup), "id"));
    CHECK(hasField(validateManifest(dup), "path"));
}

TEST_CASE("REQ-S15: SPDX expression syntax") {
    for (const char* ok : {"MIT", "CC-BY-4.0", "MIT OR Apache-2.0", "(MIT OR BSD-3-Clause) AND CC0-1.0", "GPL-3.0-or-later WITH Classpath-exception-2.0",
                           "Apache-2.0+", "LicenseRef-Custom-1", "DocumentRef-x:LicenseRef-y"})
        CHECK_MESSAGE(isValidSpdxExpression(ok), ok);
    for (const char* bad : {"", "AND", "MIT AND", "(MIT", "MIT)", "MIT OR OR BSD", "NONE", "NOASSERTION", "MIT; rm -rf", "WITH MIT"})
        CHECK_MESSAGE(!isValidSpdxExpression(bad), bad);
}

TEST_CASE("REQ-S15: manifest JSON parsing") {
    const std::string js = R"({"schema":1,"assets":[{"id":"a","version":"1","type":"image","path":"a.png","creator":"c",
      "originalUrl":"https://e.org/a","retrievalDate":"2026-10-01","spdx":"MIT","copyright":"c","attributionText":"t",
      "attributionLocation":"l","commercialUse":true,"modificationAllowed":false,"derivativeProvenance":"original",
      "originalSha256":"AA","distributedSha256":"BB","width":4,"height":4,"sizeBytes":12,
      "flash":{"containsFlashOrStrobe":true,"maxFlashesPerSecond":2.5},"permittedTiers":["base","standard"]}]})";
    auto m = parseManifestJson(js);
    REQUIRE(m.ok());
    REQUIRE(m.value().assets.size() == 1);
    const auto& e = m.value().assets[0];
    CHECK(e.commercialUse == Permission::Yes);
    CHECK(e.modificationAllowed == Permission::No);
    CHECK(e.originalSha256 == "aa");                                           // normalised to lower case
    CHECK(e.flash.declared); CHECK(e.flash.containsFlashOrStrobe); CHECK(e.flash.maxFlashesPerSecond == 2.5);
    CHECK(e.permittedTiers.size() == 2);
    CHECK_FALSE(parseManifestJson("not json").ok());
    CHECK_FALSE(parseManifestJson("{\"schema\":1}").ok());
    CHECK(parseManifestJson("{\"schema\":2,\"assets\":[]}").error().category == ErrorCategory::SchemaUnsupported);
    CHECK_FALSE(parseManifestJson("{\"schema\":1,\"assets\":[3]}").ok());
}

#ifdef SS_ASSET_CHECK_EXE
namespace {
namespace fs = std::filesystem;
int runCheck(const fs::path& manifest, const fs::path& root, const char* extra = "") {
    const std::string cmd = std::string("\"") + SS_ASSET_CHECK_EXE + "\" --manifest \"" + manifest.string() + "\" --root \"" +
                            root.string() + "\" " + extra + " >" + (fs::temp_directory_path() / "ss_asset_check.out").string() + " 2>&1";
#ifdef _WIN32
    // cmd.exe /c strips the outer quotes of a command that has several quoted tokens; wrap once more.
    const int rc = std::system(("\"" + cmd + "\"").c_str());
    return rc;
#else
    const int rc = std::system(cmd.c_str());
    return (rc & 0x7F) == 0 ? ((rc >> 8) & 0xFF) : 255;
#endif
}
void writeFile(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
}  // namespace

TEST_CASE("REQ-S15: ss_asset_check exits non-zero on absent or mismatched entries") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ss_asset_check_test";
    fs::remove_all(dir);
    const std::string content = "hello asset";
    writeFile(dir / "assets" / "img" / "a.png", content);
    auto e = goodEntry("a", "img/a.png", content);
    auto toJson = [](const AssetEntry& x) {
        std::string tiers;
        for (auto& t : x.permittedTiers) tiers += (tiers.empty() ? "\"" : ",\"") + t + "\"";
        return std::string("{\"schema\":1,\"assets\":[{\"id\":\"") + x.id + "\",\"version\":\"1.0\",\"type\":\"image\",\"path\":\"" + x.path +
               "\",\"creator\":\"c\",\"originalUrl\":\"https://e.org/a\",\"retrievalDate\":\"2026-10-01\",\"spdx\":\"MIT\",\"copyright\":\"c\"," +
               "\"attributionText\":\"t\",\"attributionLocation\":\"l\",\"commercialUse\":true,\"modificationAllowed\":true," +
               "\"derivativeProvenance\":\"original\",\"originalSha256\":\"" + x.originalSha256 + "\",\"distributedSha256\":\"" + x.distributedSha256 +
               "\",\"width\":16,\"height\":16,\"sizeBytes\":" + std::to_string(x.sizeBytes) +
               ",\"flash\":{\"containsFlashOrStrobe\":false},\"permittedTiers\":[" + tiers + "]}]}";
    };
    writeFile(dir / "assets.json", toJson(e));
    CHECK(runCheck(dir / "assets.json", dir / "assets") == 0);
    writeFile(dir / "assets" / "img" / "a.png", "tampered!!!");                 // same length, different bytes
    CHECK(runCheck(dir / "assets.json", dir / "assets") == 1);
    writeFile(dir / "assets" / "img" / "a.png", content);
    CHECK(runCheck(dir / "assets.json", dir / "assets") == 0);
    writeFile(dir / "assets" / "img" / "unlisted.png", "x");                    // shipped without manifest entry
    CHECK(runCheck(dir / "assets.json", dir / "assets") == 1);
    CHECK(runCheck(dir / "assets.json", dir / "assets", "--allow-unlisted") == 0);
    fs::remove(dir / "assets" / "img" / "unlisted.png");
    fs::remove(dir / "assets" / "img" / "a.png");                              // absent
    CHECK(runCheck(dir / "assets.json", dir / "assets") == 1);
    writeFile(dir / "bad.json", "{not json");
    CHECK(runCheck(dir / "bad.json", dir / "assets") == 2);
    CHECK(runCheck(dir / "missing.json", dir / "assets") == 2);
    fs::remove_all(dir);
}
#endif
