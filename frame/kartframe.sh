#!/bin/bash
# KartFrame launch wrapper, run by the Steam shortcut ("<this script> %command%"). There is no screen
# for messages in VR: everything goes to the session log, and a refusal is a non-zero exit.
#
# Every launch: re-applies SteamVR's per-app settings (it drops them now and then), starts the
# compositor timing logger, runs the game, then writes a session folder
# $KARTFRAME_HOME/sessions/<date-time>/ with launch.log, stdout.log, game.log, vrtiming.log, report.txt.
# $KARTFRAME_HOME/sessions/latest points at the newest one.
#
# Environment (all optional; session.env below is the way to set them from the Steam library):
#   KARTFRAME_HOME          data folder, default ~/.local/share/KartFrame (game/, build/, sessions/)
#   KARTFRAME_REFRESH_HZ    refresh rate, 90 or 120: set as SteamVR's per-app rate and passed to the game (default:
#                           [vr] refresh_rate of UserData/Config.toml, else 90). 90 is the default since session 24:
#                           at 1728 per eye 120 Hz overruns the GPU (SteamVR skipped frames in bursts). 90 Hz shows
#                           the game's 60 frames a second 1.5 refreshes each (interpolated)
#   KARTFRAME_GX_THREAD     1 (default) runs the host side of GX on its own thread (game thread 13.2 -> 8.5 ms a
#                           frame); 0 or off = the old single-thread path
#   KARTFRAME_INTERP_MAX_MS ms of gap across which the interpolation keeps the previous frame (default 33: a missed
#                           retrace glides instead of stepping); 0 or off = the game's own 1.5 intervals
#   KARTFRAME_WORKER_LOCK   narrow (default since session 25) holds the renderer lock only for a refresh set's setup
#                           (Grand Prix 53.9 -> 60.0 fps on the headset); full = the whole set under the lock, as before
#   KARTFRAME_EYE_PX        per-eye size asked of SteamVR (resolutionOverride), 1024 to 4096 (default 1728, SteamVR's
#                           Auto here; its 100 % is the panels' 2160). [vr] render_scale scales it
#   KARTFRAME_RENDER_SCALE  one launch's [vr] render_scale (eyes = SteamVR's size x this; 1.25 = 2160 at 1728)
#   KARTFRAME_FDM_LAYER=0   sets DISABLE_VULKAN_FDM_INJECTION_LAYER=1 (A/B: Valve's FDM layer off)
#   KARTFRAME_SYSMEM=0      leaves Turnip in tiled mode; default 1 sets TU_DEBUG=sysmem (measured: the
#                           race holds 120 Hz at 1728 only with it)
#   KARTFRAME_PACING        passed through to the game (auto | inline | pipelined)
#   KARTFRAME_END_PHASE     when in the refresh each frame is handed to SteamVR (default "after": just
#                           after the compositor's update, timed from the grid tools/vrtiming.py
#                           publishes; "off" = the wake-up, which doubles the image, see README)
#   KARTFRAME_STEREO_PACKED both eyes of a set in one swapchain (default 1, on: SteamVR then always shows
#                           the two eyes of the same set); 0 or off = one swapchain per eye (A/B, and the
#                           old right-eye double image, see README)
#   KARTFRAME_VRTIMING=0    no compositor timing log
#   KARTFRAME_THERMLOG=0    no thermal.tsv (hottest thermal zone, CPU and GPU clocks once a second)
#   KARTFRAME_SESSION_ENV_ONCE=1   (inside session.env) delete session.env after this launch
#   KARTFRAME_KEEP_SESSIONS number of session folders kept (default 30)
# Test hooks: KARTFRAME_VRCMD, KARTFRAME_STEAMVR_DIR, KARTFRAME_VRSETTINGS, KARTFRAME_VRTIMING_PY,
# KARTFRAME_REPORT_PY, KARTFRAME_EXE.
HERE=$(dirname "$(readlink -f "$0")")
KFHOME=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
STAMP=$(date +%Y%m%d-%H%M%S)
SESSION=$KFHOME/sessions/$STAMP
mkdir -p "$SESSION" || exit 1
ln -sfn "$STAMP" "$KFHOME/sessions/latest"
exec > "$SESSION/launch.log" 2>&1
log() { echo "$(date +%T) $*"; }
log "KartFrame wrapper, session $STAMP, steam appid '${SteamAppId:-unset}'"

