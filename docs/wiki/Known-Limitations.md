# Known Limitations

This page summarises `docs/KNOWN_LIMITATIONS.md` in the repository, which is the file that counts. It was a skeleton when this wiki was written. Each item will get a status (Open, Fixed in a version, or Accepted by owner) and a date at release. Nothing is hidden here to look better. If a limit is not written down, you will find it on stage.

## 1. Not qualified on hardware

| ID | Limit | Effect on you |
|---|---|---|
| L-01 | No physical 800 MHz class machine has been used to test the BASE tier. | Budgets are designed, not measured. |
| L-02 | No hardware DMX test. Art-Net, sACN and serial were tested against loopback and golden packets only. | No named node, fixture or adapter is claimed to work. |
| L-03 | No accuracy numbers on real music for tempo, key and onsets. Tests use synthetic clicks and tones. | BPM or key on real tracks can be wrong, half-time or unknown. The UI shows confidence. |
| L-04 | No real camera, capture card or webcam matrix. | Camera behaviour is tested with fakes. |
| L-05 | A/V sync not measured over an hour on real devices. | Lip-sync on recordings and streams is unproven. |
| L-06 | The 4-hour soak is not run on every OS. | Long-show stability is unproven on Windows and macOS. |
| L-07 | Broadcast tested against a local loopback receiver only. | Real service quirks may appear. |
| L-08 | No assistive technology pass (NVDA, Narrator, VoiceOver). | Keyboard and contrast are tested, screen readers are not. |

## 2. Distribution and trust

| ID | Limit |
|---|---|
| D-01 | Windows builds are unsigned. SmartScreen warns on first run. |
| D-02 | macOS builds are not notarised. Use the right-click Open route or `xattr`. |
| D-03 | Linux: tarball only, no package manager packages. |
| D-04 | The CLAP plugin is unsigned and tested in few hosts. Only hosts that were run are claimed. |
| D-05 | No automatic updates, by design. |
| D-06 | AGPL-3.0-or-later with JUCE under its AGPL option. A commercial JUCE licence is an owner decision. |
| D-07 | FFmpeg is not bundled. Video recording and broadcast need it installed. |

## 3. Platform limits

| ID | Limit |
|---|---|
| P-01 | JUCE gives no stable display identity. Identical monitors stay unassigned until you confirm. |
| P-02 | JUCE supplies host time only on CoreAudio. Windows and Linux A/V timing is estimated. |
| P-03 | CLAP wrapper: no host time, MIDI out limited to 2 or 3 bytes (no SysEx), embedded GUI only on Windows, macOS and X11. |
| P-04 | JUCE has no Linux camera implementation. |
| P-05 | No screen reader backend on Linux. |
| P-06 | No OS high-contrast detection. High contrast is a manual setting. |
| P-07 | Child process stdin cannot be written through JUCE, so a native wrapper is used. |
| P-08 | Multicast interface and TTL need native socket options. |
| P-09 | macOS Local Network permission may block LAN lighting output. Not checked on a real Sequoia machine. |
| P-10 | Stream keys appear on the FFmpeg command line in version 1. Other local users can see them in a process list. |
| P-11 | MP4 recordings are not crash-safe. Matroska and fragmented MP4 are the defaults. |
| P-12 | MIDI 2.0 and Windows MIDI Services are not supported. |
| P-13 | WHIP, projectM, NDI-like transports and silhouettes are research only. |

## 4. Safety and privacy

| ID | Limit |
|---|---|
| S-01 | Strobe lock covers software scenes, mappings and channels declared as Strobe. A strobe hidden in another channel is covered only if the profile declares it. |
| S-02 | Flash analysis of imported media is a heuristic. A clean result is not a guarantee. |
| S-03 | Not a medical or compliance product. WCAG 2.3.1 is an engineering bound. |
| S-04 | Crowd and silhouette features, if built, process people. Off by default. Legal review is the owner's. |
| S-05 | Emergency blackout acts on StageSignal outputs only. |

## 5. Not in v1

Mobile, web control, scripting, plugin hosting inside StageSignal, cloud sync, accounts, telemetry, multi-user control, timecode (LTC and MTC) chase, DMX input, and video walls beyond the output routing in the spec.

## 6. Documentation limits

- The `app-*` specs were written without building the application. Items marked `[U]` are unverified and must be confirmed in the first spike.
- Colour values in the UI spec are proposals until checked against the Circuit Drift Labs design system.
- `docs/TRACEABILITY.md` still lists every requirement as `Specified`. It is the source of truth for what is done. The module specs report passing unit tests, but the traceability rows have not been updated to match.

## What "done" means here

A feature is only called done when code and automated tests are merged and green in CI. Anything that needs hardware, a human or a real OS service is "verified (manual)" at best, and says who checked it, when and on what.
