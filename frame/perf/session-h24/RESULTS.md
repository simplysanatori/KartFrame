# Headset session 24: results (2026-10-27, analysis lane, logs only)

Data: `$KARTFRAME_HOME/sessions/` `20261027-194539` (01), `-194827` (02), `-195054` (03), `-195321` (04). Tools:
`report.txt`, `frame/tools/phase.py`, and the lane's scratch scripts. They put every per-second `cpulog:` line, every
5 s `Game frame rate` (FPSLOG) and `GX thread:` line, the `vr pacing:` lines, the `XRS` set lines (display time, game
frame pair and weight of every set) and the vrframes F lines on one race clock. Race time 0 is the second
`presentation=immersive-race`. "Steady" means race time 10 s to the quit minus 3 s. `thermal.tsv` (epoch time) is put on
the same clock through the vrframes P line (CLOCK_MONOTONIC and CLOCK_BOOTTIME) and the boot-time-to-epoch offset.

Owner verdicts: 01 "good! not a full race; maybe slowdown at the beginning, not sure"; 02 "good but slowdown"; 03 "good,
but occasional slowdown AND speed-up, and image stuttering every ~5 seconds"; 04 "no visual bugs but slowdown as well".
No GX-thread visual bugs in 01, 03 or 04.

## Mapping (confirmed from the logs)

| launch | folder | wrapper `session.queue: using` | game.log | end |
| --- | --- | --- | --- | --- |
| 01 | 194539 | h24-01-90hz-gx.env | `GX thread started (16 MiB command ring)`, `display refresh rate now 90 Hz` | 143 at 19:48:04 |
| 02 | 194827 | h24-02-90hz-nogx.env | `GX thread: off ([video] gx_thread)`, 90 Hz | 143 at 19:50:20 |
| 03 | 195054 | h24-03-120hz-gx.env | GX thread started, `display refresh rate now 120 Hz` | 143 at 19:52:46 |
| 04 | 195321 | h24-04-90hz-gx-interp33.env | GX thread started, 90 Hz, `VR interpolation: previous frame kept across gaps up to 33.0 ms` | 143 at 19:55:11 |

All four: 1728 per eye, packed, end phase after (locked to the compositor grid; 99.9 % of the ends after the update
end, as before). The four launches ran back to back within 10 minutes. **No launch was a full race.** Race start to
quit took 76, 57, 66 and 62 s, and no scene change follows the race. Across sessions 23 and 24, none of the 8 launches
finished 3 laps. 0 GX-thread faults, 0 FIFO decode errors, 0 new pipelines waited for in any race.

## 1. Per launch

The fps is the cpulog's per-second game frame rate (all frames). "Draws" is the FPSLOG's draw calls per frame. "GX wall"
is the GX thread's time inside its records (`GX thread busy` % / fps). "GX CPU" is the GX thread's CPU per frame (the
cpulog `gx` run ms/s / fps; this includes its idle spin, so it is an upper bound). Smoothness comes from the XRS set
lines: a set is "off the even motion" when the game position it shows (frame pair + weight) moves more than 0.25 frame
away from the local mean step.

