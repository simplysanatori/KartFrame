# Headset session 24: the GX thread, Grand Prix at 1728

Session 23 (owner: 90 Hz at 1728 the best, but "major slowdown" in all four) and its logs showed that the Grand Prix
game thread needs 13-14.5 ms of CPU a frame on the headset, so the game ran at 43-51 fps (about 70-85 % speed) and
17-49 % of the sets lost their previous frame for interpolation. The GPU was not the limit. Lane AJ (frame/perf/LANE_AJ.md)
found the cheapest lever: the **GX thread**.

## What the GX thread is

`KARTFRAME_GX_THREAD=1` (config `[video] gx_thread`) moves the host side of GX (FIFO parsing, aurora's GX calls, draw
recording) to its own thread. It is the Quest build's setting and was always off on Linux. In the mock Grand Prix it
takes 3.2-3.4 ms (30 %) off the game thread (11.1 -> 7.8 ms a frame, game 59 -> 60 fps, "no previous frame" 2 % -> 0).
On this branch it used to corrupt the FIFO decode and crash within a minute; the cause was the nameplate hook draining
aurora's FIFO from the game thread, fixed in lane AJ (those calls are posted to the GX thread). 5 of 5 mock races and
the integration race (7.65 ms a frame, 60.0 fps, 0 decode errors) are clean. The headset has not run it yet: this
session is the first look, so watch for **wrong textures or matrices, flicker, missing geometry, nameplates at the wrong
depth** that appear only with the thread on.

| launch | file | setting | the question |
|---|---|---|---|
| 1 | `h24-01-90hz-gx.env` | 90 Hz, `KARTFRAME_GX_THREAD=1` | does the game hold 60 fps and 100 % speed at 1728, 90 Hz? |
| 2 | `h24-02-90hz-nogx.env` | 90 Hz, GX thread off | the control (a repeat of session 23 launch 3) |
| 3 | `h24-03-120hz-gx.env` | 120 Hz (default), `KARTFRAME_GX_THREAD=1` | is 120 Hz at 1728 now smooth? |
| 4 | `h24-04-90hz-gx-interp33.env` | 90 Hz, GX thread on, `KARTFRAME_INTERP_MAX_MS=33` | if a launch still dips below 60: glide through a missed retrace instead of a step |

All four: Grand Prix at 1728 per eye (the default size), `KARTFRAME_STEREO_PACKED=1 KARTFRAME_END_PHASE=after` (the
defaults, written out), `KARTFRAME_MENU_CONTROLLERS=eyes`, `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1
KARTFRAME_FPSLOG=1 KARTFRAME_CPULOG=1 KARTFRAME_SESSION_ENV_ONCE=1`. No recording. Held item, throw and rumble stay at
their defaults. The queue is `$KARTFRAME_HOME/session.queue/` (default `~/.local/share/KartFrame/session.queue/`); the
KartFrame (dev) shortcut takes one file per launch, in name order. Needs `frame/install.sh --dev` (done).

Please play **one full race** (the same cup and track for all four; session 23's launches were 67-83 s partial races, too
short to show slow drift or heat), turning your head now and then.

## Report per launch

- Full speed or slowdown, and roughly when in the race (start, a lap, the end).
- Doubling (which eye, when) or jitter (all the time, or on a part of the course).
- Sharpness.
- With the GX thread on: any wrong texture, flicker, missing or misplaced geometry, nameplate (name above a kart)
  oddity: where and when.
- If noticed: the item in your hand, the throw, rumble, the menu controllers.
- Overall: which launch played best.

## How the default will be chosen

The sharpest and fastest combination the owner calls smooth. Size stays 1728. If the GX thread is clean at 120 Hz
(launch 3), the default becomes GX thread on at 120 Hz. Otherwise it becomes GX thread on at 90 Hz (launch 1, or launch
4 when the interpolation window plays better). If the GX thread shows artefacts, it stays off and the game thread's CPU
is the next target (frame/perf/LANE_AJ.md, "Per-set CPU").

## Mock numbers for this build (2026-10-27, integration lane; first person, Luigi Circuit scripted Grand Prix, 90 s, GX on)

Sets 120.6/s, late 0.01 %, repeats 0.10 %, GPU 5.11 / 8.05 ms p50 / p99 per set, encode p50 2.27 ms, game 60.0 fps
(min 5 s second 58.9), cpulog game thread 7.65 ms CPU a frame (p50 7.88, max 9.11), run queue 0.19 ms, 0 mock errors,
0 FIFO decode errors, "no previous frame" 0-4 per 10 s. Lane AJ, GX off: 10.9-11.2 ms a frame. Expect the headset's
13-14 ms to drop to about 9.5-10 ms with the thread on (inside the 60 fps zone: 11 ms or less held 60 in session 22/23).

## Reading the logs afterwards

`~/.local/share/KartFrame/sessions/<launch>/`; the first line of `session.env` names the launch. `launch.log` has
`refresh rate N Hz` and `eye size asked of SteamVR`; `game.log` has `GX thread started (16 MiB command ring)` (launches
1, 3, 4; absent in 2), `view size: recommended ...`, `VR interpolation: previous frame kept across gaps up to 33.0 ms`
(launch 4).
- **Game thread CPU and fps** (the main number): `cpulog:` lines, one a second. `fps` is the game's frame rate; `game
  ms/frame: ... cpu X` is the game thread's CPU time per frame (the target is under 11; the headset was 13-14.5 without the
  thread), `runq` its wait for a core. The line also holds the cores the game ran on, the busiest threads (game, gx,
  worker) and each allowed core's clock plus the hottest thermal zone. With the thread on, a `hitch:` line's `game_cpu_ms`
  is the GX thread's, so use cpulog for the game thread. The `GX thread:` lines (every few seconds) give the ring and
  drain waits.
- **Heat**: `thermal.tsv` in the session folder (the wrapper writes it once a second): time, hottest zone in C and its
  name, each CPU's clock in MHz (cpu0..7), the GPU clock. Line it up with the fps dips: a dip while the hottest zone is
  at 93 C or more and the CPU clocks drop (3.1 GHz -> under 2 GHz) is thermal throttling; a dip at 60-80 C is not.
  Session 23's mock showed 25 s dips at 93-97 C.
- `frame/perf/pacing-parts.py DIR` (a run.sh-style directory: copy `game.log` into a scratch directory under `~/.cache`
  first): the race row gives sets/s, late %, repeats %, GPU and encode ms, game fps.
- `python3 -I frame/tools/phase.py SESSION_DIR...`: the end phase against the compositor window (at 90 Hz the grid is
  11.1 ms and `end phase: locked to the compositor grid`).
- `VR interpolation weights`: the "no previous frame" share in the race should be near 0 with the thread on.
- `report.txt` in the session folder for the usual summary.

## If the owner stops early

To drop the rest of the queue: `rm ~/.local/share/KartFrame/session.queue/h24-*.env`. Also delete
`~/.local/share/KartFrame/session.env` if a launch was queued but never started. Without a queued file a launch uses the
defaults (1728, 120 Hz, packed, end lock after, GX thread off, menu controllers off).
