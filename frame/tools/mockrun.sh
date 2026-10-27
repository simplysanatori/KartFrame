#!/bin/bash
# Runs the game against the mock OpenXR runtime (xrmock), headless: no window, no sound.
#
#   frame/tools/mockrun.sh [--flat] [--seconds N] [--game DIR] [--exe PATH]
#
# Needs Xvfb (a virtual X server; nothing is shown anywhere). --flat runs without the mock: VR off, desktop renderer (title screen check).
# Environment: MOCK_XR_* settings of xrmock (MOCK_XR_SECONDS, MOCK_XR_HZ, MOCK_XR_TRACE, ...; MOCK_XR_STAMP
# also sets the game's KARTFRAME_FRAME_STAMP=1, MOCK_XR_PRESS the Frame controller profile),
# KF_BUILD (build dir, default <repo>/build-linux), KF_MOCKRUN_HOME (scratch user dir, default
# ~/.cache/kf-mockrun), KF_XRMOCK_DIR (where the mock is built; default one per checkout under
# ~/.cache/kf-xrmock, rebuilt when its sources change), KF_MOCKRUN_VIDEO (extra lines for the [video] section, e.g. 'gx_thread = true'), KF_MOCKRUN_VR (extra lines for the [vr]
# section, e.g. 'frame_pacing = "pipelined"'), KF_MOCKRUN_RENDER_SCALE (render_scale, default 1.0;
# 1.25 is the panel's native 2160 per eye), KF_MOCKRUN_INTRO=1 (the first-run welcome and controller
# guide show instead of being seeded done). Lane X: with KARTFRAME_END_PHASE set, a fake compositor grid for the
# game's end lock: frame/tools/vrtiming.py runs against frame/tools/openvr-stub.c (update window
# KF_MOCKRUN_STUB_UPDATE_MS, default 3.0,5.3 ms after each vsync; KF_MOCKRUN_PHASE_SECONDS stops it early to
# exercise the fallback) and the mock's wake-ups sit MOCK_XR_ALIGN_US (400) after the same vsync grid; the
# record is $OUT/phase.bin (KARTFRAME_PHASE_FILE, always set here so a mock run never reads a headset's grid). The game reads its user state from
# $XDG_DATA_HOME/WiiCompiled (Config.toml, NAND, Cache), which this script points at the scratch dir
# and seeds with a Config.toml naming the extracted game (~/Games/KartFrame/data/game or --game) and
# the Frame's VR defaults (render_scale 1.0, VR interpolation auto, plain menu background).
#
# Safety: XR_RUNTIME_JSON must name an existing, non-empty mock runtime, else the script aborts. An
# empty XR_RUNTIME_JSON once fell through to the real SteamVR in the headset.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
FLAT=0; SECONDS_RUN=60; GAME=$HOME/Games/KartFrame/data/game
EXE=${KF_BUILD:-$REPO/build-linux}/kartframe
while [ $# -gt 0 ]; do
  case $1 in
    --flat) FLAT=1 ;;
    --seconds) SECONDS_RUN=$2; shift ;;
    --game) GAME=$2; shift ;;
    --exe) EXE=$2; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "mockrun: $*" >&2; exit 1; }
[ -x "$EXE" ] || die "no game binary at $EXE"
[ -f "$GAME/.complete" ] || die "no extracted game ($GAME/.complete is missing)"
DATA=$GAME; [ -f "$GAME/sys/fst.bin" ] || DATA=$GAME/DATA
[ -f "$DATA/sys/fst.bin" ] && [ -d "$DATA/files" ] || die "$GAME is not an extracted DATA directory (sys/fst.bin, files/)"

# Never next to a running game or the owner's session.
# The game binary only (bin/kartframe, build-linux/kartframe, a bare kartframe), not toolchain
# or build processes under ~/.cache/kartframe.
RUNNING='SteamLaunch|lepton|waydroid|(^|[ /])kartframe( |$)'
M0=$HOME/.cache/headset-in-use
while [ -f "$M0" ] && kill -0 "$(awk '{print $2}' "$M0" 2>/dev/null)" 2>/dev/null; do echo "mockrun: headset in use, waiting" >&2; sleep 60; done
if pgrep -af "$RUNNING" | grep -Ev "pgrep|mockrun" >/dev/null; then
  pgrep -af "$RUNNING" | grep -Ev "pgrep|mockrun" >&2
  die "a game is running; wait for it"
fi

