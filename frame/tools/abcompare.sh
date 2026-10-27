#!/bin/bash
# A/B check of two builds in the mock runtime (lane AD): same scripted race, same deterministic settings, then
# errors, sets/s, late %, GPU ms per set p50/p95 and the eye pictures at matching scene positions side by side.
#
#   frame/tools/abcompare.sh [options] A B
#
# A, B: a worktree (build-linux/kartframe), a build dir, or a kartframe binary (resources beside it). Both may be
# the same. Options (defaults):
#   --env-a K=V / --env-b K=V   environment for one side only (repeatable), e.g. --env-b KARTFRAME_RESIDENT_GEOMETRY=1
#   --env K=V                   environment for both (repeatable)
#   --seconds 100               run length; --windows 44,46,..,62 picture dump times (s); --frames 25 per swapchain
#   --step 6                    every n-th pixel in the dumps (288 px across at 1728); --scale 1.0 render_scale
#   --menu-until 43             last A press (s): later ones would drive the kart (race GO is at about 44 s or 52 s)
#   --rounds 1                  A B A B ... interleaved; timing pooled, pictures paired per round
#   --out DIR                   report.txt, worst.png, the runs (default ~/.cache/kf-abcompare/<time>); --keep the dumps
#   --tol 2 --max-diff-pct 0.5 --max-worst-pct 5 --max-errors 0 --pos-tol 0.05 --min-pairs 20
#   --gpu-delta-max MS --late-delta-max PCT     optional claims: B - A must not exceed this
# Exit 0 PASS, 1 FAIL, 2 setup problem.
#
# What is scripted (all existing mock/mockrun/hudrace switches): hudrace.sh's Grand Prix by A presses until --menu-until, the
# head still (MOCK_XR_SWAY=0), controllers fixed (MOCK_XR_HANDS=1), the kart left standing, stamped eye images
# (MOCK_XR_STAMP=1: each image names its scene state) dumped without stalling (MOCK_XR_DUMP_QUEUE). A and B images are
# paired by eye and scene position (game frames since "entered RaceScene"), not by wall time. Runs go through
# frame/perf/run.sh: the shared build lock, headset-in-use and running-game checks, warm scratch home in
# $KF_ABC_HOME (default ~/.cache/kf-abcompare/home). Details and limits: frame/DEV.md, "A/B tool (lane AD)".
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "abcompare: $*" >&2; exit 2; }
SECS=100 WINDOWS=44,46,48,50,52,54,56,58,60,62 FRAMES=25 MENU_UNTIL=43 STEP=6 SCALE=1.0 ROUNDS=1 KEEP=0 OUT=
ENV_A=() ENV_B=() ENV_C=() PYARGS=()
BIN=()
while [ $# -gt 0 ]; do
  case $1 in
    --env-a) ENV_A+=("$2"); shift ;;
    --env-b) ENV_B+=("$2"); shift ;;
    --env) ENV_C+=("$2"); shift ;;
    --seconds) SECS=$2; shift ;;
    --menu-until) MENU_UNTIL=$2; shift ;;
    --windows) WINDOWS=$2; shift ;;
    --frames) FRAMES=$2; shift ;;
    --step) STEP=$2; shift ;;
    --scale) SCALE=$2; shift ;;
    --rounds) ROUNDS=$2; shift ;;
    --out) OUT=$2; shift ;;
    --keep) KEEP=1 ;;
    --tol|--pos-tol|--max-pairs|--min-pairs|--max-diff-pct|--max-worst-pct|--max-errors|--gpu-delta-max|--late-delta-max|--names)
      PYARGS+=("$1" "$2"); shift ;;
    -h|--help) sed -n 2,28p "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) BIN+=("$1") ;;
  esac
  shift
done
[ ${#BIN[@]} = 2 ] || die "need two builds: abcompare.sh [options] A B"
exe_dir() {  # worktree, build dir or binary -> the directory holding kartframe
  local p=$1
  if [ -f "$p" ] && [ "$(basename "$p")" = kartframe ]; then dirname "$(readlink -f "$p")"
  elif [ -x "$p/build-linux/kartframe" ]; then readlink -f "$p/build-linux"
  elif [ -x "$p/kartframe" ]; then readlink -f "$p"
  else die "no kartframe at $p"; fi
}
DIR[0]=$(exe_dir "${BIN[0]}"); DIR[1]=$(exe_dir "${BIN[1]}")
ROOT=${KF_ABC_ROOT:-$HOME/.cache/kf-abcompare}
[ -n "$OUT" ] || OUT=$ROOT/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"; OUT=$(readlink -f "$OUT")
export KF_PERF_HOME=${KF_ABC_HOME:-$ROOT/home} KF_PERF_OUT=$OUT/runs
# The deterministic scenario. The race is seeded by the menu walk, so the start frame varies by a few frames
# between runs: the pictures are paired by scene position, not time (abcompare.py).
COMMON=(MOCK_XR_STAMP=1 "MOCK_XR_PRESS=/user/hand/right/input/a/click@12/1.5/0.15/$MENU_UNTIL" MOCK_XR_SWAY=0,1,0
  KF_MOCKRUN_RENDER_SCALE=$SCALE MOCK_XR_DUMP_AT=$WINDOWS MOCK_XR_DUMP_FRAMES=$FRAMES MOCK_XR_DUMP_STEP=$STEP
  MOCK_XR_DUMP_QUEUE=512 KARTFRAME_FPSLOG=1 "${ENV_C[@]}")
RUNS=()
for r in $(seq 1 "$ROUNDS"); do
  for s in 0 1; do
    N=$([ $s = 0 ] && echo A || echo B)$r
    D=$OUT/dump-$N; rm -rf "$D"; mkdir -p "$D"
    SIDE=(); if [ $s = 0 ]; then [ ${#ENV_A[@]} = 0 ] || SIDE=("${ENV_A[@]}"); else [ ${#ENV_B[@]} = 0 ] || SIDE=("${ENV_B[@]}"); fi
    echo "abcompare: $N: ${DIR[$s]}/kartframe ${SIDE[*]:-}"
    # hudrace.sh waits for the build lock, the headset and any running game (frame/perf/run.sh), then runs mockrun.sh
    KF_BUILD=${DIR[$s]} bash "$HERE/../perf/hudrace.sh" "$N" "$SECS" "${COMMON[@]}" MOCK_XR_DUMP="$D" "${SIDE[@]}" \
      > "$OUT/$N.out" 2>&1 || echo "abcompare: $N: run ended with status $? (see $OUT/$N.out)"
    [ -s "$OUT/runs/$N/game.log" ] || die "$N produced no log (see $OUT/$N.out)"
    rm -rf "$OUT/runs/$N/dump"; ln -s "$D" "$OUT/runs/$N/dump"  # the dump dir beside the log
    RUNS+=("$OUT/runs/$N")
  done
done
set +e
python3 -I "$HERE/abcompare.py" --out "$OUT" --windows "$WINDOWS" --step "$STEP" "${PYARGS[@]}" "${RUNS[@]}"
RC=$?
if [ "$KEEP" = 0 ]; then
  # the dumps are 100 MB or more a side; keep only the worst pair's PNG and the logs
  for D in "$OUT"/dump-*; do rm -rf "$D"; done
fi
echo "abcompare: report $OUT/report.txt"
exit $RC
