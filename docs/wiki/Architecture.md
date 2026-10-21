# Architecture

Source of truth: `docs/ARCHITECTURE.md`, `docs/specs/*.md` and `cmake/ss_module.cmake`. This page summarises them. If they disagree, the repository wins.

## Shape of the system

```
+--------------------------- app/ (JUCE, not yet wired) ---------------------------+
|  Plugin (AudioProcessor, shared by standalone and CLAP)                           |
|  UI  (operator window, audience windows)   Engine (platform adapters)             |
|  App (AppController: owns threads, wires core modules together)                   |
+----------------------------------------|------------------------------------------+
                                         v
+-------------------------- core/ (portable C++17, no JUCE) -------------------------+
|  base (ids, types, queues, clock, rng, sha256, utf8, errors)                      |
|   +-- model     show document data (header only)                                  |
|   +-- security  redaction, archive checks, endpoint rules, worker limits          |
|   +-- dynamics  compressor, limiter, true-peak meter, WAV segment writer          |
|   +-- show      sole writer of show state: modes, cues, blackout, undo, snapshots  |
|   +-- render    12 scenes, 2 effects, raster and fit helpers                      |
|   +-- compose   compositor, camera layer, text layer, overlays, media timing       |
|   +-- lighting  Art-Net and sACN packets, fixture resolver, lighting engine       |
|   +-- persist   show JSON, validation, migrations, atomic save, autosave, bundles  |
|   +-- diag      event journal, feature sampler, log rotation, support bundle, report |
+-----------------------------------------------------------------------------------+
```

## Allowed dependencies

Every module may use `base` and `model`. The extra edges are:

| Module | May also depend on | As built in CMake today |
|---|---|---|
| `persist` | `security` | `base`, `model`, `security` |
| `diag` | `security` | `base`, `model`, `security` |
| `show` | `audio` (planned; not in the repo) | `base`, `model` |
| `record` (planned) | `dynamics` | n/a |
| `broadcast` (planned) | `security` | n/a |
| `doctor` (planned) | `perf`, `router`, `record`, `lighting` | n/a |
| `palette` (planned) | `show` | n/a |

`dynamics`, `security`, `render`, `compose` and `lighting` depend only on what the table shows. A new edge needs a note from the integrator in `docs/ARCHITECTURE.md`.

Note: `compose` is in the repository and its CMake deps (`base`, `model`) fit the rules, but it is not in the module list in `docs/ARCHITECTURE.md`. The integrator should add it.

## Ownership

- Show state has one writer: the `show` engine, run on the control thread. Other threads send `ss::Action` values through a bounded queue (256 items).
- Safety-critical actions (`isSafetyCritical` in `core/base/include/ss/base/actions.hpp`) are applied at submit time and are never dropped.
- Core modules create no threads. The app owns threads.

## Threads (planned app design)

From `docs/specs/app-shell.md`:

| Thread | Work | Must never |
|---|---|---|
| Audio (OS or host) | Copy, sanitise, timestamp, enqueue | Allocate, lock, log, touch another core |
| Analysis | Audio and dynamics state | File, socket, UI |
| Control | Show engine, router, palette, perf state | UI, device handles, blocking I/O |
| Render | Frame production at 25 to 30 FPS | Decode, disk, devices |
| Lighting | Art-Net and sACN at 25 Hz (max 40) | Read raw audio |
| UI | Widgets, status at 30 Hz | Block on a worker's mutex |

## Rules every module follows

1. Real-time paths allocate nothing after `prepare()`. No locks, no I/O.
2. Times are `ss::Ns` (int64 nanoseconds). Sample maths uses `samplesToNs`. Use `IClock`.
3. Failures are `ss::Result<T>` or `ss::Status` with an `ErrorCategory` (InvalidArgument, UnsupportedCapability, PermissionDenied, DeviceLost, TimestampInvalid, ResourceExhausted, DecodeFailure, EncoderUnavailable, DiskFull, TransportFailure, SchemaUnsupported, Cancelled, InternalFault). No exceptions across modules. No secrets or user paths in error text.
4. Determinism: anything seeded uses `ss::Rng`. Same seed and inputs give identical output on every platform.
5. Bounds are checked before allocation. Each module's `limits.hpp` holds the numbers.
6. Every acceptance criterion a module owns has a doctest named `REQ-XYZ: ...`. Tests are deterministic, use no sleeps, no network and no devices.
7. Commit and PR trailers are in [Contributing](Contributing).

## Determinism in practice

The show engine is driven only by the time passed to `tick()`, so a test with a fake clock reproduces a show exactly. Auto picks use `Rng` seeded from the show document and consume randomness only when a pick is made. The same document and feature stream give the same sequence on every platform.

## Where the spec mapping lives

Module names in the spec (`MOD-01` to `MOD-16`) map to core modules as follows: `show` = MOD-04, `persist` = MOD-13, `security` = MOD-15, `diag` = MOD-16, `lighting` = MOD-10, `render` = MOD-06, `compose` = the compositing half of MOD-06, `dynamics` = the dynamics half of MOD-03. Planned and not in the repo: `audio` (MOD-03 analysis), `midi` (MOD-09), `router` (MOD-05), `media` (MOD-07), `record` (MOD-11), `broadcast` (MOD-12), `perf` (MOD-14), `doctor`, `palette`.
