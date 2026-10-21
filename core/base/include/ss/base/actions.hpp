// StageSignal base: central action registry ids (IF-02). Numeric values are STABLE: they are stored in
// show files (MIDI mappings, key maps) and must never be renumbered. Append only.
#pragma once
#include <cstdint>

namespace ss {

enum class ActionId : std::uint32_t {
    None = 0,
    // Scene / show flow
    SceneSelect = 1,        // value = scene slot 0..7 on the current bank (or absolute scene id when target says so)
    NextScene = 2, PrevScene = 3, BankNext = 4, BankPrev = 5,
    Pause = 6, Resume = 7,  // pause/resume automation
    SetMode = 8,            // value: 0 Auto, 1 Programmed, 2 Hybrid
    NextCue = 9, PrevCue = 10, GoToCue = 11,
    // Blackout (latched; restore is explicit)
    VisualBlackout = 20, LightingBlackout = 21, EmergencyBlackout = 22, RestoreBlackout = 23,
    // Continuous look controls (value 0..1)
    Brightness = 30, Speed = 31, Intensity = 32, ColorHue = 33, ColorSaturation = 34,
    // Tempo / key
    SetBpm = 40, TapTempo = 41, ReleaseBpmOverride = 42, SetKey = 43, ReleaseKeyOverride = 44,
    // Text
    TextPublish = 50, TextClear = 51, TextSelect = 52,
    // Safety
    DisableStrobing = 60, EnableStrobing = 61,
    // Lighting
    ArmLighting = 70, DisarmLighting = 71, LightingDryRun = 72,
    // Recording / broadcast / journal
    StartRecording = 80, StopRecording = 81, AddMarker = 82, StartBroadcast = 83, StopBroadcast = 84,
    // Effects (value 0..1 amount; 0 = off)
    EffectPrismMirror = 90, EffectBloomPulse = 91,
    // Usability
    Undo = 100, Redo = 101, SnapshotStoreA = 102, SnapshotStoreB = 103, SnapshotRecallA = 104, SnapshotRecallB = 105,
    OpenCommandPalette = 106, RunShowDoctor = 107, ExportShowReport = 108,
    // Output
    SelectOutput = 110, ToggleOverlayBpmKey = 111, IdentifyDisplays = 112,
    // Show file
    SaveShow = 120, OpenShow = 121, ExportShow = 122
};

constexpr std::uint32_t id(ActionId a) { return static_cast<std::uint32_t>(a); }

// Actions that must stay reachable and may never be dropped by rate limiting or queue overflow.
constexpr bool isSafetyCritical(ActionId a) {
    return a == ActionId::EmergencyBlackout || a == ActionId::VisualBlackout || a == ActionId::LightingBlackout ||
           a == ActionId::RestoreBlackout || a == ActionId::DisarmLighting || a == ActionId::StopRecording ||
           a == ActionId::StopBroadcast || a == ActionId::DisableStrobing;
}

}  // namespace ss
