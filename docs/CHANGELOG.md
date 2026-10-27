# Changelog

## Unreleased

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
