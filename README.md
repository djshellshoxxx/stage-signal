# StageSignal

StageSignal is planned as a Windows live-performance visualiser, video mixer, show controller, recorder and lighting controller for DJs, bands and small venues. The operator works on the primary screen; the audience sees a separate output. Audio, camera, text, imported media, MIDI, lighting, recording and optional broadcasting form one operator workflow.

## Status

- **The specification is the current deliverable.** [`StageSignal-Specification-v0.1.md`](StageSignal-Specification-v0.1.md) (version 0.1, revision 1) is a design baseline for estimation and feasibility work. Its numerical limits are proposed acceptance targets, not measured capabilities. It records that no hardware benchmark, protocol interoperability test or legal clearance has been performed.
- **The repository also contains code.** Portable core modules live under `core/` (audio, base, broadcast, compose, diag, doctor, dynamics, lighting, media, midi, model, palette, perf, persist, record, render, router, security, show), and a JUCE app skeleton is in `app/`.
- **Verified here (Linux):** the portable core configures, builds and passes its 19 CTest tests. The JUCE app was not built.

## Build and test (portable core)

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DSS_BUILD_TESTS=ON -DSS_BUILD_APP=OFF
cmake --build build-core -j 8
ctest --test-dir build-core --output-on-failure
```

The JUCE app is built with `-DSS_BUILD_APP=ON`; see `cmake/ss_deps.cmake`. Continuous integration is in `.github/workflows/ci.yml`.

## Specification

Sections: executive summary and definition; inputs and decisions; users and journeys; feature inventory; functional requirements; system and hardware compatibility; performance and adaptive quality; architecture and data flows; module catalogue; function, API and protocol contracts; UI; audio analysis and recording; visuals and assets; MIDI and lighting; broadcasting; security, privacy and licensing; show-file lifecycle; testing; build and release; implementation plan; traceability; open-source reuse; risks; glossary and bibliography.

## Licence

No licence file is present, so no licence is granted by this README. The specification references open-source components; review their licences before reuse (specification section 22).

## Wiki

Wiki source pages are in [docs/wiki/](docs/wiki/Home.md): developer guide, architecture summary and codebase map.
