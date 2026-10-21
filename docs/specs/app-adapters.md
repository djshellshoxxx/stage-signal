# app-adapters: platform adapters over the portable core

Status: spec v0.2-draft, 20 Oct 2026. Layer: `app/Source/Adapters`. Baseline: spec v0.1 sections 7 to 18, addendum v0.2, `docs/ARCHITECTURE.md`, `core/base` interfaces. Nothing here is implemented. Facts marked [V] were checked against the JUCE `8.0.16-security` source, clap-juce-extensions, the Art-Net 4 PDF or a vendor page (sources at the end); facts marked [U] are unverified and must be confirmed by the first spike.

## 1. Scope and requirement ids

An adapter converts one platform facility into a core interface (`ss::I*` in `core/base`) and runs on the thread named in `app-shell.md` section 3. Adapters hold no show logic. Each has a fake in `core/testing` or `app/Source/Testing` so every H test runs without hardware.

| ID | Statement | Spec REQ |
|---|---|---|
| APP-AD-01 | Audio adapter delivers interleaved float blocks to a lock-free queue with exact sample index; no allocation, lock or log in the callback | REQ-A01, REQ-P05, REQ-X06 |
| APP-AD-02 | Device identity, loss and return are detected; loss never crashes and the show keeps running unanalysed | REQ-A02, REQ-D09 |
| APP-AD-03 | Audio timestamps carry a validity flag; unknown host time is never invented | REQ-R03 |
| APP-AD-04 | Display adapter tracks displays by stable identity where the OS offers one and refuses to guess when two are identical | REQ-D01, REQ-D06 |
| APP-AD-05 | Audience windows are borderless, on the assigned display, never take focus, and are recreated after display change | REQ-D05, REQ-D06 |
| APP-AD-06 | MIDI in/out via JUCE, hot-plug aware, input timestamped, output through a bounded queue; no SysEx in by default | REQ-M01 to REQ-M04 |
| APP-AD-07 | UDP sender transmits Art-Net/sACN only after Arm, to the chosen interface and destination, rate-capped, and stops at Disarm or fault | REQ-L02, REQ-L03, REQ-L05 |
| APP-AD-08 | USB-DMX serial adapter is optional, per-device-class opt-in, labelled by what was actually tested | REQ-L06 |
| APP-AD-09 | FFmpeg runs as a child process: argv-only, no shell, no secrets in argv or environment dumps, killable in 1 s | REQ-R01, REQ-B01, REQ-B05 |
| APP-AD-10 | Recording writers use atomic segment rules, report disk-full and short writes, and fsync at defined points | REQ-R04 to REQ-R06 |
| APP-AD-11 | Secrets live in the OS keychain (or an encrypted fallback), never in show files, logs or bundles | REQ-B03 |
| APP-AD-12 | ZIP read/write is path-traversal safe and bomb-limited | REQ-S13, REQ-S14 |
| APP-AD-13 | File dialogs are asynchronous and native | REQ-X01 |
| APP-AD-14 | Logging is bounded, rotated, sanitised and cannot block a realtime thread | REQ-B03, REQ-X05 |
| APP-AD-15 | Camera adapter reports availability honestly per platform; preview never implies publication | REQ-S09, REQ-S11 |

## 2. Adapter contract

Each adapter implements a core interface plus `start()`, `stop()` (idempotent, bounded time) and `health()` (state enum plus last sanitised `ss::Error`). State changes are posted as events to the controller; adapters never call into UI directly. Failures map to `ErrorCategory` from `core/base`. Every adapter accepts a clock (`IClock`) so tests use `FakeClock`.

## 3. Audio

### 3.1 Device management

