# research-deferred: decision-ready notes on items the owner has not decided

Status: research, 20 Oct 2026. All web facts were fetched or searched on 2026-10-20 unless a line says otherwise. "Verified" means I read the quoted source or ran the stated command; "unverified" means I did not. Nothing here changes a requirement; each section ends with the decision the owner needs to make.

Owner summary (one line each):

| # | Item | Recommendation | Owner decision needed |
|---|---|---|---|
| 1 | WebRTC | Stay deferred. If wanted later, WHIP through the FFmpeg child process only | Is a browser or CDN WebRTC target a real need? |
| 2 | RTP/RTCP contribution | Implement (small) through the FFmpeg `rtp` muxer, SDP export, unencrypted label | Which receiver profile (SDP) must work first? |
| 3 | projectM renderer | Keep optional and off by default; ship no preset pack | Accept a "bring your own presets" model? |
| 4 | Audience sound/silhouettes | Sound level moments first; camera silhouettes only after a written privacy decision | Venue privacy stance, jurisdictions |
| 5 | MIDI 2.0 | Defer; stay MIDI 1.0 | None now |
| 6 | Projection mapping, all-output recording | Stay non-goals | None now |
| 7 | Legacy-OS shell (800 MHz / 512 MB) | No-go for v0.2 | Provide a physical fixture and a named OS, or drop the floor |
| 8 | Code signing | Unsigned beta now; Windows via Azure Artifact Signing and macOS Developer ID when budget exists (about US$219/yr) | Budget and publisher identity (individual or registered org) |
| 9 | CLAP hosts | Test matrix in section 9; FL Studio, REAPER, Bitwig documented to support CLAP | Which host is the first "supported" one? |

---

## 1. WebRTC (deferred)

Facts.
- FFmpeg documents a `whip` muxer: "WebRTC (Real-Time Communication) muxer that supports sub-second latency streaming according to the WHIP (WebRTC-HTTP ingestion protocol) specification. This is an experimental feature." It uses HTTP for SDP exchange and ICE lite candidates, STUN, a DTLS handshake for SRTP keys, then RTP/SRTP; it asks for H.264 without B-frames and Opus audio. [S1]
- WHIP is IETF RFC 9725 (Standards Track, March 2025): an HTTP-based protocol to ingest WebRTC content into streaming services or CDNs. [S2] It is an **ingest** protocol (publisher to server). Showing the program to arbitrary browsers needs a server or SFU/CDN that speaks WHEP or its own player protocol.
- The FFmpeg shipped on this build machine is 6.1.1 (Ubuntu) and its muxer list contains `rtp`, `rtsp`, `rtp_mpegts` but no `whip` (checked with `ffmpeg -muxers`). The FFmpeg download page lists 9.0.2, 8.1.3, 8.0.3 as current (checked 2026-10-20), so a newer pinned build is a prerequisite, and whether it enables `whip` depends on its configure flags (unverified).
- libdatachannel (C++ WebRTC library) is MPL-2.0 since 0.18 (previously LGPL-2.1-or-later). [S3]

What it would take.
| Path | Work | Fit with the spec |
|---|---|---|
| A. WHIP via the existing ffmpeg child | New `Profile::Whip` in `broadcast` (URL `https://...`, bearer token from the secret store, H.264 baseline no B-frames, Opus 48 kHz). Reuses IF-10 and the state machine of spec 15. Needs FFmpeg with `whip` + a TLS/DTLS library | Good: no new socket code in our process, secrets stay out of argv (see app-adapters 8) |
| B. In-process libdatachannel | Our own H.264 RTP packetiser, RTCP feedback, congestion control, ICE, TURN config, a new MPL-2.0 dependency | Poor: new inbound-capable stack, much more test surface, violates "narrow adapters" (DEC-02) |
| C. Nothing | Use SRT/RTMPS to a service that converts to WebRTC | Zero cost |

Risks: experimental muxer; no automatic NAT/TURN story when the server is not public; browsers direct-to-performer connections would require a listener or signalling service (rejected by REQ-X04 "no public listener"); DTLS needs certificates; latency claims in the FFmpeg doc are for its example, not measured here.

