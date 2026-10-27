#!/bin/bash
# Probe session: runs the five xrprobe cases one after another, 30 s each
# with a 5 s pause between, and collects every log into one dated directory.
# Put the headset on, start SteamVR, then run this from a terminal:
#   tools/xrprobe/run-probes.sh [extra xrprobe options, e.g. --hz 90]
# Environment: PROBE_SECONDS (30), PROBE_PAUSE (5), PROBE_OUT (output directory),
#   XRPROBE (binary), VRTIMING (vrtiming.py; skipped when missing).
# KartFrame session 13: PROBE_SET=replay runs the six --replay cases of replay-case.sh instead
#   (PROBE_REPLAY_CASES="7 8 ..." picks other cases of replay-case.sh, e.g. session 21's 7-12; default 1-6)
#   (KF_REPLAY_DIR names the recording). PROBE_MOCK=1 runs against the mock OpenXR runtime
#   (frame/tools/xrmock, headless, never SteamVR) with a detailed trace per case ($OUT/<case>.trace),
#   the finished-image check (MOCK_XR_BUSY_MS=1) and MOCK_XR_DUMP_AT dumps of the released images.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${XRPROBE:-$HERE/build/xrprobe}
RUN_SECONDS=${PROBE_SECONDS:-30}
PAUSE=${PROBE_PAUSE:-5}
OUT=${PROBE_OUT:-$HOME/Games/GalaxyQuest/probe/probes-$(date +%Y%m%d-%H%M%S)}
VRTIMING=${VRTIMING:-$HOME/Games/MoonlightXRNative/src/tools/vrtiming.py}

if [ "${PROBE_MOCK:-0}" = 1 ]; then
    # As frame/tools/mockrun.sh: the mock runtime, built per checkout; never fall through to SteamVR.
    REPO=$(cd "$HERE/../../.." && pwd)
    MOCK_DIR=${KF_XRMOCK_DIR:-$HOME/.cache/kf-xrmock/$(printf %s "$REPO" | md5sum | cut -c1-8)}
    if [ ! -s "$MOCK_DIR/libxrmock.so" ] || [ -n "$(find "$HERE/../xrmock" -newer "$MOCK_DIR/libxrmock.so" -name '*.[ch]')" ]; then
        bash "$HERE/../xrmock/build.sh" "$MOCK_DIR" > /dev/null || { echo "the mock runtime did not build"; exit 1; }
    fi
    export XR_RUNTIME_JSON=$MOCK_DIR/runtime.json
    [ -s "$XR_RUNTIME_JSON" ] || { echo "no mock runtime at $XR_RUNTIME_JSON"; exit 1; }
    M0=$HOME/.cache/headset-in-use
    while [ -f "$M0" ] && kill -0 "$(awk '{print $2}' "$M0" 2> /dev/null)" 2> /dev/null; do echo "headset in use, waiting"; sleep 60; done
    export MOCK_XR_SECONDS=$((RUN_SECONDS + 30)) MOCK_XR_TRACE_DETAIL=1 MOCK_XR_BUSY_MS=${MOCK_XR_BUSY_MS:-1}
    PAUSE=0 VRTIMING=/nonexistent
    OUT=${PROBE_OUT:-$HOME/.cache/kf-xrprobe/mock-$(date +%Y%m%d-%H%M%S)}
fi
if [ ! -x "$BIN" ]; then
    echo "xrprobe not built. Build it with:"
    echo "  cmake -S $HERE -B $HERE/build -G Ninja -DCMAKE_C_COMPILER=clang"
    echo "  flock ${GQROOT:-~/Games/GalaxyQuest}/.build.lock cmake --build $HERE/build"
    exit 1
fi
if [ "${PROBE_MOCK:-0}" != 1 ] && ! pgrep -x vrcompositor >/dev/null; then
    echo "SteamVR is not running (no vrcompositor). Start SteamVR, put the headset on, then run this again."
    exit 1
fi
mkdir -p "$OUT"
touch "$OUT/.start"

# name | xrprobe arguments | what to watch
CASES=(
"1-pace|--pace|Read the frame number on the panel. Does it ever flicker, tear (part of the panel from another frame), jump backwards or freeze on a stale number? Does the ticker bar under it stutter or jump back?"
"2-pace-fence|--pace=fence|Same panel, the control case. Same questions: any flicker, tearing, backwards or stale numbers? Compare with case 1."
"3-synth|--synth|A checkerboard slides left and right (new frames only 30 times a second). Is the motion smooth, or does it judder, double or smear? Turn your head a little too."
"4-synth-no-mv|--synth --no-mv|The same checkerboard and frame rate without motion vectors. Smoother or rougher than case 3?"
"5-depth-only|--depth-only|The checkerboard at full frame rate with depth submitted. Smooth? Any warping or wobble around its edges when you move your head?"
)