- Use `juce::AudioDeviceManager` with the standard device types: WASAPI (shared), ASIO only if the user supplies an SDK build (licence), CoreAudio, ALSA and JACK. [V] A probe build (`juce_audio_devices`, console app) lists device types without hardware; its results are not recorded here because the probe was not run to completion.
- `AudioDeviceManager::initialise` then `addAudioCallback`. In standalone mode we use our own `StageSignalStandaloneApp` (`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`) so the stock standalone's input mute and feedback prompt are not in the way (app-shell 4).
- The callback only copies into a pre-allocated `SpscQueue<AudioBlock>`; analysis runs on the ANALYSIS thread.
- Identity stored in the show: type name + device name + input channel count. Changes of default device are not followed silently; the Setup tab shows "Saved device not found, using <x>" and Show Doctor warns.
- Loss: `AudioIODeviceCallback::audioDeviceError` and `audioDeviceStopped` post `AudioLost`; the adapter retries `restartLastAudioDevice` every 2 s, max 30 attempts then every 10 s, without blocking. Return posts `AudioRestored`; analysis state resets (new sample counter epoch), automation resumes per the show's rules (REQ-A02).
- Sample rates 44.1 and 48 kHz supported; others rejected in the UI. Buffer size shown, latency is "reported" not "measured".

### 3.2 TimestampMapper

Goal: map sample index to a monotonic nanosecond time for A/V alignment (REQ-R03).
- `AudioIODeviceCallbackContext` carries `hostTimeNs` (optional) only where the backend fills it; in JUCE 8.0.16 that is CoreAudio only [V]. On Windows and Linux the mapper uses `Time::getHighResolutionTicks` at callback entry plus the reported input latency, and smooths with a 1 s window linear fit (jitter rejection: discard points beyond 3 sigma).
- States: `Unconverged` (first 1 s), `Converged` (fit residual below 2 ms), `Degraded` (residual above 5 ms). Only `Converged` sets `kAudioTimestampValid`. Downstream (recorder) treats invalid timestamps as "A/V offset unknown, use the calibrated constant".
- Test: synthetic callbacks with injected jitter and drift; mapper converges, rejects outliers, flags degraded when jitter exceeds bounds.

### 3.3 Plugin mode audio

The host owns the device. `processBlock` copies into the same `SpscQueue` and stamps `firstSampleIndex` from `AudioPlayHead::PositionInfo::getTimeInSamples` when present, otherwise a running counter. Host time in ns is not available through clap-juce-extensions [V], so A/V sync for recording uses calibration (a loopback click test, stored per host) and is not guaranteed. Silence/non-zero detection treats a missing audio input bus as "no input" rather than an error. Offline render (host bounce) is detected via `isNonRealtime()` and disables recording/broadcast start.

## 4. Displays and audience windows

### 4.1 Enumeration

`juce::Desktop::getInstance().getDisplays()` provides `Display { userBounds, totalArea, logicalBounds, physicalBounds, scale, dpi, isMain, verticalFrequencyHz }` [V]. JUCE gives **no stable display identity** (no EDID, no UUID, no device path) [V], so the adapter adds a platform identity provider:

| OS | Identity source | Status |
|---|---|---|
| Windows | `QueryDisplayConfig` / `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_TARGET_DEVICE_NAME` (monitorDevicePath, EDID manufacturer + product code) | documented fields [V]; matching to a JUCE `Display` by bounds |
| Linux X11 | XRandR output + `EDID` output property | [V] property exists; Wayland: no EDID via X, falls back to name + size |
| macOS | `CGDisplayCreateUUIDFromDisplayID` | [U] the Apple doc page did not load; confirm in the spike |

Identity record: `{vendor, product, serialOrHash, connector, sizePx}`. Matching order: serial/EDID hash, then connector, then size+position. Two displays with identical records (same model, no serial) are "indistinguishable": their role stays unassigned until the operator confirms with the Identify flow (REQ-D01). The role map is stored by identity.

### 4.2 Audience windows