Recommendation: keep WebRTC deferred (spec 15 already says so). If the owner names a concrete WHIP target, do path A as a P2 requirement with its own T-CAST receiver test using a local WHIP server.
Decision: name a target service, or confirm "never".

## 2. RTP/RTCP contribution (REQ-B07, P2)

Facts.
- RTP/RTCP: RFC 3550. H.264 payload format: RFC 6184 (verified title, "RTP Payload Format for H.264 Video"). SDP: RFC 8866 (verified). SRTP: RFC 3711 (verified). [S4]
- FFmpeg 6.1.1 here: `-f rtp` muxer exists with options `skip_rtcp`, `send_bye`, `payload_type`, `ssrc`, `cname`, `h264_mode0`; `srtp` protocol options `-srtp_out_suite`, `-srtp_out_params`. I ran `ffmpeg -f lavfi -i testsrc ... -c:v libx264 -an -f rtp -sdp_file out.sdp rtp://127.0.0.1:5006` and it wrote an SDP (`m=video 5006 RTP/AVP 96`, `a=rtpmap:96 H264/90000`, `a=fmtp:96 packetization-mode=1`). Note the `fmtp` had **no** `sprop-parameter-sets` or `profile-level-id`, so a receiver must accept in-band SPS/PPS; StageSignal must force parameter-set repetition on keyframes and test it per receiver.
- RTCP is sent by the muxer unless `skip_rtcp` is set.

Design (small).
- `broadcast` gets `Profile::RtpLan`: destination `ip:port` (unicast or multicast), codec H.264 (+ Opus or AAC? keep to Opus/PCMU only as listed by the receiver SDP), payload type fixed, SSRC random per session, CNAME = show UUID short hash (no host names or paths).
- On Start, write the SDP text to the Record/Broadcast tab (copy button) and to a user-chosen `.sdp` file. The UI label says "RTP: not encrypted" unless SRTP params are configured; with SRTP the key is a secret in the store and the SDP shows the crypto line only on explicit copy.
- Multicast needs the selected NIC (app-adapters 6). macOS requires the multicast entitlement for sandboxed use; hardened non-sandboxed apps still get a Local Network permission prompt on Sequoia (see section 8 note and app-adapters 6.3).
- Verification: loopback receive with `ffprobe -protocol_whitelist file,udp,rtp -i out.sdp` in CI (needs the same pinned FFmpeg); packet-loss/jitter via `tc netem` is Linux only and manual.
Limits: no FEC, no retransmission, no congestion control; LAN only; not an internet profile.
Decision: first receiver (e.g. a hardware decoder or OBS/VLC SDP import) so we test the right SDP.

## 3. projectM optional renderer

Facts.
- Library license LGPL-2.1 "or (at your option) any later version" (file header of `COPYING` / source header, shallow clone of `projectm` master read 2026-10-20). Vendored directories: `glad`, `glm`, `hlslparser`, `projectm-eval`, `stb_image` (their licenses are not audited here). [S5]
- Latest release tag at check time: `v4.1.8`. The API `projectm_opengl_render_frame_fbo` and `projectm_create_with_opengl_load_proc` are marked `@since 4.2.0` in master headers, i.e. not in 4.1.x. `projectm_pcm_add_float`, `projectm_opengl_render_frame`, `projectm_set_window_size`, `projectm_load_preset_file` are in `src/api/include/projectM-4`. [S5]
- Requires OpenGL Core 3.3 or OpenGL ES 3.2 (docs/building.rst: "As long as a platform provides OpenGL Core 3.3 or OpenGL ES 3.2 libraries"). The library ships **no presets**; packs are separate repos (README). [S5]
- Preset rights: "Cream of the Crop" is 9,795 presets "curated and sorted by ISOSCELES" and its README states no license; the original Milkdrop 2 pack README is 444 characters and states none either (fetched). VLC's 2011 commit "Licensed OK presets for projectM" explains the project kept only presets whose license was known because "Distributions usually ship different ones whose license are unknown". [S6]
- JUCE: `OpenGLContext::setOpenGLVersionRequired` accepts `openGL3_2`, `openGL4_1` ("the latest supported by macOS at time of writing") and `openGL4_3`; `attachTo(Component&)`, `OpenGLRenderer`, `setContinuousRepainting` exist (JUCE 8.0.16 header). macOS deprecated OpenGL in 10.14 (2018). [S7]

