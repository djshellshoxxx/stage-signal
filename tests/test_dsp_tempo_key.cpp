// MOD-03 tempo and key tests (REQ-A05, REQ-A07): click corpus, silence, drums/noise, tonal vectors.
#include "test.h"
#include "test_dsp_util.h"
#include <cstdlib>
using namespace ss;
using namespace dsptest;

namespace {

std::vector<float> clickTrain(double bpm, double sec, uint32_t rate, int timbre, double offsetSec = 0.3) {
  std::vector<float> v = noiseBuf(0.001, sec, rate, 5);
  Lcg r(3);
  const double period = 60.0 / bpm;
  for (double t = offsetSec; t < sec - 0.1; t += period) {
    const size_t at = (size_t)(t * rate + 0.5);
    switch (timbre) {
      case 0: addBurst(v, at, 1000, 0.6, 0.004, rate, 0.0, r); break;     // short tick
      case 1: addBurst(v, at, 90, 0.8, 0.03, rate, 0.0, r); break;        // kick-like
      default: addBurst(v, at, 0, 0.5, 0.02, rate, 1.0, r); break;        // noise burst
    }
  }
  return v;
}

// A pitched note with harmonic series and a soft attack/decay envelope.
void addNote(std::vector<float>& v, double f, double startSec, double durSec, double amp, uint32_t rate) {
  const size_t s0 = (size_t)(startSec * rate), n = (size_t)(durSec * rate);
  for (size_t i = 0; i < n && s0 + i < v.size(); ++i) {
    const double t = (double)i / rate;
    const double env = std::min(1.0, t / 0.02) * std::exp(-t * 0.8) * std::min(1.0, (durSec - t) / 0.03);
    double x = 0;
    for (int h = 1; h <= 5; ++h) if (f * h < 1500) x += std::sin(2 * kPi * f * h * t) / h;
    v[s0 + i] += (float)(amp * env * x);
  }
}
double midiHz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

// Chord progression in a key (tonic pc, minor?) for `sec` seconds: i/I - iv/IV - V - i/I, repeated.
std::vector<float> progression(int tonic, bool minor, double sec, uint32_t rate, double detuneCents = 0) {
  std::vector<float> v((size_t)(sec * rate), 0.f);
  const int third = minor ? 3 : 4;
  const int roots[4] = {0, 5, 7, 0};
  const bool isMinorChord[4] = {minor, minor, false, minor};
  const double det = std::pow(2.0, detuneCents / 1200.0);
  double t = 0.1; int ci = 0;
  while (t < sec - 0.5) {
    const int r = 48 + ((tonic + roots[ci % 4]) % 12);
    const int th = isMinorChord[ci % 4] ? 3 : 4; (void)third;
    const int notes[3] = {r, r + th, r + 7};
    for (int n : notes) addNote(v, midiHz(n) * det, t, 1.4, 0.12, rate);
    addNote(v, midiHz(r - 12) * det, t, 1.4, 0.12, rate);
    t += 1.5; ++ci;
  }
  return v;
}

struct KeyRes { bool valid; int pc; bool minor; float conf; };
KeyRes runKey(const std::vector<float>& mono, uint32_t rate, uint32_t ch = 1) {
  Analyzer a(rate, ch);
  feed(a, ch == 2 ? dup(mono) : mono, rate, ch, 480);
  FeatureSnapshot s = a.snapshot(ns(mono.size(), rate));
  return {s.key.valid, (int)s.key.value, s.key.minor, s.key.confidence};
}

} // namespace

TEST(dsp_tempo_click_corpus_within_2pct_after_10s) {
  const double bpms[] = {60, 66, 72, 84, 90, 97, 100, 110, 120, 128, 133, 140, 150, 160, 170, 180};
  const uint32_t rates[] = {48000, 44100};
  int bad = 0, total = 0; double worst = 0;
  for (uint32_t rate : rates)
    for (int timbre = 0; timbre < 3; ++timbre)
      for (double bpm : bpms) {
        auto v = clickTrain(bpm, 10.0, rate, timbre);
        Analyzer a(rate, 1);
        feed(a, v, rate, 1, 480);
        FeatureSnapshot s = a.snapshot(ns(v.size(), rate));
        const double err = s.tempo.valid ? std::fabs(s.tempo.value / bpm - 1.0) : 1.0;
        ++total; worst = std::max(worst, err);
        if (!s.tempo.valid || err > 0.02) {
          ++bad;
          std::printf("  tempo miss: rate %u timbre %d true %.0f -> valid %d %.2f conf %.2f\n", rate, timbre, bpm, s.tempo.valid, s.tempo.value, s.tempo.confidence);
        } else {
          CHECK(s.tempo.confidence > 0.4f && s.tempo.confidence <= 1.0f);
          CHECK(s.tempo.ageNs >= 0 && s.tempo.ageNs < 3 * kNsPerSec);
        }
      }
  std::printf("  click corpus: %d/%d within 2%% after 10 s, worst error %.2f%%\n", total - bad, total, 100 * worst);
  CHECK(bad == 0);
}