if [ "${PROBE_SET:-}" = replay ]; then
    CASES=()
    while IFS='|' read -r num name _ args; do
        case " ${PROBE_REPLAY_CASES:-1 2 3 4 5 6} " in *" $num "*) ;; *) continue ;; esac
        CASES+=("$name|$num|Look at a track-side edge, turn the head slowly left and right, hold still; one eye at a time. Doubled? Which eye?")
    done < <("$HERE/replay-case.sh" --list)
fi

echo "Probes: ${#CASES[@]} cases of ${RUN_SECONDS} s, logs in $OUT"
[ -f "$VRTIMING" ] || echo "(vrtiming.py not found at $VRTIMING: compositor counters will be missing)"
{
    echo "Probe session $(date -Iseconds)"
    echo "extra options: $*"
} > "$OUT/session.txt"

i=0
for c in "${CASES[@]}"; do
    i=$((i + 1))
    IFS='|' read -r name args watch <<< "$c"
    echo
    echo "=== Next: case $i of ${#CASES[@]}: $name (xrprobe $args $*)"
    echo "    Watch: $watch"
    for s in $(seq "$PAUSE" -1 1); do
        printf '\r    starting in %d s ' "$s"
        sleep 1
    done
    printf '\r    running for %s s       \n' "$RUN_SECONDS"

    vt_pid=""
    if [ -f "$VRTIMING" ]; then
        env -u LD_LIBRARY_PATH python3 -I "$VRTIMING" $((RUN_SECONDS + 5)) "$OUT/$name.vrtiming.log" \
            > /dev/null 2>&1 &
        vt_pid=$!
    fi
    # shellcheck disable=SC2086  # $args holds several options on purpose
    [ "${PROBE_MOCK:-0}" = 1 ] && export MOCK_XR_TRACE=$OUT/$name.trace
    [ "${PROBE_MOCK:-0}" = 1 ] && [ -n "${MOCK_XR_DUMP_AT:-}" ] && { export MOCK_XR_DUMP=$OUT/$name.dump; mkdir -p "$MOCK_XR_DUMP"; }
    if [ "${PROBE_SET:-}" = replay ]; then
        XRPROBE=$BIN PROBE_SECONDS=$RUN_SECONDS PROBE_LOG=$OUT/$name.log timeout -k 5 $((RUN_SECONDS + 90)) \
            "$HERE/replay-case.sh" "$args" "$@" > "$OUT/$name.stdout" 2>&1
    else
        timeout -k 5 $((RUN_SECONDS + 45)) "$BIN" $args "$@" --seconds "$RUN_SECONDS" --log "$OUT/$name.log" \
            > "$OUT/$name.stdout" 2>&1
    fi
    rc=$?
    if [ -n "$vt_pid" ]; then
        kill "$vt_pid" 2>/dev/null
        wait "$vt_pid" 2>/dev/null
    fi
    echo "$name rc=$rc" >> "$OUT/session.txt"
    if [ "$rc" -ne 0 ]; then
        echo "    case $name exited with code $rc; last lines:"
        tail -n 5 "$OUT/$name.stdout" | sed 's/^/      /'
    else
        grep -h 'SUMMARY\|xrmock: summary' "$OUT/$name.stdout" | sed 's/^/    /'
    fi
done

# SteamVR's client logs for these runs, if it wrote any
if [ "${PROBE_MOCK:-0}" != 1 ]; then
    find "$HOME/.local/share/Steam/logs" -maxdepth 1 -name 'xrclient_*' -newer "$OUT/.start" \
        -exec cp {} "$OUT/" \; 2>/dev/null
    [ -f "$HOME/.local/share/Steam/logs/perfcriteria.txt" ] && cp "$HOME/.local/share/Steam/logs/perfcriteria.txt" "$OUT/"
fi
rm -f "$OUT/.start"

echo
echo "Done. Logs in $OUT"
if [ "${PROBE_SET:-}" = replay ]; then
    echo "Tell the agent, per case: doubled on head turns or not, and in which eye."
else
    echo "Tell the agent, for cases 1 and 2: did the numbers ever flicker, tear or look stale?"
    echo "For cases 3, 4 and 5: which looked smooth?"
fi
