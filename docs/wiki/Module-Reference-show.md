# Module Reference: show

**Purpose.** The show engine (MOD-04). The sole writer of show state. It takes actions, applies them in order at each tick, drives scene choice (Auto, Programmed, Hybrid), cues, blackouts, strobe lock, tempo and key overrides, the Energy Arc drops, undo and redo, and A/B snapshots. It emits an immutable snapshot each tick and a journal. It has no I/O, no threads and no clock of its own.

**Depends on.** `base`, `model`.

## Public headers

| Header | Contents |
|---|---|
| `engine.hpp` | `ShowEngine`: `loadDocument`, `validateDocument`, `submit`, `submitEdit`, `markDownbeat`, `tick`, `latest`, `document`, `setJournalSink`, `drainJournal`, `stats` |
| `types.hpp` | `Limits`, `Tunables`, `ShowSnapshot`, `ShowEvent`, `EventKind`, `EditCommand`, `EditKind`, `LookSlot`, `OutputState`, `Stats` |
| `beat_grid.hpp` | `BeatGrid`, the bar and beat anchor |

Sources: `engine.cpp` (about 1300 lines), `validate.cpp`.

## Key contracts

- `submit` validates and applies safety-critical actions at once. Other actions queue (256 items, including pending triggers).
- `loadDocument` validates completely, stages the document and swaps it at the next tick. A bad document leaves the live show in place.
- `tick(now, features)` returns the snapshot. Its revision bumps only when discrete state changes, so `expectedRevision` works as a conflict check.
- Undo depth 100. Redo is cleared by a new edit. Performance actions are never undoable.
- Journal queue 1024 entries. Overflow drops the newest and counts it.
- Up to 16 outputs. The primary root is the first non-duplicate output.

## Tests

`tests/unit/show`: 73 doctest cases in `test_auto.cpp`, `test_edit.cpp`, `test_programmed.cpp` and `test_safety.cpp`, with a shared rig in `rig.hpp`. Names follow `REQ-` and `NEW-` where the spec gives them.

## Status (from `docs/specs/show.md`)

- Done and unit tested on Linux with gcc and clang: sections 1 to 7 of the spec.
- Partial: `Speed` and the look controls store the 0 to 1 value as given, so the renderer must define the range. Bloom Pulse is the only flash-capable effect and is capped while locked. Bar counting is anchor based, not detected.
- Not done: the idle-screen and audio-loss policy (D09). No renderer or UI wiring. No three-thread stress test. Not run on MSVC or macOS in the module's own run.

## Needs from others

- An `ActionId` for "mark downbeat". The engine has `markDownbeat(Ns)` in the meantime.
- A `ShowDocument` field for tempo and key overrides.
- The app dispatcher routes recording, broadcast and lighting actions to their own owners.

## Limits

| Item | Value |
|---|---|
| Queue | 256 |
| Undo history | 100 |
| Journal queue | 1024 |
| Events per tick | 64 |
| Scene pads per bank | 8 |
| Outputs | 16 |
| Tempo | 30 to 300 BPM manual |
| Tap history | last 8 taps, gap over 2.5 s starts again |
