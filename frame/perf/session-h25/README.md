# Headset session 25: the narrow worker lock

Session 24 made the GX thread, 90 Hz and the 33 ms interpolation window the defaults (game thread 13.2 -> 8.5-8.8 ms a
frame; Grand Prix 57-60 fps, still slowing in heavy sections). Lane AK (frame/perf/LANE_AK.md) found that in those
sections the GX thread spends 4-5.5 ms a frame blocked on the renderer mutex, because the frame worker holds it for the
whole of each refresh set's encode (about 3 ms on the headset, 90 sets a second). `KARTFRAME_WORKER_LOCK=narrow` makes the
worker hold it only for the set's setup. This session asks whether that helps on the headset and is clean.

| launch | file | setting | the question |
|---|---|---|---|
| 1 | `h25-01-narrow.env` | `KARTFRAME_WORKER_LOCK=narrow` | does the slowdown shrink, and is the picture clean? |
| 2 | `h25-02-full.env` | `KARTFRAME_WORKER_LOCK=full` | the control (today's default) |
| 3 | `h25-03-narrow.env` | `KARTFRAME_WORKER_LOCK=narrow` | a repeat, to tell the lock from run-to-run noise |

All three: Grand Prix at the defaults (GX thread on, 90 Hz, interpolation 33 ms, 1728 per eye, packed swapchain, end
lock after) with `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_FPSLOG=1 KARTFRAME_CPULOG=1
KARTFRAME_SESSION_ENV_ONCE=1`. No recording. The queue is `$KARTFRAME_HOME/session.queue/` (default
`~/.local/share/KartFrame/session.queue/`); the "KartFrame (dev)" shortcut takes one file per launch, in name order.
Please play **one full race**, the same cup and track for all three, turning your head now and then.

## What narrow does

A refresh set (90 a second) used to hold the renderer mutex while it took the XR packet, built the replay, filled the
interpolated uniforms, encoded both eyes, the panel composite and the sink, and submitted. Narrow holds it only for the
first three (about 0.55 ms in the mock, about 0.9 ms expected on the headset, instead of 1.8 ms and about 3 ms) and
encodes without it; GPU order is unchanged. Mock (Grand Prix, GX on, interleaved A/B): the GX thread's lock wait fell
64 % at 90 Hz and 70 % at 120 Hz (1.15 -> 0.41 and 1.50 -> 0.45 ms a frame), the game's overlay hook wait to about 0,
0 Vulkan validation messages, images within the noise floor. The mock already held 60 fps with the full lock, so the
gain can only show on the headset.

## Report per launch

- Full speed or slowdown, and roughly when in the race (start, a lap, the end); how bad.
- Stutter or jitter (all the time, or on a part of the course).
- Any visual fault: wrong textures, flicker, missing or misplaced geometry (the narrow lock lets the worker encode while
  the graphics thread records, so a race between them would show up as a one-frame glitch).
- Whether the app name and overlays now say KartFrame (this build also has the rename; the library art needs the installer's
  art step and a Steam restart, not part of this session).
- Overall: which launch played best.

## How the default will be chosen

If launches 1 and 3 are both at least as smooth as 2 (the cpulog fields below agree) and show no new fault, narrow
becomes the default. A slowdown that shrinks in both narrow launches counts for it; a fault in either keeps `full` and
the lock stays opt-in.

## Reading the logs afterwards

`~/.local/share/KartFrame/sessions/<launch>/` (the first line of `session.env` names the launch); `game.log` has
`frame worker lock: narrow|full (KARTFRAME_WORKER_LOCK)` once. The `cpulog:` line (one a second) now ends with
`| lock wait ms/frame (waits): gx fifo A (n) drawdone B (n) dl C raw D end E, overlay F (n), worker seal G prep H set I
newframe J | gx waits sealed K (drawdone L) done M | worker holds ms/frame: seal N prep O newframe P; sets Q/s, R ms
encode, S ms held (T%) [full|narrow]`. In short:
- **GX lock wait** = fifo + drawdone + dl (A + B + C): the GX thread blocked on the renderer mutex. Compare full with
  narrow in the same sections of the race; the mock went 1.15 -> 0.41, the headset had 4-5.5 under full.
- **overlay F**: the game thread's wait on the mutex at the present.
- **gx waits sealed / done**: the GX thread waiting for the worker. A small `done` (0.1-0.2) appears with narrow.
- **worker holds**: `S ms held (T%)` is the share of a set's encode under the lock: about 100 % for full, about 30 % for
  narrow. `sets Q/s` should stay 90.
- The game's wait at `GXDrawDone` is in the `GX thread:` lines; fps and `game ms/frame ... cpu` as in session 24
  (target under 11 ms). `thermal.tsv` for heat. Details: frame/perf/LANE_AK.md.

## If the owner stops early

To drop the rest of the queue: `rm ~/.local/share/KartFrame/session.queue/h25-*.env`. Also delete
`~/.local/share/KartFrame/session.env` if a launch was queued but never started. Without a queued file a launch uses the
defaults (full lock).

## Mock numbers for this build (2026-10-27, integration lane, 90 s scripted Grand Prix, 90 Hz, GX on, interp 33)

Full / narrow, one run each: game 59.9 / 59.9 fps, sets 90.5/s both, late 0.02 %, 0 mock errors, 0 fatal lines. Over 36 s
of race (cpulog): GX lock wait 1.10 -> 0.37 ms a frame (-66 %), game 60.0 fps both; the worker held 2.25 ms (100 % of the
set's encode) -> 0.67 ms (30 %). "frame worker lock: narrow" is in the log, and the process transcript's first line says
KartFrame.
