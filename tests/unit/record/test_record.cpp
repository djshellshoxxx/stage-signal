#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include "doctest/doctest.h"
#include "nlohmann/json.hpp"
#include "record_fakes.hpp"
#include "ss/base/rng.hpp"
#include "ss/record/disk.hpp"
#include "ss/record/fanout.hpp"
#include "ss/record/ffmpeg_args.hpp"
#include "ss/record/journal.hpp"
#include "ss/record/naming.hpp"
#include "ss/record/prepare.hpp"
#include "ss/record/profile.hpp"
#include "ss/record/segment.hpp"
#include "ss/record/session.hpp"
#include "ss/record/wav.hpp"

using namespace ss;
using namespace ss::record;

namespace {
const std::uint64_t GiB = 1ull << 30, MiB = 1ull << 20;

RecordProfile videoProfile(VideoCodecPolicy pol = VideoCodecPolicy::PreferH264FallbackMjpeg, Ns delay = 0) {
    RecordProfileSpec s;
    s.kind = ProfileKind::VideoMatroska; s.outputDir = "/rec"; s.showName = "Friday Night / Main Room";
    s.codecPolicy = pol; s.audioDelayCompensationNs = delay;
    return RecordProfile::create(s).value();
}
RecordProfile audioProfile(std::uint32_t ch = 2, Ns segNs = Limits::kDefaultSegmentNs, std::uint64_t budget = 0) {
    RecordProfileSpec s;
    s.kind = ProfileKind::AudioOnlyWav; s.outputDir = "/rec"; s.audio.channels = ch; s.segmentNs = segNs; s.segmentSizeBudgetBytes = budget;
    return RecordProfile::create(s).value();
}
ProbeResults goodProbe() {
    ProbeResults p; p.ffmpegPresent = true;
    p.encoders.push_back({"h264_nvenc", VideoCodec::H264, true, true, true});
    p.encoders.push_back({"mjpeg", VideoCodec::Mjpeg, false, true, true});
    return p;
}
PrepareInputs goodInputs(bool video = true) {
    PrepareInputs in;
    in.path = {true, true, true};
    in.disk = {true, 200 * GiB};
    in.sources.audioConfigured = in.sources.audioDeviceOk = true;
    in.sources.audioRateHz = 48000; in.sources.audioChannels = 2;
    if (video) {
        in.sources.videoConfigured = in.sources.videoSourceOk = true;
        in.sources.videoWidth = 1280; in.sources.videoHeight = 720; in.sources.videoRateKnown = true; in.sources.videoFps = 30;
        in.encoders = goodProbe();
    }
    return in;
}
bool hasIssue(const PrepareReport& r, IssueCode c, bool blocking = true) {
    for (auto& i : r.issues) if (i.code == c && i.blocking == blocking) return true;
    return false;
}
std::vector<float> silenceBlock(std::uint32_t frames, std::uint32_t ch, float v = 0.f) { return std::vector<float>(std::size_t(frames) * ch, v); }
AudioBlockView view(const std::vector<float>& b, std::uint32_t ch, std::uint32_t rate, std::uint64_t first, Generation gen = 1) {
    AudioBlockView v; v.interleaved = b.data(); v.frames = std::uint32_t(b.size() / ch); v.channels = ch; v.rateHz = rate;
    v.firstSampleIndex = first; v.generation = gen; return v;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Profile and encoder selection (REQ-R01, section 12)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R01: profile validation and stable hash") {
    CHECK(RecordProfile::create(RecordProfileSpec{}).ok() == false);               // no output folder
    RecordProfileSpec s; s.outputDir = "/rec";
    auto p = RecordProfile::create(s);
    REQUIRE(p.ok());
    CHECK(p.value().hash().size() == 64);
    CHECK(p.value().hash() == RecordProfile::create(s).value().hash());
    s.video.width = 1281;
    CHECK_FALSE(RecordProfile::create(s).ok());                                    // odd size
    s.video.width = 1280; s.video.fpsNum = 120;
    CHECK_FALSE(RecordProfile::create(s).ok());
    s.video.fpsNum = 30; s.audio.rateHz = 22050;
    CHECK_FALSE(RecordProfile::create(s).ok());
    s.audio.rateHz = 48000; s.segmentNs = secToNs(5);
    CHECK_FALSE(RecordProfile::create(s).ok());
    s.segmentNs = Limits::kDefaultSegmentNs; s.audioDelayCompensationNs = -1;
    CHECK_FALSE(RecordProfile::create(s).ok());
    RecordProfileSpec a = s; a.audioDelayCompensationNs = 0; a.video.h264BitrateKbps = 8000;
    CHECK(RecordProfile::create(a).value().hash() != p.value().hash());            // encoding fields change the hash
    // The profile cannot be mutated through the accessor.
    static_assert(std::is_const<std::remove_reference<decltype(p.value().spec())>::type>::value, "spec() must be const");
    CHECK(p.value().effectiveSizeBudget() == Limits::kDefaultVideoSizeBudget);
}

TEST_CASE("REQ-R01: ffmpeg -encoders text is parsed; GPL encoders are never returned") {
    const std::string out =
        "Encoders:\n V..... = Video\n A..... = Audio\n ------\n"
        " V....D libx264              libx264 H.264 / AVC (codec h264)\n"
        " V....D libx265              libx265 H.265\n"
        " V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)\n"
        " V..... h264_qsv             H.264 Quick Sync (codec h264)\n"
        " V....D libopenh264          OpenH264 H.264 (codec h264)\n"
        " VFS... mjpeg                MJPEG (Motion JPEG)\n"
        " A....D aac                  AAC\n";
    const auto v = parseFfmpegEncoders(out);
    REQUIRE(v.size() == 4);
    for (auto& e : v) { CHECK(e.listed); CHECK_FALSE(e.verified); CHECK_FALSE(e.usable()); CHECK(isEligibleEncoder(e.name)); }
    CHECK(isExcludedGplEncoder("libx264"));
    CHECK_FALSE(isEligibleEncoder("libx264"));
    CHECK(parseFfmpegEncoders("no table here").empty());
}

TEST_CASE("REQ-R01: encoder selection prefers hardware H.264, falls back to MJPEG, never assumes x264") {
    ProbeResults p = goodProbe();
    p.encoders.insert(p.encoders.begin(), {"libx264", VideoCodec::H264, false, true, true});   // present and "verified" but excluded
    p.encoders.push_back({"h264_qsv", VideoCodec::H264, true, true, true});
    auto c = selectVideoEncoder(p, VideoCodecPolicy::PreferH264FallbackMjpeg);
    REQUIRE(c.ok());
    CHECK(c.value().name == "h264_nvenc");
    CHECK_FALSE(c.value().fallbackFromH264);

    ProbeResults only264; only264.ffmpegPresent = true;
    only264.encoders.push_back({"libx264", VideoCodec::H264, false, true, true});
    only264.encoders.push_back({"h264_nvenc", VideoCodec::H264, true, true, false});        // listed but the test encode failed
    only264.encoders.push_back({"mjpeg", VideoCodec::Mjpeg, false, true, true});
    auto fb = selectVideoEncoder(only264, VideoCodecPolicy::PreferH264FallbackMjpeg);
    REQUIRE(fb.ok());
    CHECK(fb.value().name == "mjpeg");
    CHECK(fb.value().fallbackFromH264);
    CHECK_FALSE(fb.value().note.empty());
    auto strict = selectVideoEncoder(only264, VideoCodecPolicy::H264Only);
    REQUIRE_FALSE(strict.ok());
    CHECK(strict.error().category == ErrorCategory::EncoderUnavailable);
    CHECK(strict.error().detail.find("x264") != std::string::npos);
    CHECK(selectVideoEncoder(only264, VideoCodecPolicy::MjpegOnly).value().fallbackFromH264 == false);
    ProbeResults none; none.ffmpegPresent = true;
    CHECK_FALSE(selectVideoEncoder(none, VideoCodecPolicy::PreferH264FallbackMjpeg).ok());
    ProbeResults noFfmpeg = goodProbe(); noFfmpeg.ffmpegPresent = false;
    CHECK_FALSE(selectVideoEncoder(noFfmpeg, VideoCodecPolicy::PreferH264FallbackMjpeg).ok());
    ProbeResults sw; sw.ffmpegPresent = true; sw.encoders.push_back({"libopenh264", VideoCodec::H264, false, true, true});
    CHECK(selectVideoEncoder(sw, VideoCodecPolicy::H264Only).value().name == "libopenh264");
}

// ---------------------------------------------------------------------------------------------------------------
// Preparing and the state machine (REQ-R06)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R06: a fully valid setup passes Preparing and picks the encoder") {
    auto r = prepareChecks(videoProfile(), goodInputs());
    CHECK(r.ok);
    REQUIRE(r.encoder.has_value());
    CHECK(r.encoder->name == "h264_nvenc");
    CHECK(r.profileHash == videoProfile().hash());
    CHECK(r.requiredFreeBytes > Limits::kReserveMinBytes);
}

