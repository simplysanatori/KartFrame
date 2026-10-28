#!/bin/bash
# Tests for install.sh, uninstall.sh, kartframe.sh and Config.frame.toml. Touches nothing real: a
# scratch Steam folder (a copy of shortcuts.vdf if there is one), a fake vrcmd on PATH, a fake game.
#   frame/test-installer.sh [SCRATCH_DIR]      (default ~/.cache/kf-test-installer, emptied first)
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
T=${1:-$HOME/.cache/kf-test-installer}
rm -rf "$T"; mkdir -p "$T"
fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
check() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
has() { grep -q -- "$2" "$1"; }
REALHOME=$HOME
export KARTFRAME_SKIP_STEAM= HOME=$T/home XDG_DATA_HOME=$T/home/xdg   # XDG must stay unused
mkdir -p "$HOME"

# ---- Config.frame.toml: every key is one the parser reads, every value valid ----
python3 -I - "$HERE/Config.frame.toml" "$REPO/runtime/include/runtime_config.h" "$REPO/runtime/src/vr/openxr_integration.cpp" << 'PY' && ok "Config.frame.toml keys and values" || bad "Config.frame.toml keys and values"
import re, sys, tomllib
cfg = tomllib.load(open(sys.argv[1], "rb"))
src = open(sys.argv[2]).read()
known = set(re.findall(r'FindConfig\w*(?:<[^>]*>)?\(\s*document,\s*"(\w+)",\s*"(\w+)"', src))
known |= set(re.findall(r'readRangedFloat\("(\w+)"', src)) and {("vr", k) for k in re.findall(r'readRangedFloat\("(\w+)"', src)}
# [vr] keys the pacing code reads itself (openxr_integration.cpp ReadVrConfigText).
known |= {("vr", k) for k in re.findall(r'ReadVrConfigText\("(\w+)"', open(sys.argv[3]).read())} if len(sys.argv) > 3 else set()
errs = []
for sec, kv in cfg.items():
    for k, v in kv.items():
        if (sec, k) not in known: errs.append(f"unknown key {sec}.{k}")
v = cfg["vr"]
want = {("vr", "render_scale"): 1.0, ("video", "resolution_multiplier"): 1.0, ("vr", "frame_interpolation_fps"): 1,
        ("vr", "menu_shader_quality"): 0, ("vr", "hand_steering"): True, ("vr", "foveation"): "off",
        ("vr", "passthrough"): False, ("vr", "adaptive_resolution"): False, ("diagnostics", "openxr_logging"): False}
for (s, k), w in want.items():
    if cfg[s][k] != w: errs.append(f"{s}.{k} = {cfg[s][k]!r}, want {w!r}")
if v["foveation"] not in ("off", "low", "medium", "high"): errs.append("foveation value")
if not 0 <= v["menu_shader_quality"] <= 3: errs.append("menu_shader_quality range")
if v["render_scale"] <= 0: errs.append("render_scale")
for s, k in (("vr", "hud_distance_meters"), ("vr", "hud_width_meters")):
    if not isinstance(cfg[s][k], float) or cfg[s][k] <= 0: errs.append(k)
print("\n".join(errs)); sys.exit(1 if errs else 0)
PY

