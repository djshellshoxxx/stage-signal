// StageSignal base: shared contract types IF-01..IF-11 (spec section 10).
// Every module depends on these; changing a field here requires an ARCHITECTURE.md note.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"

namespace ss {

// ---- IDs and generations -------------------------------------------------------------------
using Generation = std::uint32_t;    // bumped on every device/session reopen; stale ones are discarded
using Revision = std::uint64_t;      // show-state revision, monotonic, single writer (MOD-04)
using OutputId = std::uint32_t;      // logical output id (never a monitor index)
constexpr OutputId kAllOutputs = 0xFFFFFFFFu;
constexpr OutputId kPrimaryOutput = 1;

// ---- IF-01 AudioBlock ----------------------------------------------------------------------
enum AudioFlags : std::uint32_t {
    kAudioDiscontinuity = 1u << 0, kAudioSilent = 1u << 1, kAudioTimestampValid = 1u << 2
};
struct AudioBlockView {            // borrowed view; never retained across threads
    const float* interleaved = nullptr;
    std::uint32_t frames = 0, channels = 0, rateHz = 0;
    std::uint64_t firstSampleIndex = 0;   // sample counter since stream open
    Ns captureNs = 0;                     // QPC-derived capture time of first sample
    std::uint32_t flags = 0;
    Generation generation = 0;
};
struct AudioBlock {                // owning, pooled copy handed to consumers
    std::vector<float> samples;           // interleaved float32
    std::uint32_t channels = 0, rateHz = 0, flags = 0;
    std::uint64_t firstSampleIndex = 0;
    Ns captureNs = 0;
    Generation generation = 0;
    std::uint32_t frames() const { return channels ? static_cast<std::uint32_t>(samples.size() / channels) : 0; }
    AudioBlockView view() const {
        return {samples.data(), frames(), channels, rateHz, firstSampleIndex, captureNs, flags, generation};
    }
};

// ---- IF-03 FeatureSnapshot -----------------------------------------------------------------
enum class ValueSource : std::uint8_t { Unknown, Estimated, Tap, Manual };
struct TempoState {
    double bpm = 0.0;                 // 0 with source Unknown means "no estimate", never numeric zero BPM
    float confidence = 0.f;           // 0..1 calibrated reliability
    Ns ageNs = 0;                     // time since estimate last updated
    ValueSource source = ValueSource::Unknown;
    bool valid() const { return source != ValueSource::Unknown && bpm > 0.0; }
};
struct KeyState {
    int tonic = -1;                   // 0=C .. 11=B, -1 Unknown
    bool minor = false;
    float confidence = 0.f;
    Ns ageNs = 0;
    ValueSource source = ValueSource::Unknown;
    bool valid() const { return source != ValueSource::Unknown && tonic >= 0; }
};
struct FeatureSnapshot {
    float rmsDb[2] = {-120.f, -120.f};     // per channel, 50 ms window
    float peakDb[2] = {-120.f, -120.f};
    float band[3] = {0.f, 0.f, 0.f};       // low 30-250, mid 250-2k, high 2k-12k; linear energy 0..~1
    float onsetStrength = 0.f;             // 0..1 normalised positive spectral flux
    bool onsetFired = false;               // true on the snapshot where an onset was emitted
    float energyArc = 0.f;                 // 0..1 long-term energy (Energy Arc feature)
    std::uint8_t arcPhase = 0;             // 0 steady, 1 build, 2 drop (EnergyPhase)
    bool inputNearFullScale = false;       // >= -0.5 dBFS, "possible input clipping"
    std::uint32_t clipRunLength = 0;
    TempoState tempo;
    KeyState key;
    Ns sourceNs = 0;                       // capture time the snapshot describes
    bool silent = true;
};

// ---- IF-02 SubmitAction --------------------------------------------------------------------
enum class ActionSource : std::uint8_t { Ui, Keyboard, Midi, Show, Remote, System };
enum class TriggerKind : std::uint8_t { Immediate, NextBeat, NextBar, AtTime };
struct Action {
    std::uint32_t actionId = 0;            // from the action registry (core/palette), never a raw string
    ActionSource source = ActionSource::Ui;
    OutputId target = kAllOutputs;
    std::uint32_t fixtureId = 0;
    double value = 0.0;                    // normalised unless the action documents units
    TriggerKind trigger = TriggerKind::Immediate;
    Ns triggerTime = 0;                    // for AtTime
    Revision expectedRevision = 0;         // 0 = do not check
    std::uint64_t correlationId = 0;
};
struct ActionResult {
    bool accepted = false;
    std::string reason;                    // sanitized, empty when accepted
    Revision revision = 0;
};

// ---- IF-05 ShowSnapshot (minimal shared view; MOD-04 owns the full state) -------------------
enum class ShowMode : std::uint8_t { Auto, Programmed, Hybrid };
struct ShowSnapshotLite {
    Revision revision = 0;
    ShowMode mode = ShowMode::Auto;
    std::int32_t cueIndex = -1;
    std::uint32_t sceneId = 0;
    std::uint64_t seed = 0;
    ValueSource tempoSource = ValueSource::Unknown;
    bool manualOverride = false;
    bool visualBlackout = false, lightingBlackout = false, paused = false;
    bool strobingDisabled = false;
};

// ---- IF-06 OutputRoute ---------------------------------------------------------------------
enum class RouteKind : std::uint8_t { Controls, Audience, Duplicate, Independent };
struct OutputRoute {
    OutputId logicalId = 0;
    RouteKind kind = RouteKind::Audience;
    std::string displayIdentity;           // stable identity, NEVER a monitor index
    Generation generation = 0;
    int viewportW = 800, viewportH = 600, rotationDeg = 0;
    OutputId duplicateOf = 0;              // for Duplicate: shared composition source
    bool showBpmKey = false, showText = true, showBranding = false, showDiagnostics = false;
    bool connected = false;
};

// ---- IF-07 VisualFrame ---------------------------------------------------------------------
struct PixelBuffer {                       // RGBA8 premultiplied alpha, sRGB, tightly packed rows
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    PixelBuffer() = default;
    PixelBuffer(std::uint32_t w, std::uint32_t h) : width(w), height(h), rgba(std::size_t(w) * h * 4, 0) {}
    std::size_t stride() const { return std::size_t(width) * 4; }
    std::uint8_t* row(std::uint32_t y) { return rgba.data() + y * stride(); }
    const std::uint8_t* row(std::uint32_t y) const { return rgba.data() + y * stride(); }
    bool empty() const { return width == 0 || height == 0; }
};

// ---- IF-08 ScheduledOutput -----------------------------------------------------------------
enum class OutputKind : std::uint8_t { Midi, LightingFrame };
struct ScheduledMidi { std::uint8_t bytes[3] = {0, 0, 0}; std::uint8_t size = 0; Ns deadline = 0; std::uint32_t portId = 0; Generation generation = 0; };

// ---- IF-09 CapabilityStatus ----------------------------------------------------------------
struct CapabilityStatus {
    std::string profileHash;
    bool metricsAvailable = false;
    double fps = 0, p95FrameMs = 0, cpuPct = 0;
    std::uint64_t workingSetBytes = 0;
    std::vector<std::string> admitted, rejected;   // feature ids
    std::vector<std::string> reasons;
    int adaptationStep = 0;
};

// Camera/preset enumerations shared by renderer, show and UI.
enum class TriggerLevel : std::uint8_t { Off, Low, Medium, High };

}  // namespace ss
