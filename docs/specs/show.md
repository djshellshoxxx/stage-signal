# MOD-04 Show engine (`ss::show`)

Sole writer of show state. Portable C++17, depends on `base` and `model` only (it consumes `ss::FeatureSnapshot`
from base, so there is no dependency on `audio`). Everything is driven by the `Ns` passed to `tick()` (and an
optional `IClock` that stamps submissions), so every behaviour below is reproducible in tests with `FakeClock`.

Spec coverage: UJ-02..04; REQ-S01..S04, S12, D08, D09 (engine side only), D11, A06, A08 (handling); IF-02, IF-05;
section 11 blackout controls (action semantics only); section 14 precedence (blackout wins over cues); T-SHOW.
Additions from DEC-13: **Energy Arc driven auto show** and **Undo/Redo + A/B snapshots**.

## 1. Model

```
submit(Action)  --validate--> inbox (<=256 incl. pending)  --tick--> apply in order --> state
submit(critical) --validate--> applied immediately (never queued, never dropped)
loadDocument(doc) --validate--> staged --next tick--> swap (old stays live on failure)
tick(now, features) --> ShowSnapshot (immutable, revision monotonic, events since last tick)
```

* One mutex guards all public methods. `submit/submitEdit/loadDocument/markDownbeat` may be called from UI, MIDI and
  remote threads while one thread calls `tick`. Not for the audio callback. The journal sink runs after the lock is released.
* Revision starts at 1 and increments once per tick that changed discrete state (scene, flags, params, mode,
  overrides, history depth, document load). Idle time, transition progress and elapsed counters never bump it, so
  `expectedRevision` is a usable conflict check. `expectedRevision == 0` means "do not check".
* Roots and outputs: the primary root is the first non-Duplicate `OutputDef`. Extra `Independent` outputs are extra
  roots with their own scene state. `Duplicate` outputs mirror their source's root; any other `Audience/Controls`
  output mirrors the primary root. Automation (auto picks, cues, drops) drives the primary root only.
  Independent roots change only by targeted actions (or `kAllOutputs`). A target naming a duplicate resolves to its
  source. Unknown target -> rejected "unknown output".

## 2. Modes (REQ-S01..S04, UJ-02..04)

| Mode | Behaviour |
|---|---|
| Auto | Weighted pick from the eligible pool every `max(minDwellSec, autoIntervalSec)` of running time. |
| Programmed | Cue list in order. Duration in seconds or beats (`TimeBase`), `0` = hold until trigger, `waitForTrigger` = entering that cue needs NextCue/GoToCue/Resume-advance (foot controller). Transition kind/duration come from the cue. |
| Hybrid | Auto pool, plus manual override. |

**Auto pool.** Eligible = `inAutoPool` and not (`strobe` while strobing is disabled). Candidates exclude the current
scene and the last `repeatHistory` scenes shown (history includes manual and cue scenes). If that leaves nothing, the history
window shrinks one step at a time down to 0, but the current scene is never a candidate. Consequences: pool of 2
alternates A,B,A,B; pool of 1 never changes (the dwell timer re-arms, no loop-spin); empty pool holds. Picks use
`ss::Rng` seeded from `doc.seed`, consumed only when a pick is made, so the same document + same feature stream gives
the same sequence on every platform (tested over 3 runs). Minimum dwell is measured in running time (frozen by
Pause/override) and applies to automatic changes and drops; explicit operator actions ignore it.

**Override (REQ-S03).** Manual scene selection (SceneSelect, NextScene, PrevScene, snapshot recall) that touches the
primary root sets `manualOverride`; in Auto it also switches mode to Hybrid (UJ-02). While overridden, no automatic
cue fires and no auto/drop change happens. Explicit cue triggers (NextCue/PrevCue/GoToCue) still execute (they are operator
actions), the override stays. `Resume` clears Pause and override together. `SetMode` to another mode clears the override.