| | 01 | 02 (control) | 03 | 04 |
| --- | --- | --- | --- | --- |
| setting | 90 Hz, GX on | 90 Hz, GX off | 120 Hz, GX on | 90 Hz, GX on, interp 33 |
| race start to quit / full race | 76 s / no | 57 s / no | 66 s / no | 62 s / no |
| **game fps, steady mean** | **57.6** | **48.6** | **59.5** | **57.0** |
| min / max 5 s window | 51.9 / 60.1 | 43.7 / 52.5 | 57.9 / 60.1 | 48.7 / 60.1 |
| seconds below 58 / below 56 | 44 % / 27 % | 100 % / 100 % | 12 % / 0 % | 44 % / 30 % |
| race start (0-10 s) fps | 48.3-48.5 | 48.5-48.9 | 58.5-59.1 | 57.1-58.5 |
| draw calls a frame, mean (window range) | 473 (324-554) | 528 (482-579) | 476 (435-522) | 524 (464-575) |
| **game-thread CPU ms/frame**, mean (p10-p90) | **8.50** (7.42-9.63) | **13.20** (12.33-14.00) | **8.78** (8.37-9.32) | **8.76** (8.39-9.25) |
| same, race thirds | 9.22 / 7.91 / 8.37 | 12.64 / 13.53 / 13.40 | 8.76 / 8.63 / 8.94 | 8.76 / 8.68 / 8.84 |
| run-queue / off-CPU ms/frame | 0.30 / 8.61 | 0.48 / 7.03 | 0.29 / 7.74 | 0.26 / 8.63 |
| game waits for the GX thread (2 `GXDrawDone` drains), ms/frame | 3.42 | - | 4.89 | 4.51 |
| overlay hook (takes the renderer lock), ms/frame | 1.45 | 1.58 | 0.77 | 1.49 |
| game thread on the X4 core (cpu7) | 67 % | 53 % | 61 % | 69 % |
| **GX CPU / GX wall, ms/frame** | **6.1 / 10.3** | - | **6.2 / 11.6** | **6.2 / 11.7** |
| GX: FIFO decode / `GX__DrawDone_gx` / `CallDisplayList` wall, ms/frame | 5.9 / 2.5 / 1.0 | - | 6.8 / 2.6 / 0.7 | 5.9 / 3.4 / 1.4 |
| GX (aurora's producer) waits for SEALED, ms/frame | 0.99 | 0.01 | 1.29 | 1.25 |
| frame worker CPU ms/s | 314 | 324 | 406 | 342 |
| sets/s, late %, repeats % | 90.5, 0.16, 0.19 (1) | 90.6, 0.05, 0.03 | 120.7, 0.03, 0.02 | 90.4, 0.20, 0.14 |
| GPU ms/set p50 / p99 (median s); worst-second p99 | 9.21 / 10.23; 12.35 | 9.06 / 10.39; 11.10 | 6.63 / 7.74; 12.23 | 9.13 / 10.22; 11.29 |
| encode ms/set p50 (median s) | 3.07 | 3.05 | 2.92 | 3.06 |
| **sets without a previous frame** (10 s windows) | **4.9 %** (0.2-17) | **26.4 %** (21-31) | **1.2 %** (0-2.4) | **0.0 %** (0-0.1) |
| sets off the even motion by > 0.25 / > 0.5 frame | 8.9 % / 3.8 % | 42 % / 24 % | 2.3 % / 0.6 % | 6.5 % / 1.8 % |
| SteamVR app frames not presented / presented twice (race) | 16 / 16 (10-14 s) | 1 / 1 | **66 / 63** | 0 / 0 |
| hottest zone, race mean / max | 87.4 / 95.0 C | 85.0 / 91.9 C | 91.2 / 96.2 C | 86.9 / 93.4 C |
| CPU clock caps (cpulog) | none | none | none | none |
| big-core clock min, cpu2-4 / cpu7 (MHz) | 2899 / 2246 | 2841 / 1824 | 3014 / 2496 | 2899 / 2112 |
| GPU clock in the race | 903 MHz (500 only in the focus loss) | 903 | 903 | 903 |

(1) Without the 3.5 s focus loss at race time 17.8-21.5 s. In that window the OpenXR session went VISIBLE ->
SYNCHRONIZED -> VISIBLE -> FOCUSED (SteamVR took the app's visibility, for example through the dashboard or system
button). 0 sets were rendered and the last image was repeated, while the game kept running at 59.9 fps. With that window
included: 85.4 sets/s, 5.8 % repeats. It is the freeze-then-jump of 01 near the start, and it was not ours.

## 2. Did the GX thread deliver?

**Yes, and more than predicted.** All frames, steady race:

| | game CPU ms/frame | game fps |
| --- | --- | --- |
| session 23, h23-03 (1728, 90 Hz, GX off; slow frames only) | 13.1 (p90 14.4) | 50.3 |
| 02 (the same, all frames) | 13.20 | 48.6 |
| 01 / 03 / 04 (GX on) | 8.50 / 8.78 / 8.76 | 57.6 / 59.5 / 57.0 |
| mock (lane AJ), GX on / off | 7.8 / 11.1 | 60.0 / 59-59.8 |

The thread takes 4.4-4.7 ms a frame (34-36 %) off the game thread. The prediction was 9.5-10 ms; it came in at 8.5-8.8.
The control reproduces session 23 (48.6 vs 50.3 fps, 13.2 vs 13.1 ms). GX on gains 8.4-10.9 fps, and the sets without a
previous frame fall from 26 % to 0-5 %. The game thread's CPU is no longer the limit (section 3b).

But the game still does not hold 60 at 90 Hz: 44 % of the seconds are below 58 in 01 and 04. The GX thread's own CPU is
6.1-6.2 ms a frame (mock 5.2-5.5), well inside a frame. Its **wall time inside its records is 10.3-11.7 ms a frame**,
against 6.0-7.1 in the mock. So **4-5.5 ms of every frame it is blocked inside its records**; in the mock it was about
1 ms. The game thread waits for it at the game's two `GXDrawDone` calls a frame (a drain: on hardware DrawDone blocks
until the GP has consumed the FIFO). That wait is 3.4-4.9 ms a frame here and 1.3-1.9 ms in the mock. The mock's GX
thread waited 0.3-0.5 ms a frame for SEALED; here it waits 1.0-1.3 ms.

