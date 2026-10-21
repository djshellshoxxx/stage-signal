# Codebase Map

| I want to... | Look in |
|---|---|
| Portable core modules | `core/` (each module has `include/ss/<module>/` and `src/`) |
| Base utilities (queues, RNG, SHA-256, time, errors, actions) | `core/base/` |
| Audio analysis (FFT, tempo, key, energy arc, analyser) | `core/audio/` |
| JUCE app (plugin processor and editor) | `app/Source/Plugin/` |
| Build helpers | `cmake/ss_module.cmake`, `cmake/ss_deps.cmake` |
| Specification | `StageSignal-Specification-v0.1.md` |
| CI and beta workflow | `.github/workflows/` |