# One launch's test environment: VAR=value lines (KARTFRAME_*, DISABLE_VULKAN_FDM_INJECTION_LAYER, ...).
# A queue of one-launch test settings (KFHOME/session.queue/*.env, taken in name order): when no
# session.env is waiting, the next queued file becomes this launch's session.env.
if [ ! -f "$KFHOME/session.env" ] && [ -d "$KFHOME/session.queue" ]; then
    NEXT=$(ls "$KFHOME/session.queue"/*.env 2> /dev/null | head -1)
    [ -n "$NEXT" ] && { mv "$NEXT" "$KFHOME/session.env"; log "session.queue: using $(basename "$NEXT")"; }
fi
if [ -f "$KFHOME/session.env" ]; then
    log "session.env:"; cat "$KFHOME/session.env"; cp "$KFHOME/session.env" "$SESSION/session.env"
    set -a; . "$KFHOME/session.env"; set +a
    [ "$KARTFRAME_SESSION_ENV_ONCE" = 1 ] && { rm -f "$KFHOME/session.env"; log "session.env deleted (KARTFRAME_SESSION_ENV_ONCE)"; }
fi
[ "$KARTFRAME_FDM_LAYER" = 0 ] && { export DISABLE_VULKAN_FDM_INJECTION_LAYER=1; log "A/B: Valve FDM layer disabled"; }
[ "${KARTFRAME_SYSMEM:-1}" = 1 ] && { export TU_DEBUG=sysmem; log "TU_DEBUG=sysmem (KARTFRAME_SYSMEM=0 turns it off)"; }
[ -n "$KARTFRAME_PACING" ] && { export KARTFRAME_PACING; log "KARTFRAME_PACING=$KARTFRAME_PACING"; }
EP=${KARTFRAME_END_PHASE:-after}
if [ "$EP" = off ] || [ "$EP" = 0 ]; then unset KARTFRAME_END_PHASE; log "KARTFRAME_END_PHASE=off: frames handed over at the wake-up"; else export KARTFRAME_END_PHASE=$EP; log "KARTFRAME_END_PHASE=$EP"; fi
SP=${KARTFRAME_STEREO_PACKED:-1}
if [ "$SP" = off ] || [ "$SP" = 0 ]; then export KARTFRAME_STEREO_PACKED=0; log "KARTFRAME_STEREO_PACKED=0: one swapchain per eye"; else export KARTFRAME_STEREO_PACKED=1; log "KARTFRAME_STEREO_PACKED=1: both eyes of a set in one swapchain"; fi

# GX thread (the game reads KARTFRAME_GX_THREAD=0|1 only, anything else it ignores): on unless 0 or off.
GXT=${KARTFRAME_GX_THREAD:-1}
if [ "$GXT" = off ] || [ "$GXT" = 0 ]; then export KARTFRAME_GX_THREAD=0; log "KARTFRAME_GX_THREAD=0: GX on the game thread"; else export KARTFRAME_GX_THREAD=1; log "KARTFRAME_GX_THREAD=1: GX thread on (0 turns it off)"; fi
# Interpolation window: 33 ms unless set (a number of ms) or 0 / off. Read by the renderer, 1 to 100 ms.
IM=${KARTFRAME_INTERP_MAX_MS:-33}
if [ "$IM" = off ] || [ "$IM" = 0 ]; then unset KARTFRAME_INTERP_MAX_MS; log "KARTFRAME_INTERP_MAX_MS=off: previous frame kept across 1.5 intervals only"
elif [[ "$IM" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then export KARTFRAME_INTERP_MAX_MS=$IM; log "KARTFRAME_INTERP_MAX_MS=$IM (0 turns it off)"
else log "KARTFRAME_INTERP_MAX_MS=$IM is not a number of ms, using 33"; export KARTFRAME_INTERP_MAX_MS=33; fi
# Frame worker lock (the game reads KARTFRAME_WORKER_LOCK=narrow only; anything else is the full lock): narrow unless full.
WL=${KARTFRAME_WORKER_LOCK:-narrow}
if [ "$WL" = full ]; then export KARTFRAME_WORKER_LOCK=full; log "KARTFRAME_WORKER_LOCK=full: refresh sets hold the renderer lock throughout"
else [ "$WL" = narrow ] || log "KARTFRAME_WORKER_LOCK=$WL is not narrow or full, using narrow"; export KARTFRAME_WORKER_LOCK=narrow; log "KARTFRAME_WORKER_LOCK=narrow (full holds the lock for the whole set)"; fi

# The game: Steam's %command% (the binary the shortcut names), else the installed one.
EXE=${KARTFRAME_EXE:-${1:-$KFHOME/bin/kartframe}}
[ $# -gt 0 ] && shift
[ -x "$EXE" ] || { log "REFUSING TO START: no game binary at $EXE (run frame/setup.sh)"; exit 1; }

# Kit check: the installed binary must have been built against the kit that is installed now.
# Installed kit: <app>/kit/kit.json "fingerprint"; the built game's record: $KFHOME/bin/kartframe.fingerprint.
# kit_fingerprint FILE prints the "fingerprint" of a kit.json, or the first line of a plain file.
kit_fingerprint() {
    python3 -I -c '
import json, sys
t = open(sys.argv[1]).read()
try:
    print(json.loads(t)["fingerprint"])
except Exception:
    print(t.split("\n")[0].strip())' "$1" 2> /dev/null
}
kit_check() {  # prints the reason and returns 1 when the binary and the kit disagree
    local kit= built=
    [ -f "$HERE/kit/kit.json" ] && kit=$HERE/kit/kit.json
    [ -f "$KFHOME/bin/kartframe.fingerprint" ] && built=$KFHOME/bin/kartframe.fingerprint
    [ -n "$kit" ] || { log "kit check: no kit fingerprint installed (development build), skipped"; return 0; }
    [ -n "$built" ] || { echo "no record of the kit this binary was built against; run frame/setup.sh again"; return 1; }
    local k b
    k=$(kit_fingerprint "$kit"); b=$(kit_fingerprint "$built")
    [ -n "$k" ] && [ "$k" = "$b" ] && { log "kit check: ok (${k:0:12})"; return 0; }
    echo "the game was built for another kit (${b:0:12} vs installed ${k:0:12}); run frame/setup.sh again"; return 1
}
if ! why=$(kit_check); then log "REFUSING TO START: $why"; exit 78; fi
[ -n "$why" ] && echo "$why"

# Portable mode: portable.txt in KFHOME (a parent of bin/) makes the game's root $KFHOME/UserData.
STATE=$KFHOME/UserData
mkdir -p "$STATE"; [ -f "$KFHOME/portable.txt" ] || : > "$KFHOME/portable.txt"
if [ ! -f "$STATE/Config.toml" ] && [ -f "$HERE/Config.frame.toml" ]; then
    sed "s|@DVD_ROOT@|$KFHOME/game/DATA|" "$HERE/Config.frame.toml" > "$STATE/Config.toml" &&
        log "seeded $STATE/Config.toml"
fi

# SteamVR's per-app settings, every launch. SteamAppId is set by Steam for a library launch.
# The refresh rate: KARTFRAME_REFRESH_HZ, else the config's [vr] refresh_rate, else 120. SteamVR offers the game
# only the rate its per-app setting runs, so the wrapper sets that rate and hands the same one to the game.
HZ_FROM=KARTFRAME_REFRESH_HZ HZ=$KARTFRAME_REFRESH_HZ
if [ -z "$HZ" ]; then
    HZ_FROM="Config.toml [vr] refresh_rate"
    HZ=$(awk -F= '/^[ \t]*\[/ { vr = ($0 ~ /^[ \t]*\[vr\][ \t]*$/) } vr && $1 ~ /^[ \t]*refresh_rate[ \t]*$/ {
        v = $2; sub(/#.*/, "", v); gsub(/[ \t"]/, "", v); print v; exit }' "$STATE/Config.toml" 2> /dev/null)
    [ -n "$HZ" ] || { HZ_FROM=default; HZ=90; }
fi
case "$HZ" in 90 | 90.0 | 120 | 120.0) HZ=${HZ%.0} ;; *) log "refresh rate $HZ ($HZ_FROM) is not 90 or 120, using 90"; HZ=90 HZ_FROM=default ;; esac
export KARTFRAME_REFRESH_HZ=$HZ
log "refresh rate $HZ Hz ($HZ_FROM)"
# The per-eye size SteamVR gives the game (its recommended view size); the game multiplies it by render_scale.
EYE_PX=${KARTFRAME_EYE_PX:-1728}
if ! [[ "$EYE_PX" =~ ^[0-9]+$ ]] || [ "$EYE_PX" -lt 1024 ] || [ "$EYE_PX" -gt 4096 ]; then
    log "KARTFRAME_EYE_PX=$EYE_PX is not a size from 1024 to 4096, using 1728"; EYE_PX=1728