## 3. Why GX-on launches still slow down

**(a) Thermal: no.** No CPU clock cap appeared in any cpulog line. The GPU stayed at 903 MHz throughout all four races,
except 3 s at 500 MHz in 01, during the focus loss when it had nothing to render. The big cores stayed at 2.8-3.15 GHz
(schedutil follows the load). The hottest zone ran at 85-91 C on average, with a 92-96 C peak in every launch. The
**hottest launch (03, mean 91.2 C, max 96.2) was the fastest**. The last launch (04) was cooler than 03 (86.9 C). Per
second, fps against hot zone gives r = -0.28 to +0.22. Launch order shows no heat trend: 57.6, 48.6, 59.5, 57.0 fps. The
races lasted only 57-76 s, so heat over a full 3-lap race (2.5 min or more) remains untested, but up to 96 C nothing
throttled.

**(b) CPU: the game thread waits; it does not compute more.** Per 5 s window, fps follows the draw calls: GX-on windows
fit frame time = 13.1 ms + 0.87 ms per 100 draws a frame. Every 04 dip lies at 564-575 draws (23-28 s, 48 s; FIFO
56-59 KiB a frame against 49 elsewhere). In those dips the game CPU rose only 0.4-0.7 ms (8.5 -> 8.9-9.2). Off-CPU rose
1-2.7 ms: the drain wait grew to 5.1-5.6 ms (from 4.2-4.6) and SEALED to 1.4-1.8 (from 0.8-1.1). The same holds in 01
(start: 545-554 draws, 48 fps). 03 never went above 522 draws (mean 476). 04 averaged 524 draws, about as many as the
control 02. At equal draws (500-530), 01, 03 and 04 all ran at 56.5-59 fps. So **03 was faster mostly because its race
was lighter, not because of 120 Hz** (one launch each; the course is not logged, the pack position differs).

