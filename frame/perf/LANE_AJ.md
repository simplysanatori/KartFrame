# Lane AJ: game-thread CPU in Grand Prix, and the GX thread

Problem (session 23): in Grand Prix the game thread used 13-14.5 ms of CPU per slow frame on the headset, so the
game ran at 43-51 fps ("70 % speed"), and 17-49 % of the sets lost their previous frame for interpolation.

## Headline: the GX thread (`KARTFRAME_GX_THREAD=1`)

`[video] gx_thread` (the Quest's default; off on Linux) moves the host side of GX to its own thread: FIFO parsing,
aurora's GX calls and draw recording. On this branch it first **corrupted the FIFO decode in every race and crashed
within 10-60 s** (5 of 5 runs, also with body IK off, the held item off, a drain at every present, and on lane AI's
binary). gdb found the cause: `aurora_set_stereo_hud_world_depth` / `_anchor` drain aurora's FIFO, and our nameplate
hook (`mkw_vr_instrumentation.cpp`, layout control draw) called them on the game thread. The game thread then decoded
aurora's FIFO at the same time as the GX thread. Fixed: both calls are posted through `GxThread::Post`, which runs
them inline when the thread is off, and unchanged values are not posted again.

Mock Grand Prix (hudrace.sh Luigi Circuit, first person, 1728, 120 Hz, packed, end lock after, `MOCK_XR_BUSY_MS=1`,
FPSLOG=1, CPULOG=1). The race part comes from the cpulog lines: 12 s after RaceScene to the end minus 3 s, 70 s.
Sets come from pacing-parts.py. The runs were interleaved A B A B A B, with no other build running (checked before
and after each run).

| run | GX thread | game fps (5 s min) | game CPU ms/frame mean / p50 | runq ms | off-CPU ms | GX thread ms/s | worker ms/s | game on X4 core | sets/s, late % | GPU p50/p99 | no previous frame | hottest |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ab-A1 | on | 60.0 (59.9) | 7.80 / 8.08 | 0.23 | 8.6 | 316 | 275 | 64 % | 120.5, 0.01 | 5.06 / 7.17 | 0.0 % | 88.0 C |
| ab-B1 | off | 59.0 (54.5) | 11.18 / 11.93 | 0.39 | 5.4 | - | 297 | 31 % | 120.5, 0.01 | 5.63 / 7.25 | 2.4 % | 90.0 C |
| ab-A2 | on | 60.0 (59.9) | 7.94 / 8.23 | 0.23 | 8.5 | 331 | 290 | 62 % | 120.5, 0.01 | 5.96 / 7.30 | 0.0 % | 90.7 C |
| ab-B2 | off | 59.8 (58.5) | 10.86 / 11.66 | 0.40 | 5.5 | - | 280 | 33 % | 120.5, 0.01 | 5.13 / 7.07 | 0.5 % | 88.0 C |
| ab-A3 | on | 60.0 (59.9) | 7.77 / 8.09 | 0.25 | 8.6 | 312 | 282 | 64 % | 120.5, 0.01 | 5.17 / 7.17 | 0.0 % | 95.0 C |
| ab-B3 | off | 59.1 (57.1) | 11.12 / 11.92 | 0.39 | 5.4 | - | 295 | 31 % | 120.4, 0.01 | 4.99 / 7.20 | 2.1 % | 90.7 C |

- The GX thread takes **3.2-3.4 ms per frame off the game thread** (11.1 -> 7.8, -30 %). It costs 5.2-5.5 ms per frame
  on its own thread (312-331 ms/s). The mock's GX-off Grand Prix is at 11 ms, just under the session-23 threshold
  (11 ms or less: 60 fps; 13 or more: 40-52). The headset ran 13.1-13.7 ms, so expect about 9.5-10 ms with the
  thread on: inside the 60 fps zone.
- With the thread on, the game thread stayed on the X4 core (cpu7, capacity 1024) for 62-70 % of its frames, against
  31-33 % with it off (uclamp min 1024 does not keep a 65 %-busy thread there). The CPU per frame drops partly because
  the thread now runs on the faster core more of the time.
- After the fix: 0 FIFO decode errors, 0 fatal lines, 0 GX-thread exceptions in 5 race runs. The eye images at 65 and
  75 s (fix-A1) match the GX-off run (track, karts, HUD, minimap, both eyes).
- Not covered by the mock: rumble/throw (input only), the TT ghost composite, long sessions. The hitch line's
  `game_cpu_ms` is measured where aurora's end_frame runs, so with the GX thread on it is the **GX thread's** CPU.
  Use the cpulog line for the game thread.

## Where the game thread's time goes (GX thread off)

`perf record` (user space, 30 s of race, run p1). An unrelated build started during that run, so read the shares, not
the absolute times. Game thread samples:

| part | share |
| --- | --- |
| translated game code (func_8..., HLE'd SDK functions) | 54 % |
| GX HLE + FIFO decode + aurora draw recording | 24 % |
| runtime dispatch (indirect calls, thunks) | 7 % |
| locks, clock reads | 7 % |
| memcpy | 3 % |
| our VR hooks (first person, IK, culling, held item, nameplates) | 0.1 % |

The cpulog timers agree: commit + update + nameplate hooks take 0.01-0.03 ms a frame. The overlay (ImGui end/begin at
the present, which takes the renderer lock) takes 0.8-1.2 ms a frame with the thread off and 0.2-0.8 ms with it on.
`pace` (PaceToRetraceBoundary) was 0 in every race: no frame finished before its retrace. The off-CPU rest is the
guest idle loop waiting for the next retrace. The run queue was 0.2-0.4 ms a frame, so placement is not the lever.
With the GX thread on, the game thread's remaining host-side work is mostly PostRecordRaw (the ring copy, about 6 %).

Compiler flags: the translated shards build with `-O2 -fno-fast-math -ffp-contract=off -fno-slp-vectorize
-mcpu=native` (the FP flags are needed for PowerPC semantics). They were not changed: a rebuild is heavy, and the GX
thread gives more.

Clocks and heat in the mock (cpulog, one sample a second): cpu7 (X4) was at its 3.05 GHz top in 26-30 of 60 race
seconds with the GX thread on and in 14-16 with it off. In the other seconds it read 1.8-2.7 GHz, because schedutil
follows the load and the game thread was elsewhere. No scaling_max_freq cap appeared, up to 95 C. No run dipped like A2 of the session-23 calibration. Heat as the
cause of the headset dips is still open; the wrapper now logs it.

## Switches (all default off)

- `KARTFRAME_GX_THREAD=0|1`: overrides `[video] gx_thread` for one launch (main.cpp). Log: `GX thread: requested
  ([video] gx_thread, KARTFRAME_GX_THREAD)`, `GX thread started (16 MiB command ring)`, and `thread priority: ...; GX
  nice -15`.
- `KARTFRAME_CPULOG=1`: one `cpulog:` line a second (runtime/src/cpu_monitor.cpp, a monitor thread). It holds: game
  ms/frame (wall, cpu, runq, pace, other off-CPU), the hook timers, the cores the game thread ran on, the busiest
  threads (run/wait ms per second @ last core; game, gx, worker labelled), each allowed core's clock (with any cap)
  and the hottest thermal zone with its name. Cost on the game thread: a few clock reads a frame.
- `KARTFRAME_INTERP_MAX_MS=<ms>`: keeps the interpolation pair across boundary gaps up to `<ms>`, rounded up to whole
  VI intervals (33 means one missed retrace). Display side only (aurora.cpp seal). Mock with the GX thread off
  (im-B1): "no previous frame" 0 % against 0.5-2.4 % in B1-B3. The game's own speed does not change. Log line once:
  `VR interpolation: previous frame kept across gaps up to 33.0 ms`.
- `KARTFRAME_GAME_CORE=<n>|auto[:shared]`: pins the game thread and keeps every other thread of the process off that
  core (re-applied every second). Built but **not measured**: the run queue is small, and the GX thread already moved
  the game thread to the X4 most of the time.
- `KARTFRAME_GX_SYNC=present` (test only): drains the GX thread at each present. It did not stop the corruption (the
  bug was the concurrent drain above) and cost 3.2 ms a frame. Leave it off.
- Wrapper: `thermal.tsv` in the session folder (time, hottest zone in C and its name, each CPU's clock in MHz, the GPU
  clock). Shell builtins only. `KARTFRAME_THERMLOG=0` turns it off.

## Next headset session

All launches: Grand Prix, 1728 per eye, `KARTFRAME_CPULOG=1 KARTFRAME_FPSLOG=1`, at least one full 3-lap race.

1. 1728 @ 90 Hz, `KARTFRAME_GX_THREAD=1`. Read the cpulog game cpu ms/frame (target under 11) and fps.
2. 1728 @ 90 Hz, GX thread off (the control, a repeat of h23 launch 03).
3. 1728 @ 120 Hz, `KARTFRAME_GX_THREAD=1` (the original target, if the game holds 60).
4. 1728 @ 90 Hz, `KARTFRAME_GX_THREAD=1 KARTFRAME_INTERP_MAX_MS=33` (if a launch still dips below 60: glide instead of
   step).

Look at: textures, models and HUD with the GX thread on (a hazard bug shows only there: wrong textures or matrices,
flicker); nameplates at the right depth; the held item; the TT ghost if there is time. `thermal.tsv` against the
cpulog fps over a long race settles the heat question.

## Per-set CPU (the second lever, ideas only)

Set encode was 2.0-2.3 ms p50 a set (mock) and 2.7-3.0 on the headset, 120 times a second: about a third of a core.
With the GX thread on, the game, GX and worker threads plus the pacing thread share 5 cores with SteamVR. Ideas:
- Re-encode only what changed between the two interpolated sets of a game frame. The draw list is the same and only
  the weight and the head pose differ, so a pre-recorded render bundle per eye pass, with the matrices in a uniform
  buffer, would turn the encode into a buffer write plus a bundle execute.
- 90 Hz cuts it by 25 % (session 23: 322 -> 267 ms/s).
