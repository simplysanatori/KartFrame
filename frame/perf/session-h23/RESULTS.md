# Headset session 23: results (2026-10-27, analysis lane, logs only)

Data: `$KARTFRAME_HOME/sessions/` `20261027-181407` (01), `-181623` (02), `-181852` (03), `-182059` (04), and for
comparison `20261027-121256` (the earlier Grand Prix, 1728 at 120 Hz, before the held item / throw / rumble merge, no
FPSLOG). Tools: `frame/perf/pacing-parts.py` (on copies of game.log), `report.txt`, and per-window scripts in the lane's
scratch dir that line the `Game frame rate` (FPSLOG), `hitch:`, `vr pacing:`, `VR interpolation weights` and vrframes F
lines up on the race clock (race time 0 = the second `presentation=immersive-race`, the race after the intro).

Owner verdicts: 01 "great, maybe slightly slower game speed"; 02 "okay, but a lot of slowdown, like 70 % speed";
03 "good but again felt like running at a slower speed"; 04 "good but major slowdown, like 50 %". No doubling in any
launch. The owner prefers the higher resolutions by far.

## Mapping (confirmed from the logs)

| launch | folder | wrapper `session.queue: using` | game.log | end |
| --- | --- | --- | --- | --- |
| 01 | 181407 | h23-01-gp-1440.env | `view size: recommended 1440x1440 ... -> eyes 1440x1440`, packed 2880x1440, `display refresh rate now 120 Hz` | status 143 at 18:16:16 |
| 02 | 181623 | h23-02-gp-1536.env | eyes 1536x1536, packed 3072x1536, 120 Hz | 143 |
| 03 | 181852 | h23-03-gp-1728-90hz.env | eyes 1728x1728, packed 3456x1728, `display refresh rates: 90; requested 90 Hz`, now 90 Hz | 143 |
| 04 | 182059 | h23-04-gp-1440-split3.env | eyes 1440x1440, 120 Hz, `set submit split: 3 queue submissions a set (eye 0 \| eye 1 \| sink)` | 143 |

All four: packed, end phase after, menu controllers eyes, `item throw: on, right hand`, `cockpit item: hand right,
size 0.2 m`, 41 rumble lines each. **Each launch was one partial race**: 1.9-2.3 min per launch, race start to quit
67-83 s (no launch finished the 3 laps). The four launches ran back to back within 9 minutes. The draw-call profile per
5 s window has the same shape in all four (peaks at race 0-10, 30-35 and 55-65 s), so it was most likely the same course
each time (the logs do not name it).

## 1. Per launch

Race = race start to the quit (includes the last seconds' dashboard). Game fps from the FPSLOG frame counts (race
windows from +2 s). Hitch lines (game frame over 20 ms) are capped at 30 a second, so their numbers describe the slow
frames only.

