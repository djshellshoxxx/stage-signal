# Handoff

Updated 28 Oct 2026. Earlier handoffs are history; everything below was checked in this session.

## Repo state
- Base: PR #15 head `d4aaab297a7366317074441040517835038082bc` (`claude/wonderful-franklin-kwdd6l`), draft, base `main` (`62db701`).
- Open PRs: #13 `feat/app-contract` (non-draft), #14 `docs/readme-and-wiki` (draft), #15 (draft), #16 `claude/gap-audit` (draft, base = PR #15 branch).
- Unmerged branch with real extra work, **not reviewed**: `fix/macos-display-identity` (native display identity, moving media, camera publication, VST3 matrix entry that conflicts with DEC-11).

## Done this session
| Branch | What | Pushed SHA |
|---|---|---|
| `claude/gap-audit` | `docs/GAP_AUDIT.md` | see PR #16 |
| `claude/ss-audio-health` | REQ-D09 audio-loss/idle policy: core monitor + engine + app wiring + tests | see below |

## Tests actually run (this machine, Linux x86-64, gcc, Release)
- Core: `cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF` + full build: 0 warnings; `ctest`: 19/19 modules pass (also after the D09 slice). `test_show` 95/95 (85 + 10 new), run 3x.
- App: configured with JUCE `ea77444` and clap-juce-extensions `7adee3a` (the repo pins), built `StageSignal_Standalone` and `StageSignal_CLAP`; `xvfb-run StageSignal --selftest all`: **53/53 ok** (52 existing + `shell: audio loss idle`). The app build prints JUCE-flag `-Wfloat-equal` warnings from `core/model/show_model.hpp` (pre-existing, not suppressed).
- Not run: Windows/macOS builds (CI only), any hardware, soak, interop, assistive technology.

## Remaining (priority order, see GAP_AUDIT.md section 6)
1. REQ-S16 offline self-test; REQ-X04 real socket enumeration.
2. Root `THIRD-PARTY-NOTICES`; refresh stale `docs/specs/app-contract.md`.
3. REQ-S05 core draft/publish test; NEW-02 flash-rate test; REQ-A06/A08 "save/load preserves override" (needs a model/persist field).
4. Moving-media decode; camera adapter; macOS Keychain.

## Blockers needing the owner
- DEC-05 licence confirmation; VST3 in the release matrix vs DEC-11; which USB-DMX adapter (Q-03); whether to adopt `fix/macos-display-identity`; true-peak limiter qualification corpus.
