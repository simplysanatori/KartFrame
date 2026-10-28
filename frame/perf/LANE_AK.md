# Lane AK: the renderer GPU mutex between the frame worker and the GX thread

Problem (session 24, Grand Prix at 1728, GX thread on): the GX thread spent 10.3-11.7 ms a frame inside its records
against 6.1-6.2 ms of CPU, and the game thread waited 3.4-4.9 ms a frame for it at `GXDrawDone`. Suspect: the frame
worker holds `g_rendererGpuMutex` through each refresh set's whole encode (`run_retained_stereo_frame`, 2.9-3.1 ms a
set, 90-120 sets a second), and the GX thread's FIFO decode takes the same mutex for every chunk.

## Timers (always on; printed with `KARTFRAME_CPULOG=1`)

Appended to the end of the `cpulog:` line (the fields before keep their places), per game frame unless noted:

`| lock wait ms/frame (waits): gx fifo A (n) drawdone B (n) dl C raw D end E, overlay F (n), worker seal G prep H set I
newframe J | gx waits sealed K (drawdone L) done M | worker holds ms/frame: seal N prep O newframe P; sets Q/s, R ms
encode, S ms held (T%) [full|narrow]`

- lock wait: time blocked acquiring the renderer mutex, by site (`TimedGpuLock`: one `try_lock` when free, two clock
  reads only when it has to wait). gx fifo = FIFO decode outside `GXDrawDone`, drawdone = the decode inside it (a
  thread_local flag set by `GXDrawDone`), dl = `GXCallDisplayList`, raw = raw bridge draws, end = aurora `end_frame`'s
  drain (all on the GX thread when it is on, the game thread otherwise); overlay = `aurora_imgui_host_frame_begin`
  (game thread, the present); worker seal/prep/set/newframe = the frame worker's own acquisitions.
- gx waits: the producer's waits for the worker's SEALED (the part inside `GXDrawDone` in brackets) and DONE phases.
- worker holds: how long the worker holds the mutex for the seal, the next frame's preparation and the ImGui restart
  (per frame), and per immersive refresh set: sets a second, the set's encode (lock taken to submit done) and the part
  of it under the mutex.

Code: `aurora-main/lib/aurora.cpp` (`GpuLockStats`, `TimedGpuLock`, `aurora_lock_stats_line`), `internal.hpp`,
`gx/command_processor.cpp`, `dolphin/gx/GXManage.cpp`, `runtime/src/cpu_monitor.cpp`.

## Lock map

