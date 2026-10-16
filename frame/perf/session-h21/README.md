# Headset session 21 (ready, not run): the clean replay of session 13 with the game's GPU load added

Lane Y (frame/perf/LANE_Y.md): SteamVR's per-frame compositor GPU time is the same in the doubled and the clean
launches (0.82-0.86 ms p50, p95 0.90-0.91 in the main race) and on anomaly frames, so the compositor's passes are
not stretched by our work; whether their START is delayed by our set queued ahead of them is not in any SteamVR
field. What the clean probe of session 13 lacked against the doubling game is the game's 5-7 ms of GPU work a
refresh, submitted right after the frame's end on our queue. This session adds exactly that to the probe, then
the two mitigations the game could use (split submissions, a low queue priority), a GalaxyFrame-sized load and
the control.

## The cases (xrprobe --replay, session 13 case 2 "drivE" plus load; replay-case.sh cases 7-12)

| launch | label | xrprobe arguments | expected if the load is the cause |
| --- | --- | --- | --- |
| 1 | 1 LOAd5 | `--replay-gpu-ms 5` (one submission right after xrEndFrame: the game's shape) | doubled |
| 2 | 2 SPLIt8 | `--replay-gpu-ms 5 --replay-gpu-split 8` (8 submissions of 0.63 ms) | clean if the compositor gets the GPU between submissions |
| 3 | 3 LO-Pri | `--replay-gpu-ms 5 --queue-priority low` (VK_KHR_global_priority 128) | clean if priority decides the order |
| 4 | 4 LOAd2 | `--replay-gpu-ms 2` (GalaxyFrame-sized) | clean (GalaxyFrame is) |
| 5 | 5 drivE | none (session 13 case 2 again, control) | clean |
| 6 | 6 norEC | `--replay-gpu-ms 5`, no panel recording | as launch 1 unless framecorder's load is part of it |

All 45 s of visible time, `KARTFRAME_VRTIMING_FRAMES=1` (vrframes.log), panels recorded (`KARTFRAME_RECORD=raw`)
except launch 6. Launch 6 is there because framecorder records at 120 fps and costs 1.9 ms of GPU a frame in the
probe; in the game's races it is starved to 7-80 fps (3-34 ms a frame), i.e. the GPU is oversubscribed in every
recorded race launch so far (LANE_Y.md section 3). Load: the pace modes' heavy fragment pass, two eye-sized
(1728x1728) passes per submission into an offscreen image, calibrated at start and re-scaled every 60 frames to
the target; the probe's GPU sum per frame is then 5.0 ms (mock: p50 5.00 / 5.15 split, p95 5.04-5.36).

## Install and queue (the orchestrator)

```
cmake -S frame/tools/xrprobe -B ~/.cache/kf-xrprobe -G Ninja -DCMAKE_C_COMPILER=clang
flock ~/Games/KartFrame/.build.lock cmake --build ~/.cache/kf-xrprobe
cp frame/tools/vrtiming.py ~/Games/KartFrame/app/tools/   # v2 vrframes: + presubmit/postsubmit/totalrender GPU, compositor CPU, submit
frame/perf/session-h21/queue.sh ~/.cache/kf-xrprobe/xrprobe ~/.cache/kf-laneT/dump-h13rec
```

queue.sh copies the binary and replay-case.sh into `$KARTFRAME_HOME/h21/` and writes `h21-01-load5.env` ..
`h21-06-norec.env` into `$KARTFRAME_HOME/session.queue/` (as session 13's queue.sh: `KARTFRAME_EXE`, own session
folder, SteamVR's per-app settings, `KARTFRAME_SESSION_ENV_ONCE=1`). The recording of session 13 must still be at
`~/.cache/kf-laneT/dump-h13rec` (1.2 GB). vrtiming.py v2 only appends columns; phase.py and LANE_Y's reading
read v1 and v2.

Owner, each launch from the KartFrame shortcut, as in session 13: wait for the race image, look at a track-side
edge, turn the head slowly left and right for ten seconds, hold still for five; again with the left eye closed,
then the right. Per launch: doubled or not, which eye, and whether one eye feels "more sensitive to movement".

## Reading it

- `xrprobe.log`: `queue priority:` (offered, the family's priorities: Turnip 128, 256, 512, no realtime; launch 3:
  `device created with global priority 128`, or the refusal and the retry), `replay load:` (submissions,
  iterations, after/before the end), one `G` line a frame (n, sum of the pieces, span from the first start to the
  last end, gaps between pieces, submit time after xrEndFrame returned, CPU time of the submits, each piece's ms
  when split), `SUMMARY replay load` lines. SteamVR side: `vrframes.log` per compositor frame; LANE_Y's tables
  (comp GPU and the new app-GPU columns against the presents) per launch.
- Note: SteamVR's client submits on the same queue inside xrEndFrame / xrBeginFrame; in launch 3 its work in our
  process is low priority too, as it would be in the game.

## What the outcomes would mean

- **1 doubled, 5 clean**: our GPU load right after the end is the cause (the content, hand-over and phase were
  cleared in sessions 13 and 20). Then:
  - 2 clean: the compositor gets the GPU between our submissions (scheduler-level, no mid-job preemption). The
    game's cheap split is per eye (`KARTFRAME_SET_SUBMIT_SPLIT=2|3`, about 2.5-3 ms pieces, built, default off);
    if only 0.6 ms pieces help, render_impl's pass loop needs a submit hook (LANE_Y.md section 5).
  - 3 clean: priority decides; `KARTFRAME_QUEUE_PRIORITY=low` in the game (built, default off) is the first
    session-22 launch.
  - 4 clean and 1 doubled: a load threshold between 2 and 5 ms; the game cannot get under it (its set is 4-7 ms)
    without a split or a priority.
  - 6 clean while 1 doubled: the recorder's GPU load (oversubscription) is part of it; every recorded verdict so
    far is suspect and session 22 runs the game unrecorded.
- **1 clean** (and 6 clean): GPU load alone does not double this loop; what remains is the game's CPU side (its
  threads beside SteamVR's client and compositor on 5 CPUs) and Dawn's submission details (its own fences and
  semaphores around SteamVR's). Next: a probe case with CPU load (threads spinning at the game's priorities).
- **5 doubled**: the control is not clean today; the day's verdicts cannot separate the cases (repeat 1 and 5).

## Mock verification (lane Y)

```
XRPROBE=<binary> PROBE_MOCK=1 PROBE_SET=replay PROBE_REPLAY_CASES="7 8 9 10 11" PROBE_SECONDS=20 PROBE_OUT=<dir> \
  KF_REPLAY_DIR=~/.cache/kf-laneT/dump-h13rec frame/tools/xrprobe/run-probes.sh
python3 -I frame/perf/session-h13/replaycheck.py <dir>
```

The mock runtime (never SteamVR), 120 Hz, 1728 eyes, MOCK_XR_TRACE_DETAIL=1, MOCK_XR_BUSY_MS=1, 20 s a case:

| case | mock | frame order (trace) | sets | load GPU ms sum p50 / p95 | span / gaps p50 | submit after the end's return, us p50 / p95 | queue priority |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 LOAd5 | 0 errors, 4802 releases finished | wait, begin, locate +0, acquire L R, release L R, end [proj] 100 % | 2401 of 2401 = display-time set | 5.00 / 5.04 | 5.00 / 0 | 28 / 92 | default |
| 2 SPLIt8 | 0 errors | same, 100 % | 2401 of 2401 | 5.15 / 5.36 (8 x 0.64) | 5.20 / 0.056 | 124 / 355 (8 submits: 80 us CPU) | default |
| 3 LO-Pri | 0 errors | same, 100 % | 2402 of 2402 | 5.00 / 5.31 | 5.00 / 0 | 26 / 92 | 128, accepted |
| 4 LOAd2 | 0 errors | same, 100 % | 2401 of 2401 | 2.02 / 2.06 | 2.02 / 0 | 28 / 93 | default |
| 5 drivE | 0 errors | same, 100 % | 2401 of 2401 | (no load) | | | default |

Every case: end display time = predicted 2401 of 2401, poses from no locate (the recorded ones) and the recorded
fovs on every layer, steps of one set per refresh, fence wait before release p50 0.52-0.77 ms. The load starts
after the frame's xrEndFrame returned in every frame of cases 1-4 (submit_us > 0 in every G line); with
`--replay-gpu-before` (not a session case) it lands before the call (submit_us < 0: section below).
