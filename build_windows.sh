#!/bin/sh
# Cross-compiles StageSignal.exe for Windows with MinGW-w64 (posix threads, static) and packages both variants:
#   dist/StageSignal-<ver>-x86-base/   (i586, no SSE/MMX, 387 FP: legacy baseline)   + .zip
#   dist/StageSignal-<ver>-x64/                                                       + .zip
# Usage: ./build_windows.sh [x64|x86|all]     (default all)   Needs: cmake, i686/x86_64-w64-mingw32-g++-posix, windres, zip
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
WHAT=${1:-all}
VER=$(sed -n 's/^project(StageSignal VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
JOBS=${JOBS:-4}
DIST="$ROOT/dist"
mkdir -p "$DIST"

package() { # name triple builddir
  NAME=$1; TRIPLE=$2; BD=$3
  OUT="$DIST/StageSignal-$VER-$NAME"
  rm -rf "$OUT"; mkdir -p "$OUT/docs"
  cp "$BD/StageSignal.exe" "$OUT/StageSignal.exe"
  "$TRIPLE-strip" "$OUT/StageSignal.exe"
  cp -r "$ROOT/assets" "$OUT/assets"
  cp -r "$ROOT/licenses" "$OUT/licenses"
  cp -r "$ROOT/docs/help" "$OUT/docs/help"
  [ -f "$ROOT/docs/LIGHTING.md" ] && cp "$ROOT/docs/LIGHTING.md" "$OUT/docs/"
  mkdir -p "$OUT/samples"; cp -r "$ROOT/samples/." "$OUT/samples/"
  cat > "$OUT/README.txt" <<README
StageSignal $VER ($NAME) - Visual Core Beta, unsigned
========================================================
Run StageSignal.exe. No installer, no network, no login. Nothing is downloaded or installed.
Settings, autosaves, logs and benchmark results are stored in %LOCALAPPDATA%\\StageSignal (not in this folder).
Delete this folder to uninstall; your shows and recordings are kept.

Start here
  1. Setup tab: identify displays, choose audio input, set hardware gain, run the benchmark.
  2. File > Open show... and choose samples\\demo.stagesignal (12 scenes, 8 programmed cues, 4 text messages).
  3. Live tab: keys 1-8 pick scenes, F5 next, F6 pause, F7 resume, F8 emergency blackout, Shift+F8 restore, F1 help.
  DMX, recording and broadcasting are always OFF at startup and need an explicit Start/Arm.

Command line
  StageSignal.exe --version | --selftest | --benchmark [--out file.json] [--scale 0.1] | --data-dir <folder>

Contents: StageSignal.exe, assets\\ (original CC0 procedural assets + manifest), licenses\\ (inventory + third-party notices),
          docs\\help\\ (offline help sources, also embedded in the exe), samples\\ (demo show), docs\\LIGHTING.md.
Build: $NAME build, MinGW-w64 static, Windows Vista-or-later API set resolved at run time where optional.
Known limits (spec section 23): SRT broadcast is not available in this build; H.264 uses the Windows Media Foundation
encoder and is not benchmark-qualified; no USB DMX adapter is hardware-qualified; the GUI has had no test on a real Windows desktop.
Supported minimum screen: 800x600 effective client area (smaller areas switch to a scrolling compact layout).
README
  (cd "$DIST" && rm -f "StageSignal-$VER-$NAME.zip" && zip -qr "StageSignal-$VER-$NAME.zip" "StageSignal-$VER-$NAME")
  echo "packaged $OUT ($(wc -c < "$OUT/StageSignal.exe") bytes exe)"
}

build() { # name triple toolchain
  NAME=$1; TRIPLE=$2; TC=$3; BD="$ROOT/build_win_$NAME"
  cmake -S "$ROOT" -B "$BD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/$TC" -DSS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BD" --target StageSignal -j "$JOBS"
  package "$NAME" "$TRIPLE" "$BD"
}

case "$WHAT" in
  x64|all) build x64 x86_64-w64-mingw32 toolchain-mingw-x64.cmake ;;
esac
case "$WHAT" in
  x86|all) build x86-base i686-w64-mingw32 toolchain-mingw-x86-base.cmake ;;
esac
echo "done: $(ls "$DIST"/*.zip)"
