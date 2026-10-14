// MOD-11 clock model tests (REQ-R03): timeline compensation, cadence accounting, 1 h drift simulation, flash/click helpers.
#include "test.h"
#include "recording/sync.h"
#include <cmath>
using namespace ss;

TEST(sync_rescale_timebases) {
  Rational ms{1, 1000}, ns{1, 1000000000}, ntsc{1001, 30000}, a48{1, 48000};
  CHECK(rescale(1000, ms, ns) == 1000000000LL);
  CHECK(rescale(1500000000LL, ns, ms) == 1500);
  CHECK(rescale(48000, a48, ms) == 1000);
  CHECK(rescale(30, ntsc, ns) == 1001000000LL);           // 30 NTSC frames == 1.001 s
  CHECK(nsToTicks(kNsPerSec, a48) == 48000);
  CHECK(ticksToNs(1, a48) == 20833);                      // rounded to nearest ns
  CHECK(rescale(-1500000000LL, ns, ms) == -1500);
  CHECK(rec::samplesToNs(48000ull * 3600, 48000) == 3600 * kNsPerSec);   // exact over 1 h
  CHECK(rec::samplesToNs(1, 44100) == 22675);
}

TEST(sync_timeline_compensates_delay) {
  SyncTimeline t; DelayCompensation c; c.audioProcessingDelaySamples = 240; /* 5 ms @48k */ c.audioEncoderPrimingNs = 0;
  t.anchor(1000 * kNsPerMs, 48000, c);
  // first captured input sample at host 1000 ms emerges from the dynamics at output sample 240 => timeline 5 ms.
  CHECK(t.audioToTimeline(240) == 5 * kNsPerMs);
  CHECK(t.videoToTimeline(1000 * kNsPerMs) == 5 * kNsPerMs);       // a video frame captured at that same instant lines up
  CHECK(t.videoToTimeline(1100 * kNsPerMs) == 105 * kNsPerMs);
  // camera offset calibration (camera frames stamped 20 ms early by driver)
  SyncTimeline t2; c.cameraOffsetNs = -20 * kNsPerMs; t2.anchor(0, 48000, c);
  CHECK(t2.videoToTimeline(100 * kNsPerMs) == 125 * kNsPerMs);
  c = DelayCompensation(); c.audioEncoderPrimingNs = 21333333; SyncTimeline t3; t3.anchor(0, 48000, c);
  CHECK(t3.totalAudioDelayNs() == 21333333); CHECK(t3.videoToTimeline(0) == 21333333);
}

TEST(sync_cadence_dup_drop_accounting) {
  FrameCadence c; c.configure(30, 1); c.reset(0);
  Ns d = kNsPerSec / 30;
  auto a = c.push(0); CHECK(!a.drop && a.slot == 0 && a.dupBefore == 0);
  auto b = c.push(d + 1000); CHECK(b.slot == 1 && b.dupBefore == 0);
  auto s = c.push(4 * d); CHECK(s.slot == 4 && s.dupBefore == 2);        // source stalled: 2 repeats
  auto dr = c.push(4 * d + d / 4); CHECK(dr.drop);                       // second frame in same slot dropped
  CHECK(c.duplicated() == 2 && c.dropped() == 1);
  // 25 fps source into 30 fps output over 10 s: ~ 5 dups per second
  FrameCadence c2; c2.configure(30, 1); c2.reset(0);
  for (int i = 0; i < 250; ++i) c2.push((Ns)i * 40 * kNsPerMs);
  CHECK(c2.dropped() == 0); CHECK(c2.duplicated() >= 48 && c2.duplicated() <= 52);
  // 60 fps source into 30 fps output: every other frame dropped
  FrameCadence c3; c3.configure(30, 1); c3.reset(0);
  for (int i = 0; i < 600; ++i) c3.push((Ns)((long double)i * 1e9L / 60));
  CHECK(c3.dropped() >= 295 && c3.dropped() <= 305);
  FrameCadence c4; c4.configure(30000, 1001); c4.reset(0); CHECK(c4.slotToNs(30) == 1001000000LL);
}

