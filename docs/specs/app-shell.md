# app-shell: AppController, threads, lifecycle, standalone vs CLAP

Status: spec v0.2-draft, 20 Oct 2026. Layer: `app/Source/App` and `app/Source/Plugin`. Baseline: `StageSignal-Specification-v0.1.md` (spec) section 8 and 10, `docs/spec-addendum-v0.2.md` (DEC-04, DEC-11), `docs/ARCHITECTURE.md`.
Implementation status (shell-integration, 21 Oct 2026): see the "Implementation status" section just before Sources. Every "verified" below means: read in the pinned source tree (JUCE `8.0.16-security`, sha `ea77444750de45e036e0279bb0291144bf8e3688`; clap-juce-extensions `7adee3a1`, read at `main` on 2026-10-20) or in a cited document, not run.

## 1. Scope

In: the `AppController` that owns every thread and every adapter, wires core modules together, and enforces start/stop order; the audio ingress path shared by standalone and CLAP; plugin-state save/restore; crash recovery; offline-first rules.
Out: UI layout (`app-ui.md`), device adapters (`app-adapters.md`), core module internals (`docs/specs/<module>.md`).

## 2. Requirements (app-level ids; each links to spec REQs)

| ID | Statement | Spec REQ |
|---|---|---|
| APP-SH-01 | One `AppController` instance per process owns all threads, adapters and core objects. Core modules never create threads (WORKER_RULES 6). | REQ-D02, REQ-X03 |
| APP-SH-02 | Show state has exactly one writer: the control thread running `show`. UI, MIDI, palette and remote sources submit `ss::Action` (IF-02) through a bounded queue (256). | REQ-D02, REQ-D11 |
| APP-SH-03 | The audio callback (device or host) only copies, sanitises, timestamps and enqueues. No allocation, lock, file, socket, UI or logging call. | REQ-P05, T-RT |
| APP-SH-04 | Safety-critical actions (`ss::isSafetyCritical`) are never dropped by queue overflow or rate limiting; they use a separate atomic latch plus a priority slot. | REQ-D08, REQ-L04 |
| APP-SH-05 | Startup order is fixed (section 5); nothing transmits, records, broadcasts or publishes camera at startup. Restore or crash recovery always yields idle and disarmed. | REQ-B02, REQ-L05, spec 8 |
| APP-SH-06 | Shutdown order follows spec section 8; each stage has a timeout and records a diagnostic result; a stuck stage never blocks the next stage past its timeout. | spec 8 |
| APP-SH-07 | Standalone: input audio reaches the analyser; the processor output is silence (monitoring off); the stock JUCE input-mute is not used (section 6). | REQ-A01, spec 12 |
| APP-SH-08 | CLAP: host audio feeds the analyser and passes through bit-exact (no gain, no dynamics on the main bus); recording/broadcast use their own tap. | DEC-11 |
| APP-SH-09 | `getStateInformation` returns a versioned, bounded blob that never contains armed flags, secrets, absolute user paths or device-only identifiers; `setStateInformation` never starts any output. | REQ-B03, REQ-S14 |
| APP-SH-10 | Restart after an unclean exit starts in a Recovery state: idle scene, DMX disarmed, recording/broadcast off, camera unpublished, restored copy chosen explicitly by the operator. | REQ-X01, spec 17 |
| APP-SH-11 | Offline-first: with no network the app starts, shows, renders and saves with zero outbound connection attempts and no login. No update check, telemetry or font/asset fetch exists in the code. | REQ-S16, REQ-X04, REQ-B06 |
| APP-SH-12 | A fault in any worker (media, ffmpeg child, camera, lighting, MIDI-out) leaves UI, audio ingress and show engine running and reports the fault through the diagnostics bus. | REQ-X03, T-FAULT |
| APP-SH-13 | Heartbeats from control, render, analysis and scheduler threads are visible to Diagnostics and Show Doctor; a thread silent for more than 2 s raises a warning. | REQ-X05, NEW-06 |
| APP-SH-14 | The same `AudioProcessor` code path is used by both formats; format differences live in a small `HostMode` strategy, not in forks. | DEC-11 |

## 3. Object ownership

