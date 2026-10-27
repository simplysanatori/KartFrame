# Lane Y: test switches against the right-eye double image (verification)

Both switches are default off and only for headset A/B sessions (frame/perf/session-h21-probes/README.md).

## What they do

- `KARTFRAME_QUEUE_PRIORITY=low|medium|high|realtime` (runtime/src/vr/openxr_vulkan_win32.cpp): chains
  `VkDeviceQueueGlobalPriorityCreateInfoKHR` on the queue Dawn asks for (the one queue, which SteamVR's client
  also submits on inside our process) and enables VK_KHR_global_priority (or the EXT). If device creation is
  refused (high/realtime without CAP_SYS_NICE) it logs a warning and retries with Dawn's own request. Log lines at
  startup: `queue priority: KARTFRAME_QUEUE_PRIORITY <n>, chained on 1 queue(s) with <ext>` and then
  `queue priority: device created with global priority <n>` (or `device creation refused ... retrying`).
- `KARTFRAME_SET_SUBMIT_SPLIT=2|3` (aurora-main/lib/aurora.cpp, run_retained_stereo_frame): a set's GPU work
  as 2 (eye 0 | eye 1 + sink) or 3 (eye 0 | eye 1 | sink) queue submissions instead of one. Logs once
  `set submit split: N queue submissions a set` when the first immersive (race) set is encoded. It does not act
  on the virtual-screen (menu) path, so a menu-only run never prints that line.

## Mock numbers (mock runtime, real Turnip GPU, never SteamVR; 2026-10-27)

Build: lane/Y at 01e6447 (all 32 ctest tests pass). Runs: `frame/perf/run.sh NAME 100 MOCK_XR_SKIP=1 TU_DEBUG=sysmem
"KF_MOCKRUN_VR=hud_virtual_screen = true" ... MOCK_XR_BUSY_MS=1` with the scripted A presses of hudrace.sh, so the
demo reaches a real 1-player race; the rows are the race part (67 s) from `frame/perf/pacing-parts.py`. GPU is ms
per set (both eyes), p50 / p99 (the log's percentile is p99). Every run: mock 0 errors, 0 releases before the GPU
finished, session reached FOCUSED, 0 fatal lines.

| run | sets/s | late % | repeats % | GPU p50 / p99 | encode p50 / p99 | game fps |
| --- | --- | --- | --- | --- | --- | --- |
| default 1 | 120.4 | 0.01 | 0.09 | 5.39 / 7.43 | 2.31 / 3.89 | 54.8 |
| default 2 | 120.4 | 0.02 | 0.09 | 4.54 / 5.21 | 2.06 / 2.95 | 59.9 |
| default 3 | 120.5 | 0.01 | 0.09 | 4.63 / 5.33 | 2.12 / 2.99 | 59.7 |
| priority low 1 | 120.4 | 0.04 | 0.12 | 5.48 / 7.35 | 2.36 / 3.72 | 56.9 |
| priority low 2 | 120.4 | 0.02 | 0.10 | 4.65 / 6.53 | 2.00 / 2.97 | 59.9 |
| split 2 (b) | 120.5 | 0.01 | 0.09 | 3.76 / 4.64 | 2.17 / 3.28 | 59.9 |
| split 3 | 120.4 | 0.01 | 0.09 | 3.68 / 4.57 | 2.29 / 3.38 | 59.8 |
| split 3 (b) | 120.5 | 0.01 | 0.09 | 3.80 / 4.70 | 2.21 / 3.27 | 59.8 |

- Priority low: Turnip accepted it, `device created with global priority 128` (VK_KHR_global_priority, one queue),
  no refusal and no retry. Pacing is unchanged. What it does to SteamVR's compositor cannot be seen in the mock.
- Splitting: pacing unchanged (120.4-120.5 sets/s, late 0.01 %); the GPU time per set reads about 0.8 ms lower
  than the default of the same back-to-back batch (3.7-3.8 against 4.5-4.6). Not explained: the per-pass timestamps
  span the pieces, so either the default's first pass includes waiting, or the pieces overlap better; the wall time
  of a set was not measured, so do not read it as a saving yet.
- The first split 2 race (13.8 % late, GPU 7.2 / 18.0, 37 fps) ran while the owner's Moonlight launched on the Frame
  (the run starts mid-way); repeated in a clean batch it matched split 3. Treat that row as contaminated, not as a
  property of the switch. The default GPU p50 itself moved 4.5 to 5.4 between batches (other jobs on the GPU), so
  only back-to-back rows are comparable.
- Menu/title-only 60 s mock runs (no race) on the same build: default and low 0 errors; split lines absent as
  expected (virtual-screen path).
