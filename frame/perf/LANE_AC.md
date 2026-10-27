# Lane AC: resident geometry and native vertex input (off by default)

Ported from the iChris4/Wiicompiled_VR fork (GPL-3.0, same aurora lineage): b5d9c19 (resident geometry, native
vertex input) and e4c98da (staging growth). The fork measured on a Quest 3 (Adreno 740 at 492 MHz, render_scale
0.8): both eyes 6.69 -> 4.57 ms at 338 draws, app GPU -24 %.

## Design

1. Staging growth (always on): the frame's vertex staging 3 -> 8 MB, index staging 2 -> 4 MB. A draw whose FIFO
   bytes or indices do not fit is skipped and logged (`Frame staging full`) instead of the process aborting
   silently; any other overflow logs every region's fill before the abort.
2. De-indexing (`deindex`): indexed vertex attributes are resolved on the CPU into flat, 4-byte-aligned records as
   the FIFO is decoded (command_processor.cpp `deindex_plan`), so the vertex shader reads one record instead of an
   index plus a dependent array fetch. The pipeline config follows through `populate_vertex_layout` (gx.hpp), which
   both share. Past 3/4 of the vertex staging a frame falls back to FIFO records for its remaining draws.
3. Geometry cache (`cache`): `GXCallDisplayList` goes through `process_display_list`. A list that only draws
   triangles of one vertex format with indexed attributes (perspective only, no native-wheel arrays) is resolved
   once into a persistent 32 MiB GPU buffer (gfx/geometry_buffer.*) and drawn from there with 32-bit absolute
   indices; a hit stages only its indices. Key: list address, size, record layout (format, array addresses,
   strides, offsets). Valid while the guest-RAM write tracking (gx_guest_write.h generations, 4 KiB granules)
   shows neither the list nor any array it indexes written; a list whose granule moved is re-digested (xxh3) before
   it is dropped. A list changed twice within 600 frames is left to the plain path ("volatile"). Retired blocks are
   reused only 4 frames later; uploads of an aborted frame are dropped; everything is cleared at race exit
   (`aurora_request_geometry_cache_clear` from the RaceScene exit hook).
4. Native vertex input (`native`): a cached list whose records map onto WebGPU vertex formats (no NBT normals,
   RGBA8 colours only, no lines/points) is converted once to little endian and drawn through vertex buffer 0 with
   real vertex attributes. Pipeline variant bit `ShaderConfig::nativeVertices` comes out of the padding.

Pipeline cache safety: with the switch off every shader config and every generated WGSL text is byte-identical to
before (populate_vertex_layout reproduces the old layout exactly; the shader's `le` change only applies to DIRECT
attributes with le set, which only de-indexed configs have), so the shipped initial_pipeline_cache.db and players'
caches stay valid and GXPipelineConfigVersion is unchanged. Every new variant differs in its config bytes (DIRECT
attributes at aligned offsets, the nativeVertices bit), which is the recipe key and the shader-module key; Dawn's
blob cache keys on the WGSL. A stored recipe with nativeVertices whose layout is invalid is rejected on load.

Differences from the fork: no model-visibility / particle-quad hooks (not in this tree); the shared draw tail keeps
this tree's fade / invisible-blend / quad-signature / HUD-depth logic; a cached draw merges only into a cached draw
of the same format, stride and vertex-input mode; a list without per-vertex matrix indices uses the current matrix
slot at draw time (the fork reused the slot recorded at resolve, wrong when the same list is drawn for another kart
under another current matrix); an INDEX16 index is always read big endian (the fork read it in the array's byte
order once arrays could be little endian); geometry_cache.* is self-contained (backend hooks, std::unordered_map)
so a runtime unit test can drive it.

## The switch

`KARTFRAME_RESIDENT_GEOMETRY=0|off` (default) `| deindex | cache | 1|on|native`, or `[video] resident_geometry` in
Config.toml (the environment wins); `KARTFRAME_GEOMETRY_CACHE_MB` / `[video] geometry_cache_mb` (default 32). Startup
line: `resident geometry: mode=native deindex=1 cache_mb=32 native_vertex_input=1 (KARTFRAME_RESIDENT_GEOMETRY=1)`.
Every 10 s while on: `Resident geometry (10 s, N frames), a frame: ... lists from the cache (... native ...), ...
resolved into it, in place ... unsupported / untracked / no room / rewritten by the game; entries, MiB resident,
retired / free blocks, volatile keys; staging high water ...; de-indexing stopped early on N frames, N draws skipped`.
Test only: `KARTFRAME_RESIDENT_GEOMETRY_TOGGLE=<start s>/<period s>` switches the mode off/on live (logged).

The first race with the switch on compiles the new pipeline variants (de-indexed and native configs); they then sit
in the user's pipeline cache. For a headset A/B, give the "on" side one warm-up launch first.

## How to verify