```
AppController (message thread creates, destroys)
 |- Settings/Secrets          persist + security cores, platform shims in Engine/
 |- DiagBus                   diag core (bounded journal), fed by all threads through SPSC rings
 |- ShowHost (control thread) show + palette + router cores, owns the ShowSnapshot LatestMailbox
 |- AudioIngress              ring + pooled blocks, sanitiser, timestamp mapper (section 4.2)
 |- AnalysisWorker            audio + dynamics cores
 |- RenderHost (render thread) render core, frame pool, per-output LatestMailbox
 |- OutputManager (UI thread) audience windows, display watcher (app-adapters 4)
 |- LightingScheduler / MidiScheduler (own threads), both DISARMED by default
 |- MediaWorker               decode, camera mailbox, ffmpeg child supervisors
 |- PerfMonitor               perf core, 1 Hz sampler on the control thread
 '- StageSignalProcessor      the juce::AudioProcessor, holds a non-owning pointer to AudioIngress
```

The `StageSignalProcessor` owns nothing heavy. In plugin mode the host creates the processor first, so `AppController` is created lazily on first `prepareToPlay` or first `createEditor`, whichever comes first, and lives in a process-wide `std::shared_ptr` (several plugin instances in one host are refused, see 7.3). In standalone the application class creates the controller first and then the processor.

## 4. Threads and data flow

### 4.1 Thread table

| Thread | Created by | JUCE type, priority | Cadence | Owns | May call | Must never |
|---|---|---|---|---|---|---|
| UI | JUCE message thread | `MessageManager` | event driven, 30 Hz status `Timer` | widgets, audience `Component`s, dialogs | enqueue actions, read mailboxes, `Component` API | block on a mutex held by a worker, decode, write files >4 KiB, wait on a child process |
| AUDIO | device layer (standalone) or host (CLAP) | OS audio thread | one callback per buffer | nothing | `AudioIngress::push`, atomics | allocate, lock, log, touch any other core |
| ANALYSIS | `AppController` | `juce::Thread`, `Priority::high` | wakes per block (counting semaphore, 50 ms timeout) | `audio` and `dynamics` state | publish `FeatureSnapshot` mailbox, push processed blocks to consumer rings | file, socket, UI |
| CONTROL | `AppController` | `juce::Thread`, `Priority::high` | wakes on action queue or 5 ms tick (200 Hz max), 25 Hz show tick | `show`, `router`, `palette`, `perf` state, undo stack | publish `ShowSnapshotLite` mailbox; push `ScheduledMidi` / lighting commands | UI, device handles, blocking I/O |
| RENDER | `AppController` | `juce::Thread`, `Priority::normal` | target 25 to 30 FPS from elapsed time, never per-frame increments | frame pool, software rasteriser state | read feature+show mailboxes, publish frames to per-output mailbox | decode, disk, device |
| LIGHT | `AppController` | `juce::Thread`, `Priority::high` | 25 Hz default, 40 Hz max | UDP sockets (Art-Net/sACN), resolved channel buffers | read latest show+feature snapshots | read raw audio, run when disarmed except to hold blackout frames |
| MIDI-OUT | `AppController` | `juce::Thread`, `Priority::high` | deadline loop, final 1 ms spin | `juce::MidiOutput` handles | `sendMessageNow` | run on UI thread; SysEx off by default |
| MIDI-IN | JUCE `MidiInput` (OS thread) | callback | per message | nothing | push `RawMidi` into SPSC (512) | call core, UI or allocate |
| MEDIA | `AppController` | `juce::ThreadPool` (2 workers) + one thread per ffmpeg child pair | on demand | decoders, camera newest-frame mailbox (capacity 2), ffmpeg stdin/stderr pumps | publish decoded frames, writer state | block the CONTROL thread |
| IO | `AppController` | `juce::Thread`, `Priority::low` | 30 s autosave, journal flush 1 s | atomic file writes, log rotation | persist core | hold the show mutex (there is none; it snapshots an immutable document) |
| CAMERA | JUCE `CameraDevice` (any thread) | callback | per frame | nothing | copy into mailbox | convert pixel formats inline beyond a memcpy |

All mailboxes are `ss::LatestMailbox<T>`; all queues are `ss::SpscQueue<T>` (`core/base/include/ss/base/queue.hpp`). Capacities are the spec section 8 baselines: capture to DSP 250 ms, processed audio to each consumer 2 s, camera mailbox 2, render to encoder 3 frame handles, UI actions 256, MIDI event queue 512, one lighting latest-state mailbox per universe.

