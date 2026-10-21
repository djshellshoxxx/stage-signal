# MOD-09 midi: portable MIDI core (`ss::midi`)

Owner: worker `feat/midi`. Depends on `base`, `model`. Spec refs: sections 5 (REQ-M01..M04, D11, X02), 8, 10 (IF-08),
14, 18 (T-MIDI, T-FAULT). The device edge (JUCE `MidiInput`/`MidiOutput`) is NOT in this module: the app adapter feeds bytes
in and writes returned packets out.

## Scope and REQ coverage
| REQ | What the module does | Tests (prefix `REQ-`) |
|---|---|---|
| M01 | Parser/normaliser, learn, mapping table (add/inspect/edit/delete/load/export), absolute and explicit relative modes, name fallback only after operator confirmation | M01 cases in `test_parser`, `test_mapping`, `test_input` |
| M02 | Output scheduler (monotonic deadlines), 24 PPQN clock from a tempo source, Start/Stop/Continue mapped separately, owned-note tracking, bounded SysEx | M02 cases in `test_output` |
| M03 | Origin tags, explicit enable for feedback-risk routes, echo drop, >20 identical trigger events in 1 s latches an alarm and suspends the route | `test_feedback` |
| M04 | Loss marks mappings unavailable at detection (<= 1 s), reconnect clears the old generation, 512 queue + priority latch, 1000 msg/s/port ceiling | `test_input`, `test_output` |
| D11 | MIDI half: brightness/speed/intensity/hue/saturation/next-scene mappings produce `ss::Action` | `test_input` |

## Data flow
```
device callback --bytes--> MidiEngine::onBytes -> Parser -> onMessage(port, generation, msg, now)
  1 generation/availability check (stale callbacks dropped)      6 echo check (output seen on a linked route <= 250 ms)
  2 realtime/SysEx: counted only (clock never rate-limited)       7 storm detector (identical trigger events) + suspension
  3 learn session consumes the message first                      8 per mapping: absolute scale | relative accumulate | trigger edge
  4 mapping match (identity, or confirmed name) by channel/type/#  9 scene-trigger debounce 50 ms
  5 1000 msg/s/port sliding ceiling (critical actions bypass)    10 critical -> PriorityLatch, trigger -> FIFO, continuous -> coalesced
MidiEngine::poll(now, out) -> latch first, then queue FIFO -> std::vector<ss::Action>{source = Midi}
show/UI --> MidiEngine::send(port, msg, deadline, origin, now) -> feedback policy -> OutputScheduler heap
worker --> MidiEngine::dueOutput(now, out) / scheduler().nextWake(now) -> packets for the device write
```

## Rules and limits (all in `limits.hpp`)
- Note-on velocity 0 becomes note-off (parser and engine). Running status and interleaved realtime are handled.
- Mapping value range scales absolute values, clamped 0..1; inverted ranges are rejected. Relative modes (two's complement,
  binary offset, sign bit) are valid only on CC mappings of continuous actions; the engine accumulates ticks into an absolute
  normalised value (`MappingOptions::relativeStep`, `initialValue`). Nothing is guessed: the mode is stored per mapping.
- Continuous actions: Brightness, Speed, Intensity, ColorHue, ColorSaturation, SetBpm, EffectPrismMirror, EffectBloomPulse.
  `SetBpm` carries BPM (20..300), everything else 0..1. All other actions are triggers; `MappingOptions::triggerValue` is the
  value they carry (scene slot for SceneSelect).
- Scene triggers (SceneSelect, Next/Prev Scene/Bank/Cue, GoToCue) debounce 50 ms per mapping; critical actions never debounce.
- Queue 512: same-mapping continuous values coalesce; on overflow the oldest continuous entry is dropped first, then the
  oldest trigger. Blackout/stop class (`ss::isSafetyCritical`) goes to the `PriorityLatch` (one slot per action id),
  bypasses the rate ceiling, debounce, suspension and queue, and drains first. RestoreBlackout is critical but is the one latch entry
  dropped on reconnect (blackouts and stops survive; a stale restore does not).
- Feedback: an output port is a feedback-risk route when an input has the same identity or the operator called `linkPorts`.
  `send()` refuses it until `setFeedbackRouteEnabled`. An action whose origin is `Midi` may not emit a message the linked input
  maps (action->MIDI->action). Inbound messages matching an output sent within 250 ms on a linked route are dropped as echo.
  More than 20 identical trigger events (same port/channel/type/number/value) inside a 1 s window latch a `FeedbackAlarm`,
  suspend the input signature and any recently sent output signature, until `clearAlarm`. Relative encoders, knobs, clock and
  critical actions are never suspended. The alarm is still raised for critical actions.
- Rate ceiling: 1000 messages/s/port (exact sliding window) on input and output, with counters. Realtime clock, note-off and
  transport are exempt on output.
- Output: queue 512 ordinary items (+ must-deliver up to 2048). Already-due CC values coalesce. Stale items are discarded at
  hand-over (clock > 50 ms late, note-on/CC/PC > 250 ms late); note-off and transport never. Priority at equal deadline:
  Stop, Start/Continue, Clock, Note-off, rest. Clock grid is anchored and computed as `anchor + round(k * 2.5e9 / bpm)`
  (no drift); tempo changes re-anchor at the last pulse; an unusable tempo (outside 20..300 or non-finite) holds the clock
  and re-anchors on recovery. Clock and transport are opt-in (`configureClock`).
