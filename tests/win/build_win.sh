#!/bin/sh
# Cross-compile the Windows adapters + smoke test for both baselines. Usage: tests/win/build_win.sh [outdir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-/tmp/ss_win_build}
mkdir -p "$OUT"
SRCS="$(ls "$ROOT"/src/platform/windows/*.cpp) $ROOT/src/core/clock.cpp"
COMMON="-std=c++17 -O2 -Wall -Wextra -Wpedantic -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNOMINMAX -I$ROOT/src -I$ROOT/src/platform/windows"
LIBS="-lmfuuid -lole32 -loleaut32 -luuid -lwinmm -lws2_32 -lgdi32 -luser32 -ladvapi32 -lcrypt32 -liphlpapi"
x86_64-w64-mingw32-g++-posix $COMMON -static $SRCS "$ROOT/tests/win/smoke_platform.cpp" -o "$OUT/smoke_x64.exe" $LIBS
i686-w64-mingw32-g++-posix $COMMON -march=i586 -mno-sse -static $SRCS "$ROOT/tests/win/smoke_platform.cpp" -o "$OUT/smoke_x86.exe" $LIBS
echo "built $OUT/smoke_x64.exe $OUT/smoke_x86.exe"