OUT=${KF_MOCKRUN_HOME:-$HOME/.cache/kf-mockrun}
mkdir -p "$OUT/data/WiiCompiled" "$OUT/logs"
cat > "$OUT/data/WiiCompiled/Config.toml" <<TOML
[paths]
dvd_root = "$DATA"

[video]
widescreen = true
display_mode = "windowed"
${KF_MOCKRUN_VIDEO:-}

[vr]
mirror_view = "none"
enabled = $([ $FLAT = 1 ] && echo false || echo true)
required = $([ $FLAT = 1 ] && echo false || echo true)
render_scale = ${KF_MOCKRUN_RENDER_SCALE:-1.0}
frame_interpolation_fps = 1
menu_shader_quality = 0
$([ "${KF_MOCKRUN_INTRO:-0}" = 1 ] && echo "# KF_MOCKRUN_INTRO=1: the first-run welcome and controller guide show." || printf '%s\n' \
  "# No first-run welcome or tutorial: the attract-mode demo race starts on its own." "welcome_complete = true" "tutorial_completed = 3")
${KF_MOCKRUN_VR:-}
TOML
LOG=$OUT/logs/$(date +%Y%m%d-%H%M%S)-$([ $FLAT = 1 ] && echo flat || echo mock).log

# Headless: nothing on the owner's displays, nothing audible. The game gets a private virtual X server
# (Xvfb on a display it picks itself) for its window; Dawn needs a window surface to choose the
# adapter, and KARTFRAME_HEADLESS=1 (aurora) never configures or presents that swapchain.
unset WAYLAND_DISPLAY
XVFB_DISPLAY_FILE=$OUT/xvfb.display; : > "$XVFB_DISPLAY_FILE"
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$XVFB_DISPLAY_FILE" >"$OUT/logs/xvfb.log" 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null' EXIT
for _ in $(seq 50); do [ -s "$XVFB_DISPLAY_FILE" ] && break; sleep 0.2; done
[ -s "$XVFB_DISPLAY_FILE" ] || die "Xvfb did not start (see $OUT/logs/xvfb.log)"
export DISPLAY=:$(head -1 "$XVFB_DISPLAY_FILE")
export SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=dummy KARTFRAME_HEADLESS=1
# The loader looks for explicit layers (the validation layer: VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation)
# under XDG_DATA_HOME, which points at the scratch dir below; keep the user's own layer folder findable.
[ -d "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/explicit_layer.d" ] &&
  export VK_ADD_LAYER_PATH="${VK_ADD_LAYER_PATH:+$VK_ADD_LAYER_PATH:}${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/explicit_layer.d"
export XDG_DATA_HOME=$OUT/data

if [ $FLAT = 0 ]; then
  MOCK_DIR=${KF_XRMOCK_DIR:-$HOME/.cache/kf-xrmock/$(printf %s "$REPO" | md5sum | cut -c1-8)}
  if [ ! -s "$MOCK_DIR/libxrmock.so" ] || [ -n "$(find "$HERE/xrmock" -newer "$MOCK_DIR/libxrmock.so" -name '*.[ch]' 2>/dev/null)" ]; then
    bash "$HERE/xrmock/build.sh" "$MOCK_DIR" >/dev/null || die "the mock runtime did not build"
  fi
  export XR_RUNTIME_JSON=$MOCK_DIR/runtime.json
  [ -n "${XR_RUNTIME_JSON}" ] && [ -s "$XR_RUNTIME_JSON" ] || die "XR_RUNTIME_JSON is empty or missing: $XR_RUNTIME_JSON"
  LIBPATH=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime"]["library_path"])' "$XR_RUNTIME_JSON")
  [ -s "$LIBPATH" ] || die "mock runtime library missing or empty: $LIBPATH"
  export MOCK_XR_SECONDS=${MOCK_XR_SECONDS:-$SECONDS_RUN}
  # The mock's image/pose check needs the game's stamp in each eye image.
  [ -n "${MOCK_XR_STAMP:-}" ] && export KARTFRAME_FRAME_STAMP=1
  # Scripted presses act on the Frame controller profile.
  [ -n "${MOCK_XR_PRESS:-}" ] && export MOCK_XR_PROFILE=${MOCK_XR_PROFILE:-/interaction_profiles/valve/frame_controller}
  export MOCK_XR_TRACE=${MOCK_XR_TRACE:-$OUT/logs/$(basename "$LOG" .log).trace}
  export KARTFRAME_PHASE_FILE=$OUT/phase.bin; rm -f "$KARTFRAME_PHASE_FILE"
  if [ -n "${KARTFRAME_END_PHASE:-}" ]; then
    STUB=$OUT/openvr-stub; mkdir -p "$STUB"
    cc -O1 -shared -fPIC -o "$STUB/libopenvr_api.so" "$HERE/openvr-stub.c" || die "the OpenVR stub did not build"
    export MOCK_XR_ALIGN_US=${MOCK_XR_ALIGN_US:-400}
    KARTFRAME_STEAMVR_DIR=$STUB STUB_UPDATE_MS=${KF_MOCKRUN_STUB_UPDATE_MS:-3.0,5.3} \
      python3 -I "$HERE/vrtiming.py" "${KF_MOCKRUN_PHASE_SECONDS:-$((SECONDS_RUN + 30))}" "$OUT/logs/stub-vrtiming.log" > /dev/null 2>&1 &
    PUB_PID=$!
    trap 'kill $XVFB_PID $PUB_PID 2>/dev/null' EXIT
  fi
