// StageSignal show engine (MOD-04): public value types. See docs/specs/show.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/model/show_model.hpp"
#include "ss/show/audio_health.hpp"

namespace ss::show {

// Hard limits (spec sections 8 and 13). Checked before any allocation they guard.
struct Limits {
    static constexpr std::size_t kQueueCapacity = 256;     // normal (non-critical) queued + pending actions
    static constexpr std::size_t kHistoryDepth = 100;      // undo depth (redo is bounded by the same)
    static constexpr std::size_t kJournalCapacity = 1024;  // bounded journal queue (diag drains it)
    static constexpr std::size_t kEventsPerTick = 64;      // events carried in one snapshot
    static constexpr std::size_t kScenePadsPerBank = 8;
    static constexpr std::size_t kMaxOutputs = 16;
    static constexpr std::size_t kRepeatHistoryMax = 64;
    static constexpr int kSnapshotSlots = 2;                // A and B
};

// All behavioural tunables in one struct. Defaults are the shipped behaviour; tests override them.
struct Tunables {
    // --- tempo grid / quantisation
    int beatsPerBar = 4;
    double minBpm = 30.0, maxBpm = 300.0;
    double regridBpmEpsilon = 0.05;           // estimate jitter below this never moves the grid
    Ns quantiseFallbackNs = 4 * kNsPerSec;    // quantised trigger with no tempo fires after this wait
    Ns tapResetNs = 2500 * kNsPerMs;          // gap that starts a new tap sequence
    int tapWindow = 8;                        // taps averaged
    // --- auto mode
    double autoIntervalSec = 16.0;            // scene lifetime; never below doc.minDwellSec
    bool quantiseAutoToBar = true;            // when tempo is valid, auto changes land on a bar boundary
    model::TransitionKind autoTransition = model::TransitionKind::Crossfade;
    double autoTransitionSec = 1.0;
    model::TransitionKind manualTransition = model::TransitionKind::Crossfade;
    double manualTransitionSec = 0.25;
    // --- Energy Arc driven auto show
    bool energyArcEnabled = true;             // prefer pool scenes whose tier matches the arc level
    float lowMax = 0.33f, mediumMax = 0.66f;  // energyArc < lowMax -> Low, < mediumMax -> Medium, else High
    std::uint32_t tierMatchWeight = 400, tierAdjacentWeight = 100, tierFarWeight = 50, tierAnyWeight = 100;
    bool reserveDropScenes = true;            // Drop-tier scenes are only chosen by a drop (unless pool is all Drop)
    bool dropEnabled = true;                  // arcPhase==2 schedules a bar-quantised Drop-tier scene change
    double dropExpireSec = 8.0;               // a drop that could not land within this window is skipped
    double dropCooldownSec = 4.0;             // minimum gap between executed drops
    // --- safety
    float strobeLockedBloomMax = 0.0f;        // Bloom Pulse is the only flash-capable effect; capped while locked
    // --- programmed mode
    bool loopCues = false;
    // --- audio loss (REQ-D09)
    AudioLossPolicy audioLossPolicy = AudioLossPolicy::Continue;   // Idle = audience shows the idle screen on loss
    AudioHealthConfig audioHealth;                                  // detect (<= 2 s) and restore-hold times
    // --- undo
    Ns coalesceNs = 750 * kNsPerMs;           // continuous-control edits within this window share one undo step
};

enum class EventKind : std::uint8_t {
    DocumentLoaded, SceneChanged, CueFired, CueSkipped, CueListEnded,
    DropScheduled, Drop, DropSkipped,
    ModeChanged, OverrideChanged, Paused, Resumed,
    BlackoutChanged, StrobeLockChanged, TempoOverrideChanged, KeyOverrideChanged,
    SnapshotStored, SnapshotRecalled, UndoApplied, RedoApplied, EditApplied, ActionRejected,
    AudioLost, AudioRestored
};
const char* eventKindName(EventKind k);

struct ShowEvent {
    EventKind kind = EventKind::SceneChanged;
    OutputId output = kAllOutputs;       // logical root output, kAllOutputs for global events
    std::uint32_t sceneId = 0;
    std::int32_t cueIndex = -1;
    Ns atNs = 0;
    std::string detail;                  // sanitized, short
};

struct OutputState {
    OutputId id = 0;
    OutputId mirrorOf = 0;               // 0 = this output holds its own scene state; else the root it mirrors
    bool hasScene = false;
    std::uint32_t sceneId = 0;           // scene the output is on (incoming scene while a transition runs)
    bool hasFrom = false;                // outgoing scene while transitionProgress < 1
    std::uint32_t fromSceneId = 0;
    double transitionProgress = 1.0;     // 0..1, 1 when idle
    model::TransitionKind transition = model::TransitionKind::Cut;
    bool hasNext = false;                // Programmed: next eligible cue's scene
    std::uint32_t nextSceneId = 0;
    std::uint64_t sceneSeed = 0;
    model::SceneParams params;           // effective params after strobe stripping
    bool strobeStripped = false;         // true when strobe content/effect was removed or must be suppressed
};

struct LookSlot {
    bool used = false;
    std::uint32_t sceneId = 0;
    model::SceneParams params;
};

struct ShowSnapshot {
    Revision revision = 0;
    Ns atNs = 0;
    ShowMode mode = ShowMode::Auto;
    std::uint64_t seed = 1;
    // cues
    std::int32_t cueIndex = -1;
    std::int32_t cueCount = 0;
    double cueElapsedSec = 0.0, cueElapsedBeats = 0.0;
    bool cueHolding = false;             // waiting for a trigger (hold cue, or next cue waitForTrigger)
    bool cueWaitingForTempo = false;     // beat-based cue but no valid tempo: elapsed beats are not advancing
    bool programEnded = false;
    // outputs (one entry per OutputDef, document order)
    std::vector<OutputState> outputs;
    // flow
    bool manualOverride = false, paused = false;
    bool visualBlackout = false, lightingBlackout = false, emergencyBlackout = false;
    bool strobingDisabled = false;
    bool lightingStrobeAllowed = true;   // lighting side reads this: false = strobe channels must be held at safe value
    // tempo / key (effective: override wins over estimate)
    TempoState tempo;
    KeyState key;
    bool tempoOverride = false, keyOverride = false;
    std::uint64_t barIndex = 0;          // bars since the downbeat anchor (see docs: not a detected downbeat)
    int beatInBar = 0;
    bool gridValid = false;
    // pads
    int bank = 0, bankCount = 1;
    OutputId selectedOutput = kAllOutputs;
    // usability
    LookSlot slots[Limits::kSnapshotSlots];
    std::uint32_t undoDepth = 0, redoDepth = 0;
    // energy arc state
    model::EnergyTier wantedTier = model::EnergyTier::Any;
    bool dropPending = false;
    // audio health (REQ-D09): `audioLost` = loss confirmed; `audioIdle` = policy says the audience shows the idle screen
    bool audioLost = false, audioIdle = false;
    // events emitted since the previous snapshot
    std::vector<ShowEvent> events;
    std::uint32_t eventsDropped = 0;

