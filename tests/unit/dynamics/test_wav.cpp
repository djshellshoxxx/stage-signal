// T-REC (audio-only part): 24-bit WAV round trip, segmentation, header finalisation, recovery, gap policy.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "doctest/doctest.h"
#include "dsp_helpers.hpp"
#include "ss/base/types.hpp"
#include "ss/dynamics/wav_reader.hpp"
#include "ss/dynamics/wav_writer.hpp"

using namespace ss;
using namespace ss::dynamics;

namespace {
constexpr std::uint32_t kRate = 48000;

// float exactly representable in 24 bit: k / 2^23
float fromInt(std::int32_t k) { return static_cast<float>(k) / 8388608.0f; }

std::vector<std::int32_t> makeInts(std::size_t frames, std::uint16_t ch, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<std::int32_t> v(frames * ch);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const std::uint32_t pick = rng.below(20);
        if (pick == 0) v[i] = -8388608;
        else if (pick == 1) v[i] = 8388607;
        else if (pick == 2) v[i] = 0;
        else v[i] = static_cast<std::int32_t>(rng.below(16777216)) - 8388608;
    }
    return v;
}
std::vector<float> toFloats(const std::vector<std::int32_t>& v) {
    std::vector<float> f(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) f[i] = fromInt(v[i]);
    return f;
}

std::vector<std::int32_t> readAll(const MemorySinkFactory& fac) {
    std::vector<std::int32_t> all;
    for (const auto& seg : fac.segments) {
        auto r = readWav(*seg);
        REQUIRE(r.ok());
        all.insert(all.end(), r.value().samples.begin(), r.value().samples.end());
    }
    return all;
}

// Sinks that fail once a shared byte budget is used up (disk full simulation).
class LimitedSink final : public IByteSink {
public:
    LimitedSink(std::shared_ptr<std::vector<std::uint8_t>> s, std::shared_ptr<std::uint64_t> remaining)
        : mem_(std::move(s)), remaining_(std::move(remaining)) {}
    Status write(const std::uint8_t* d, std::size_t n) override {
        if (n > *remaining_) return makeError(ErrorCategory::DiskFull, "disk full (injected)");
        *remaining_ -= n;
        return mem_.write(d, n);
    }
    Status patch(std::uint64_t o, const std::uint8_t* d, std::size_t n) override { return mem_.patch(o, d, n); }
    Status flush() override { return okStatus(); }
    Status close() override { return okStatus(); }
    std::uint64_t size() const override { return mem_.size(); }
private:
    MemorySink mem_;
    std::shared_ptr<std::uint64_t> remaining_;
};
class LimitedFactory final : public ISinkFactory {
public:
    explicit LimitedFactory(std::uint64_t budget) : remaining_(std::make_shared<std::uint64_t>(budget)) {}
    Result<OpenedSink> open(const SegmentInfo& info) override {
        auto store = std::make_shared<std::vector<std::uint8_t>>();
        segments.push_back(store);
        return OpenedSink{std::make_unique<LimitedSink>(store, remaining_), "limited_" + std::to_string(info.index)};
    }
    std::vector<std::shared_ptr<std::vector<std::uint8_t>>> segments;
private:
    std::shared_ptr<std::uint64_t> remaining_;
};
}  // namespace

TEST_CASE("REQ-R05: 24-bit WAV round trip is bit exact (mono and stereo, extreme values)") {
    for (std::uint16_t ch : {std::uint16_t{1}, std::uint16_t{2}}) {
        MemorySinkFactory fac;
        WavWriterConfig cfg; cfg.channels = ch; cfg.sampleRateHz = kRate;
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        const auto ints = makeInts(20000, ch, 11 + ch);
        const auto fl = toFloats(ints);
        std::uint64_t idx = 0;
        Rng rng(2);
        while (idx < 20000) {
            const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(1 + rng.below(3000), 20000 - idx));
            REQUIRE(w.write(fl.data() + idx * ch, n, idx).ok());
            idx += n;
        }
        REQUIRE(w.stop().ok());
        REQUIRE(fac.segments.size() == 1);
        auto r = readWav(*fac.segments[0]);
        REQUIRE(r.ok());
        CHECK(r.value().info.channels == ch);
        CHECK(r.value().info.sampleRateHz == kRate);
        CHECK(r.value().info.bitsPerSample == 24);
        CHECK(r.value().samples == ints);
        CHECK(wavToFloat(r.value()) == fl);   // float path is exact too
        CHECK(fac.segments[0]->size() == kWavHeaderBytes + ints.size() * 3);
        CHECK(w.clippedSamples() == 0);
    }
}

