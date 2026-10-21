# MIDI and Controllers

**Status: planned.** There is no `midi` core module in the repository yet. The action registry (`ActionId`) and the mapping data model (`model::MidiMapping`) exist in `base` and `model`. The device adapter, the mapping engine and the Learn flow are specified in `docs/specs/app-adapters.md` section 5 and `app-ui.md` section 5.4, but not built.

## What it will do

- Any MIDI input can be mapped to any registered action (scene pads, blackout, tap tempo, cues, undo and so on).
- Learn: move a control, pick an action, save. Esc cancels.
- Mapping table: device, channel, type, number, value range, encoder mode, action.
- MIDI clock out: opt-in, drift-free, generated from the show tempo.
- Feedback to controllers that have lights or motor faters, with loop prevention in the router.

## Learn flow (as specified)

1. Idle: "Waiting for a MIDI message. Move a control. [Cancel]"
2. Captured: device, channel, type and number are shown.
3. Pick an action from the list, then Save or Cancel.

If the device disconnects during Learn, the flow ends with "Device disconnected. Learn cancelled."

## Rules that will apply

| Rule | Why |
|---|---|
| Input is timestamped on arrival and queued. Parsing happens off the audio and MIDI callbacks. | Real-time safety |
| Mappings can only point at registered action ids. A show file with an unknown id fails validation. | Keeps show files honest |
| Safety actions (blackouts, Restore, DisableStrobing) can be mapped. They are never delayed or dropped. | Operators need them on a pad |
| Arm, Start recording and Start broadcast are not on the keyboard map. The spec wants them behind an explicit press, so mapping them to a controller still needs a confirmation step (to be confirmed in the build). | Ambiguous toggles are a show-day hazard |
| SysEx input is off by default and limited to 1024 bytes. | Keeps the input surface small |

## Plugin mode limits

In CLAP mode the wrapper sends only 2 or 3 byte messages. SysEx output is disabled with a visible note. See [Known Limitations](Known-Limitations), P-03.

## Not in v1

MIDI 2.0 and Windows MIDI Services are not supported (P-12). Virtual MIDI ports are available on macOS and Linux only. On Windows you need a third-party loopback driver.

## Testing status

No automated tests exist yet for MIDI, because the module is not in the repo. The traceability rows REQ-M01 to REQ-M04 are still `Specified` (see the repository's `docs/TRACEABILITY.md`).
