# Headset session 21: results of launches 1-3 (2026-10-27, analysis lane, logs only)

Launches 4-6 were not run (their files are still in `session.queue/`). Data: `$KARTFRAME_HOME/sessions/`
`20261027-103514` (01), `20261027-103705` (02), `20261027-103821` (03). Tools: `frame/tools/phase.py` (views A-D),
`frame/perf/pacing-parts.py` (on a copy of game.log), plus per-frame scripts in the lane's scratch dir (XRF / XRS /
vrframes F lines lined up on the common clock as phase.py does).

Tool note: phase.py stops with a ValueError on 01 and 02. In each log, two XRF lines have an `[aurora]` line glued
on after `t=`, so the `t=(\S+)` group fails to parse as a float. The numbers below come from a copy that uses numeric
groups (`e=(-?[\d.]+) ... m=([\d.]+) r=(-?[\d.]+) t=([\d.]+)`). The repo's phase.py needs the same one-line fix.

## Mapping (confirmed from the logs)

| launch | folder | wrapper `session.queue: using` | game.log | eye pairs |
| --- | --- | --- | --- | --- |
| 01 control | 103514 | h21-01-control.env, `KARTFRAME_END_PHASE=after` | `end phase: ... after`, locked; no packed line | 4 |
| 02 packed | 103705 | h21-02-packed.env, `END_PHASE=after` | `stereo packed: on (3456x1728 ...)` + `end phase: ... after`, locked | 4 |
| 03 packed-nolock | 103821 | h21-03-packed-nolock.env, `END_PHASE=off: frames handed over at the wake-up` | `stereo packed: on`, no `end phase:` lines | 3 |

## 1. Per launch