Design.
1. `SceneKind::ProjectM` is not in `core/model` (enum has `Image, Gif, Video, Count`); adding it is a model change for the integrator, behind compile flag `SS_ENABLE_PROJECTM`.
2. Integration: an `OpenGLContext` attached to the **audience** window component (not the operator window). `newOpenGLContextCreated` calls `projectm_create()`, `renderOpenGL` pushes PCM (a 2048-sample float mono tap from the analysis worker through a mailbox) with `projectm_pcm_add_float`, sets window size, calls `projectm_opengl_render_frame` (4.1.x) or `_fbo` (4.2+), `openGLContextClosing` destroys. Must run on the GL thread.
3. The program output for recording/broadcast needs the pixels: asynchronous PBO read-back (`glReadPixels` into a pixel pack buffer, two frames of latency) into `ss::PixelBuffer`. Cost is unmeasured.
4. **Safety problem (blocking):** presets can flash arbitrarily, and REQ-S12 (P0) says Disable Strobing must prevent flash/strobe presets. We cannot inspect shader presets for flash risk. Options: (a) refuse ProjectM scenes while strobing is disabled (simple, honest); (b) add an output-stage luminance flash limiter on read-back that caps global luminance change to at most 3 flashes in any one-second period, matching WCAG 2.3.1's threshold wording ("anything that flashes more than three times in any one second period, or the flash is below the general flash and red flash thresholds"). [S8] Option (b) also needs the GL path to be post-processed before the audience window shows it, which complicates direct display. Recommend (a) for first release.
5. Tier: Enhanced and above only. BASE does not promise GL 3.3 (spec 6: no "modern OpenGL").
6. Presets: ship none. The Media/Text tab gets a "Preset folder" picker (user supplied). Documentation says the user is responsible for preset rights. If a bundled look is wanted, author original presets in-house under a license we choose.
7. Licensing: LGPL-2.1+ code linked into an AGPL-3.0-or-later app is normally combinable (LGPL 2.1 section 3 lets recipients choose GPL terms); dynamic linking keeps the replace-the-library path simple. Owner/legal to confirm; no legal approval claimed (addendum DEC-05).
8. Headless test: Mesa software GL under Xvfb is the usual route; I did not verify a GL 3.3 context on the CI image, so the first spike must. Golden-image tests are not portable across GPUs; compare by perceptual threshold or skip.
Decision: accept bring-your-own-presets, and choose option (a) vs (b).

## 4. Audience sound-driven moments and silhouettes (optional, M7)

Split into two features with different risk.

4a. Sound-driven moments (audience noise level triggers a visual moment, e.g. crowd swell).
- Uses the existing analyser on a second input channel or the main mic; only level and band features are used; **no audio from this path is stored, recorded or streamed** (the recorder/broadcaster tap never includes this route unless the user puts it there).
- Opt-in per show, off by default, label "Crowd mic moments" with a visible indicator on the top bar when active. Capped trigger rate and strobe-lock apply.
- Acceptance: synthetic noise swell triggers within 1 s; at silence nothing fires; with Disable Strobing the effect uses non-flash modulation only.

4b. Silhouettes (camera aimed at the audience).
- Technical: on-device motion mask from frame differencing on a 160x120 greyscale downscale; the full-resolution frame is never kept; the mask is rendered as an abstract shape. No face detection, no tracking IDs, no identity.
- Privacy facts: GDPR Article 4(14) defines biometric data as personal data from technical processing of physical characteristics "which allow or confirm the unique identification of that natural person, such as facial images"; a non-identifying silhouette is designed to stay outside that definition but is still a camera view of people. [S9] British Columbia's PIPA defines "personal information" as "information about an identifiable individual". [S10] Whether a given silhouette view is identifiable depends on resolution and context. Some US states have biometric statutes (for example Illinois BIPA); I did not verify their text here.
- Rules to adopt: default off; per-show opt-in with a plain-language confirm ("This turns on a camera pointed at the audience. Nothing is saved or sent anywhere."); permanent indicator in the status bar and a Show Doctor warning when enabled together with recording or broadcast; the audience camera can never be routed to the recorder or broadcaster as video; no persistence of frames or masks, enforced by a test that greps the writer paths; macOS camera permission and the OS camera-in-use indicator stay visible.
- Recommendation: build 4a in the M7 window; do 4b only after the owner records a privacy stance and the venues agree (signage is a venue matter). Not legal advice.
Decision: yes/no per feature and the privacy stance wording.

