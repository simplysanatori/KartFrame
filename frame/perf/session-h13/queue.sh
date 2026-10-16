#!/bin/bash
# Headset session 13: installs the replay beside KartFrame and queues its six launches, one case each.
#   frame/perf/session-h13/queue.sh XRPROBE_BINARY [RECORDING_DIR]
# Copies the binary and replay-case.sh into $KARTFRAME_HOME/h13/ and writes h13-01..06-<case>.env into
# $KARTFRAME_HOME/session.queue/. The KartFrame shortcut's wrapper then takes one per launch, in name order,
# and runs the case instead of the game (KARTFRAME_EXE), with its own session folder (xrprobe.log, stdout.log,
# vrtiming.log) and the panel recording (KARTFRAME_RECORD=raw). Nothing else changes.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
KFHOME=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
BIN=${1:?xrprobe binary}
REC=$(cd "${2:-$HOME/.cache/kf-laneT/dump-h13rec}" && pwd)
[ -x "$BIN" ] || { echo "no xrprobe binary at $BIN" >&2; exit 1; }
[ -s "$REC/index.txt" ] || { echo "no recording (index.txt) in $REC" >&2; exit 1; }
mkdir -p "$KFHOME/h13" "$KFHOME/session.queue"
cp "$BIN" "$KFHOME/h13/xrprobe"
cp "$HERE/../../tools/xrprobe/replay-case.sh" "$KFHOME/h13/"
OTHER=$(ls "$KFHOME/session.queue"/*.env 2> /dev/null | grep -v '/h13-' || true)
[ -n "$OTHER" ] && echo "note: other queued launches come first or in between (name order): $OTHER"
[ -f "$KFHOME/session.env" ] && echo "note: $KFHOME/session.env is waiting and goes before the queue"
while IFS='|' read -r num name label args; do
  [ "$num" -le 6 ] || continue  # cases 7-12 are session 21's
  F=$KFHOME/session.queue/h13-0$num-${name#*-}.env
  cat > "$F" << ENV
# Headset session 13, launch $num of 6 (frame/perf/session-h13/README.md): xrprobe --replay case "$label"
# ($args). Look at a track-side edge, turn the head slowly left and right for ten seconds, hold still;
# again with one eye closed, then the other. Doubled or not, which eye?
KARTFRAME_EXE=$KFHOME/h13/replay-case.sh
KF_REPLAY_CASE=$num
KF_REPLAY_DIR=$REC
XRPROBE=$KFHOME/h13/xrprobe
PROBE_LOG=$KFHOME/sessions/latest/xrprobe.log
KARTFRAME_RECORD=raw
KARTFRAME_SESSION_ENV_ONCE=1
ENV
  echo "queued $F"
done < <("$HERE/../../tools/xrprobe/replay-case.sh" --list)