fi
[ -n "$KARTFRAME_RENDER_SCALE" ] && export KARTFRAME_RENDER_SCALE
log "eye size asked of SteamVR: $EYE_PX (SteamVR: Auto 1728, 100 % 2160), render_scale ${KARTFRAME_RENDER_SCALE:-from Config.toml}"
VRCMD=${KARTFRAME_VRCMD:-$(command -v vrcmd || echo /opt/steamvr/bin/linuxarm64/vrcmd)}
VRSETTINGS=${KARTFRAME_VRSETTINGS:-$HOME/.config/openvr/config/steamvr.vrsettings}
STEAMVR_DIR=${KARTFRAME_STEAMVR_DIR:-/opt/steamvr/bin/linuxarm64}
SETTINGS="preferredRefreshRate=$HZ framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=$EYE_PX"
if [ -z "$SteamAppId" ]; then
    log "no SteamAppId: SteamVR per-app settings NOT applied (not a Steam library launch)"
elif [ ! -x "$VRCMD" ]; then
    log "no vrcmd at $VRCMD: SteamVR per-app settings NOT applied"
else
    for kv in $SETTINGS; do
        "$VRCMD" --set-settings-int "steam.app.$SteamAppId.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
            log "WARNING: vrcmd could not set ${kv%=*} (is SteamVR running?)"
    done
    sleep 0.3   # SteamVR writes its settings file shortly after a change
    # Read back from SteamVR's settings file and log every difference.
    python3 -I - "$VRSETTINGS" "steam.app.$SteamAppId" $SETTINGS << 'PY' || log "settings read-back failed"