The chain at 60 fps (03, per frame): game CPU 8.8 + run queue 0.3 + waiting for the GX thread about 4.9 + the rest about
2.7 (SEALED handshake and overlay lock included) = 16.7 ms. Only 1.5-2 ms of slack is left. **The critical path is now
the GX thread, and the GX thread is mostly blocked, not busy.** What blocks it:
- Measured: the SEALED handshake with the frame worker, 1.0-1.3 ms a frame.
- The rest (3-4 ms a frame) is not timed. The only other blocking point in its records is the **renderer GPU mutex**:
  `aurora::gx::fifo::process` takes it for every FIFO chunk (900-1250 a frame), and `GXDrawDone` drains the FIFO under
  it. The frame worker holds the same mutex through the **whole encode of every interpolated set**
  (`run_retained_stereo_frame` in `aurora-main/lib/aurora.cpp`: `std::lock_guard gpuLock(g_rendererGpuMutex)` covers
  `render_stereo_eye` for both eyes, the composite, the sink and the submit). Each set's encode takes 2.9-3.1 ms, 90-120
  sets a second (280-360 ms/s), and the seal takes about 1 ms a frame on top.
- Evidence for the mutex: (1) the 01 focus loss is a natural experiment. With no sets encoded, the overlay hook (which
  also takes that mutex on the game thread) fell from 1.5-2.1 to 0.53-0.59 ms a frame, and the game ran at 59.9 fps
  with 8.2-8.5 ms of CPU. Before, at the same place, it ran 53-58 fps. (2) Per window, the GX thread's blocked time
  (wall minus CPU) follows the worker's estimated lock hold (sets/s x encode + fps x seal): r = 0.86 (01), 0.76 (03),
  0.02 (04). (3) The GX work matches the mock (same records a frame, about 7000; FIFO 45-53 KiB vs 42-47), but the
  headset's set encode is slower (2.9-3.1 vs 2.27 ms) and SteamVR's client shares the queue. Not proven: no timer covers
  that lock yet.

GX thread as the bottleneck: its CPU (6.1-6.2 ms a frame) is not. Its blocked wall time is.

**(c) 03's "speed-up": the game never ran faster than 60.** No 0.5 s window displayed more than 63 game frames a second.
The game produced at most 62 frames a second (rounding), and cpulog never went above 60.1. 03 had two short slow
stretches (race 19-30 s: 55.9-59 fps; 52-58 s: 56.8-59). Inside them, the 120 Hz sets showed a freeze followed by a
full-frame jump: 43 sets moved more than 1.6x the normal step, against 6-8 in 01 and 04. The return to 60 after a
stretch, and these jumps, are the likely "speed-up". It is not a catch-up of the game clock.

