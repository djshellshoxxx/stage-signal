# Gap audit: spec vs code

Audit date 28 Oct 2026. Branch `claude/gap-audit`, based on PR #15 head `d4aaab297a7366317074441040517835038082bc` (`claude/wonderful-franklin-kwdd6l`, which sits directly on `main` `62db701`).
Sources of truth: `StageSignal-Specification-v0.1.md`, `docs/spec-addendum-v0.2.md`, `docs/specs/*.md`. Earlier handoffs were treated as history.

## 0. How to read this

| Class | Meaning |
|---|---|
| **Done** | Real implementation and an automated test that names the requirement, which passed in this audit's own run. Hardware/OS limits are listed separately. |
| **Partial** | Real code exists but the requirement's acceptance criterion is not fully met or not provable here (missing app wiring, adapter, platform branch, measurement). |
| **Missing** | No implementation, or only a null/fake adapter or registration entry. |

"Compiles" never counts. Hardware, protocol interoperability and legal clearance are **not** claimed anywhere below (see `docs/KNOWN_LIMITATIONS.md`). Status vocabulary in `docs/TRACEABILITY.md` is stricter (`Implemented` needs merged + green CI); this file does not change that table.

## 1. Evidence gathered in this audit

| Check | Result |
|---|---|
| `git ls-remote origin` | PR #15 head = `d4aaab2…` = `claude/build-to-spec` = `claude/wonderful-franklin-kwdd6l`. `main` = `62db701…`. |
| Open PRs | #13 `feat/app-contract` (non-draft, base main, `014429`), #14 `docs/readme-and-wiki` (draft), #15 (draft, base main). No others open. #13 and #15 overlap (PR #15 contains the app layer). |
| Core build | `cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Release`, build exit 0, **0 compiler warnings** in the log. |
| `ctest` | **19/19 modules passed**, 3.0 s. Doctest case counts per module: see §3. |
| PR #15 CI (head `d4aaab2`) | `core tests` ubuntu/macos/windows: success; `app compile + headless smoke (linux)`: success. Beta workflow (windows-x64, linux-x64, macos-arm64, macos-x86_64, release): success (check-runs from the GitHub API, not re-run here). |
| App target | See §2 (filled in after the local build/selftest run). |
| Stub scan | `grep` for TODO/FIXME/stub in `core` and `app/Source`: no TODO/FIXME in shipped paths. Genuine non-implementations are listed in §4. |

## 2. App target verification (this machine)