- `AudienceWindow : juce::Component` added with `addToDesktop (ComponentPeer::windowIsTemporary)`, no title bar, no drop shadow, `setAlwaysOnTop (true)` only while Live. Not `Desktop::setKioskModeComponent` as the default (on macOS it hides the Dock and menu bar for all displays; kiosk is offered as an option per output) [U].
- Placement: `setBounds (display.userBounds)` for the assigned display. No fullscreen exclusive mode.
- The window never accepts focus (`setWantsKeyboardFocus (false)`, `setInterceptsMouseClicks (false,false)`), hides the cursor, and forwards unhandled keys to the operator command manager.
- Rendering: the RENDER thread produces frames; the window is driven by `VBlankAttachment` and presents the latest frame from a `LatestMailbox` (no queue growth). CPU path first; an OpenGL path is behind a tier flag (JUCE `OpenGLContext`) and is not required for BASE.
- Rate: independent per window; dropped frames counted per output.

### 4.3 Hot-plug

`Desktop::Displays` changes call `DisplayWatcher` (a `Desktop::...` listener via `Displays::Display` change callback or a 1 s poll as fallback [U]). On change: re-identify, re-place windows for displays that return, close windows for displays that left (no automatic move onto the controls display, REQ-D06), post `DisplayChanged`, and Show Doctor warns. Resolution/scale change re-sizes without restarting the scene.

### 4.4 Headless behaviour and tests

Under Xvfb JUCE still needs an X root. Display enumeration under Xvfb returns the screens/outputs that XRandR/Xinerama exposes; the number must be asserted by the test (`xvfb-run -s "-screen 0 1280x720x24 -screen 1 1024x768x24"`), not assumed. For all logic tests the identity provider and `getDisplays` are replaced by `FakeDisplayProvider` (lists, hot-plug scripts, identical-monitor cases). Real placement checks (window bounds equal display bounds) run under Xvfb with the real provider as a smoke test. Real multi-monitor, HiDPI, VRR and macOS Spaces behaviour is manual.

## 5. MIDI

- `juce::MidiInput::getAvailableDevices()` / `MidiInput::openDevice` with `MidiInputCallback`; `MidiOutput::openDevice` for output [V]. Virtual ports: JUCE can create them on macOS/Linux (`createNewDevice`), not on Windows (needs a third-party loopback); the UI says so.
- Input callback runs on a JUCE/OS MIDI thread: it stamps `IClock::now()` and pushes the raw 1 to 3 byte message into an `SpscQueue`; no parsing there. SysEx input is dropped unless enabled (limit 1024 bytes); running status and active sensing are handled in the `midi` core.
- Output uses a bounded queue drained by the MIDI-OUT thread (`MidiOutput::sendMessageNow`); clock out is generated by that thread from the tempo with a drift-free scheduler (absolute next-tick time, not sleep-accumulation). Queue overflow drops oldest clock ticks first and counts it.
- Loss: devices are polled every 1 s (`getAvailableDevices` diff) for hot-plug; lost devices post `MidiDeviceLost`; feedback prevention is in `router`.
- Plugin mode (CLAP): output is limited to 2 or 3 byte messages through clap-juce-extensions [V]; SysEx out is disabled in plugin mode with a visible note.
- MIDI 2.0 / Windows MIDI Services: not in v1 (see research-deferred).

## 6. UDP (Art-Net and sACN)

### 6.1 Socket

`juce::DatagramSocket` limits [V]: no setter for multicast interface or TTL, so the adapter wraps native sockets where needed (`SO_BINDTODEVICE`/`IP_MULTICAST_IF`/`IP_MULTICAST_TTL` through `getRawSocketHandle()` and `setsockopt`) behind `UdpSender` (IF-09). The sender: binds to the chosen interface address, `SO_BROADCAST` only if broadcast is enabled, non-blocking, send on the LIGHT thread only. Nothing listens at idle (a receive port is opened only for ArtPoll discovery if the operator enables it).

### 6.2 Protocol rules