**(d) 03's image stutter: SteamVR misses our sets at 120 Hz (GPU), not a 5 s period.** In the race, SteamVR showed 63 of
our frames twice and never showed 66 (vrframes presents 2 then 0, the next frame's "frame ready" 1 ms later). There
were 26 bursts at 10-63 s, with gaps of 0.3-9.8 s (median 0.9 s). The bursts are densest at 23-33 s, where our GPU per
set rose to 7.0-7.3 ms p50 and 9-11 ms p99. The skipped frames had a SteamVR pre-submit GPU time of 7.3-8.4 ms (normal
p50 5.1, p90 5.8). Our own counters do not see it (late 0.03 %, repeats 0.02 %). The bursts are not tied to the 5 s log
lines (phase spread uniformly). 01, 02 and 04 at 90 Hz: 16 (only at 10-14 s), 1 and 0. The owner's "every ~5 s" reads
as the bigger bursts. This is session 22's lesson again: at 1728 and 120 Hz the GPU per set is at the 7.1 ms budget.

## 4. INTERP_MAX_MS=33 (04 vs 01)

It did what it should. Sets without a previous frame: **0.0 %** (01: 4.9 %, windows up to 17 %). Sets off the even
motion by more than 0.25 frame: 6.5 % vs 8.9 %. More than 0.5 frame: 1.8 % vs 3.8 %. It did this at a slightly lower
game rate (57.0 vs 57.6) and on a heavier race (524 vs 473 draws). It does not change the game's speed, and the owner
reported no visual difference or artefact. Freezes from late frames remain (5.2 % of the sets, against 5.8 %).

## 5. Recommendation

**(a) Default now: GX thread on, 90 Hz, `KARTFRAME_INTERP_MAX_MS=33`, 1728.**
- GX thread on: -4.5 ms of game CPU a frame and +8.4 to +10.9 fps against the control. No visual bugs in three
  launches, 0 faults, 0 decode errors.
- 90 Hz: GPU per set 9.1 ms p50 against an 11.1 ms refresh, and 0-1 SteamVR skips in the race (04, 02). At 1728, 120 Hz
  skips 1.2 frames a second in bursts (the owner's image stutter), and its better fps came from a lighter race.
- Interpolation window 33 ms: measurably smoother at the same fps, with no cost seen.
- 120 Hz stays the goal once the per-set GPU time drops about 1 ms (p99 under about 7 ms).

**(b) Residual slowdown, ranked causes and the next fix for each:**
1. **The GX thread is blocked on the frame worker** (wall 10.3-11.7 ms a frame against 6.1-6.2 ms CPU), and the game
   waits 3.4-4.9 ms a frame for it at `GXDrawDone`. The steps:
   - Confirm (cheap, logs only): time the acquisition of `renderer_gpu_mutex()` in `fifo::process` and
     `GXDrawDone` (GX thread) and in `aurora_imgui_host_frame_begin` (game thread). Time the worker's hold per set, and
     put them in the `GX thread:` line.
   - Fix: hold `g_rendererGpuMutex` in `run_retained_stereo_frame` only for the producer-shared part
     (`request_stereo_frame`, `prepare_late_stereo_replay`, the interpolation bookkeeping). Encode both eyes, the
     composite and the sink outside it, once an audit shows that this encode reads only the sealed frame and
     worker-owned objects (check the pipeline and bind-group caches, the staging buffers and the ImGui draw data).
   - Alternatively, or as well: cut the per-set encode (2.9-3.1 ms, 90-120 times a second) with pre-recorded render
     bundles per eye pass and the matrices in a uniform buffer (lane AJ's idea). That shortens the hold and the
     worker's CPU.
   - Expected gain: up to 3-4 ms a frame off the critical path, which is about the margin the 560-575-draw sections need.
2. **Heavier sections (560-575 draws a frame)** push the GX work (FIFO decode 5.9-6.8 ms wall a frame,
   `CallDisplayList` 0.7-1.4) and the waits past the retrace. After fix 1, the next step is the GX thread's own CPU per
   draw.
3. **120 Hz GPU per set** (image stutter, not speed): GPU per set must drop about 1 ms. The lever is the same encode
   and draw cost; render scale is not an option. Keep 90 Hz until then.
4. **Game thread's own CPU**: 8.5-8.8 ms, no longer the limit. `KARTFRAME_GAME_CORE` (pinning) and `-O3`/LTO for the
   translated code: not now.
5. **Thermal**: not a cause up to 96 C in 60-76 s races. Re-check on a full race with `thermal.tsv`.
6. The SteamVR focus loss in 01 (3.5 s freeze at race start): SteamVR's doing, no action. Ask the owner not to open the
   dashboard during a measured race.

**(c) Next session (at most 4 launches, Grand Prix, 1728, same course, one FULL 3-lap race each, FPSLOG + CPULOG +
thermal):**
- With fix 1 behind a switch (mock A/B first: GX wall vs CPU, drain wait, lock-wait timers):
  1. 90 Hz, GX on, interp 33 (the new default; baseline).
  2. 90 Hz, GX on, interp 33, narrowed lock.
  3. 120 Hz, GX on, interp 33, narrowed lock: SteamVR skips and GPU per set.
  4. A repeat of 2 (variance, and heat over a full race).
- Without fix 1:
  1. The default.
  2. 120 Hz with the default.
  3. The default with `KARTFRAME_GAME_CORE=auto`.
  4. The default again.
  All with the lock-wait timers if they can be built in time.
