# Testing and CI

Two workflows live in `.github/workflows/`. Neither has been run from this wiki work, so treat the details below as the config in the repo, not as proof of a green run.

## Last local run

On the wiki build machine (Linux, default `c++` compiler, CMake with Ninja, `SS_BUILD_APP=OFF`, Release) every core suite built with no compiler warnings in the build log. `ctest` ran 10 of 10 suites green, and doctest reports 388 test cases passing:

| Suite | Test cases |
|---|---|
| base | 9 |
| compose | 67 |
| diag | 30 |
| dynamics | 38 |
| lighting | 60 |
| model | 1 |
| persist | 58 |
| render | 28 |
| security | 24 |
| show | 73 |

This is one machine, one compiler. The module specs say MSVC and macOS were not run for every module. CI covers those in `ci.yml`.

## Test layers

| Layer | Where | What it proves |
|---|---|---|
| Core unit tests | `tests/unit/<module>/` (doctest) | Behaviour of each portable module, including REQ acceptance criteria named `REQ-XYZ: ...` |
| Golden tests | `tests/unit/render/golden/frames.txt`, lighting packet goldens in `test_packets.cpp` | Bytes and frames match checked-in references |
| Allocation tests | `test_alloc.cpp` in render and compose | No heap use on the hot paths after `prepare()` |
| Fault tests | `persist`, `show`, `lighting` | Disk full, torn writes, disconnects, bad input |
| App smoke test | `ci.yml` (`app-linux`) | The binary stays alive for 8 seconds under Xvfb |
| Hardware and soak | Manual | Not in CI (see [Known Limitations](Known-Limitations)) |

## ci.yml

Runs on push to `main` and on pull requests. Concurrency cancels older runs of the same ref.

| Job | Runs on | Steps |
|---|---|---|
| `core` | ubuntu, windows and macos (matrix, fail-fast off) | Configure with `SS_BUILD_APP=OFF`, build, `ctest` |
| `app-linux` | ubuntu, after `core` | Install ALSA and X11 deps, build `StageSignal_Standalone` and `StageSignal_CLAP`, run the standalone under `xvfb-run` with `timeout 8` and require exit code 124 (still alive) |

ccache is used for the app jobs.

## beta.yml

Manual trigger only (`workflow_dispatch`). The input `tag` defaults to `v0.1.0-beta.1`.

| Job | Purpose |
|---|---|
| `test` | Configure, build and run the core tests once, on Linux |
| `build` (matrix) | Build from the same commit, no tests repeated. Targets: `windows-x64`, `macos-arm64`, `macos-x86_64`, `linux-x64`. Each packages its files. |
| `release` | Download all packages, write `SHA256SUMS.txt`, publish a prerelease with the tag and the notes from `docs/RELEASE_NOTES_BETA.md` |

Packages:

| Platform | Files |
|---|---|
| Windows | `StageSignal-<tag>-windows-x64-standalone.zip` (exe), `...-clap.zip` (`StageSignal.clap`) |
| macOS | `...-standalone.zip` (ad-hoc signed `.app`), `...-clap.zip` (ad-hoc signed `.clap`) |
| Linux | `...-standalone.tar.gz`, `...-clap.tar.gz` |

macOS builds use `CMAKE_OSX_DEPLOYMENT_TARGET=10.15`. The builds are ad-hoc signed with `codesign -s -`, not notarised.

The workflow needs `contents: write` to publish the release.

## Release checklist

1. Finish `docs/KNOWN_LIMITATIONS.md`. Each item gets a status and a date.
2. Check the notes in `docs/RELEASE_NOTES_BETA.md`.
3. Make sure `main` is green in `ci.yml`.
4. Run `beta.yml` with a new tag, for example `v0.1.0-beta.2`.
5. Download one package per platform and run the checks in [Getting Started](Getting-Started).
6. Verify `SHA256SUMS.txt`.
7. Update the Evidence column in `docs/TRACEABILITY.md` with the run link. Do not mark anything `Implemented` on a spec-only change.

## Known gaps in CI

- The app job checks that the binary stays alive. It does not test audio, UI, lighting or recording, because those are not built.
- No hardware runners. No soak runs.
- Core tests run on hosted runners only.
