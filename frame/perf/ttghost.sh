#!/bin/bash
# A Time Trial against the staff ghost in the mock (lane AM), with eye images to look at.
#
#   frame/perf/ttghost.sh NAME [SECONDS] [VAR=value ...]
#
# Like hudrace.sh, but the scripted presses (TT_PRESS, one MOCK_XR_PRESS list: path@start/every/hold/until,
# up to 16) walk to Single Player, Time Trials, the default character, kart and course, Race Ghost and the
# staff ghost, and the kart then stands at the start line so the ghost drives away in front of the camera.
# The scratch home (KF_PERF_HOME) must already hold a license (copy a hudrace home). Eye images are written
# at TT_DUMP_AT (MOCK_XR_DUMP_AT list) into the run directory's dump/ as PNG (every MOCK_XR_DUMP_STEP-th pixel).
# TT_HOLD: press length (0.15 s); TT_HANDS: MOCK_XR_HANDS (unset: no pointer on the menu screen).
# NOT YET A WORKING ROUTE (lane AM, 3 tries): with MOCK_XR_HANDS=1 the Wii pointer sits on the Single Player menu's
# third row and A picks VS Race whatever the D-pad did; without hands that menu's cursor still starts on VS Race (the
# save remembers the last mode, so D-pad up once would be Time Trials), a lost license costs ~10 s of license
# creation, and A did not leave the drift-mode screen. Whether a staff ghost is offered on a fresh save is unknown.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?name}; SECS=${2:-90}; shift; [ $# -gt 0 ] && shift
OUT=${KF_PERF_OUT:-$HOME/.cache/kf-laneAM/runs}
DUMP=$OUT/$NAME/dump
export KF_PERF_OUT=$OUT KF_PERF_HOME=${KF_PERF_HOME:-$HOME/.cache/kf-laneAM/home}
A=/user/hand/right/input/a/click; D=/user/hand/left/input/dpad_down/click
once() { echo "$1@$2/1000/${TT_HOLD:-0.15}/$(echo "$2 + 1" | bc)"; }
DEFAULT_PRESS="$(once $A 12);$(once $A 15);$(once $A 18);$(once $D 21.5);$(once $A 23);$(once $A 26);$(once $A 29);$(once $A 32);$(once $A 35);$(once $A 38);$(once $A 41);$(once $A 44);$(once $A 47);$(once $A 50)"
PRESS=${TT_PRESS:-$DEFAULT_PRESS}
TMP=$OUT/.$NAME-dump; rm -rf "$TMP"; mkdir -p "$TMP"   # run.sh empties the run directory first
bash "$HERE/run.sh" "$NAME" "$SECS" MOCK_XR_SKIP=1 TU_DEBUG=sysmem "KF_MOCKRUN_VR=hud_virtual_screen = true
hand_hud = false" "MOCK_XR_PRESS=$PRESS"${TT_HANDS:+ MOCK_XR_HANDS=$TT_HANDS} \
  MOCK_XR_DUMP=$TMP "MOCK_XR_DUMP_AT=${TT_DUMP_AT:-60,70}" MOCK_XR_DUMP_FRAMES=${TT_DUMP_FRAMES:-1} "$@" || true
mkdir -p "$DUMP"
mv "$TMP"/* "$DUMP"/ 2>/dev/null || true; rm -rf "${TMP:?}"
python3 -I - "$DUMP"/*.ppm <<'PY'
import os, sys, zlib, struct
for src in sys.argv[1:]:
    if not os.path.exists(src): continue
    data = open(src, 'rb').read()
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    px = data[len(data) - w * h * 3:]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(src[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                                        + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    os.remove(src)
PY
grep -a 'entered RaceScene\|Time Trial ghost' "$OUT/$NAME/game.log" | head -6 || true
echo "ttghost: eye images in $DUMP"
