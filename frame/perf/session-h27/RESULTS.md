# Headset session 27: results (2026-10-28, lane AP, logs only)

Data: `$KARTFRAME_HOME/sessions/20261028-092053`, the only folder after `20261028-021654`: **one launch with both
races in it** (09:20:53 to 09:25:13, exit 143), not two launches. Defaults (GX thread on, narrow lock, 90 Hz, 1728,
packed, end phase after, interp 33, menu controllers eyes), no `session.env`, so no `KARTFRAME_CPULOG`/`FPSLOG`/
`FRAME_LOG` lines: the evidence is the per-second `vr pacing:` lines, the `hitch:` lines (each game frame of 20 ms or
more), the 10 s interpolation/eye-pose lines, `vrtiming.log` (SteamVR per second), `thermal.tsv` (lane AO columns) and
the journal. Clock: pacing t (t 1 = 09:21:08; checked against the journal's focus transitions at t 127/129/240).
Game fps is estimated from the hitch lines (frames = hitches + (1000 - their ms) / 16.67 per second), so it counts
only frames of 20 ms or more as slow. Scratch scripts: `~/.cache/kf-laneAP` (sec.py, seg.py), not committed.

| part | t | what |
| --- | --- | --- |
| menus | 3-29 | boot menus |
| Time Trial vs ghost | 31-107 | race presentation t 31 (frame 1548), exit t 108; SteamVR focus lost t 101-104 |
| menus | 109-127 | |
| Grand Prix | 133-240 | race presentation t 133 (items: triple bananas, a shell thrown); KartFrame's VR settings panel (X+Y chord) open t 134-135 and t 137-140; quit t 241 |

Owner: "darn near perfect; no slowdown in either; the only slowdown was with the SteamVR dashboard open over the
running game (settings), gone the moment it closed; cleanest run yet."

## 1. Per race (driving seconds; focus loss and the settings panel excluded, below)

| | Time Trial (t 41-99, 58 s) | Grand Prix (t 143-238, 95 s) |
| --- | --- | --- |
| game fps est. mean / min 5 s | **60.00 / 60.00** | **59.71 / 58.90** |
| hitch frames (>= 20 ms) / >= 25 / >= 33 ms | 0 / 0 / 0 | 96 (1.0 a s, p50 20.3 ms) / 9 / 3 |
| CPU on the hitch frames: game / run queue p50 (p90) | - | 7.0 / 0.7 (1.5) ms; worker wait 0 |
| game-thread CPU a frame, GX wait, GX lock wait | not logged (CPULOG off at the defaults) | same |
| sets/s; late; repeats; missed | 90.6; 0; 0; 0 | 90.6; 2 (0.02 %); 1; 0 |
| interpolation weight 1 (10 s windows) | 0 in every window | 41, 59, then 9-17, then 0-3 (start crowd) |
| sets without a previous frame | 0 | 3 (of about 8600) |
| SteamVR: not presented / reused / mispresented | 0 / 0 / 0 | 0 / 0 / 0 (all of the run's 5 / 17 / 37 are at loads, transitions, the panel and the quit) |
| GPU ms/set p50 / p99 (median s); worst-second p99 | 9.50 / 10.33; 11.20 | 8.63 / 9.83; 11.11 |
| encode ms p50 / p99 (median s); worst | 1.81 / 2.51; 7.93 | 3.30 / 9.13; 10.06 |
| finished sets dropped (overtaken) | 0 | 0 |
| hottest zone mean / max | 80.0 / 85.3 C | 86.9 / 91.1 C |
| clocks | cpu2-4 3148, cpu5-6 2956 MHz (no cap, min 2899); GPU 903 MHz throughout | same (cpu7 governor 2035-3052) |
| busy / game / other CPUs (of 5) | 1.94 / 1.00 / 0.93 (max 1.16) | 2.14 / 1.45 / 0.69 (max 1.27) |
| runnable threads mean / max | 3.4 / 9 | 4.3 / 9 |

**The machine was quiet.** `launch.log` has no build warning; the journal has no compiler or container lines during
the launch; `report.txt`: other processes 0.94 CPUs mean (max 2.42, at the launch and the quit), game 1.26, hottest
zone 84.5 mean / 96.9 max (one second at the launch). The ~0.7-0.9 "other" CPUs while driving are SteamVR itself
(XR service loop thread about 0.57 CPU, vrserver, vrcompositor, eyetracking about 0.13 each) plus the Steam client.
Run-queue wait on hitch frames 0.1-0.7 ms (session 26 under the builds: 10-12 ms).

One thing outside the game: the Steam client has been looping `UpdatesJob: apps still needs updates, run again`
since about 09:10 (about 7 log lines a second during the launch, 14 a second afterwards; the steam process about
6 % of a CPU). Small, not ours; it ends when Steam finishes or is restarted.

## 2. The ghost

- `Time Trial ghost: composite of a depth copy left flat (no perspective draw in the copied picture)` once, at the
  first race frame (1548, nothing drawn yet); `pass 2 (21 perspective draws, 1 other; copies in passes 2..2) is
  drawn by each eye at the composite in pass 4 (alpha 0.50)` at frame 1549. The reject message is logged up to 4
  times a session (`link_composite_source`), so no other frame of the race was left flat.
- Cost: none visible on the CPU (Time Trial encode p50 1.81 ms against the Grand Prix's 3.30; 0 hitch frames; 60.00
  fps). GPU p50 9.50 against 8.63 ms: different courses (the course is not logged), session 26's ghost race was 8.55,
  and the ghost's 21 re-issued draws per eye are small; still 1.6 ms under the 11.1 ms refresh at p50, 0.8 at p99.

## 3. The "dashboard" slowdown: it was KartFrame's own settings panel

Two candidates in the log, measured the same way:

| | SteamVR focus lost (Time Trial t 101-104, 4 s) | KartFrame VR settings panel over the Grand Prix (t 135-140, 6 s) |
| --- | --- | --- |
| OpenXR session state | VISIBLE (SteamVR dashboard/system UI) | FOCUSED throughout (chord X+Y, `VR settings opened with controller chord`, twice) |
| sets/s; late; repeats; missed | 90.5; 0; 0; 0 | **83.7; 30 (5.5 %); 40 (7.4 %); 2** |
| game fps est.; hitch frames | 59.95; 1 | **58.3; 4 a s (p50 23 ms; at t 138-140 the game thread waited 5.5 ms on the frame worker)** |
| sets without a previous frame | 0 | 5 (10 s window) |
| GPU p50 / p99; encode p50 / p99 | 9.46 / 11.25; 2.08 / 6.62 | 7.19 / 10.20; **4.80 / 11.11** |
| SteamVR | compositor GPU 0.8 -> 1.4 ms, 0 dropped, 0 mispresented | 1 dropped, 2 mispresented a second at each open |
| after | unchanged | t 141 on: 0 late, 0 repeats (the moment it closed) |

So SteamVR's dashboard cost us nothing measurable here (it took 0.6 ms of GPU and up to 2 CPUs for its overlay, and
the game kept 90 sets with 0 late); the slowdown the owner saw, gone the moment it closed, is the settings panel,
which is ours and fixable.

**Why the panel (and the menus) are slower.** With the panel open the presentation switches to the virtual screen,
and its sets take the menu branch of `run_retained_stereo_frame` (aurora-main/lib/aurora.cpp): that branch **keeps the
renderer lock for the whole set** (`hold.release(); gpuLock.reset();` happen only on the immersive branch), so the
narrow lock does not apply there; and with `KARTFRAME_MENU_CONTROLLERS=eyes` it calls `vrui::render` four times a set
(environment and screen composite, each eye), and each call transforms both controller models on the CPU (about
9000 triangles), sorts them and uploads a fresh vertex buffer (`aurora-main/lib/vr_ui.hpp`). Menus encode p50 at
90 Hz: 4.0-6.7 ms with eyes against 2.7-3.4 ms without it (sessions of 2026-10-27); p99 10.4-11.8 ms, at or over
the 11.1 ms refresh, hence the late sets; and the GX thread waits on that lock, hence the game's slower frames (menu
hitch frames here: 22 a second at 22-24 ms with only 4 ms of game CPU and 0.3 ms of run queue). The same costs give
every launch's menus their 4-6 % late sets (here 4.0 % and 6.1 %).

