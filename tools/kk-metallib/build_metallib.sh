#!/bin/bash
# Offline builder for KosmicKrisp prebuilt Metal libraries (nps5).
#
#   build_metallib.sh <collect-dir> <out-dir> [jobs]
#
# <collect-dir> is what KosmicKrisp wrote with KK_MSL_COLLECT_DIR=<dir>
# (<key>.metal + <key>.opts). Each MSL file is compiled with Apple's Metal
# toolchain (xcrun metal -c -> xcrun metallib) into <out-dir>/<key>.metallib.
# The key already hashes MSL + KosmicKrisp build + macOS build + GPU, so the
# runtime (KK_METALLIB_DIR=<out-dir>) only loads libraries made for this
# machine/OS/driver. MANIFEST.txt records the toolchain used. Re-running skips
# libraries that are already built. If the Metal toolchain is missing:
#   xcodebuild -downloadComponent MetalToolchain
set -euo pipefail
src=${1:?collect dir}; out=${2:?out dir}; jobs=${3:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || echo 4)}
if ! xcrun -sdk macosx -f metal >/dev/null 2>&1; then
  echo "Metal toolchain not installed (xcrun -f metal failed). Run: xcodebuild -downloadComponent MetalToolchain" >&2
  exit 2
fi
mkdir -p "$out"
{
  echo "built: $(date -u +%FT%TZ)"
  echo "macos: $(sw_vers -productVersion) ($(sysctl -n kern.osversion))"
  xcrun -sdk macosx metal --version | head -1
} > "$out/MANIFEST.txt"

build_one() {
  f=$1; out=$2
  key=$(basename "$f" .metal)
  [ -f "$out/$key.metallib" ] && return 0
  opts="${f%.metal}.opts"
  msl=$(sed -n 's/^msl=//p' "$opts"); math=$(sed -n 's/^math=//p' "$opts"); fp=$(sed -n 's/^fp=//p' "$opts")
  maj=$((0x$msl >> 16)); min=$((0x$msl & 0xffff))
  case $math in 2) mm=fast;; 1) mm=relaxed;; *) mm=safe;; esac
  case $fp in 0) ff=fast;; *) ff=precise;; esac
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  xcrun -sdk macosx metal -c -std=metal$maj.$min -fmetal-math-mode=$mm -fmetal-math-fp32-functions=$ff \
      -w "$f" -o "$tmp/$key.air"
  xcrun -sdk macosx metallib "$tmp/$key.air" -o "$tmp/$key.metallib"
  mv "$tmp/$key.metallib" "$out/$key.metallib"
  echo "built $key"
}
export -f build_one
n=$(ls "$src"/*.metal 2>/dev/null | wc -l | tr -d ' ')
echo "building $n libraries with $jobs jobs"
ls "$src"/*.metal | xargs -P "$jobs" -I{} bash -c 'build_one "$1" "$2"' _ {} "$out"
echo "done: $(ls "$out"/*.metallib | wc -l | tr -d ' ') metallibs in $out"
