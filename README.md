# StageSignal

StageSignal is a cross-platform live-performance visualiser, video mixer, show controller, recorder and lighting controller for DJs, bands and small venues. The operator works on the primary screen; the audience sees a separate output. Audio, camera, text, imported media, MIDI, lighting, recording and optional broadcasting form one operator workflow.

## Status

- **The specification defines the acceptance targets.** [`StageSignal-Specification-v0.1.md`](StageSignal-Specification-v0.1.md) (version 0.1, revision 1) is a design baseline for estimation and feasibility work. Its numerical limits are proposed acceptance targets, not measured capabilities. It records that no hardware benchmark, protocol interoperability test or legal clearance has been performed.
- **The repository also contains code.** Portable core modules live under `core/` (audio, base, broadcast, compose, diag, doctor, dynamics, lighting, media, midi, model, palette, perf, persist, record, render, router, security, show), and the JUCE application in `app/` connects them through desktop adapters, the operator UI, persistence and the media worker.
- **Qualification remains incomplete:** hardware interoperability, long-show soak, real-music accuracy and assistive-technology checks remain open in [Known limitations](docs/KNOWN_LIMITATIONS.md). Automated checks do not establish these results.

## Build and test (portable core)

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DSS_BUILD_TESTS=ON -DSS_BUILD_APP=OFF
cmake --build build-core -j 2
ctest --test-dir build-core --output-on-failure
```

The JUCE app is built with `-DSS_BUILD_APP=ON`; see `cmake/ss_deps.cmake`. Continuous integration is in `.github/workflows/ci.yml`.

## Native applications and plugins

The native beta workflow builds standalone, VST3 and CLAP artifacts for Windows x64, macOS arm64, macOS x86_64 and Linux x64. It runs core tests and Linux application functional self-tests before publishing a prerelease. Each target must be built on its native runner; see [Building](docs/wiki/Building.md). Windows binaries are unsigned and macOS bundles are ad-hoc signed, without notarisation.

For a local JUCE build:

```sh
cmake -S . -B build-app -DSS_BUILD_APP=ON -DSS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-app -j 2 --target StageSignal_Standalone StageSignal_VST3 StageSignal_CLAP
# Linux, with Xvfb installed
xvfb-run -a build-app/app/StageSignal_artefacts/Release/Standalone/StageSignal --selftest all
```

## Specification

Sections: executive summary and definition; inputs and decisions; users and journeys; feature inventory; functional requirements; system and hardware compatibility; performance and adaptive quality; architecture and data flows; module catalogue; function, API and protocol contracts; UI; audio analysis and recording; visuals and assets; MIDI and lighting; broadcasting; security, privacy and licensing; show-file lifecycle; testing; build and release; implementation plan; traceability; open-source reuse; risks; glossary and bibliography.

## Licence

No licence file is present, so no licence is granted by this README. The specification references open-source components; review their licences before reuse (specification section 22).

## Wiki

Wiki source pages are in [docs/wiki/](docs/wiki/Home.md): developer guide, architecture summary and codebase map.
