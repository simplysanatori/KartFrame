# Headset session 26: results (2026-10-28, lane AO, logs only)

Data: `$KARTFRAME_HOME/sessions/20261028-021654` (one launch, 02:16:54 to 02:19:15). Time Trial against a ghost at
the defaults (GX thread on, narrow worker lock, 90 Hz, 1728 per eye, packed, interpolation 33 ms) plus
`KARTFRAME_PASS_DUMP=2 KARTFRAME_FRAME_LOG=1`. No `KARTFRAME_CPULOG`/`FPSLOG` in this launch, so the evidence is the
per-second `vr pacing:` lines, the `hitch:` lines (each game frame of 20 ms or more), the pass dump, `thermal.tsv`
and the system journal. Clock: pacing t = seconds since `vr pacing clock: session start` (t 1 = 02:17:14); the race
starts at t 40 (`presentation=immersive-race`), the quit is at t 120. Compared with session 25 launches 01
(`20261027-220915`) and 03 (`20261027-233740`): same defaults, a Grand Prix, both "good". Scratch scripts:
`~/.cache/kf-laneAO` (sec.py, corr.py), not committed.

Owner: the ghost looked right (one kart at the right depth); the race stuttered every 2-5 s and felt slowed; the
2D menus before the race were slow too.

## Verdict: (c), other work on the game's CPUs. Not the ghost, not the logging

Two podman container builds of another project (DXVK d3d9 and a mingw proxy, about 600 s each, started before the
launch) compiled through the whole launch, a third started at 02:17:42 (t 29) and ran past the quit (journal: compile
lines in every minute 02:07-02:19; the last DXVK link 02:19:37, a mingw link failure 02:19:25). User processes here
share CPUs 3-7 (affinity mask f8: 5 CPUs), so the game's threads queued behind compilers:

| | H26 menus (t 7-38) | H26 race (t 44-119) | H25-03 menus | H25-03 race | H25-01 menus | H25-01 race (25-60 s) |
| --- | --- | --- | --- | --- | --- | --- |
| sets/s | 83.7 | 86.0 | 78.1 | **90.7** | 88.5 | 90.5 |
| late / repeats (% of ticks) | 5.6 / 7.3 | **5.4 / 4.2** | 10.3 / 13.8 | **0 / 0** | 1.6 / 2.2 | 0.1 / 0.3 |
| hitch frames (>= 20 ms) per s | 19.2 | 5.1 | 18.5 | 0.5 | 21.9 | 0.9 |
| **run-queue wait per hitch frame, p50 / p90 ms** | **10.3 / 16.1** | **12.0 / 15.7** | 0.2 / 0.6 | 0.7 / 1.5 | 0.2 / 0.7 | 0.4 / 1.2 |
| run-queue wait, ms per second | **221** | **61** | 6 | 0 | 7 | 1 |
| GPU ms/set p50 / p99 (median s) | 5.50 / 8.96 | 8.55 / 11.39 | 6.11 / 9.98 | 8.66 / 9.94 | 6.66 / 8.04 | 9.11 / 10.11 |
| encode ms p50 / p99 (median s) | 3.93 / 10.95 | 2.69 / 9.63 | 5.37 / 12.02 | 3.10 / 9.16 | 5.24 / 8.48 | 3.08 / 3.63 |
| corr. run-queue/s vs late+repeats/s | 0.66 | 0.69 | -0.33 | - | 0.49 | 0.17 |

- **97 % of the hitch frames (1074 of 1109) waited 5 ms or more for a CPU** (`runq_ms`); session 25: 0.6 % (4 of 695)
  and 0.2 % (1 of 424). A slow frame here is a thread that was runnable and not running, not more work: the GX
  thread's CPU on those frames is 3-5 ms (`game_cpu_ms`), like session 25's.
- **The menus show it without any ghost** (t 7-38, no depth-copy composite yet): 221 ms of run-queue wait a second
  against 6-7 in session 25's menus. So the ghost path cannot be the cause.
- **Heat and clocks**: the machine was already at 95.0 C at the launch (session 25: 68-78 C at the launch) and stayed
  at 95.2 C mean (max 97.3; session 25: 82-84 mean); the big cores ran at 2502 MHz mean against 2946-2951 (-15 %).
  The builds heated it; the clocks then made every thread slower on top of the queueing. GPU clock 903 MHz as before.
- The per-second stutter rhythm follows the builds' load (late+repeats track run-queue wait, r = 0.66-0.69), not
  anything periodic in the game: no pipeline creation after t 3 except 9 at the race start (`pipelines_new`); texture
  conversion 0.2 ms in the menu frames that have it (session 25's menus the same) and in 15 race frames; 1 begin join. The calm stretches (t 43-55, 97-98, 107-112: 0 late, 1-2 hitches a
  second) still have 12 ms of run-queue wait in the hitches they have.
- GPU p99 rises in the bad seconds (12-16 ms) because a set's GPU span includes the gaps while its submitting thread
  waits for a CPU (r = 0.41-0.73 with the run-queue wait); the medians (8.55 race) are session 25's (8.66-9.11), with a
  Time Trial's lighter scene and the ghost.

## (a) The per-eye ghost: cheap by construction, nothing in the data

- Log: frame 2034 `Time Trial ghost: composite of a depth copy left flat (no perspective draw in the copied picture)`
  (the first race frame: pass 2 had 0 draws, the ghost not drawn yet); frame 2035 `pass 2 (21 perspective draws,
  1 other; copies in passes 2..2) is drawn by each eye at the composite in pass 4 (alpha 0.50); 296 eye pipeline
  configs known`. Both lines are logged once a session, so they do not say when the ghost was visible; the owner saw
  it right.
- Code (`aurora-main/lib/gfx/common.cpp`, `replay_composite_source`): the ghost's constant-blend pipelines are resolved
  once per frame when the composite is linked (`compositeSourcePipelines`); per set and eye the worker re-issues the
  ghost's ~21 recorded draws with that eye's existing uniform ranges, and skips the same 21 draws where they were
  recorded and the full-eye composite quad. No pipeline, bind group or buffer is made per set. Net draws per eye: the
  same as the flat path minus one quad.
- Data: race encode p50 2.1-2.7 ms in calm seconds (session 25 Grand Prix 2.85-3.3), GPU p50 8.5-9.0. Nothing to fix.

## (b) The logging switches: not the cause

`KARTFRAME_FRAME_LOG=1` was on in all three session-25 launches too, which were smooth: 240 log lines a second here
(29380 lines, 5.4 MB in 123 s) against 265-270 a second there (13.4 MB in 4.5 min). `KARTFRAME_PASS_DUMP=2` wrote 12
lines in two frames. Off for normal play anyway (neither is a default).

## Change (lane AO)

The wrapper now says when this happens (frame/kartframe.sh only, see `frame/perf/LANE_AO.md`): a warning in
launch.log when compilers or build tools are running at the launch, four more `thermal.tsv` columns (busy CPUs of the
game's CPUs, the game's share, other processes' share, runnable threads), and a `machine` summary line (plus a
warning when other processes used a CPU or more on average) at the end of `report.txt`.

## For the owner

Play with nothing else building: the container builds that ran here belong to another project and did not wait for
`~/.cache/headset-in-use` (KartFrame's own `kfbuild.sh` does). One Time Trial against a ghost with no extra switches,
with the machine idle and cool: smooth (no stutter every few seconds), the ghost still one kart at the right depth;
`report.txt`'s new `machine` line should read about 0 other CPUs.
