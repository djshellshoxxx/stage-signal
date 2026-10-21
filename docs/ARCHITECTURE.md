# StageSignal architecture and working rules (v0.2)

StageSignal = portable C++17 **core** (`core/*`, no JUCE, no OS headers) + thin JUCE **app** (`app/`)
that ships as a standalone app (Windows, macOS arm64+x86_64, Linux) and a CLAP plugin. Requirement IDs
(REQ-*), module IDs (MOD-*), interface IDs (IF-*) and test families (T-*) come from
`StageSignal-Specification-v0.1.md` and are stable. Changes to the baseline are in `docs/spec-addendum-v0.2.md`.

## Layout and ownership
```
core/<module>/include/ss/<module>/*.hpp   public headers (namespace ss::<module>)
core/<module>/src/*.cpp                   implementation (globbed)
core/<module>/CMakeLists.txt              one line: ss_add_module(<module> DEPS base ...)
tests/unit/<module>/{CMakeLists.txt,*.cpp} doctest tests; CMakeLists: ss_add_test(<module> DEPS <module> ...)
docs/specs/<module>.md                    implementable spec for the module (owner writes/finishes it)
app/Source/{Plugin,UI,Engine,App}/        JUCE layer
```
Modules and spec mapping: base (shared types), audio (MOD-03 analysis + Energy Arc), dynamics (MOD-03 dynamics,
audio bus, WAV writer), show (MOD-04), persist (MOD-13), security (MOD-15), diag (MOD-16 + Show Report),
lighting (MOD-10), midi (MOD-09), router (MOD-05 logic), render (MOD-06), perf (MOD-14), media (MOD-07 rules),
record (MOD-11 session logic), broadcast (MOD-12 session logic), doctor (Show Doctor), palette (action registry + Command Palette).

The root `CMakeLists.txt` auto-discovers `core/*/CMakeLists.txt` and `tests/unit/*/CMakeLists.txt`.
**Never edit root CMake, `core/base`, or another module's files** (open an issue note in your PR instead).
A module may depend only on `base` and modules named in its `DEPS`. Allowed dependency edges:
`audio,dynamics,midi,lighting,persist,security,diag,render,media,perf,router -> base`;
`show -> base,audio`; `record -> base,dynamics`; `broadcast -> base,security`; `doctor -> base,perf,router`;
`palette -> base,show`. Anything else needs a note in ARCHITECTURE.md from the integrator.

## Rules every module follows
1. Real-time paths (audio DSP, render hot loops) allocate nothing after `prepare()`; no locks, no I/O.
2. All times are integer nanoseconds (`ss::Ns`); sample math uses `samplesToNs`. Use `IClock`, never a global clock.
3. Failures return `ss::Result<T>`/`ss::Status` with an `ErrorCategory`; no exceptions across module boundaries,
   no `assert` for runtime conditions, no secrets or user paths in error text.
4. Determinism: anything seeded uses `ss::Rng`. Same seed + inputs => identical output on every platform.
5. Bounds are checked **before** allocation (spec section 13/17 limits live in each module's `limits.hpp`).
6. Tests: every REQ acceptance criterion the module owns has a doctest case named `REQ-XYZ: ...`. Tests are
   deterministic, no sleeps, no network, no real devices, finish in seconds.
7. Commit messages end with the Co-Authored-By and Claude-Session lines given in the task; PR bodies end with the
   Claude Code line given in the task.

## App layer
`app/Source/Plugin` = AudioProcessor/Editor shared by standalone and CLAP. `app/Source/Engine` = adapters that turn
platform I/O into core types (audio capture -> IF-01, MIDI, UDP lighting transport, display manager, camera,
WAV/ffmpeg writers, secrets). `app/Source/UI` = operator UI, audience windows, wizard. `app/Source/App` = the
`AppController` that owns threads and wires core modules together (single show-state writer = `show`).
Callbacks never call UI, DMX, MIDI-out, filesystem or network directly (spec section 8).

## Build and test
```
cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-app -G Ninja -DSS_BUILD_APP=ON -DSS_JUCE_DIR=<juce> -DSS_CLAP_EXT_DIR=<clap-juce-extensions>
```
CI (`.github/workflows`): `ci.yml` runs core tests once per OS; `beta.yml` builds each platform in its own job
from a single test-gated commit (no repeated tests) and publishes the beta release.
