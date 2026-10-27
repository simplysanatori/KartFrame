# Headset session 23: Grand Prix at three eye sizes and at 90 Hz

The owner, after the Grand Prix in session 20261027-121256 (1728 per eye, 120 Hz, packed, end lock after): "terrible:
much doubling, jittery, slowdown". No dynamic resolution; "I just want it to work". This session looks for the
sharpest fixed configuration that plays a Grand Prix smoothly. It also gives the first headset look at the features
merged on 2026-10-27 (held item, throw, rumble, menu controllers, Time Trial ghost).

Needs the dev build installed (`frame/install.sh --dev`, done). The four files are in `$KARTFRAME_HOME/session.queue/`
(default `~/.local/share/KartFrame/session.queue/`). The KartFrame (dev) shortcut takes one per launch, in name order.

## What the logs of 20261027-121256 say (the session the owner called terrible)

The GPU was not the main problem there. The game was.
- Race, per second (pacing lines, the driving part t=108-226 s): sets 119.6/s, late 0.25 %, repeats 0.29 %. GPU per set
  p50 6.85 ms (median second; 6.5-7.3 for most seconds, max 12.5), p99 8.5 ms (median second). Encode p50 3.1 ms.
- **The game ran at about 43 frames a second, not 60.** The first-person anchor line (one every 60 game frames) came
  every 1.4-1.6 s instead of every 1.0 s, and almost every game frame was a `hitch:` (frame over 20 ms). The log caps
  those at 30 a second, and they hit the cap. Frame p50 24.9 ms, p90 30.3 ms. The game thread's own CPU time was
  14.5 ms a frame (p50; p90 16.1). The Time Trials of session 22 had 9-10 ms in their few hitches and ran 60 frames a
  second. So the Grand Prix (11 CPU karts) is a CPU overrun of the game thread as well as a GPU load. That is the
  "slowdown": the game ran at about 72 % speed.
- The interpolation broke with it. A game frame later than 1.5 game intervals (25 ms) is not "continuous", and the
  sets after it are shown without interpolation. `VR interpolation weights` counted 21-61 % of the race's sets (mostly 40-50 %)
  as "no previous frame" (Time Trial, session 22: 0). The scene then steps at an uneven 40-ish Hz: the "jitter" and,
  with head motion, the doubling.

So for each launch below, read **game frames per second** first (`KARTFRAME_FPSLOG=1` is on in all four), then the GPU.