- Owned notes: every note-on handed to the device is tracked, note-off clears it. `releaseOwned/releaseAllOwned` send note-offs
  for tracked notes only and drop queued note-ons. No CC 123 is ever sent. On port reopen (new generation) leftover owned
  notes are released first.
- SysEx: off by default (input blocked, output `PermissionDenied`); when enabled frames are bounded to 1024 bytes including
  F0/F7, payload must be 7-bit, oversize is refused before queueing.

## Failure behaviour
Errors are `ss::Result`/`Status` with `ErrorCategory` (`InvalidArgument`, `DeviceLost`, `PermissionDenied`, `ResourceExhausted`).
No exceptions, no I/O, no threads, no globals. `MidiEngine` is single-threaded: the app's MIDI worker owns it (device callbacks
push bytes to that worker, or take a lock around `onBytes`).

## Status
- Done and tested: everything in the table above, in portable logic. 59 doctest cases, ~32k assertions, ASan/UBSan clean.
- Clock timing evidence is a simulation: a fake clock with a seeded worker wake-up jitter of 0..2 ms over 10 min at
  120 BPM (28800 pulses, none missing, deadlines within 1 ns of the ideal grid, observed interval error median < 1 ms and p95 < 3 ms
  under that jitter model). It proves the scheduler maths and policies, NOT hardware or OS timer behaviour. No ENHANCED-fixture or
  named-hardware qualification was performed; the app must keep the spec's advisory if a device misses the target.
- Not done here (belongs to the app layer): JUCE port enumeration/open/close, device identity derivation, the worker thread and its
  timer, MIDI learn/mapping UI, writing packets to ports.
- Limits of this PR: input MIDI clock/Start/Stop/Continue is counted but not mappable (`model::MidiMsgType` has no such type);
  input SysEx is parsed and counted but no consumer exists; polyphonic aftertouch/pitch bend/channel pressure are parsed away.
- `MappingOptions` (triggerValue, relativeStep, initialValue) and `nameFallbackConfirmed` are runtime-only because
  `model::MidiMapping` has no fields for them; see "needs from others" in the PR.

## Integration API
```cpp
#include "ss/midi/engine.hpp"              // namespace ss::midi; links ss::midi (base, model)
MidiEngine eng;                            // one owner thread
eng.mappings().load(doc.midi);             // model::MidiMapping list from the show; toModel() to save
eng.setSysexEnabled(false);                // default off

// Ports. Derive a stable identity string in the JUCE adapter (USB ids/serial/endpoint id); name is only a fallback.
Generation g = eng.openInputPort(portId, identity, name, now);   // call again after every reopen
eng.inputPortLost(portId, now);                                  // or eng.updateInputDevices(present, now) from a <=500 ms poll
Generation og = eng.openOutputPort(outId, identity, name, now);
eng.outputPortLost(outId, now);
eng.linkPorts(inId, outId);               // optional: declare a loop-back; same identity is detected automatically

// Input (device callback -> worker): bytes or parsed messages, always with the generation you opened with.
eng.onBytes(portId, g, bytes, n, now);    // or eng.onMessage(portId, g, msg, now)
std::vector<Action> acts; eng.poll(now, acts);   // submit each to show; source == ActionSource::Midi; poll every <= 5 ms

// Learn (REQ-M01)
LearnFilter lf; lf.anyPort = false; lf.portId = p; lf.channel = 255;
eng.learn().begin(lf);                                           // consumes the next Note/CC/PC
const LearnCapture* c = eng.learn().capture();                   // number, observed min/max, normalisedCurrent()
auto m = eng.learn().commit(ActionId::Brightness, EncoderMode::Absolute);   // -> eng.mappings().add(m.value(), opts)
eng.mappings().setNameFallbackConfirmed(id, true);               // only after the operator confirms a name-only match
MappingStatus st = eng.mappingStatus(id);                        // Available | DeviceUnavailable(since) | NeedsNameConfirmation | NoMatchingDevice

// Output. origin = the source of the action that causes this MIDI (pass Midi for MIDI-born actions).
Status s = eng.send(outId, makeNoteOn(ch, note, vel), deadlineOrNow, origin, now);
eng.setFeedbackRouteEnabled(outId, true);                        // explicit operator enable for feedback-risk routes
eng.scheduler().setTempoSource(&tempo);                          // ITempoSource: show tempo (manual/estimated)
eng.scheduler().configureClock({true, outId}, now);              // clock is opt-in
eng.scheduler().setTransportMap({ActionId::Resume, ActionId::Pause, ActionId::None});   // start, stop, continue
eng.scheduler().onShowAction(actionId, now);                     // call for every executed show action
std::vector<OutPacket> out; eng.dueOutput(now, out);             // worker loop: sleep until eng.scheduler().nextWake(now)
// write p.midi.bytes[0..size) (or p.msg.sysex) to the port; drop packets whose p.midi.generation != your port generation
eng.scheduler().releaseAllOwned(now);                            // show stop / app exit: note-offs for our own notes only
// Diagnostics: eng.totals(), eng.counters(port), eng.queueCounters(), eng.scheduler().stats(), eng.feedback().alarms()
// Operator clears a feedback alarm: eng.feedback().clearAlarm(alarm.sig)
```
Call `now` = `IClock::nowMonotonic()`. Loss detection latency is the adapter's enumeration period (use `kDevicePollPeriodNs`
= 500 ms) so the mapping is marked unavailable well inside the 1 s requirement.
