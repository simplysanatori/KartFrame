#!/bin/bash
# Headset session 13 (frame/perf/session-h13/README.md): one xrprobe --replay case per launch.
#   replay-case.sh [CASE]   CASE 1..12 (default $KF_REPLAY_CASE); extra arguments go to xrprobe
# Cases 1-6: session 13. Cases 7-12: session 21 (frame/perf/session-h21/README.md), synthetic GPU load.
#   replay-case.sh --list   the cases, one "number|name|label|xrprobe arguments" line each
# The KartFrame wrapper runs it as the game (KARTFRAME_EXE=<this script> in a session.queue file, written by
# frame/perf/session-h13/queue.sh): the session folder, the panel recording (KARTFRAME_RECORD) and SteamVR's
# per-app settings then apply as for the game. run-probes.sh runs all six (PROBE_SET=replay).
# Env: KF_REPLAY_DIR (the recording: index.txt and PPMs; default ~/.cache/kf-laneT/dump-h13rec),
# XRPROBE (binary; default xrprobe beside this script),
# PROBE_SECONDS (45, counted while the session is visible), PROBE_LOG (default ./xrprobe-<case>.log).
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
CASES=(
"1|1-standing|1 StAnd|--replay-window standing --replay-sets 168"
"2|2-driving|2 drivE|--replay-window driving"
"3|3-phase|3 PHASE|--replay-window driving --replay-offset 1"
"4|4-pairs|4 PAIrS|--replay-window driving --replay-pairs 3"
"5|5-pairs-quad|5 qUAd|--replay-window driving --replay-pairs 3 --replay-quad"
"6|6-hold|6 HOLd|--replay-window driving --replay-hold 2"
"7|21-1-load5|1 LOAd5|--replay-window driving --replay-gpu-ms 5"
"8|21-2-split8|2 SPLIt8|--replay-window driving --replay-gpu-ms 5 --replay-gpu-split 8"
"9|21-3-lowprio|3 LO-Pri|--replay-window driving --replay-gpu-ms 5 --queue-priority low"
"10|21-4-load2|4 LOAd2|--replay-window driving --replay-gpu-ms 2"
"11|21-5-control|5 drivE|--replay-window driving"
"12|21-6-norec|6 norEC|--replay-window driving --replay-gpu-ms 5"
)
if [ "${1:-}" = --list ]; then printf '%s\n' "${CASES[@]}"; exit 0; fi
N=${KF_REPLAY_CASE:-}
case "${1:-}" in [1-9] | 1[0-2]) N=$1; shift ;; esac
# Run as the game by the wrapper (the case from the queue file): Steam's launcher appends its own
# arguments (--oom-score-adjust ...), which the game ignores and xrprobe rejects; drop them.
[ -n "${KF_REPLAY_CASE:-}" ] && set --
[ -n "$N" ] && [ "$N" -ge 1 ] 2> /dev/null && [ "$N" -le ${#CASES[@]} ] || { echo "replay-case: no case 1..${#CASES[@]} given" >&2; exit 2; }
IFS='|' read -r _ NAME LABEL ARGS <<< "${CASES[$((N - 1))]}"
BIN=${XRPROBE:-$HERE/xrprobe}
DIR=${KF_REPLAY_DIR:-$HOME/.cache/kf-laneT/dump-h13rec}
LOG=${PROBE_LOG:-./xrprobe-$NAME.log}
echo "replay-case: case $NAME ($ARGS), recording $DIR, log $LOG"
# shellcheck disable=SC2086  # ARGS holds several options on purpose
exec "$BIN" --replay "$DIR" $ARGS --replay-label "$LABEL" --hz 120 --seconds "${PROBE_SECONDS:-45}" --log "$LOG" "$@"
