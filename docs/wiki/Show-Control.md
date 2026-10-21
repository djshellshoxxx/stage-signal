# Show Control

**Status: core done (`show` module, unit-tested on Linux), UI wiring planned.** The rules below are what the show engine enforces. The buttons and tabs that call them are not built yet.

## Modes

| Mode | Behaviour |
|---|---|
| Auto | Picks a scene from the eligible pool every `max(min dwell, auto interval)` seconds. |
| Programmed | Runs an ordered cue list. A cue can wait for a trigger (for a foot controller), hold until triggered, or run for a set time in seconds or beats. |
| Hybrid | Auto pool plus manual override. |

**Override.** Touching a scene pad, stepping a scene, or recalling a snapshot sets a manual override. In Auto mode the show also switches to Hybrid. While overridden, automatic changes and cues do not fire. Explicit cue buttons still work. Resume clears the override.

**Pause.** Freezes cue timing and dwell. Resume 0 continues. Resume 1 advances to the next cue or picks a new scene, then continues.

**Cue edge cases.** A beat cue with no valid tempo waits and reports it. It never guesses a BPM. A cue that uses a strobe scene is skipped while strobing is disabled. At the end of a list the show holds on the last cue. Looping is a tunable.

## Blackout

| Control | Effect | Clear it with |
|---|---|---|
| Visual blackout (Ctrl+F8) | Blacks out screens | Restore |
| Lighting blackout (Ctrl+Shift+F8) | Sends safe values to DMX | Restore |
| Emergency blackout (F8) | Both of the above | Restore (Shift+F8) |

Blackouts latch. Pressing one again does nothing. Automation keeps running underneath, so Restore shows where the show has got to. Blackout wins over cues.

## Undo and redo

- Depth: 100 steps.
- Undoable: look controls, mode changes, pool and cue edits, and snapshot recalls.
- Not undoable: scene pads, cue triggers, pause and resume, blackouts, strobe lock, tempo and key overrides, output selection, and snapshot stores.
- A knob sweep on one control within 750 ms is one step.
- Undo and redo return the show to the same serialised state.

## A/B snapshots

- **Store** (Shift+F2 for A, Shift+F3 for B) captures the scene and parameters of the target output into slot A or B.
- **Recall** (F2 for A, F3 for B; see [Command Palette and Shortcuts](Command-Palette-and-Shortcuts)) crossfades to the stored look over 0 to 10 seconds. Zero is a cut.
- The indicator shows A* after you change the look away from A.
- Recall is refused for an empty slot, a missing scene, or a strobe scene while the strobe lock is on.

## Tempo and key

`SetBpm` (30 to 300) and `TapTempo` override the detector until `ReleaseBpmOverride`. `SetKey` and `ReleaseKeyOverride` do the same for key. Overrides survive loading another show. Saving them into the show file is not wired yet (see [Show File Format and Migration](Show-File-Format-and-Migration)).

## Safety-critical actions

Blackouts, Restore and DisableStrobing skip the queue. They take effect at submit time and are never delayed to a beat.
