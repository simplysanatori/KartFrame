# Headset session 22: results (2026-10-27, analysis lane, logs only)

Data: `$KARTFRAME_HOME/sessions/` `20261027-110933` (01), `20261027-111114` (02), `20261027-111251` (03),
`20261027-111448` (04). Tools: `frame/tools/phase.py` (views A, C, D; the fixed version ran without errors on all
four), `frame/perf/pacing-parts.py` (on copies of game.log), `report.txt`, plus per-frame scripts in the lane's
scratch dir. Those scripts line up the vrframes F lines, the XRF/XRS lines, the `vr pacing` / `vr eyes` / `hitch`
lines and the `end phase:` lines on the race clock (race time 0 = the first XRF after `entered RaceScene`).

Owner verdicts: 01 "perfect"; 02 "bad, doubling in the right eye"; 03 "good initially, but slight jitter/stuttering
every few seconds with head movement"; 04 "terrible: right eye true doubling every moment, left also bad, with head
movement".

## Mapping (confirmed from the logs)

| launch | folder | wrapper `session.queue: using` | game.log | end |
| --- | --- | --- | --- | --- |
| 01 packed | 110933 | h22-01-packed.env, `KARTFRAME_STEREO_PACKED=1`, `END_PHASE=after` | `stereo packed: on (3456x1728, one swapchain per set)`; `end phase: ... after`, locked | status 143 at 11:10:53 |
| 02 per-eye | 111114 | h22-02-pereye.env, `STEREO_PACKED=0`, `END_PHASE=after` | no packed line; `end phase: ... after`, locked | status 143 at 11:12:20 |
| 03 packed | 111251 | h22-03-packed.env, `STEREO_PACKED=1`, `END_PHASE=after` | packed on; after, locked | status 143 at 11:14:08 |
| 04 per-eye | 111448 | h22-04-pereye.env, `STEREO_PACKED=0`, `END_PHASE=after` | no packed line; after, locked | status 143 at 11:15:58 |

All four: TU_DEBUG=sysmem, 4 eye pairs, 5 of 5 per-app SteamVR settings read back.

**These were not full 3-lap races.** Each launch ran about 1 minute (game.log 0.8-1.1 min). The race scene lasted
29-40 s, including the intro and countdown. The owner then quit mid-race through the dashboard (exit status 143
every time; the last early-update episode of each launch is the dashboard). Driving time up to the dashboard: 01 32 s,
02 22 s, 03 37 s, 04 25 s, about one lap. So the "long race" question of the README is still open. The A/B itself is
clear (below).

## 1. Per launch

"Race" = race scene to the end of the log (includes the dashboard episode). "Driving" = race start to the start of
the dashboard episode.