import json, sys, time
path, section, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
try:
    got = json.load(open(path)).get(section, {})
except (OSError, ValueError) as e:
    print(f"{time.strftime('%T')} read-back: cannot read {path} ({e.__class__.__name__}); settings unverified")
    sys.exit(0)
bad = 0
for p in pairs:
    k, v = p.split("=")
    if str(got.get(k)) != v:
        bad += 1
        print(f"{time.strftime('%T')} MISMATCH {section}.{k}: wanted {v}, SteamVR has {got.get(k)}")
print(f"{time.strftime('%T')} read-back: {len(pairs) - bad} of {len(pairs)} per-app settings as asked")
PY
fi

# Compositor timing for the whole session; it cannot stop the game from starting.
VRT_PID=
VRT=${KARTFRAME_VRTIMING_PY:-$HERE/tools/vrtiming.py}
if [ "${KARTFRAME_VRTIMING:-1}" != 0 ] && [ -f "$VRT" ] && [ -f "$STEAMVR_DIR/libopenvr_api.so" ]; then
    env -u LD_LIBRARY_PATH python3 -I "$VRT" 86400 "$SESSION/vrtiming.log" > /dev/null 2>&1 &
    VRT_PID=$!
else
    log "no vrtiming.log (disabled, or vrtiming.py / libopenvr_api.so missing)"
fi

