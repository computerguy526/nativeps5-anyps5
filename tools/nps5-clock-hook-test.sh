#!/bin/bash
# Builds and runs core/libs/tests/GuestClockHook.cpp against the arm64 host libkernel (the cmake test target needs BUILD_TESTING,
# which the APS5_HOST_ARM64 configuration skips). usage: tools/nps5-clock-hook-test.sh [build-arm64 dir]
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"; U="${1:-$R/build-arm64}/core/libs/libs/unpatched"; O="$(mktemp -d)"
clang++ -std=c++20 -I"$R/core/libs" -I"$R/core/libs/prx" -I"$R/core" -I"$R" "$R/core/libs/tests/GuestClockHook.cpp" -o "$O/gch" "$U/libkernel.prx" "$U/libc.prx" -Wl,-rpath,"$U"
"$O/gch"; rm -rf "$O"
