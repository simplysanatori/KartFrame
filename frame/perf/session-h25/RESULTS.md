# Headset session 25: results (2026-10-27, analysis lane, logs only)

Data: `$KARTFRAME_HOME/sessions/` `20261027-220915` (01), `-233336` (02), `-233740` (03). Method as in session 24
(`session-h24/RESULTS.md`): every per-second `cpulog:` line (now with the lane AK lock fields), the 5 s FPSLOG and
`GX thread:` lines, `vr pacing:`, the XRS set lines and the vrframes F lines on one race clock; race time 0 is the second
`presentation=immersive-race`; steady = race time 10 s to the quit minus 3 s. `thermal.tsv` on the same clock through the
vrframes P line.

Owner verdicts: 01 "good"; 02 "okay but had jitter"; 03 "good". No visual fault reported.

## Mapping (confirmed from the logs)

| launch | folder | `session.queue: using` | game.log | end |
| --- | --- | --- | --- | --- |
| 01 | 220915 | h25-01-narrow.env | `frame worker lock: narrow`; cpulog `[narrow]` | 143 at 22:13:58 |
| 02 | 233336 | h25-02-full.env | `frame worker lock: full`; cpulog `[full]` | 143 at 23:37:28 |
| 03 | 233740 | h25-03-narrow.env | `frame worker lock: narrow`; cpulog `[narrow]` | 143 at 23:40:23 |

All three: GX thread on, 90 Hz, interpolation 33 ms, 1728 per eye, packed, end phase after. `session.env` and
`session.queue/` are empty afterwards.

## SteamVR focus losses, and what was a race

SteamVR took the app's focus (OpenXR session VISIBLE, not FOCUSED; input blocked) for long stretches in 01 and 02: the
dashboard or system menu was up. In those stretches the game was mostly **paused on a still frame** (FPSLOG draws and GX
records identical to four digits for minutes) and still rendered at 90 sets a second.

| launch | race start to quit | SteamVR unfocused | of it a paused still frame | driving | full race? |
| --- | --- | --- | --- | --- | --- |
| 01 | 242 s | 108 s in 9 stretches (37-52, 68-125, 131-138, 148-162, 198-209 s ...) | 40-50 s and 70-140 s | about 130 s | probably (laps are not logged) |
| 02 | 139 s | 71 s (57-122 s) | 63-118 s | about 68 s | no |
| 03 | 124 s | 3 s (at the quit) | none | 121 s | probably |

So the per-launch table uses the **driving seconds** (steady, focused, 1 s margin around each focus change). The two
paused stretches give an extra, controlled A/B: the same still frame, full against narrow (section 2).

## 1. Per launch (driving)

