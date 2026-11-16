# Known limitations

Skeleton created 20 Oct 2026 on `feat/app-specs`. The release engineer finalises this at release: every line gets a status (Open, Fixed in <version>, Accepted by owner) and a date. Nothing is hidden here to look better. If a limit is not written down, users will find it on stage.

Format per item: what is not done or not proven, why, what it means for the user, what would close it.

## 1. Not qualified on hardware

| # | Limit | Why | Effect on users | Closes when |
|---|---|---|---|---|
| L-01 | No physical 800 MHz class qualification. The BASE tier targets that class (spec v0.1 section 7) but CI runs on shared x86-64 and arm64 cloud machines, which are far faster | No such device in the project | A BASE promise like 24 of 25 frames per second (REQ-P06) is only checked on the benchmark rig, not on real old hardware | Owner runs the benchmark on a real low-end machine and records the report |
| L-02 | No hardware DMX qualification. Art-Net, sACN and serial adapters were tested against loopback sockets and golden packets only | No nodes, fixtures or adapters in CI | We do not claim any named adapter, node or fixture works. REQ-L06 requires a name and a result for each; none exist yet | A person tests each adapter and records model, driver, OS, result |
| L-03 | No music-corpus accuracy numbers on real music. Tempo, key and onset targets (REQ-A04, A05, A07) are tested on synthetic clicks and tones | Licensing of real recordings and no labelled set | BPM and key on real tracks can be wrong, half-time, or Unknown. The UI shows confidence for this reason | A licensed or self-made labelled corpus is added and results published |
| L-04 | No real camera, capture card or webcam matrix | None in CI | Camera behaviour is tested with fakes | Manual pass per OS |
| L-05 | A/V sync (REQ-R03) is not measured over one hour on real devices | Needs a flash-and-click rig | Lip-sync on recordings and streams is unproven | Rig test and result recorded |
| L-06 | The 4 hour soak (REQ-X06) is not run on every supported OS | Time and runner limits | Long-show stability is unproven on Windows and macOS | Soak run per OS on release candidate |
| L-07 | Broadcast is tested against a local loopback receiver, not against live platforms (RTMPS ingest services, SRT receivers) | Accounts, cost, and not wanting tests to transmit | Real service quirks may appear | Manual test against each documented receiver |
| L-08 | No real assistive technology pass | No test machines | Keyboard and contrast are tested; Narrator, NVDA and VoiceOver are not | Manual pass |

## 2. Distribution and trust

| # | Limit | Detail |
|---|---|---|
| D-01 | Windows binaries are unsigned in the beta | Windows SmartScreen shows a warning on first run. Signing options and costs are in `docs/specs/research-deferred.md` (code signing section) |
| D-02 | macOS builds are not notarised in the beta | Gatekeeper blocks a plain double click; users must use the right-click Open route or `xattr`. Notarisation needs a paid Apple Developer account |
| D-03 | Linux has no package-manager packages in v1 | Tarball or AppImage only; no distro review |
| D-04 | The CLAP plugin is unsigned and tested in few hosts | FL Studio, REAPER and Bitwig behaviour is described in research-deferred; only hosts we ran are claimed |
| D-05 | No automatic updates | By design (offline first) |
| D-06 | Owner selected existing commercial JUCE license on 22 October 2026 | No project source license is granted; commercial entitlement is owner-declared, not audited. Dependency notices accompany each archive; see DISTRIBUTION.md |
| D-07 | FFmpeg is not bundled by default | Video recording and broadcast need an FFmpeg on the machine; the feature turns off with a clear message if missing |

## 3. Platform limits found while writing the specs

| # | Limit | Source |
|---|---|---|
| P-01 | Native monitor paths/UUIDs/EDID hashes supplement JUCE geometry; duplicate native identities and unmatched geometry remain ambiguous and require operator confirmation | app-adapters 4.1 |
| P-02 | JUCE 8.0.16 supplies host time only on CoreAudio. Windows and Linux A/V timing is estimated | app-adapters 3.2 |
| P-03 | CLAP plugin: no host time, MIDI out limited to 2 or 3 byte messages (no SysEx), embedded GUI only on Windows, macOS and X11 (no Wayland, no floating window) | clap-juce-extensions; app-shell 7 |
| P-04 | JUCE has no Linux camera implementation | app-adapters 14 |
| P-05 | JUCE has no accessibility backend for Linux, so no screen reader support there | app-ui 8 |
| P-06 | No OS high-contrast detection in JUCE; High-contrast theme is a manual setting | app-ui 3.2 |
| P-07 | `juce::ChildProcess` cannot write stdin and merges output; we use a native wrapper | app-adapters 8.2 |
| P-08 | `juce::DatagramSocket` cannot set the multicast interface or TTL; native socket options are used | app-adapters 6.1 |
| P-09 | macOS Local Network permission may block LAN lighting output; behaviour not verified on a real Sequoia machine | app-adapters 6.3 |
| P-10 | Stream keys appear on the FFmpeg command line in v1 (visible to other local users while streaming) | app-adapters 8.2 |
| P-11 | MP4 recordings are not crash-safe; Matroska or fragmented MP4 are the defaults | app-adapters 8.5 |
| P-12 | MIDI 2.0 and Windows MIDI Services are not supported in v1 | research-deferred |
| P-13 | WHIP, projectM, NDI-like transports, silhouettes and similar items are research only | research-deferred |