| holder | thread | when | held for |
| --- | --- | --- | --- |
| FIFO decode `gx::fifo::process` | GX (game with GX off) | every FIFO chunk, every drain (also GXDrawDone's) | the decode |
| `process_display_list`, `submit_raw_draw` | GX | display lists, raw draws | the list |
| `end_frame` drain | GX | before the seal job is posted | the last decode |
| `aurora_imgui_host_frame_begin` | game | the present (overlay hook) | ImGui new frame |
| seal (`seal_frame_locked`) | worker | once a game frame | 0.45-0.67 ms a frame (mock) |
| prepare (`begin_frame_render_state_impl`) | worker | after the permit | 0.07-0.09 ms a frame |
| ImGui restart | worker | end of a cycle | ~0 |
| refresh set (`run_retained_stereo_frame`) | worker | 83-112 a second (mock, 90/120 Hz) | full: the whole set, 1.7-1.8 ms; narrow: 0.52-0.58 ms |

Other locks: `g_queueSubmitMutex` (every queue submit), the worker's state mutex, the pipeline cache's own mutex, and
Dawn's device mutex: the device is created with `ImplicitDeviceSynchronization`, and in the pinned Dawn's `dawn.json`
every CommandEncoder, RenderPassEncoder and Queue method and `CreateCommandEncoder` take it (CreateBuffer, CreateTexture
and CreateBindGroup synchronise internally). So two threads may record command encoders at once.

## The change: `KARTFRAME_WORKER_LOCK=narrow` (default `full`, the old behaviour)

An immersive refresh set holds the mutex only while it takes the XR packet (`request_stereo_frame`), builds the replay
(`interpolated_stereo_frame`), fills its interpolated uniforms (`prepare_late_stereo_replay`) and notes the scene
frames; it encodes both eyes, the panel composite and the sink, and submits (under `g_queueSubmitMutex`), without it.
The menu (virtual screen) path keeps the whole-set lock. Log line once: `frame worker lock: narrow|full
(KARTFRAME_WORKER_LOCK)`.

### Audit (what the unlocked part touches)

- `render_stereo_eye` -> `render_impl` / `render_eye_planned` / `render_pass_impl` / `gx::render`: the sealed passes
  (moved out of `g_renderPasses` at the seal); texture bind groups resolved to raw handles when the draw was recorded
  (`resolvedTextureBindGroup`), kept alive by the bind-group cache or `g_retiredBindGroups`, which only the seal clears
  and only the seal expires; pipelines through `wait_pipeline`/`try_pipeline` (own mutex); buffers and bind groups made
  at start-up (`g_uniformBuffer`, `g_indexBuffer`, `g_uniformBindGroup`, `g_staticBindGroup`,
  `g_emptyTextureBindGroup`); the geometry cache buffer (created once, before the first frame that uses it is sealed);
  `cockpit` / `cockpit_item` (own mutexes, replay only); `window_mask`; thread_local eye plans and eye-scene records.
  No `bind_group_ref`, `sampler_ref` or staging push (`push_uniform`, `map_*`) is reached from it: those are reached
  only from the decode and the seal.
- `stereo_overlay::composite_immersive`: the panel texture the worker's own encode prepared.
- `run_stereo_sink`: sink registration (own mutex), the worker's eye targets, the Vulkan sink (worker and XR thread,
  own synchronisation).
- GPU timing and the direct-eyes calibration (own mutex): worker only.
- GPU order is unchanged: the producer never submits (its staging copies go out on the worker's seal encoder, after
  the earlier sets), so only CPU-side concurrency changes.
