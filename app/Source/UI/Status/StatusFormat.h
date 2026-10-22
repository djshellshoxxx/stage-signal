// Status bar chip text (app-ui 4.1). Pure std logic: label, value, accessible name and level for each chip.
// Level is also drawn as a glyph, so state is never colour only (APP-UI-03). Owner: ui-live.
#pragma once
#include <cstdio>
#include <string>
#include "App/UiModel.h"
#include "UI/Live/LiveFormat.h"

namespace ss::app::status {

enum class Level : std::uint8_t { Off, Ok, Info, Warn, Danger };

struct Chip {
    std::string label;      // full label, kept as tooltip and accessible name in compact layout
    std::string value;      // shown always
    Level level = Level::Off;
    std::string name() const { return label + ": " + value; }
    char glyph() const {
        switch (level) {
            case Level::Ok: return 'o';
            case Level::Info: return 'i';
            case Level::Warn: return '!';
            case Level::Danger: return 'x';
            case Level::Off: return '-';
        }
        return {};
    }
};

inline Chip showChip(const UiSnapshot& s) {
    // The snapshot has no dirty flag; undo depth is the proxy until shell-integration adds one.
    const bool edited = s.show.undoDepth > 0;
    return {"Show", edited ? "Unsaved changes" : "Saved", edited ? Level::Warn : Level::Off};
}

inline Chip audioChip(const UiSnapshot& s) {
    const auto& a = s.audio;
    if (a.deviceState == AdapterState::Unavailable) return {"Audio", "No device", Level::Warn};
    if (a.deviceState == AdapterState::Fault || a.deviceState == AdapterState::Degraded) return {"Audio", "Lost", Level::Danger};
    if (a.deviceState == AdapterState::Stopped || a.deviceState == AdapterState::Starting) return {"Audio", "Starting", Level::Off};
    if (s.features.inputNearFullScale) return {"Audio", "CLIP", Level::Warn};
    if (!a.signalAlive || s.features.silent) return {"Audio", "No signal", Level::Info};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f dB", static_cast<double>(s.features.rmsDb[0]));
    return {"Audio", buf, Level::Ok};
}

inline Chip modeChip(const UiSnapshot& s) {
    std::string v = live::modeWord(s.show.mode);
    if (s.show.paused) v += " paused";
    else if (s.show.manualOverride) v += " override";
    return {"Mode", v, (s.show.paused || s.show.manualOverride) ? Level::Warn : Level::Ok};
}

inline Chip tempoChip(const UiSnapshot& s) {
    const auto& t = s.show.tempo;
    if (!t.valid()) return {"Tempo", "BPM unknown", Level::Off};
    return {"Tempo", live::tempoText(t) + " " + live::tempoSourceWord(t.source), Level::Info};
}

inline Chip keyChip(const UiSnapshot& s) {
    return {"Key", s.show.key.valid() ? live::keyText(s.show.key) : std::string("Key unknown"), s.show.key.valid() ? Level::Info : Level::Off};
}

inline Chip midiChip(const UiSnapshot& s) {
    return s.midi.outputEnabled ? Chip{"MIDI", "Out on", Level::Ok} : Chip{"MIDI", "Out off", Level::Off};
}

inline Chip lightingChip(const UiSnapshot& s) {
    switch (s.lighting.state) {
        case LightingUiState::Armed: return {"Lighting", "Armed", Level::Ok};
        case LightingUiState::DryRun: return {"Lighting", "Dry run", Level::Info};
        case LightingUiState::Fault: return {"Lighting", "Fault", Level::Danger};
        case LightingUiState::Disarmed: return {"Lighting", "Disarmed", Level::Off};
    }
    return {};
}

inline Chip recordChip(const UiSnapshot& s) {
    const auto ms = s.record.elapsedMs;
    char buf[32];
    switch (s.record.state) {
        case RecordUiState::Recording:
            std::snprintf(buf, sizeof buf, "%02llu:%02llu", static_cast<unsigned long long>(ms / 60000),
                          static_cast<unsigned long long>((ms / 1000) % 60));
            return {"Recording", buf, Level::Danger};
        case RecordUiState::Failed: return {"Recording", "Failed", Level::Danger};
        case RecordUiState::Preparing:
        case RecordUiState::Stopping: return {"Recording", "Working", Level::Info};
        case RecordUiState::Recoverable: return {"Recording", "Recoverable", Level::Warn};
        case RecordUiState::Off: return {"Recording", "Idle", Level::Off};
    }
    return {};
}

inline Chip broadcastChip(const UiSnapshot& s) {
    switch (s.broadcast.state) {
        case BroadcastUiState::Live: return {"Broadcast", "Live", Level::Ok};
        case BroadcastUiState::Reconnecting: return {"Broadcast", "Retry", Level::Warn};
        case BroadcastUiState::Failed: return {"Broadcast", "Error", Level::Danger};
        case BroadcastUiState::Preparing:
        case BroadcastUiState::Stopping: return {"Broadcast", "Working", Level::Info};
        case BroadcastUiState::Off: return {"Broadcast", "Off", Level::Off};
    }
    return {};
}

inline Chip perfChip(const UiSnapshot& s) {
    if (!s.perf.valid) return {"Perf", "No data", Level::Off};
    char buf[32];
    std::snprintf(buf, sizeof buf, "CPU %.0f%%", s.perf.cpuPct);
    return {"Perf", buf, s.perf.cpuPct >= 80.0 ? Level::Warn : Level::Ok};
}

}  // namespace ss::app::status