| | 01 narrow | 02 full (control) | 03 narrow |
| --- | --- | --- | --- |
| driving seconds analysed | 105 | 53 | 109 |
| **game fps, mean / min 5 s / seconds < 58** | **60.0 / 59.5 / 0 %** | **53.9 / 42.3 / 64 %** | **60.0 / 59.9 / 0 %** |
| same, whole steady race incl. unfocused | 59.8 / 57.7 / 3 % | 50.5 / 42.3 / 84 % | 60.0 / 59.9 / 0 % |
| draw calls a frame, mean (window range) | 481 (419-529) | 508 (442-579) | 486 (424-528) |
| game-thread CPU ms/frame (p10-p90) | 8.17 (7.76-8.71) | 8.72 (8.27-9.42) | 8.66 (8.35-9.05) |
| **game waits for the GX thread (`GXDrawDone`, 2 drains), ms/frame** | **2.32** | **4.60** | **2.85** |
| **GX CPU / wall / blocked (wall - CPU), ms/frame** | **5.86 / 8.31 / 2.45** | **6.05 / 12.09 / 6.03** | **5.88 / 8.79 / 2.91** |
| **GX lock wait fifo / drawdone / dl = total, ms/frame** | 0.71 / 0.83 / 0.19 = **1.74** | 2.83 / 1.42 / 0.76 = **5.00** | 0.52 / 1.85 / 0.25 = **2.61** |
| overlay hook lock wait (game thread), ms/frame | 0.01 | 0.65 | 0.03 |
| worker hold per set: held / encode ms (%) | 1.67 / 3.37 (48 %) | 3.78 / 3.78 (100 %) | 2.34 / 4.11 (57 %) |
| worker hold per frame: seal / prep ms | 0.93 / 0.14 | 1.00 / 0.14 | 0.99 / 0.15 |
| GX waits for SEALED / DONE, ms/frame | 0.96 / 0.22 | 1.50 / 0.03 | 0.83 / 0.03 |
| sets/s, late % (n), repeats % (n) | 90.7, 0.00 (0), 0.00 (0) | 90.5, 0.50 (24), 0.25 (12) | 90.7, 0.00 (0), 0.00 (0) |
| sets without a previous frame | 0 of 7168 | 0 of 3573 | 0 of 9893 |
| sets off the even motion by > 0.25 / > 0.5 frame | 0.1 % / 0.1 % | **12.0 % / 3.6 %** | 0.0 % / 0.0 % |
| game frames >= 25 ms / >= 33 ms (hitch lines) | 6 / 0 | **312 / 82** | 0 / 0 |
| SteamVR frames not presented / presented twice, driving | 0 / 0 | 0 / 0 | 0 / 0 |
| same, whole race (all inside the unfocused stretches) | 91 / 82 | 83 / 83 | 0 / 0 |
| GPU ms/set p50 / p99 (median s); worst-second p99 | 8.89 / 9.83; 10.69 | 8.95 / 9.97; 11.74 | 8.66 / 9.93; 11.77 |
| GX faults; new log messages | 0; none | 0; none | 0; none |
| hottest zone, race mean / max (thermal.tsv) | 86.2 / 95.4 C | 81.8 / 92.7 C | 83.1 / 88.0 C |
| GPU clock; CPU clock caps; cpu7 min | 903 MHz throughout; none; 1593 MHz | 903; none; 1824 | 903; none; 1824 |

Per frame values in 02 are per slower frame (54 fps), so its ms/frame read a little high; the paused A/B below has no
such bias in the GX lines that matter (per second the GX lock wait is 270 ms/s in 02 against 104-157 in 01/03).

## 2. The paused still frame: the same image, full against narrow

01 race time 70-140 s (532 draws, 8167 GX records a frame) against 02 race time 63-118 s (537 draws, 8142 records):

| | 01 narrow | 02 full |
| --- | --- | --- |
| game fps (5 s min) | **59.9 (58.7)** | **47.6 (42.9)** |
| GX lock wait, ms/frame | 1.04 | 5.84 (-82 % with narrow) |
| GX blocked (wall - CPU), ms/frame | 2.10 | 6.85 (-69 %) |
| game waits at `GXDrawDone`, ms/frame | 4.28 | 8.86 (-52 %) |
| overlay hook lock wait | 0.02 | 0.49 |
| worker held per set (encode) | 1.12 ms (2.97), 38 % | 3.95 ms (3.95), 100 % |
| game CPU ms/frame | 5.98 | 6.29 |

Same frame, same game CPU: the full lock alone costs 12 frames a second here.

## 3. Answers

**(1) Did narrow cut the GX thread's blocked time and the game's wait as the mock predicted (about -2/3)?** Yes. GX lock
wait 5.00 -> 1.74 / 2.61 ms a frame driving (-65 % / -48 %), -82 % on the same still frame; GX blocked 6.03 -> 2.45 /
2.91 (-59 % / -52 %), -69 % on the still frame; the game's `GXDrawDone` wait 4.60 -> 2.32 / 2.85 (-50 % / -38 %), -52 %
on the still frame; the overlay hook's wait about 0. One part came in worse than predicted: narrow's hold is 1.1-2.4 ms a
set (37-57 % of the encode) on the headset, not about 0.9 (30 %). Per second the GX lock wait follows that hold (r = 0.96,
0.92, 0.79 in 01, 02, 03), and the hold is larger while focused and driving (1.6-2.4 ms) than in the unfocused pauses
(1.1-1.2), which points at the OpenXR provider call (`request_stereo_frame`: the packet, the acquired swapchain image,
input) that narrow still makes under the lock.

