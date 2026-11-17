#!/bin/zsh
# nps5-prewarm.sh: fill a title's GPU cache before its first launch (the `nps5 prewarm <title>` step).
#   tools/nps5-prewarm.sh <gpu-cache dir> [--jobs N] [--build-dir DIR]
# Pass 1 replays the pipeline journal (state + SPIR-V of every pipeline the title ever built): KosmicKrisp compiles each
# one and the store collects the MSL. Then every collected MSL is built to a metallib (xcrun metal, N parallel jobs, default
# half the cores, background priority). Pass 2 replays again so the Metal pipeline archive is built from the metallibs, which is
# what a later launch loads (a pipeline link of ~1 ms instead of a compile). Every step is bounded by PREWARM_TIMEOUT (s, default 3000).
# --check: only compare the stamp (exit 0 fresh, 10 stale, 3 no journal). --batch N (default 200): pass 1/2 replay N journal entries per process (a compiler process stays well under ~2 GB; the memory is
# returned between batches), until the range covers the journal. Needs the same VK_DRIVER_FILES the title runs with. Safe to re-run: present pipelines are skipped, stale rows are invalidated.
set -u
# /bin/zsh is a SIP binary and drops DYLD_LIBRARY_PATH from its children: set it again (the Vulkan loader lives in /opt/homebrew/lib).
DY=${NPS5_DYLD:-/opt/homebrew/lib}
# (nps5_prewarm lowers its own priority; nice/taskpolicy are SIP binaries too.)
CACHE=${1:?usage: nps5-prewarm.sh <gpu-cache dir> [--jobs N] [--build-dir DIR]}; shift
CORES=$(sysctl -n hw.ncpu); JOBS=0; RJOBS=$((CORES / 2)); [ $RJOBS -lt 1 ] && RJOBS=1
W=$(cd "$(dirname "$0")/.." && pwd); B=$W/build-arm64
BATCH=200
while [ $# -gt 0 ]; do case $1 in --check) CHECK=1; shift;; --batch) BATCH=$2; shift 2;; --force) FORCE=1; shift;; --jobs) JOBS=$2; RJOBS=$2; shift 2;; --build-dir) B=$2; shift 2;; *) echo "unknown $1"; exit 2;; esac; done
T=${PREWARM_TIMEOUT:-3000}
P=$B/core/libs/prx/libSceAgcDriver
# The prebuilt stamp: SHA-256 of the exact inputs (the journal bytes, the macOS build, the KosmicKrisp ICD and its library, this tool's binary and the
# GPU). A Mac opens Metal archives only from its own OS build, so a stamp from another machine or after an update never matches. A matching
# stamp skips the whole step (--force ignores it).
sha() { shasum -a 256 "$@" 2>/dev/null | cut -c1-64; }
ICDLIB=$(sed -n 's/.*"library_path"[^"]*"\([^"]*\)".*/\1/p' "${VK_DRIVER_FILES:-/nonexistent}" 2>/dev/null | head -1)
case "$ICDLIB" in ""|/*) ;; *) ICDLIB="$(dirname "${VK_DRIVER_FILES}")/$ICDLIB";; esac
# Replay workers: Metal's concurrent compile task count (nps5_prewarm --print-workers; nxnative 72d00571), not half the cores.
[ "$JOBS" = 0 ] && JOBS=$(DYLD_LIBRARY_PATH=$DY $P/nps5_prewarm --print-workers 2>/dev/null); [ -n "$JOBS" ] && [ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=$RJOBS
# The journal of THIS tool's recompiler version (<cache>/<version>/pipelines.journal): `ls | head -1` picked the first of several stale
# version directories. Fallback for a tool without --print-version: the most recently written journal.
VER=$(DYLD_LIBRARY_PATH=$DY $P/nps5_prewarm --print-version 2>/dev/null)
JOURNAL="$CACHE/$VER/pipelines.journal"
[ -n "$VER" ] && [ -f "$JOURNAL" ] || JOURNAL=$(ls -t "$CACHE"/*/pipelines.journal 2>/dev/null | head -1)
[ -f "$JOURNAL" ] || { echo "no pipelines.journal under $CACHE (run the title once first)"; exit 3; }
# The journal bytes are NOT part of the stamp: a run on this configuration appends to the journal and compiles those pipelines itself
# (the archive is written at exit), so only a change of KosmicKrisp, its flags, the OS, the GPU or this tool makes the cache stale.
KKFLAGS=$(env | grep -E "^(MESA_KK_|KK_|kk_)" | grep -v -E "^(KK_PIPELINE_ARCHIVE|KK_METALLIB_DIR|KK_MSL_COLLECT_DIR)=" | sort | shasum -a 256 | cut -c1-64)
STAMP=$(print -r -- "journal present $(basename "$(dirname "$JOURNAL")")
flags $KKFLAGS
os $(sw_vers -buildVersion) $(sysctl -n machdep.cpu.brand_string)
icd ${VK_DRIVER_FILES:-} $(sha "${VK_DRIVER_FILES:-/nonexistent}") lib $(sha "${ICDLIB:-/nonexistent}")
tool $(sha "$P/nps5_prewarm") batch $BATCH" | shasum -a 256 | cut -c1-64)
if [ -z "${FORCE:-}" ] && [ -f "$CACHE/prewarm.stamp" ] && [ "$(cat "$CACHE/prewarm.stamp")" = "$STAMP" ]; then echo "prewarm: stamp $STAMP matches, nothing to do"; exit 0; fi
[ -n "${CHECK:-}" ] && { echo "prewarm: stamp is stale (config changed since the cache was last built)"; exit 10; }
export ANYPS5_SHADER_CACHE_DIR=$CACHE APS5_PIPELINE_JOURNAL_THREADS=$JOBS
replay() {  # bounded batches: batch k replays entries 0..(k+1)*BATCH-1; the earlier ones are archive hits, only the new ones compile (rc 11 = end reached)
  local count=$BATCH
  while :; do
    gtimeout -s KILL $T env DYLD_LIBRARY_PATH=$DY $P/nps5_prewarm --cache "$CACHE" --threads $JOBS --range 0:$count; rc=$?
    [ $rc = 11 ] && break
    [ $rc != 0 ] && { echo "batch to $count rc=$rc"; [ $rc = 137 ] && break; }
    count=$((count + BATCH))
  done
}
for step in 1 2; do
  echo "== pass $step: journal replay ($JOBS threads, batches of $BATCH)"
  replay
  if [ $step = 1 ]; then
    echo "== metallib build ($RJOBS jobs)"
    gtimeout -s KILL ${REBUILD_TIMEOUT:-900} $P/nps5_gpucache rebuild --cache "$CACHE" --jobs $RJOBS || echo "rebuild rc=$?"
  fi
done
$P/nps5_gpucache stats --cache "$CACHE"
$P/nps5_gpucache verify --cache "$CACHE" && print -r -- "$STAMP" > "$CACHE/prewarm.stamp"