| | 121256 (ref) | 01 | 02 | 03 | 04 |
| --- | --- | --- | --- | --- | --- |
| eye, rate, submit | 1728, 120 | 1440, 120 | 1536, 120 | 1728, 90 | 1440, 120, split 3 |
| race to quit | 201 s | 83 s | 71 s | 78 s | 67 s |
| **game fps, race mean** (5 s windows min-max) | ~43 (anchors) | **51.3** (45.5-56.6) | **47.5** (43.6-50.6) | **50.3** (47.0-57.8) | **47.4** (42.9-51.6) |
| game speed = fps / 60 | ~72 % | 86 % | 79 % | 84 % | 79 % |
| game fps first 30 s / later | - | 51.9 / 51.0 | 48.7 / 46.9 | 49.5 / 50.9 | 47.5 / 47.4 |
| sets with **no previous frame** (interpolation off) | 49 % (21-64) | **17 %** (8-27) | **30 %** (22-37) | **22 %** (10-29) | **30 %** (21-33) |
| slow-frame lines a second (cap 30) | 26.3 | 18.6 | 24.0 | 24.6 | 25.1 |
| slow frame: frame_ms p50 / p90 | 24.8 / 30.0 | 22.2 / 24.5 | 23.5 / 26.0 | 22.0 / 24.9 | 23.7 / 26.2 |
| slow frame: **game-thread CPU ms p50 / p90** | 14.5 / 16.0 | 13.3 / 14.6 | 13.7 / 14.9 | 13.1 / 14.4 | 13.3 / 14.6 |
| slow frame: run-queue ms p50 / p90 | 0.6 / 1.3 | 0.6 / 1.3 | 0.5 / 1.2 | 0.4 / 1.1 | 0.6 / 1.2 |
| slow frame: worker + lock wait p50 / p90 | 0 / 2.3 | 0 / 2.2 | 0 / 2.0 | 0 / 0 | 0 / 1.7 |
| slow frame: off-CPU rest p50 / p90 | 9.6 / 12.6 | 8.1 / 10.1 | 9.0 / 11.9 | 8.6 / 10.3 | 9.4 / 12.3 |
| sets/s, late %, repeats % (race) | 119.8, 0.46, 0.55 | 120.3, 0.16, 0.24 | 120.3, 0.19, 0.23 | 90.4, 0.09, 0.16 | 119.9, 0.53, 0.67 |
| GPU ms/set p50 / p99 (median second) | 6.71 / 8.54 | 5.78 / 7.09 | 6.09 / 7.20 | 9.13 / 10.29 | 5.79 / 7.14 |
| GPU ms/set p99, worst second | 17.5 | 14.3 (quit) | 13.3 (quit) | 11.5 | 14.8 (quit) |
| encode ms/set p50 | 3.06 | 2.68 | 2.73 | 2.95 | 2.89 |
| set encode CPU a second (sets x encode p50) | 367 ms | 322 ms | 328 ms | 267 ms | 347 ms |
| worker per game frame: seal / prepare / encode ms | - | 0.97 / 0.17 / 0.52 | 1.03 / 0.21 / 0.50 | 0.96 / 0.16 / 0.61 | 0.97 / 0.16 / 0.53 |
| producer wait for DONE / SEALED ms per frame | - | 0.06-0.24 / 0 | 0.10-0.39 / 0-0.18 | 0.08-0.31 / 0 | 0.08-0.24 / 0 |
| draw calls per game frame (mean) | - | 497 | 498 | 496 | 509 |
| SteamVR frames presented 0x, race | - | 55 (49 at the quit) | 40 (27 at 60-70 s) | **0** | 23 (all at the quit) |
| SteamVR pre-submit GPU p50 / p95 | - | - | 4.25 / 5.27 | 4.87 / 6.34 | 4.09 / 4.88 |

pairwait 0 everywhere. The late sets and repeats sit at race start and at the quit; 04 also had 1-6 per 10 s in the
middle of the race (01: 0).

**Does the game frame rate explain the verdicts?** It explains the ranking, not the size. By fps: 01 (51.3) > 03
(50.3) > 02 (47.5) = 04 (47.4), which is the owner's order (01 great, 03 good, 02 70 %, 04 50 %). But 70 % speed would
be 42 fps and 50 % would be 30 fps; the worst 5 s windows were 43.6 (02) and 42.9 (04), and nothing near 30. What
separates the "slow" launches more than their fps is the interpolation: 30 % of 02's and 04's sets were shown without
a previous frame, against 17 % in 01 and 22 % in 03. A game frame later than 25 ms (1.5 game intervals) breaks the
pair, and the sets after it step instead of gliding. The share grows much faster than the fps falls (51 -> 47 fps
nearly doubles it), and an uneven, stepping scene reads as slower than it is. 121256 (about 43 fps, 49 % of the sets
without interpolation) was the owner's "terrible".

## 2. Why 04 (1440, the same eye size as 01) was slower than 01

On the race clock (same course, windows paired within 3 s), **04 was slower than 01 in all 13 windows**, by 1.2-8.4
fps (mean 4.2). 02 was slower in 12 of 14 (mean 4.2), 03 in 10 of 15 (mean 1.2). The candidates:

- **Order / heat accumulation: not supported.** The order was 01, 02, 03, 04, but the fps went 51.3, 47.5, 50.3, 47.4,
  up again at the third launch. Game CPU per slow frame went 13.3, 13.7, 13.1, 13.3, with no trend. GPU per set at
  1440 was 5.78 in 01 and 5.79 in 04. A thermally throttled CPU would show as more CPU time for the same work, and 04
  had none. Within each launch the CPU per slow frame is flat per 10 s window (01 12.8-14.0, 04 12.8-13.9) and so is
  the fps (first 30 s vs later in the table). Encode per set rose 2.68 -> 2.89 (+8 %), the only proxy that moved
  with the order. Caveat: each race lasted 67-83 s and the wrapper logs no temperatures. In the 200 s race of 121256,
  the last 20 s had 15.5-16.2 ms per slow frame against 13.8-14.9 before, a hint (no more) that heat shows only after
  minutes.
- **Course: the same course.** Draw calls per frame differ by 1-2 % (04: +12 a frame). The fps follows the draw calls
  closely in every launch (r = -0.74 to -0.89 per 5 s window). Frame time grows by 2.5-2.7 ms per 100 draw calls a
  frame in all four launches. At equal draw calls, 02 and 04 sit 1-1.5 ms a frame above 01 and 03 (fitted at 450
  draws: 18.4, 19.9, 18.8, 19.6 ms).