**Pause/Resume (REQ-S04).** Pause freezes cue elapsed and dwell; cue index is kept. Resume `value 0` continues:
elapsed counters resume where they stopped, the picture does not change (so after an override the manual scene stays
until the next automatic event). Resume `value 1` advances explicitly to the next cue (Programmed) or does an auto pick
(Auto/Hybrid), then continues. Any other value is rejected.

**Cue edge cases.** Beat cues with no valid tempo do not advance and report `cueWaitingForTempo` (no guessed BPM).
Cues whose scene is a strobe scene are skipped (CueSkipped event) while strobing is disabled. End of list: stays on the
last cue, `programEnded`, one CueListEnded event (`Tunables::loopCues` restarts). Cue referencing a missing scene fails
document validation.

**Pads.** `SceneSelect`: `value` = slot 0..7 on the current bank (scene index = bank*8 + slot); if `Action.fixtureId != 0`
it is an absolute scene id instead. BankNext/BankPrev wrap.

## 3. Blackout and strobe lock (REQ-D08, REQ-S12, section 14)

* `VisualBlackout`, `LightingBlackout`, `EmergencyBlackout` set latches; `RestoreBlackout` is the only way back.
  Pressing a blackout again does nothing. Emergency sets both effective latches plus `emergencyBlackout`.
  Blackout is independent of Pause; automation and cues keep running underneath, scene state is untouched, so restore
  shows the state automation reached. Snapshot `visualBlackout = visual||emergency`, `lightingBlackout = lighting||emergency`.
  Consumers (router, lighting) apply the blackout to the final composition/fixture output; the engine publishes state.
* Safety-critical ids (`isSafetyCritical`, those that show handles: the three blackouts, Restore, DisableStrobing) are applied
  at submit time, bypass the queue bound, ignore stale revision and any non-Immediate trigger (a blackout is never delayed
  to a beat). Others in the base list (DisarmLighting, StopRecording, StopBroadcast) belong to other modules and are
  rejected here with "action not handled by show engine"; the app dispatcher must route them to their owner.
* `DisableStrobing`: rejects SceneSelect/NextScene/GoToCue/recall of strobe scenes, rejects `EffectBloomPulse` above
  `Tunables::strobeLockedBloomMax` (default 0; Bloom Pulse is the only flash-capable effect, `PrismMirror` is unaffected),
  immediately moves any live strobe scene to the first non-strobe scene (Cut), skips strobe cues, excludes strobe scenes
  from the pool and drops. Snapshot: `strobingDisabled`, `lightingStrobeAllowed=false` (lighting must hold strobe channels at
  their safe value), per-output `strobeStripped` and clamped bloom amount. If a show has no non-strobe scene the scene stays
  but `strobeStripped=true` tells the renderer to suppress flashes. The lock is sticky across `loadDocument`; only
  `EnableStrobing` clears it. `doc.strobingDisabled` turns it on at load.

## 4. Tempo, key and quantisation (REQ-A06, REQ-A08, IF-03)

* Owner of the override state is this engine. `SetBpm` (30..300, source Manual), `TapTempo` (average of the last 8 taps,
  a gap over 2.5 s starts a new sequence, needs 2 taps; `Action.triggerTime` may carry the input arrival time, else the
  submit time is used), `ReleaseBpmOverride`. `SetKey` value 0..11 major, 12..23 minor (tonic = value mod 12);
  `ReleaseKeyOverride`. Override wins over the detector until released; the snapshot carries the effective `TempoState`/`KeyState`
  with `source` Manual/Tap. Overrides survive document loads. Persisting them in the show file is the persist module's job
  (`document()` does not include them; see Status).