- Precedent: `encode_sealed_frame` already encodes the same eyes, composite and sink without the mutex (and on the
  Quest, concurrently with the producer's drains, since SEALED is published before its encode there).

Not covered by the mutex either way (unchanged risks): a surface reconfigure on the producer during a set (only on a
window resize; not on the headset), and the mid-frame EFB flush (`aurora_flush_efb_copies_to_ram`, which bumps the
replay generation and does not hold the mutex across its end_batch and submit); Mario Kart Wii's runtime never calls
it.

## Mock A/B (Grand Prix, interleaved A B x3 per rate)

`hudrace.sh` Luigi Circuit, 110 s, GX thread on, 1728, packed, end phase after, `KARTFRAME_INTERP_MAX_MS=33`,
`MOCK_XR_BUSY_MS=1`, `MOCK_XR_HZ`/`KARTFRAME_REFRESH_HZ` 90 or 120; A = `full`, B = `narrow`. Race = 12 s after the
second race presentation to the end minus 3 s (61-69 s). ms are per game frame unless noted. "game wait on GX" is the
game thread's wait in its two `GXDrawDone` drains (`GX thread:` line); "GX lock wait" = fifo + drawdone + dl; "GX wall"
= GX thread busy / fps; GX CPU (schedstat) includes its idle spin, so wall minus CPU is not usable in the mock (it goes
negative). No other build ran during any run.

| run | lock | fps (5 s min) | game CPU | game wait on GX | GX wall | GX lock wait (fifo/dd/dl) | GX SEALED / DONE wait | overlay lock wait | set encode / held ms (%) | sets/s, late % | worker ms/s | errors |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 90-A1 | full | 60.0 (59.9) | 6.80 | 0.63 | 5.40 | 1.14 (0.78/0.16/0.20) | 0.26 / 0.00 | 0.40 | 1.75 / 1.75 (93) | 90.5, 0.00 | 204 | 0 |
| 90-B1 | narrow | 60.0 (59.9) | 6.69 | 0.57 | 4.84 | 0.42 (0.31/0.01/0.10) | 0.37 / 0.18 | 0.01 | 1.85 / 0.58 (29) | 90.6, 0.00 | 215 | 0 |
| 90-A2 | full | 60.0 (59.9) | 6.85 | 0.68 | 5.36 | 1.12 (0.78/0.10/0.24) | 0.31 / 0.00 | 0.37 | 1.72 / 1.72 (94) | 90.6, 0.00 | 203 | 0 |
| 90-B2 | narrow | 60.0 (59.9) | 6.86 | 0.71 | 5.03 | 0.37 (0.26/0.03/0.08) | 0.41 / 0.22 | 0.01 | 1.87 / 0.58 (29) | 90.6, 0.00 | 220 | 0 |
| 90-A3 | full | 60.0 (59.9) | 6.88 | 0.75 | 5.35 | 1.20 (0.80/0.20/0.20) | 0.31 / 0.01 | 0.37 | 1.79 / 1.79 (94) | 90.6, 0.00 | 209 | 0 |
| 90-B3 | narrow | 60.0 (59.9) | 6.69 | 0.44 | 4.52 | 0.43 (0.28/0.06/0.09) | 0.26 / 0.03 | 0.05 | 1.85 / 0.57 (29) | 90.7, 0.00 | 216 | 0 |
| 120-A1 | full | 60.0 (59.9) | 7.31 | 0.85 | 6.65 | 1.97 (1.47/0.15/0.35) | 0.41 / 0.01 | 0.72 | 1.80 / 1.80 (94) | 120.6, 0.00 | 265 | 0 |
| 120-B1 | narrow | 60.0 (59.9) | 6.93 | 0.79 | 5.44 | 0.69 (0.49/0.00/0.20) | 0.55 / 0.18 | 0.05 | 1.87 / 0.58 (29) | 120.6, 0.00 | 279 | 0 |
| 120-A2 | full | 60.0 (59.9) | 7.26 | 1.58 | 6.11 | 1.42 (0.56/0.77/0.09) | 0.34 / 0.03 | 0.01 | 1.75 / 1.75 (94) | 120.6, 0.00 | 257 | 0 |
| 120-B2 | narrow | 60.0 (59.9) | 6.99 | 0.45 | 4.66 | 0.32 (0.14/0.12/0.06) | 0.18 / 0.08 | 0.01 | 1.64 / 0.52 (30) | 120.5, 0.00 | 240 | 0 |
| 120-A3 | full | 60.0 (59.7) | 7.12 | 1.10 | 5.34 | 1.12 (0.39/0.68/0.05) | 0.20 / 0.02 | 0.01 | 1.68 / 1.68 (94) | 120.6, 0.00 | 244 | 0 |
| 120-B3 | narrow | 60.0 (59.9) | 7.22 | 0.27 | 4.64 | 0.34 (0.21/0.06/0.07) | 0.16 / 0.00 | 0.01 | 1.73 / 0.53 (29) | 120.7, 0.00 | 256 | 0 |

Means, full -> narrow:

| | 90 Hz | 120 Hz |
| --- | --- | --- |
| GX lock wait, ms/frame | 1.15 -> 0.41 (-64 %) | 1.50 -> 0.45 (-70 %) |
| GX wall in its records, ms/frame | 5.37 -> 4.80 (-0.57) | 6.03 -> 4.91 (-1.12) |
| game wait on the GX thread (GXDrawDone), ms/frame | 0.69 -> 0.57 | 1.18 -> 0.50 |
| overlay hook lock wait (game thread), ms/frame | 0.38 -> 0.02 | 0.25 -> 0.02 |
| GX DONE wait, ms/frame | 0.00 -> 0.14 | 0.02 -> 0.09 |
| set: under the lock / encode, ms | 1.75 / 1.75 -> 0.58 / 1.86 | 1.74 / 1.74 -> 0.54 / 1.75 |
| game CPU, ms/frame | 6.84 -> 6.75 | 7.23 -> 7.05 |
| game fps (5 s min) | 60.0 (59.9) both | 60.0 (59.7-59.9) both |

- The timers confirm the suspect in the mock: about 1.1-1.5 ms a frame of the GX thread's record time is spent blocked
  on the renderer mutex (decode chunks, the drain inside GXDrawDone, display lists), and the overlay hook on the game
  thread waits 0.4-0.7 ms on it when a set holds it. Narrow cuts the GX thread's lock wait by 64-70 %, its record wall
  by 0.6-1.1 ms a frame, and the game's wait at GXDrawDone by 0.1-0.7 ms; the overlay hook's wait goes to ~0.
- What remains under the lock is the set's prologue: 0.52-0.58 ms, almost all of it `prepare_late_stereo_replay`'s
  uniform interpolation. A small DONE wait appears (0.1-0.2 ms): the GX thread now reaches `end_frame` while a set is
  still encoding and waits for it (the producer must wait for DONE before a seal reuses the sealed frame).
- The mock already ran the game at 60 fps with the full lock, so it cannot show an fps gain; the headset (2.9-3.1 ms
  sets, 4-5.5 ms blocked a frame) is where it should. Scaled by the hold (3.0 -> about 0.9 ms a set), the headset's
  lock wait should fall by roughly two thirds.
- Set encode +0.1 ms at 90 Hz, +0.0 at 120 Hz (noise or Dawn's device mutex now shared with the GX thread's few Dawn
  calls); sets/s, late %, GPU per set unchanged.

## Correctness

- 0 errors in all 12 runs: mock 0 errors, 0 fatal lines, 0 GX-thread faults, every release checked finished
  (`MOCK_XR_BUSY_MS`), 0 `error` lines besides the mock summary.
- Images (`frame/tools/abcompare.sh --env KARTFRAME_GX_THREAD=1 --env KARTFRAME_STEREO_PACKED=1 --env
  KARTFRAME_INTERP_MAX_MS=33`, 120 Hz, kart standing, stamped dumps paired by scene position): full against narrow,
  120 pairs, 0.746 % of pixels off by more than 2/255 on average, worst pair 0.851 %, PSNR 33.2 / 32.4 dB. The tool's
  PASS bar (0.5 %) fails, but the noise floor, full against full with the same build, was worse: 77 pairs, 0.975 %
  mean, worst 1.838 %, PSNR 32.5 / 31.9 dB. The worst pair's differences are the distant AI karts and the start banner
  (moving content a game frame apart), nothing near the kart, HUD or edges. GPU ms p50 +0.09 (floor +0.07), late %,
  sets/s, encode and game fps unchanged.
- Vulkan validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, default settings incl. thread safety;
  chain `VK_LAYER_FROG_gamescope_wsi_aarch64 VK_LAYER_KHRONOS_validation`), narrow, 120 Hz, the same Grand Prix: 69 s of
  race, 0 validation messages, 0 mock errors, 0 fatal lines. (The layer makes a set's encode 8.3 ms, so its timing is
  not comparable.) The Linux build has no switch for Dawn's own front-end validation (`skip_validation` is always on in
  release; `webgpu/gpu.cpp` has an Android property only), so that was not run.

## Risks

- Same as the audit's unchanged risks. If a future change makes a producer-side path submit, or rebuild a buffer the
  eyes read (`g_uniformBuffer`, the geometry buffer, the index buffer) or the start-up bind groups during a race, the
  narrow set would race it; the full lock would not.
- The DONE wait shows the next limit: with the GX thread no longer blocked, `end_frame` meets an in-flight set more
  often.

## Next

- Headset: `KARTFRAME_WORKER_LOCK=narrow` against the default, 90 Hz, GX on, interp 33, Grand Prix, CPULOG + FPSLOG:
  the lock-wait fields and the game's wait at GXDrawDone in the 560-575-draw sections.
- Narrower still (not done): `prepare_late_stereo_replay` reads only the sealed frame and creates its own buffer; it
  could leave the lock too (the only thing it guards is the dead mid-frame flush's generation bump), leaving the
  request alone under it.
- Cheaper sets either way: pre-recorded render bundles per eye pass (lane AJ's idea).
