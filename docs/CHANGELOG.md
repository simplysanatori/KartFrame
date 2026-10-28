# Changelog

## 1.0.0 (October 2026)

The first release of KartFrame: Mario Kart Wii VR as a native Arm64 Linux program for the Valve Steam Frame. If you have been playing the Quest APK through Lepton, this is the same game without the Android layer underneath, and it runs better.

### The game on the Frame

- A native Arm64 Linux build that runs in the Frame's own SteamVR through OpenXR, on SteamVR's Vulkan device. No Android layer, no Lepton, no APK re-signing, no adb.
- 90 Hz at 1728 pixels per eye, filled by the game's own frame interpolation from its 60 frames a second.
- The game's graphics work runs on its own thread, and both eyes of a refresh go to SteamVR in one image.
- The Frame controller layout in code, with no SteamVR binding file.
- A first run that builds the game from your own disc on the Frame, and one Steam shortcut that starts it directly.

### What is better than running the APK through Lepton

- Under Lepton the Quest build showed its 60 game frames at 72 Hz with judder, or blurred at 120 Hz. The Android build leaves the game's frame interpolation out. Here the interpolation runs at 90 Hz, and the game holds a full 60 frames a second in Time Trial and Grand Prix.
- Grand Prix holds 60 because the game's graphics work runs on its own CPU core.
- Under Lepton the image ghosted and doubled when I turned my head. Here both eyes of a refresh go to SteamVR in one image, and the picture is clean.
- Menus ran at 37 to 45 frames a second under Lepton. Here they run at the display rate.
- Under Lepton anything above 1728 per eye brought late frames and stalls of 30 to 50 ms, so I played at 1382 to 1728. Here 1728 is the default, at 90 Hz.
- The chip does less work for the same game: native code, no Android system running underneath it. Battery life should be better for it.
- Nothing to work around at launch: no adb, no launcher taps, no re-signed APK or patched dex. It is one shortcut in Steam.
- The controls are made for the Frame controllers, so every button does what it should. Under Lepton I had whatever SteamVR's remap of the Vive-wand bindings allowed.

### Added

- Rumble on the Frame controllers. The game's bumps and hits shake both of them. `KARTFRAME_RUMBLE=0` turns it off.
- Refresh rate and eye size are settings: `[vr] refresh_rate` and `render_scale` in `Config.toml`.
- A log of CPU use and temperatures every second, in each session folder.
- Mario Kart Wii library art for the Steam entry, and the KartFrame name throughout.
- Features from Wiicompiled VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

### Known issues

- The VR menu over a running race slows the race until you close it.
- 120 Hz is there if you want to try it, but at 1728 per eye the GPU runs out of time and SteamVR skips frames.
- The heaviest parts of a few courses dip a little below 60.
- Anything else heavy running on the Frame while you play (a compile, a download) makes it stutter.

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