TEST(dsp_tempo_stereo_and_age_grows_without_update) {
  auto v = clickTrain(120, 12.0, 48000, 0);
  Analyzer a(48000, 2);
  feed(a, dup(v), 48000, 2, 480);
  FeatureSnapshot s = a.snapshot(ns(v.size(), 48000));
  CHECK(s.tempo.valid); CHECK_NEAR(s.tempo.value, 120.0, 2.4);
  FeatureSnapshot later = a.snapshot(ns(v.size(), 48000) + 5 * kNsPerSec);
  CHECK(later.tempo.ageNs >= s.tempo.ageNs + 4 * kNsPerSec);
  CHECK(s.tempo.source == Estimate::Estimated);
}

TEST(dsp_tempo_silence_is_unknown) {
  Analyzer a(48000, 2);
  feed(a, std::vector<float>(48000 * 2 * 15, 0.f), 48000, 2);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(!s.tempo.valid); CHECK(!s.key.valid); CHECK(s.tempo.value == 0);
  // Tempo that was valid becomes Unknown after sustained silence.
  Analyzer b(48000, 1);
  feed(b, clickTrain(120, 12.0, 48000, 0), 48000, 1);
  CHECK(b.snapshot(0).tempo.valid);
  feed(b, std::vector<float>(48000 * 4, 0.f), 48000, 1);
  FeatureSnapshot t = b.snapshot(0);
  CHECK(!t.tempo.valid); CHECK(t.silent);
}

TEST(dsp_tempo_noise_not_forced) {
  Analyzer a(48000, 1);
  feed(a, noiseBuf(0.3, 20.0, 48000, 17), 48000, 1);
  CHECK(!a.snapshot(0).tempo.valid);
}

TEST(dsp_tempo_follows_change) {
  Analyzer a(48000, 1);
  feed(a, clickTrain(100, 12.0, 48000, 0), 48000, 1);
  FeatureSnapshot s = a.snapshot(0);
  CHECK(s.tempo.valid); CHECK_NEAR(s.tempo.value, 100.0, 2.0);
  auto w = clickTrain(150, 20.0, 48000, 0);
  feed(a, w, 48000, 1, 480, [](size_t) {}, 12 * 48000);
  s = a.snapshot(0);
  CHECK(s.tempo.valid); CHECK_NEAR(s.tempo.value, 150.0, 3.0);
}

// ---- Key (REQ-A07) --------------------------------------------------------------------------------

TEST(dsp_key_major_and_minor_progressions) {
  const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
  int bad = 0, total = 0;
  for (uint32_t rate : {48000u, 44100u})
    for (int tonic = 0; tonic < 12; ++tonic)
      for (int minor = 0; minor < 2; ++minor) {
        if (rate == 44100 && tonic % 3 != 0) continue;
        KeyRes r = runKey(progression(tonic, minor != 0, 22.0, rate), rate, tonic % 2 ? 1 : 2);
        ++total;
        if (!(r.valid && r.pc == tonic && r.minor == (minor != 0))) {
          ++bad;
          std::printf("  key miss: rate %u true %s %s -> valid %d pc %d minor %d conf %.2f\n", rate, names[tonic], minor ? "min" : "maj", r.valid, r.pc, r.minor, r.conf);
        }
      }
  std::printf("  key vectors: %d/%d correct\n", total - bad, total);
  CHECK(bad == 0);
}

TEST(dsp_key_simple_triads_and_scale_with_tonic) {
  const uint32_t rate = 48000;
  // C major triad sustained (repeated) -> C major; A minor triad -> A minor.
  auto triad = [&](int root, int third) {
    std::vector<float> v((size_t)(20 * rate), 0.f);
    for (double t = 0.1; t < 19; t += 1.0) {
      addNote(v, midiHz(root), t, 0.95, 0.15, rate); addNote(v, midiHz(root + third), t, 0.95, 0.15, rate);
      addNote(v, midiHz(root + 7), t, 0.95, 0.15, rate);
    }
    return v;
  };
  KeyRes c = runKey(triad(60, 4), rate);
  CHECK(c.valid && c.pc == 0 && !c.minor);
  KeyRes a = runKey(triad(57, 3), rate);
  CHECK(a.valid && a.pc == 9 && a.minor);
  // Major scale melody that revisits the tonic and dominant.
  const int sc[] = {0, 2, 4, 5, 7, 9, 11, 12, 7, 4, 0, 7, 0, 4, 7, 0};
  std::vector<float> m((size_t)(22 * rate), 0.f);
  double t = 0.1; int i = 0;
  while (t < 21) { const int n = sc[i++ % 16]; addNote(m, midiHz(62 + n), t, 0.5, 0.25, rate); if (i % 2 == 0) addNote(m, midiHz(50), t, 0.5, 0.2, rate); t += 0.5; }
  KeyRes d = runKey(m, rate);
  CHECK(d.valid && d.pc == 2 && !d.minor);
}

