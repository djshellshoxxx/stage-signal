# Handoff

Written 29 Oct 2026 at integration head `ed320f7436b187a3ef75458c9d5f8069bf49b86e`. Everything below was checked in the session that wrote it; earlier handoffs and PR text are history. Anything marked **unverified** was not run.

## 1. Where the code is

| Item | State |
|---|---|
| Integration branch | `claude/wonderful-franklin-kwdd6l` = PR [#15](https://github.com/djshellshoxxx/stage-signal/pull/15) (draft, base `main`). All work below is merged into it. **Nothing has been merged into `main`**; `main` is still `62db701`. Merging #15 into `main` is the owner's call. |
| Other open PRs | #13 `feat/app-contract` (old contract commit, superseded by #15), #14 `docs/readme-and-wiki`, #26 `claude/docs-screenshots` (README/CHANGELOG/TODO from another Claude session, based on `main`; its text says "no runnable UI (placeholder JUCE window)", which is out of date: refresh or close before merging). |
| Merged this effort (into #15) | #16 gap audit, #17 audio-loss policy (REQ-D09), #18 adopted `fix/macos-display-identity`, #19 third-party notices, #20 Bloom Pulse flash fix (NEW-02), #21 audience policy + published text (REQ-A09/S05/S06), #22 tempo/key overrides saved with the show (REQ-A06/A08), #23 opt-in true-peak limiter, #24 macOS Keychain, #25 operator controls. |
| Leftover branches | `claude/gap-audit`, `claude/ss-*` are merged and can be deleted. `claude/build-to-spec` and `claude/jolly-hawking-608dbp` pre-date this effort; not reviewed. |
| Git rules the owner set | work on `claude/` branches, draft PRs into the #15 branch, no force-push, no pushes to `main`, merge only on the owner's say-so, no new keys/tokens/certificates. |

## 2. How to rebuild and test (what was actually run)

Core (JUCE-free, ~2 min on 4 cores, 0 warnings):
```bash
cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4 && ctest --test-dir build --parallel 4 --output-on-failure
```
App (needs the pinned JUCE and clap-juce-extensions; `cmake/ss_deps.cmake` fetches them, or pass local checkouts):
```bash
# apt: libasound2-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev
#      libfreetype-dev libfontconfig1-dev libgl1-mesa-dev pkg-config xvfb
cmake -S . -B build-app -G Ninja -DSS_BUILD_APP=ON -DSS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DSS_JUCE_DIR=<JUCE ea77444750de45e036e0279bb0291144bf8e3688> -DSS_CLAP_EXT_DIR=<clap-juce-extensions 7adee3a1bd4684d4caa5601100e364abccff4b4d, with submodules>
cmake --build build-app --target StageSignal_Standalone StageSignal_CLAP StageSignal_VST3 --parallel 4
xvfb-run -a build-app/app/StageSignal_artefacts/Release/Standalone/StageSignal --selftest all   # one JSON line per test
```
`--selftest <name>` matches by prefix/substring, but names with spaces break through `xvfb-run`: run `all` and grep.

### Results at `ed320f7` (Linux x86-64, gcc, Release)
- Core: `ctest` **19/19 modules**. Doctest cases: audio 58, base 9, broadcast 63, compose 67, diag 30, doctor 19, dynamics 46, lighting 60, media 33, midi 59, model 1, palette 51, perf 52, persist 61, record 40, render 32, router 28, security 24, show 100 (833 total). `show` run 3x identical.
- App: builds Standalone, CLAP, VST3. `--selftest all`: **72 ok, 1 failed**. The failure is `shell: native idle has no Internet sockets`: this sandbox has no `/proc/net/tcp6`/`udp6`, so the check refuses to pass. It passes on the CI Linux runner. The test was not weakened.
- CI (GitHub Actions) was green on every PR head before it was merged (core tests Ubuntu/Windows/macOS, app compile macOS arm64 + x86_64, Linux app build with `--selftest all`). **CI for the final integrated head `ed320f7` had not completed when this was written**; check PR #15's checks. CI only runs on pull requests and pushes to `main`, so the integration branch is validated through PR #15.
- ASan+UBSan: dynamics tests ran clean (42 cases) with the true-peak limiter.
- **Not run anywhere**: Windows or macOS app runtime, any hardware (audio, MIDI, camera, DMX, displays), plugin host validation, live RTMPS/SRT, soak (4 h), A/V sync (1 h), BASE-tier (800 MHz) benchmark, assistive technology, signing/notarisation.

## 3. What exists (summary; per-requirement detail is `docs/GAP_AUDIT.md`)
Audit counts when it was written: 72 REQ rows = 47 done, 23 partial, 2 missing; the merges above moved A06, A08, A09, D09, S05, S06 and NEW-02 to done/closer (the audit file carries the corrections).

- **Portable core (19 modules)**: analysis, dynamics, show engine, persistence, security, diagnostics, lighting (Art-Net/sACN + USB-DMX frame builders), MIDI, router, render, compose, perf, media rules, record, broadcast, doctor, palette. Unit-tested.
- **App**: JUCE standalone + CLAP + VST3 shell, real audio/MIDI/UDP/ffmpeg-supervisor/secret-store adapters (Linux-tested only), operator UI, audience windows, media/record/broadcast workers.
- **Added this effort**: audio-loss/idle policy; per-output BPM/key overlay and published text (the app previously never fed the compositor policy or `TextLayer`); overrides saved with the show; opt-in true-peak limiter; Bloom Pulse rate limit (real defect: up to 60 flashes/s with an onset every frame); macOS Keychain; third-party notices; operator controls with validated `SetPref`.

## 4. Things a new person must know
- **True-peak limiter** (`DynamicsConfig::truePeakLimiter`, pref `dynamics.truepeak=1`, read at start): validated on synthetic signals against an independent reference meter, **not on music**. Aims 0.45 dB under the set ceiling; full-band loud noise reads about +0.06 dB over it. 31 samples extra latency. About 2.8 % of a modern core (not measured on BASE). The app does not apply the dynamics processing delay to recorded audio timestamps (existing gap, REQ-R03).
- **macOS Keychain** is compiled by CI on both macOS architectures but never run against a real keychain. Windows/Linux stores are also unverified at runtime.
- **BPM/key overlay toggle** (`ToggleOverlayBpmKey`) changes the router and render policy at runtime only: it is not saved in the show file and the Setup row still draws "off".
- Prefs keys are lowercase only (`[a-z0-9._-]`); `SetPref` has a whitelist plus per-key value checks in `AppShell.cpp`.
- Stream keys are still on the ffmpeg command line (P-10). USB-DMX has no serial transport (core frame builders only; `NullSerialDmx` in the app).
- Display identity: native providers exist (Windows/Linux/macOS code from #18), CI-compiled; identical monitors can still collide.
- Licence: the owner's 22 Oct direction (in `docs/spec-addendum-v0.2.md` and `docs/DISTRIBUTION.md`) is the commercial JUCE licence and a VST3 + CLAP matrix; no project source licence is granted in the repo. Not a legal review. `THIRD-PARTY-NOTICES.md` indexes pins and licences.

## 5. Remaining work, in priority order
1. Run PR #15's CI on the integrated head; fix anything it finds; then the owner decides on merging #15 into `main`. Refresh or close #26 first.
2. Manual verification the code cannot give itself: Keychain round trip on a Mac; camera, displays, MIDI, audio device loss on Windows/macOS; ffmpeg video record and a live broadcast test; A/V sync rig; 4 h soak per OS; BASE-tier benchmark; screen readers.
3. Show the BPM/key overlay state in Setup and save it in the show file; controls for `text.y` / `text.size`.
4. Apply the dynamics processing delay to recorded audio timestamps (REQ-R03) with a test.
5. Animated GIF / video playback in the app is only fake-ffmpeg tested (`ClipDecoder`); camera adapter is compile-only on Windows/macOS.
6. Serial USB-DMX transport (`termios`/Win32 behind `ISerialDmx`, testable with a pty) and engine/model/UI wiring, once the owner names the first adapter model.
7. Stream-key handling that keeps the key off the command line (P-10); BS.1770 conformance and a music/codec corpus for the limiter; RTP (REQ-B07, P2); stale `docs/specs/app-contract.md` (still labels adapters as STUB).

## 6. Decisions only the owner can make
Merge #15 into `main`; which USB-DMX adapter to support first (Q-03); a licensed or self-made music corpus for tempo/key and limiter qualification; signing/notarisation budget; whether #26 is wanted; real hardware access for section 5.2.

## 7. Token/cost notes (from this effort, not measured per feature)
Largest remaining token consumers: video/GIF playback end to end, recording/broadcast hardening, cross-platform camera, UI/accessibility passes, packaging and signing. Cheap: items 3, 4 and the doc refreshes above.