TEST_CASE("REQ-R06: every start failure is blocking and carries an actionable message") {
    auto prof = videoProfile();
    struct Case { const char* name; IssueCode code; std::function<void(PrepareInputs&)> mutate; };
    std::vector<Case> cases = {
        {"missing folder", IssueCode::PathMissing, [](PrepareInputs& i) { i.path.exists = false; }},
        {"read-only folder", IssueCode::PathNotWritable, [](PrepareInputs& i) { i.path.writable = false; }},
        {"unknown disk", IssueCode::DiskUnknown, [](PrepareInputs& i) { i.disk.valid = false; }},
        {"full disk", IssueCode::DiskTooLow, [](PrepareInputs& i) { i.disk.freeBytes = 100 * MiB; }},
        {"no ffmpeg", IssueCode::FfmpegMissing, [](PrepareInputs& i) { i.encoders.ffmpegPresent = false; }},
        {"no encoder", IssueCode::EncoderUnavailable, [](PrepareInputs& i) { i.encoders.encoders.clear(); }},
        {"no muxer", IssueCode::MuxerMissing, [](PrepareInputs& i) { i.encoders.segmentMuxer = false; }},
        {"no audio", IssueCode::AudioSourceMissing, [](PrepareInputs& i) { i.sources.audioDeviceOk = false; }},
        {"audio rate", IssueCode::AudioFormatMismatch, [](PrepareInputs& i) { i.sources.audioRateHz = 44100; }},
        {"no video", IssueCode::VideoSourceMissing, [](PrepareInputs& i) { i.sources.videoSourceOk = false; }},
        {"video size", IssueCode::VideoFormatMismatch, [](PrepareInputs& i) { i.sources.videoWidth = 800; }},
        {"video fps", IssueCode::VideoFormatMismatch, [](PrepareInputs& i) { i.sources.videoFps = 25; }},
    };
    for (auto& c : cases) {
        INFO(c.name);
        PrepareInputs in = goodInputs();
        c.mutate(in);
        auto r = prepareChecks(prof, in);
        CHECK_FALSE(r.ok);
        CHECK(hasIssue(r, c.code));
        CHECK_FALSE(r.encoder.has_value());
        for (auto& i : r.issues) { CHECK_FALSE(i.message.empty()); CHECK_FALSE(i.action.empty()); }
    }
    // messages never contain the configured path
    auto r = prepareChecks(prof, [] { auto i = goodInputs(); i.path.exists = false; return i; }());
    for (auto& i : r.issues) { CHECK(i.message.find("/rec") == std::string::npos); CHECK(i.action.find("/rec") == std::string::npos); }
}

TEST_CASE("REQ-R06: low (not critical) disk space and MJPEG fallback are warnings, not blockers") {
    PrepareInputs in = goodInputs();
    in.disk.freeBytes = 1 * GiB + 900 * MiB;
    auto r = prepareChecks(videoProfile(), in);
    CHECK(r.ok);
    CHECK(hasIssue(r, IssueCode::DiskLowWarning, false));
    PrepareInputs fb = goodInputs();
    fb.encoders.encoders.erase(fb.encoders.encoders.begin());     // only MJPEG remains
    auto r2 = prepareChecks(videoProfile(), fb);
    CHECK(r2.ok);
    CHECK(hasIssue(r2, IssueCode::EncoderFallback, false));
    CHECK(r2.encoder->codec == VideoCodec::Mjpeg);
    CHECK(r2.nominal.bytesPerSec > prepareChecks(videoProfile(), goodInputs()).nominal.bytesPerSec);   // MJPEG plans for more space
}

TEST_CASE("REQ-R06: Recording is claimed only after Preparing succeeded") {
    FakeClock clock;
    RecordSession s(clock);
    CHECK(s.state() == State::Idle);
    REQUIRE(s.beginPrepare(videoProfile(), "0123abcd").ok());
    CHECK(s.state() == State::Preparing);
    CHECK_FALSE(s.addMarker("x").ok());                           // not Recording yet
    CHECK_FALSE(s.requestStop().ok());
    auto in = goodInputs(); in.disk.freeBytes = 10 * MiB;
    auto rep = s.completePrepare(in);
    CHECK_FALSE(rep.ok);
    CHECK(s.state() == State::Failed);                            // never visited Recording
    CHECK(s.summary().hasError);
    CHECK(s.summary().lastError.category == ErrorCategory::DiskFull);
    CHECK(s.summary().losses.empty());
    // a new attempt after a failure
    REQUIRE(s.beginPrepare(videoProfile(), "0123abcd").ok());
    CHECK(s.completePrepare(goodInputs()).ok);
    CHECK(s.state() == State::Recording);
    CHECK(s.summary().encoderName == "h264_nvenc");
    CHECK_FALSE(s.beginPrepare(videoProfile(), "0123abcd").ok());  // already active
}

TEST_CASE("REQ-R06: state machine Idle -> Preparing -> Recording -> Stopping -> Completed") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(videoProfile(), "deadbeef").ok());
    REQUIRE(s.completePrepare(goodInputs()).ok);
    s.onSegmentOpened(1, "a.mkv", 0);
    clock.advance(secToNs(5));
    REQUIRE(s.requestStop().ok());
    CHECK(s.state() == State::Stopping);
    CHECK(s.requestStop().ok());                                  // idempotent
    clock.advance(secToNs(2));
    s.onSegmentClosed(1, 1000, secToNs(5));
    s.onWriterStopped(true);
    CHECK(s.state() == State::Completed);
    CHECK(s.summary().stopReason == StopReason::User);
    CHECK_FALSE(s.summary().endedEarly);
    CHECK(s.summary().durationNs == secToNs(5));                  // frozen when Stop was requested
    CHECK(s.elapsedNs() == secToNs(5));
    s.reset();
    CHECK(s.state() == State::Idle);
}

TEST_CASE("REQ-R03: show and scene changes never reset the session clock") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(videoProfile(), "deadbeef").ok());
    REQUIRE(s.completePrepare(goodInputs()).ok);
    clock.advance(secToNs(30));
    CHECK(s.elapsedNs() == secToNs(30));
    s.noteShowEvent(); s.noteShowEvent();
    clock.advance(secToNs(30));
    CHECK(s.elapsedNs() == secToNs(60));
    CHECK(s.showEvents() == 2);
}

// ---------------------------------------------------------------------------------------------------------------
// Disk thresholds (REQ-R04)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R04: reserve is max(256 MiB, 30 s of estimated bytes)") {
    RateEstimate small; small.bytesPerSec = 288000; small.relUncertainty = 0;             // audio PCM: 30 s = 8.6 MB
    CHECK(DiskMonitor::reserveFor(small) == 256 * MiB);
    RateEstimate big; big.bytesPerSec = 20e6; big.relUncertainty = 0.0;                    // 30 s = 600 MB
    CHECK(DiskMonitor::reserveFor(big) == 600000000ull);
    big.relUncertainty = 0.5;                                                              // uncertainty enlarges the reserve
    CHECK(DiskMonitor::reserveFor(big) == 900000000ull);
}