* **Bars without downbeat certainty.** `BeatGrid` has an anchor that is beat 0 of a bar. Anchor = time of the engine's first
  tick, or the time given to `markDownbeat(t)` (the operator's last "bar" action). Bars are `beatsPerBar` (4) beats from the
  anchor. A tempo change (beyond `regridBpmEpsilon` 0.05 bpm) re-anchors on the last beat of the old grid, so beat/bar phase is
  continuous. Nothing is inferred from audio about where bar 1 is. `barIndex/beatInBar` in the snapshot are counted from that anchor.
* Triggers: `NextBeat`/`NextBar` fire at the first tick at or after the first boundary strictly after the submit time.
  They are rejected at submit when no valid tempo exists; if the tempo vanishes while pending they fire after
  `quantiseFallbackNs` (4 s). `AtTime` fires at the first tick `>= triggerTime` (past times fire on the next tick; negative rejected).
  Pending items count toward the 256 bound.

## 5. Energy Arc driven auto show (new)

Scenes carry `model::EnergyTier` (Any/Low/Medium/High/Drop).
* **Level preference.** Each tick maps `FeatureSnapshot.energyArc` to a wanted tier (`< lowMax` 0.33 Low, `< mediumMax`
  0.66 Medium, else High; `arcPhase==2` -> High; `silent` input or `energyArcEnabled=false` -> Any). Pool weights: tier match 400,
  adjacent tier 100, far tier 50, tier Any 100. Drop-tier scenes are reserved for drops (weight 0) unless nothing else is eligible.
* **Drop.** On the rising edge of `arcPhase == 2` (Auto/Hybrid, not paused/overridden, past `dropCooldownSec` 4 s) a
  drop is scheduled (DropScheduled event, journal kind `drop`). It executes on the first tick at or after the next bar boundary
  after detection: picks a Drop-tier pool scene (not current, not strobe when locked, repeat-history relaxed like normal picks),
  crossfades, journals a `drop`/Drop event. If minimum dwell has not elapsed at that bar it retries on the following bar. It is
  skipped (DropSkipped, reason in event) when it expires after `dropExpireSec` (8 s), no drop-tier scene is eligible, automation
  is held (pause/override/mode), or strobe lock removed the candidates. Without a valid tempo it is unquantised ("quantise=none")
  and fires on the first tick that satisfies dwell. Fully deterministic; every tunable is in `Tunables`.
* Auto changes themselves also land on the next bar when a tempo is valid (`quantiseAutoToBar`).

## 6. Undo/Redo and A/B snapshots (new)

* History depth 100 (oldest dropped); a new edit clears redo; loading a document clears both.
* **Rule.** Undoable = durable edits: look controls (Brightness, Speed, Intensity, ColorHue, ColorSaturation,
  EffectPrismMirror, EffectBloomPulse; written through to the `SceneDef` so they persist), `SetMode`, pool membership and cue-list
  edits (via `submitEdit`), and `SnapshotRecallA/B`. **Not** undoable = live performance actions: scene pads (SceneSelect,
  NextScene/PrevScene, banks), cue triggers, Pause/Resume, all blackouts, strobe lock, tempo/key overrides, SelectOutput, SnapshotStore,
  Undo/Redo themselves. Edits of the same control on the same target within `coalesceNs` (750 ms) are one step (a knob sweep).
* Cue edits adjust the current cue index so it keeps pointing at the same cue; undo/redo restore it exactly.
* `SnapshotStoreA/B` captures the target root's scene + params (incl. effect amounts) into slot A/B (target `kAllOutputs` = primary).
  `SnapshotRecallA/B` restores scene and params on the targeted roots (it is a manual scene change, so it engages override like a pad),
  journals kind `snapshot`, and is one undo step. Recall is refused when the slot is empty, the scene is gone, or it is a strobe scene while locked.
  Undo/redo of a recall are likewise refused while the lock would be violated.

## 7. Limits, failure behaviour, journal

* Queue 256 (inbox + pending, edits included); history 100; events per snapshot 64 (overflow counted in `eventsDropped`);
  journal queue 1024 (drops newest, `Stats::journalDropped`); outputs 16; repeat history <= 64; scenes/cues from `model::Limits`.
* All validation happens before queueing and again at apply; an action that became invalid is dropped with an ActionRejected event.
  `ActionResult.reason` strings are fixed sanitized text (no user strings). Document errors: `Status` with `InvalidArgument` (or
  `SchemaUnsupported`); the live document is untouched.
* Document swap discards queued non-critical actions (one ActionRejected event says so), resets cue/auto/override/pause state and
  history, reseeds the Rng, keeps blackout latches, strobe lock, tempo/key overrides.
* Journal: `setJournalSink` (called from `tick` after unlock) or `drainJournal` (bounded queue). Entries are `model::JournalEntry`
  (`monoNs`, `wallUnixMs` from the clock, `kind` in scene|cue|mode|safety|tempo|drop|snapshot|edit|doc|reject, `detail` =
  "EventName cause=...").

## 8. Tests (`tests/unit/show`, doctest, ~0.1 s total)

REQ-S01 seeded x3, history window (1,2,3,5), pool 1/2/empty, dwell, ineligible scenes, bar-quantised auto, tier steering, silent input;
Energy Arc drop at bar, dwell deferral, strobe skip, no-tempo, not in Programmed; REQ-S02 order in seconds/beats, no tempo wait, waitForTrigger,
next/prev/goto, missing cue/scene, list end/loop; REQ-S03 override with 120 s of no auto fire, Hybrid switch; REQ-S04 pause/resume/advance;
REQ-D08 latch arbitration, restore, pause independence, immediate apply, survive load; REQ-S12 strobe rejection/strip/clamp/sticky;
IF-02 rejection reasons table, queue bound + critical bypass, pending bound, stale revision; IF-05 revision monotonic; IF-11 failed swap;
REQ-A06/A08 tap/set/release/key; quantised NextBeat/NextBar/AtTime/fallback, downbeat anchor, grid phase; IF-06 outputs; D11 banks;
undo/redo coalesce/depth/mode/pool/cues/snapshots; journal sink/queue bounds.

## Status

* **Done and unit-tested:** everything in sections 1-7 above.
* **Partial / judgement calls to confirm:** `Speed` and the other look controls store the normalised 0..1 value as given (`SceneParams.speed`
  defaults to 1.0, so the renderer must define its range); Bloom Pulse is treated as the only flash-capable effect and is capped to 0
  while locked (the model has no per-effect strobe flag); bar counting is anchor-based by design (no detected downbeat); a Programmed cue
  drives only the primary root; the tempo/key overrides are not part of `ShowDocument` so a saved show does not carry them (REQ-A06/A08 "save/load
  preserves override" needs a model field or persist to store `tempoOvr/keyOvr` and call a setter; engine API for this is `SetBpm/SetKey` actions).
* **REQ-D09 (audio-loss policy), added 28 Oct 2026:** `AudioHealthMonitor` (`ss/show/audio_health.hpp`) debounces the app's "capture is
  delivering" flag (`ShowEngine::setAudioAvailable`). Loss is confirmed after `detectNs` (default 1 s, hard max 2 s, invalid config falls back to
  defaults), cleared after `restoreHoldNs` (default 500 ms) of stable audio. `ShowSnapshot::audioLost` reports the confirmed loss; `audioIdle`
  is true only with `AudioLossPolicy::Idle` (`setAudioLossPolicy`, default Continue). Neither changes the revision, blackout latches or scene.
  Journal kind `device` (`AudioLost` / `AudioRestored`). App wiring: `AppController::showTick` feeds `analysis_.signalAlive`, `RenderHost`
  draws the idle screen and hides media/text layers while `audioIdle`, pref key `audio.losspolicy` = `idle` | `continue`. **Not done:** a
  Setup-tab control for the policy (pref only), a configurable idle-screen *content* (the compositor's built-in idle frame is used), and
  persisting the policy in the show document (it is an app preference). Tests: `REQ-D09:` cases in `tests/unit/show/test_audio_health.cpp`,
  app self-test `shell: audio loss idle`.
* **Not done:** no UI/renderer wiring beyond D09; no 3-thread stress test (single mutex design only reasoned about); not run on MSVC/macOS here (gcc and clang on Linux only).
* **Needs from others:** an ActionId for "mark downbeat" (the engine exposes `markDownbeat(Ns)` meanwhile); a `ShowDocument` field for tempo/key overrides;
  the app dispatcher must route non-show ids (recording, broadcast, DisarmLighting...) to their owners.

## Integration API

```cpp
#include "ss/show/engine.hpp"            // namespace ss::show; link target ss::show (deps base, model)
ss::show::ShowEngine eng(&clock, ss::show::Tunables{});   // clock optional (stamps submissions + journal wall time)

// 1. Load (any thread). Validates fully; returns Status. Swap happens at the next tick. Old show stays on failure.
ss::Status st = eng.loadDocument(doc);

// 2. Operator/MIDI/remote input (any thread). Validate-first; reason is a sanitized string when rejected.
ss::Action a; a.actionId = ss::id(ss::ActionId::NextScene); a.target = ss::kAllOutputs;
a.expectedRevision = lastSnapshot.revision;   // optional conflict check
ss::ActionResult r = eng.submit(a);           // r.accepted / r.reason / r.revision
eng.submit(blackoutAction);                   // safety-critical: applied now, never queued
ss::show::EditCommand e; /* pool / cue edits */ eng.submitEdit(e);
eng.markDownbeat(clock.nowMonotonic());       // operator "bar" button; bars count from here

// 3. The one writer loop (one thread, e.g. 60 Hz or audio-block rate):
ss::show::ShowSnapshot s = eng.tick(clock.nowMonotonic(), latestFeatureSnapshot);
//    publish s (or s.lite()) to render/router/lighting/UI via a LatestMailbox or shared_ptr.
//    s.events = SceneChanged/CueFired/Drop/... since the previous tick (bounded 64).
//    Others may read eng.latest() (copy) at any time.

// 4. Consumers read the snapshot, never mutate:
//    s.outputs[i]: id, mirrorOf, sceneId, fromSceneId/hasFrom/transitionProgress/transition, nextSceneId, params, strobeStripped
//    router: apply s.visualBlackout to the final composition; lighting: s.lightingBlackout, s.lightingStrobeAllowed
//    UI: mode, cueIndex/cueElapsedSec/cueHolding, paused, manualOverride, tempo/key (+tempoOverride), slots[2], undoDepth/redoDepth, bank

// 5. Persist: eng.document() returns the live ShowDocument (param/pool/cue edits, current mode, strobe lock) for the persist module.
// 6. Diag: eng.setJournalSink(fn) or eng.drainJournal(vec); eng.stats() for queue/reject/drop counters.
// Routing note: show handles SceneSelect..BankPrev, Pause/Resume, SetMode, cues, blackouts, look controls, tempo/key,
// strobe, Undo/Redo, Snapshot*, SelectOutput. Everything else returns "action not handled by show engine".
// Action conventions: SceneSelect value=slot 0..7 (fixtureId!=0 = absolute scene id); Resume value 0 continue / 1 advance;
// SetMode 0 Auto/1 Programmed/2 Hybrid; SetKey 0..23 (+12 = minor); GoToCue value = cue index; SelectOutput value = id or -1.
```

## Overrides saved with the show (REQ-A06 / REQ-A08, 28 Oct 2026)
`ShowDocument::overrides` (`model::TempoKeyOverrides`) holds the operator's tempo override (BPM 30..300, labelled Manual or Tap) and manual key
(tonic 0..11 plus minor). `ShowEngine::document()` fills it from the live override; loading a document that carries one applies it
(the tap history is cleared). A document without an override keeps the operator's running override, as before: a load never silently drops it.
Invalid values (BPM outside 30..300 or not finite, tonic outside 0..11) are rejected by `validateDocument` before any state changes.
Persistence: JSON keys `tempoOverride {bpm, source: manual|tap}` and `keyOverride {tonic, minor}`, written only when active; schema minor is now 1 and
older readers report the keys as unknown fields. Tests: `show/test_overrides.cpp`, `persist/test_show_json.cpp`, app `shell: overrides travel with the state`.