| | 01 packed | 02 per-eye | 03 packed | 04 per-eye |
| --- | --- | --- | --- | --- |
| launch (about) / race scene / driving | 66 s / 36 s / 32 s | 48 s / 29 s / 22 s | 60 s / 40 s / 37 s | 54 s / 31 s / 25 s |
| race sets/s, late %, repeats % (pacing-parts) | 120.3, 0.37, 0.30 | 118.0, 3.25, 2.03 | 120.1, 0.35, 0.35 | 118.9, 1.99, 1.29 |
| ...where the late sets fall | dashboard (14) | dashboard (s21-25; 12 in the last driving second) | dashboard (s36-38) | dashboard + 2+2 in the heavy stretch |
| menus sets/s, late % | 113.7, 1.00 | 112.7, 0.84 | 110.3, 1.11 | 113.6, 0.80 |
| set GPU ms, race: median-second p50 / worst p99 (report) | 7.25 / 17.08 | 7.30 / 19.10 | 7.40 / 17.01 | 7.24 / 18.99 |
| set GPU p50 in the heavy stretch (see 3) | 7.6-8.0 (s18-19) | 8.0-8.35 (s19-21) | 8.0-8.4 (s19-26) | 7.6-8.15 (s21-24) |
| encode p50 (race) | 2.08 | 1.95 | 2.10 | 1.97 |
| end after wake-up (XRF e, fresh sets) p5/p50/p95/p99 | 5.44/5.64/6.05/6.39 | 4.41/5.59/6.09/6.44 | 5.46/5.65/6.04/6.46 | 5.32/5.64/6.13/6.56 |
| lock (D, steady 10 s lines): end after vsync p50; minus update end p50 (p95) | 5.87-5.89; +0.52-0.65 (0.58-0.82) | 5.87-5.89; +0.51-0.71 (0.57-0.76) | 5.83-5.89; +0.54-0.65 (0.65-0.83) | 5.89-5.90; +0.57-0.63 (0.68-0.97) |
| fixed-delay frames / fallback-relock pairs | 181 in the first 10 s (menus); 1 pair at race +2.1 s (1 frame) | 151 in the first 10 s; 4 pairs at +24.4-25.8 (dashboard) | 129 in the first 10 s + 2 pairs at load; 3 pairs at +38.1-38.7 (dashboard) | 158 in the first 10 s; 7 pairs at +27.1-28.1 (dashboard) |
| update-window class (C): before / inside / after | 0 / 3 / 4290 (99.9 %) | 0 / 4 / 3206 (99.9 %) | 0 / 8 / 4611 (99.8 %) | 0 / 0 / 3487 (100 %) |
| class changes between refreshes | 6 (0.1 %) | 8 (0.2 %) | 14 (0.3 %) | 0 |
| end minus update end p5/p50/p95 (C) | +0.56/+0.75/+1.49 | +0.56/+0.80/+1.92 | +0.57/+0.80/+1.48 | +0.57/+0.82/+1.73 |
| SteamVR presented 0x / 2x+ / mispresented, race (C frames) | 32 / 29 / 50 of 4290 | 195 / 161 / 416 of 3206 | 203 / 170 / 306 of 4611 | 187 / 99 / 295 of 3487 |
| the same, driving only (0x = compositor frame without update) | **11 / 11 / 12 of 3875 (0.3 %)** | 56 / 56 / 57 of 2688 (2.1 %) | **148 / 146 / 204 of 4404 (3.4 %)** | 74 / 66 / 82 of 3000 (2.5 %) |
| ...where | s1-2, 8, **18.96 (7 in 0.23 s)**, 24.7 | **19.5-22.4 (55)** | s1.7, **19.8-27.6 (146)** | s1.3, 4.7, 9.7, 18.0 (2), **21.7-25.0 (64)** |
| early update-start episodes (C, race time) | 2.2 s (0.39), 32.4 s (3.6, dashboard) | 22.5 (0.08), 22.6 (0.10), 24.0 (4.7, dashboard) | 22.0 (0.22, inside the heavy stretch), 36.8 (1.4) + 38.2 (2.2, dashboard) | 26.8 (4.1, dashboard) |
| interpolation holds (XRS w=1.000) race / mid-race (5 s to the dashboard) | 258 (6.2 %) / **0** | 126 (3.9 %) / **0** | 247 (5.3 %) / **0** | 233 (6.6 %) / 4 |
| game frames/s while driving | 60 | 60-61 | 60-61 | 61 |

The holds sit in the race intro (s1-4, 21-60 a second) and in the dashboard episode. While driving there were none
(04: 4). That is better than session 21's 4.7-8.8 %.

## 2. The A/B: packed (01, 03) against per-eye (02, 04)

**The owner's verdicts split cleanly by mode: both packed launches good (01 perfect, 03 good with a residual), both
per-eye launches doubled (02 bad, 04 terrible). The run order was interleaved, so order does not explain it.**

In the logs, nothing on the compositor side separates the modes:
- End placement is the same in all four: 99.8-100 % "after", 0-14 class changes, end minus update end p5 +0.56-0.57.
- In the clean stretch (race 5 s to 18 s) SteamVR's counters are about 0 in all four (0-2 frames without an update
  in the whole stretch).
- Every launch's 0x / 2x+ / mispresented frames are in the same two places: the heavy stretch of the course (3) and
  the dashboard. The per-eye launches' higher race percentages come from shorter driving against an equally long
  dashboard episode.
- The game side always had both eyes from one pose packet (XRS `same=1`, kL = kR, mL = mR = 0 in every set of all
  four).

What does differ (measurable, but not the doubling):
- Releases per set: 1 packed, 2 per-eye (as designed).
- XRF `i=` (released image index left/right): packed always equal. Per-eye: the right swapchain's index is offset
  from the left one's in all 4 pairs in 02 and in 2 of 4 pairs in 04. The two swapchains of a pair were acquired a
  different number of times at some point. Harmless by OpenXR semantics (separate swapchains), and it does not follow
  the verdicts (02 "bad" has more offset pairs than 04 "terrible").
