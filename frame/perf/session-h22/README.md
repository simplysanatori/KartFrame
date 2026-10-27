# Headset session 22: packed against per-eye, A B A B over full races

Follow-up to session 21 (frame/perf/session-h21/RESULTS.md): packed (`KARTFRAME_STEREO_PACKED=1`, now the wrapper
default) was clean in one short race each, with and without the end lock. This session asks whether it stays clean
over full races, and whether per-eye (`KARTFRAME_STEREO_PACKED=0`) shows the right-eye double image or the
both-eye stutter in the same conditions. The end phase stays at its default (`after`) in all four; no recording.

Needs the dev build installed (`frame/install.sh --dev`); the four files are in `$KARTFRAME_HOME/session.queue/`
(default `~/.local/share/KartFrame/session.queue/`) and the KartFrame (dev) shortcut takes one per launch.

## Launches (queue order = name order, interleaved A B A B)

| launch | file | setting | why |
|---|---|---|---|
| 1 | `h22-01-packed.env` | `KARTFRAME_STEREO_PACKED=1` | the candidate default over a long race |
| 2 | `h22-02-pereye.env` | `KARTFRAME_STEREO_PACKED=0` | control: one swapchain per eye |
| 3 | `h22-03-packed.env` | `KARTFRAME_STEREO_PACKED=1` | repeat |
| 4 | `h22-04-pereye.env` | `KARTFRAME_STEREO_PACKED=0` | control repeat |

All four: `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_SESSION_ENV_ONCE=1`.

Routine for every launch: a FULL 3-lap Time Trial (2 minutes or more) on the water/beach course of your choice, the
same course for all four launches. Turn your head during the race; quit only after the race. Note the lap or moment of
any glitch.

## Report per launch

- Right-eye double image: yes or no (and when).
- Both-eye stutter ("every few seconds"): yes or no, and when (lap, moment).
- Sensitivity: is one eye more sensitive to head movement than the other.
- Latency feel: how immediate head turns feel, compared with the other launches.
- Packed launches only: a seam in the middle, blur or foveation artefacts, colour differences between the eyes.
- Overall: which launch was best, and whether the order mattered.

## Reading the logs afterwards

`~/.local/share/KartFrame/sessions/<launch>/`; the first line of `session.env` there names the launch.
- `game.log`: `stereo packed: on (3456x1728, one swapchain per set)` in launches 1 and 3, absent in 2 and 4;
  `end phase:` lines in all four. The wrapper's `launch.log` says `KARTFRAME_STEREO_PACKED=1` or `=0`.
- `python3 -I frame/tools/phase.py SESSION_DIR...` : view C (each end against the compositor's update window of its
  refresh: before / inside / after, SteamVR's presents 0x / 2x+ / mispresented, class changes, early update-start
  episodes) and D (the end lock's achieved phase). Count the early-update episodes per launch and match them to what
  was reported.
- `frame/perf/pacing-parts.py DIR` wants a run.sh-style directory: copy `game.log` into a scratch directory
  (under `~/.cache`) first. It gives sets/s, late %, repeats % and GPU/encode ms for the menus and the race
  separately.
- vrframes presented counts as in session-h21/RESULTS.md: the `vrframes.log` F lines' presents (0x, 2x+) and
  mispresented over the race, and where in the race they fall (race start, mid-race, the end of the race).
  Interpolation holds (XRS `w=1.000`) per launch as well.
- `report.txt` in the session folder for the usual summary.

## If the owner stops early

The queue is consumed one file per launch. To drop the rest:
`rm ~/.local/share/KartFrame/session.queue/h22-*.env` (and `~/.local/share/KartFrame/session.env` if a launch was
queued but not started). Without a queued file the launch uses the defaults (packed on).