**(2) Does it explain 02's jitter?** Yes. 02 drove at 42-60 fps (64 % of seconds below 58; dips at 23-33 s and 43-57 s),
with the GX thread blocked 6 ms a frame; 12 % of its sets were off the even motion (narrow 0-0.1 %), 312 game frames
took 25 ms or more (narrow 0-6), and it had the only late sets (24) and repeats (12). The jitter is the game's frame rate
wobbling under the interpolation, not missing previous frames (0 in all three: the 33 ms window covers them) and not
SteamVR (its skips happened only behind the dashboard, in 01 as much as in 02). 02's driving was shorter and somewhat
heavier (508 draws against 481-486), but the still-frame A/B removes that: 47.6 against 59.9 fps on the same image.

**(3) Anything new or worse with narrow?** No. Both narrow launches have exactly the control's start-up warnings and
nothing new (their only lines the control lacks are the lock mode and a vanilla-camera note for one item; the control alone had a Gamescope WSI swapchain re-creation); 0 GX faults, 0 late sets, 0
repeats, 0 SteamVR skips while driving, 0 sets without a previous frame. The DONE wait appeared as the mock said: 0.22
(01) and 0.03 (03) ms a frame. The set encode is 3.37 / 4.11 ms against 3.78 (no trend), GPU per set unchanged. The owner
saw no visual fault in either narrow launch.

**(4) What still limits Grand Prix at 60 with narrow?** Up to about 530 draws a frame it holds 60.0 with about 3.4 ms of
slack (game CPU 8.2-8.7 + run queue 0.3 + `GXDrawDone` 2.3-2.9 + SEALED 0.8-1.0 + overlay hook 0.7 of 16.7 ms). Neither
narrow launch reached 560+ draws while focused; the only heavy section is 01's 589-615-draw stretch at 195-212 s (behind
the dashboard, game running): 59.1 fps, 5 s min 58.1, GX wall 10.6 ms a frame (FIFO decode 6.2), blocked 3.3, the game's
drain wait 5.5, SEALED 1.6. So the next limits are, in order: the GX thread's own decode per draw, the lock time still
left (narrow's 1.1-2.4 ms a set at 90 sets a second, and the seal's 0.9-1.0 ms a frame), and the SEALED handshake. The
game thread's CPU (8.2-8.7 ms) is not one. Thermal is not one either: 01, the longest launch, peaked at 95.4 C with the GPU
at 903 MHz and no CPU cap, and ran 60.0 throughout its driving.

**(5) Recommendation.** Make `narrow` the default: both narrow launches were "good", ran at 60.0 fps with 0 late sets and
no faults, and the control showed the slowdown and jitter they lack; the still-frame A/B shows the lock is the cause.
`KARTFRAME_WORKER_LOCK=full` stays as the override. Next levers, ranked:
1. Shrink narrow's hold: time the provider call (`request_stereo_frame`), `interpolated_stereo_frame` and
   `prepare_late_stereo_replay` separately; if the provider call is the 1-1.5 ms extra (the focused-versus-paused
   difference says so), take the XR packet before the renderer lock (audit: it touches only the OpenXR side and the
   stereo registration mutex). Expected: hold toward the mock's 0.55 ms, GX lock wait under 1 ms a frame.
2. The seal: 0.9-1.0 ms a frame under the lock plus the GX thread's 0.8-1.0 ms SEALED wait.
3. The GX thread's decode per draw (FIFO decode 4.7-6.2 ms a frame), for the 560-615-draw sections.
4. The per-set encode (3.4-4.1 ms on the worker, render bundles), which also gates 120 Hz on the GPU side.
5. 120 Hz again after 1-4, with `thermal.tsv` over a full race.
For the next session: a heavy course on purpose (one with 560+ draws), and no dashboard during a measured race.

Scratch scripts: `~/.cache/kf-h25` (an25.py, smooth.py, vrf25.py, th25.py), not committed.