TEST_CASE("REQ-R05: quantisation rule - round half up, clip to 24-bit range, NaN to zero, dither off by default") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.channels = 1;
    WavSegmentWriter w(cfg, fac);
    REQUIRE(w.start(0).ok());
    const float in[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, std::nanf(""), INFINITY, 0.25f / 8388608.0f, 0.75f / 8388608.0f, -0.5f / 8388608.0f, -0.75f / 8388608.0f};
    REQUIRE(w.write(in, 12, 0).ok());
    REQUIRE(w.stop().ok());
    auto r = readWav(*fac.segments[0]);
    REQUIRE(r.ok());
    const std::vector<std::int32_t> expect = {4194304, -4194304, 8388607, -8388608, 8388607, -8388608, 0, 0, 0, 1, 0, -1};
    CHECK(r.value().samples == expect);
    CHECK(w.clippedSamples() == 3);   // +1.0, +2.0, -2.0 clip; -1.0 is exactly representable; NaN/Inf are written as 0 and not counted
}

TEST_CASE("REQ-R05: optional TPDF dither is deterministic, bounded to +-1 LSB, and off by default") {
    auto run = [](bool dither, std::uint64_t seed) {
        MemorySinkFactory fac;
        WavWriterConfig cfg; cfg.channels = 1; cfg.dither = dither; cfg.ditherSeed = seed;
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        std::vector<float> in(4000, 0.123456789f);
        REQUIRE(w.write(in.data(), 4000, 0).ok());
        REQUIRE(w.stop().ok());
        return readWav(*fac.segments[0]).value().samples;
    };
    const auto plain = run(false, 1), d1 = run(true, 1), d1b = run(true, 1), d2 = run(true, 2);
    for (auto v : plain) CHECK(v == plain[0]);
    CHECK(d1 == d1b);
    CHECK(d1 != d2);
    bool varies = false;
    for (auto v : d1) { CHECK(std::abs(v - plain[0]) <= 1); varies |= v != plain[0]; }
    CHECK(varies);
}

TEST_CASE("REQ-R05: segments split at exact sample boundaries with no overlap or loss") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.segmentSeconds = 1.0;   // 48000 frames per segment
    WavSegmentWriter w(cfg, fac);
    const std::uint64_t start = 123456;
    REQUIRE(w.start(start).ok());
    CHECK(w.segmentFrames() == 48000);
    const std::size_t total = 2 * 48000 + 24000;
    const auto ints = makeInts(total, 2, 77);
    const auto fl = toFloats(ints);
    std::uint64_t idx = 0;
    Rng rng(4);
    while (idx < total) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(1 + rng.below(7000), total - idx));
        REQUIRE(w.write(fl.data() + idx * 2, n, start + idx).ok());
        idx += n;
    }
    REQUIRE(w.stop().ok());
    REQUIRE(fac.segments.size() == 3);
    const auto& j = w.journal();
    REQUIRE(j.size() == 3);
    CHECK(j[0].firstSample == start);            CHECK(j[0].endSample == start + 48000);
    CHECK(j[1].firstSample == start + 48000);    CHECK(j[1].endSample == start + 96000);
    CHECK(j[2].firstSample == start + 96000);    CHECK(j[2].endSample == start + 96000 + 24000);
    CHECK(j[0].startNs == 0);
    CHECK(j[1].startNs == kNsPerSec);
    CHECK(j[2].startNs == 2 * kNsPerSec);
    CHECK(j[2].endNs == samplesToNs(120000, kRate));
    for (const auto& e : j) { CHECK(e.state == SegmentState::Completed); CHECK(e.headerFinalised); CHECK(e.frames == e.endSample - e.firstSample); }
    CHECK(j[0].name == "mem_0000.wav");
    for (std::size_t i = 0; i < 3; ++i) CHECK(readWav(*fac.segments[i]).value().frames() == j[i].frames);
    CHECK(readAll(fac) == ints);                 // concatenation equals the input: no overlap, no loss
    CHECK(w.totalFramesWritten() == total);
}