# Heat and clocks once a second (lane AJ): thermal.tsv = time, hottest zone (C) and its type, each CPU's
# current clock (MHz, cpu0..), the GPU clock (MHz). Shell builtins only (no process per sample).
THERM_PID=
if [ "${KARTFRAME_THERMLOG:-1}" != 0 ]; then
    (
        zones=(/sys/class/thermal/thermal_zone*); types=()
        for z in "${zones[@]}"; do t=?; read -r t < "$z/type" 2> /dev/null; types+=("$t"); done
        cpus=(/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq)
        gpu=$(ls -d /sys/class/devfreq/*.gpu 2> /dev/null | head -1)
        echo "# time hot_C zone_type MHz:${cpus[*]//[!0-9 ]/} gpu_MHz"
        while :; do
            hot=-999999; ht=?; i=0
            for z in "${zones[@]}"; do v=; read -r v < "$z/temp" 2> /dev/null
                [ -n "$v" ] && [ "$v" -gt "$hot" ] 2> /dev/null && { hot=$v; ht=${types[$i]}; }; i=$((i + 1)); done
            line="$EPOCHREALTIME $((hot / 1000)).$(((hot % 1000) / 100)) $ht"
            for c in "${cpus[@]}"; do v=0; read -r v < "$c" 2> /dev/null; line+=" $((v / 1000))"; done
            v=0; [ -n "$gpu" ] && read -r v < "$gpu/cur_freq" 2> /dev/null; line+=" $((v / 1000000))"
            echo "$line"; sleep 1
        done
    ) > "$SESSION/thermal.tsv" 2> /dev/null &
    THERM_PID=$!
fi

# Not exec: the wrapper stays to stop the logger and write the report. A signal for the wrapper
# (Steam stopping the app) goes on to the game.
# The headset is in use: development builds and headless GPU runs on this machine wait while this
# file exists (KartFrame's build script does; other projects can check the same file).
HEADSET_MARK=${XDG_CACHE_HOME:-$HOME/.cache}/headset-in-use
log "starting $EXE"
touch "$SESSION/.start"
"$EXE" "$@" > "$SESSION/stdout.log" 2>&1 &
GAME_PID=$!
echo "kartframe $GAME_PID" > "$HEADSET_MARK" 2> /dev/null
# KARTFRAME_RECORD=raw records both panels as scanned out (framecorder; about 1.6 ms of GPU a frame)
# into the session folder, for questions the logs cannot settle (per-eye ghosting, flicker).
# The recorder starts 8 s in and only if the game is still there then (it once started after a game
# that had died at 3 s and recorded the headset for hours). It runs in its own session (setsid), so
# stopping it can reach everything it started (the framecorder-grab helper too). .rec-started marks
# that it really began.
REC_PID=
# Steam starts games with a short PATH (no ~/.local/bin), so look for the recorder by its full path too.
FRAMECORDER=$(command -v framecorder 2> /dev/null || ls "$HOME/.local/bin/framecorder" 2> /dev/null)
if [ -n "$KARTFRAME_RECORD" ] && [ -z "$FRAMECORDER" ]; then log "RECORDING NOT POSSIBLE: framecorder not found"; fi
if [ -n "$KARTFRAME_RECORD" ] && [ -n "$FRAMECORDER" ]; then
    rm -f "$SESSION/.rec-started"
    setsid bash -c 'for ((i = 0; i < 80; i++)); do kill -0 "$1" 2> /dev/null || exit 0; sleep 0.1; done
        touch "$2/.rec-started"; shift 2; exec "$@"' _ "$GAME_PID" "$SESSION" \
        "$FRAMECORDER" --view "${KARTFRAME_RECORD}" --fps "${KARTFRAME_RECORD_FPS:-120}" --qp 18 "$SESSION/panels.mkv" \
        > "$SESSION/framecorder.log" 2>&1 &
    REC_PID=$!
    log "recording the panels (framecorder --view $KARTFRAME_RECORD) into panels.mkv"
fi
# Anything of the recorder still alive (a finished process that nobody has waited for does not count).
rec_alive() {
    ps -o stat= --sid "$REC_PGID" 2> /dev/null | grep -q '^[^Z]' ||
        pgrep -f "framecorder .* $SESSION/panels.mkv" > /dev/null
}
# Stops the recorder in every case: not started yet (the waiting subshell is killed and never starts it),
# running (SIGINT to finish the file, then a bounded wait, then TERM and KILL). Runs once.
stop_recorder() {
    [ -n "$REC_PID" ] || return 0
    REC_PGID=$REC_PID; REC_PID=
    local n how=stopped
    if [ -f "$SESSION/.rec-started" ]; then
        kill -INT "$REC_PGID" 2> /dev/null
        for ((n = 0; n < 300; n++)); do rec_alive || break; sleep 0.1; done   # up to 30 s to finish the file
        if rec_alive; then
            how="stopped after TERM (SIGINT did not finish it)"
            kill -TERM -- "-$REC_PGID" 2> /dev/null; pkill -TERM -s "$REC_PGID" 2> /dev/null
            pkill -TERM -f "framecorder .* $SESSION/panels.mkv" 2> /dev/null
            for ((n = 0; n < 50; n++)); do rec_alive || break; sleep 0.1; done
        fi
        if rec_alive; then
            how="killed (SIGINT and TERM did not stop it)"
            kill -KILL -- "-$REC_PGID" 2> /dev/null; pkill -KILL -s "$REC_PGID" 2> /dev/null
            pkill -KILL -f "framecorder .* $SESSION/panels.mkv" 2> /dev/null
            sleep 0.5
        fi
        wait "$REC_PGID" 2> /dev/null
        rec_alive && log "WARNING: a recorder process is still alive for $SESSION/panels.mkv"
        log "recorder $how"
    else
        kill -TERM "$REC_PGID" 2> /dev/null; kill -TERM -- "-$REC_PGID" 2> /dev/null
        pkill -TERM -s "$REC_PGID" 2> /dev/null
        wait "$REC_PGID" 2> /dev/null
        log "recorder was never started (the game was gone before the 8 s were up)"
    fi
    # A leftover part file is the owner's to judge: logged, never deleted.
    if [ -f "$SESSION/panels.mkv.part" ] && ! rec_alive; then
        log "panels.mkv.part left in the session folder, $(stat -c %s "$SESSION/panels.mkv.part") bytes, no recorder alive"
    fi
}
trap 'kill -TERM "$GAME_PID" 2> /dev/null; stop_recorder' TERM INT HUP
rc=0
while kill -0 "$GAME_PID" 2> /dev/null; do
    wait "$GAME_PID"; rc=$?
done
trap - TERM INT HUP
rm -f "$HEADSET_MARK"
stop_recorder
log "game exited with status $rc"
[ -n "$VRT_PID" ] && { kill "$VRT_PID" 2> /dev/null; wait "$VRT_PID" 2> /dev/null; }
[ -n "$THERM_PID" ] && { kill "$THERM_PID" 2> /dev/null; wait "$THERM_PID" 2> /dev/null; }

# The game's own log: the newest console.log of this session, else what it printed.
GL=$(find "$STATE/Logs" -name console.log -newer "$SESSION/.start" 2> /dev/null | xargs -r ls -t 2> /dev/null | head -1)
if [ -n "$GL" ]; then cp "$GL" "$SESSION/game.log"; else log "no console.log from this run; the report reads stdout.log"; cp "$SESSION/stdout.log" "$SESSION/game.log"; fi
rm -f "$SESSION/.start"
REPORT=${KARTFRAME_REPORT_PY:-$HERE/tools/session-report.py}
if [ -f "$REPORT" ]; then
    set -- "$SESSION/game.log"; [ -s "$SESSION/vrtiming.log" ] && set -- "$@" "$SESSION/vrtiming.log"
    python3 -I "$REPORT" "$@" > "$SESSION/report.txt" 2>&1 || log "session-report exited non-zero"
else
    log "no session-report.py at $REPORT"
fi
# Keep the newest sessions only.
KEEP=${KARTFRAME_KEEP_SESSIONS:-30}
ls -1d "$KFHOME"/sessions/2* 2> /dev/null | head -n -"$KEEP" | xargs -r rm -rf
exit $rc
