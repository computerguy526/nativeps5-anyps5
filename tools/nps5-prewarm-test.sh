#!/bin/zsh
# End-to-end test of the prewarm step on a Calculator3D frame capture (no game data, no guest, silent). Every GPU process is under gtimeout -s KILL 55.
#   tools/nps5-prewarm-test.sh <capture dir, e.g. work/s3/rosetta/captures_old/A> [build dir]
# Checks: cold run journals pipelines; prewarm from the journal alone builds them; the prewarmed run has 0 draw-time stalls and pixels identical to
# the reference frame (max channel diff 0); the stamp skips a second prewarm; a corrupted metallib row is detected by verify and dropped, never loaded.
set -u
CAP=${1:?capture dir}; W=$(cd "$(dirname "$0")/.." && pwd); B=${2:-$W/build-arm64}; P=$B/core/libs/prx/libSceAgcDriver
: ${VK_DRIVER_FILES:?set VK_DRIVER_FILES to the KosmicKrisp icd json}
D=$(mktemp -d /tmp/nps5-prewarm-test.XXXXXX); trap 'rm -rf $D' EXIT; fail=0
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
run() { env DYLD_LIBRARY_PATH=/opt/homebrew/lib ANYPS5_SHADER_CACHE_DIR=$1 gtimeout -s KILL 55 $P/agc_frame_replay "$CAP" > $D/$2.out 2> $D/$2.err; }
mkdir -p $D/cold $D/pre; run $D/cold cold
V=$(ls -d $D/cold/*/ | head -1); chk "cold run wrote a journal" "[ -s $V/pipelines.journal ]"
mkdir -p $D/pre/$(basename $V); cp $V/pipelines.journal $D/pre/$(basename $V)/
PREWARM_TIMEOUT=55 $W/tools/nps5-prewarm.sh $D/pre --jobs 2 --batch 1 > $D/prewarm.log 2>&1
chk "prewarm wrote a stamp" "[ -s $D/pre/prewarm.stamp ]"
run $D/pre pre
chk "prewarmed run: 0 draw-time stalls" "grep -q ' 0 draw-time stalls' $D/pre.err"
chk "prewarmed run: pixels identical" "grep -q '\"max_channel_diff\":0' $D/pre.out"
chk "second prewarm skipped by stamp" "PREWARM_TIMEOUT=55 $W/tools/nps5-prewarm.sh $D/pre --jobs 2 --batch 1 | grep -q 'matches, nothing to do'"
sqlite3 $D/pre/gpucache.sqlite "UPDATE artefact SET data = substr(data,1,length(data)/2) || X'FF' || substr(data,length(data)/2+2) WHERE kind=2 AND rowid=(SELECT min(rowid) FROM artefact WHERE kind=2)"
chk "verify detects a corrupted metallib" "! $P/nps5_gpucache verify --cache $D/pre > /dev/null"
run $D/pre corrupt
chk "run with a corrupted metallib still renders identical pixels" "grep -q '\"max_channel_diff\":0' $D/corrupt.out"
exit $fail