struct Lcg { uint32_t s = 12345; double next() { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xFFFF) / 65535.0; } };

struct DriftRun { double maxResidualMs = 0, endResidualMs = 0, maxAfterWarmMs = 0; DriftStats st; Status firstError; };
static DriftRun runDrift(double trueEnPpm, double hours, uint32_t rate, double jitterMs, bool correct, bool hiccup = false) {
  DriftCorrector dc; DriftConfig cfg; cfg.enabled = correct; dc.configure(rate, 1, cfg);
  const uint32_t N = rate / 100;                  // 10 ms blocks
  const double devRate = rate * (1.0 + trueEnPpm * 1e-6);
  const uint64_t blocks = (uint64_t)(hours * 3600 * 100);
  std::vector<float> in(N, 0.1f), out; Lcg rnd; DriftRun r;
  const double base = 5000.0;                       // arbitrary host epoch (s)
  for (uint64_t k = 0; k < blocks; ++k) {
    double hostStart = base + (double)(k * N) / devRate;
    double jit = (rnd.next() - 0.5) * 2 * jitterMs * 1e-3;
    if (hiccup && k == 90000) jit += 0.080;
    Ns cap = (Ns)((hostStart + jit) * 1e9);
    Status s = dc.process(in.data(), N, cap, true, out);
    if (!s.ok() && r.firstError.ok()) r.firstError = s;
    double hostEnd = base + (double)((k + 1) * N) / devRate;
    double residual = ((double)dc.stats().outFrames / rate - (hostEnd - base)) * 1000.0;   // ms (output audio ahead of host => +)
    if (k * N / rate > 40) r.maxAfterWarmMs = std::max(r.maxAfterWarmMs, std::fabs(residual));
    r.maxResidualMs = std::max(r.maxResidualMs, std::fabs(residual)); r.endResidualMs = residual;
  }
  r.st = dc.stats(); return r;
}

TEST(sync_drift_one_hour_residual_under_20ms) {
  // Without correction an 80 ppm clock gains 288 ms in an hour (this is what the controller must remove).
  DriftRun raw = runDrift(80, 1.0, 8000, 1.0, false);
  CHECK_NEAR(raw.endResidualMs, 288.0, 3.0);
  DriftRun fix = runDrift(80, 1.0, 8000, 1.0, true);
  std::printf("   +80ppm: max residual after warm-up %.2f ms, end %.2f ms, measured %.2f ppm, applied %.2f\n", fix.maxAfterWarmMs, fix.endResidualMs, fix.st.measuredPpm, fix.st.appliedPpm);
  CHECK(fix.maxAfterWarmMs <= 20.0); CHECK(fix.maxResidualMs <= 20.0); CHECK(std::fabs(fix.endResidualMs) <= 5.0);
  CHECK_NEAR(fix.st.measuredPpm, 80.0, 3.0); CHECK(!fix.st.excessive); CHECK(fix.firstError.ok());
  DriftRun neg = runDrift(-150, 1.0, 8000, 2.0, true);
  std::printf("   -150ppm: max after warm-up %.2f ms, end %.2f ms\n", neg.maxAfterWarmMs, neg.endResidualMs);
  CHECK(neg.maxAfterWarmMs <= 20.0); CHECK(std::fabs(neg.endResidualMs) <= 5.0);
  DriftRun hic = runDrift(60, 1.0, 8000, 1.0, true, true);   // one 80 ms scheduler hiccup in a timestamp is rejected as an outlier
  CHECK(hic.maxAfterWarmMs <= 20.0); CHECK(hic.st.outliersIgnored >= 1);
  DriftRun at48 = runDrift(-40, 0.05, 48000, 0.5, true);
  CHECK(std::fabs(at48.endResidualMs) <= 5.0);
}