**Throttling while unfocused.** On focus loss the frame loop changes nothing: `shouldRender` stays true in VISIBLE,
the game keeps running (it does not pause by itself), full sets go out at 90 Hz, and input holds its last state for
1 s and then idles (`OpenXRInput::Sync`, `FocusHeld`). Rendering every other refresh (retained repeats) while
VISIBLE for more than 1 s would be easy and safe, but this session shows nothing to gain; only session 25's
minute-long dashboard stays had SteamVR skips (91 / 82). Low priority.

## 4. Anything else before 1.0.0

- Late/repeat bursts while driving: none (Grand Prix 2 late, 1 repeat in 95 s, both at t 171). Heavy-section dips:
  the Grand Prix's first 20 s (the pack together) had 1.5 hitch frames a second and the most weight-1 sets (41-59
  per 10 s), 59.6 fps; 3 frames of 33-34 ms in the whole race (t 192-212), none visible in the sets.
- Thermal: 4.1 min only; race mean 80-87 C, max 91.9 C, 95.0 C in the quit second; no CPU or GPU clock cap. A full
  three-race cup is still unmeasured (session 25-01, 4 min, peaked at 95.4 C without a cap).
- Warnings: the same start-up set as session 25 (Vulkan loader layer messages, the unsupported interaction
  profiles, NAND open of absent files); nothing new. One `audio output queue full ... dropping blocks` at the race
  load (a 110 ms load frame), as in 8 earlier launches. The race pose check's 128 mm / 8 deg maximum is the runtime
  recenter at t 33 (`cleared startup correction after runtime recenter`), not a fault.
