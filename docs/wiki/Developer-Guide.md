# Developer Guide

StageSignal is a portable C++17 core with a thin JUCE app on top. The core has no JUCE and no OS headers. It is built and tested on its own, which is why most of the work can be checked on a Linux box with CMake and Ninja.

## Layout

```
core/<module>/include/ss/<module>/*.hpp   public headers, namespace ss::<module>
core/<module>/src/*.cpp                   implementation (globbed)
core/<module>/CMakeLists.txt              one line: ss_add_module(...)
tests/unit/<module>/                      doctest tests, one test_<module> executable
docs/specs/<module>.md                    the spec for that module, with an Integration API section
app/Source/{Plugin,UI,Engine,App}/        JUCE layer (standalone and CLAP share the processor)
third_party/                              doctest and nlohmann/json, vendored
cmake/                                    ss_add_module, ss_add_test, pinned JUCE and clap-juce-extensions
docs/                                     architecture, worker rules, addendum, traceability, limitations
```

The root `CMakeLists.txt` finds every `core/*/CMakeLists.txt` and `tests/unit/*/CMakeLists.txt` by itself. You do not edit the root file to add a module.

## Where to start

| If you want to... | Read |
|---|---|
| Understand the module graph and the rules | [Architecture](Architecture) |
| Build and run the tests | [Building](Building) |
| Know what CI checks and how a beta ships | [Testing and CI](Testing-and-CI) |
| Learn what a module exposes | [Module Reference](Module-Reference-base) |
| Add a render effect | [Adding an Effect](Adding-an-Effect) |
| Add a button or MIDI target | [Adding an Action](Adding-an-Action) |
| Touch save files | [Show File Format and Migration](Show-File-Format-and-Migration) |
| Touch input handling or files | [Security Model](Security-Model) |
| Open a PR | [Contributing](Contributing) |

## Status of the code

| Layer | State |
|---|---|
| Core modules `base`, `model`, `show`, `render`, `compose`, `lighting`, `persist`, `security`, `diag`, `dynamics` | Implemented in portable code with unit tests. Built and tested on Linux in the module specs. |
| Core modules listed in the spec but not in the repo: `audio`, `midi`, `router`, `media`, `record`, `broadcast`, `doctor`, `palette`, `perf` | Not started in `main`. |
| `app/` | A placeholder processor and an editor that draws the name. Audio, UI, lighting, recording and file dialogs are not wired yet. |

## Rules you will hit first

- No exceptions across public APIs. Return `ss::Result<T>` or `ss::Status` with an `ErrorCategory`.
- Times are integer nanoseconds (`ss::Ns`). Use `IClock`, not a global clock.
- Real-time code allocates nothing after `prepare()`. No locks, no I/O.
- Seeded randomness uses `ss::Rng`, so the same seed and input give the same output everywhere.
- Limits are checked before allocation.

Full list in [Architecture](Architecture) and [Contributing](Contributing).

## Specs first

Each module has a spec in `docs/specs/`. The worker rules ask for the spec before the code. The spec states the requirement IDs, the public API, the limits, the tests, and an honest Status section. When you change behaviour, change the spec and the traceability row in the same PR.

Requirement IDs (`REQ-*`), module IDs (`MOD-*`) and interface IDs (`IF-*`) come from `StageSignal-Specification-v0.1.md`, and the addendum (`docs/spec-addendum-v0.2.md`) changes some of them. IDs are stable. Do not renumber them.
