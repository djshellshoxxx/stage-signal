// Shared synthetic-audio helpers for the audio tests (everything is generated on the fly, deterministic).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "ss/audio/analyzer.hpp"
#include "ss/base/rng.hpp"

namespace sst {

constexpr double kPi = 3.14159265358979323846;

inline std::vector<float> sine(double f, double amp, double sec, std::uint32_t rate, double phase = 0.0) {
    std::vector<float> v(static_cast<std::size_t>(sec * rate));
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(amp * std::sin(2 * kPi * f * double(i) / rate + phase));
    return v;
}

// Interleave a mono signal into `ch` channels (optionally inverting the second one).
inline std::vector<float> toChannels(const std::vector<float>& mono, int ch, bool invertSecond = false) {
    if (ch == 1) return mono;
    std::vector<float> o(mono.size() * ch);
    for (std::size_t i = 0; i < mono.size(); ++i) { o[i * ch] = mono[i]; o[i * ch + 1] = invertSecond ? -mono[i] : mono[i]; }
    return o;
}

// Feeds interleaved audio into an Analyzer in blocks of `block` frames. Tracks the sample counter.
struct Feeder {
    ss::audio::Analyzer& a;
    std::uint32_t ch, rate;
    std::uint64_t idx = 0;
    Feeder(ss::audio::Analyzer& an, std::uint32_t c, std::uint32_t r) : a(an), ch(c), rate(r) {}
    void feed(const std::vector<float>& data, std::size_t block = 480, std::uint32_t flags = 0) {
        const std::size_t frames = data.size() / ch;
        for (std::size_t pos = 0; pos < frames; pos += block) {
            const std::size_t n = std::min(block, frames - pos);
            ss::AudioBlockView v;
            v.interleaved = data.data() + pos * ch; v.frames = static_cast<std::uint32_t>(n);
            v.channels = ch; v.rateHz = rate; v.firstSampleIndex = idx; v.flags = pos == 0 ? flags : 0;
            a.process(v);
            idx += n;
        }
    }
};

inline void addInPlace(std::vector<float>& dst, const std::vector<float>& src, std::size_t at) {
    for (std::size_t i = 0; i < src.size() && at + i < dst.size(); ++i) dst[at + i] += src[i];
}

inline float dbToLin(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// A percussive hit: exponentially decaying noise + sine body. kind selects timbre.
inline std::vector<float> hit(int kind, double amp, std::uint32_t rate, ss::Rng& rng) {
    const std::size_t n = static_cast<std::size_t>(0.25 * rate);
    std::vector<float> h(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / rate;
        double s = 0.0;
        switch (kind % 4) {
            case 0: s = (i < 4) ? 1.0 : 0.0; break;                                                   // click
            case 1: s = (rng.unit() * 2 - 1) * std::exp(-t / 0.025); break;                           // snare-ish noise burst
            case 2: s = std::sin(2 * kPi * (60 + 90 * std::exp(-t / 0.03)) * t) * std::exp(-t / 0.06); break;   // kick
            default: s = std::sin(2 * kPi * 1500 * t) * std::exp(-t / 0.03); break;                   // tonal blip
        }
        const double taper = std::min(1.0, double(n - i) / (0.02 * rate));        // avoid a click at the truncation point
        h[i] = static_cast<float>(amp * s * taper);
    }
    return h;
}

// Click train at `bpm` for `sec` seconds (single-sample-ish clicks, 4 samples wide), mono.
inline std::vector<float> clickTrain(double bpm, double sec, std::uint32_t rate, double amp = 0.8) {
    std::vector<float> v(static_cast<std::size_t>(sec * rate), 0.f);
    const double period = 60.0 / bpm * rate;
    for (double p = 0.1 * rate; p < double(v.size()) - 8; p += period) {
        const std::size_t i = static_cast<std::size_t>(std::llround(p));
        for (int k = 0; k < 4; ++k) v[i + k] = static_cast<float>(amp * (k % 2 ? -1.0 : 1.0));
    }
    return v;
}

// ---- musical synthesis (additive) for key tests -------------------------------------------------------
inline double midiToHz(double m, double cents = 0.0) { return 440.0 * std::pow(2.0, (m - 69.0 + cents / 100.0) / 12.0); }

// Adds a note with 6 decaying harmonics and a short attack/release envelope.
inline void addNote(std::vector<float>& out, double midi, double startS, double durS, double amp, std::uint32_t rate,
                    double cents = 0.0) {
    const double f0 = midiToHz(midi, cents);
    const std::size_t s0 = static_cast<std::size_t>(startS * rate), n = static_cast<std::size_t>(durS * rate);
    for (int h = 1; h <= 6; ++h) {
        const double f = f0 * h;
        if (f > 0.45 * rate) break;
        const double a = amp / (h * 1.0);
        const double w = 2 * kPi * f / rate;
        double c = 1.0, sn = 0.0;                                   // phasor recurrence: no per-sample sin()
        const double cw = std::cos(w), sw = std::sin(w);
        for (std::size_t i = 0; i < n && s0 + i < out.size(); ++i) {
            const double t = double(i) / rate;
            const double env = std::min(1.0, t / 0.02) * std::min(1.0, (durS - t) / 0.03);
            out[s0 + i] += static_cast<float>(a * env * sn);
            const double nc = c * cw - sn * sw;
            sn = sn * cw + c * sw; c = nc;
        }
    }
}

// Triad built on root midi note: major or minor.
inline void addTriad(std::vector<float>& out, int rootMidi, bool minor, double startS, double durS, double amp,
                     std::uint32_t rate, double cents = 0.0) {
    addNote(out, rootMidi, startS, durS, amp, rate, cents);
    addNote(out, rootMidi + (minor ? 3 : 4), startS, durS, amp, rate, cents);
    addNote(out, rootMidi + 7, startS, durS, amp, rate, cents);
}

// I-IV-V-I (major) or i-iv-V-i (minor, harmonic-minor V) progression repeated to fill `sec` seconds. tonic 0..11.
inline std::vector<float> progression(int tonic, bool minor, double sec, std::uint32_t rate, double cents = 0.0,
                                      double amp = 0.1) {
    std::vector<float> out(static_cast<std::size_t>(sec * rate), 0.f);
    const int root = 48 + tonic;                         // C3-based
    const double chordS = 1.0;
    int i = 0;
    for (double t = 0; t + chordS <= sec; t += chordS, ++i) {
        switch (i % 4) {
            case 0: addTriad(out, root, minor, t, chordS, amp, rate, cents); break;
            case 1: addTriad(out, root + 5, minor, t, chordS, amp, rate, cents); break;
            case 2: addTriad(out, root + 7, false, t, chordS, amp, rate, cents); break;
            default: addTriad(out, root, minor, t, chordS, amp, rate, cents); break;
        }
    }
    return out;
}

}  // namespace sst