- Art-Net: UDP port 6454 (0x1936); ArtDmx: ID `Art-Net\0`, OpCode 0x5000 little-endian, ProtVer 14 (big-endian), Sequence, Physical, 15-bit Port-Address (SubUni low byte, Net high 7 bits), Length big-endian (even, 2 to 512), data; a DMX gateway should send no more than 44 Hz per universe [V, Art-Net 4 spec]. Sequence 0 means "disabled", so the counter runs 1 to 255.
- sACN (E1.31): UDP port 5568, universe 1 to 63999, multicast group 239.255.hi.lo [U: confirm in the ANSI E1.31 text before writing the test vectors], priority 0 to 200 (default 100), per-source CID (random UUID per run), sequence number per universe. Terminate with the stream-terminated option bit on Disarm (three packets) [U].
- Rate: default 25 Hz, max 40; fixed-timestep scheduler on the LIGHT thread; Disarm and Blackout flush the frame immediately.
- Safety sequencing: Armed is explicit, per session (not saved as armed), and cleared on any fault, interface change, destination change or reconnect (REQ-L05). Blackout writes zeros to every patched channel and holds them; strobe-lock zeroes strobe channels (REQ-S12).

### 6.3 Platform notes

- macOS: sending UDP on the local network from Sequoia onward triggers a Local Network privacy prompt for apps that talk to LAN addresses; the app needs `NSLocalNetworkUsageDescription` in `Info.plist` [U: version and exact behaviour to be confirmed on a Sequoia machine]. If the user denies it, sends fail silently on some versions; the adapter treats "no route to host" and send errors as `Fault` and Show Doctor explains the permission.
- Windows: the firewall prompt appears on first bind; sending only normally does not need an inbound rule. Interface choice uses `juce::IPAddress::getAllAddresses` with names from `GetAdaptersAddresses` (name lookup is native code).
- Linux: multiple NICs need explicit selection; broadcast to a subnet requires `SO_BROADCAST`.
- Hardware qualification of any node, fixture or adapter is out of scope here and listed in KNOWN_LIMITATIONS.

## 7. Serial USB-DMX (optional)

`juce::SerialPort` does not exist in JUCE [V: not in the module list]. Serial access is a thin native wrapper (`termios` on macOS/Linux, `CreateFile`/`DCB` on Windows) behind `IDmxSerial`. Supported classes are those the owner qualifies, shown as "Tested: <model>, <OS>" only after a physical test; every other FTDI-like device is "Not supported". Opening requires Arm; break/mark-after-break timing on non-realtime OS threads is jittery, which is why an Art-Net/sACN path is preferred. Device loss triggers Fault + Disarm. No claim of DMX512 timing compliance is made.

## 8. FFmpeg child process

Used by `record` (video+audio encode, if no internal writer applies) and `broadcast`.

### 8.1 Discovery and version

Order: user-set path in Setup, next to the app executable (`<app>/ffmpeg[.exe]`), then `PATH`. Run `ffmpeg -hide_banner -version` and `-encoders` with a 3 s timeout; parse the version string and required encoders (`libx264`, `aac` or `libopus`); results are cached for the session and shown in Doctor `D-REC-2`. Not found or no encoder: the feature is disabled with the cause, and the internal WAV recorder still works. StageSignal does not bundle FFmpeg unless the release engineer ships an LGPL build and its notices (licence and patents are a release decision, see research-deferred).

### 8.2 Launch