- `ctest` in build-linux: `mkw_geometry_cache_tests` (validation by generations and digest, volatility, retire
  delay, dropped uploads, no staging room, invalid records, full buffer, race-exit clear, buffer loss).
- Mock A/B: `bash ../laneAD/frame/tools/abcompare.sh --env-b KARTFRAME_RESIDENT_GEOMETRY=1 --rounds 2 . .`
  (lane AD's tool; a warm-up round first so the "on" pipelines are compiled).
- In the game log while on: lists from the cache ~190-260 a frame in the race, ~95 % of them native, 0 untracked,
  0 no room, ~1 a frame rewritten by the game (2 volatile keys), 0 skipped draws.

## Numbers (mock, real Turnip GPU, 1728 per eye, 120 Hz pipelined, 2026-10-27)

abcompare, 2 interleaved rounds (A B A B) after a warm-up round, same binary, race seconds outside the dump windows:

| metric | off | on (native) | delta |
| --- | --- | --- | --- |
| errors | 0 | 0 | 0 |
| sets/s | 119.85 | 119.88 | +0.04 |
| late % | 0.06 | 0.06 | 0 |
| repeats % | 0.45 | 0.45 | 0 |
| GPU ms per set p50 | 4.46 | 4.37 | -0.09 (-2 %) |
| GPU ms per set p95 | 5.03 | 4.97 | -0.06 |
| encode ms p50 | 1.95 | 2.10 | +0.15 |
| game fps | 59.11 | 58.99 | -0.12 |

The per-second `gpu_ms` p50 during the race sits 0.05-0.15 ms lower with the switch on in every second of both
rounds; nowhere near the fork's -32 %. On this GPU (Adreno 750 class at its normal clock, 1728 px eyes) the eye
passes are evidently not bound by the vertex shader's dependent fetches the way the Quest 3 at 492 MHz and
render_scale 0.8 was; the cost is elsewhere (fragment work, pass count). Lane AC's port is therefore worth little
GPU time here; the headset A/B decides whether it is worth keeping on.

xrmock's own interpolation check is unchanged: perspective draws per set taken from the frame pair 93.2 % (off) vs
93.3 % (on), 0 drawn with the recorded uniform, all pose stamps exact: cached draws keep their frame-interpolation
identity.

Images: cross-run pairs cannot prove identity in a race here: the game is not deterministic run to run (the CPU
karts and the player's kart sit in different places at the same "scene position"): off vs on 34 % of pixels off
by more than 2/255 (400 pairs), but on vs on (B1 against B2) 67 %, and off vs off matched only 8 pairs (1.8 %).

Same-scene check instead (one run, the race paused at 50 s, head still, the mode toggled live every 1.5 s with
KARTFRAME_RESIDENT_GEOMETRY_TOGGLE=53/1.5, every released eye image dumped for 5 s across at least three toggles,
1/8 scale): consecutive images differ by at most 0.15 % of pixels by more than 2/255 (median 0.002 %; 263 of 594
left-eye and 295 of 594 right-eye consecutive pairs bit-identical at that scale), with no step at any toggle; the
remaining differences are the paused menu's pulsing button. The cache served 130-285 lists a frame in that run
(over 90 % native). So the picture with resident geometry on matches the picture with it off, within what a 1/8
scale (linear-filtered) dump can show; a full-resolution pixel-exact comparison was not made.

Soak (switch on, 310 s: menus, the attract race and its exit, a whole Grand Prix race at Luigi Circuit driven with A
held to the result screen): 0 fatal / validation / staging messages, 0 draws skipped, de-indexing never stopped
early; the cache settled at 192 entries, 1.4 of 32 MiB, 2 volatile keys (about one list a frame rewritten by the
game, left to the plain path), 0 untracked, 0 no room, 85-266 lists a frame from the cache (over 95 % native);
staging high water 1.2 MB of vertices (race start), under 0.4 MB of indices. Session: 120.3 sets/s, 0.1 % late, 78
repeats. Game RSS grew 417 -> 630 MB over the first 150 s (loading) and stayed at 634-640 MB for the last 150 s.
Eye dumps at 100, 200 and 290 s show the race and the result screen with no torn or misplaced geometry.

## Untested

- The headset: whether the small GPU saving shows there (SteamVR's compositor beside us; the mock runs above already
  used TU_DEBUG=sysmem through hudrace.sh, as the wrapper does), and whether it touches the right-eye double image.
- A course change in one session (race exit with a full cache, the next course at reused addresses): the attract
  race's exit was exercised, a second Grand Prix race was not. Other courses' lists (Retro tracks are not in this
  game) and multiplayer.
- `deindex` and `cache` alone were not measured (only off against native).
- Full-resolution pixel-exact identity (the same-scene check was at 1/8 scale).
- gx_fifo_tests (MKW_BUILD_GX_TESTS, off by default) were given the new stubs but not built.