TEST_CASE("REQ-R05: a stream ending exactly on a boundary does not open an empty extra segment") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.segmentSeconds = 0.5;
    WavSegmentWriter w(cfg, fac);
    REQUIRE(w.start(0).ok());
    std::vector<float> s(48000 * 2, 0.25f);   // exactly 2 segments of 24000 frames
    REQUIRE(w.write(s.data(), 48000, 0).ok());
    REQUIRE(w.stop().ok());
    CHECK(fac.segments.size() == 2);
    CHECK(w.journal().back().endSample == 48000);
}

TEST_CASE("REQ-R05: default 15 minute segments and size budget segmentation; classic RIFF limit respected") {
    MemorySinkFactory fac;
    {
        WavSegmentWriter w(WavWriterConfig{}, fac);
        REQUIRE(w.start(0).ok());
        CHECK(w.segmentFrames() == 900ull * 48000);
    }
    {
        WavWriterConfig cfg; cfg.maxSegmentBytes = kWavHeaderBytes + 6 * 1000;   // byte budget wins over time
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        CHECK(w.segmentFrames() == 1000);
        std::vector<float> s(2500 * 2, 0.5f);
        REQUIRE(w.write(s.data(), 2500, 0).ok());
        REQUIRE(w.stop().ok());
        CHECK(w.journal().size() == 3);
        CHECK(w.journal()[0].bytes == 6000);
    }
    {
        WavWriterConfig cfg; cfg.maxSegmentBytes = 1ull << 40; cfg.segmentSeconds = 1e9;   // would exceed RIFF
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        CHECK(kWavHeaderBytes + w.segmentFrames() * 6 <= kRiffMaxFileBytes);
        CHECK(w.segmentFrames() * 6 > kRiffMaxFileBytes - (2u << 20));
    }
    {
        WavWriterConfig cfg; cfg.channels = 3;
        WavSegmentWriter w(cfg, fac);
        CHECK(w.start(0).error().category == ErrorCategory::UnsupportedCapability);
        WavWriterConfig bad; bad.maxSegmentBytes = 10;
        WavSegmentWriter w2(bad, fac);
        CHECK_FALSE(w2.start(0).ok());
    }
}

TEST_CASE("REQ-R05: header is patched on flush and finalised on close; a crash copy after flush is a valid file") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.channels = 2; cfg.bufferFrames = 64;
    WavSegmentWriter w(cfg, fac);
    REQUIRE(w.start(0).ok());
    const auto ints = makeInts(1000, 2, 5);
    const auto fl = toFloats(ints);
    REQUIRE(w.write(fl.data(), 1000, 0).ok());
    REQUIRE(w.flush().ok());
    auto crashCopy = *fac.segments[0];            // what is on disk if power fails right now
    const auto more = makeInts(500, 2, 6);
    const auto mfl = toFloats(more);
    REQUIRE(w.write(mfl.data(), 500, 1000).ok());   // not flushed
    CHECK(w.journal()[0].flushedFrames == 1000);
    CHECK(w.journal()[0].frames == 1000);        // journal entries are refreshed at open/flush/close
    CHECK(w.journal()[0].state == SegmentState::Active);
    auto r = readWav(crashCopy);
    REQUIRE(r.ok());
    CHECK(r.value().samples == ints);
    REQUIRE(w.stop().ok());
    auto r2 = readWav(*fac.segments[0]);
    REQUIRE(r2.ok());
    CHECK(r2.value().frames() == 1500);
    CHECK(w.stop().ok());                          // idempotent
}

