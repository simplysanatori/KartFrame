# Headset session 21: the right-eye double image, game A/B (packed swapchain, queue priority, split submissions)

Where we are (frame/perf/LANE_X.md, LANE_Y.md, LANE_AA.md): the end phase alone does not decide the doubling
(the clean probe of session 13 ended at the same phase as the doubled game); SteamVR's compositor GPU time is the same
in doubled and clean launches. Three candidate causes remain, one switch each:
- **Pick-up pairing** (lane AA): the compositor takes each eye's latest released image, so a right eye from one set
  could be shown with a left eye from another. `KARTFRAME_STEREO_PACKED=1` makes both eyes one swapchain image per
  set, released once.
- **Our GPU work queued ahead of the compositor's** (lane Y): `KARTFRAME_QUEUE_PRIORITY=low` (the one queue gets a
  low global priority) and `KARTFRAME_SET_SUBMIT_SPLIT=3` (three small submissions a set instead of one 4-7 ms one).
- The end lock (`KARTFRAME_END_PHASE=after`, the wrapper default since session 20) is a symptom treatment with added
  latency; launch 3 asks whether packing makes it unnecessary.

Needs: the dev build installed (`frame/install.sh --dev`, app dir `~/Games/KartFrame/app-dev`) with this tree's
`frame/tools/vrtiming.py` in the app tools dir (install does it; it also publishes the grid for the end lock).
The probe plan that was session 21 before (xrprobe replay with synthetic GPU load) is
frame/perf/session-h21-probes/ (queue files `h21p-*`); it is not queued.

## Launches (queue order = name order)

| launch | file | setting | what it tests |
|---|---|---|---|
| 1 | `h21-01-control.env` | defaults (`KARTFRAME_END_PHASE=after`) | the control, as session 20 launch 1 |
| 2 | `h21-02-packed.env` | `KARTFRAME_STEREO_PACKED=1` | packing, end lock as default |
| 3 | `h21-03-packed-nolock.env` | `KARTFRAME_STEREO_PACKED=1 KARTFRAME_END_PHASE=off` | packing alone, frames handed over at the wake-up as before the lock: the ideal result, no added latency |
| 4 | `h21-04-packed-nofdm.env` | `KARTFRAME_STEREO_PACKED=1 DISABLE_VULKAN_FDM_INJECTION_LAYER=1` | packing without Valve's FDM layer; only informative if launch 2 showed a seam or foveation artefact |
| 5 | `h21-05-priority-low.env` | `KARTFRAME_QUEUE_PRIORITY=low` | queue priority |
| 6 | `h21-06-split3.env` | `KARTFRAME_SET_SUBMIT_SPLIT=3` | split submissions |

All six: `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_SESSION_ENV_ONCE=1`, and **no panel recording**
(`KARTFRAME_RECORD` unset: framecorder's GPU load is itself suspected). Queue: the six files are in
`$KARTFRAME_HOME/session.queue/` (default `~/.local/share/KartFrame/session.queue/`); the KartFrame (dev) shortcut takes
one per launch. Routine for every launch: menus, Time Trial on Luigi Circuit, head turns at the countdown and while
driving, hold still, one eye closed then the other, quit.

## What to look for (owner, per launch and per eye)

- Right eye doubled or clean; the left eye; did it change during the race (a beat, a burst, the water section).
- "More sensitive to movement" in one eye than the other (session 19 launch 2's right eye).
- Launch 2/3/4 (packed): a **seam in the middle** of the image (a vertical line or a smeared column where the eyes meet),
  **blur or foveation artefacts** (soft edges toward the periphery, a shifting sharp area), colour or brightness
  differences between the eyes, menus still correct (the screen is a quad from the left half).
- Launch 3 vs 2: if 3 is as clean as 2, the end lock is no longer needed and the latency stays at the old value; say
  if 3 feels more immediate (response to a head turn).
- Launch 4 vs 2: any change in the seam or foveation look, nothing else.
- Launch 5/6: doubled as in launch 1 or better; any new judder, hitching or tearing of the whole image (a whole-image
  problem would mean the compositor is now starved or late, not the pairing).
- Overall: which launch is best, which are bad, and whether the order of launches mattered (the control is first).

## Reading the logs afterwards

`~/.local/share/KartFrame/sessions/<launch>/` (`latest` is a link); the launch number is the first line of `session.env` there.
- `game.log`: each launch's switch is announced once at startup, the lines to check:
  `stereo packed: on (3456x1728, one swapchain per set)` (launches 2-4; absent in the others),
  `queue priority: KARTFRAME_QUEUE_PRIORITY low ...` then `queue priority: device created with global priority <n>`
  (launch 5; a `device creation refused ... retrying` means the switch did nothing),
  `set submit split: 3 queue submissions a set` (launch 6, printed when the first race set is encoded),
  `end phase:` (launches 1, 2, 4, 5, 6: `locked to the compositor grid` once near the start, then every 10 s the achieved
  phase; launch 3: no end phase lines, the wrapper logs `KARTFRAME_END_PHASE=off`). A `fixed delay` fallback line
  is logged with its reason.
- `python3 -I frame/tools/phase.py SESSION_DIR...` (race only; `--all` for the whole launch): view A SteamVR's own app
  timings, B the app's wake-up / end / release against the nearest compositor anchor, **C** every end against the update
  window of its refresh (before / inside / after, with SteamVR's presents: 0x, 2x+, mispresented; class changes between
  consecutive refreshes; re-phase episodes), **D** the game's `end phase:` lines. Without the lock (launch 3) C and D
  are about where the ends fall by themselves; in packed launches the XRF-based columns still apply (release per set).
- `python3 -I frame/perf/pacing-parts.py DIR` needs a run.sh-style directory (game.log with `vr pacing:` lines); for a
  headset launch copy `game.log` into a scratch directory first (the menus and the race are separated by
  "entered/exited RaceScene"): sets/s, late %, repeats %, GPU and encode ms per set.
- `vrframes.log` (KARTFRAME_VRTIMING_FRAMES=1): compositor frames with presents, mispresented, dropped and, in v2
  columns, pre/post-submit and total render GPU ms; compare p50 / p95 across launches (lane Y found them equal in
  doubled and clean launches, so a difference here would be new).
- `frame/tools/session-report.py` (report.txt in the session folder) for the usual summary.

## Expected from the mock (not the headset)

Packed: mock 0 errors, 0 releases before the GPU finished, 120 sets/s, late under 1 % (the mock cannot judge the
pick-up, its checks skip a shared-swapchain race layer: LANE_AA.md). Priority and split: pacing unchanged (LANE_Y.md).
Everything about the double image itself is only decidable in the headset.

## If the owner stops early

The queue is consumed one file per launch (moved to `session.env`, deleted at the launch's start by
`KARTFRAME_SESSION_ENV_ONCE`). To drop the rest: `rm ~/.local/share/KartFrame/session.queue/h21-*.env` (and
`rm ~/.local/share/KartFrame/session.env` if a launch was queued but not started). To keep going later nothing needs
doing; the next launch takes the next file. To run one specific launch next, copy its file to
`~/.local/share/KartFrame/session.env`. Without a queued file or session.env the launch uses the defaults.