| launch | file | setting | the question |
|---|---|---|---|
| 1 | `h23-01-gp-1440.env` | `KARTFRAME_EYE_PX=1440` (69 % of 1728's pixels) | does less GPU work give the game thread its 60 back? |
| 2 | `h23-02-gp-1536.env` | `KARTFRAME_EYE_PX=1536` (79 %) | the middle step, if 1440 is smooth |
| 3 | `h23-03-gp-1728-90hz.env` | `KARTFRAME_REFRESH_HZ=90` | full size, 25 % fewer sets to render and encode (the owner's accepted fallback) |
| 4 | `h23-04-gp-1440-split3.env` | `KARTFRAME_EYE_PX=1440 KARTFRAME_SET_SUBMIT_SPLIT=3` | 1440 with each set split into three GPU submissions so the compositor can run between them |

All four: `KARTFRAME_STEREO_PACKED=1 KARTFRAME_END_PHASE=after` (the defaults, written out), `KARTFRAME_MENU_CONTROLLERS=eyes`,
`KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_SESSION_ENV_ONCE=1 KARTFRAME_FPSLOG=1`. The new features
stay at their defaults: held item in the right hand, throw on, rumble on, ghost per eye. No recording. The wrapper
sets SteamVR's per-app size and rate on every launch, so launch 3's 90 Hz and launches 1, 2 and 4's sizes do not
carry over to later launches.

Routine for every launch (also in each file): Grand Prix, 50cc or 100cc, any cup, at least one full race; also look
at: the item in your hand, swing to throw, rumble on hits, the menu controllers in front of the screen, and a Time
Trial ghost if time allows.

## Report per launch

- Smooth or not. Doubling (which eye, when). Jitter (all the time, or on a part of the course). Slowdown (the game
  slower than normal).
- Sharpness: acceptable or not (1440 and 1536 are softer than 1728).
- Held item: in the hand, right size, right place on the Frame controller.
- Throw: does a quick swing forward or back throw the item that way. Too easy or too hard to trigger?
- Rumble: on bumps and hits, both controllers; too strong or too weak?
- Menus: are the controllers and the ray in front of the screen now, in 3D, the same in both eyes? Is the screen as
  sharp as before?
- Time Trial ghost (if tried): one ghost kart in 3D, not two flat copies.
- Overall: which launch played best.

## How the default will be chosen

The sharpest configuration the owner calls smooth becomes the default. By sharpness: 1728 at 90 Hz (launch 3), then
1536, then 1440 (launches 2, 1 and 4). So 1728 at 90 Hz wins if the owner calls it smooth (90 Hz felt wrong in early
sessions; the owner's verdict here decides). Otherwise the larger of 1536 and 1440 that is smooth wins. Split submission
(launch 4) stays on only if it plays better than launch 1. If no launch is smooth and the game still runs under 60
frames a second at 1440, the eye size is not the cause, and the game thread's CPU time is the next target.

## Reading the logs afterwards

`~/.local/share/KartFrame/sessions/<launch>/`. The first line of `session.env` there names the launch.
`launch.log` gives `refresh rate N Hz (KARTFRAME_REFRESH_HZ)` and `eye size asked of SteamVR: N`. `game.log` gives
`view size: recommended NxN ... -> eyes NxN` (check that SteamVR honoured the size) and `stereo packed: on (...)`.
- Game frames per second: `Game frame rate X FPS` every 5 s (FPSLOG), with the producer and worker split. Count the
  `hitch:` lines and their `game_cpu_ms`, and the `VR interpolation weights` "no previous frame" share in the race
  (0 is the goal).
- `frame/perf/pacing-parts.py DIR` wants a run.sh-style directory, so copy `game.log` into a scratch directory under
  `~/.cache` first. Read the race row: sets/s, late %, repeats %, GPU ms per set p50/p99, encode, game fps. The
  pacing lines' `gpu_ms` (per second) give the p50/p99 when there is no samples.txt.
- `python3 -I frame/tools/phase.py SESSION_DIR...`: view C (each end against the compositor's update window, SteamVR's
  presents 0x / 2x+ / mispresented, class changes, early-update episodes) and view D (the lock's achieved phase).
  At 90 Hz the grid must be 11.1 ms and locked (`end phase: locked to the compositor grid`), not the fixed delay.
- vrframes skips as in session-h22/RESULTS.md: compositor frames with 0 presents of ours and the frame before shown
  twice, per second of the race, against SteamVR's pre-submit GPU (v2 field). 1728 at 120 Hz skipped mostly where
  that was 7 ms or more.
- Features: `cockpit item: hand right, size 0.2 m` and `... parsed in X ms`; `item throw: on, right hand, ...`,
  `Held item thrown forward (controller, N cm in M ms)` or `Swing forward ignored: nothing to throw`;
  `gamepad rumble #n on, amplitude a`; `menu controllers: eyes (a screen quad per eye)`; `Time Trial ghost: pass N ...
  is drawn by each eye` or `... left flat (<reason>)`.
- `report.txt` in the session folder for the usual summary.

## Mock calibration (2026-10-27, after the merge; first person, Luigi Circuit scripted Grand Prix, 90 s)

MOCK_CALIBRATION

## If the owner stops early

The queue is consumed one file per launch. To drop the rest:
`rm ~/.local/share/KartFrame/session.queue/h23-*.env`. Also delete `~/.local/share/KartFrame/session.env` if a
launch was queued but never started. Without a queued file a launch uses the defaults (1728, 120 Hz, packed, end lock
after, menu controllers off).
