# Changelog

## Unreleased

- The product is called KartFrame everywhere the player sees it (runtime log lines, settings overlay, window and log titles, the installer); WiiCompiled stays only in credits and licence text. Library art: a KartFrame logo, dock icon and portrait, wide and hero images (`frame/art`, our own artwork, with an optional SteamGridDB key-art variant that is downloaded, never shipped); `frame/install.sh` installs them for the shortcut (`--art DIR` for your own, `--no-art` for none) and the release tarball carries them. UNTESTED: the art and the dock icon after a Steam restart.
- The frame worker's narrow renderer lock is the launcher default (`KARTFRAME_WORKER_LOCK=narrow`, set and logged by the wrapper; `full` in `session.env` goes back): the worker holds the lock only for a refresh set's setup and encodes the eyes outside it. Headset session 25 (two narrow launches, one full control, Grand Prix): 60.0 frames a second while driving with narrow against 53.9 (5 s minimum 42.3) with full, the graphics thread's lock wait 5.0 -> 1.7-2.6 ms a frame, 0 late sets, 0 faults, no visual fault; on the same paused frame 59.9 against 47.6 fps (frame/perf/session-h25/RESULTS.md). In the mock: lock wait -64 to -70 %, 0 validation messages, images within the noise. The `cpulog:` line ends with the renderer-lock waits by site and the worker's holds (frame/perf/LANE_AK.md). UNTESTED: narrow over long sessions.
- New launcher defaults from headset session 24: the GX thread on (`KARTFRAME_GX_THREAD=1`: game-thread CPU 13.2 to 8.5-8.8 ms a frame, Grand Prix from about 49 to 57-60 frames a second, 0 faults in three launches), 90 Hz (`KARTFRAME_REFRESH_HZ=90`; 120 Hz at 1728 still overruns the GPU, 66 frames dropped by SteamVR in one launch) and an interpolation window of 33 ms (`KARTFRAME_INTERP_MAX_MS=33`: sets without a previous frame 4.9 % to 0 %). Each is overridable in `session.env` (`0` or `off`, or a value) and logged in `launch.log`. Eye size stays 1728, packed swapchain and the end lock stay. New installs seed `refresh_rate = 90`; an older Config.toml keeps its 120 until changed. Grand Prix slowdown in heavy sections is improved, not gone; under investigation.
- Both eyes of a set now share one swapchain image by default (`KARTFRAME_STEREO_PACKED=1`, set by the launch wrapper; the game itself still defaults to off when run without it). In headset session 21 this was clean in a race with the end lock and also without it. `KARTFRAME_STEREO_PACKED=0` in `session.env` goes back to one swapchain per eye. Clean in one session each; the long A B A B comparison is pending.
- First person: the item you hold is drawn in your hand (right by default; `[vr] cockpit_item_hand = "left"|"right"|"off"`, `cockpit_item_size` in metres, default 0.2), ported from iChris4's Wiicompiled_VR fork; the models come from your own disc at run time. Mock-tested only; the hand offset was tuned on Quest controllers.
- The Time Trial ghost is drawn in 3D: each eye draws the ghost kart itself, from its own view, translucent and depth-tested against the course, where the game composited one flat picture of it into both eyes (seen doubled, one copy to each side). Adapted from iChris4/Wiicompiled_VR's composite-source fix for Quest (commits f2f7c50, e89988f). `KARTFRAME_GHOST_MONO=1` restores the flat ghost, `KARTFRAME_GHOST_ALPHA` (0.5) sets its strength. Not yet seen in the headset.
- First person: a quick swing of the item hand forward or back throws the held item that way (`[vr] cockpit_item_throw`, on by default; the same hand as the drawn item), and the game's GameCube rumble (bumps, hits) now reaches both controllers in the default gamepad mode (`KARTFRAME_RUMBLE=0` turns it off). Ported from iChris4/Wiicompiled_VR (throw); swing thresholds were tuned on Quest controllers.
- Menus: `KARTFRAME_MENU_CONTROLLERS=eyes` (with the packed default) draws the controllers and the ray in front of the menu screen in 3D, one screen quad per eye; `projected` draws the screen into the environment layer instead. Off by default until seen in the headset.
- The refresh rate (`[vr] refresh_rate` or `KARTFRAME_REFRESH_HZ`, 90 or 120) is now passed by the wrapper to SteamVR and to the game, and the eye size can be set per launch (`KARTFRAME_EYE_PX`, `KARTFRAME_RENDER_SCALE`); the game logs `view size:` at start. Defaults unchanged (1728 per eye, 120 Hz).
- Resident geometry (`KARTFRAME_RESIDENT_GEOMETRY=1`, off by default): display lists cached on the GPU and drawn through native vertex input (ported from iChris4/Wiicompiled_VR); about 2 % less GPU time a set in the mock. Vertex and index staging grew to 8 and 4 MB, and an overflowing draw is skipped and logged instead of aborting.
- `frame/tools/phase.py` no longer stops on XRF lines with another log line glued on after `t=`.
- Compacted-layout shaders bake the position-matrix index as the constant 0 instead of a uniform load with a dynamic index (ported from iChris4/Wiicompiled_VR 236665c, GPL-3.0).
- Projected reflections (Rainbow Road, Coconut Mall glass) no longer follow the head: texgen-only position matrices keep the recorded camera in eye replay (ported from iChris4/Wiicompiled_VR 4aac0ec, GPL-3.0).

