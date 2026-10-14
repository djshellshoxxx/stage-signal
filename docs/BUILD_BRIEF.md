# Build brief (all contributors)
Spec: `StageSignal-Specification-v0.1.md` (REQ-/MOD-/IF- IDs). Build the product to spec; honest status only.

## Rules
- C++17, portable modules compile with g++ on Linux AND mingw (i686 BASE: `-march=i586 -mno-sse`, no SIMD, no x64-only). No exceptions across module APIs beyond std::bad_alloc; no RTTI tricks.
- Namespace `ss`. Include style `#include "core/types.h"` (include root = `src/`; third_party root = `third_party/`: `nlohmann/json.hpp`, `stb/stb_image*.h`).
- Foundation (do not change signatures; additive only, tell lead if you need a change): `src/core/{types.h,queue.h,clock.cpp,ports.h}`, `CMakeLists.txt`, `tests/test.h`.
- CMake globs `src/<module>/**/*.cpp` into `ss_<module>` for: core render media midi lighting recording broadcast persistence diagnostics perf. Windows-only code goes in `src/platform/windows`, `src/ui`, `src/app` (built only for WIN32; must cross-compile with `x86_64-w64-mingw32-g++-posix` / `i686-w64-mingw32-g++-posix`). Never put Windows headers in portable dirs.
- Tests: `tests/test_<area>.cpp` using `tests/test.h` (ends with `int main(){return ss_test::runAll();}`), deterministic, no sleeps >200 ms. Each test file is auto-built. Name test files with your area prefix to avoid collisions.
- Only touch files in your owned dirs/tests. Header-first: public headers must be stable and documented in 1-line comments; other agents code against them concurrently.
- No heap allocation/locks in DSP hot paths after construction. Bounded everything (spec §8). Memory: BASE budget 96 MiB total process.
- Don't commit; the lead commits. Build/test: `cd build && cmake .. >/dev/null && make -j4 <target> && ./<test>` (use your own build dir `build_<area>` to avoid clobbering).
- Report back in <=15 lines: files, public API headers, what's verified (tests), any spec gaps/deviations. No long prose.