- **Our hooks (held item, throw, rumble, FPSLOG): no visible cost.** 121256 had none of them and took 14.5 ms of CPU
  per slow frame. Today's launches with all of them took 13.1-13.7, and 03 at the same 1728 took 13.1. The cockpit
  item costs a one-off 6.6 ms parse at race load.
- **Split 3: the remaining difference, but not through the game thread's CPU.** 04's slow frames took the same CPU
  as 01's (13.3 / 14.6) and the same run-queue (0.6). Their extra length is all in the off-CPU rest (9.4 vs 8.1 ms
  p50), and there were more of them (25.1 vs 18.6 lines/s). The split's extra submissions run on the frame worker,
  whose per-frame stretches did not change (seal 0.97, encode 0.53). 04 had the most late sets and repeats of today
  (0.53 / 0.67 %, also mid-race), and its encode per set was 0.2 ms higher. One launch per setting cannot separate the
  split from run-to-run variance. But 02 shows the same -4 fps without a split, so nothing here argues for keeping the
  split.

## 3. Where the game thread's time goes

What the logs can split (slow frames, race, all launches):
- **Game-thread CPU 13.1-13.7 ms p50.** That is all guest code: the guest's threads are libco fibers on the one
  host thread, so the sound and DVD threads are in it too. It also holds the GX HLE, the FIFO decode and Aurora's draw
  recording. `[video] gx_thread` is **off** on Linux (`runtime_config.h`: on by default only on Android), so none of
  that GX work leaves the game thread on the Frame.
- Run-queue 0.4-0.6 ms (3 %). The game thread is not starved for a core (it already has nice -15 and uclamp min 1024).
- Waiting for the frame worker or the renderer lock: 0 at p50, about 2 ms at p90. FPSLOG agrees: the producer waited
  0.1-0.3 ms a frame for DONE and 0 for SEALED. Begin, pipeline waits, new pipelines and texture conversion: about 0
  in the race.
- **An off-CPU rest of 8-9.5 ms per slow frame that no counter names.** By elimination it is the guest scheduler's
  idle loop: `VI_HLE_WaitForNextRetracePoll`, the alarm and sleep timers, and `PaceToRetraceBoundary`. In other words
  the game waits for the next VI retrace, or for an alarm. Time Trial slow frames (session 22) look the same: about 9 ms
  of CPU and 10-11 ms off-CPU, at 60 fps.

So in Grand Prix the game thread is busy about 65 % of the time. It loses fps because each frame's CPU work no longer
fits the window before the next retrace, so the frame waits for a later one. It is not fully saturated. The CPU per
frame decides it, with a sharp threshold. At 11 ms or less it runs 60 fps: Time Trial took 9-10 ms, and mock run A1
took 11.1 ms at 59.8 fps. At 13 ms or more it runs 43-52 fps: today 13.1-13.7, 121256 14.5, and mock A2 13.8 ms at
39-48 fps. **About 2.5-3 ms a frame off the game thread should give 60 back.**

What would split the rest (cheap, logs only):
1. In the hitch line: the time in the scheduler's idle loop and in `PaceToRetraceBoundary`; the retraces elapsed
   during the frame; and the frame start's phase against the VI grid.
2. In the FPSLOG line: game-thread CPU ms per frame over **all** frames, not only the slow ones (the hitch lines are a
   biased sample capped at 30/s), plus frame-time p50/p90.
3. CPU per guest fiber (main vs sound thread), plus the GX HLE + FIFO + Aurora-recording share of the game thread.
   Either use counters around the GX entry points, or run one `perf record -t <game tid>` for 10 s mid-race (the
   Quest profile split it into 62 % game code, 9 % GX HLE, 6 % FIFO decode, 4 % copies and 3.5 % dispatch).
4. In the wrapper: SoC thermal zones, the CPU frequency of the game thread's core, and the GPU clock, once a second
   (still missing; needed to settle heat).

## 4. Does 90 Hz (03) lower the CPU load?

On the set side, yes: 90 sets a second instead of 120 cut the set encode CPU from 322 (01) to 267 ms a second
(-17 %). For the game thread, hardly: 13.1 vs 13.3 ms per slow frame (within the noise), 50.3 vs 51.3 fps,
22 % vs 17 % of the sets without interpolation. 90 Hz did not make the game faster than 1440 at 120. What it did
was let **full-size 1728 run as fast as 1440 at 120 Hz**. Against 121256 (1728 at 120 Hz: 14.5 ms, about 43 fps,
49 % without interpolation; another session, so a weaker comparison), 1728 at 90 took about 1.4 ms less CPU per slow
frame and gained about 7 fps. 03 was also the only launch with 0 SteamVR skips in the race, and it had the lowest
late and repeat rates. Its GPU margin is thin: 9.13 ms per set p50, p99 10.3, against 11.1 ms a refresh minus the
compositor's 0.8 ms.