TEST_CASE("REQ-R04: measured output rate and its uncertainty") {
    RateEstimator est(1000.0, 0.5);
    auto e0 = est.estimate();
    CHECK_FALSE(e0.measured);
    CHECK(e0.bytesPerSec == 1000.0);
    CHECK(e0.relUncertainty == 0.5);
    std::uint64_t bytes = 0;
    for (int i = 0; i <= 10; ++i) { est.addSample(secToNs(i), bytes); bytes += 1'000'000; }      // steady 1 MB/s
    auto e = est.estimate();
    CHECK(e.measured);
    CHECK(e.bytesPerSec == doctest::Approx(1e6).epsilon(0.01));
    CHECK(e.relUncertainty <= 0.06);                                                        // clean data: floor at 5%
    RateEstimator noisy(1000.0, 0.5);
    bytes = 0;
    for (int i = 0; i <= 12; ++i) { noisy.addSample(secToNs(i), bytes); bytes += (i % 2) ? 2'000'000 : 100'000; }
    auto n = noisy.estimate();
    CHECK(n.measured);
    CHECK(n.relUncertainty > e.relUncertainty);                                             // bursty output is less certain
    // counters that go backwards are ignored
    noisy.addSample(secToNs(13), 1);
    CHECK(noisy.estimate().bytesPerSec == doctest::Approx(n.bytesPerSec));
}

TEST_CASE("REQ-R04: warn at 2 GiB free or ~10 min remaining; stop at the reserve") {
    DiskMonitor m(288000.0, 0.0);                                                          // audio-only nominal, exact
    auto st = m.update(50 * GiB);
    CHECK(st.level == DiskLevel::Ok);
    st = m.update(2 * GiB);
    CHECK(st.level == DiskLevel::Warning);
    CHECK(st.warnByFree);
    CHECK_FALSE(st.message.empty());
    st = m.update(2 * GiB + 1);
    CHECK(st.level == DiskLevel::Ok);                                                      // 2 GiB / 288 kB/s is hours
    // High-rate output: 20 MB/s; 10 min = 12 GB above the 600 MB reserve
    DiskMonitor hi(20e6, 0.0);
    CHECK(hi.update(14 * GiB).level == DiskLevel::Ok);
    auto w = hi.update(12'500'000'000ull);
    CHECK(w.level == DiskLevel::Warning);
    CHECK(w.warnByTime);
    CHECK_FALSE(w.warnByFree);
    CHECK(w.reserveBytes == 600'000'000ull);
    CHECK(hi.update(599'000'000ull).level == DiskLevel::StopNow);
    CHECK(hi.update(600'000'000ull).level == DiskLevel::StopNow);                          // at the reserve: stop now
    CHECK(hi.update(600'000'001ull).level != DiskLevel::StopNow);
    // uncertainty is shown in the remaining-time range: pessimistic < point estimate
    DiskMonitor unc(20e6, 0.5);
    auto u = unc.update(30 * GiB);
    CHECK(u.remainingLowNs < u.remainingPointNs);
    CHECK(u.rate.relUncertainty == 0.5);
    CHECK_FALSE(u.rate.measured);
}

TEST_CASE("REQ-R04: disk-low moves the session to Stopping so the writer can close files gracefully") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(audioProfile(), "11112222").ok());
    REQUIRE(s.completePrepare(goodInputs(false)).ok);
    s.onSegmentOpened(1, "a.wav", 0);
    CHECK(s.checkDisk(50 * GiB).level == DiskLevel::Ok);
    CHECK(s.state() == State::Recording);
    CHECK(s.checkDisk(1 * GiB).level == DiskLevel::Warning);
    CHECK(s.state() == State::Recording);                                                  // warning does not stop
    CHECK(s.checkDisk(100 * MiB).level == DiskLevel::StopNow);
    CHECK(s.state() == State::Stopping);
    CHECK(s.summary().stopReason == StopReason::DiskLow);
    s.onSegmentClosed(1, 4096, secToNs(1));
    s.onWriterStopped(true);
    CHECK(s.state() == State::Completed);
    CHECK(s.summary().endedEarly);
    CHECK(s.summary().segments[0].closed);
}

