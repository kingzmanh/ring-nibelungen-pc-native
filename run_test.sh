#!/bin/sh
# Hidden, muted test run: ./run_test.sh <seconds> ; extra RING_* env vars pass through.
cd "$(dirname "$0")"
S=${1:-15}
RING_HIDDEN=1 RING_MUTE=1 RING_QUIT=$((S*1000)) RING_REGFILE=${RING_REGFILE:-test_registry.txt} timeout $((S+20)) ./Ring.exe
echo "exit $?"
tail -n ${TAIL:-40} ring.log
