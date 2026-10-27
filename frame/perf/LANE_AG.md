# Lane AG: eye size and refresh rate, user-settable (2026-10-27)

Two questions: can the eyes go to SteamVR's 100 % and hold 120 Hz, and can the user switch between 90 and
120 Hz without trouble.

## Facts

From SteamVR's own logs (`vrcompositor.txt`, every compositor start, also before any app sets anything):

```
HMD driver recommended: 2160x2160 90.0Hz HiddenArea(15.64%) = 708 MP/sec
Raw ideal render target scale = 0.64          (GPU speed override on platform: 450)
New result of GetRecommendedRenderTargetSize for current app: 1728x1728
```

- **SteamVR's 100 % on the Frame is 2160 x 2160 per eye**, the panel's own resolution (the driver's
  recommendation). SteamVR's Auto resolution picks 64 % of those pixels on this GPU: 2160 x 0.8 = **1728**.
  The OpenXR runtime reports `recommendedImageRect` 1728 x 1728 (max 4096 x 4096; xrprobe and the mock both
  log it), and the wrapper pins the same 1728 per app (`resolutionOverride`). Earlier docs called 1728
  "SteamVR's 100 %"; that was Auto, not 100 %. Corrected in README, TECHNICAL_NOTES, DEV.md.
- Sizes in SteamVR's percent (of pixels): 1728 = 64 %, 1920 = 79 %, 2048 = 90 %, 2160 = 100 %.
  Pixels against 1728: 1920 x1.23, 2048 x1.40, 2160 x1.56.
- The game's eye = the runtime's recommended size x `[vr] render_scale` (clamped to the max). So 2160 is
  reachable two ways: `render_scale = 1.25` (the panel's "Resolution per eye" slider, or
  `KARTFRAME_RENDER_SCALE=1.25`), or `KARTFRAME_EYE_PX=2160` (SteamVR then recommends 2160).
- The display modes SteamVR switches between here: 72, 90, 120 Hz (4320x2160 output). With the per-app rate set,
  `XR_FB_display_refresh_rate` offers only that one rate (`display refresh rates: 120`).

## Budget (headset numbers, session 22)