TEST_CASE("REQ-R05: recoverWav repairs unfinalised and truncated files and discloses the lost tail") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.bufferFrames = 64; cfg.headerPatchIntervalFrames = 1u << 30;   // header never patched before close
    WavSegmentWriter w(cfg, fac);
    REQUIRE(w.start(0).ok());
    const auto ints = makeInts(5000, 2, 21);
    const auto fl = toFloats(ints);
    REQUIRE(w.write(fl.data(), 5000, 0).ok());
    // crash: bytes on disk are header-with-placeholder + whatever the staging buffer already drained
    auto crash = *fac.segments[0];
    CHECK_FALSE(readWav(crash).ok());              // sizes are placeholders: not a valid file as-is
    const std::uint64_t onDiskFrames = (crash.size() - kWavHeaderBytes) / 6;
    CHECK(onDiskFrames < 5000);
    crash.resize(crash.size() - 4);                // power loss mid-frame: 4 stray bytes of a partial frame...
    crash.push_back(0x7F);                         // ...plus a torn byte
    RecoveryHints hints; hints.hasExpectedFrames = true; hints.expectedFrames = 5000;
    auto rep = recoverWav(crash, hints);
    REQUIRE(rep.ok());
    CHECK_FALSE(rep.value().headerWasConsistent);
    CHECK(rep.value().framesRecovered == onDiskFrames - 1);   // 6-byte frame cut by 4, +1 stray byte => lost one frame
    CHECK(rep.value().partialFrameBytesDropped == 3);
    CHECK(rep.value().lostTailKnown);
    CHECK(rep.value().framesLostAtLeast == 5000 - rep.value().framesRecovered);
    CHECK(rep.value().disclosure.find("missing") != std::string::npos);
    auto r = readWav(crash);
    REQUIRE(r.ok());
    REQUIRE(r.value().frames() == rep.value().framesRecovered);
    for (std::size_t i = 0; i < r.value().samples.size(); ++i) CHECK(r.value().samples[i] == ints[i]);   // never invents audio

    // without a journal figure the disclosure says the amount is unknown
    auto again = recoverWav(crash);
    REQUIRE(again.ok());
    CHECK(again.value().headerWasConsistent);
    CHECK_FALSE(again.value().lostTailKnown);
    CHECK(again.value().disclosure.find("unknown") != std::string::npos);

    // a finalised file chopped in the middle of the audio
    REQUIRE(w.stop().ok());
    auto whole = *fac.segments[0];
    whole.resize(kWavHeaderBytes + 6 * 2345 + 2);
    auto rep2 = recoverWav(whole);
    REQUIRE(rep2.ok());
    CHECK(rep2.value().framesRecovered == 2345);
    CHECK(rep2.value().claimedFrames == 5000);
    CHECK(readWav(whole).value().samples == std::vector<std::int32_t>(ints.begin(), ints.begin() + 2345 * 2));

    // garbage and header-less files fail cleanly
    std::vector<std::uint8_t> junk(100, 0xAB);
    CHECK(recoverWav(junk).error().category == ErrorCategory::DecodeFailure);
    std::vector<std::uint8_t> shortHdr(whole.begin(), whole.begin() + 30);
    CHECK_FALSE(recoverWav(shortHdr).ok());
}

