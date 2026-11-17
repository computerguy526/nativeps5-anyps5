#!/bin/zsh
# nps5-autoprewarm.sh: the script-level hook. Run it right before a title run with the same VK_DRIVER_FILES / KK_* environment
# the run will use:
#   tools/nps5-autoprewarm.sh <gpu-cache dir> [--build-dir DIR] [--jobs N]
# If the cache's stamp is stale (KosmicKrisp build, its flags, macOS build, GPU or the prewarm tool changed since the cache was last
# built) and a pipeline journal exists, it rebuilds the cache offline with tools/nps5-prewarm.sh, so the run starts warm. A fresh
# stamp, no journal, or missing tools (nps5_prewarm, nps5_gpucache not built in --build-dir) all return at once; the run then
# relies on the in-process background rebuild (the journal replay with Now/Ahead/Idle priorities) and still works.
# AUTO_PREWARM=0 turns the hook off; AUTO_PREWARM_TIMEOUT (s, default 1800) bounds it. Exit status is always 0: a failed prewarm
# is reported and the run goes on (a cold cache is slow, not wrong).
set -u
[ "${AUTO_PREWARM:-1}" = 0 ] && { echo "autoprewarm: off"; exit 0; }
CACHE=${1:?usage: nps5-autoprewarm.sh <gpu-cache dir> [--build-dir DIR] [--jobs N]}; shift
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$HERE/../build-arm64
ARGS=()
while [ $# -gt 0 ]; do case $1 in --build-dir) BUILD=$2; ARGS+=(--build-dir "$2"); shift 2;; *) ARGS+=("$1"); shift;; esac; done
P=$BUILD/core/libs/prx/libSceAgcDriver
if [ ! -x "$P/nps5_prewarm" ] || [ ! -x "$P/nps5_gpucache" ]; then
  echo "autoprewarm: nps5_prewarm / nps5_gpucache not built in $BUILD (cmake --build <dir> -j3 --target nps5_prewarm nps5_gpucache); skipping, the run rebuilds in the background"
  exit 0
fi
"$HERE/nps5-prewarm.sh" "$CACHE" "${ARGS[@]}" --check > /dev/null 2>&1
case $? in
  0) echo "autoprewarm: stamp fresh, nothing to do"; exit 0;;
  3) echo "autoprewarm: no journal under $CACHE yet (first run records it)"; exit 0;;
  10) ;;
  *) echo "autoprewarm: stamp check failed; skipping"; exit 0;;
esac
echo "autoprewarm: stamp stale, rebuilding $CACHE from its journal"
T0=$(date +%s)
gtimeout -s KILL "${AUTO_PREWARM_TIMEOUT:-1800}" "$HERE/nps5-prewarm.sh" "$CACHE" "${ARGS[@]}" > "${AUTO_PREWARM_LOG:-/dev/null}" 2>&1
rc=$?
echo "autoprewarm: rc=$rc in $(( $(date +%s) - T0 )) s"
exit 0