# ---- install.sh into a copied Steam folder ----
STEAM=$T/steam; mkdir -p "$STEAM/userdata/1/config"
REAL=$(ls "$REALHOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf 2> /dev/null | head -1)   # read only, copied
[ -n "$REAL" ] && cp "$REAL" "$STEAM/userdata/1/config/shortcuts.vdf"
VDF=$STEAM/userdata/1/config/shortcuts.vdf
count() { python3 -I -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
d = m.vdf_load(sys.argv[2])["shortcuts"]
print(sum(1 for v in d.values() if v.get("AppName") == sys.argv[3]) if len(sys.argv) > 3 else len(d))' "$HERE/steam-cef.py" "$VDF" "$@"; }
field() { python3 -I -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
d = m.vdf_load(sys.argv[2])["shortcuts"]
print(next(v[sys.argv[4]] for v in d.values() if v.get("AppName") == sys.argv[3]))' "$HERE/steam-cef.py" "$VDF" "$@"; }
# The copy of the owner's Steam config may already hold KartFrame shortcuts (a real dev install):
# remove them from the copy so the counts below start from a clean state.
python3 -I -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
data = m.vdf_load(sys.argv[2]); d = data["shortcuts"]
keep = [v for v in d.values() if not str(v.get("AppName", "")).startswith("KartFrame")]
data["shortcuts"] = {str(i): v for i, v in enumerate(keep)}
m.vdf_save(sys.argv[2], data)' "$HERE/steam-cef.py" "$VDF"
BEFORE=$(count)
BUILD=$T/build; mkdir -p "$BUILD/resources" "$BUILD/wii_bootstrap"
printf '#!/bin/bash\necho fake game\n' > "$BUILD/kartframe"; chmod +x "$BUILD/kartframe"
echo x > "$BUILD/dsp_coef.bin"; echo x > "$BUILD/resources/a"
export KARTFRAME_APP=$T/app KARTFRAME_HOME=$T/kfhome KARTFRAME_STEAM_DIR=$STEAM
"$HERE/install.sh" "$BUILD" > "$T/install1.log" 2>&1; check "install.sh from a build tree exits 0" test $? -eq 0
K=$T/kfhome
check "binary and resources copied to KARTFRAME_HOME/bin" test -x "$K/bin/kartframe" -a -f "$K/bin/dsp_coef.bin" -a -f "$K/bin/resources/a"
check "portable.txt and seeded UserData/Config.toml" test -f "$K/portable.txt" && has "$K/UserData/Config.toml" "dvd_root = \"$K/game/DATA\""
check "wrapper, tools, config installed" test -x "$T/app/kartframe.sh" -a -f "$T/app/tools/vrtiming.py" -a -f "$T/app/tools/session-report.py" -a -f "$T/app/Config.frame.toml" -a ! -e "$T/app/kartframe"
check "shortcut added once" test "$(count KartFrame)" = 1
check "shortcut count grew by one" test "$(count)" = $((BEFORE + 1))
check "launch options run the wrapper" test "$(field KartFrame LaunchOptions)" = "\"$T/app/kartframe.sh\" %command%"
check "exe is the installed binary" test "$(field KartFrame Exe)" = "\"$K/bin/kartframe\""
"$HERE/install.sh" "$BUILD" > "$T/install2.log" 2>&1
check "second install updates, not duplicates" test "$(count KartFrame)" = 1
mkdir -p "$T/art"; echo png > "$T/art/p.png"; echo png > "$T/art/icon.png"
"$HERE/install.sh" --art "$T/art" "$BUILD" > "$T/install3.log" 2>&1
APPID=$(field KartFrame appid)
check "art copied under the appid" test -f "$STEAM/userdata/1/config/grid/${APPID}p.png" -a -f "$STEAM/userdata/1/config/grid/${APPID}_icon.png"
KARTFRAME_APP=$T/app-dev "$HERE/install.sh" --dev "$BUILD" > "$T/install4.log" 2>&1
check "--dev adds a separate shortcut" test "$(count 'KartFrame (dev)')" = 1 -a "$(count KartFrame)" = 1
check "--dev copies the build to bin and points at it" test "$(field 'KartFrame (dev)' Exe)" = "\"$K/bin/kartframe\"" -a -x "$T/app-dev/kartframe.sh"
# a tarball (top folder or flat)
mkdir -p "$T/tb/kartframe-1"; cp -r "$BUILD"/* "$T/tb/kartframe-1/"; tar -czf "$T/rel.tar.gz" -C "$T/tb" kartframe-1
rm -rf "$T/app"; "$HERE/install.sh" "$T/rel.tar.gz" > "$T/install5.log" 2>&1
check "tarball unpacks into the app dir only" test -x "$T/app/kartframe" -a -f "$T/app/resources/a"
mkdir -p "$T/kf"; mkdir -p "$KARTFRAME_HOME/game"; mkdir -p "$HOME/.local/share/WiiCompiled"; echo mine > "$HOME/.local/share/WiiCompiled/x"; echo save > "$KARTFRAME_HOME/keep.txt"
"$HERE/uninstall.sh" > "$T/un1.log" 2>&1
check "uninstall removes shortcut and app" test "$(count KartFrame)" = 0 -a ! -d "$T/app"
check "uninstall keeps the dev shortcut and the user data" test "$(count 'KartFrame (dev)')" = 1 -a -f "$KARTFRAME_HOME/keep.txt"
KARTFRAME_APP=$T/app-dev "$HERE/uninstall.sh" --dev > "$T/un2.log" 2>&1
check "uninstall --dev removes it" test "$(count 'KartFrame (dev)')" = 0 -a "$(count)" = "$BEFORE"
"$HERE/uninstall.sh" --purge > "$T/un3.log" 2>&1
check "--purge removes KARTFRAME_HOME only" test ! -e "$KARTFRAME_HOME" -a -f "$HOME/.local/share/WiiCompiled/x"

# ---- kartframe.sh with a fake vrcmd, fake vrtiming and a fake game ----
unset KARTFRAME_STEAM_DIR
W=$T/w; mkdir -p "$W/bin" "$W/app/tools" "$W/openvr" "$W/svr" "$W/home/bin"
export KARTFRAME_HOME=$W/home KARTFRAME_STEAMVR_DIR=$W/svr KARTFRAME_VRSETTINGS=$W/vrsettings.json
touch "$W/svr/libopenvr_api.so"
cp "$HERE/kartframe.sh" "$HERE/Config.frame.toml" "$W/app/"; cp "$HERE/tools/session-report.py" "$W/app/tools/"
cat > "$W/app/tools/vrtiming.py" << 'PY'
import sys, time
open(sys.argv[2], "w").write("fake vrtiming\n"); time.sleep(60)
PY
cat > "$W/bin/vrcmd" << 'SH'
#!/bin/bash
echo "$@" >> "$FAKE_VRCMD_LOG"
[ -n "$FAKE_VRCMD_NOWRITE" ] && exit 0
python3 -I - "$2" "$3" "$FAKE_VRSETTINGS" << 'PY'
import json, os, sys
key, val, path = sys.argv[1:]
sec, name = key.rsplit(".", 1)
d = json.load(open(path)) if os.path.exists(path) else {}
d.setdefault(sec, {})[name] = int(val)
json.dump(d, open(path, "w"))
PY
SH
cat > "$W/home/bin/kartframe" << 'SH'
#!/bin/bash
D=$(dirname "$0")/../UserData/Logs/run1; mkdir -p "$D"; [ -f "$(dirname "$0")/../portable.txt" ] || echo "NO PORTABLE" > "$D/bad"
{ echo "env FDM=$DISABLE_VULKAN_FDM_INJECTION_LAYER TU=$TU_DEBUG PACING=$KARTFRAME_PACING HZ=$KARTFRAME_REFRESH_HZ SCALE=$KARTFRAME_RENDER_SCALE GX=$KARTFRAME_GX_THREAD IM=$KARTFRAME_INTERP_MAX_MS"; echo "args: $*"; } > "$D/console.log"
echo "game stdout"; sleep 3
SH
chmod +x "$W/bin/vrcmd" "$W/home/bin/kartframe" "$W/app/kartframe.sh"
export FAKE_VRCMD_LOG=$W/vrcmd.log FAKE_VRSETTINGS=$KARTFRAME_VRSETTINGS
run() { env PATH="$W/bin:$PATH" SteamAppId=4242 "$@" "$W/app/kartframe.sh" "$W/home/bin/kartframe" --steamarg > "$W/out.txt" 2>&1; echo $? > "$W/rc"; }
run
S=$KARTFRAME_HOME/sessions/latest
check "wrapper exits with the game's status" test "$(cat "$W/rc")" = 0
for k in preferredRefreshRate=90 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=1728; do
    check "vrcmd --set-settings-int steam.app.4242.${k%=*} ${k#*=}" has "$W/vrcmd.log" "^--set-settings-int steam.app.4242.${k%=*} ${k#*=}\$"
done
check "read-back logged as matching" has "$S/launch.log" "read-back: 5 of 5"
check "game log copied into the session" has "$S/game.log" "args: --steamarg"
check "vrtiming.log written" has "$S/vrtiming.log" "fake vrtiming"
check "report.txt written" test -s "$S/report.txt"
check "stdout.log written" has "$S/stdout.log" "game stdout"
check "portable.txt made" test -f "$KARTFRAME_HOME/portable.txt"
check "wrapper never uses WiiCompiled/XDG" test ! -e "$XDG_DATA_HOME/WiiCompiled"
check "Config.toml seeded with the game path" has "$KARTFRAME_HOME/UserData/Config.toml" "dvd_root = \"$KARTFRAME_HOME/game/DATA\""
check "vrtiming logger stopped at exit" test -z "$(pgrep -f "$W/app/tools/vrtiming.py")"
check "defaults: sysmem on, no other switches" has "$S/game.log" "env FDM= TU=sysmem PACING="
check "defaults: 90 Hz handed to the game, no render_scale override" has "$S/game.log" "PACING= HZ=90 SCALE= "
check "defaults: GX thread on, interpolation window 33 ms" has "$S/game.log" "GX=1 IM=33$"
check "defaults logged" has "$S/launch.log" "KARTFRAME_GX_THREAD=1: GX thread on"
sleep 1
run KARTFRAME_GX_THREAD=0 KARTFRAME_INTERP_MAX_MS=0
check "KARTFRAME_GX_THREAD=0 turns the GX thread off, INTERP_MAX_MS=0 the window" has "$S/game.log" "GX=0 IM=$"
sleep 1
run KARTFRAME_GX_THREAD=off KARTFRAME_INTERP_MAX_MS=50
check "GX thread off spelled out, window 50" has "$S/game.log" "GX=0 IM=50$"
sleep 1
run KARTFRAME_GX_THREAD=1 KARTFRAME_INTERP_MAX_MS=junk
check "a bad window falls back to 33" has "$S/game.log" "GX=1 IM=33$"
sleep 1
run KARTFRAME_REFRESH_HZ=120
check "KARTFRAME_REFRESH_HZ=120 overrides the 90 default" has "$S/game.log" "HZ=120 "
sleep 1
run KARTFRAME_SYSMEM=0
check "KARTFRAME_SYSMEM=0 turns sysmem off" has "$S/game.log" "env FDM= TU= PACING="
sleep 1
# mismatch logged
echo '{"steam.app.4242": {"preferredRefreshRate": 72, "framesToThrottle": 0}}' > "$KARTFRAME_VRSETTINGS"; run FAKE_VRCMD_NOWRITE=1
check "mismatch logged when SteamVR dropped the settings" has "$KARTFRAME_HOME/sessions/latest/launch.log" "MISMATCH steam.app.4242.preferredRefreshRate"
check "missing settings logged as mismatches, not as passed" has "$KARTFRAME_HOME/sessions/latest/launch.log" "read-back: 1 of 5"
sleep 1
# switches and session.env
printf 'KARTFRAME_SYSMEM=1\nKARTFRAME_SESSION_ENV_ONCE=1\n' > "$KARTFRAME_HOME/session.env"
run KARTFRAME_FDM_LAYER=0 KARTFRAME_PACING=pipelined KARTFRAME_REFRESH_HZ=90
S=$KARTFRAME_HOME/sessions/latest
check "A/B switches reach the game" has "$S/game.log" "env FDM=1 TU=sysmem PACING=pipelined"
check "90 Hz choice applied" has "$W/vrcmd.log" "preferredRefreshRate 90"
check "session.env deleted after use, copy kept" test ! -f "$KARTFRAME_HOME/session.env" -a -f "$S/session.env"
sleep 1
# refresh rate from the config, eye size and render_scale switches (lane AG)
sed -i 's/^refresh_rate = .*/refresh_rate = 120/' "$KARTFRAME_HOME/UserData/Config.toml"; : > "$FAKE_VRCMD_LOG"
run KARTFRAME_EYE_PX=2048 KARTFRAME_RENDER_SCALE=1.25
S=$KARTFRAME_HOME/sessions/latest
check "Config.toml refresh_rate 120 reaches SteamVR" has "$W/vrcmd.log" "preferredRefreshRate 120"
check "and the game" has "$S/game.log" "HZ=120 SCALE=1.25"
check "KARTFRAME_EYE_PX sets SteamVR's size" has "$W/vrcmd.log" "resolutionOverride 2048"
sleep 1
: > "$FAKE_VRCMD_LOG"; run KARTFRAME_EYE_PX=big KARTFRAME_REFRESH_HZ=75
S=$KARTFRAME_HOME/sessions/latest
check "a bad eye size falls back to 1728" has "$W/vrcmd.log" "resolutionOverride 1728"
check "a bad rate falls back to 90 (env wins over the config)" has "$S/game.log" "HZ=90"
sed -i 's/^refresh_rate = .*/refresh_rate = 90/' "$KARTFRAME_HOME/UserData/Config.toml"
sleep 1
# kit check
mkdir -p "$W/app/kit"; echo '{"fingerprint":"kitA"}' > "$W/app/kit/kit.json"; echo kitB > "$W/home/bin/kartframe.fingerprint"
run
check "kit mismatch refuses (exit 78)" test "$(cat "$W/rc")" = 78
check "refusal logged" has "$KARTFRAME_HOME/sessions/latest/launch.log" "REFUSING TO START"
sleep 1
echo kitA > "$W/home/bin/kartframe.fingerprint"; run
check "matching kit starts" test "$(cat "$W/rc")" = 0
sleep 1
# not a library launch
run SteamAppId=; check "no SteamAppId: settings skipped, game still runs" has "$KARTFRAME_HOME/sessions/latest/launch.log" "no SteamAppId"
echo
[ $fails = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
