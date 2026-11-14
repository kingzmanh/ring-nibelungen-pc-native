#!/bin/sh
# Hidden, muted test run: ./run_test.sh <seconds> ; extra RING_* env vars pass through.
# The switches are exported (a mixed MSYS2 PATH lost inline assignments once,
# and the game came up visible with sound).
cd "$(dirname "$0")"
PATH=/usr/bin:/bin:$PATH
S=${1:-15}
export RING_HIDDEN=1 RING_MUTE=1 RING_QUIT=$((S*1000))
export RING_REGFILE=${RING_REGFILE:-test_registry.txt}
timeout $((S+20)) ./Ring.exe
echo "exit $?"
tail -n ${TAIL:-40} ring.log