| | 01 control | 02 packed | 03 packed, no lock |
| --- | --- | --- | --- |
| log span / race | 82.5 s / one race of 33.5 s (from 49 s, quit in the race) | 51.5 s / one race of 31.8 s (from 20 s) | 108.2 s / two races: 35.4 s (from 36 s) + 26.5 s (from 82 s) |
| race sets/s, late %, repeats % (pacing-parts) | 120.0, 0.50, 0.35 | 120.2, 0.31, 0.29 | 119.6, 1.34, 0.82 (97 of 99 late and 53 of 60 retained repeats in the last 3 s of race 2, GPU p50 9-12 ms; 2 late before that) |
| menus sets/s, late % | 116.5, 0.75 | 112.8, 0.83 | 117.1, 0.36 |
| set GPU ms p50 (median of seconds) / p99 (median) | 6.36 / 7.82 | 6.90 / 7.97 | 4.79 / 6.46 |
| encode ms p50 | 2.03 | 1.89 | 2.14 |
| end after wake-up (XRF e) p5/p50/p95/p99 | 5.35/5.63/6.30/6.66 | 5.35/5.62/6.09/6.47 | 0.09/0.15/0.40/0.90 |
| wake-up after the vsync (B) p5/p50/p95 | +0.27/+0.41/+0.68 | +0.23/+0.36/+0.61 | **+1.37/+2.49/+2.67** |
| end after the vsync (C) p50/p95 | +6.16/+6.96 | +6.08/+6.69 | +2.72/+3.11 |
| end vs its refresh's update window (C): before / inside / after | 0 / 0.1 % / 99.9 % | 0 / 0.1 % / 99.9 % | **12.0 % / 87.9 %** / 0.1 % |
| class changes between consecutive refreshes | 9 (0.2 %) | 8 (0.2 %) | **856 (9.9 %)** |
| end minus update end p5/p50/p95 | +0.57/+0.97/+1.84 | +0.56/+0.76/+1.59 | -2.97/-2.51/-1.91 |
| view D (achieved, steady 10 s lines) | end vsync+5.89-5.95 (p95 5.99-6.08), minus update end +0.60-0.68 (p95 +0.72-0.88); 153 fixed-delay frames in the first 10 s, then 0-2; 3 fallback/relock pairs (one at race entry) | vsync+5.88-5.90 (p95 5.91-6.29), minus update end +0.53-0.68 (p95 +0.60-1.15); 170 fixed-delay frames in the first 10 s; 2 fallback/relock pairs | (no lock) |
| SteamVR race frames: presented 0x / 2x+ / mispresented | 108 / 100 / 169 of 4002 (2.7/2.5/4.2 %) | 56 / 51 / 102 of 3809 (1.5/1.3/2.7 %) | 47 / 46 / 74 of 8648 (0.5/0.5/0.9 %) |
| ...where they fall | first 10 s of the race (35/30/37, including a 0.26 s early-start episode at 8.2 s) + the end episode (62/42/88) | race start (3/3/7) + the end episode (41/33/69); **0 between** | race 1 start, the end episodes, the race-2 finish; ~0 mid-race |
| early update-start episodes (C) | 8.2 s (0.08 s), 8.3 s (0.18 s), 29.1 s (0.41 s), 29.5 s (3.9 s) | 27.8 s (3.9 s), 31.7 s (0.13 s) | 9.4 s (0.16), 30.8 s (1.4), 35.5 s (0.4), 40.9 s (0.3), 68.6 s (3.4) |
| display time minus wake-up p50 | 27.18 ms | 27.22 ms | 25.10 ms |
| pose prediction ("poses predicted ahead", display minus locate) p50 | 43.7 ms | 43.7-43.8 ms | **41.4 ms** |
| interpolation holds (XRS w=1.000: the set's next game frame not ready, so the old one is shown again) | 184 (4.8 %; 116 in the end episode) | 170 (4.7 %; 142 in the first 2.4 s, about 3 mid-race) | **617 (8.8 %), in bursts all through both races** |

The end episodes, about 3-4 s before each quit or race exit (01 29.5 s, 02 27.8 s, 03 30.8 and 68.6 s), line up
with the owner leaving the race. They are probably the system menu or dashboard, not driving.

"One refresh late": SteamVR's mispresented count, as LANE_X used it. With the lock it is 4.2 % (01) and 2.7 % (02),
nearly all at race start and in the end episodes. 03 has 0.9 %.

## 2. Launch 3: the both-eye stutter "every few seconds"

What is NOT behind it (mid-race, both races):
- No set was presented twice or skipped by SteamVR's own count. The 0x / 2x+ / mispresented frames are all at race
  starts, the end episodes and the race-2 finish. The frames that ended before the update start have **0** anomalies
  out of 958.
- No late sets, no retained repeats (f=0), no display step other than 1 (XRF d=1.00 in 7413 of 7425 frames), no
  GPU spike (set GPU p99 5.5-8.2 ms per second).
- No clock beat between our wake-up and the grid. The wake-up interval is steadier than with the lock (|dev| p95
  0.21 ms vs 0.33-0.35), and the wake-up's phase holds at vsync+2.50-2.55 every second.
- Early-update episodes are rare (5) and are not "every few seconds". During them the end is clear of the update
  start (0 class changes).

What IS there, in bursts:
1. **The hand-over sits on the compositor's update start.** With a short frame (wake-up to end 0.15 ms), SteamVR
   wakes the app about 2.1 ms later than with the lock: at vsync+2.49, which is where the compositor's update starts
   (vsync+2.48 p50). So the release (0.11 ms after the wake-up) lands -0.19 / +0.19 / +1.04 ms (p5/p50/p95) from the
   update start. The update start drifts between vsync+2.2 and +2.85 over seconds. When it drifts above about +2.6,
   consecutive refreshes flip between "before" and "inside", 15-50 times a second for 1-3 s. Then come quiet stretches
   of 2-10 s with 0-4 flips a second (start at or below +2.4). The bursts in race 1 were at 39, 48-49, 52-63 s; race 2
   at 84, 91-94 and 97-106 s (pacing clock).

   Under the pick-up model of LANE_X (the compositor binds the newest released image when it reaches it during the
   update), each flip shows a set one refresh early, then repeats it, in whatever the compositor reads after our
   release. Packing removes the difference between the eyes, so the effect is a whole-image hitch and not a right-eye
   double. SteamVR's per-frame accounting does not count it, as before.
2. **The game's scene interpolation stalls more often.** These are sets whose next game frame was not sealed when the
   set started, so the old frame is shown again (w clamps to 1.000) and the next set jumps (content steps of 0.34 /
   0.66 frame instead of 0.5 / 0.5). Each one is a small hitch in both eyes. The rate is 8.8 % of sets, against 4.7-4.8 %
   today and 27-133 per race in sessions 19-20 (the highest of those was session 20's "before" launch, also an early
   end). They come in bursts all through 03; the median gap between bursts is 1.1 s. Their per-second count is
   anti-correlated with the update-start flips (r = -0.42), so these are two different events. Why an early end
   raises the hold rate is not known. The game ran 59-60 frames/s throughout, so the game itself did not slow down.

Both are both-eye events in bursts every few seconds, both are invisible to SteamVR's counters, and both are absent
or rare mid-race in 01 and 02 (01/02 flips 0.2 %; holds mid-race 01 about 60 in 27 s, 02 about 3 in 26 s). The flips
fit "whole picture hitching, momentarily like a double image" best. The holds may add to it. The logs cannot split
the two.

## 3. Packed (02) vs control (01), both END_PHASE=after

- Releases per set: 1 in packed (XRF i=a/a, one swapchain per set), 2 in the control. Same 4 pairs, same set rate.
- Pacing: late 0.31 % vs 0.50 %, repeats 0.29 vs 0.35 %. This is within the launch-to-launch spread.
- Set GPU p50 6.90 vs 6.36 ms (p99 7.97 vs 7.82). The mock had packed equal (LANE_AA). Different stretches of track
  and one launch each, so this is not significant. Note 03 (packed, no lock) reads 4.79. With the lock, our set's GPU
  work starts at vsync+0.4 and overlaps the compositor's render at vsync+2.4-3.3. Without the lock it starts after
  the render. So the measured span includes sharing with the compositor.
- End position: identical (view D and C: after 99.9 %, end minus update end p50 +0.97 vs +0.76).
- Compositor fields, race p50/p95: compositor GPU 0.87/1.44 vs 0.85/1.49 ms; pre-submit GPU 4.80/6.54 vs 4.69/5.93;
  total render GPU 5.77/7.49 vs 5.59/6.96; compositor CPU 0.47/1.21 vs 0.40/1.11; update start p50 2.42 vs 2.79
  (the compositor's own drift, as session 19 saw).
- Presents: 02 is cleaner (2.7 % vs 4.2 % mispresented), but the difference sits in 01's first 10 s and the end
  episodes. Mid-race both are about 0.
- Holds: 02 had the fewest of any launch measured (about 3 mid-race).

Nothing in 02 is worse than 01. Its advantage (one release, so the eyes cannot mix sets) only matters where 01 is
exposed: in episodes and at race start, which these 30 s races barely sampled.

## 4. 01 vs the owner's right-eye flicker in normal play

01 does show the session-20 launch-1 pattern twice:
- A 0.26 s early-start episode at race 8.2 s, with a burst of 12/9/12 SteamVR anomalies in that second. The first
  10 s of the race had 35/30/37.
- The end episode (29.1-33 s, update start at vsync+0.24-0.33, GPU p99 up to 15.9 ms) with 62/42/88 anomalies and
  116 interpolation holds.

That is the "right eye worse towards the end" window of session 20 launch 1 (published start 0.35 ms in the last D
line, against 0.57 then). Between those, 01 is clean. A 33 s race (and, if the end episodes are the quit, about 25 s
of real driving) is too short to see how often mid-race episodes come in long play. Session 19's doubled launch had
them 5-6 s apart. A per-eye control will show the right-eye flicker exactly in those moments, and packing should
not, so long races are the test.

## 5. Recommendation

**Default: packed + `END_PHASE=after`.** It was clean for the owner, and it is at least as good as the control in
every measurable. The lock's cost is **not a refresh**: the display time and the compositor frame that uses the set
are the same in all three launches (display time vsync+2.59, used by the next compositor frame). The cost is that
SteamVR wakes the app 2.1 ms earlier when the frame is long. The real price is +2.1-2.3 ms of pose age
(43.7 vs 41.4 ms), about 5 % of the pose prediction. The 41 ms pipeline is the latency lever, not the end phase.

**No cheaper end position exists, because of how SteamVR wakes the app on the Frame:**
- "off" is not the old hand-over. With a short frame, SteamVR's adaptive wake-up moves it onto the compositor's
  update start, the worst place. Session 20's "before" launch showed the same feedback (wake-up moved to vsync+1.18
  p50, p95 +2.57).
- "before" (end ahead of the update start) is defeated by that feedback, so any short wake-up-to-end runs into the
  update start again.
- Any end between the update start (+2.2..2.9) and the update end (+5.2..5.5) is the window that doubled in
  sessions 18-19.
- A fixed `END_DELAY_US` cannot follow the episodes. To clear the update end it needs about 5500-5800 us, which is
  the lock without its tracking. Session 17's 6000 us launch was bad.
- A smaller after margin (`after:100` instead of 300) saves 0.2 ms and halves the tail margin (end minus update end
  p5 is +0.56 now). Not worth it.

If latency matters later, take it from the pipeline (sets located 5 refreshes ahead), not from the end phase.

**Follow-up session (4 launches, interleaved A B A B, each one a full 3-lap Time Trial of 2 min or more; note the
lap or moment of any glitch; quit only after the race):**

| launch | setting | why |
| --- | --- | --- |
| 1 | `KARTFRAME_STEREO_PACKED=1` (+ default after) | the candidate default, over a long race with mid-race episodes |
| 2 | defaults (per-eye + after) | control: the right-eye flicker in long play |
| 3 | `KARTFRAME_STEREO_PACKED=1` | repeat (launch-to-launch spread, sessions 16/18) |
| 4 | defaults | control repeat |

All four with `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_SESSION_ENV_ONCE=1`, no recording.
Owner, per launch: right eye during head turns in long driving, any whole-image hitch, and the time or lap of each.
Logs: per-eye vs packed in the early-start episodes (count them per launch and match them to the verdicts), the
interpolation holds, and mispresented outside the episodes.

Drop the queued h21-04/05/06 first. 04 (packed without FDM) is moot, since launch 2 showed no seam or foveation
artefact. 05/06 (queue priority, split) target the doubling in per-eye mode, which packing should make unnecessary.
Keep them only if launch 2 of the follow-up still flickers.

Open: why an early end raises the interpolation hold rate (the seal of the game frame against the set start is not
logged). It does not matter if the lock stays.
