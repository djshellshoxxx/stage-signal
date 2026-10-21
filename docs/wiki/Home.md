# StageSignal Wiki

StageSignal is a live visuals, lighting and recording app for DJs and bands, made by Circuit Drift Labs. It listens to the room (or to a DAW track when running as a CLAP plugin), follows tempo, key and energy, and drives screens, DMX lighting, a text panel and optional recordings and streams from one operator window.

> **Project status (October 2026):** StageSignal is in the specification and core-library stage. The portable C++17 core (`core/*`) is built and unit-tested on Linux. The JUCE operator UI, audio capture, lighting output and recording wiring are still being built. The app on `main` today is a placeholder window. Pages on this wiki say which parts are done and which are planned. Nothing here claims a feature works until it is marked done.

## For operators

| Page | What is in it |
|---|---|
| [Getting Started](Getting-Started) | Install per platform, unsigned beta notes |
| [Quick Tour](Quick-Tour) | The operator window, tab by tab |
| [Audio and Analysis](Audio-and-Analysis) | Tempo, key, energy arc, input setup |
| [Scenes and Effects](Scenes-and-Effects) | The scene catalogue, Prism Mirror, Bloom Pulse |
| [Show Control](Show-Control) | Modes, cues, undo/redo, A/B snapshots |
| [MIDI and Controllers](MIDI-and-Controllers) | Learn, mapping, clock out |
| [Command Palette and Shortcuts](Command-Palette-and-Shortcuts) | Ctrl+K and the key map |
| [Lighting and DMX](Lighting-and-DMX) | Art-Net, sACN, arming, blackout |
| [Displays and Outputs](Displays-and-Outputs) | Audience windows, roles, identify |
| [Media and Text](Media-and-Text) | Imported media, live text panel |
| [Recording and Streaming](Recording-and-Streaming) | WAV, Matroska, FFmpeg, broadcast |
| [Show Doctor and Troubleshooting](Show-Doctor-and-Troubleshooting) | Preflight checks and common problems |
| [Show Report](Show-Report) | Setlist and energy export |
| [Safety and Blackout](Safety-and-Blackout) | Strobe lock, photosensitivity, blackout limits |
| [FAQ](FAQ) | Short answers |
| [Known Limitations](Known-Limitations) | What is not done or not proven |

## For developers

| Page | What is in it |
|---|---|
| [Developer Guide](Developer-Guide) | Overview and where to start |
| [Architecture](Architecture) | Module graph, allowed dependencies, real-time rules |
| [Building](Building) | Core tests, app build, pinned dependencies |
| [Testing and CI](Testing-and-CI) | `ci.yml`, `beta.yml`, release flow |
| Module Reference: [base](Module-Reference-base), [model](Module-Reference-model), [show](Module-Reference-show), [render](Module-Reference-render), [compose](Module-Reference-compose), [lighting](Module-Reference-lighting), [persist](Module-Reference-persist), [security](Module-Reference-security), [diag](Module-Reference-diag), [dynamics](Module-Reference-dynamics) | One page per core module |
| [Contributing](Contributing) | Worker rules, branches, PRs, trailers |
| [Adding an Effect](Adding-an-Effect) | Steps for a new render effect |
| [Adding an Action](Adding-an-Action) | Steps for a new action id |
| [Show File Format and Migration](Show-File-Format-and-Migration) | Save file, JSON schema, migrations |
| [Security Model](Security-Model) | What the core rejects and why |
| [Roadmap and Deferred Work](Roadmap-and-Deferred-Work) | Open decisions and deferred items |

## Sources

The authoritative documents live in the repository: `StageSignal-Specification-v0.1.md`, `docs/spec-addendum-v0.2.md`, `docs/ARCHITECTURE.md`, `docs/WORKER_RULES.md`, `docs/specs/*.md`, `docs/TRACEABILITY.md` and `docs/KNOWN_LIMITATIONS.md`. If this wiki and those files disagree, the files win. Tell us and we will fix the page.
