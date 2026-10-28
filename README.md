# StageSignal

StageSignal is an audio-reactive visuals and show-control tool for live performers: it analyses music (tempo, key, energy), drives software-rendered scenes, DMX lighting (Art-Net, sACN, USB DMX), MIDI controllers, recording and broadcast. It is built as a portable, JUCE-free C++17 **core** plus a thin JUCE **app** (standalone and CLAP plugin).

## Status

Early development (version 0.1.0). The portable core under `core/` is implemented and unit-tested (19 test suites pass). The JUCE app layer is still a placeholder window (`app/Source/Plugin`); the operator UI, audio capture, display management and output wiring are not built yet. See `docs/wiki/Roadmap-and-Deferred-Work.md`.

## Screenshots

None yet. There is no operator UI to capture: the app is a placeholder window, and the CLAP plugin needs a DAW host. Screenshots will be added once the UI exists.

## Layout

| Path | Contents |
|---|---|
| `core/<module>/` | Portable C++17 modules (base, model, audio, dynamics, show, persist, security, diag, lighting, midi, router, render, compose, perf, media, record, broadcast, doctor, palette) |
| `tests/unit/<module>/` | doctest unit tests, one directory per module |
| `app/` | JUCE standalone and CLAP plugin (`SS_BUILD_APP=ON`) |
| `docs/` | Architecture, per-module specs (`docs/specs`), traceability, known limitations, wiki sources (`docs/wiki`) |
| `StageSignal-Specification-v0.1.md` | Baseline specification; changes in `docs/spec-addendum-v0.2.md` |

## Build and test (core)

Requires CMake 3.22+, Ninja and a C++17 compiler.

```sh
cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

The JUCE app additionally needs JUCE and clap-juce-extensions; see `docs/ARCHITECTURE.md` and `.github/workflows/ci.yml` for the exact configure line and Linux dependencies.

## Documentation

- `docs/ARCHITECTURE.md` - layout, dependency rules, working rules
- `docs/KNOWN_LIMITATIONS.md` - what is not proven or not done
- `docs/TRACEABILITY.md` - requirement to test mapping
- `docs/wiki/` - wiki page sources (see `docs/wiki/README.md` for publishing)
- `CHANGELOG.md`, `TODO.md`

## License

Intended license is AGPL-3.0-or-later (JUCE AGPL option), per DEC-05 in `docs/spec-addendum-v0.2.md`. The owner has not yet confirmed it and no `LICENSE` file is committed yet.