- `juce::ChildProcess::start (StringArray args, streamFlags)` with an argv vector, never a shell string [V]. `ChildProcess` has no stdin write API and merges stdout/stderr when asked [V], so the video pipe uses a native wrapper (`CreateProcess` + anonymous pipes on Windows, `posix_spawn` + `pipe` on macOS/Linux) in `FfmpegProcess`. Raw RGBA video goes to `pipe:0` with `-f rawvideo -pix_fmt rgba -s WxH -r N`; audio goes in a second pipe or a named pipe/FIFO [U: Windows needs a named pipe]; the simplest v1 is to mux audio from the WAV segment file rather than a second pipe (see 8.5).
- Progress and errors are read from a pipe fed by `-progress pipe:2 -nostats -loglevel warning`; lines are parsed on the IO thread with a 64 KiB per-line cap.
- Secrets: the stream key is **not** on the command line, since argv is visible to other local users in process listings. FFmpeg offers `-/opt file` to load an option value from a file, but tested locally it does not apply to muxer private options such as `-rtmp_playpath` (kill test, result recorded in research-deferred), so that route is closed. Chosen design: the key is passed through a one-shot local relay: FFmpeg is given a `tcp://127.0.0.1:<random>` or `pipe:` output, and a StageSignal-owned relay thread forwards to the RTMP(S) endpoint. [U] RTMP client code would be new work, so v1 ships the weaker, documented fallback: URL on argv with the key, a visible Setup warning ("the key is visible to other users on this computer while streaming"), plus redaction in all logs. This is an open decision for the owner (research-deferred section on WHIP lists a better path). 
- Environment: scrubbed to a whitelist (`PATH`, `HOME`/`USERPROFILE`, locale); no inherited proxy credentials.
- Protocol allowlist: `-protocol_whitelist file,pipe,tcp,tls,rtmp,rtmps,srt,rtp,udp,crypto` to stop FFmpeg following unexpected URL types from a playlist/user field.

### 8.3 Lifecycle

States mirror the core: `Preparing -> Running -> Stopping -> Exited(code)`. Normal stop: close stdin (EOF), wait 5 s for exit so the muxer finalises the file, then terminate, then kill at 6 s. Broadcast Stop must return control within 1 s: reconnect timers are cancelled first and the process is signalled; a late exit is reaped on the IO thread. Unexpected exit during recording starts a new segment (REQ-R04) and posts `RecordFailed`/`Recoverable`. Unexpected exit during broadcast follows the reconnect policy with backoff 1, 2, 4, 8, max 30 s. Zombie prevention: a reaper joins every child at shutdown stage 5 (app-shell 5).

### 8.4 Back-pressure

The video pipe write is on its own thread with a bounded frame queue (3 frames). If the pipe blocks, the oldest frame is dropped and counted; the render thread is never blocked (REQ-X01). Audio is never dropped silently: an audio overrun marks the segment.

### 8.5 Container and recovery facts

Local test with the installed ffmpeg and a kill -9 during write: Matroska with `-flush_packets 1` stayed playable up to the last flushed cluster; plain MP4 without fragmentation lost the whole file (no moov atom). Therefore the default record container is Matroska (or fragmented MP4 `-movflags frag_keyframe+empty_moov`), and the file extension makes the format explicit. The local ffmpeg build has no `whip` muxer (so WHIP depends on the FFmpeg build, research-deferred). Test results above are from this run's scratch tests, not from the vendor docs.

## 9. Recording writers

- Audio WAV (internal): RF64/WAV writer on the IO thread from the `AudioBlock` queue; 32-bit float or 24-bit PCM; header rewritten every 5 s and on close so a crash loses at most 5 s [V design, test below]. The 4 GiB limit is avoided by 1 h or 2 GiB segments, whichever first.
- Segment rules: `<show>_<UTC timestamp>_<n>.<ext>`, `.part` suffix while open, atomic rename on close; a `recovery.json` per session lists segments and states (REQ-R04). Free space is checked at start and every 5 s; below 2 GiB warns, below 256 MiB stops with `DiskFull` and a clear message; short writes are errors.
- fsync: file after each header rewrite and at close; directory after atomic rename. Network or removable folders are allowed but warned (write latency).
- Start failure order: validate folder, free space, encoder, then start; never report Recording until the first block is written (REQ-R06).
- Recovery on next launch: `.part` files listed in the recovery banner with options Keep, Repair (WAV header fix), Delete. Nothing is deleted without a confirm.

## 10. Secrets store