### 4.2 Audio ingress (shared by both formats)

`StageSignalProcessor::processBlock` is the only entry. Pseudocode of what it does, in order:
1. Read `AudioIngress` stream generation; if the processor was `prepareToPlay`-ed with new rate/size, the generation changed (staged off the audio thread in `prepareToPlay`).
2. For each input channel mapped by the route (mono or stereo), copy into a pooled `ss::AudioBlock` slot (interleave to float32), count and zero NaN/Inf (`sanitisedCount` atomic).
3. Stamp `firstSampleIndex` (running counter, exact) and `captureNs` via `TimestampMapper` (app-adapters 3.2). Set `kAudioTimestampValid` only when the mapper has converged.
4. `SpscQueue<AudioBlock*>::push`. If full: set `kAudioDiscontinuity` on the next block, advance the counter, drop the oldest on the consumer side (never wait).
5. Format strategy: standalone clears the output buffer; CLAP leaves the buffer untouched (passthrough).

Worst-case callback budget: under 5% of the buffer period at 256 frames/48 kHz (about 0.27 ms). Verified by the T-RT test (section 9).

### 4.3 Control flow of an operator action

UI/keyboard/MIDI/palette -> `Action` (IF-02) -> action queue (256, SPSC per producer, merged by the control thread) -> `show` validates, applies, bumps `Revision` -> publishes `ShowSnapshotLite` -> render, lighting, UI read it. The `ActionResult` returns to the producer through a per-producer result ring; the UI shows rejected reasons (sanitised text from core).
`isSafetyCritical` actions additionally set an atomic latch (`BlackoutLatch`) read directly by RENDER and LIGHT at their next tick, so a stalled control thread cannot delay blackout beyond one render or lighting tick. This satisfies REQ-D08 ("next admitted frame") and REQ-L04 ("next lighting tick").

## 5. Startup order

Mirror of the spec section 8 stop order, plus everything the spec leaves implicit. Each step logs `start.<step>` with duration. A failing step marks that subsystem Unavailable and continues, except steps 1 to 3 which abort to a minimal error window.

| # | Step | Detail | Failure result |
|---|---|---|---|
| 1 | Diagnostics bus + log files | rotation 5 x 5 MiB (spec 16), redaction filters installed first | stderr only, banner in UI |
| 2 | Settings and secret store open | read-only; no secret is read into memory until a feature needs it | settings reset to defaults, copy of old file kept |
| 3 | Crash marker check | `running.lock` present => Recovery mode (section 8) | n/a |
| 4 | Show document | load last show through `persist` validation, else built-in sample show. Old state stays if validation fails (REQ-S14) | empty show, Setup wizard offered |
| 5 | Core engines | `show`, `palette` registry, `router`, `perf` constructed; mode Auto, **paused automation off**, blackout states restored as OFF, strobing per saved safety setting (default: disabled strobing = true on first run) | fatal: error window |
| 6 | Renderer | frame pool preallocated for admitted outputs; software raster baseline | no preview, controls still work |
| 7 | UI and audience windows | operator window first; audience windows only for routes whose stored display identity matches a connected display; unmatched route stays idle, never placed on the controls display (REQ-D06) | route marked Disconnected |
| 8 | Analysis worker | prepared before audio starts so the first block has a consumer | analysis off, meters show Unavailable |
| 9 | Audio | standalone: `AudioDeviceManager::initialise` with the saved device and `selectDefaultDeviceOnFailure=false`. CLAP: wait for host `prepareToPlay` | state "No audio device" (REQ-A02), never another device |
| 10 | MIDI input | open mapped devices by stored identity; unmatched mappings marked unavailable | mappings greyed |
| 11 | Schedulers constructed, **disarmed** | LIGHT and MIDI-OUT threads exist but transmit nothing; lighting frames go to the null sink until Arm (REQ-L03, REQ-L05) | n/a |
| 12 | Camera and media workers | idle; camera opens only when the operator selects it, publication stays off (spec 13) | n/a |
| 13 | Perf monitor | start sampling; benchmark never runs automatically | n/a |
| 14 | Ready | write `running.lock`, emit `startup.complete` with total time | n/a |

Targets (hardware not qualified, number are budgets): cold start to interactive controls under 3 s on a modern desktop; the 800 MHz budget is a design target only (addendum DEC-03).