else
  unset XR_RUNTIME_JSON
fi
echo "mockrun: $EXE ($([ $FLAT = 1 ] && echo flat || echo "mock runtime $XR_RUNTIME_JSON")), ${SECONDS_RUN}s, log $LOG"
set +e
# The game has no run time of its own: the mock ends the XR session after MOCK_XR_SECONDS and the
# game would carry on in desktop mode, so it is stopped (SIGTERM, then SIGKILL) shortly after.
STOP=$SECONDS_RUN; [ $FLAT = 0 ] && STOP=$((SECONDS_RUN + 8))
"$EXE" > "$LOG" 2>&1 &
PID=$!
( sleep "$STOP"; kill -TERM $PID 2>/dev/null; sleep 15; kill -KILL $PID 2>/dev/null ) &
WATCH=$!
# The owner may put the headset on mid-run (frame/kartframe.sh writes this file while a game runs):
# a headless run must not compete with it, so it stops at once.
MARK=$HOME/.cache/headset-in-use
( while kill -0 $PID 2>/dev/null; do
    if [ -f "$MARK" ] && kill -0 "$(awk '{print $2}' "$MARK" 2>/dev/null)" 2>/dev/null; then
      echo "mockrun: headset in use, stopping this run" >&2; kill -KILL $PID 2>/dev/null; break; fi
    sleep 2; done ) &
HWATCH=$!
wait $PID; RC=$?
kill $WATCH $HWATCH 2>/dev/null
echo "mockrun: game stopped (exit $RC; 143 is the stop signal)"
grep -a 'end phase: ' "$LOG" | sed 's/.*\(end phase: \)/\1/' | head -20 || true
grep -a 'xrmock: summary\|xrmock: end times\|xrmock: frames\|xrmock: queue drain\|xrmock: exact\|xrmock: stamps\|xrmock: end after wake-up\|xrmock: late-end model\|vr pacing totals' "$LOG" || true
echo "mockrun: session states: $(grep -a 'session state -> ' "$LOG" | sed 's/.*session state -> //' | tr '\n' ' ')"
FATAL=$(grep -a -c 'fatal dialog\|Signal 11' "$LOG")
echo "mockrun: fatal lines: $FATAL"
[ "$FATAL" = 0 ] || exit 1
if [ $FLAT = 0 ]; then
  grep -aq 'session state -> FOCUSED' "$LOG" || { echo "mockrun: FAIL, the session never reached FOCUSED" >&2; exit 1; }
  grep -aq 'xrmock: summary: 0 errors' "$LOG" || { echo "mockrun: FAIL, mock errors (see the trace)" >&2; exit 1; }
  # KF_MOCKRUN_END_GATE=<percent>: FAIL when more frames than that ended later than 1 ms after the mock's
  # wake-up (lane K: SteamVR showed such frames inconsistently on the headset, frame/perf/H7.md).
  if [ -n "${KF_MOCKRUN_END_GATE:-}" ]; then
    PCT=$(grep -a 'xrmock: end after wake-up' "$LOG" | sed -n 's/.*later than 1 ms [0-9]* (\([0-9.]*\) %).*/\1/p' | tail -1)
    [ -n "$PCT" ] || { echo "mockrun: FAIL, no end-after-wake-up line" >&2; exit 1; }
    awk -v p="$PCT" -v g="$KF_MOCKRUN_END_GATE" 'BEGIN{exit !(p <= g)}' ||
      { echo "mockrun: FAIL, $PCT % of frames ended later than 1 ms after the wake-up (gate $KF_MOCKRUN_END_GATE %)" >&2; exit 1; }
  fi
fi
echo "mockrun: ok"
