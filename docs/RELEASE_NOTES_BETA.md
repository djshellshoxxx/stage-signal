Beta build. Unsigned: on macOS run `xattr -dr com.apple.quarantine <file>` once; Windows SmartScreen may warn.
Contents: standalone app, VST3 plugin and CLAP plugin for Windows x64, macOS arm64, macOS x86_64 and Linux x64, with SHA256SUMS.txt.
Known limitations are listed in docs/KNOWN_LIMITATIONS.md.

This beta adds guarded audience routing, explicit camera preview/publication on Windows/macOS, optional external FFmpeg GIF/video playback, resumable setup and baseline software benchmarks. BEN-03 through BEN-06 and physical hardware, music-corpus, accessibility and four-hour qualification remain open; this release does not claim every specification is qualified. Linux camera is explicitly unsupported in v1.