## 0.9.0-alpha.1 (October 2026), pre-release

The first public build of the native port. Everything below has been played on a Steam Frame unless marked
otherwise. One known issue remains open, the double image in races (see the README); this build carries
the mitigation that removes it.

### Confirmed in the headset

- Runs natively on the Frame in SteamVR at 120 Hz, 1728 pixels per eye, both eyes rendered on every refresh: races hold 119-120 sets a second with under 1 % late.
- The game itself runs at its full 60 frames a second (the first session's "half speed" was a window-sync call sleeping 10 ms every game frame under gamescope; fixed).
- Menus and the intro screens as a flat quad layer in front of you, sharp and steady; the race HUD on the virtual screen with correct clipping; the thin line beside the map removed.
- The Frame controller layout, hand steering with the grips, the simplified Frame controller models in the hands.
- First run from your own disc in about 6 minutes: extract, translate, compile with the bundled compiler, link against the kit.
- The installer, the Steam library entry, the launch wrapper with SteamVR's per-app settings and the session logs.

### Rendering and pacing work

- Dawn's Vulkan backend on SteamVR's own device; eye images released only after a GPU fence (timeline semaphore under Dawn's lock).
- Pipelined pacing: each set of eyes rendered two refreshes ahead, every frame handed over at a fixed phase against SteamVR's compositor (the double-image mitigation, `KARTFRAME_END_PHASE`), released only when finished; retained repeat when late; half-rate never by default.
- An sRGB sink and a cut of the native render pass: eyes to swapchain 2.3 ms -> 0.47 ms.
- Turnip's sysmem mode by default (7-8 % faster for this draw-bound game).
- Thread priority (RealtimeKit) and a utilisation clamp so the game thread and the frame worker stay on the big cores.
- Input kept through SteamVR's short focus losses; refresh rate requested only when offered.

### Tools (for developers)

- A mock OpenXR runtime (`frame/tools/xrmock`) with exact checks: finished releases, pose stamps, end times, layer lists, per-eye probes, image dumps.
- `xrprobe --replay`: SteamVR shown the game's own recorded eye images through a minimal loop.
- Compositor timing per frame (`tools/vrtiming.py`), `frame/tools/phase.py`, panel recordings and their analysis.

### Known issues

- The double image in races: see the README. This build removes it by timing the hand-over; the right eye still runs one set ahead of the left, which reads as extra sensitivity to head movement.
- The Time Trial ghost is flat. Full-view 2D effects (Blooper ink) do not cover the whole view. The laser pointer's dot is not visible on the 2D menu screen.

## 1.0.0 (planned)

The items of the 1.0.0 plan that are not yet confirmed are listed in [frame/UNTESTED.md](../frame/UNTESTED.md).
