# Building

## Prerequisites

- CMake 3.22 or newer
- Ninja (CI uses it)
- A C++17 compiler: GCC 13, Clang, or MSVC 2022 (CI runs all three OSes)
- Git

No network is needed for the core build. The core uses `third_party/doctest` and `third_party/nlohmann`, which are in the repo.

## Core library and tests

This is the everyday loop. It builds only the portable core and its tests.

```sh
# Configure, build and run all core tests
cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Build and run one module while you work on it. The machine is shared, so keep builds small:

```sh
# Build and run only the show module tests
cmake --build build --target test_show
./build/tests_show/test_show
```

Test executables are named `test_<module>` and sit in `build/tests_<module>/`.

Useful options:

| Option | Default | Meaning |
|---|---|---|
| `SS_BUILD_TESTS` | ON | Build the test executables |
| `SS_BUILD_APP` | OFF | Build the JUCE app (needs JUCE, below) |
| `CMAKE_BUILD_TYPE` | Release (set by the root file if unset) | Build type |

## Warnings

The module CMake applies `-Wall -Wextra -Wshadow -Wno-unused-parameter` on GCC and Clang, and `/W4 /permissive- /utf-8` on MSVC. Warnings in your code should be zero. The build does not use `-Werror`.

## JUCE app

The app build needs JUCE and clap-juce-extensions. Both are pinned by commit in `cmake/ss_deps.cmake`:

| Dependency | Pin |
|---|---|
| JUCE | 8.0.16-security, commit `ea77444750de45e036e0279bb0291144bf8e3688` (AGPLv3 option) |
| clap-juce-extensions | `7adee3a1bd4684d4caa5601100e364abccff4b4d` (MIT, includes the CLAP SDK and clap-helpers) |

If you already have checkouts, point CMake at them to skip the download:

```sh
# Configure the app with local JUCE and clap-juce-extensions checkouts
cmake -S . -B build-app -G Ninja -DSS_BUILD_APP=ON -DSS_BUILD_TESTS=OFF \
  -DSS_JUCE_DIR=/path/to/JUCE -DSS_CLAP_EXT_DIR=/path/to/clap-juce-extensions
cmake --build build-app --target StageSignal_Standalone StageSignal_VST3 StageSignal_CLAP
```

Without those options CMake downloads both at the pinned commits.

### Linux packages for the app

These are the packages CI installs on Ubuntu:

```sh
# Build dependencies for the JUCE app on Ubuntu
sudo apt-get install -y --no-install-recommends ninja-build pkg-config libasound2-dev libx11-dev \
  libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
  libfreetype-dev libfontconfig1-dev libgl1-mesa-dev xvfb
```

`xvfb` is only needed for the headless smoke test.

## Running the app

The app on `main` is a placeholder that opens a window with the name on it. The build is still useful as a compile check for the JUCE layer.

## Common problems

| Symptom | Likely cause |
|---|---|
| CMake cannot find JUCE | Missing `SS_JUCE_DIR` or no network for the download |
| Linker errors for ALSA or X11 on Linux | Missing `-dev` packages above |
| A test fails on one OS only | Check the test for hidden time or locale assumptions. Tests must be deterministic. |
