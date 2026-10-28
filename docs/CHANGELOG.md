# Changelog

## 1.0.0 (October 2026)

The first full release of KartFrame, Mario Kart Wii VR built as a native Arm64 Linux program for the Valve Steam Frame. Time Trial and Grand Prix run at the game's full 60 frames a second, at 1728 pixels per eye and 90 Hz with the game's own frame interpolation, and the right-eye double image is gone. Under Lepton the Quest APK had no interpolation until I rebuilt it, menus at 37 to 45 frames a second and stalls above 1728 per eye; the [release notes](RELEASE_NOTES_1.0.0.md) have the comparison.

### The game on the Frame

- A native Arm64 Linux build that runs in the Frame's own SteamVR through OpenXR, on SteamVR's Vulkan device. No Android layer, no Lepton, no APK re-signing, no adb.
- The Frame controller layout in code, with no SteamVR binding file.
- A first run that builds the game from your own disc on the Frame, and one Steam shortcut that starts it directly.

### Fixed

- The right-eye double image in races. I put both eyes of a refresh in one swapchain image, so SteamVR can no longer pair a right eye from one refresh with a left eye from another (`KARTFRAME_STEREO_PACKED=1`).
- Grand Prix speed. I moved the game's graphics work to its own thread (the GX thread) and made the frame worker hold the renderer lock only for a set's setup (`KARTFRAME_WORKER_LOCK=narrow`). Grand Prix holds 60 frames a second, where it ran at 48 to 54.
- In menus the controllers and the pointer ray are drawn in front of the menu screen, in 3D, not behind it.

### Added

- 90 Hz at 1728 per eye with the game's own frame interpolation, and a 33 ms interpolation window (`KARTFRAME_INTERP_MAX_MS`): one missed game frame glides instead of stepping.
- Rumble on the Frame controllers: the game's bumps and hits shake both of them (`KARTFRAME_RUMBLE`, strength 0.7, `0` turns it off).
- Refresh rate and eye size settings: `[vr] refresh_rate` (90 or 120) and `render_scale`, or `KARTFRAME_REFRESH_HZ`, `KARTFRAME_RENDER_SCALE` and `KARTFRAME_EYE_PX` for one launch. The wrapper writes the refresh rate into SteamVR's per-app settings at every launch.
- Logs: CPU use per second (`KARTFRAME_CPULOG=1`), `thermal.tsv` in each session folder, and a `machine` line in `report.txt` with other processes' CPU use. The launch log warns when compilers or other builds are running.
- Library art: Mario Kart Wii key art from SteamGridDB (downloaded at install, never shipped), a plain KartFrame logo and a dock icon. `--art DIR` installs your own images, `--no-art` skips them.
- The name KartFrame in the logs, the settings panel, the window title and the installer.
- Features from iChris4's Wiicompiled_VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

### Changed defaults

- 90 Hz, was 120. New installs get `refresh_rate = 90`; an older `Config.toml` keeps its 120 until you change it.
- GX thread on, narrow renderer lock, both eyes in one swapchain, 33 ms interpolation window.
- Controllers in front of the menu screen (`KARTFRAME_MENU_CONTROLLERS=eyes`).
- The runtime kit was rebuilt: after updating, run `setup.sh` again (about 6 minutes). Saves and settings stay where they are.

### Known issues

- Opening KartFrame's own settings panel over a running race slows the race (about 84 frames a second, some late frames) until you close it. The SteamVR dashboard costs nothing.
- 120 Hz at 1728 per eye needs about 1 ms more GPU time per refresh than the Frame has, so SteamVR skips frames in bursts. It stays a switch (`KARTFRAME_REFRESH_HZ=120`).
- The heaviest sections of a course dip a little, to about 58 frames a second.
- Builds or other heavy work on the Frame while you play make the game stutter.
- One swapchain per eye (`KARTFRAME_STEREO_PACKED=0`) is retired: it brings the right-eye double image back.
- Blooper ink and the black of a respawn do not cover the whole view. The pointer's dot does not show on the 2D menu screen; the ray does.

### Credits

- iChris4's [Wiicompiled_VR](https://github.com/iChris4/Wiicompiled_VR) for the features named above (GPL-3.0).
- heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), which KartFrame is a fork of.
- Library art from SteamGridDB: AVEX (portrait), Sho. (wide), MegaCrashFan96 (hero, an upscale of CaptainScarLeg's). Asset ids in `frame/art/CREDITS.md`.

## 0.9.0-alpha.1 (October 2026), pre-release

The first public build of the native port. Everything below has been played on a Steam Frame unless marked
otherwise. One known issue remains open, the double image in races (see the README); this build carries
the mitigation that removes it.

### What works

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
