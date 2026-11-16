# Roadmap and Deferred Work

This page lists what is left. It does not promise dates. The authoritative lists are `docs/spec-addendum-v0.2.md`, `docs/specs/research-deferred.md` and `docs/KNOWN_LIMITATIONS.md`.

## Open owner decisions

| Item | Question | Where |
|---|---|---|
| Q-01, Q-02 | Is a physical legacy fixture available for the 800 MHz and 512 MB tier, or is that floor dropped? | Addendum, `research-deferred` section 7 |
| Q-03 | Which broadcast receivers and USB-DMX models must be supported? | Addendum |
| DEC-05 | Confirm the AGPL-3.0-or-later licence, or budget a commercial JUCE licence | Addendum, `KNOWN_LIMITATIONS` D-06 |
| Signing | Budget and publisher identity for Windows and macOS signing | `research-deferred` section 8 |
| True-peak | Qualify a true-peak limiter. Today only a meter exists. | `dynamics` spec |
| DEC-10 | A key-detection corpus. Accuracy on real music is unknown. | Addendum, `KNOWN_LIMITATIONS` L-03 |
| CLAP host | Which host is the first officially supported one (REAPER is suggested) | `research-deferred` section 9 |

## Core modules

All portable core modules named in `docs/ARCHITECTURE.md` now exist under `core/` (audio, midi, router, media, record, broadcast, perf, doctor, palette and the rest), each with unit tests under `tests/unit/`. What remains is the JUCE app layer below and the hardware evidence further down. Per-module status is tracked in `docs/TRACEABILITY.md`.

## App layer (the largest gap)

- Audio device capture and the `AudioIngress` path
- Operator UI (800 x 600 layout, eight tabs, status bar, bottom strip, wizard)
- Display enumeration and audience windows
- Art-Net and sACN UDP transport, serial USB DMX
- FFmpeg child process and recording writers
- OS secret store, file dialogs, zip reader
- Thread wiring in `AppController`

## Deferred by owner or research

| Item | Status |
|---|---|
| WebRTC (WHIP) | Deferred. Only through FFmpeg if wanted later. |
| RTP and RTCP contribution | Small, possible later through FFmpeg |
| projectM renderer | Optional, off by default, no presets shipped |
| Audience sound moments and silhouettes | Needs a written privacy decision first |
| MIDI 2.0 | Deferred |
| Projection mapping, all-output recording | Non-goals |
| Legacy-OS shell for the 800 MHz floor | No-go for v0.2 |

## Evidence still missing

These need a person and real equipment, and are recorded in `docs/TRACEABILITY.md` as manual:

- Hardware qualification for each DMX node and adapter (L-02)
- Sync measured over an hour on real devices (L-05)
- Four-hour soak on every OS (L-06)
- Live broadcast receivers (L-07)
- Assistive technology pass (L-08)
- Music-corpus accuracy (L-03)

## How this list changes

When an item moves, update the module spec's Status, the traceability row, and the matching limitation, in one PR. Nothing moves to Implemented on a spec-only change.