TEST_CASE("REQ-R05: file sink round trip and on-disk recovery") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("ss_dyn_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(&dir)));
    fs::create_directories(dir);
    const std::string base = makeRecordingBaseName(1'700'000'000LL * kNsPerSec, "S01", "Friday Night: Main/Stage!");
    CHECK(base == "20231114T221320Z_S01_Friday-Night-MainStage");
    {
        FileSinkFactory fac(dir.u8string(), base);
        WavWriterConfig cfg; cfg.segmentSeconds = 0.25;
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        const auto ints = makeInts(30000, 2, 3);
        const auto fl = toFloats(ints);
        REQUIRE(w.write(fl.data(), 30000, 0).ok());
        REQUIRE(w.stop().ok());
        REQUIRE(w.journal().size() == 3);
        CHECK(w.journal()[1].name == base + "_0001.wav");
        for (int i = 0; i < 3; ++i) {
            const fs::path p = dir / (base + "_000" + std::to_string(i) + ".wav");
            REQUIRE(fs::exists(p));
            std::ifstream f(p, std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto r = readWav(bytes);
            REQUIRE(r.ok());
            CHECK(r.value().frames() == w.journal()[static_cast<std::size_t>(i)].frames);
        }
        // truncate the first file mid-frame and repair it on disk
        const fs::path p0 = dir / (base + "_0000.wav");
        fs::resize_file(p0, kWavHeaderBytes + 6 * 7000 + 5);
        auto rep = recoverWavFile(p0.u8string());
        REQUIRE(rep.ok());
        CHECK(rep.value().framesRecovered == 7000);
        CHECK(rep.value().partialFrameBytesDropped == 5);
        CHECK(fs::file_size(p0) == kWavHeaderBytes + 6 * 7000);
        std::ifstream f(p0, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto r = readWav(bytes);
        REQUIRE(r.ok());
        CHECK(r.value().samples == std::vector<std::int32_t>(ints.begin(), ints.begin() + 7000 * 2));
        CHECK_FALSE(recoverWavFile((dir / "missing.wav").u8string()).ok());
    }
    {
        FileSinkFactory bad((dir / "no_such_dir").u8string(), "x");
        WavSegmentWriter w(WavWriterConfig{}, bad);
        REQUIRE(w.start(0).ok());
        std::vector<float> s(200, 0.1f);
        auto st = w.write(s.data(), 100, 0);
        CHECK_FALSE(st.ok());
        CHECK(st.error().category == ErrorCategory::DiskFull);
        CHECK(st.error().detail.find("no_such_dir") == std::string::npos);   // no paths in error text
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("REQ-R05: naming helpers sanitise show names and format UTC") {
    CHECK(sanitizeShowName("") == "show");
    CHECK(sanitizeShowName("../../etc/passwd") == "etcpasswd");
    CHECK(sanitizeShowName("A B.C") == "A-B-C");
    CHECK(sanitizeShowName(std::string(200, 'x')).size() == 48);
    CHECK(formatUtcCompact(0) == "19700101T000000Z");
    CHECK(formatUtcCompact(1'700'000'000LL * kNsPerSec) == "20231114T221320Z");
    CHECK(formatUtcCompact(951'782'400LL * kNsPerSec) == "20000229T000000Z");   // leap day
}

TEST_CASE("REQ-R05: gap policy - <=250 ms becomes labelled timestamped silence, longer or repeated gaps stop") {
    const auto blockOf = [](std::size_t frames, float v) { return std::vector<float>(frames * 2, v); };
    SUBCASE("short gap is filled with silence and labelled") {
        MemorySinkFactory fac;
        WavSegmentWriter w(WavWriterConfig{}, fac);
        REQUIRE(w.start(0).ok());
        std::vector<GapEvent> seen;
        w.onGap = [&](const GapEvent& g) { seen.push_back(g); };
        auto a = blockOf(1000, 0.5f), b = blockOf(1000, 0.25f);
        REQUIRE(w.write(a.data(), 1000, 0).ok());
        REQUIRE(w.write(b.data(), 1000, 1000 + 4800, kAudioDiscontinuity).ok());   // 100 ms hole
        REQUIRE(w.stop().ok());
        REQUIRE(w.gaps().size() == 1);
        const GapEvent& g = w.gaps()[0];
        CHECK(g.kind == GapKind::SilenceInserted);
        CHECK(g.startSample == 1000);
        CHECK(g.frames == 4800);
        CHECK(g.startNs == samplesToNs(1000, kRate));
        CHECK(g.durationNs == 100 * kNsPerMs);
        CHECK(g.label.find("silence inserted") != std::string::npos);
        CHECK(seen.size() == 1);
        CHECK(w.discontinuityFlags() == 1);
        CHECK(w.journal()[0].gapCount == 1);
        auto r = readWav(*fac.segments[0]);
        REQUIRE(r.ok());
        CHECK(r.value().frames() == 1000 + 4800 + 1000);                // sample positions preserved
        CHECK(r.value().samples[2 * 999] == 4194304);                    // 0.5
        for (std::size_t i = 1000; i < 5800; ++i) { CHECK(r.value().samples[2 * i] == 0); CHECK(r.value().samples[2 * i + 1] == 0); }
        CHECK(r.value().samples[2 * 5800] == 2097152);                   // 0.25 resumes at the right place
        CHECK(w.nextSampleIndex() == 6800);
    }
    SUBCASE("exactly 250 ms is allowed, one frame more stops the recording") {
        MemorySinkFactory fac;
        WavSegmentWriter w(WavWriterConfig{}, fac);
        REQUIRE(w.start(0).ok());
        auto a = blockOf(100, 0.5f);
        REQUIRE(w.write(a.data(), 100, 0).ok());
        REQUIRE(w.write(a.data(), 100, 100 + 12000).ok());                // exactly 250 ms
        CHECK_FALSE(w.failed());
        auto st = w.write(a.data(), 100, 12200 + 12001);                  // 250 ms + 1 frame
        CHECK_FALSE(st.ok());
        CHECK(st.error().category == ErrorCategory::TimestampInvalid);
        CHECK(w.failed());
        CHECK(w.gaps().back().label.find("exceeds allowance") != std::string::npos);
        CHECK_FALSE(w.write(a.data(), 100, 40000).ok());                  // stays failed
        // the data accepted before the stop is a complete, readable segment
        CHECK(w.journal()[0].state == SegmentState::Completed);
        auto r = readWav(*fac.segments[0]);
        REQUIRE(r.ok());
        CHECK(r.value().frames() == 100 + 12000 + 100);
        CHECK_FALSE(w.stop().ok());                                       // failure remains visible
    }
    SUBCASE("repeated short gaps stop the recording; spaced-out gaps do not") {
        MemorySinkFactory fac;
        WavSegmentWriter w(WavWriterConfig{}, fac);   // 3 gaps per 60 s
        REQUIRE(w.start(0).ok());
        auto a = blockOf(480, 0.1f);
        std::uint64_t idx = 0;
        auto put = [&](std::uint64_t gapFrames) { idx += gapFrames; auto st = w.write(a.data(), 480, idx); idx += 480; return st; };
        REQUIRE(put(0).ok());
        CHECK(put(480).ok());   // gap 1
        CHECK(put(480).ok());   // gap 2
        CHECK(put(480).ok());   // gap 3
        auto st = put(480);     // gap 4 within 60 s
        CHECK_FALSE(st.ok());
        CHECK(st.error().detail.find("repeated") != std::string::npos);
        CHECK(w.gaps().size() == 4);

        MemorySinkFactory fac2;
        WavWriterConfig cfg2; cfg2.sampleRateHz = 8000; cfg2.gapWindowSeconds = 1.0;
        WavSegmentWriter w2(cfg2, fac2);
        REQUIRE(w2.start(0).ok());
        std::vector<float> sec2(16000 * 2, 0.1f);
        std::uint64_t i2 = 0;
        for (int k = 0; k < 8; ++k) {          // a 60 ms gap every 2 s with a 1 s window: never more than one in the window
            i2 += 480;
            REQUIRE(w2.write(sec2.data(), 16000, i2).ok());
            i2 += 16000;
        }
        CHECK(w2.gaps().size() == 8);
        CHECK_FALSE(w2.failed());
    }
    SUBCASE("a gap that spans a segment boundary keeps exact boundaries") {
        MemorySinkFactory fac;
        WavWriterConfig cfg; cfg.segmentSeconds = 1.0;
        WavSegmentWriter w(cfg, fac);
        REQUIRE(w.start(0).ok());
        auto a = blockOf(47000, 0.5f), b = blockOf(2000, 0.5f);
        REQUIRE(w.write(a.data(), 47000, 0).ok());
        REQUIRE(w.write(b.data(), 2000, 47000 + 6000).ok());               // 125 ms gap crossing 48000
        REQUIRE(w.stop().ok());
        REQUIRE(w.journal().size() == 2);
        CHECK(w.journal()[0].endSample == 48000);
        CHECK(w.journal()[1].firstSample == 48000);
        CHECK(w.journal()[1].endSample == 55000);
        CHECK(w.gaps()[0].segmentIndex == 0);
        CHECK(w.journal()[0].gapCount == 1);
    }
    SUBCASE("duplicate or overlapping delivery is trimmed, never written twice") {
        MemorySinkFactory fac;
        WavSegmentWriter w(WavWriterConfig{}, fac);
        REQUIRE(w.start(1000).ok());
        const auto ints = makeInts(400, 2, 9);
        const auto fl = toFloats(ints);
        REQUIRE(w.write(fl.data(), 300, 1000).ok());
        REQUIRE(w.write(fl.data() + 200 * 2, 200, 1200).ok());   // frames 1200..1299 duplicate, 1300..1399 new
        REQUIRE(w.write(fl.data(), 50, 1000).ok());              // entirely stale
        REQUIRE(w.stop().ok());
        auto r = readWav(*fac.segments[0]);
        REQUIRE(r.ok());
        CHECK(r.value().frames() == 400);
        CHECK(r.value().samples == ints);
        REQUIRE(w.gaps().size() == 2);
        CHECK(w.gaps()[0].kind == GapKind::OverlapTrimmed);
        CHECK(w.gaps()[0].frames == 100);
    }
}

TEST_CASE("REQ-R04: sink failure surfaces DiskFull and leaves earlier completed segments recoverable") {
    // budget fits exactly one full 1000-frame segment plus a bit of the next
    LimitedFactory fac((kWavHeaderBytes + 6 * 1000) + kWavHeaderBytes + 6 * 300);   // second segment dies after ~256 frames
    WavWriterConfig cfg; cfg.maxSegmentBytes = kWavHeaderBytes + 6 * 1000; cfg.bufferFrames = 64;
    WavSegmentWriter w(cfg, fac);
    REQUIRE(w.start(0).ok());
    const auto ints = makeInts(3000, 2, 31);
    const auto fl = toFloats(ints);
    auto st = w.write(fl.data(), 3000, 0);
    CHECK_FALSE(st.ok());
    CHECK(st.error().category == ErrorCategory::DiskFull);
    CHECK(w.failed());
    REQUIRE(w.journal().size() == 2);
    CHECK(w.journal()[0].state == SegmentState::Completed);
    CHECK(w.journal()[1].state == SegmentState::Failed);
    auto r = readWav(*fac.segments[0]);
    REQUIRE(r.ok());
    CHECK(r.value().frames() == 1000);
    CHECK(r.value().samples == std::vector<std::int32_t>(ints.begin(), ints.begin() + 2000));
    CHECK_FALSE(w.stop().ok());
    // the failed segment is repairable and its disclosure reports what the journal had accepted
    auto torn = *fac.segments[1];
    RecoveryHints h; h.hasExpectedFrames = true; h.expectedFrames = w.journal()[1].frames;
    auto rep = recoverWav(torn, h);
    REQUIRE(rep.ok());
    CHECK(rep.value().framesRecovered == 256);
    CHECK(rep.value().lostTailKnown);
    CHECK(readWav(torn).ok());
    // opening the next segment can fail too
    MemorySinkFactory f2; f2.failOpenAtIndex = 1;
    WavSegmentWriter w2(cfg, f2);
    REQUIRE(w2.start(0).ok());
    auto st2 = w2.write(fl.data(), 3000, 0);
    CHECK(st2.error().category == ErrorCategory::DiskFull);
    CHECK(readWav(*f2.segments[0]).ok());
}

TEST_CASE("REQ-R05: journal callback reports open, flush and close of each segment") {
    MemorySinkFactory fac;
    WavWriterConfig cfg; cfg.segmentSeconds = 0.5;
    WavSegmentWriter w(cfg, fac);
    std::vector<SegmentState> states;
    std::vector<bool> finalised;
    w.onJournal = [&](const SegmentJournalEntry& e) { states.push_back(e.state); finalised.push_back(e.headerFinalised); };
    REQUIRE(w.start(0).ok());
    std::vector<float> s(30000 * 2, 0.1f);
    REQUIRE(w.write(s.data(), 30000, 0).ok());    // 24000 fills segment 0, 6000 go to segment 1
    REQUIRE(w.flush().ok());
    REQUIRE(w.stop().ok());
    const std::vector<SegmentState> want = {SegmentState::Active, SegmentState::Completed, SegmentState::Active, SegmentState::Active, SegmentState::Completed};
    CHECK(states == want);
    CHECK(finalised == std::vector<bool>{false, true, false, false, true});
}