## 5. MIDI 2.0 deferral

Facts.
- JUCE 8.0.16 contains UMP code (`modules/juce_audio_devices/midi_io/ump`) and a build switch `JUCE_USE_WINDOWS_MIDI_SERVICES` whose header says it enables MIDI 2.0 on Windows, needs `NEEDS_WINDOWS_MIDI_SERVICES TRUE`, the Windows SDK 10.0.20348.0 or newer, the CppWinRT tool and a NuGet package that "is not hosted in the official repository", and a runtime SDK package installed by our installer. [S7]
- Windows MIDI Services: MIDI.org states it started "rolling out to retail Microsoft Windows throughout the month of February, 2026". [S11]
- The CLAP wrapper only forwards MIDI out messages of 2 or 3 bytes (wrapper source), `core/base` `ScheduledMidi` holds 3 bytes, and the spec baseline is MIDI 1.0 (spec 6).
Decision basis: no requirement needs 32-bit values or property exchange; class-compliant MIDI 1.0 controllers are what the show needs. Extra OS-specific installer burden on Windows.
Recommendation: defer. Keep `MidiMapping` values normalised (already double 0..1 at IF-02) so a UMP front end can be added later without changing the show format. Do not enable `JUCE_USE_WINDOWS_MIDI_SERVICES`, `JUCE_USE_WINRT_MIDI` stays 0 until a BLE-MIDI need appears.
Decision: none now; revisit when a specific controller needs MIDI 2.0.

## 6. Projection mapping and all-output recording (non-goals, with the price list)

Projection mapping (spec section 1 non-goal). What it would take: per-output warp mesh and edge blend (GPU or a CPU bilinear warp), a calibration UI with test grids, per-output mesh storage in the show model, and projector-specific lens/blend workflows. Software rendering at BASE has no measured headroom for a full-frame warp. Cheapest honest subset if ever asked: 4-corner keystone per output as a post-process; unmeasured. Stays a non-goal.
All-output recording (AS-03 gives single program output). Would need N frame fan-outs, N encoders (cost scales with N, see BEN-04), N disk writers and a per-output sync reference. The model already has logical outputs, so it is a `record` session-per-output feature gated by admission on STANDARD and above. Stays a non-goal for v0.2; documented so nobody quietly promises it.

## 7. Legacy-OS shell for the 800 MHz / 512 MB floor