- Encode p50 is 0.1 ms higher packed (2.08-2.10 vs 1.95-1.97). SteamVR's pre-submit GPU p50 while driving is 5.22 /
  5.32 packed vs 5.05 / 5.14 per-eye. Both differences are small.
- Game `hitch:` lines (game frame interval of 20 ms or more): 01 1-4 a second, 03 7-19 a second, per-eye 0 while
  driving. The game still ran 60 frames/s with 0 holds, and 03's clean stretch had these hitches with 0 SteamVR
  anomalies. So they are not visible. Cause not looked into (see 3).

**Is 04 worse than 02 in the compositor's view? No.** 04 had 0 class changes (02: 8), 1 early-update episode (the
dashboard; 02: 3, two of them short ones just before its dashboard), and a similar driving skip rate (2.5 % vs 2.1 %,
both in the heavy stretch). "Terrible" vs "bad" is not in SteamVR's counters or our logs. This fits the per-eye
pick-up seen in sessions 19-21: SteamVR's per-frame accounting does not count the right eye taking another set.

One finding against LANE_X's model: LANE_X said per-eye exposes the right eye only when the end flips between
"inside" and "after". In 04 the end was "after" in 100 % of refreshes with 0 flips, yet the owner saw the right eye
doubled "every moment". So per-eye doubles even with a steady end well after the update end. The window-flip model
is not the whole mechanism for per-eye, and no end phase makes per-eye clean. Packing is the fix.

## 3. Launch 3's residual: GPU overrun on one stretch of the course, not the end phase

**What it is.** From race second 19.8 to 27.6 in 03, SteamVR's compositor skipped one of our frames 146 times. The
compositor frame had no update times and our frame was presented 0x; the frame before was shown twice and counted
mispresented. That is 12-33 a second for 8 s. Before 19 s and after 28 s: 0. The skip is a whole-image repeat. The
compositor shows the previous app image again, and with head rotation it is visible as a stutter. 03's head speed in
that stretch was mostly about 100 deg/s (49-146, p50 per second).