## 6. Standalone mode

### 6.1 Why a custom standalone app

JUCE's stock standalone wrapper (`StandalonePluginHolder`) keeps a persisted `shouldMuteInput` flag that defaults to true and, when set, replaces the input buffers passed to the processor with silence before `processBlock` (JUCE@8.0.16-security `modules/juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h`, constructor sets `shouldMuteInput = ! isInterAppAudioConnected()`, load defaults `getBoolValue ("shouldMuteInput", true)`, callback replaces `inputChannelData` with `emptyBuffer`). With that default, StageSignal's analyser would see silence. The wrapper also owns the `AudioDeviceManager`, so the Setup tab could not own device selection.

Decision D-SHELL-01: define `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1` (documented in `juce_events/messages/juce_Initialisation.h`) and provide a `juce::JUCEApplication` subclass `StageSignalApp` that:
- creates `AppController`, which owns the `AudioDeviceManager` and a `juce::AudioProcessorPlayer` wired to a `StageSignalProcessor`;
- uses `AudioProcessorPlayer` so `processBlock` is the same function as in CLAP (it forwards `AudioIODeviceCallbackContext::hostTimeNs` into `AudioPlayHead::PositionInfo`, verified in `juce_AudioProcessorPlayer.cpp`);
- never exposes an input-mute switch; "monitoring" is a processor-level fact (output cleared in standalone).
Fallback if the custom app proves costly: keep the stock holder and force `getMuteInputValue().setValue(false)` at startup and hide the Options button; documented as inferior because the persisted setting can be flipped by the stock settings panel.
Cost of the decision: we must implement app lifecycle, single-instance guard, command line (`--show <file>`, `--safe`), window state persistence, and quit handling ourselves.

### 6.2 Behaviour

- Audio in: one capture endpoint, mono or stereo route, 44.1 or 48 kHz (app-adapters 3).
- Monitoring off: processor output buffers are cleared in `processBlock`; the output device is opened with zero active output channels where the backend allows, else the silent buffer is sent. The app never sends audio to a speaker (spec 12, StageSignal is not controlling the PA). Verified in the existing skeleton: `PluginProcessor.cpp` already clears the buffer when `wrapperType == wrapperType_Standalone`; keep it, but key it on `HostMode` not `wrapperType`.
- Visuals: audience windows are separate top-level `Component`s on chosen displays; the operator window is the main window.
- Single instance: a named inter-process lock (`juce::InterProcessLock`) so a second launch focuses the first instead of opening the same audio device twice.
- Quit: `systemRequestedQuit` runs the shutdown sequence (section 7) then `quit()`. Unsaved show prompts Save / Discard / Cancel. A running recording or live broadcast requires an explicit confirm dialog naming it.

## 7. CLAP plugin mode

### 7.1 Facts about the wrapper (verified in clap-juce-extensions source)

- It calls `AudioProcessor::getStateInformation` / `setStateInformation` unmodified for the CLAP state extension.
- It supports **embedded** GUI only (`guiIsApiSupported` returns false when `isFloating`; true for win32, cocoa, x11 only). No Wayland, no floating-window mode. The CLAP `gui.h` itself says embedding is not supported on Wayland.
- MIDI out is translated only for 2 and 3 byte messages (no SysEx). MIDI in is note-port based.
- It does not populate `hostTimeNs`; it provides tempo, time signature, beat and time position from the CLAP transport (`clap-juce-wrapper.cpp`, lines near 826 to 901).
- All plugins report `wrapperType_Undefined`; use `clap_juce_extensions::clap_properties::is_clap`.
- Unofficial, not supported by the JUCE team (README).

### 7.2 Behaviour

