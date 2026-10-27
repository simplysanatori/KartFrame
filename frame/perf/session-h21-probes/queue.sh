#!/bin/bash
# Headset session 21 (frame/perf/session-h21-probes/README.md): the session-13 replay with synthetic GPU load.
#   frame/perf/session-h21-probes/queue.sh XRPROBE_BINARY [RECORDING_DIR]
# Copies the binary and replay-case.sh into $KARTFRAME_HOME/h21/ and writes h21p-01..06-<case>.env into
# $KARTFRAME_HOME/session.queue/ (replay-case.sh cases 7..12). The KartFrame shortcut's wrapper takes one per
# launch, in name order, and runs the case instead of the game (KARTFRAME_EXE), with its own session folder
# (xrprobe.log, stdout.log, vrtiming.log, vrframes.log) and, except in launch 6, the panel recording.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
KFHOME=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
BIN=${1:?xrprobe binary}
REC=$(cd "${2:-$HOME/.cache/kf-laneT/dump-h13rec}" && pwd)
[ -x "$BIN" ] || { echo "no xrprobe binary at $BIN" >&2; exit 1; }
[ -s "$REC/index.txt" ] || { echo "no recording (index.txt) in $REC" >&2; exit 1; }
"$BIN" --help | grep -q -- --replay-gpu-ms || { echo "$BIN has no --replay-gpu-ms (lane Y build needed)" >&2; exit 1; }
mkdir -p "$KFHOME/h21" "$KFHOME/session.queue"
cp "$BIN" "$KFHOME/h21/xrprobe"
cp "$HERE/../../tools/xrprobe/replay-case.sh" "$KFHOME/h21/"
OTHER=$(ls "$KFHOME/session.queue"/*.env 2> /dev/null | grep -v '/h21p-' || true)
[ -n "$OTHER" ] && echo "note: other queued launches come first or in between (name order): $OTHER"
[ -f "$KFHOME/session.env" ] && echo "note: $KFHOME/session.env is waiting and goes before the queue"
while IFS='|' read -r num name label args; do
  [ "$num" -ge 7 ] || continue
  i=$((num - 6))
  F=$KFHOME/session.queue/h21p-0$i-${name#21-?-}.env
  REC_LINE=KARTFRAME_RECORD=raw
  [ "$i" = 6 ] && REC_LINE="# no panel recording in this launch (framecorder's own GPU load is the question)"
  cat > "$F" << ENV
# Headset session 21, launch $i of 6 (frame/perf/session-h21-probes/README.md): xrprobe --replay case "$label"
# ($args). Look at a track-side edge, turn the head slowly left and right for ten seconds, hold still;
# again with one eye closed, then the other. Doubled or not, which eye?
KARTFRAME_EXE=$KFHOME/h21/replay-case.sh
KF_REPLAY_CASE=$num
KF_REPLAY_DIR=$REC
XRPROBE=$KFHOME/h21/xrprobe
PROBE_LOG=$KFHOME/sessions/latest/xrprobe.log
KARTFRAME_VRTIMING_FRAMES=1
$REC_LINE
KARTFRAME_SESSION_ENV_ONCE=1
ENV
  echo "queued $F"
done < <("$HERE/../../tools/xrprobe/replay-case.sh" --list)