**Cause: our GPU work per frame exceeds what the compositor can still fit into the refresh.** SteamVR's own per-frame
pre-submit GPU time (the app's GPU work for that frame, vrframes v2 field) predicts the skip almost perfectly. Skip
share by pre-submit GPU, driving frames of all four launches:

| pre-submit GPU ms | 01 | 02 | 03 | 04 |
| --- | --- | --- | --- | --- |
| < 6.5 | 1 of 3827 | 1 of 2572 | 1 of 4014 | 8 of 2903 |
| 6.5-7.0 | 0 of 37 | 1 of 61 | 23 of 216 | 7 of 36 |
| 7.0 and more | 10 of 11 | 54 of 55 | 124 of 174 | 59 of 61 |

Per second, the pre-submit p95 and the skip count correlate r = 0.82 (03), 0.92 (02), 0.94 (04). The compositor's
own GPU (0.81-0.83 ms p50, p99 0.92-0.95) did not change. 03's game-side set GPU p50 went from 7.2-7.4 to 8.0-8.4 ms
in the same seconds (p99 up to 11.4). With the compositor's 0.8 ms on the same GPU, that is more than the 8.33 ms
refresh.

**The same stretch hit every launch** at the same race time (the same place on the course): 02 at 19.5-22.4 s (55
skips, then the dashboard), 04 at 21.7-25.0 s (64), and 01 only lightly at 18.96 s (7 skips in 0.23 s, pre-submit p95
6.6-7.0 against 03's 7.7-8.4). 01 was "perfect" because its heavy stretch was lighter: the same course, but a
different line or view. The logs have no kart position to tell which. Head speed alone does not raise the GPU: 03's
seconds 29-34 had the fastest head turns of the session (150-181 deg/s) with pre-submit p95 5.6-5.8 and 0 skips.
Head speed vs skips per second: r = 0.29 (03), 0.42-0.48 (others). Head movement makes the repeats visible; the
course content causes them.

**Every other frame is heavier, and the skips land on the heavy ones.** The pre-submit GPU alternates by about 0.3-0.6
ms between consecutive frames (03 heavy stretch: p50 6.47 vs 6.17, p95 7.70 vs 6.65). Of 03's 147 skips, 120 are on
the heavy parity, and the most common gap between skips is 4 refreshes, i.e. 2 game frames. At 60 game frames over
120 sets, one set of each pair also carries the game frame's own GPU work, which is encoded in the seal: probe blits,
EFB downloads, texture conversion. Our pacing hides this (0 late sets in the stretch), because we release only after
the fence and end at the locked phase. But the GPU time is taken from the compositor's next frame.

**What it is NOT** (01 vs 03 compared directly):
- Not the end phase or the lock. In the heavy stretch, the end minus update end p5 stayed +0.49 to +0.64 per second,
  the same as the clean stretch (+0.52-0.63) and as 01. The margin did not shrink or go negative, except for single
  frames (min -0.48), the same as in 01's clean seconds. There were no lock fallbacks while driving (03 0, 01 1 frame
  at +2.1 s), and 14 class changes in the whole race. The compositor's update start moved earlier in the stretch
  (2.34-2.51 instead of 2.8-3.0), because it was skipping.
- Not early-update episodes. 03 had one 0.22 s episode inside the stretch at 22.0 s. Nothing periodic.
- Not late sets, repeats or holds: 0 late sets, 0 retained repeats and 0 holds in 03 from 5 s to 35 s, and game
  frames 60-61/s.
- Not thermal, as far as the logs go. No SoC temperature or GPU clock is logged in these sessions. The clean stretch
  costs the same in all four launches (pre-submit p50 4.7-5.3, set GPU 6.9-7.5) despite running back to back over
  6 minutes.
- "Every few seconds" is not a beat. It is one 8-second stretch with skips every 2-10 refreshes, perceived as
  recurring jitter while the head moves.

## 4. Recommendation

**(a) Packed stays the default: confirmed.** Two clean packed launches against two doubled per-eye launches,
interleaved. Together with session 21 (packed clean twice), that is 4 of 4 packed launches without a double image,
and per-eye doubled in both controls here. **Per-eye can now be documented as "known doubled on the Frame"**, even
with the end lock holding the end after the update end in 100 % of refreshes (04). Keep `KARTFRAME_STEREO_PACKED=0`
as a diagnostic switch only. The h21-05/06 ideas were for per-eye doubling and are no longer needed for that.

**(b) The launch-3 residual is a GPU budget overrun at one stretch of the course.** A larger `after` margin
(`END_PHASE=after:<us>`) would not help: the margin did not shrink, and it would only add pose age. The cheapest
things to try are the two switches already built in lane Y. Neither needs a build, both are env only, and both are
compatible with packed (the split acts on the eye loop and the sink, not the swapchain):
- `KARTFRAME_SET_SUBMIT_SPLIT=3`: a set as three queue submissions, so the GPU can run the compositor's work between
  the pieces. In the mock, the GPU span per set read 0.8 ms lower than default, back to back (not explained).
- `KARTFRAME_QUEUE_PRIORITY=low`: our queue at global priority 128, so the compositor's queue wins when both are
  ready. Turnip accepted it in the mock; its effect on SteamVR is unknown.

Two headset launches, packed (the default) + `END_PHASE=after`, same course as session 22. Each one drives through
the heavy stretch (about 15-25 s after the countdown, where the owner felt it in 03) with head turns there, and
completes at least one full lap:

| launch | env | settles |
| --- | --- | --- |
| 1 | `KARTFRAME_SET_SUBMIT_SPLIT=3` | whether interleaving keeps the compositor from skipping when our frame is heavy |
| 2 | `KARTFRAME_QUEUE_PRIORITY=low` | whether priority does the same |

Both with `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_SESSION_ENV_ONCE=1`. Read: the skip share at
pre-submit GPU of 7.0 ms or more (today 71 % in 03, 91-98 % in the others) and the skips per second in the heavy stretch
(03: 12-33). Session 22's 03 is the reference. If either leaves the heavy stretch at 01's level (a few skips, no
"jitter" verdict), make it the default after one repeat.

If neither helps, the remaining levers are GPU cost:
- spread the per-game-frame seal work (probe blits, EFB downloads, texture conversion) over both sets of a pair, or
  move it to the lighter one. That is a code change; it takes 0.3-0.6 ms off the heavy parity, about the overrun.
- 90 Hz, the owner's accepted last fallback.

Render scale stays out (owner decision).

Open:
- why 01's pass through the heavy stretch was lighter (no kart position or view direction in the logs);
- why packed launches log game `hitch:` lines and per-eye does not (01 2/s, 03 10/s; no visible effect found);
- whether full 3-lap races show the heavy stretch every lap (expected, since it is course content).