- Bus layout: stereo in / stereo out, or mono/mono (existing `isBusesLayoutSupported`). Main bus passes through unmodified. Any gain, trim or compressor/limiter in StageSignal applies only to the **recording and broadcast tap**, never to the host's audio.
- Analysis input is the host main input. Tempo: `AudioPlayHead::getPosition()->getBpm()` becomes a fourth tempo source `Host` with higher priority than `Estimated` and lower than `Tap`/`Manual`, shown as "Host" in the BPM label. (Needs a one-line `ValueSource` addition in `base`; recorded as needs-from-others in the PR.)
- Visuals: the CLAP editor (embedded in the host's plugin window) is the operator UI at 800 x 600 default, resizable down to 640 x 480 with compact layout. Audience windows are separate top-level windows owned by `OutputManager`, opened by an explicit "Open audience window" action, never at plugin load.
- Host transport is never controlled. Start/stop of the host does not start recording or broadcast.
- Processing while offline-rendering or freeze: when the host renders offline (`isNonRealtime()` true) StageSignal runs analysis only, forces audience windows idle, refuses record/broadcast/lighting arm (they are wall-clock features).
- The editor may be closed while the plugin keeps running (hosts destroy the GUI but keep the plugin). Anything that must continue (analysis, lighting, MIDI, recording, show timers) lives in `AppController`, not in the editor.

### 7.3 State save and restore (`getStateInformation`)

Blob = 4-byte magic `SSST`, u16 format version, u32 payload length, UTF-8 JSON payload (nlohmann/json via `persist`), SHA-256 of payload (base `sha256.hpp`). Size cap 2 MiB (hosts store this per instance); larger shows store a **reference** (show UUID and a hash) and require the user to re-open the `.stagesignal` file, with a clear message when the file is missing.
Contents: show document (without media bytes), mapping table, UI layout preferences, safety toggles (disable strobing), per-output role intent as non-secret matching hints.
Never contained: armed/recording/live flags, stream endpoints' secret references resolved, absolute paths (relative or hashed only), device-only identifiers beyond the matching hints used by exports.
`setStateInformation`: parse on the calling thread into a staged document, validate completely (size, schema, magic, hash), then hand to the control thread to swap at a safe boundary. Invalid blob: keep the current state, raise a sanitised error, never throw across the boundary. It must be callable repeatedly (hosts do) and from non-message threads: it posts to the message thread via `MessageManager::callAsync` for anything that touches UI.
Multiple instances: two `StageSignal` plugin instances would fight for audience windows and lighting sockets. The second instance runs in "Secondary" mode: analysis and UI only, all outputs disabled, banner explains why. (Process-wide singleton guard, `std::weak_ptr` in a function-local static.)

### 7.4 Plugin-mode limits (honest)

- No capture-device control; the host owns the audio device and its latency. A/V recording sync in plugin mode relies on the host's sample counter and is calibrated, not guaranteed (app-adapters 3.3).
- Operator UI in a host window can lose keyboard focus to the host. Host shortcut collisions are expected (Space, Ctrl+S, Ctrl+K may be consumed by the host). The editor sets `EDITOR_WANTS_KEYBOARD_FOCUS` (already in CMake), but the Command Palette is also reachable by a visible button and the bottom-strip emergency controls always work with the mouse.
- Hosts differ in how they treat editor resize and scaling; see `research-deferred.md` section 8.

## 8. Crash recovery

- `running.lock` (JSON: pid, start time, build id, mode standalone/CLAP) is written at step 14 and removed during clean shutdown stage 8. A stale lock (pid not alive or older boot id) means the last run was unclean.
- Recovery mode: scene = idle screen, visual blackout OFF but audience outputs show the Idle screen only, DMX disarmed, MIDI clock off, recording/broadcast off, camera unpublished. A banner lists what was restored and what was not.
- The operator picks "Restore autosave from hh:mm" or "Open last saved" or "Start empty". Autosave files are the retained prior three valid saves plus the 30 s autosave (spec 17). Nothing is auto-restored without a click.
- In CLAP mode a host crash takes the whole process; the same lock logic applies on the next load. If the host restores plugin state itself, `setStateInformation` still starts disarmed (APP-SH-09).
- Hard faults inside the process (null deref, etc.) are not caught or recovered in-process; no minidump is written by default (spec 16 forbids crash memory dumps by default). A crash report offered at next start is a sanitised text summary only.

## 9. Shutdown order

Stage numbers are the spec section 8 sequence; timeouts are proposed and tunable.

| # | Stage | Action | Timeout |
|---|---|---|---|
| 1 | Disarm outward actions | `DisarmLighting`, MIDI clock off, camera unpublish, no new record/broadcast starts | 200 ms |
| 2 | Cancel broadcast, finalise writers | `StopBroadcast` (cancel retry), flush and finalise recorder segments, close ffmpeg stdin and wait | 5 s, then kill child, mark segment Recoverable |
| 3 | Quiesce MIDI and lighting | send qualified safe/blackout frames once, MIDI note cleanup for notes StageSignal owns, join LIGHT and MIDI-OUT | 1 s |
| 4 | Stop capture and camera | remove audio callback (`removeAudioCallback`), close device, close camera | 1 s |
| 5 | Drain bounded tasks | analysis, media pool, IO thread: signal, drain, join | 2 s each |
| 6 | Destroy worker resources | decoders, ffmpeg supervisors, sockets, secret handles zeroised | 1 s |
| 7 | Destroy renderer | release frame pool, GL contexts if any | 500 ms |
| 8 | Close UI, remove lock | close audience windows then operator window, delete `running.lock` | 500 ms |

A stage that times out records `shutdown.<stage>.timeout` in the journal and the sequence continues; the process still exits. In plugin mode the same sequence runs when the last plugin instance is destroyed, except stage 4 only detaches (the host owns the device) and the GUI is just the embedded editor.

## 10. Offline-first rules

1. No code path opens a socket except: the lighting adapter to the operator-configured destination after Arm; the broadcast ffmpeg child after an explicit Start; an explicit user-chosen "Check for updates" if the owner later adds it (not in v0.2).
2. `JUCE_USE_CURL=0` and `JUCE_WEB_BROWSER=0` stay set (already in `app/CMakeLists.txt`) so no URL streams or embedded browser are linked.
3. Help is offline HTML/Markdown bundled in the binary (BinaryData), no web fonts, no remote images.
4. Default sockets at idle: none (REQ-X04). The T-SEC "default sockets" test enumerates the process's sockets (Linux `/proc/<pid>/net`, `ss -p`; Windows `netstat -ano`; macOS `lsof -i`) after 60 s idle and must see zero listening and zero established.
5. Art-Net node discovery (ArtPoll broadcast) is off by default and needs an explicit button.

## 11. JUCE classes and APIs used

`juce::JUCEApplication`, `juce::AudioDeviceManager`, `juce::AudioProcessorPlayer`, `juce::AudioProcessor` (`processBlock`, `prepareToPlay`, `getPlayHead`, `isNonRealtime`, `getStateInformation`/`setStateInformation`), `juce::Thread` (`Priority`), `juce::ThreadPool`, `juce::WaitableEvent`, `juce::InterProcessLock`, `juce::MessageManager::callAsync`, `juce::Timer`, `juce::File::getSpecialLocation(userApplicationDataDirectory)`, `juce::SystemStats`, `clap_juce_extensions::clap_properties`.
Build flags to add or keep: `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`, keep `JUCE_USE_CURL=0`, `JUCE_WEB_BROWSER=0`, `JUCE_USE_CAMERA=1`. Note `app/CMakeLists.txt` currently links no `juce_video` target although `JUCE_USE_CAMERA=1` is defined; `CameraDevice` lives in `juce_video`, so the integrator must add `juce::juce_video` (needs-from-others).

## 12. Edge and failure cases

| Case | Behaviour |
|---|---|
| Audio device lost while running | state "No audio" within 1 s (watchdog 500 ms of no callbacks plus device-list listener); analysis idles; show continues per D09 policy; no auto-switch to another device |
| Audio callback slower than period | xrun counter / gap heuristic marks discontinuity; perf core counts it; never retried inside the callback |
| UI thread blocked 2 s | CONTROL, RENDER, LIGHT continue; audience windows may freeze since painting is on the message thread; heartbeat shows "UI stalled" in Diagnostics when it recovers; blackout still works through the latch for lighting and for the render frames already handed to windows |
| Control thread stalls | latch still drives blackout; watchdog on the UI thread shows a red banner; no automatic restart of the thread (state ownership) |
| Second launch | focuses first instance |
| `setStateInformation` with huge/hostile blob | rejected before allocation (length field checked against 2 MiB cap) |
| Host calls `prepareToPlay` with unsupported rate | analyser runs at the host rate if between 22.05 and 192 kHz; below/above: analysis Unavailable with message, passthrough continues |
| Editor opened twice by host | one `AppController`, editors are views; second editor shares state |
| Low disk at start | warn only; recording refuses to start (REQ-R06) |

## 13. Tests and headless verification

Categories: U = core unit tests in CI without JUCE (existing); H = app tests run headless with Xvfb on Linux in CI; M = manual or hardware, recorded by hand.

| Test | Layer | Verifies |
|---|---|---|
| `shell: startup order` | H | controller with fake adapters; log shows steps 1 to 14 in order; no transmit calls recorded by the null sinks |
| `shell: shutdown order and timeouts` | H | inject a stuck writer; sequence completes, stage 2 timeout logged, child killed |
| `shell: callback no alloc` | H | link-time `operator new` hook counts allocations during 10 000 synthetic callbacks: zero (REQ-P05, T-RT) |
| `shell: callback worst time` | H | synthetic callbacks at 256/48k; 99.9th percentile under 5% of period on the CI machine (informational on shared CI) |
| `shell: safety latch beats stalled control` | H | stall CONTROL for 2 s; assert RENDER output black within one tick (REQ-D08) |
| `shell: state blob roundtrip` | U/H | save, load, compare documents; magic, hash, version; oversize and corrupt blobs rejected |
| `shell: state blob never arms` | H | set state with armed flags crafted in JSON: ignored, controller disarmed |
| `shell: recovery marker` | H | stale `running.lock` => Recovery mode, nothing armed, restore needs click |
| `shell: no sockets at idle` | H | after 10 s idle, process has no listening or connected sockets (Linux `/proc/net/*`) |
| `shell: standalone input not muted` | H | feed a sine through `AudioProcessorPlayer` with a fake device; analyser sees it; output buffer is zero |
| `shell: clap passthrough` | H | load built `.clap` with `clap-validator` or a minimal CLAP host test, check output equals input bit-exact |
| `shell: multiple instances` | H | second instance enters Secondary mode |

How Xvfb is used: CI already builds the standalone and runs it for 8 s under `xvfb-run -a` (`.github/workflows/ci.yml`, step "Headless smoke"). H tests extend this with a hidden `--selftest <name>` command line flag in the standalone binary (Linux/macOS/Windows) that constructs the controller with fake audio/MIDI/display adapters (no ALSA needed), runs the named scenario, writes a JSON result and exits with a code. Xvfb supplies a display so JUCE can create windows; no GPU, no sound card. `xvfb-run -a -s "-screen 0 1280x720x24 -screen 1 1024x768x24"` is the form for multi-screen scenarios (Xvfb supports multiple screens; in the Xinerama/Randr layouts that JUCE enumerates, the exact count seen by `Desktop::getDisplays` must be asserted by the test itself, not assumed; see app-adapters 4.4).
CLAP validation: the free-audio `clap-validator` project can load and exercise a `.clap` headless; I have not run it here and pin no version; the integrator picks the release.

## 14. Honest limits

- No timing, memory or CPU number in this document has been measured; they are budgets.
- No CLAP host was exercised; host behaviour is in `research-deferred.md` and is documentation-level evidence.
- The custom standalone decision (D-SHELL-01) trades JUCE-maintained plumbing for control; if JUCE changes the stock holder, we do not inherit fixes.
- Plugin mode is not a good place for audience windows on Linux/Wayland sessions (the wrapper only supports X11 embedding); on Wayland the editor is unavailable in hosts that need `x11` embedding unless they run under XWayland.
- The crash marker proves "ended unclean", not why.
- Thread priorities map differently per OS; `Priority::high` is a request.

## Implementation status (shell-integration, 21 Oct 2026)

Code: `App/AppController.*`, `App/AppShell.cpp` (shell services), `App/ShellImpl.h`, `App/ShellState.*` (prefs, lock text, state blob), `App/StandaloneApp.cpp`, `App/ControllerHost.*`, `Plugin/PluginProcessor.cpp`. Self-tests in `Tests/ShellIntegrationTests.cpp` (run headless, fake adapters, no hardware).

| Item | State | Verified |
|---|---|---|
| Startup steps 2, 3, 4, 14 | Done. Settings (`settings.json` via core/persist, `prefs.txt` for UI prefs); unreadable settings keep a `.bad` copy and fall back to defaults. `running.lock` JSON written at step 14, removed in stage 8. | `shell: recovery marker`, `shell: show persistence` |
| Recovery mode | Done: stale lock (pid dead) or an autosave newer than the file enters it. Sample show, paused, nothing armed; banner with Restore autosave / Open last saved / Start empty; nothing is restored without a click. | `shell: recovery marker` (pid-dead path). Pid check on Windows is untested code |
| Persist-backed show, 30 s autosave, Save | Load of the last show (or `--show`), save/save-as, dirty flag by document hash, autosave through `persist::ShowSession` (recovery file). | Save, reload, `--safe` tested. The 30 s autosave timer itself is core-tested (`AutosaveController`), not waited for in the app test |
| `--show`, `--safe`, single-instance lock, quit confirm | Done in `StandaloneApp.cpp` (`InterProcessLock` plus JUCE's own pipe hand-off; async alert for unsaved work / recording / broadcast). | Not run (needs a window and a human) |
| Plugin state blob | Done: `SSST`, version, length, JSON, SHA-256, 2 MiB cap with reference form; set stages, validates, swaps on CONTROL, always disarms; unknown JSON keys (armed, keys) ignored. | `shell: state blob roundtrip`, `shell: state blob never arms`. No CLAP host was used |
| Secondary instance | Done: second controller in the same process gets fake adapters, no files, every output action refused, banner. | `shell: secondary mode`. Not exercised in a real host |
| Router, palette keymap, perf cores | Instantiated. Perf adaptation runs at 1 Hz from render deadline misses and CPU and lowers the render rate. Keymap overrides load from prefs but the Live-tab shortcut table is still the static default. Router receives the topology from the message thread; audience windows still use `OutputManager` directly. | Perf ladder not driven to a shed step in a test |
| Analysis onset drain | `AnalysisWorker::onsetsSince(cursor)`; the show tick no longer misses onsets. | Counter logic only |
| Shutdown with a stuck writer | Stage 2 runs on a helper with a real timeout; stage 6 releases it; joined in stage 8. | `shell: shutdown stuck writer` |
| `shell: callback no alloc` | `Hooks/AllocHook.cpp` (standalone executable only) counts `operator new` on the calling thread: 0 in 10 000 `AudioIngress::pushBlock` calls. The full device callback and `processBlock` are not covered. | Linux x86-64 |
| Engines fed | MIDI engine actions enter the inbox with source Midi; show load configures the MIDI mapping table and the lighting patch/destination; an armed show drives the (fake) UDP sink; disarm silences it. | `shell: midi feeds actions`, `shell: lighting driven when armed` |
| Commands | Handlers for 1 to 6 (audio device, lighting interface/destination, MIDI ports), 20 to 29, 22 to 24 (report folder export, support bundle zip, doctor no-op) and 40 to 49. Import media, remove media and publish text return a stated "needs the media worker". | Handlers run via the IO thread; real devices untested |
| Musical features | `PhraseTracker` (per bar), `KeyColorMapper` (25 Hz), `BeatLooks` on a shell-side `BeatGrid` mirror feed `UiSnapshot::music`, an optional render tint (pref `ui.keytint`) and an opt-in lighting "beat pulse" (pref `lighting.beatlook`, cue layer, strobe lock respected). Live tab shows phrase text and a key swatch. | `shell: phrase wiring`, `key colour wiring`, `beat looks wiring`. Phrase-quantised auto scene changes are NOT done: the show engine owns auto timing and has no phrase hook (needs a `show` change); auto changes still quantise to the bar |

Not done: window position persistence per display identity; wizard completion persistence; display role storage is a pref key reserved (`display.*`), not yet read by the audience window code; `shell: callback worst time`, `safety latch beats stalled control`, `no sockets at idle`, `clap passthrough` tests.

## Sources

- [S1] JUCE `8.0.16-security` source tree, files named inline (tag sha `ea77444750de45e036e0279bb0291144bf8e3688`), read 2026-10-20.
- [S2] clap-juce-extensions: https://github.com/free-audio/clap-juce-extensions (README, `src/wrapper/clap-juce-wrapper.cpp`), read 2026-10-20 from a shallow clone at `main`.
- [S3] CLAP 1.2.10 headers, `include/clap/ext/gui.h`: https://github.com/free-audio/clap, read 2026-10-20.
- [S4] StageSignal spec v0.1 sections 8, 10, 16, 17 and addendum v0.2 (in repo).
- [S5] `.github/workflows/ci.yml` (Xvfb smoke step), `app/CMakeLists.txt`, `app/Source/Plugin/PluginProcessor.cpp` (in repo).