    ShowSnapshotLite lite() const {
        ShowSnapshotLite l;
        l.revision = revision; l.mode = mode; l.cueIndex = cueIndex;
        l.sceneId = outputs.empty() ? 0 : outputs.front().sceneId;
        l.seed = seed; l.tempoSource = tempo.source; l.manualOverride = manualOverride;
        l.visualBlackout = visualBlackout; l.lightingBlackout = lightingBlackout; l.paused = paused;
        l.strobingDisabled = strobingDisabled;
        return l;
    }
    const OutputState* output(OutputId id) const {
        for (const auto& o : outputs) if (o.id == id) return &o;
        return nullptr;
    }
};

// Editor commands that are not in the action registry (pool membership, cue list edits). They share the
// action queue bound, are validated at submit and again at apply, and are undoable.
enum class EditKind : std::uint8_t { SetPoolMembership, InsertCue, RemoveCue, ReplaceCue, MoveCue };
struct EditCommand {
    EditKind kind = EditKind::SetPoolMembership;
    std::uint32_t sceneId = 0;           // SetPoolMembership
    bool inPool = true;                  // SetPoolMembership
    std::int32_t index = 0;              // Insert/Remove/Replace/Move(from)
    std::int32_t toIndex = 0;            // Move(to)
    model::CueDef cue;                   // Insert/Replace
    Revision expectedRevision = 0;
};

struct Stats {
    std::uint64_t submitted = 0, accepted = 0, rejected = 0, queueRejected = 0;
    std::uint64_t journalDropped = 0, eventsDropped = 0, criticalApplied = 0;
    std::size_t queued = 0, pending = 0;
};

}  // namespace ss::show