- Windows: Credential Manager (`CredWriteW`/`CredReadW`, generic credential, persist local machine); macOS: Keychain (`SecItemAdd`, `kSecClassGenericPassword`, device-only accessibility); Linux: libsecret via the Secret Service (`secret_password_store_sync`), loaded with `dlopen` so there is no hard dependency. [U] exact API usage verified against current headers in the spike.
- Fallback when no keychain exists (headless Linux, CI): an encrypted file `secrets.bin` using libsodium secretbox with a key derived from a user passphrase (Argon2id) or, if the user declines a passphrase, refuse to store and prompt every time. Never store plaintext.
- Show files and support bundles contain only a secret **reference** (`secret:<uuid>`); export tools strip even references. Memory holding a secret is zeroed after use (`sodium_memzero` or `SecureZeroMemory`). The UI shows "saved" and offers Replace and Delete only.
- Test: `FakeSecretStore` for logic; a Linux CI test runs against an in-memory libsecret mock or the encrypted fallback.

## 11. File dialogs

`juce::FileChooser::launchAsync` only; `JUCE_MODAL_LOOPS_PERMITTED=0` is set so a modal loop cannot be introduced by accident [V]. On Linux native dialogs go through the GTK/zenity/kdialog route JUCE implements; if none exists JUCE falls back to its own chooser [U]. The chooser object must outlive the async call (`std::shared_ptr` captured by the callback). Results are validated in the callback: canonical path, within an allowed set for Save-as-copy, extension allowlist (`.stagesignal`, `.json`, media types), size limits (`security` module). On macOS sandboxed builds, security-scoped bookmarks would be needed [U]; v1 is not sandboxed. Headless: `FakeFileChooser` returns scripted results; real dialogs are not run in CI.

## 12. ZIP

`juce::ZipFile` reads and `ZipFile::Builder` writes [V]. Reading is wrapped in `SafeZip`: reject entries with absolute paths, `..`, drive letters, backslashes mapped to `/` before checks, symlink entries and names with NUL; cap entry count (10,000), per-entry size (256 MiB) and total expanded size (2 GiB) and compression ratio (200:1); extract to a fresh temp directory and rename only after validation. Writing uses a fixed order and fixed timestamps for deterministic output. Portable export (.stagesignal) and support bundles use these. Test vectors: zip-slip names, huge-ratio bomb, duplicate names, case-colliding names on case-insensitive volumes.

## 13. Logging

`juce::Logger` is replaced by `RingLogger`: a lock-free per-thread ring for realtime threads (fixed-size records with a message id and two integer args, formatted later by the IO thread) and a normal buffered path for non-realtime threads. File sink: `logs/stagesignal-<date>.log`, rotated at 5 MiB, 5 files, UTF-8, timestamps UTC. All strings go through `security::sanitise` (no secrets, no absolute user paths: the home directory becomes `~`). Levels Error/Warn/Info/Debug; Debug is off by default. Log writing failure never throws and never blocks; dropped records are counted. The support bundle contains the sanitised logs only (spec 16).

## 14. Camera

Use `juce::CameraDevice` (module `juce_video`) where it exists. JUCE has no Linux `CameraDevice` implementation in the 8.0.16 tree [V], and `app/CMakeLists.txt` does not yet link `juce_video` (needs-from-others). Linux camera is therefore "not supported in v1" unless a V4L2 adapter is added (`CAMERA` thread, `VIDIOC_*` via `ioctl`). Permission: macOS needs `NSCameraUsageDescription` and returns no frames until the user allows; Windows privacy settings can block silently. States: Off, Previewing (operator only), Published (visible to audience), Lost. Starting the camera never publishes it; publishing needs the explicit Publish control and the status chip shows an eye icon. Frame delivery is asynchronous via callback into a `LatestMailbox`.

## 15. Tests and headless verification