What the floor is: Windows Vista Home Basic requirements (800 MHz CPU, 512 MB RAM) per the spec's source SRC-22. Microsoft's lifecycle page: Windows Vista extended support ended 4/12/2017 (checked). [S12]
Why the JUCE app cannot be that shell: JUCE 8.0.13 "Bumped the minimum Windows target version to 1607" (JUCE `CHANGE_LIST.md`, tag 8.0.16-security). [S7] The addendum DEC-03 already dropped Vista/XP claims.
What a shell would need:
1. A second UI/audio/display shell over the JUCE-free `core/*`. wxWidgets 3.2.8's install guide says it targets "Windows (XP, Vista, 7, 8, 10, 11)" while the current master says "Windows 7 or later" (both read 2026-10-20), so a legacy shell means a pinned 3.2.x LTS. [S13] That is the original v0.1 plan.
2. Audio capture: WASAPI exists on Vista and later; XP would need WinMM/DirectSound; timestamps differ per API (spec 6).
3. A toolchain whose output runs there and has `std::filesystem` and C++17 in its runtime (unverified: I did not check XP-capable toolset versions).
4. Core compiled without SSE2/AVX (spec 6 forbids requiring them). The core currently has no CPU-dispatch code; a CI job compiling `core/*` with `-march=pentium3 -mno-sse2`-style flags on 32-bit would prove it builds, not that it runs fast.
5. No TLS 1.2 on old systems means no RTMPS/SRT (spec RISK-10 already says "offline limited tier").
6. A **physical** 800 MHz fixture: Q-01/Q-02 are unanswered, so there is nowhere to measure. Emulators (QEMU) can check instruction-set compatibility but not timing.
Honest verdict: **no-go for v0.2.** The JUCE-free core keeps the door open (that was the addendum's intent) and the benchmark targets stay as engineering budgets. Cheap hedges worth doing now: a headless `ss_bench` target that runs the core DSP+show+software-raster loop and prints frame times; a CI compile check of `core/*` for 32-bit without SSE2 if a 32-bit toolchain is available. Spend nothing on a legacy shell until the owner supplies a fixture and a named OS.
Decision: supply fixture + OS, or formally retire the 800 MHz floor as a benchmark-only budget.

## 8. Code signing, notarisation and SmartScreen

| OS | What users see unsigned | Option | Cost (checked 2026-10-20) | Notes |
|---|---|---|---|---|
| Windows | SmartScreen warning ("Windows protected your PC") | Azure Artifact Signing (formerly Trusted Signing) | about US$9.99/month for up to 5,000 signatures and one certificate profile; US$99.99/month for 100,000 signatures and 10 profiles [S14] | Eligibility per Microsoft: organisations in USA, Canada, EU, UK; individual developers "currently limited to the USA and Canada"; identity validation takes a few business days; no hardware token; **does not give instant SmartScreen trust**, reputation builds with consistent signing [S15] |
| Windows | same | OV certificate (DigiCert, Sectigo, GlobalSign...) | typically US$150 to 300/year; private key must be on an HSM or hardware token since June 2023 [S15] | worldwide |
| Windows | same | EV certificate | US$400+/year; Microsoft says the instant-SmartScreen benefit "was removed in 2024" and paying the premium solely for it "is no longer justified" [S15] | not recommended |
| Windows | same | SignPath Foundation (free for OSS) | free [S16] | Conditions page: "OSI-approved Open Source license without commercial dual-licensing for all components", no proprietary components, actively maintained, already released. JUCE is dual-licensed (AGPL / commercial), so eligibility is **uncertain**; ask them. The cert is issued to SignPath Foundation, so the publisher shown is "SignPath Foundation" |
| macOS | Gatekeeper blocks or warns; users run `xattr -dr com.apple.quarantine` (current beta notes) | Apple Developer Program + Developer ID + notarisation | US$99 per membership year (fee waiver exists for nonprofits, educational, government) [S17] | Apple: sign "any apps, plug-ins, or installer packages" with Developer ID and submit to be notarised; JUCE CMake doc: `HARDENED_RUNTIME_ENABLED` is "Required for notarisation" [S7][S18]. CI currently ad-hoc signs (`codesign -s -`) which satisfies the arm64 "must be signed" rule but gives no trust |
| Linux | no OS gate | none required; publish `SHA256SUMS.txt` plus a detached signature (minisign or GPG) | free | optional Flatpak/AppImage later; unsigned is normal on Linux |

Options for v0.2: (1) ship unsigned betas with accurate notes (what the repo does now); (2) pay for macOS first (US$99/yr), because the macOS friction is worst, and add Windows signing at the first public release. Minimum total for both OS: about US$218.88 per year (99 + 12 x 9.99), before any identity or tax paperwork. The owner is in Canada, which Microsoft's page lists as eligible for individual developers. The signed publisher name is the validated legal name; a "Circuit Drift Labs" publisher name needs a registered organisation (unverified how Microsoft validates a business name in Canada).
CLAP specifics: a `.clap` is a DLL (Windows) or bundle (macOS): sign the binary inside the bundle first, then the bundle. Whether hardened-runtime hosts accept an ad-hoc or differently-signed plug-in bundle (library validation) is unverified; test per host.
Local Network note (macOS Sequoia): sending to LAN addresses (Art-Net/sACN/RTP) may trigger the Local Network privacy prompt; multicast requires Apple's multicast entitlement, which Apple documents as needing a request (Apple developer forum threads on Sequoia multicast; details unverified). [S19] JUCE exposes `NETWORK_MULTICAST_ENABLED` for the entitlement. Plan: Art-Net/sACN **unicast** is the default on macOS; multicast is advanced and documented as possibly blocked.
Decision: budget, publisher identity (individual vs registered organisation), and whether SignPath is worth asking about.

## 9. CLAP host caveats (FL Studio, REAPER, Bitwig and others)

Which hosts support CLAP (what I verified).
| Host | Evidence | Version |
|---|---|---|
| Bitwig Studio | MusicRadar, 28 June 2022: Bitwig 4.3 "adds support for CLAP" [S20] | 4.3 and later |
| REAPER | KVR, 30 Nov 2022: REAPER v6.71 "adds support for CLAP plugins"; REAPER's current `whatsnew.txt` shows continuing CLAP work (for example "CLAP: support requesting plug-in bus size", "CLAP: hide non-automatable parameters from various REAPER automation interfaces") [S21] | 6.71 and later |
| FL Studio | Rekkerd, 27 June 2024: FL Studio 2024 lists "CLAP plugin support"; Image-Line release notes: "Introducing CLever Audio Plugin (CLAP) plugin support", plus fixes "CLAP plugins scale incorrectly" (macOS) and "CLAP plugins do not save the wrapper settings" [S22] | 2024 (24.x) |
| MultitrackStudio | named as a CLAP 1.0 host in the clap-juce-extensions README [S23] | unspecified |
| Others (Ableton Live, Logic Pro, Pro Tools, Cubase, Studio One) | one vendor blog (Antares) says Ableton, Logic, Pro Tools and FL Studio did not support CLAP; it contradicts Image-Line for FL Studio, so I treat it as outdated and **unverified** for the rest [S24] | not claimed |

Wrapper facts that shape behaviour (verified in source, see app-shell 7.1): embedded GUI only, win32/cocoa/x11 only (no Wayland, no floating window), MIDI out only 2 to 3 byte messages, no `hostTimeNs`, `wrapperType_Undefined`.

Per-host risks to test (all manual, none run). Expect different outcomes per host.
1. GUI size and scaling: FL Studio had scaling fixes listed; test 100%, 150%, 200% on Windows and a Retina Mac; resize to 640x480 compact layout.
2. Keyboard focus: hosts consume Space, Ctrl+S, Ctrl+K, F-keys. The editor must work with mouse alone for blackout and Next. Record per host which keys arrive.
3. Opening audience windows from inside a host process: top-level windows from a plug-in may open behind the host or on the wrong display; host fullscreen modes and macOS Spaces change this. Verify the window lands on the selected display and survives the host window being minimised.
4. Closing the editor while the plugin runs: analysis, lighting and show must keep going; reopening must show live state.
5. Effect plug-in MIDI output routing: whether the host forwards plug-in MIDI out from an audio effect to hardware; many routes need an instrument-type plugin. We ship as an audio effect (CLAP feature `audio-effect analyzer utility`, `app/CMakeLists.txt`). Test and document per host; MIDI clock out may not be possible from some hosts in effect mode.
6. Buffer size and sample-rate changes mid-session, offline render/bounce/freeze (analysis only; no outputs), plugin delay compensation (we report zero latency on the main bus).
7. State: save/reopen project; blob size limit; "Secondary" mode when two instances load.
8. Sleep/wake and display hotplug while in a host.
9. Linux: X11 embedding only; REAPER on Wayland runs through XWayland, which is the practical route.
10. Crash isolation: some hosts sandbox plug-ins out of process; lighting sockets and audience windows then live in that process. Test with each host's sandbox settings.
Decision: choose the first officially supported host (suggest REAPER, because it has the longest CLAP history in this table, and Bitwig), publish the tested host/OS/version triples in KNOWN_LIMITATIONS.

---

## Sources (all accessed 2026-10-20)

- [S1] FFmpeg formats documentation, section "whip": https://ffmpeg.org/ffmpeg-formats.html ; download page https://ffmpeg.org/download.html
- [S2] RFC 9725 WebRTC-HTTP Ingestion Protocol (WHIP): https://www.rfc-editor.org/rfc/rfc9725.html
- [S3] libdatachannel README (MPL 2.0 since 0.18): https://raw.githubusercontent.com/paullouisageneau/libdatachannel/master/README.md
- [S4] RFC 3550 (spec SRC-16), RFC 6184 https://www.rfc-editor.org/rfc/rfc6184.html , RFC 8866 https://www.rfc-editor.org/rfc/rfc8866.html , RFC 3711 https://www.rfc-editor.org/rfc/rfc3711.html ; local run of FFmpeg 6.1.1 `-f rtp -sdp_file`.
- [S5] projectM source (shallow clone of https://github.com/projectM-visualizer/projectm, master dated 2026-10-01; tags via `git ls-remote --tags`): `COPYING`, `README.md`, `docs/building.rst`, `src/api/include/projectM-4/*.h`.
- [S6] Preset pack READMEs: https://raw.githubusercontent.com/projectM-visualizer/presets-cream-of-the-crop/master/README.md and `.../presets-milkdrop-original/master/README.md`; VLC commit mail https://mailman.videolan.org/pipermail/vlc-commits/2011-January/004936.html
- [S7] JUCE `8.0.16-security` tree (sha `ea77444750de45e036e0279bb0291144bf8e3688`): `CHANGE_LIST.md`, `modules/juce_opengl/opengl/juce_OpenGLContext.h`, `modules/juce_audio_devices/juce_audio_devices.h`, `docs/CMake API.md`.
- [S8] WCAG 2.2 SC 2.3.1: https://www.w3.org/WAI/WCAG22/Understanding/three-flashes-or-below-threshold.html ; macOS OpenGL deprecation: https://appleinsider.com/articles/18/06/04/opengl-opencl-deprecated-in-favor-of-metal-2-in-macos-1014-mojave
- [S9] GDPR Article 4: https://gdpr-info.eu/art-4-gdpr/
- [S10] BC Personal Information Protection Act definitions: https://www.bclaws.gov.bc.ca/civix/document/id/complete/statreg/03063_01
- [S11] MIDI Association, "MIDI 2.0 Coming to Windows 11": https://midi.org/midi-2-0-coming-to-windows-11
- [S12] Microsoft lifecycle, Windows Vista: https://learn.microsoft.com/en-us/lifecycle/products/windows-vista
- [S13] wxWidgets MSW install guides: https://raw.githubusercontent.com/wxWidgets/wxWidgets/v3.2.8/docs/msw/install.md and `.../master/docs/msw/install.md`
- [S14] DevClass, 14 Jan 2026 (pricing tiers, eligibility): https://www.devclass.com/security/2026/01/14/code-signing-windows-apps-may-be-easier-and-more-secure-with-new-azure-artifact-service/4079554
- [S15] Microsoft, "Code signing options": https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options
- [S16] SignPath Foundation conditions: https://signpath.org/terms and https://signpath.org/
- [S17] Apple Developer Program enrolment: https://developer.apple.com/programs/enroll/
- [S18] Apple, Developer ID: https://developer.apple.com/developer-id/
- [S19] Apple developer forums, Sequoia multicast: https://developer.apple.com/forums/thread/757824 (page text did not load for me; the title only), https://developer.apple.com/forums/thread/765285
- [S20] https://musicradar.com/news/bitwig-studio-four-point-three
- [S21] https://www.kvraudio.com/news/56611 ; https://www.reaper.fm/whatsnew.txt
- [S22] https://rekkerd.org/image-line-launches-fl-studio-2024-incl-clap-plugin-support/ ; https://forum.image-line.com/viewtopic.php?p=1955986
- [S23] https://github.com/free-audio/clap-juce-extensions (README)
- [S24] https://www.antarestech.com/blog/vst3-au-aax-clap-plugin-formats-2026