- Transition seconds (race start, exits, quit) carry the run's few SteamVR reused/mispresented frames, as before.

## 5. Verdict and next

**1.0.0: ship.** Both races at 90 sets/s with 0-2 late sets, 60 fps (Time Trial) and 59.7 (Grand Prix) on a quiet
machine, no faults, no new warnings; the one slowdown is the settings panel, a menu-path cost, not the race.

Post-1.0.0, ranked:
1. The menu/panel set path: release the renderer lock before its encode as the immersive branch does (audit
   `vrui`/`stereo_overlay` state first), and build the controller geometry once (static vertex buffer in grip space,
   per-eye transform in the shader, one sort shared by the two passes of an eye). Fixes the owner's only slowdown
   and the menus' 4-6 % late sets. A/B: `KARTFRAME_MENU_CONTROLLERS=off` against eyes in one launch's menus.
2. The XR packet request out of the narrow lock (`request_stereo_frame`, 1.1-2.4 ms of hold a set in session 25):
   the Grand Prix's remaining 20-21 ms frames (game CPU 7 ms, run queue under 1 ms) are lock/GX waits.
3. The seal (0.9-1.0 ms a frame under the lock plus the GX thread's SEALED wait).
4. GX decode per draw, for 560+ draw sections (none reached here).
5. Render bundles for the per-set encode (3.3 ms p50 in the Grand Prix).
6. 120 Hz last: GPU per set is 8.6-9.5 ms p50 against about 7.1 ms of budget at 120 Hz, so it needs a 25-30 % GPU
   cut that 2-5 do not give; measure with `thermal.tsv` over a full cup.
7. Optional: half-rate sets while VISIBLE for more than 1 s.
For the next measured session: `KARTFRAME_CPULOG=1` in `session.env` if the GX/lock split is wanted (the defaults do
not log it), and a full cup for thermals.
