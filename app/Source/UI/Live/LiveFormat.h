// Operator text for the Live tab and status chips. Pure std logic so the self-tests can check labels.
// Owner: ui-live. Copy follows app-ui 3.5: plain words, no em dashes, no banned phrases.
#pragma once
#include <cstdio>
#include <string>
#include "ss/base/types.hpp"
#include "ss/show/types.hpp"

namespace ss::app::live {

inline std::string tempoText(const ss::TempoState& t) {
    if (!t.valid()) return "BPM unknown";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f BPM", t.bpm);
    return buf;
}

inline std::string tempoSourceWord(ss::ValueSource s) {
    switch (s) {
        case ss::ValueSource::Estimated: return "Est";
        case ss::ValueSource::Tap: return "Tap";
        case ss::ValueSource::Manual: return "Manual";
        case ss::ValueSource::Unknown: return "No estimate";
    }
    return {};
}

inline std::string keyText(const ss::KeyState& k) {
    if (!k.valid()) return "Key unknown";
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(kNames[k.tonic % 12]) + (k.minor ? " minor" : " major");
}

inline int confidencePercent(float c) {
    if (!(c > 0.f)) return 0;
    if (c >= 1.f) return 100;
    return static_cast<int>(c * 100.f + 0.5f);
}

inline const char* arcPhaseWord(std::uint8_t phase) {
    switch (phase) {
        case 1: return "Build";
        case 2: return "Drop";
        default: return "Steady";
    }
    return {};
}

inline const char* modeWord(ss::ShowMode m) {
    switch (m) {
        case ss::ShowMode::Programmed: return "Programmed";
        case ss::ShowMode::Hybrid: return "Hybrid";
        case ss::ShowMode::Auto: return "Auto";
    }
    return {};
}

// The snapshot carries cue indexes but not cue names, so the label stops at the number.
inline std::string cueText(std::int32_t cueIndex, std::int32_t cueCount) {
    if (cueCount <= 0) return "No cues";
    if (cueIndex < 0) return "Cue not started, " + std::to_string(cueCount) + " in show";
    return "Cue " + std::to_string(cueIndex + 1) + " of " + std::to_string(cueCount);
}

inline std::string bankText(int bank, int bankCount) {
    return "Bank " + std::to_string(bank + 1) + " of " + std::to_string(bankCount < 1 ? 1 : bankCount);
}

inline std::string percentText(double unit) {
    int p = static_cast<int>(unit * 100.0 + 0.5);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return std::to_string(p) + "%";
}

}  // namespace ss::app::live