| Test | Layer | Verifies |
|---|---|---|
| `adapters: audio fake loss/return` | U | loss/return sequences with FakeAudioDevice; epoch increments; no crash |
| `adapters: timestamp mapper` | U | convergence, outlier rejection, valid flag only when converged |
| `adapters: callback rt-safety` | U | callback body allocates nothing (allocation hook counter) |
| `adapters: display identity` | U | identical monitors flagged; reorder and hot-plug keep roles; fallback matching |
| `adapters: audience window smoke` | H (Xvfb) | window created, borderless, bounds equal fake display, no focus, recreated after change |
| `adapters: midi queue` | U | stamp, overflow policy, clock tick scheduler drift under 1 ms over 60 s with FakeClock |
| `adapters: udp sender` | U | with a loopback socket: Art-Net packet bytes equal golden; sACN root/framing/DMP layer golden; sequence rules; zero packets before Arm; stops at Disarm |
| `adapters: udp rate` | U | 25 Hz default, 40 Hz max, 44 Hz hard cap never exceeded |
| `adapters: ffmpeg argv` | U | no shell metacharacters expanded; secret not in argv in the preferred mode; allowlist present; env whitelist |
| `adapters: ffmpeg lifecycle` | H | with a stub child (a tiny script) test normal exit, crash, hang (kill at 6 s), stop within 1 s |
| `adapters: ffmpeg kill container` | M/H | the Matroska vs MP4 kill test above, rerun on the CI ffmpeg |
| `adapters: wav writer` | U | header valid after abrupt stop (truncate at random points), segment rotation, disk-full injection |
| `adapters: secrets` | U | fake store round trip, fallback file encrypts (no plaintext in bytes), references only in show |
| `adapters: safe zip` | U | zip-slip, bomb limits, deterministic output |
| `adapters: logger` | U | rotation, sanitiser, realtime ring drops counted not blocked |
| `adapters: camera availability` | U | state machine with FakeCamera; preview never sets Published |

Under Xvfb: window and focus tests run with the real JUCE peer; there is no sound card, so audio uses the fake device or JUCE's ALSA null device if present [U]. Real USB-DMX, real MIDI hardware, real cameras, real multi-monitor and the macOS permission prompts are manual (M) and recorded in KNOWN_LIMITATIONS.

## 16. Honest limits

- No stable display identity from JUCE; identity providers are platform code that cannot be tested in CI beyond fakes.
- Windows/Linux audio host time is estimated, so A/V sync is calibrated, not guaranteed; plugin mode cannot read host time.
- Stream key exposure on the command line in v1 is a known weakness until a relay or a different client exists.
- No Linux camera in v1. No Wayland embedded CLAP GUI. No serial DMX timing claim.
- macOS Local Network and camera permission behaviour is unverified on real machines.
- Nothing here was run on real hardware.

## Sources

- [A1] JUCE `8.0.16-security` (sha ea77444750de45e036e0279bb0291144bf8e3688): `juce_audio_devices` (`AudioIODeviceCallbackContext`, hostTimeNs only on CoreAudio), `juce_gui_basics` `Displays` struct, `juce_core` `ChildProcess`, `DatagramSocket`, `ZipFile`, `juce_video` platform directories. Read from a local clone 2026-10-20.
- [A2] clap-juce-extensions (MIT): README and source, embedded GUI platforms, MIDI out message sizes, no hostTimeNs.
- [A3] Art-Net 4 specification PDF (Artistic Licence): ArtDmx layout, port 6454, 44 Hz limit. Downloaded and text-extracted 2026-10-20.
- [A4] Microsoft Learn: `DISPLAYCONFIG_TARGET_DEVICE_NAME` fields.
- [A5] X.Org RandR protocol (EDID property).
- [A6] Local FFmpeg tests in the author's sandbox (kill test, `-/opt` test, `whip` muxer absent); not reproducible from a URL, rerun in CI.
- [A7] ESTA ANSI E1.31 (sACN): not fetched in this pass, hence [U] markers.