On the headset at 1728 with packed eyes and the after lock, a race set's GPU time (the game's timestamps, which on
SteamVR include sharing the GPU with the compositor) is **7.2 to 7.4 ms p50**, and **8.0 to 8.4 ms** on one heavy
stretch of the beach course, where SteamVR skipped 12 to 33 frames a second (the owner's "slight jitter"). The
refresh is 8.33 ms. **The 120 Hz budget at 1728 is already used up on the headset.** The mock's 4.4 ms is not the
number to plan with.

Upper bound for bigger eyes: headset GPU x pixel ratio (the game is partly draw-bound, so the real growth may be
smaller; the mock matrix that would measure it was not run, see below).

| eye | pixels | 120 Hz (8.33 ms): p50 / heavy, pixel-scaled | 90 Hz (11.1 ms): same numbers |
| --- | --- | --- | --- |
| 1728 | 1.00 | 7.3 / 8.4 (at the limit today) | 7.3 / 8.4: 2.7 ms of room |
| 1920 | 1.23 | 9.0 / 10.4: does not fit | fits, heavy stretch 0.7 ms under |
| 2048 | 1.40 | 10.3 / 11.8: does not fit | p50 fits, heavy stretch over |
| 2160 | 1.56 | 11.4 / 13.1: does not fit | does not fit |


## What changed (settings, no code surgery)

- `frame/kartframe.sh`: the refresh rate is `KARTFRAME_REFRESH_HZ`, else `[vr] refresh_rate` of
  `UserData/Config.toml`, else 120; 90 or 120, anything else falls back to 120 with a log line. The wrapper sets
  it as SteamVR's per-app `preferredRefreshRate` (re-applied every launch, as before) **and exports it to the
  game**. Before, `[vr] refresh_rate = 90` in the config did nothing on SteamVR: the wrapper forced 120 per app,
  SteamVR then offers only 120, and the game requests a rate only when offered.
- `KARTFRAME_EYE_PX` (SteamVR's per-app `resolutionOverride`) accepts 1024 to 4096, else 1728 with a log line.
- `KARTFRAME_RENDER_SCALE`: one launch's `[vr] render_scale` (read in `openxr_integration.cpp`, same 0.25 to
  2.0 clamp as the config).
- The runtime logs `view size: recommended WxH (max ...), render_scale S -> eyes WxH` at every start.
- `frame/tools/openvr-stub.c` (the mock's fake compositor for the end lock) follows `MOCK_XR_HZ`; it was fixed
  at 120 Hz, so a 90 Hz mock with `KARTFRAME_END_PHASE` fell back to the fixed delay.
- `frame/test-installer.sh`: config rate to SteamVR and the game, env over config, bad values fall back.
- The mock already had `MOCK_XR_EYE` and `MOCK_XR_HZ`; nothing added there.

## 90 Hz: what follows the rate by itself (code read)

- Interpolation: `frame_interpolation_fps = 1` renders every display tick and weights by display time
  (`SceneClock`, time-based), so it follows 90. Leave it at 1: a fixed 90 or 120 cap there on the other rate
  gives uneven cadence.
- End lock: `vrtiming.py` measures the period from the compositor's frame times (no constant); the game takes
  the grid only when its period is within 1 % of `xrWaitFrame`'s, otherwise the 4 ms fixed delay. So a 90 Hz
  display gives a 90 Hz grid. Not run in the mock at 90 Hz (matrix stopped).
- Pacing: sets rendered `ahead_lead_` (2) refreshes ahead of the period `xrWaitFrame` reports; deadlines from
  that period; the 1 ms margin is absolute. The only 8 333 333 is a fallback when the runtime reports no period.
  At 90 Hz "two refreshes ahead" is 22 ms instead of 17 ms of pose prediction.
- `KARTFRAME_HALF_RATE=auto`'s window is 3600 sets (30 s at 120, 40 s at 90); opt-in only.
- `XR_FB_display_refresh_rate`: keep the per-app setting as the authority. SteamVR offers only the per-app rate,
  and in session 1 it offered only 90 while the per-app key ran 120, and asking for 90 switched the display. The
  game now asks for the same rate the wrapper set, which is a confirmation, never a change.
- 90 Hz shows each 60 fps game frame on 1.5 refreshes (interpolated) instead of 2: "90 Hz felt wrong for a 60 fps
  game" (owner). Offered as an option with that caveat; the default stays 120.

## Memory and copies for bigger eyes

- Packed swapchain (both eyes side by side, RGBA8, SteamVR gives 3 images a swapchain, 4 pairs at the moment):
  one image 2W x W x 4 bytes. 1728: 23.9 MB (12 images 287 MB); 1920: 29.5 MB (354 MB); 2048: 33.6 MB
  (403 MB); 2160: 37.3 MB (448 MB). Aurora's own per-eye targets (colour + depth, 8 bytes a pixel, both eyes)
  48 MB at 1728, 75 MB at 2160 per set of targets. Fine in 15 GB of shared memory; bandwidth is the cost.
- At 2160 the packed image is 4320 wide: wider than the runtime's `maxImageRectWidth` 4096. Each view's rect
  is 2160 (within the max), so it is legal OpenXR, but whether SteamVR accepts a 4320-wide swapchain is
  untested. Up to 2048 the packed image stays at 4096 or below. If 2160 fails to start in the headset,
  `KARTFRAME_STEREO_PACKED=0` is the fallback (with its own right-eye problem).
- The sRGB sink (eyes into the swapchain through an sRGB view, no MUTABLE_FORMAT, compressed): 0.47 ms a set at
  1728, scales with pixels, about 0.73 ms at 2160.

## Mock matrix

NOT MEASURED. The lane's build waited for the shared build lock, then the owner's usage budget ran out and
the orchestrator stopped the lane before any run. Planned, for whoever picks it up (scratch script, not committed):
`frame/perf/hudrace.sh NAME 95 MOCK_XR_EYE=<e>x<e> MOCK_XR_HZ=<hz> KARTFRAME_REFRESH_HZ=<hz> KARTFRAME_END_PHASE=after
KARTFRAME_STEREO_PACKED=1 MOCK_XR_DUMP_AT=999`, configs 1728/1920/2048/2160 @120 and 1728/2048/2160 @90, two
interleaved rounds, `frame/perf/pacing-parts.py` per run (sets/s, late %, GPU ms p50/p99, encode, game fps).
The code changes of this lane are also NOT BUILT yet (the build never got the lock): build and run
`frame/test-installer.sh` (passed for the wrapper part before the commit) and one mock run before merging.

## Recommendation

- Defaults stay 1728 per eye at 120 Hz. Bigger eyes at 120 Hz are not viable today: 1728 already uses the
  whole headset budget (7.3 ms p50, 8.0 to 8.4 ms on the heavy stretch, against 8.33 ms).
- At 90 Hz (11.1 ms), by the pixel-scaled headset numbers: 1920 fits (9.0 p50, 10.4 heavy), 2048 fits at p50
  (10.3) but not on the heavy stretch (11.8), 2160 does not fit (11.4 p50). So 90 Hz + `render_scale 1.111`
  (1920) is the one larger-eye combination worth a headset try; it trades 120 Hz smoothness, which the owner
  disliked at 90, for 23 % more pixels.
- Everything above is untested in the headset; the settings are there so the owner can try.
