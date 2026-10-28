# Third-party notices

Index of the third-party code StageSignal builds with. The full licence texts are copied from the exact pinned source trees into every release bundle by `scripts/collect_release_notices.py` (folders `JUCE/`, `clap-juce-extensions/`, `core-dependencies/`); this file is the human-readable index and records where each pin lives. It is not a licence audit, and it does not grant rights in StageSignal itself (see `docs/DISTRIBUTION.md`).

Facts below were read from the pinned source trees on 28 Oct 2026. Anything not listed was not checked.

| Component | Version / pin | Where pinned | Licence (as found in the source tree) | Used by |
|---|---|---|---|---|
| JUCE | `8.0.16-security`, commit `ea77444750de45e036e0279bb0291144bf8e3688` | `cmake/ss_deps.cmake` | Dual: AGPLv3 or the commercial JUCE licence (`LICENSE.md`). The owner declared the commercial route on 22 Oct 2026 (`docs/DISTRIBUTION.md`); entitlement is owner-declared and not audited here. | app layer (`app/`) only |
| VST3 SDK (bundled inside JUCE `modules/juce_audio_processors_headless/format_types/VST3_SDK`) | as shipped in the JUCE pin | JUCE | MIT (`VST3_SDK/LICENSE.txt`) | VST3 build |
| clap-juce-extensions | commit `7adee3a1bd4684d4caa5601100e364abccff4b4d` | `cmake/ss_deps.cmake` | MIT (Paul Walker, 2019-2020; `LICENSE.md`) | CLAP build |
| CLAP SDK (submodule of clap-juce-extensions) | `clap-libs/clap` at `29ffcc273be7c7c651f6c9953b99e69700e2387a` (1.2.7) | submodule of the pin above | MIT (Alexandre Bique; `LICENSE`) | CLAP build |
| clap-helpers (submodule) | `clap-libs/clap-helpers` at `a61bcdf0ecc2c8db1e80bfe8bf9cb7e8d9fd2bbc` | submodule of the pin above | MIT (Alexandre Bique; `LICENSE`) | CLAP build |
| nlohmann/json | single header in `third_party/nlohmann/json.hpp`, header banner says 3.11.3 | vendored copy | MIT (Niels Lohmann, 2013-2022; `third_party/nlohmann/LICENSE.MIT`) | core (`persist`, `broadcast`, `media`, `perf`, `record`) and app |
| doctest | 2.4.11, `third_party/doctest/doctest.h` | vendored copy | MIT (Viktor Kirilov, 2016-2023; `third_party/doctest/LICENSE.txt`) | tests only, never shipped |

JUCE itself contains further third-party code (zlib, libjpeg, OpenGL loader notices, others). `scripts/collect_release_notices.py` copies those notices into the bundle; consult the bundle rather than this table for them.

## Not bundled
- FFmpeg is **not** bundled or linked. Recording and broadcast start it as a separate executable that the user installs (`docs/KNOWN_LIMITATIONS.md` D-07); its own licence applies to that install.

## Other trees in this repository
`skills/` and `shared/scripts/` are agent tooling that is not part of the StageSignal build. `skills/_third_party_licenses/` holds the licences for the skill packs it contains. Neither is compiled into or shipped with the app.

## Rules for adding a dependency
Pin the version or commit, record it in this table, read the licence from the source tree, and make sure `scripts/collect_release_notices.py` copies its notice. Code with a missing or incompatible licence is not added.
