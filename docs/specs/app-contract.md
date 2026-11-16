# app-contract: the interfaces and file ownership that let the JUCE app layer be built in parallel

Status: contract commit, 20 Oct 2026. Layer: `app/Source`. Baseline: `app-shell.md`, `app-adapters.md`, `app-ui.md`, `docs/ARCHITECTURE.md`, `docs/WORKER_RULES.md`. Everything marked **STUB** below compiles and runs but does not do the real job yet. Nothing here was run on real hardware; the proof is a headless run with fake adapters.

## 1. What exists after this commit

| Piece | Where | State |
|---|---|---|
| Adapter interfaces + null/fake implementations | `Engine/I*.h`, `Engine/Fakes.h`, `Engine/AdapterSet.*` | interfaces final for v1, fakes work |
| Real desktop adapters | `Engine/DesktopAdapters.cpp` | **STUB**: returns the fake set with "No audio device" |
| `AppController` (graph, 6 worker threads, start order 1 to 14, stop stages 1 to 8) | `App/AppController.*` | works headless; some start steps are marked Stubbed (section 6) |
| `UiModel` facade, `UiSnapshot`, `submitAction`, `postCommand`, listener + 30 Hz poller | `App/UiModel.*`, `App/UiModelPoller.h` | works |
| `HostMode` (Standalone vs CLAP) | `App/HostMode.h`, used by `Plugin/PluginProcessor.cpp` | works |
| Custom standalone app (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`) | `App/StandaloneApp.cpp` | compiles, links and starts a window under Xvfb |
| UI skeleton: `Theme`, `TabHost`, 7 tab panels + `StatusBar` + `PerformanceStrip`, `OperatorRoot`, `OperatorWindow` | `UI/` | panels are EMPTY by design |
| Headless self-test `--selftest [smoke\|all\|<name>]` | `Tests/` | `smoke` passes |
| Media / record / broadcast worker | `App/MediaWorker.*` | **STUB**: counts blocks, refuses record/broadcast with a clear message |

### Custom standalone decision (D-SHELL-01), verified
With the pinned JUCE (`8.0.16-security`) the stock wrapper source `juce_audio_plugin_client_Standalone.cpp` skips its own application class when `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1` and instead references an external `juce::JUCEApplicationBase* juce_CreateApplication()`. `START_JUCE_APPLICATION(StageSignalApp)` in `App/StandaloneApp.cpp` defines it (the macro is only allowed because `JucePlugin_Build_Standalone` is set for the shared-code target). Without the application class the link fails with `undefined reference to juce_CreateApplication()`, which we observed before adding it. The same shared code is linked into the CLAP target; there the function is simply unused. So the documented fallback (stock holder with forced-unmuted input) was **not** needed. The processor is not instantiated by `StageSignalApp` yet: the real device path (`AudioDeviceManager` + `AudioProcessorPlayer` calling `StageSignalProcessor::processBlock`) belongs to the `IAudioSource` implementation (engine-adapters-desktop).

## 2. Interface list

All in namespace `ss::app`, JUCE-free headers (std + `ss::` core only), C++17. Each adapter derives from `IAdapter` (`start`, `stop`, `health`) unless noted. Where a core interface already exists the app interface extends it instead of inventing a second one.

| Interface | File | Extends / reuses | Thread that calls it | Fake / null |
|---|---|---|---|---|
| `IAudioSource` + `IAudioSink` | `Engine/IAudioSource.h` | sink implemented by `AudioIngress` | AUDIO calls `pushBlock` | `SineAudioSource`, `NoAudioSource` |
| `ITimestampMapper` | `Engine/ITimestampMapper.h` | | AUDIO | `UnknownTimestampMapper` (never valid), `FixedLatencyTimestampMapper` |
| `IMidiPortProvider`, `IMidiInputSink`, `IMidiOutPort` | `Engine/IMidiPortProvider.h` | | OS MIDI thread (in), MIDI-OUT (out) | `FakeMidiPortProvider` |
| `IPacketSink` | `Engine/IPacketSink.h` | `ss::lighting::IDatagramSink` | LIGHT | `NullPacketSink`, `RecordingPacketSink` |
| `ISerialDmx` | `Engine/ISerialDmx.h` | | LIGHT | `NullSerialDmx` |
| `IDisplayProvider` | `Engine/IDisplayProvider.h` | `ss::router::PhysicalDisplay` | UI | `FakeDisplayProvider` |
| `ICameraSource` + `ICameraSink` | `Engine/ICameraSource.h` | | any (memcpy only) | `NullCameraSource` |
| `IProcessSupervisor` | `Engine/IProcessSupervisor.h` | `ss::broadcast::IProcessFactory` | IO / MEDIA | `FakeProcessSupervisor` |
| `ISecretStoreAdapter` | `Engine/ISecretStoreAdapter.h` | `ss::security::ISecretStore` | any non-RT | `MemorySecretStoreAdapter` |
| `IDurableFileSystem` | `Engine/IDurableFileSystem.h` | `ss::persist::IFileSystem` + fsync + free space | IO | `StdDurableFileSystem` (no real fsync) |
| `IZipArchive` | `Engine/IZipArchive.h` | `ss::security::ArchiveEntryInfo` | IO | `UnsupportedZipArchive` |
| `IPerfCounters` | `Engine/IPerfCounters.h` | | CONTROL, 1 Hz | `FixedPerfCounters` |
| `IFileDialogs` | `Engine/IFileDialogs.h` | | message thread | `ScriptedFileDialogs` |
| `ILogSink` | `Engine/ILogSink.h` | | any non-RT | `NullLogSink` |
| Clock | `ss::IClock` from `core/base` | | any | `SystemClock`, `ss::FakeClock` |

`AdapterSet` (`Engine/AdapterSet.h`) bundles one `shared_ptr` per interface plus the clock; `makeFakeAdapterSet(withSine)` fills all of them; `makeDesktopAdapterSet()` is the single replacement point for real adapters.

Engine-side graph objects (not adapters): `AudioIngress` (pooled SPSC audio entry, the only place audio enters), `ActionInbox` (multi-producer actions with a safety-critical priority lane), `BlackoutLatch`, `Heartbeats`, `LatestShared<T>`, `FrameExchange` (`App/Mailboxes.h`), `AnalysisWorker`, `RenderHost`, `LightingScheduler`, `MidiScheduler`, `MediaWorker`, `DiagBus`, `ControllerHost` (plugin-mode process-wide holder).

UI-facing facade (`App/UiModel.h`): `UiSnapshot` (read-only, copy of everything a tab shows), `UiModel::submitAction(Action)` (any thread, returns a ticket with `correlationId`), `UiModel::postCommand(AppCommand)` (setup/device/file requests handled on the IO thread), `ActionOutcome` / `CommandOutcome` delivered through `UiModelListener` on the message thread, `UiModel::latestFrame(OutputId)` for previews, `UiModelPoller` (30 Hz `juce::Timer`).

## 3. Thread table as built

| Thread | Body | Cadence | Notes |
|---|---|---|---|
| CONTROL | `AppController::controlBody` | wake 5 ms; show tick 25 Hz; UI snapshot ~30 Hz; perf sample 1 Hz | the single writer of show state; drains `ActionInbox` (priority lane first) |
| ANALYSIS | `AnalysisWorker::step` | polls every 2 ms when idle | analyser on raw capture, then dynamics; processed blocks go to `IProcessedAudioConsumer`s |
| RENDER | `RenderHost::renderFrame` | 25 FPS from elapsed time | scenes + effects (render core), compositor (compose core), one `FrameExchange` per output |
| LIGHT | `LightingScheduler::tick` | 25 Hz (1 to 40) | **disarmed**: handler gets a `NullPacketSink` |
| MIDI-OUT | `MidiScheduler::tick` | 2 ms | **output disabled**: handler gets a null port |
| IO | `AppController::ioBody` | 20 ms | drains `DiagBus` into `diag::EventJournal` (sole writer) and `ILogSink`, runs `AppCommand` handlers |

AUDIO, MIDI-IN, CAMERA and MEDIA decode threads belong to adapters. Audio enters only through `AudioIngress::pushBlock`.

## 4. Ownership map

One owner per file. A worker edits only its own files plus new files inside its own directories; CMake globs `app/Source/**/*.cpp` recursively, so adding a file needs no CMake edit. A change to a file owned by someone else is requested in the PR body, never made.

| Worker | Owns (files and directories) | First tasks (from the specs) |
|---|---|---|
| **shell-integration** | `App/AppController.*`, `App/UiModel.*`, `App/UiModelPoller.h`, `App/Mailboxes.h`, `App/ActionInbox.h`, `App/AudioIngress.*`, `App/AnalysisWorker.*`, `App/RenderHost.*`, `App/DiagBus.h`, `App/HostMode.h`, `App/SampleShow.*`, `App/ControllerHost.*`, `App/StandaloneApp.cpp`, `Plugin/**`, `Engine/AdapterCommon.h`, `Engine/AdapterSet.*`, `Engine/Fakes.h`, `UI/Theme.h`, `UI/PanelBase.h`, `UI/TabHost.*`, `UI/OperatorRoot.*`, `UI/OperatorWindow.*`, `Tests/SelfTest.*`, `Tests/ShellSelfTests.cpp`, `app/CMakeLists.txt`, `docs/specs/app-shell.md`, this file | settings + `running.lock` + Recovery mode (steps 2, 3, 14), persist-backed show load and 30 s autosave, state blob (`getStateInformation`), single-instance lock, `--show`/`--safe`, quit confirm, wire router/palette/perf cores, secondary plugin instance mode, shutdown timeout tests, callback no-alloc test |
| **engine-adapters-desktop** | `Engine/IAudioSource.h`, `ITimestampMapper.h`, `IMidiPortProvider.h`, `IPacketSink.h`, `ISerialDmx.h`, `IDisplayProvider.h`, `ISecretStoreAdapter.h`, `IDurableFileSystem.h`, `IZipArchive.h`, `IPerfCounters.h`, `IFileDialogs.h`, `ILogSink.h`, `Engine/DesktopAdapters.*`, `Engine/Desktop/**`, `App/LightingScheduler.*`, `App/MidiScheduler.*`, `App/Views/LightingView.h`, `App/Views/MidiView.h`, `Tests/AdaptersDesktopSelfTests.cpp` | JUCE audio source (device manager + `AudioProcessorPlayer`, loss/return), timestamp mapper, MIDI provider, UDP sender + Art-Net/sACN tick handler driving `ss::lighting::LightingEngine`, MIDI tick handler driving `ss::midi::MidiEngine`, display provider with identity, keychain, fsync filesystem, safe ZIP, perf counters, async dialogs, ring logger |
| **engine-media-record** | `Engine/ICameraSource.h`, `Engine/IProcessSupervisor.h`, `Engine/Media/**`, `App/MediaWorker.*`, `App/Media/**`, `App/Views/MediaView.h`, `RecordView.h`, `BroadcastView.h`, `Tests/MediaRecordSelfTests.cpp` | media supervisor + decoders, camera, text raster to `PixelBuffer`, ffmpeg supervisor, `record::RecordSession`, `broadcast::BroadcastSession`, `IRenderLayerSource` layers, processed-audio tap, fill the three views |
| **ui-live** | `UI/LivePanel.*`, `UI/StatusBar.*`, `UI/PerformanceStrip.*`, `UI/Live/**`, `UI/Status/**`, `UI/Strip/**`, `UI/Audience/**` (audience windows, `OutputManager`), `Tests/UiLiveSelfTests.cpp` | Live tab (5.1), status bar chips (4.1), bottom strip with blackout latch UI (4.2), audience windows fed by `UiModel::latestFrame`, keyboard dispatch for the Live tab |
| **ui-setup** | `UI/SetupPanel.*`, `UI/Setup/**`, `Tests/UiSetupSelfTests.cpp` | Setup tab (5.2), wizard (9), device pickers through `AppCommand`, appearance and high-contrast theme switch (through new files, not by editing `Theme.h`) |
| **ui-other** | `UI/MediaTextPanel.*`, `UI/MidiLightingPanel.*`, `UI/RecordBroadcastPanel.*`, `UI/DiagnosticsPanel.*`, `UI/HelpPanel.*`, `UI/MediaText/**`, `UI/MidiLighting/**`, `UI/RecordBroadcast/**`, `UI/Diagnostics/**`, `UI/Help/**`, `UI/Overlays/**` (Show Doctor, Command Palette, Undo/A-B, report export), `Tests/UiOtherSelfTests.cpp` | tabs 5.3 to 5.7, Doctor (10), palette (12), undo (13), report (14) |

Shared file with a fixed protocol: `Tests/UiSelfTests.cpp` (three UI workers, one line each, see the comment in the file).

`UiSnapshot` fields: the top-level struct is owned by shell-integration. A worker that needs new data adds it inside its own `Views/*View.h` (and the producer's `fillView`). A worker that needs a new top-level field or a new `AppCommandKind` asks in its PR body. `AppCommandKind` numbers are append-only and pre-grouped: 1 to 9 and 30 to 39 adapters, 10 to 19 media-record, 20 to 29 shell/ui-other.

## 5. Rules (apply to every worker)

1. **Realtime**: `AudioIngress::pushBlock` and anything it calls never allocate, lock, log or touch UI. `RenderHost::renderFrame` does not allocate in steady state (it allocates only on scene change and output reconfiguration).
2. **One writer**: only the CONTROL thread calls `ShowEngine`. UI and everything else submit `ss::Action` through `UiModel::submitAction`. Actions the show engine does not own are routed with `AppController::registerActionHandler` (CONTROL thread, must be quick; post slow work as an `AppCommand` or to your own thread).
3. **Disarmed by default**: nothing transmits, records, broadcasts or publishes camera at startup, after restore, or after recovery. `ArmLighting` is accepted only from Ui/Keyboard/Midi sources. `DisarmLighting` and the blackout actions are safety-critical: they set `BlackoutLatch` flags at submit time and are read by RENDER and LIGHT directly.
4. **UI never blocks and never touches adapters or core**. Read `UiSnapshot`, write through `submitAction` / `postCommand`. Panels do not create timers for engine state; `UiModelPoller` ticks `UiModelListener`s.
5. **Adapters** hold no show logic, never call UI, are idempotent in `start`/`stop`, report through `health()` with sanitised text, and have a fake. Real adapters live in `Engine/Desktop/**` (or `Engine/Media/**`) and are returned from `makeDesktopAdapterSet()`.
6. **Interfaces are additive**: adding a method with a default or a new interface is fine; changing or removing a signature needs the orchestrator.
7. **Style**: C++17, namespace `ss::app`, compile clean with `-Wall -Wextra` (JUCE's recommended warnings are on; the `-Wfloat-equal` hits that remain come from `core/model/show_model.hpp`, not from app code). Text shown to operators follows `app-ui.md` 3.5 (no em dashes, no AI-sounding copy).
8. **Tests**: add H-tests as `--selftest <name>` registrations in your own `Tests/*SelfTests.cpp`; they use fake adapters, no sleeps longer than needed, no hardware. Run under `xvfb-run -a`.
9. Commit trailers and PR body ending as in `docs/WORKER_RULES.md`.

## 6. Honest status: stubbed or deviating

Updated by shell-integration, 21 Oct 2026. Detail and test names: `app-shell.md` "Implementation status".

- **Start steps** all run for real now except step 7 (UI windows are created by the host application, logged Stubbed) and step 12 (media, owned by the media worker, logged Stubbed). Settings, `running.lock`, Recovery mode, persist-backed show load, 30 s autosave, `--show` and `--safe`, single-instance lock and quit confirmation exist. With an empty `AppConfig::dataDir` (self-tests, CI) nothing is read or written.
- Router, palette keymap and perf adaptation are instantiated; the keymap used by the Live-tab shortcuts is still the static default and audience windows do not read the router yet.
- `Engine/DesktopAdapters.cpp` returns the real desktop adapters (see `app-adapters.md`); the controller now feeds them (MIDI actions, mapping table, lighting patch and destination). Real hardware is untested.
- `MediaWorker` (media worker's code) is wired: native process supervisor, frame provider, config commands, markers, import/remove/publish text, two-step broadcast start.
- Analysis still wakes by a 2 ms poll. The onset drain exists (`onsetsSince`), so a slow consumer no longer misses onsets.
- Every output has its own compositor; `FrameExchange` is a frame pool, not a `LatestMailbox`.
- Shutdown stage 2 has a real timeout with a helper thread; other stages are still timed after the fact.
- Plugin mode: versioned state blob and Secondary-instance mode are implemented and self-tested, but no CLAP host was used. `juce_video` is still not linked (camera).
- Allocation: `shell: callback no alloc` proves the `AudioIngress::pushBlock` entry only (standalone build, Linux). Callback time and behaviour under load are not measured.
- Phrase-quantised auto scene changes are not possible without a hook in `core/show` (documented in app-shell); phrase position, key colour and beat looks are published and shown.
- Not done: window position persistence, wizard completion persistence, display roles read-back, `shell: callback worst time`, `safety latch beats stalled control`, `no sockets at idle`, `clap passthrough`.

## 7. How this was proven

```
cmake -S . -B build-app -G Ninja -DSS_BUILD_APP=ON -DSS_BUILD_TESTS=OFF -DSS_JUCE_DIR=<juce> -DSS_CLAP_EXT_DIR=<clap-juce-extensions>
cmake --build build-app -j2
xvfb-run -a build-app/app/StageSignal_artefacts/Release/Standalone/StageSignal --selftest        # smoke, exit 0
xvfb-run -a build-app/app/StageSignal_artefacts/Release/Standalone/StageSignal --selftest all
```
`smoke` starts `AppController` with fake adapters, feeds a 440 Hz sine for 2 s, and checks: 14 startup steps in order, none failed; a `FeatureSnapshot` shows the sine; a non-black 800x600 frame exists for output 1; the `UiSnapshot` is populated, no heartbeat is stale; `NextScene` round-trips and is accepted; `ArmLighting` from a System source is refused; `VisualBlackout` sets the latch at submit time and yields a black frame, `RestoreBlackout` resumes; zero packets and zero MIDI were sent; 8 shutdown stages in order, none timed out, no thread still running. The same build also produces the CLAP module (`StageSignal.clap`); it was built, not loaded in a host.