## 5. Recommendation

**(a) Best fixed configuration today: 1728 per eye at 90 Hz** (launch 03), packed, end phase after, no split.
- It is the sharpest. Its game rate is within 1 fps of the best launch (50.3 vs 51.3). It had 0 compositor skips, the
  lowest late and repeat rates, and the owner called it "good".
- The slowdown it shares with every launch is a game-thread limit that no eye size or rate removes: 1440 at 120 is
  only 1 fps faster.
- 1536 at 120 is worse than both (slower than 01 and softer than 03).
- Split submission: drop it (no gain anywhere, slower in all 13 paired windows).

**(b) CPU-side fixes for 60 fps in Grand Prix, ranked:**
1. **Turn on the GX thread on the Frame** (`[video] gx_thread = true`, an existing switch, no new code). On the Frame
   the game thread carries the guest, the GX HLE, the FIFO decode and the draw recording, and the frame time grows
   2.5 ms per 100 draw calls. On the Quest the GX thread took about 20 % of the game thread (13.3 -> about 10.6 ms here,
   inside the 60 fps zone). There it turned the same crowded Grand Prix start from 52-56 into 60 fps, with the GX
   thread 25-40 % busy. Risks: it needs a free core (5 usable; the game, worker, pacing thread and SteamVR already
   compete), and it has never run on the Frame (hazard-rule bugs would show as wrong textures or matrices). Try a mock
   A/B first (abgp.sh, the Luigi Circuit Grand Prix, Config.toml switch, interleaved). There is no environment
   override yet: a session file cannot switch it, so the wrapper or the session needs a `KARTFRAME_GX_THREAD` key
   (small change in `main.cpp` / `runtime_config.h` reading; not made here, analysis lane).
2. **Less per-set CPU on the worker and pacing side.** The game thread's CPU per frame went with the set load: 13.1 at
   90 Hz (267 ms/s of set encode), 13.3-13.7 at 1440-1536 at 120 (322-347), and 14.5 at 1728 at 120 (367, other
   session). The run-queue stayed at 0.4-0.6 ms, so the coupling is through shared memory, cache and clocks, not
   scheduling. The levers: re-record less per interpolated set (2.7-3.0 ms encode per set, 120 times a second), and
   90 Hz (already in (a)).
3. **Interpolation tolerance (display side, not CPU).** At 47-51 fps, 17-30 % of the sets lose their previous frame,
   and that share, more than the fps, follows the owner's sense of slowdown. Keeping the pair for frames up to 2 game
   intervals (33 ms) instead of 1.5 (25 ms) would make a 50 fps game glide instead of step. It would not make the game
   faster. Mock-testable.
4. **Thermal management: measure first.** Today's data show no heat effect: CPU per slow frame and GPU per set were
   flat within and across the four launches. But the mock slowed down at 93-97 C, and 121256 rose by 1.3 ms a frame
   after 3 minutes. Add the temperature and clock log to the wrapper before any change.
5. **Thread placement and priority: not the lever.** The run-queue is 3 % of a slow frame. The game thread already has
   nice -15 and uclamp 1024, and the producer barely waits on the worker. Pinning the game thread (and later the GX
   thread) to their own big cores is worth trying only after 1.
6. **Our per-frame hooks: no action.** No measurable cost (13.1-13.7 with them vs 14.5 without).

**(c) Next headset session (4 launches, same course, at least one full 3-lap race each, FPSLOG on, temperatures
logged if the wrapper has it):**
1. 1728 at 90 Hz, GX thread on.
2. 1728 at 90 Hz, GX thread off (the control: repeats 03).
3. 1728 at 120 Hz, GX thread on (the original target, if the game reaches 60. The GPU is tight there: 6.7 ms per set
   p50 in 121256, and session 22 saw compositor skips wherever SteamVR's pre-submit GPU passed 7 ms.)
4. 1728 at 90 Hz, GX thread on (repeat of 1, for order and heat).

Prerequisites: the mock A/B of the GX thread (correct images, game fps, its busy share), the environment key to
switch it per launch, and ideally items 1-2 of the instrumentation in 3. If the mock shows no fps gain from the GX
thread, replace launches 1 and 4 with the interpolation-tolerance change at 1728 at 90 Hz.
