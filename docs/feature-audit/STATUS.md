# StageSignal v0.1 — build status and feature audit (vs spec v0.1)

Branch `claude/jolly-hawking-608dbp`. Repo began with spec only (no code). Windows packages: `releases/StageSignal-0.1.0-x64.zip`, `releases/StageSignal-0.1.0-x86-base.zip` (i586, no SSE).
Verified: clean native build, 47/47 ctest; both exes cross-built; x64 `--selftest/--version/--benchmark` and 6-tab GUI screenshots run under wine. **Nothing run on real Windows, real devices, or the 800 MHz/512 MB fixture; x86 exe never executed.**

Legend: DONE = implemented + tested (portable/mock); ADAPTER = code exists, unrun on hardware; GAP = absent.

| Area (REQ) | Status | Notes |
|---|---|---|
| Audio capture A01/A02 | ADAPTER | WASAPI + WinMM fallback, no silent device switch; untested on devices |
| Analysis A03–A10 | DONE | bands/RMS/onset/BPM/key/clip tests pass; key thresholds provisional; no music corpus |
| Tap/manual BPM, key A06/A08 | DONE | show engine |
| Displays/outputs D01–D11 | DONE(mock)/ADAPTER | router + Win32 audience windows; no multi-monitor hardware test |
| Show/auto/programmed/hybrid S01–S05,S12 | DONE | seeded, override/resume, strobe disable |
| Media S07,S08,S15,X03 | DONE | stills/GIF (own disposal handling), manifest+SHA-256; in-process worker (not separate process) |
| Video decode S07 | ADAPTER | MF reader; none bundled/untested |
| Camera S09–S11 | ADAPTER | MF capture; untested on cameras |
| Persistence S13,S14,X01 | DONE | atomic saves, 3 generations, hostile-zip tests |
| MIDI M01–M04 | DONE(core)/ADAPTER | WinMM port unrun on hardware |
| Lighting L01–L05 | DONE(core) | Art-Net/sACN golden packets, arm/blackout; sACN sync not implemented |
| USB-DMX L06 | GAP/partial | ENTTEC framing only, explicitly unqualified; none qualified |
| Perf/benchmark P01–P06 | DONE(logic) | gates/adaptation/admission tested with fakes; **BASE hardware gates not measured (REQ-P06 open)** |
| Recording R01–R06 | DONE(core) | WAV, AVI/MKV MJPEG, H.264-in-MKV w/ fake encoder; MF H.264/AAC never executed; sync 1h simulated only |
| Dynamics R02 | DONE | sample-peak limiter; true-peak mode not implemented |
| Broadcast B01–B07 | PARTIAL | RTP (MJPEG/L16) + RTMP/RTMPS client vs mock server only; Schannel/live untested; **SRT absent** (UnsupportedCapability) |
| Security/privacy X04, B03 | DONE | redaction, path/protocol allowlists, no default listeners |
| Diagnostics X05 | DONE | journal, rotation, support bundle |
| Soak X06 | GAP | 4 h/100-cycle soak not run on hardware |
| UI §11 | ADAPTER | Win32 UI built; layout seen only under wine |
| projectM/GL, extra enhanced items, WebRTC, audience moments (M7) | GAP | deferred/optional per spec |
| M0 gates (hardware fixture, OS/toolchain, license decision Q-01/02/04) | OPEN | owner decisions; no legal clearance claimed |

Known deviations from the provisional stack (spec §1): native Win32 + GDI instead of wxWidgets; own Art-Net/sACN instead of ETCLabs; no FFmpeg/projectM/miniaudio/RtMidi.
Hardware-only test families (T-AUD/DISPLAY/CAM/MIDI/DMX/SYNC/SOAK/PERF on BASE) remain to be run per §18.