TEST_CASE("REQ-R04: a writer fault leaves Recoverable (completed segments) or Failed, and records the lost interval") {
    FakeClock clock;
    {
        RecordSession s(clock);
        REQUIRE(s.beginPrepare(videoProfile(), "aaaabbbb").ok());
        REQUIRE(s.completePrepare(goodInputs()).ok);
        s.onSegmentOpened(1, "a.mkv", 0);
        clock.advance(secToNs(20));
        s.onSegmentClosed(1, 5000, secToNs(20));
        s.onSegmentOpened(2, "b.mkv", secToNs(20));
        clock.advance(secToNs(7));
        s.onWriterFault(makeError(ErrorCategory::DiskFull, "write failed"), false);
        CHECK(s.state() == State::Recoverable);
        REQUIRE(s.summary().losses.size() == 1);
        CHECK(s.summary().losses[0].fromNs == secToNs(27));
        CHECK_FALSE(s.summary().losses[0].lengthKnown);                                    // honest: we cannot measure it
        CHECK(s.summary().endedEarly);
        CHECK(s.summary().lastError.category == ErrorCategory::DiskFull);
    }
    {
        RecordSession s(clock);
        REQUIRE(s.beginPrepare(videoProfile(), "aaaabbbb").ok());
        REQUIRE(s.completePrepare(goodInputs()).ok);
        s.onWriterFault(makeError(ErrorCategory::EncoderUnavailable, "encoder died at start"), true, secToNs(3));
        CHECK(s.state() == State::Failed);                                                 // nothing on disk
        REQUIRE(s.summary().losses.size() == 1);
        CHECK(s.summary().losses[0].lengthKnown);
        CHECK(s.summary().losses[0].lengthNs == secToNs(3));
    }
    {
        RecordSession s(clock);                                                           // an unclean close while Stopping
        REQUIRE(s.beginPrepare(videoProfile(), "aaaabbbb").ok());
        REQUIRE(s.completePrepare(goodInputs()).ok);
        s.onSegmentOpened(1, "a.mkv", 0);
        REQUIRE(s.requestStop().ok());
        s.onWriterStopped(false);
        CHECK(s.state() == State::Recoverable);
    }
    {
        RecordSession s(clock);                                                           // fault while Preparing
        REQUIRE(s.beginPrepare(videoProfile(), "aaaabbbb").ok());
        s.onWriterFault(makeError(ErrorCategory::InternalFault, "x"));
        CHECK(s.state() == State::Failed);
        CHECK(s.summary().losses.empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R01: file names carry UTC timestamp, session id and a sanitised show name") {
    CHECK(formatUtcCompact(1'700'000'000LL * kNsPerSec) == "20231114T221320Z");
    CHECK(formatUtcCompact(0) == "19700101T000000Z");
    CHECK(formatUtcCompact(951'782'400LL * kNsPerSec) == "20000229T000000Z");               // leap day
    CHECK(sanitizeShowName("Friday Night / Main Room") == "Friday_Night_Main_Room");
    CHECK(sanitizeShowName("../../etc/passwd") == "etc_passwd");
    CHECK(sanitizeShowName("C:\\Windows\\evil") == "C_Windows_evil");
    CHECK(sanitizeShowName("") == "show");
    CHECK(sanitizeShowName("   ...   ") == "show");
    CHECK(sanitizeShowName("CON") == "_CON");
    CHECK(sanitizeShowName("lpt1.txt") == "_lpt1.txt");
    CHECK(sanitizeShowName("caf\xC3\xA9 \xE2\x82\xAC") == "caf");
    CHECK(sanitizeShowName(std::string(200, 'a')).size() == 48);
    CHECK(sanitizeShowName("a%s%n%d") == "a_s_n_d");
    CHECK(sanitizeShowName("tab\tnew\nline") == "tab_new_line");
    Rng rng(42);
    const auto id = makeSessionId(rng);
    CHECK(isValidSessionId(id));
    Rng rng2(42);
    CHECK(makeSessionId(rng2) == id);                                                      // deterministic
    CHECK_FALSE(isValidSessionId("XYZ"));
    CHECK(segmentFileName(1'700'000'000LL * kNsPerSec, "0a1b2c3d", "My Show", 7, "mkv") == "20231114T221320Z_0a1b2c3d_My_Show_seg0007.mkv");
    CHECK(segmentPattern(1'700'000'000LL * kNsPerSec, "0a1b2c3d", "My Show", "mkv") == "20231114T221320Z_0a1b2c3d_My_Show_seg%04d.mkv");
    CHECK(journalFileName(0, "0a1b2c3d", "x").find(".journal") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------
// Segmentation (REQ-R04/R05, section 12)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R05: audio segmenter cuts on exact samples with no overlap and no loss") {
    AudioSegmenter seg(48000, secToNs(10), 3500ull * MiB, 6, kWavHeaderBytes);
    CHECK(seg.framesPerSegment() == 480000);
    CHECK(seg.plannedReason() == CutReason::Time);
    std::vector<AudioSegmenter::Piece> pieces;
    std::uint64_t total = 0;
    for (std::uint64_t block : {100000ull, 300000ull, 100000ull, 7ull, 900000ull}) {
        pieces.clear();
        seg.split(block, pieces);
        std::uint64_t sum = 0;
        for (auto& p : pieces) { CHECK(p.frames > 0); sum += p.frames; }
        CHECK(sum == block);                                                               // nothing lost, nothing repeated
        total += block;
    }
    const auto& closed = seg.closed();
    REQUIRE(closed.size() == 2);
    CHECK(closed[0].frames == 480000);
    CHECK(closed[0].startNs == 0);
    CHECK(closed[0].endNs == secToNs(10));
    CHECK(closed[1].startNs == closed[0].endNs);                                           // contiguous: no gap, no overlap
    CHECK(closed[1].endNs == secToNs(20));
    auto last = seg.finish();
    CHECK(last.index == 3);
    CHECK(last.startNs == secToNs(20));
    CHECK(last.frames == total - 960000);
    CHECK(last.reason == CutReason::Stop);
}

TEST_CASE("REQ-R05: 44.1 kHz boundaries use exact sample math") {
    AudioSegmenter seg(44100, secToNs(10), 3500ull * MiB, 6, kWavHeaderBytes);
    std::vector<AudioSegmenter::Piece> p;
    seg.split(441000 * 2 + 1, p);
    REQUIRE(seg.closed().size() == 2);
    CHECK(seg.closed()[1].endNs == secToNs(20));
    CHECK(seg.framesInCurrent() == 1);
}

TEST_CASE("REQ-R05: the size budget cuts before the time limit when it is smaller") {
    AudioSegmenter seg(48000, Limits::kMaxSegmentNs, 16 * MiB, 6, kWavHeaderBytes);
    CHECK(seg.plannedReason() == CutReason::Size);
    CHECK(seg.framesPerSegment() == (16 * MiB - kWavHeaderBytes) / 6);
    // never beyond the classic RIFF limit
    AudioSegmenter huge(48000, Limits::kMaxSegmentNs, 64ull * GiB, 6, kWavHeaderBytes);
    CHECK(huge.framesPerSegment() * 6 + kWavHeaderBytes <= 0xFFFFFFFFull);
}

TEST_CASE("REQ-R04: video segment duration is time-limited and size-derived") {
    CHECK(videoSegmentDuration(secToNs(900), 2048 * MiB, 1e6) == secToNs(900));            // 2 GiB at 1 MB/s lasts 2147 s
    CHECK(videoSegmentDuration(secToNs(900), 2048 * MiB, 10e6) == secToNs(214));           // size-derived
    CHECK(videoSegmentDuration(secToNs(900), 100 * MiB, 100e6) == secToNs(10));            // floor at 10 s
}

TEST_CASE("REQ-R04: ffmpeg segment list is parsed and gaps/overlaps are reported") {
    std::size_t bad = 0;
    auto v = parseSegmentListCsv("a_0001.mkv,0.000000,900.000000\r\na_0002.mkv,900.000000,1800.040000\nbroken line\n\"c,3.mkv\",1800.040000,2000.000000\n", &bad);
    REQUIRE(v.size() == 3);
    CHECK(bad == 1);
    CHECK(v[1].startNs == secToNs(900));
    CHECK(v[1].endNs == 1'800'040'000'000LL);
    CHECK(v[2].file == "c,3.mkv");
    CHECK(checkContinuity(v, msToNs(1)).empty());
    v[2].startNs += msToNs(250);
    auto gaps = checkContinuity(v, msToNs(1));
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0].find("gap") != std::string::npos);
    v[2].startNs -= msToNs(600);
    CHECK(checkContinuity(v, msToNs(1))[0].find("overlap") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------
// Audio-only recording path (REQ-R05)
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R05: audio-only start needs no renderer, encoder or ffmpeg") {
    PrepareInputs in = goodInputs(false);                    // no video source, ffmpeg absent, no encoders
    in.encoders = ProbeResults{};
    auto r = prepareChecks(audioProfile(), in);
    CHECK(r.ok);
    CHECK_FALSE(r.encoder.has_value());
    auto v = prepareChecks(videoProfile(), in);              // the same machine cannot record video
    CHECK_FALSE(v.ok);
    CHECK(hasIssue(v, IssueCode::FfmpegMissing));
    CHECK(hasIssue(v, IssueCode::VideoSourceMissing));
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(audioProfile(), "cafe0001").ok());
    CHECK(s.completePrepare(in).ok);
    CHECK(s.state() == State::Recording);
}

TEST_CASE("REQ-R05: WAV header layout and PCM conversion") {
    const std::string h = makeWavHeader(48000, 2, 1000);
    REQUIRE(h.size() == kWavHeaderBytes);
    CHECK(h.compare(0, 4, "RIFF") == 0);
    CHECK(h.compare(8, 4, "WAVE") == 0);
    auto info = parseWavHeader(h + std::string(1000, '\0'));
    REQUIRE(info.ok());
    CHECK(info.value().rateHz == 48000);
    CHECK(info.value().channels == 2);
    CHECK(info.value().bitsPerSample == 24);
    CHECK(info.value().blockAlign == 6);
    CHECK(info.value().dataOffset == kWavHeaderBytes);
    CHECK(info.value().declaredDataBytes == 1000);
    CHECK_FALSE(parseWavHeader("RIFFxxxx").ok());
    CHECK_FALSE(parseWavHeader(std::string(100, 'x')).ok());
    const float in[] = {0.f, 1.f, -1.f, 0.5f, 2.0f, -3.0f, std::nanf("")};
    std::uint8_t out[21];
    pcm24FromFloat(in, 7, out);
    auto s24 = [&](int i) { std::int32_t v = out[i * 3] | (out[i * 3 + 1] << 8) | (out[i * 3 + 2] << 16); return v & 0x800000 ? v - 0x1000000 : v; };
    CHECK(s24(0) == 0);
    CHECK(s24(1) == 8388607);
    CHECK(s24(2) == -8388607);
    CHECK(s24(3) == 4194304);
    CHECK(s24(4) == 8388607);                                // clamped
    CHECK(s24(5) == -8388607);
    CHECK(s24(6) == 0);                                      // NaN
}

TEST_CASE("REQ-R05: recorder writes exact segments, finalizes headers and journals every step") {
    fake::MemFs fs; fake::MemJournal j;
    auto prof = audioProfile(1, secToNs(10));
    AudioOnlyRecorder rec(prof, "abcd1234", 1'700'000'000LL * kNsPerSec, fs, j);
    REQUIRE(rec.start().ok());
    const std::uint32_t kBlock = 4800;
    std::uint64_t idx = 0;
    auto blk = silenceBlock(kBlock, 1, 0.25f);
    for (int i = 0; i < 250; ++i) {                          // 25 s of mono audio
        REQUIRE(rec.write(view(blk, 1, 48000, idx)).ok());
        idx += kBlock;
    }
    REQUIRE(rec.stop().ok());
    CHECK_FALSE(rec.running());
    REQUIRE(rec.fileNames().size() == 3);
    CHECK(rec.fileNames()[0] == "20231114T221320Z_abcd1234_show_seg0001.wav");
    CHECK(rec.fileNames()[2] == "20231114T221320Z_abcd1234_show_seg0003.wav");
    const auto& f1 = fs.files[rec.fileNames()[0]];
    CHECK(f1.data.size() == kWavHeaderBytes + 480000u * 3);
    CHECK(f1.closed);
    auto h1 = parseWavHeader(f1.data.substr(0, 128));
    REQUIRE(h1.ok());
    CHECK(h1.value().declaredDataBytes == 480000u * 3);      // header finalized
    CHECK(fs.files[rec.fileNames()[2]].data.size() == kWavHeaderBytes + 240000u * 3);   // 5 s = 240000 mono frames
    const auto& closed = rec.closedSegments();
    REQUIRE(closed.size() == 3);
    CHECK(closed[0].endNs == secToNs(10)); CHECK(closed[1].startNs == secToNs(10)); CHECK(closed[1].endNs == secToNs(20));
    CHECK(closed[2].startNs == secToNs(20)); CHECK(closed[2].endNs == secToNs(25));
    CHECK(rec.framesWritten() == 25u * 48000u);
    // sample content: 0.25 -> 0x200000
    const auto& d = f1.data;
    CHECK(std::uint8_t(d[kWavHeaderBytes]) == 0x00); CHECK(std::uint8_t(d[kWavHeaderBytes + 1]) == 0x00); CHECK(std::uint8_t(d[kWavHeaderBytes + 2]) == 0x20);
    // journal: every record parses and the last is a clean End
    auto pj = parseJournal(j.text());
    CHECK(pj.corruptLines == 0);
    CHECK_FALSE(pj.tornTail);
    REQUIRE_FALSE(pj.records.empty());
    CHECK(pj.records.front().type == JournalType::Begin);
    CHECK(pj.records.back().type == JournalType::End);
    CHECK(pj.records.back().clean);
    int opens = 0, closes = 0;
    for (auto& r : pj.records) { if (r.type == JournalType::Open) ++opens; if (r.type == JournalType::Close) ++closes; }
    CHECK(opens == 3);
    CHECK(closes == 3);
    // a clean session recovers as all-Complete
    auto rep = recoverFromJournal(j.text(), fs);
    CHECK(rep.sessionEndedCleanly);
    REQUIRE(rep.segments.size() == 3);
    for (auto& s : rep.segments) CHECK(s.state == SegmentState::Complete);
    CHECK(rep.lost.empty());
    CHECK(rep.recoveredDurationNs == secToNs(25));
}

TEST_CASE("REQ-R05: size budget rotates stereo segments at exact frame counts") {
    fake::MemFs fs; fake::MemJournal j;
    auto prof = audioProfile(2, Limits::kMaxSegmentNs, 16 * MiB);
    AudioOnlyRecorder rec(prof, "abcd1234", 0, fs, j);
    REQUIRE(rec.start().ok());
    auto blk = silenceBlock(48000, 2, 0.f);
    std::uint64_t idx = 0;
    for (int i = 0; i < 130; ++i) { REQUIRE(rec.write(view(blk, 2, 48000, idx)).ok()); idx += 48000; }
    REQUIRE(rec.stop().ok());
    const std::uint64_t per = (16 * MiB - kWavHeaderBytes) / 6;
    const auto& c = rec.closedSegments();
    REQUIRE(c.size() == 3);
    CHECK(c[0].frames == per);
    CHECK(c[0].reason == CutReason::Size);
    CHECK(c[1].startFrame == per);
    CHECK(c[0].endNs == c[1].startNs);
    CHECK(c[2].startFrame == 2 * per);
    CHECK(c[0].frames * 6 + kWavHeaderBytes <= 16 * MiB);
    std::uint64_t sum = 0;
    for (auto& s : c) sum += s.frames;
    CHECK(sum == 130u * 48000u);
}

TEST_CASE("REQ-R05: capture discontinuities are filled with silence and journaled; overlaps are trimmed") {
    fake::MemFs fs; fake::MemJournal j;
    AudioOnlyRecorder rec(audioProfile(1, secToNs(60)), "abcd1234", 0, fs, j);
    REQUIRE(rec.start().ok());
    auto blk = silenceBlock(4800, 1, 0.5f);
    REQUIRE(rec.write(view(blk, 1, 48000, 0)).ok());
    REQUIRE(rec.write(view(blk, 1, 48000, 9600)).ok());                 // 4800 frames missing
    CHECK(rec.gapCount() == 1);
    CHECK(rec.silenceFramesInserted() == 4800);
    CHECK(rec.framesWritten() == 4800u * 3);
    REQUIRE(rec.write(view(blk, 1, 48000, 12000)).ok());                // overlaps by 2400 frames (expected 14400)
    CHECK(rec.framesWritten() == 4800u * 3 + 2400);
    REQUIRE(rec.write(view(blk, 1, 48000, 0, 2)).ok());                 // new capture generation: restart noted
    CHECK(rec.gapCount() == 2);
    CHECK_FALSE(rec.write(view(silenceBlock(480, 2), 2, 48000, 0)).ok());   // wrong channel count is rejected
    REQUIRE(rec.stop().ok());
    bool sawGap = false;
    for (auto& r : parseJournal(j.text()).records) if (r.type == JournalType::Gap) sawGap = true;
    CHECK(sawGap);
}

TEST_CASE("REQ-R04: disk-full during audio recording surfaces DiskFull and earlier segments stay recoverable") {
    fake::MemFs fs; fake::MemJournal j;
    AudioOnlyRecorder rec(audioProfile(1, secToNs(10)), "abcd1234", 0, fs, j);
    REQUIRE(rec.start().ok());
    auto blk = silenceBlock(48000, 1, 0.1f);
    std::uint64_t idx = 0;
    Status last = okStatus();
    fs.failAfterBytes = 12 * 48000 * 3;                                  // runs out in the middle of segment 2
    for (int i = 0; i < 20 && last.ok(); ++i) { last = rec.write(view(blk, 1, 48000, idx)); idx += 48000; }
    REQUIRE_FALSE(last.ok());
    CHECK(last.error().category == ErrorCategory::DiskFull);
    (void)rec.stop(false);
    auto rep = recoverFromJournal(j.text(), fs);
    REQUIRE(rep.segments.size() >= 1);
    CHECK(rep.segments[0].state == SegmentState::Complete);              // segment 1 is whole
    CHECK(rep.segments[0].endNs == secToNs(10));
    CHECK_FALSE(rep.sessionEndedCleanly);
}

TEST_CASE("REQ-R05: header recovery after power loss repairs the WAV and discloses the lost tail") {
    fake::MemFs fs; fake::MemJournal j;
    AudioOnlyRecorder rec(audioProfile(2, secToNs(60)), "abcd1234", 0, fs, j);
    REQUIRE(rec.start().ok());
    auto blk = silenceBlock(4800, 2, 0.1f);
    std::uint64_t idx = 0;
    for (int i = 0; i < 35; ++i) { REQUIRE(rec.write(view(blk, 2, 48000, idx)).ok()); idx += 4800; }   // 3.5 s
    const std::string name = rec.fileNames()[0];
    fs.crash();                                                          // no stop(): power cut
    fs.files[name].data.append("\x01\x02\x03\x04\x05", 5);               // torn, partial last frame
    auto before = parseWavHeader(fs.files[name].data.substr(0, 128));
    REQUIRE(before.ok());
    CHECK(before.value().declaredDataBytes == 3u * 48000 * 6);           // header was last patched at the 3 s flush
    // Make the header wrong on purpose (worst case: crash before the header patch reached the disk)
    fs.files[name].data.replace(64, 4, std::string("\0\0\0\0", 4));
    auto rep = recoverFromJournal(j.text(), fs);
    CHECK_FALSE(rep.sessionEndedCleanly);
    REQUIRE(rep.segments.size() == 1);
    CHECK(rep.segments[0].state == SegmentState::RecoveredWav);
    CHECK(rep.segments[0].endNs == secToNs(3));                          // 3 s were durable
    CHECK(rep.recoveredDurationNs == secToNs(3));
    REQUIRE(rep.lost.size() == 1);
    CHECK(rep.lost[0].fromNs == secToNs(3));
    CHECK(rep.lost[0].maxUnflushedNs == Limits::kFlushIntervalNs);
    CHECK_FALSE(rep.notes.empty());
    auto after = parseWavHeader(fs.files[name].data.substr(0, 128));
    REQUIRE(after.ok());
    CHECK(after.value().declaredDataBytes == 3u * 48000 * 6);
    CHECK((fs.files[name].data.size() - kWavHeaderBytes) % 6 == 0);      // whole frames only
}

TEST_CASE("REQ-R04: journal tolerates a torn last line and flags corrupt lines") {
    fake::MemJournal j;
    JournalWriter w(j);
    REQUIRE(w.begin("abcd1234", "hash", ProfileKind::VideoMatroska, 1).ok());
    REQUIRE(w.segmentOpened(1, "a.mkv", 0).ok());
    REQUIRE(w.segmentClosed(1, 100, secToNs(900)).ok());
    REQUIRE(w.segmentOpened(2, "b.mkv", secToNs(900)).ok());
    REQUIRE(w.progress(2, 50, secToNs(905)).ok());
    std::string text = j.text();
    auto clean = parseJournal(text);
    CHECK(clean.records.size() == 5);
    CHECK(clean.corruptLines == 0);
    // torn tail
    const std::string torn = text + j.lines.back().substr(0, 30);
    auto pt = parseJournal(torn);
    CHECK(pt.records.size() == 5);
    CHECK(pt.tornTail);
    CHECK(pt.corruptLines == 1);
    // flipped byte in the middle
    std::string flipped = text;
    flipped[flipped.find("a.mkv")] = 'X';
    auto pf = parseJournal(flipped);
    CHECK(pf.records.size() == 4);
    CHECK(pf.corruptLines == 1);
    CHECK_FALSE(pf.tornTail);
    // recovery of a video session: closed segment complete (size matches), active one is only Unverified
    fake::MemFs fs;
    fs.files["a.mkv"].data = std::string(100, 'a');
    fs.files["b.mkv"].data = std::string(60, 'b');
    auto rep = recoverFromJournal(text, fs);
    REQUIRE(rep.segments.size() == 2);
    CHECK(rep.segments[0].state == SegmentState::Complete);
    CHECK(rep.segments[1].state == SegmentState::Unverified);            // never claimed playable
    CHECK(rep.segments[1].endNs == secToNs(905));
    CHECK(rep.recoveredDurationNs == secToNs(900));                      // only the complete segment counts
    CHECK(rep.lost.size() == 1);
    fs.files["a.mkv"].data = std::string(99, 'a');
    CHECK(recoverFromJournal(text, fs).segments[0].state == SegmentState::Mismatch);
    fs.files.erase("a.mkv");
    CHECK(recoverFromJournal(text, fs).segments[0].state == SegmentState::Missing);
    CHECK_FALSE(recoverFromJournal("", fs).journalPresent);
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
}

TEST_CASE("REQ-R05: real files through StdioRecorderFs and FileJournalSink") {
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / "ss_record_test";
    fsys::remove_all(dir);
    fsys::create_directories(dir);
    {
        StdioRecorderFs sfs(dir.string());
        FileJournalSink js;
        REQUIRE(js.open((dir / "s.journal").string()).ok());
        AudioOnlyRecorder rec(audioProfile(1, secToNs(10)), "abcd1234", 0, sfs, js);
        REQUIRE(rec.start().ok());
        auto blk = silenceBlock(48000, 1, 0.3f);
        std::uint64_t idx = 0;
        for (int i = 0; i < 12; ++i) { REQUIRE(rec.write(view(blk, 1, 48000, idx)).ok()); idx += 48000; }
        REQUIRE(rec.stop().ok());
        REQUIRE(rec.fileNames().size() == 2);
        CHECK(fsys::file_size(dir / rec.fileNames()[0]) == kWavHeaderBytes + 480000u * 3);
        CHECK(fsys::file_size(dir / rec.fileNames()[1]) == kWavHeaderBytes + 96000u * 3);
        std::uint64_t sz = 0;
        CHECK(sfs.fileSize(rec.fileNames()[0], sz));
        CHECK_FALSE(sfs.fileSize("../escape", sz));
        CHECK_FALSE(sfs.createFile("sub/dir.wav").ok());
        // simulate a crash after the fact: chop 100 bytes (partial frames) off the second file and corrupt its header
        fsys::resize_file(dir / rec.fileNames()[1], kWavHeaderBytes + 96000u * 3 - 100);
        js.close();
        std::ifstream in(dir / "s.journal");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // drop the Close/End lines of segment 2 so it looks unclosed
        std::string cut;
        { std::istringstream ls(text); std::string line; while (std::getline(ls, line)) { if (line.find("\"t\":\"end\"") != std::string::npos) continue; if (line.find("\"t\":\"close\"") != std::string::npos && line.find("\"seg\":2") != std::string::npos) continue; cut += line + "\n"; } }
        auto rep = recoverFromJournal(cut, sfs);
        REQUIRE(rep.segments.size() == 2);
        CHECK(rep.segments[0].state == SegmentState::Complete);
        CHECK(rep.segments[1].state == SegmentState::RecoveredWav);
        CHECK(rep.segments[1].note.find("partial frame") != std::string::npos);
        const auto sz2 = fsys::file_size(dir / rec.fileNames()[1]);
        CHECK((sz2 - kWavHeaderBytes) % 3 == 0);
        auto hdr = sfs.readPrefix(rec.fileNames()[1], 128);
        REQUIRE(hdr.ok());
        CHECK(parseWavHeader(hdr.value()).value().declaredDataBytes == sz2 - kWavHeaderBytes);
    }
    fsys::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("REQ-R01: markers are stamped with the session clock and mapped to segments") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(videoProfile(), "deadbeef").ok());
    REQUIRE(s.completePrepare(goodInputs()).ok);
    s.onSegmentOpened(1, "a.mkv", 0);
    clock.advance(secToNs(10));
    auto m1 = s.addMarker("Drop at chorus");
    REQUIRE(m1.ok());
    CHECK(m1.value().sessionNs == secToNs(10));
    CHECK(m1.value().segment == 1);
    CHECK(m1.value().offsetInSegmentNs == secToNs(10));
    s.onSegmentClosed(1, 10, secToNs(900));
    s.onSegmentOpened(2, "b.mkv", secToNs(900));
    auto m2 = s.addMarkerAt("Encore", secToNs(950));
    REQUIRE(m2.ok());
    CHECK(m2.value().segment == 2);
    CHECK(m2.value().offsetInSegmentNs == secToNs(50));
    auto ctl = s.addMarker(std::string("line1\nline2\x01"));
    REQUIRE(ctl.ok());
    CHECK(ctl.value().label == "line1 line2 ");
    auto longm = s.addMarker(std::string(500, 'x'));
    REQUIRE(longm.ok());
    CHECK(longm.value().label.size() == 128);                              // truncated, never rejected during a live press
    CHECK_FALSE(s.addMarker("bad \xC0\xAF").ok());
    CHECK(s.summary().markers.size() == 4);
    const std::string csv = markersCsv({Marker{1, secToNs(1), "=SUM(A1)", 1, 0}, Marker{2, secToNs(2), "say \"hi\", ok", 1, 0}});
    CHECK(csv.find("\"'=SUM(A1)\"") != std::string::npos);                 // spreadsheet formula guard
    CHECK(csv.find("\"say \"\"hi\"\", ok\"") != std::string::npos);
    const std::string meta = markersFfmetadata(s.summary().markers, secToNs(2000));
    CHECK(meta.rfind(";FFMETADATA1", 0) == 0);
    CHECK(meta.find("START=10000") != std::string::npos);
    CHECK(markersFfmetadata({Marker{1, 0, "a=b;c", 0, 0}}, secToNs(1)).find("title=a\\=b\\;c") != std::string::npos);
    REQUIRE(s.requestStop().ok());
    s.onWriterStopped(true);
    CHECK_FALSE(s.addMarker("late").ok());
}

TEST_CASE("REQ-R01: marker count is capped") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(videoProfile(), "deadbeef").ok());
    REQUIRE(s.completePrepare(goodInputs()).ok);
    for (std::size_t i = 0; i < Limits::kMaxMarkers; ++i) REQUIRE(s.addMarker("m").ok());
    auto r = s.addMarker("one too many");
    CHECK_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::ResourceExhausted);
}

TEST_CASE("REQ-R02: dynamics settings and activity are written to the session metadata") {
    FakeClock clock;
    RecordSession s(clock);
    REQUIRE(s.beginPrepare(videoProfile(VideoCodecPolicy::PreferH264FallbackMjpeg, msToNs(5)), "deadbeef").ok());
    REQUIRE(s.completePrepare(goodInputs()).ok);
    s.onSegmentOpened(1, "a.mkv", 0);
    DynamicsMetadata d; d.bypassed = true; d.limiterActiveBlocks = 1234; d.inputClipWarningSeen = true; d.inputClipEvents = 3; d.processingDelayNs = msToNs(5);
    auto j = nlohmann::json::parse(s.metadataJson(d));
    CHECK(j["dynamics"]["bypassed"] == true);
    CHECK(j["dynamics"]["limiterActiveBlocks"] == 1234);
    CHECK(j["dynamics"]["inputClipWarningSeen"] == true);
    CHECK(j["dynamics"]["processingDelayNs"] == 5'000'000);
    CHECK(j["audioDelayCompensationNs"] == 5'000'000);
    CHECK(j["profileHash"] == s.summary().profileHash);
    CHECK(j["segments"].size() == 1);
    CHECK(j["sessionId"] == "deadbeef");
    CHECK(j.dump().find("/rec") == std::string::npos);                     // no output path in the sidecar
}

// ---------------------------------------------------------------------------------------------------------------
// ffmpeg argv (REQ-R01, REQ-R03)
// ---------------------------------------------------------------------------------------------------------------
namespace {
RecordCommandInput cmdInput(const RecordProfile& p, const char* enc = "h264_nvenc", VideoCodec codec = VideoCodec::H264) {
    RecordCommandInput in(p);
    in.encoder.name = enc; in.encoder.codec = codec; in.encoder.hardware = codec == VideoCodec::H264;
    in.video.endpoint = "pipe:3"; in.video.width = 1280; in.video.height = 720; in.video.fpsNum = 30; in.video.fpsDen = 1;
    in.audio.endpoint = "pipe:4"; in.audio.rateHz = 48000; in.audio.channels = 2;
    in.sessionId = "0a1b2c3d"; in.sessionStartWallUnixNs = 1'700'000'000LL * kNsPerSec;
    return in;
}
std::size_t indexOf(const std::vector<std::string>& v, const std::string& s, std::size_t from = 0) {
    for (std::size_t i = from; i < v.size(); ++i) if (v[i] == s) return i;
    return v.size();
}
std::string after(const std::vector<std::string>& v, const std::string& key, std::size_t from = 0) {
    const auto i = indexOf(v, key, from);
    return i + 1 < v.size() ? v[i + 1] : std::string("<none>");
}
}  // namespace

TEST_CASE("REQ-R01: record argv is a vector with raw video + PCM inputs, H.264, PCM audio and Matroska segments") {
    auto in = cmdInput(videoProfile());
    auto r = FfmpegCommandBuilder::buildRecord(in);
    REQUIRE(r.ok());
    const auto& a = r.value().argv;
    CHECK(a[0] == "ffmpeg");
    CHECK(indexOf(a, "-nostdin") < a.size());
    CHECK(after(a, "-video_size") == "1280x720");
    CHECK(after(a, "-framerate") == "30/1");
    CHECK(after(a, "-pix_fmt") == "rgba");                                 // first -pix_fmt is the input
    const auto firstI = indexOf(a, "-i");
    CHECK(a[firstI + 1] == "pipe:3");
    CHECK(a[indexOf(a, "-i", firstI + 1) + 1] == "pipe:4");
    CHECK(after(a, "-c:v") == "h264_nvenc");
    CHECK(after(a, "-b:v") == "6000k");
    CHECK(after(a, "-g") == "60");                                         // 2 s keyframe interval at 30 fps
    CHECK(after(a, "-c:a") == "pcm_s24le");
    CHECK(after(a, "-f", indexOf(a, "-c:a")) == "segment");
    CHECK(after(a, "-segment_format") == "matroska");
    CHECK(after(a, "-segment_time") == "900");
    CHECK(after(a, "-force_key_frames") == "expr:gte(t,n_forced*900)");
    CHECK(after(a, "-reset_timestamps") == "1");
    CHECK(after(a, "-segment_start_number") == "1");
    CHECK(after(a, "-segment_list_type") == "csv");
    CHECK(a.back() == "/rec/20231114T221320Z_0a1b2c3d_Friday_Night_Main_Room_seg%04d.mkv");
    CHECK(r.value().segmentListPath == "/rec/20231114T221320Z_0a1b2c3d_Friday_Night_Main_Room.segments.csv");
    // no element is a shell string: none contains a space or quote
    for (auto& x : a) { CHECK(x.find(' ') == std::string::npos); CHECK(x.find('"') == std::string::npos); CHECK(x.find('\'') == std::string::npos); }
    CHECK(r.value().segmentDurationNs == secToNs(900));
}

TEST_CASE("REQ-R01: MJPEG fallback command has no forced H.264 keyframes and a size-derived segment time") {
    RecordProfileSpec s; s.kind = ProfileKind::VideoMatroska; s.outputDir = "/rec"; s.video.width = 1920; s.video.height = 1080; s.video.fpsNum = 30;
    auto p = RecordProfile::create(s).value();
    auto in = cmdInput(p, "mjpeg", VideoCodec::Mjpeg);
    in.video.width = 1920; in.video.height = 1080;
    auto r = FfmpegCommandBuilder::buildRecord(in);
    REQUIRE(r.ok());
    const auto& a = r.value().argv;
    CHECK(after(a, "-c:v") == "mjpeg");
    CHECK(after(a, "-q:v") == "5");
    CHECK(indexOf(a, "-force_key_frames") == a.size());
    CHECK(indexOf(a, "-b:v") == a.size());
    CHECK(r.value().segmentDurationNs < secToNs(900));                     // 2 GiB budget at ~11 MB/s cannot hold 15 min
    CHECK(r.value().segmentDurationNs >= Limits::kMinSegmentNs);
    CHECK(std::stoll(after(a, "-segment_time")) * kNsPerSec == r.value().segmentDurationNs);
}

TEST_CASE("REQ-R03: audio PTS offset compensates the supplied processing delay") {
    // Same capture start, 5 ms of lookahead delay: audio content is 5 ms late, so it must be moved 5 ms EARLIER,
    // which is expressed as video being delayed by 5 ms instead (offsets are never negative).
    auto o = ff::computeInputOffsets({secToNs(100), secToNs(100), msToNs(5)});
    CHECK(o.audio == 0);
    CHECK(o.video == msToNs(5));
    // audio started 250 ms after video: audio is delayed 250 ms
    auto o2 = ff::computeInputOffsets({secToNs(100), secToNs(100) + msToNs(250), 0});
    CHECK(o2.audio == msToNs(250));
    CHECK(o2.video == 0);
    // both: audio started 250 ms late and has 5 ms processing delay -> 245 ms
    auto o3 = ff::computeInputOffsets({secToNs(100), secToNs(100) + msToNs(250), msToNs(5)});
    CHECK(o3.audio == msToNs(245));
    CHECK(o3.video == 0);
    // audio started before video
    auto o4 = ff::computeInputOffsets({secToNs(100) + msToNs(40), secToNs(100), 0});
    CHECK(o4.audio == 0);
    CHECK(o4.video == msToNs(40));
    // the argv carries them as -itsoffset before the matching -i
    auto in = cmdInput(videoProfile(VideoCodecPolicy::PreferH264FallbackMjpeg, msToNs(5)));      // profile carries the 5 ms delay
    in.sync = {secToNs(100), secToNs(100) + msToNs(250), 0};
    auto r = FfmpegCommandBuilder::buildRecord(in);
    REQUIRE(r.ok());
    const auto& a = r.value().argv;
    const auto first = indexOf(a, "-itsoffset");
    CHECK(a[first + 1] == "0.000000");
    CHECK(a[first + 2] == "-i");
    CHECK(r.value().offsets.audio == msToNs(245));
    const auto second = indexOf(a, "-itsoffset", first + 1);
    CHECK(a[second + 1] == "0.245000");
    CHECK(a[second + 2] == "-i");
    CHECK(a[indexOf(a, "-i", second) + 1] == "pipe:4");
    CHECK(ff::secondsArg(-1500000) == "-0.001500");
    CHECK(ff::secondsArg(0) == "0.000000");
    auto in0 = cmdInput(videoProfile());                                  // no compensation supplied
    in0.sync = {secToNs(100), secToNs(100) + msToNs(250), 0};
    auto r0 = FfmpegCommandBuilder::buildRecord(in0);
    REQUIRE(r0.ok());
    CHECK(after(r0.value().argv, "-itsoffset", indexOf(r0.value().argv, "-itsoffset") + 1) == "0.250000");   // changes with the supplied value
    CHECK(r0.value().offsets.audio == msToNs(250));
}

TEST_CASE("REQ-R01: builder rejects unsafe endpoints, URLs, GPL encoders and mismatched inputs") {
    CHECK(ff::checkEndpoint("pipe:3").ok());
    CHECK(ff::checkEndpoint("\\\\.\\pipe\\ss_video").ok());
    CHECK(ff::checkEndpoint("/run/user/1000/ss.fifo").ok());
    CHECK(ff::checkEndpoint("C:\\tmp\\v.pipe").ok());
    for (const char* bad : {"", "-i", "rtmp://example.org/live", "http://x/y", "file:foo", "fd:3", "pipe:", "pipe:3;rm", "relative/path", "pipe:3\n-y",
                            "tcp:127.0.0.1:1"})
        CHECK_MESSAGE(!ff::checkEndpoint(bad).ok(), bad);
    auto base = cmdInput(videoProfile());
    { auto in = base; in.encoder.name = "libx264"; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.encoder.name = "not_an_encoder"; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.encoder.codec = VideoCodec::Mjpeg; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.video.endpoint = "rtmp://evil/x"; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.audio.endpoint = in.video.endpoint; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.video.width = 640; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.audio.rateHz = 44100; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.sessionId = "../../x"; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = base; in.ffmpegPath = ""; CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }
    { auto in = cmdInput(audioProfile()); CHECK_FALSE(FfmpegCommandBuilder::buildRecord(in).ok()); }   // audio-only never uses ffmpeg
    RecordProfileSpec s; s.outputDir = "relative/dir";
    CHECK_FALSE(FfmpegCommandBuilder::buildRecord(cmdInput(RecordProfile::create(s).value())).ok());
    s.outputDir = "rtmp://host/app";
    CHECK_FALSE(FfmpegCommandBuilder::buildRecord(cmdInput(RecordProfile::create(s).value())).ok());
    s.outputDir = "/rec/100%/take";                                         // '%' in the folder is escaped for the segment pattern
    auto pct = FfmpegCommandBuilder::buildRecord(cmdInput(RecordProfile::create(s).value()));
    REQUIRE(pct.ok());
    CHECK(pct.value().segmentPattern.rfind("/rec/100%%/take/", 0) == 0);
    CHECK(pct.value().segmentListPath.rfind("/rec/100%/take/", 0) == 0);
    CHECK_FALSE(ff::checkArgv({"ffmpeg", "a\nb"}).ok());
    CHECK_FALSE(ff::checkArgv({}).ok());
}

// ---------------------------------------------------------------------------------------------------------------
// Record-while-broadcasting coexistence
// ---------------------------------------------------------------------------------------------------------------
namespace {
MediaChunk chunk(std::uint64_t seq, std::size_t bytes, bool key = true) {
    MediaChunk c; c.seq = seq; c.ptsNs = Ns(seq) * msToNs(33); c.durationNs = msToNs(33); c.keyframe = key;
    c.data = std::make_shared<const std::vector<std::uint8_t>>(bytes, std::uint8_t(0));
    return c;
}
}  // namespace

TEST_CASE("REQ-B04: a stalled network consumer never stalls or starves the disk consumer") {
    SinkFanout f(10'000, 3'000);
    f.setNetworkEnabled(true);
    std::uint64_t diskGot = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        f.publish(chunk(i, 1000));                       // network consumer never drains
        MediaChunk c;
        if (f.disk().pop(c)) { CHECK(c.seq == diskGot); ++diskGot; }
    }
    CHECK(diskGot == 1000);                              // disk saw every chunk, in order
    CHECK(f.disk().stats().dropped == 0);
    CHECK(f.network().stats().dropped >= 990 - 3);      // the network queue shed load
    CHECK(f.network().stats().maxDepthBytes <= 3'000);
    CHECK(f.network().stats().depthBytes <= 3'000);
    CHECK(f.maxMemoryBytes() == 13'000);
    // DropOldest: what remains queued is the freshest media
    MediaChunk c;
    REQUIRE(f.network().pop(c));
    CHECK(c.seq >= 997);
}

TEST_CASE("REQ-P05: queues stay bounded when both consumers stall; disk loss is reported, not hidden") {
    SinkFanout f(5'000, 2'000);
    f.setNetworkEnabled(true);
    for (std::uint64_t i = 0; i < 100; ++i) f.publish(chunk(i, 1000));
    CHECK(f.disk().stats().depthBytes <= 5'000);
    CHECK(f.network().stats().depthBytes <= 2'000);
    CHECK(f.disk().stats().dropped == 95);               // DropNewest: first 5 kept, 95 lost
    auto loss = f.disk().takeLosses();
    REQUIRE(loss.size() == 1);                           // contiguous drops merge into one range
    CHECK(loss[0].fromNs == Ns(5) * msToNs(33));
    CHECK(loss[0].lengthNs == Ns(95) * msToNs(33));
    CHECK(f.disk().takeLosses().empty());
    MediaChunk c;
    REQUIRE(f.disk().pop(c));
    CHECK(c.seq == 0);                                   // the oldest data is what was preserved
    f.publish(chunk(100, 1000));                         // room again after the pop
    f.setNetworkEnabled(false);
    CHECK(f.network().size() == 0);                      // network off: queue emptied, nothing new queued
    f.publish(chunk(101, 10));
    CHECK(f.network().size() == 0);
}

TEST_CASE("REQ-B04: encoded packets resync at a keyframe after the network queue drops") {
    ConsumerQueue q("net", 3000, OverflowPolicy::DropOldest, true);
    for (std::uint64_t i = 0; i < 3; ++i) CHECK(q.push(chunk(i, 1000, i == 0)));
    CHECK_FALSE(q.push(chunk(3, 1000, false)));         // overflow drops the keyframe and the deltas behind it: queue is empty, waiting for a key
    MediaChunk c;
    CHECK_FALSE(q.pop(c));                               // the consumer never starts mid-GOP
    CHECK(q.takeKeyframeRequest());
    CHECK_FALSE(q.takeKeyframeRequest());
    CHECK_FALSE(q.push(chunk(4, 1000, false)));         // waiting for a keyframe: deltas are discarded
    CHECK(q.push(chunk(5, 1000, true)));
    q.clear();                                           // reconnect: abandon queued media
    CHECK(q.size() == 0);
    CHECK_FALSE(q.push(chunk(6, 100, false)));
    CHECK(q.push(chunk(7, 100, true)));
    CHECK_FALSE(q.push(chunk(8, 5000, true)));          // bigger than the queue
}

TEST_CASE("REQ-B04: coexistence plan keeps independent encoders and explains why") {
    EncodeSignature rec; rec.videoCodec = "h264"; rec.width = 1280; rec.height = 720; rec.fpsNum = 30; rec.videoBitrateKbps = 6000;
    rec.audioCodec = "pcm_s24le"; rec.audioRateHz = 48000; rec.audioChannels = 2;
    EncodeSignature cast = rec; cast.videoBitrateKbps = 2500; cast.audioCodec = "aac"; cast.audioBitrateKbps = 128;
    auto p = planCoexistence(rec, cast);
    CHECK_FALSE(p.wouldMatch);
    CHECK_FALSE(p.shareEncodedPackets);
    CHECK(p.encoderInstances == 2);
    CHECK(p.reasons.size() >= 3);
    auto same = planCoexistence(rec, rec);
    CHECK(same.wouldMatch);
    CHECK_FALSE(same.shareEncodedPackets);              // independent muxers by design
    CHECK(same.encoderInstances == 2);
}