TEST(sync_drift_beyond_bound_is_error) {
  DriftRun d = runDrift(300, 0.05, 8000, 0.2, true);   // 3 min of 300 ppm mismatch
  CHECK(!d.firstError.ok()); CHECK(d.firstError.code == Err::TimestampInvalid); CHECK(d.st.excessive); CHECK(d.st.appliedPpm == 0);
  CHECK(d.firstError.detail.find("200") != std::string::npos);
  DriftRun edge = runDrift(190, 0.05, 8000, 0.2, true); CHECK(edge.firstError.ok());
}

TEST(sync_resampler_is_smooth_and_bounded) {
  DriftCorrector dc; DriftConfig cfg; cfg.warmupSec = 2; dc.configure(48000, 2, cfg);
  const double f = 1000.0; uint64_t n = 0; double maxStep = 0, peak = 0; float prev[2] = {0, 0}; bool havePrev = false;
  std::vector<float> in(960 * 2), out;
  for (int k = 0; k < 4000; ++k) {               // 80 s, device clock +150 ppm
    for (int i = 0; i < 960; ++i) { float v = (float)std::sin(2 * M_PI * f * (double)(n + i) / 48000.0); in[i * 2] = v; in[i * 2 + 1] = -v; }
    Ns cap = (Ns)((double)n / (48000.0 * (1 + 150e-6)) * 1e9); n += 960;
    dc.process(in.data(), 960, cap, true, out);
    for (size_t i = 0; i + 1 < out.size(); i += 2) { if (havePrev && k > 200) maxStep = std::max(maxStep, (double)std::fabs(out[i] - prev[0])); prev[0] = out[i]; prev[1] = out[i + 1]; havePrev = true; if (k > 200) peak = std::max(peak, (double)std::fabs(out[i])); CHECK(out[i] == -out[i + 1] || std::fabs(out[i] + out[i + 1]) < 1e-4f); }
  }
  CHECK(peak > 0.995 && peak < 1.005);
  CHECK(maxStep < 2 * M_PI * f / 48000.0 * 1.02);   // no clicks/discontinuities
  CHECK_NEAR(dc.stats().appliedPpm, 150.0, 8.0);
  CHECK(dc.stats().inFrames > dc.stats().outFrames);   // dropped frames to follow a fast clock
}

TEST(sync_flash_click_offset_helpers) {
  // Synthetic recording: flash at video frame 7 (30 fps timeline pts), click at audio sample for t = 0.2 s
  std::vector<double> luma; for (int i = 0; i < 20; ++i) luma.push_back(i == 7 || i == 8 ? 230.0 : 20.0 + (i % 3));
  int fi = detectFlashIndex(luma, 100.0); CHECK(fi == 7);
  Frame dark; dark.resize(16, 16); for (auto& p : dark.px) p = rgb(10, 10, 10);
  Frame white; white.resize(16, 16); for (auto& p : white.px) p = rgb(255, 255, 255);
  CHECK(meanLuma(white) > 250 && meanLuma(dark) < 12);
  std::vector<float> a(48000 * 2, 0.f); size_t clickAt = 9600;
  a[clickAt * 2] = 0.9f; a[clickAt * 2 + 1] = 0.9f;
  int64_t cs = detectClickSample(a.data(), 48000, 2, 0.5f); CHECK(cs == 9600);
  Ns flashNs = (Ns)((long double)fi * 1e9L / 30), clickNs = rec::samplesToNs((uint64_t)cs, 48000);
  Ns off = flashClickOffsetNs(flashNs, clickNs);
  CHECK(std::llabs(off - (233333333 - 200000000)) < 1000);        // video 33.3 ms later than audio
  a[(clickAt + 24000) * 2] = 0.9f; auto cl = detectClicks(a.data(), 48000, 2, 0.5f, 4800); CHECK(cl.size() == 2 && cl[1] - cl[0] == 24000);
  // offset series with +10 ms/h drift
  std::vector<double> t, o; for (int i = 0; i <= 6; ++i) { t.push_back(i * 600.0); o.push_back(5e6 + i * 600.0 / 3600.0 * 10e6); }
  CHECK_NEAR(offsetDriftNsPerHour(t, o), 10e6, 1e4);
}

int main() { return ss_test::runAll(); }