## 4. Safety and privacy

| # | Limit | Detail |
|---|---|---|
| S-01 | Strobe lock covers software scenes, mappings and configured fixture strobe channels only | A fixture with a built-in strobe mode on another channel is only covered if the profile declares it (REQ-S12) |
| S-02 | Flash analysis of imported media is a heuristic | A video flagged clean can still flash; imported media carry a visible warning (spec 13) |
| S-03 | We are not a medical or compliance product | We use the WCAG 2.3.1 three-flashes-per-second threshold as an engineering bound, not as a guarantee of safety for photosensitive viewers |
| S-04 | Audience silhouettes and crowd-mic moments, if built, process people in a venue | Biometric and consent rules differ by place (GDPR, BC PIPA); opt-in and off by default, no storage; legal review is the owner's |
| S-05 | Emergency blackout acts on StageSignal outputs only | It cannot switch off a fixture that ignores DMX, a dimmer pack with its own look, or a projector's own feed |

## 5. Product scope not in v1

Windows-only wxWidgets shell (original v0.1 plan), mobile, web control surface, scripting, plugin hosting inside StageSignal, cloud sync, accounts, telemetry, multi-user control, timecode (LTC/MTC) chase, DMX input, video walls beyond the output routing in the spec.

## 6. Documentation limits

- The specs in `docs/specs/app-*.md` were written without building the application. Items marked `[U]` are unverified and must be confirmed in the first spike.
- Colour values in app-ui are proposals until checked against the Circuit Drift Labs design system.
- `docs/TRACEABILITY.md` shows `Specified` for every row on creation; it is the source of truth for what is actually done.

## 7. To finalise at release

- Replace every `Open` with a result and a date.
- Add anything found in the beta (crash reports, user-facing surprises).
- Link each limit to the issue that tracks it.
- Re-run the checks that this file depends on: the contrast script, the copy lint, and the sample benchmark report.

## Development evidence and open implementation gates (22 October 2026)

- Linux standalone, VST3 and CLAP targets build; all 19 portable CTest targets and all 63 application self-tests passed locally. Physical device, host and long-duration qualification above remains open.
- Camera preview/publication now has a native JUCE adapter on Windows/macOS and an explicit Unsupported Linux fallback. Camera callbacks copy an image reference into a newest-frame mailbox; RGBA conversion runs on MEDIA. Fake-camera tests verify preview/publication separation, selected-source binding, per-output routing, bounded loss and close. Native hardware and permission prompts remain untested.
- Audience windows close conservatively on changed topology, DPI, operator movement or invalid window bounds, and need an explicit reopen. The message-thread watchdog polls every 50 ms; this is not proof of zero transient OS window migration, and hardware hotplug timing is unqualified.
- Setup wizard progress and completion now persist and resume through local preferences. Fake-UI tests cover keyboard completion, saved-step resume and corrupt progress.
- Moving-image playback now uses optional external FFmpeg/ffprobe with bounded metadata/protocols, frames, queues and timeouts on MEDIA. Generated local H.264 and animated GIF tests verify distinct decoded frames and clean shutdown; fake-process tests cover pause, timing, alpha premultiplication, overflow and timeout. Codec/container/device coverage remains bounded by the documented allowlist.
- Software benchmark UI and wizard execute real BEN-01/BEN-02 raster/compositor workloads and persist reports; BEN-03 through BEN-06 remain explicitly skipped. Missing measured CPU/latency/device metrics produce Incomplete, never a fabricated qualification. Completing those scenarios and physical qualification remains required.
- Window bounds and weak display geometry hints persist; outputs require explicit display reconfirmation and never auto-open from ambiguous identity. Native identity now resolves Windows monitor device paths, macOS display UUIDs and Linux XRandR EDID hashes. Duplicate identities across connectors and ambiguous geometry are refused. Mixed-DPI matching, real identical monitors and hardware hotplug remain unqualified; outputs still require explicit reopening.
- New tests prove emergency composed black frames while CONTROL is blocked and no owned TCP/UDP sockets during ten seconds of native Linux idle. Full-callback timing, long-duration and cross-platform network qualification remain open. Real plugin-host validation is recorded separately against the exact tested bundle; an earlier bundle pass does not qualify changed binaries. Hardware/corpus/accessibility/soak gates are distinct from implementation gaps.
- Serial USB-DMX remains an explicitly unsupported optional capability. The current normative adapter scope makes serial optional; Art-Net and sACN remain the implemented lighting transports.