Configured with JUCE `ea77444750de45e036e0279bb0291144bf8e3688` and clap-juce-extensions `7adee3a1bd4684d4caa5601100e364abccff4b4d` (the repo's pins in `cmake/ss_deps.cmake`). Results are recorded in `docs/HANDOFF.md` under "Tests actually run".

## 3. Requirement matrix

Test column: doctest case count that names the requirement (`REQ-x: ...`) per `tests/unit`. "app" = headless `--selftest` registered in `app/Source/Tests` (listed in HANDOFF once run).

### 3.1 REQ-A (audio and analysis)

| ID | Class | Evidence |
|---|---|---|
| A01 device select + stable identity | **Partial** | No core test (by design, app-owned). `app/Source/Engine/Desktop/JuceAudioSource.cpp` is a real `AudioDeviceManager` source; self-test `adapters-audio-no-device` covers the no-device path only. No real device run. |
| A02 unavailable/lost shown <1 s, no silent fallback | **Partial** | `AppController.cpp:214`, `UiModel.h:42` model it as a state; supervisor retry in `JuceAudioSource`. Test: `adapters-audio-no-device` only (loss/return path unexercised with a device). No core `REQ-A02` doctest although TRACEABILITY promises one. |
| A03 RMS/peak/bands | **Done** | `audio/test_dsp.cpp` (9) |
| A04 onsets | **Done** | `audio/test_dsp.cpp` (3) |
| A05 tempo | **Done (synthetic)** | `audio/test_tempo_key.cpp` (10); synthetic clicks only (L-03) |
| A06 tap/manual BPM | **Done** after `claude/ss-overrides` | Tap/override tests as before, plus the override is now saved with the show (`ShowDocument::overrides`, schema minor 1, validated 30..300 BPM) and applied on load: `show/test_overrides.cpp` (5), `persist/test_show_json.cpp` (3 new), app self-test `shell: overrides travel with the state`. |
| A07 key | **Done (synthetic)** | `audio/test_tempo_key.cpp` (7); no real-music corpus (L-03) |
| A08 manual key | **Done** after `claude/ss-overrides` | As A06: manual key override saved and restored (tonic 0..11, minor flag); tests above. |
| A09 per-output BPM/key labels | **Partial -> Done in the app after `claude/ss-text-layer`** | Core `router/test_router.cpp` (2) was green, but at audit time the app never used it: `RenderHost` set no compositor policy and no overlay, and nothing handled `ToggleOverlayBpmKey`, so the Setup toggle did nothing. Now `AppController::shellPushAudiencePolicies` feeds the router policy per output to `RenderHost`, which draws the overlay from the effective tempo/key. Tests: app self-tests `audience: REQ-A09 ...` and `audience: ToggleOverlayBpmKey reaches the router` (both fail with the wiring removed). Runtime-only: the toggle is not saved in the show file. |
| A10 input vs output clip meters | **Done** | `audio/test_dsp.cpp` (2), `dynamics/test_dsp.cpp` (1) |

### 3.2 REQ-D (screens and control)

| ID | Class | Evidence |
|---|---|---|
| D01 roles on matched identity | **Partial** | `router/test_router.cpp` (6). Display identity on desktop is a hash of mode+scale (`Engine/Desktop/DesktopUi.cpp:16`); identical monitors collide (P-01). Unmerged branch `fix/macos-display-identity` (`41f5986`) adds native identities; **not** in PR #15 and not reviewed here. |
| D02 third screen | **Done (logic)** | `router/test_router.cpp` (1); app window code exists (`UI/Audience`), ui-setup self-test |
| D03 extra screens | **Done (logic)** | `router/test_router.cpp` (3) |
| D04 800x600 fit | **Done** | compose (5), render (2) |
| D05 audience has no diagnostics | **Done** | `router` (2), `compose/test_font_overlay` (2), app `ui-setup: audience display guard` |
| D06 lost output never covers controls | **Done (logic)** | `router/test_router.cpp` (10); real hot-unplug not tested |
| D07 resolution/rotation | **Done** | compose (2), render (2), router (1) |
| D08 blackout | **Done** | `show/test_safety.cpp` (5), compose (2), router (1) |
| D09 idle screen / audio-loss policy within 2 s | **Missing at audit time; addressed in PR #17** (`claude/ss-audio-health`, draft; becomes Partial: pref-only control, built-in idle frame) | `docs/specs/show.md` stated "Not done"; no audio-health input, no test. |
| D10 keyboard wizard at 800x600 | **Partial** | App self-test `ui-setup: wizard keyboard path`; not run on real screens; no core test. |
| D11 keyboard/MIDI value actions | **Done** | midi (1), palette (4), show (2) |

### 3.3 REQ-S (show, media, camera, safety)

| ID | Class | Evidence |
|---|---|---|
| S01 auto selection | **Done** | `show/test_auto.cpp` (10) |
| S02 programmed cues | **Done** | `show/test_programmed.cpp` (9) |
| S03 manual override pauses | **Done** | `test_programmed.cpp` (4) |
| S04 pause/resume | **Done** | `test_programmed.cpp` (3) |
| S05 text draft vs publish | **Done in the app after `claude/ss-text-layer`** (draft still lives only in the UI) | Published text now goes through `compose::TextLayer` per output via a `TextRequest` handed to the render thread; only the selected output (`ShowSnapshot::selectedOutput`, or all) changes, and nothing changes until `PublishText`. Test: `audience: REQ-S05 ...` (fails when the target check is removed). |
| S06 text layout | **Done** | `compose/test_text.cpp` (8) for layout; the app now uses it (placement, size, duration, fade, one scroll pass) through prefs `text.duration`, `text.fade`, `text.scroll`, `text.y`, `text.size`, and honours the per-output `showText` policy (`audience: REQ-S06 ...`). No UI controls for these yet. |
| S07 stills/GIF/video profiles | **Partial** | `media/test_media.cpp` (17) covers validation/import rules. App decodes stills only; **GIF shows first frame, no video decode** (app-adapters status). Moving-media playback exists only on unmerged `fix/macos-display-identity`. |
| S08 missing media placeholder | **Done** | `media/test_media.cpp` (5) |
| S09 camera preview/format | **Partial** | `compose/test_camera.cpp` (1); the **camera adapter is `NullCameraSource`** (`Engine/AdapterSet.cpp:16`); `juce_video` not linked. |
| S10 camera layer effects | **Done** | `compose/test_camera.cpp` (9) |
| S11 lost camera | **Partial** | `compose` (4) with fakes; no real source. |
| S12 Disable Strobing | **Done** | show (5), render (2), lighting (2) |
| S13 export/import bundle | **Partial** | `persist` (13). ZIP I/O in app (`SafeZip.cpp`, 10 hostile archives); Linux only. |
| S14 invalid show rejected | **Done** | persist (8), security (13) |
| S15 asset manifest | **Done (metadata only)** | `media/test_media.cpp` (6). Cannot judge whether a stated licence is true. No repo-level THIRD-PARTY notices file (see §4). |
| S16 offline works, no login | **Partial (Linux only)** | Superseded finding: PR #18 added self-test `shell: native idle has no Internet sockets` (`app/Source/Tests/ReadinessSelfTests.cpp`): native adapters, 10 s idle, no TCP/UDP socket owned by the process. CI runs `--selftest all` on Linux (`ci.yml`, job `app-linux`), and that job was green on the merged head. Not covered: Windows/macOS, saving/rendering while network is explicitly disabled, "no login" is by absence of any login code. Proxy only: owning no socket is not the same as proving zero connection attempts. |

### 3.4 REQ-M, REQ-L

| ID | Class | Evidence |
|---|---|---|
| M01 learn/edit | **Done (logic)** | midi (23) |
| M02 notes/CC/clock out | **Done (logic)** | midi (17). Timing from fakes. |
| M03 feedback protection | **Done** | midi (9) |
| M04 lost connection | **Done (logic)** | midi (9); app `adapters-midi-no-ports` only |
| L01 fixture profiles | **Done** | lighting (12) |
| L02 Art-Net/sACN | **Done (loopback)** | lighting (23); app `adapters-udp-loopback` (Linux loopback only; Winsock/macOS branches not compiled here) |
| L03 preview sends nothing | **Done** | lighting (5); app `adapters-lighting-gate` |
| L04 blackout | **Done** | lighting (8) |
| L05 fault, stays disarmed | **Done** | lighting (7) |
| L06 named adapters | **Partial** | `lighting/test_packets.cpp` (3) covers the naming rule. **No USB-DMX adapter is supported**: `NullSerialDmx` only. |

### 3.5 REQ-P, REQ-X (performance, reliability)

| ID | Class | Evidence |
|---|---|---|
| P01 benchmark report | **Partial** | perf (21). Real workloads and OS counters are app-layer (`ProcessPerfCounters`, Linux verified only). |
| P02 benchmark isolation | **Done** | perf (3) |
| P03 adaptation | **Done** | perf (9) |
| P04 locked quality | **Done** | perf (3) |
| P05 bounded queues, capture never waits | **Done (logic)** | dynamics (4), media (1), record (1); app `shell: callback no alloc` |
| P06 BASE 25 fps | **Partial** | perf (11), render (1) check gates/logic. **Not measured on an 800 MHz machine (L-01).** |
| X01 atomic save | **Done** | persist (32); app durable FS (Linux) |
| X02 keyboard + focus | **Partial** | palette (36) core keymap; visible focus and non-colour labels are UI and only covered by self-tests. |
| X03 decoder failure isolation | **Done (logic)** | media (3). Decoding is in-process stills only. |
| X04 no public listener | **Partial (Linux only)** | The same idle-socket self-test (above) checks the running process owns no TCP/UDP socket when idle; `security/test_policy.cpp` (2) checks the policy. Not covered: sockets while lighting is armed (UDP by design), other OSes, a port scan from outside. The test fails closed where `/proc/net/tcp6`/`udp6` are missing (this sandbox). |
| X05 diagnostics timeline | **Done** | diag (8) |
| X06 4 h soak | **Partial** | `SoakMonitor` logic (perf, 3). No soak has been run. |

### 3.6 REQ-R, REQ-B (record, broadcast)

| ID | Class | Evidence |
|---|---|---|
| R01 record program + audio | **Partial** | record (9). WAV works end to end via `RecordRunner`; video needs an external ffmpeg with an eligible encoder. Linux self-tests use a **fake ffmpeg script** (`record:video-via-fake-ffmpeg`). |
| R02 compressor/limiter default | **Partial (true-peak limiter added, opt-in)** after `claude/ss-truepeak` | `dynamics/test_dsp.cpp` (13), record (1). New opt-in true-peak limiter validated on a synthetic adversarial set against an independent reference meter (`test_truepeak_limiter.cpp`); default remains the sample-peak limiter; no music corpus, so qualification stays open. App does not apply the dynamics processing delay to recorded audio timestamps (existing gap). |
| R03 A/V sync | **Partial** | dynamics (7), record (2). Logic only; not measured over 1 h (L-05). Windows/Linux host time is estimated (P-02). |
| R04 disk-low graceful | **Done (logic)** | record (9), dynamics (1) |
| R05 audio-only recording | **Done** | record (10), dynamics (12) |
| R06 start failures actionable | **Done** | record (5) |
| B01 RTMPS/SRT | **Partial** | broadcast (16). Local fake-ffmpeg only (L-07). |
| B02 explicit start | **Done** | broadcast (9); two-step start in `BroadcastRunner` |
| B03 secrets never leak | **Partial** | broadcast (8) + diag (10) + persist (3). **Stream key is on the ffmpeg argv** (P-10). OS secret stores: Windows/Linux unverified, **macOS Keychain (now implemented on branch `claude/ss-keychain-mac`; CI-compiled only, never run against a real keychain) not implemented** (`SecretStore.cpp:124`), no file fallback. |
| B04 bounded reconnect | **Done (logic)** | broadcast (15), record (3) |
| B05 state + stop <1 s | **Done (logic)** | broadcast (7) |
| B06 network optional | **Done** | broadcast (3) |
| B07 RTP | **Missing** | one doctest covers refusing/labelling; RTP is "documented but not implemented" (P2, deferred in research-deferred). |

### 3.7 Added features

| ID | Class | Evidence |
|---|---|---|
| NEW-01 Prism Mirror | **Done** | `render/src/effects.cpp`; 3 render doctests (names are `PrismMirror: ...`, not `NEW-01`). Nearest sampling; bilinear not done. |
| NEW-02 Bloom Pulse | **Done (proxy-measured)** after branch `claude/ss-tests-gaps` | At audit time acceptance 2 had no test. Adding one exposed a real defect (15-60 flashes/s with an onset every frame); fixed with attack/release in `core/render/src/effects.cpp`. See `docs/specs/render.md`. Whole-frame luminance proxy only. |
| NEW-03 Undo/Redo, A/B | **Done** | `show/test_edit.cpp` (≥10 cases); app `ui-other: undo and fade` |
| NEW-04 Energy Arc | **Done (synthetic)** | `audio/test_arc.cpp` (8), show arc cases (6) |
| NEW-05 Show Report | **Done** | `diag/show_report.cpp`; app `ui-other: report export guards` |
| NEW-06 Show Doctor | **Done** | `doctor/test_doctor.cpp` (19). The app fills the snapshot (disk, lighting conflicts) per app code; `ui-other: doctor input from snapshot`. |
| NEW-07 Command Palette | **Done** | palette (51); app `ui-other: palette search and confirm` |
| FEAT-001..003 | **Done** | audio/palette/show tests (10/11/12) |

Row-naming drift: NEW-xx and FEAT tests are not all named with their ID, so the traceability table's "Test" column cannot be grepped for NEW-01..07 (only FEAT-xxx). Renaming tests is cheap and non-destructive; do it in the module branch that touches them.

## 4. Area-level gaps (cross-cutting)

| # | Area | Finding | Class | Smallest closing action |
|---|---|---|---|---|
| G1 | Packaging and CI | No `THIRD-PARTY-NOTICES` / LICENSE file at repo root; `skills/` and `shared/scripts/` hold unrelated tooling and third-party skill text. Dependency pins are in `cmake/ss_deps.cmake` only. | Missing | Add notices file (JUCE, clap-juce-extensions, CLAP SDK, doctest, nlohmann/json: licences read from `third_party/` and the pinned checkouts). **Not** changing the project licence (DEC-05 is the owner's). |
| G2 | Packaging and CI | `beta.yml` / VST3: commit `815314a` (VST3 in release matrix) exists on an unmerged branch and conflicts with DEC-11 (VST3 is a non-goal). Not in PR #15. | Info | Owner decision. |
| G3 | CI | Core tests run once per OS with no ccache, no path filters; app job not run on macOS/Windows per push (beta workflow only). | Partial | See user CI-speed rules; do after features. |
| G4 | App contract | `docs/specs/app-contract.md` still labels `DesktopAdapters` and `MediaWorker` as STUB. They are real now. Stale doc. | Partial | Edit doc status. |
| G5 | Camera | `JuceCameraSource` exists for Windows/macOS (added with PR #18, CI-compiled, `CameraSelfTests` use fakes); there is none on Linux (JUCE limit). Never run against a real camera. | Partial | Manual pass per OS. |
| G6 | Serial DMX | `NullSerialDmx` only; no supported adapter (REQ-L06 names none). | Missing | Owner decision on model (Q-03); until then keep Unavailable. |
| G7 | Secrets | macOS Keychain adapter written (`claude/ss-keychain-mac`) and compile-checked by CI; Windows/Linux/macOS stores all unverified at runtime. | Partial | Run the secrets round trip on each OS (manual). |
| G8 | Moving media | Video/GIF animation not decoded in the app. | Missing | Module `ss-media-decode` (ffmpeg child process route is already in the spec; no new dependency). |
| G9 | Audio-loss/idle policy (D09) | Not implemented. | Missing | Module `ss-audio-health` (core `show` + app wiring). |
| G10 | X04 port scan | Idle-process check exists on Linux (PR #18); armed-state and non-Linux enumeration do not. | Partial | Extend the self-test to the armed lighting state (expect exactly the declared UDP socket) and add Windows/macOS equivalents. |
| G11 | True-peak limiter | Implemented opt-in (PR `claude/ss-truepeak`), synthetic-validated only. | Partial | Qualification on a music corpus and a codec round trip (needs the owner's corpus); BASE-tier CPU measurement. |
| G12 | S16 offline test | Covered on Linux by the PR #18 idle-socket test; see S16. | Partial | Windows/macOS equivalents. |
| G13 | Display identity | Hash only. | Partial | Evaluate `fix/macos-display-identity` (separate PR, not merged here). |

## 5. Not provable without hardware or people

Real devices (audio, MIDI, camera, DMX nodes/adapters), 800 MHz BASE benchmark, 1 h A/V sync, 4 h soak per OS, live RTMPS/SRT receivers, assistive technology, code signing/notarisation, licence clearance. All remain **Open** in `docs/KNOWN_LIMITATIONS.md`.

## 6. Priority order for the build phase

1. G9 D09 audio-loss/idle policy (P1, core logic + test first, small).
2. G10/G12 remainder: armed-state socket check, Windows/macOS equivalents.
3. A06/A08 override persistence (model + persist field). G1 notices file; G4 stale contract doc.
4. UI controls for text options and the BPM/key toggle state; persist the overlay toggle in the show file.
5. G8 moving-media decode; G5 camera adapter (needs macOS/Windows to verify; CI only).
6. G7 macOS Keychain (CI-only verification).
7. Owner-dependent: G2, G6, G11, G13, licence (DEC-05).