TEST(dsp_key_unknown_for_silence_noise_drums) {
  const uint32_t rate = 48000;
  KeyRes s = runKey(std::vector<float>(rate * 30, 0.f), rate);
  CHECK(!s.valid);
  KeyRes w = runKey(noiseBuf(0.3, 30.0, rate, 21), rate);
  CHECK(!w.valid);
  // Pink-ish low-passed noise.
  auto p = noiseBuf(0.3, 30.0, rate, 22); float st = 0; for (auto& x : p) { st = 0.97f * st + 0.03f * x; x = st * 6; }
  CHECK(!runKey(p, rate).valid);
  // Chromatic scale loop (all pitch classes equal) and a sustained open fifth are ambiguous: no key.
  std::vector<float> ch((size_t)(30 * rate), 0.f);
  for (int i = 0; i * 0.25 < 29; ++i) addNote(ch, midiHz(48 + (i % 24)), i * 0.25, 0.24, 0.2, rate);
  CHECK(!runKey(ch, rate).valid);
  auto fifth = sine(220, 0.2, 25, rate), up = sine(329.63, 0.2, 25, rate);
  for (size_t i = 0; i < fifth.size(); ++i) fifth[i] += up[i];
  CHECK(!runKey(fifth, rate).valid);
  CHECK(!runKey(sine(261.63, 0.3, 25, rate), rate).valid);          // single sustained note
  // Drum pattern at 120 BPM: kick sweep + snare noise + hats; no tonal key may be forced.
  std::vector<float> d((size_t)(40 * rate), 0.f);
  Lcg r(8);
  for (double t = 0.2; t < 39; t += 0.5) {
    const int beat = (int)std::lround((t - 0.2) / 0.5);
    const size_t at = (size_t)(t * rate);
    if (beat % 4 == 0 || beat % 4 == 2) {                 // kick: sweeping sine 130 -> 45 Hz
      for (size_t i = 0; i < (size_t)(0.25 * rate) && at + i < d.size(); ++i) {
        const double tt = (double)i / rate, f = 45 + 85 * std::exp(-tt / 0.03);
        d[at + i] += (float)(0.8 * std::exp(-tt / 0.08) * std::sin(2 * kPi * (45 * tt + 85 * 0.03 * (1 - std::exp(-tt / 0.03)))));
        (void)f;
      }
    }
    if (beat % 4 == 1 || beat % 4 == 3) addBurst(d, at, 0, 0.5, 0.05, rate, 1.0, r);   // snare
    addBurst(d, at, 7000, 0.2, 0.01, rate, 0.9, r);                                       // hat
  }
  KeyRes k = runKey(d, rate);
  CHECK(!k.valid);
  if (k.valid) std::printf("  drums forced key %d minor %d conf %.2f\n", k.pc, k.minor, k.conf);
  // And drums are still tracked as a tempo (~120 BPM or an octave relative).
  Analyzer a(rate, 1);
  feed(a, d, rate, 1);
  FeatureSnapshot f = a.snapshot(0);
  CHECK(f.tempo.valid);
  CHECK(std::fabs(f.tempo.value / 120.0 - 1.0) < 0.03 || std::fabs(f.tempo.value / 60.0 - 1.0) < 0.03 || std::fabs(f.tempo.value / 240.0 - 1.0) < 0.03);
}

TEST(dsp_key_not_before_context_and_clears_after_silence) {
  const uint32_t rate = 48000;
  Analyzer a(rate, 1);
  auto v = progression(7, false, 30.0, rate);
  bool early = false;
  feed(a, v, rate, 1, 480, [&](size_t end) {
    if (end < 6 * (size_t)rate && a.snapshot(0).key.valid) early = true;
  });
  CHECK(!early);                                   // needs >= 8 s of tonal evidence
  FeatureSnapshot s = a.snapshot(ns(v.size(), rate));
  CHECK(s.key.valid && s.key.value == 7 && !s.key.minor);
  CHECK(s.key.ageNs >= 0 && s.key.ageNs < 6 * kNsPerSec);
  feed(a, std::vector<float>(rate * 8, 0.f), rate, 1, 480, [](size_t) {}, v.size());
  CHECK(!a.snapshot(0).key.valid);
}

int main() { return ss_test::runAll(); }
