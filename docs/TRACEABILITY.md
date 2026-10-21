# Traceability

Living table. One row per requirement. Update the Status and Evidence cells in the same PR that changes the state; a PR that adds a requirement without a row fails review.

Columns: **ID** requirement id (spec v0.1 section 5, addendum v0.2); **Module** owner (MOD id from the spec, mapped to the core module or app layer in `docs/ARCHITECTURE.md`); **Test** the test that proves it (core tests are doctest cases named `<ID>: ...` per `docs/WORKER_RULES.md`; T-xxx is the spec's test family); **Status**; **Evidence** where to look.

Status values:
- `Specified`: written down with an owner and a test name, nothing implemented. This is the state of every row when this file was created (20 Oct 2026, branch `feat/app-specs`).
- `In progress`: code or tests exist on a branch or open PR (link in Evidence).
- `Implemented`: code merged, automated test merged and green in CI (link the run).
- `Verified (manual)`: needs hardware, a human, or a real OS service; record who ran it, when, on what.
- `Deferred`: moved out of v1 by an owner decision (link the decision).
- `Waived`: owner accepted a gap (link; also appears in KNOWN_LIMITATIONS.md).

Rules: never mark `Implemented` on a spec-only change; a requirement that can only be checked on hardware can reach `Verified (manual)` at best and says so in Evidence; priority P0 rows must not be `Waived` without an owner decision.

Priorities are not repeated per row here; P0 requirements are: REQ-A02, REQ-A10, REQ-D05, REQ-D06, REQ-D08, REQ-S03, REQ-S08, REQ-S11, REQ-S12, REQ-S14, REQ-M03, REQ-M04, REQ-L03, REQ-L04, REQ-L05, REQ-P02, REQ-P05, REQ-R03, REQ-R04, REQ-R06, REQ-B02, REQ-B03, REQ-B06, REQ-X01, REQ-X03, REQ-X04, REQ-X06, REQ-P06.

## Requirements

| ID | Module | Test | Status | Evidence |
|---|---|---|---|---|
| REQ-A01 | audio ingress (app + base) (MOD-02) | `REQ-A01: ...` doctest (T-AUD) | Specified | spec v0.1 sec 6, 10, 12; app-adapters 3.1 |
| REQ-A02 | audio ingress (app + base) (MOD-02) | `REQ-A02: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 6, 10, 12; app-adapters 3.1; app-ui 4.1 |
| REQ-A03 | audio / dynamics (MOD-03) | `REQ-A03: ...` doctest (T-DSP) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A04 | audio / dynamics (MOD-03) | `REQ-A04: ...` doctest (T-DSP) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A05 | audio / dynamics (MOD-03) | `REQ-A05: ...` doctest (T-TEMPO) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A06 | show (MOD-04) | `REQ-A06: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A07 | audio / dynamics (MOD-03) | `REQ-A07: ...` doctest (T-KEY) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A08 | show (MOD-04) | `REQ-A08: ...` doctest (T-KEY) | Specified | spec v0.1 sec 6, 10, 12 |
| REQ-A09 | router (MOD-05) | `REQ-A09: ...` doctest (T-UI) | Specified | spec v0.1 sec 6, 10, 12; app-ui 5.1 |
| REQ-A10 | audio / dynamics (MOD-03) | `REQ-A10: ...` doctest (T-DSP) | Specified | spec v0.1 sec 6, 10, 12; app-ui 5.5 |
| REQ-D01 | router (MOD-05) | `REQ-D01: ...` doctest (T-DISPLAY) | Specified | spec v0.1 sec 6, 8, 11; app-adapters 4.1; app-ui 5.2 |
| REQ-D02 | app (shell/UI) (MOD-01) | `REQ-D02: ...` doctest (T-DISPLAY) | Specified | spec v0.1 sec 6, 8, 11; app-shell 3; app-adapters 4.2 |
| REQ-D03 | router (MOD-05) | `REQ-D03: ...` doctest (T-DISPLAY) | Specified | spec v0.1 sec 6, 8, 11; app-ui 5.2 |
| REQ-D04 | render (MOD-06) | `REQ-D04: ...` doctest (T-DISPLAY) | Specified | spec v0.1 sec 6, 8, 11; app-ui 2 |
| REQ-D05 | router (MOD-05) | `REQ-D05: ...` doctest (T-UI) | Specified | spec v0.1 sec 6, 8, 11; app-adapters 4.2; app-ui 16 |
| REQ-D06 | router (MOD-05) | `REQ-D06: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 6, 8, 11; app-adapters 4.3 |
| REQ-D07 | render (MOD-06) | `REQ-D07: ...` doctest (T-DISPLAY) | Specified | spec v0.1 sec 6, 8, 11 |
| REQ-D08 | router (MOD-05) | `REQ-D08: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 6, 8, 11; app-ui 4.2 |
| REQ-D09 | show (MOD-04) | `REQ-D09: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 6, 8, 11; app-adapters 3.1 |
| REQ-D10 | app (shell/UI) (MOD-01) | `REQ-D10: ...` doctest (T-UI) | Specified | spec v0.1 sec 6, 8, 11; app-ui 9 (wizard), 2 (compact) |
| REQ-D11 | show (MOD-04) | `REQ-D11: ...` doctest (T-UI) | Specified | spec v0.1 sec 6, 8, 11; app-ui 6, 4.2 |
| REQ-S01 | show (MOD-04) | `REQ-S01: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S02 | show (MOD-04) | `REQ-S02: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S03 | show (MOD-04) | `REQ-S03: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S04 | show (MOD-04) | `REQ-S04: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S05 | app (shell/UI) (MOD-01) | `REQ-S05: ...` doctest (T-UI) | Specified | spec v0.1 sec 11, 13, 17; app-ui 11 |
| REQ-S06 | render (MOD-06) | `REQ-S06: ...` doctest (T-MEDIA) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S07 | media (MOD-07) | `REQ-S07: ...` doctest (T-MEDIA) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S08 | media (MOD-07) | `REQ-S08: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 11, 13, 17; app-ui 5.3 |
| REQ-S09 | camera (app) + render (MOD-08) | `REQ-S09: ...` doctest (T-CAM) | Specified | spec v0.1 sec 11, 13, 17; app-adapters 14 |
| REQ-S10 | render (MOD-06) | `REQ-S10: ...` doctest (T-CAM) | Specified | spec v0.1 sec 11, 13, 17 |
| REQ-S11 | camera (app) + render (MOD-08) | `REQ-S11: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 11, 13, 17; app-adapters 14 |
| REQ-S12 | show (MOD-04) | `REQ-S12: ...` doctest (T-SHOW) | Specified | spec v0.1 sec 11, 13, 17; app-ui 5.2 (Safety), 8 |
| REQ-S13 | persist (MOD-13) | `REQ-S13: ...` doctest (T-DATA) | Specified | spec v0.1 sec 11, 13, 17; app-adapters 12 |
| REQ-S14 | persist (MOD-13) | `REQ-S14: ...` doctest (T-SEC) | Specified | spec v0.1 sec 11, 13, 17; app-adapters 11, 12 |
| REQ-S15 | media (MOD-07) | `REQ-S15: ...` doctest (T-LICENSE) | Specified | spec v0.1 sec 11, 13, 17; app-ui 3.3 |
| REQ-S16 | app (shell/UI) (MOD-01) | `REQ-S16: ...` doctest (T-OFFLINE) | Specified | spec v0.1 sec 11, 13, 17; app-shell 9 (offline) |
| REQ-M01 | midi (MOD-09) | `REQ-M01: ...` doctest (T-MIDI) | Specified | spec v0.1 sec 10, 14; app-adapters 5; app-ui 5.4 |
| REQ-M02 | midi (MOD-09) | `REQ-M02: ...` doctest (T-MIDI) | Specified | spec v0.1 sec 10, 14; app-adapters 5 |
| REQ-M03 | midi (MOD-09) | `REQ-M03: ...` doctest (T-MIDI) | Specified | spec v0.1 sec 10, 14; app-ui 5.4 |
| REQ-M04 | midi (MOD-09) | `REQ-M04: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 10, 14; app-adapters 5 |
| REQ-L01 | lighting (MOD-10) | `REQ-L01: ...` doctest (T-DMX) | Specified | spec v0.1 sec 14 |
| REQ-L02 | lighting (MOD-10) | `REQ-L02: ...` doctest (T-DMX) | Specified | spec v0.1 sec 14; app-adapters 6 |
| REQ-L03 | lighting (MOD-10) | `REQ-L03: ...` doctest (T-DMX) | Specified | spec v0.1 sec 14; app-adapters 6; app-ui 10 |
| REQ-L04 | lighting (MOD-10) | `REQ-L04: ...` doctest (T-DMX) | Specified | spec v0.1 sec 14 |
| REQ-L05 | lighting (MOD-10) | `REQ-L05: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 14; app-adapters 6.2; app-ui 5.4 |
| REQ-L06 | lighting (MOD-10) | `REQ-L06: ...` doctest (T-DMX) | Specified | spec v0.1 sec 14; app-adapters 7 |
| REQ-P01 | perf (MOD-14) | `REQ-P01: ...` doctest (T-PERF) | Specified | spec v0.1 sec 7, 8 |
| REQ-P02 | perf (MOD-14) | `REQ-P02: ...` doctest (T-SEC) | Specified | spec v0.1 sec 7, 8 |
| REQ-P03 | perf (MOD-14) | `REQ-P03: ...` doctest (T-PERF) | Specified | spec v0.1 sec 7, 8 |
| REQ-P04 | perf (MOD-14) | `REQ-P04: ...` doctest (T-PERF) | Specified | spec v0.1 sec 7, 8 |
| REQ-P05 | audio ingress (app + base) (MOD-02) | `REQ-P05: ...` doctest (T-RT) | Specified | spec v0.1 sec 7, 8; app-adapters 3.1, 8.4 |
| REQ-R01 | record (MOD-11) | `REQ-R01: ...` doctest (T-REC) | Specified | spec v0.1 sec 12; app-adapters 8, 9 |
| REQ-R02 | audio / dynamics (MOD-03) | `REQ-R02: ...` doctest (T-DSP) | Specified | spec v0.1 sec 12 |
| REQ-R03 | record (MOD-11) | `REQ-R03: ...` doctest (T-SYNC) | Specified | spec v0.1 sec 12; app-adapters 3.2, 3.3 |
| REQ-R04 | record (MOD-11) | `REQ-R04: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 12; app-adapters 9 |
| REQ-R05 | record (MOD-11) | `REQ-R05: ...` doctest (T-REC) | Specified | spec v0.1 sec 12; app-adapters 9 |
| REQ-R06 | record (MOD-11) | `REQ-R06: ...` doctest (T-REC) | Specified | spec v0.1 sec 12; app-ui 5.5; app-adapters 9 |
| REQ-B01 | broadcast (MOD-12) | `REQ-B01: ...` doctest (T-CAST) | Specified | spec v0.1 sec 15, 16; app-adapters 8 |
| REQ-B02 | broadcast (MOD-12) | `REQ-B02: ...` doctest (T-SEC) | Specified | spec v0.1 sec 15, 16; app-ui 5.5 |
| REQ-B03 | security (MOD-15) | `REQ-B03: ...` doctest (T-SEC) | Specified | spec v0.1 sec 15, 16; app-adapters 10, 13 |
| REQ-B04 | broadcast (MOD-12) | `REQ-B04: ...` doctest (T-CAST) | Specified | spec v0.1 sec 15, 16; app-adapters 8.3 |
| REQ-B05 | broadcast (MOD-12) | `REQ-B05: ...` doctest (T-CAST) | Specified | spec v0.1 sec 15, 16; app-adapters 8.3; app-ui 5.5 |
| REQ-B06 | broadcast (MOD-12) | `REQ-B06: ...` doctest (T-OFFLINE) | Specified | spec v0.1 sec 15, 16; app-shell 9 |
| REQ-B07 | broadcast (MOD-12) | `REQ-B07: ...` doctest (T-CAST) | Specified | spec v0.1 sec 15, 16; research-deferred (RTP) |
| REQ-X01 | persist (MOD-13) | `REQ-X01: ...` doctest (T-DATA) | Specified | spec v0.1 sec 8, 16-19; app-adapters 11, 13 |
| REQ-X02 | app (shell/UI) (MOD-01) | `REQ-X02: ...` doctest (T-UI) | Specified | spec v0.1 sec 8, 16-19; app-ui 6, 7, 8 |
| REQ-X03 | media (MOD-07) | `REQ-X03: ...` doctest (T-FAULT) | Specified | spec v0.1 sec 8, 16-19; app-shell 8 |
| REQ-X04 | security (MOD-15) | `REQ-X04: ...` doctest (T-SEC) | Specified | spec v0.1 sec 8, 16-19; app-adapters 6.1 |
| REQ-X05 | diag (MOD-16) | `REQ-X05: ...` doctest (T-DATA) | Specified | spec v0.1 sec 8, 16-19; app-adapters 13; app-ui 14 |
| REQ-X06 | perf (MOD-14) | `REQ-X06: ...` doctest (T-SOAK) | Specified | spec v0.1 sec 8, 16-19; app-shell 13 (soak, manual) |
| REQ-P06 | perf (MOD-14) | `REQ-P06: ...` doctest (T-PERF) | Specified | spec v0.1 sec 7, 8 |

## Added features (DEC-13)

Acceptance criteria are written so each one is a yes/no check. "Test" names the doctest or UI test that will carry it. Owner module is in the Module column; app-ui and app-adapters sections hold the control and platform side.

| ID | Module | Test | Status | Evidence |
|---|---|---|---|---|
| NEW-01 | render (effect) + app-ui 5.1 | `NEW-01: prism mirror ...` render goldens; `ui: layout 800x600` | Specified | DEC-13; app-ui 5.1 (Effects) |
| NEW-02 | render (effect) + show | `NEW-02: bloom pulse ...` flash-rate and range tests | Specified | DEC-13; app-ui 8 (WCAG 2.3.1 bound) |
| NEW-03 | show + app-ui 13 | `NEW-03: undo ...`, `ui: undo` | Specified | DEC-13; app-ui 13 |
| NEW-04 | audio (Energy Arc) + show | `NEW-04: energy arc ...` synthetic build/drop corpus | Specified | DEC-13; app-ui 5.1 |
| NEW-05 | diag + app-ui 14 | `NEW-05: show report ...`, `ui: report export` | Specified | DEC-13; app-ui 14 |
| NEW-06 | doctor + app-ui 10 | `NEW-06: doctor ...`, `ui: doctor read-only` | Specified | DEC-13; app-ui 10 |
| NEW-07 | palette + app-ui 12 | `NEW-07: palette ...`, `ui: palette` | Specified | DEC-13; app-ui 12 |

### Acceptance criteria

**NEW-01 Prism Mirror** (effect, camera and scene layers)
1. Segment count is an integer 2 to 16; default 6; amount 0 is a bit-exact pass-through of the input frame.
2. Output is symmetric: for N segments, the wedge at angle k*360/N is the mirror of its neighbour within 1 LSB in the golden frame.
3. It routes only to the layers it is applied to (same rule as REQ-S10); other layers are unchanged.
4. Cost is bounded: runs inside the BASE frame budget at 800x600 or is refused by the benchmark with a visible reason (REQ-P03).
5. Setting is saved in the show and undoable (NEW-03).

**NEW-02 Bloom Pulse** (effect)
1. Pulse follows onset or beat events with attack and release; brightness change per frame is bounded.
2. The effect never produces more than 3 flashes in any one second window, measured on the golden luminance trace (WCAG 2.3.1 bound), and obeys Disable Strobing (REQ-S12): when locked, amount is clamped so no general-flash threshold crossing occurs.
3. Amount 0 is pass-through; peak output luminance never exceeds the display range (no clipping artefacts in the golden).
4. With no audio events, it holds a steady glow and does not pulse by itself.

**NEW-03 Undo/Redo and A/B snapshots**
1. At least 100 undo steps; slider drags and typing coalesce into one step.
2. Undo then Redo returns to byte-identical show state (compare serialised document).
3. Performance actions (blackout, pause, arm, record, broadcast, tempo tap) are never undoable and never enter the history.
4. A and B each store scene id, parameters and target; recall crossfades over the set time (0 to 10 s, 0 = cut); the indicator shows "A*" after a change.
5. Recall of a snapshot that references a strobe scene while strobe is locked is refused with a reason.
6. History is cleared on opening another show, kept across save.

**NEW-04 Energy Arc**
1. Output is a 0 to 1 value plus phase (Steady, Build, Drop), each with age and confidence.
2. On the synthetic corpus (8-bar build then drop), the Drop event lands within one beat of the true drop for at least 90 percent of corpus items, with at most 10 percent false drops on the steady corpus.
3. In silence or unknown tempo it reports Steady and never triggers a drop hit.
4. Drop hits quantise to the next bar line when tempo is known; otherwise fire unquantised and say so.
5. Tiered scene choice respects pool eligibility, dwell and strobe lock (REQ-S01, REQ-S12). A manual override pauses Arc-driven changes (REQ-S03).
6. It can be turned off per show; off means no behaviour change from v0.1.

**NEW-05 Show Report**
1. Export produces a single self-contained HTML file and/or UTF-8 CSV with the selected categories only.
2. Same journal in gives the same bytes out.
3. No secret, no absolute file path, no raw audio or video appears (regex scan over the output).
4. A CSV cell that begins with `=`, `+`, `-`, `@`, tab or CR is neutralised.
5. An empty journal gives "Nothing to report yet." and writes no file.
6. Disk-full or non-writable folder reports an error and leaves no partial file.

**NEW-06 Show Doctor**
1. Runs read-only: zero datagrams sent, zero recorder or broadcaster opened, camera not started, verified by instrumented null sinks (REQ-L03).
2. Every check in app-ui 10 returns a severity and a one-line cause, and each Blocker or Warning has a fix target.
3. Golden findings match for the fake adapter states in the test set (no audio, one display, missing media, armed at load, low disk, no encoder).
4. Runs automatically before Arm, Start recording and Start broadcast, and a Blocker prevents that action with the reason shown.
5. Completes in under 1 s excluding device enumeration, which has a 3 s timeout.

**NEW-07 Command Palette**
1. Ctrl+K opens it from any tab; Esc closes it and returns focus.
2. Every registered, user-invocable action appears when valid in context; actions not valid show the disabled reason.
3. Fuzzy match ranks prefix and word-start matches first; safety-critical actions rank first for their keywords.
4. Executing goes through the same dispatcher as buttons; the palette cannot Arm, Start recording or Start broadcast without the same inline confirmation.
5. F8 (emergency blackout) still works while the palette is open.
6. Screen-reader users get role, name and selected-item announcements.

## Notes

- App-layer requirements (windows, devices, UI) have their automated tests defined in `docs/specs/app-shell.md` section 13, `app-ui.md` section 17 and `app-adapters.md` section 15. Those tests run under Xvfb with fakes; real hardware, real OS permission prompts and real assistive technology are `Verified (manual)` at best.
- Requirements with no automated test possible by nature (REQ-X06 soak, REQ-D04 VGA check, REQ-L06 adapter qualification, REQ-R03 one-hour sync) stay `Specified` until a person records a result in Evidence.
- Row counts: 72 spec requirements, 7 added features.
