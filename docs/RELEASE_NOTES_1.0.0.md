# KartFrame 1.0.0

Mario Kart Wii in VR, running natively on the Valve Steam Frame. KartFrame is a fork of heurazy's Wiicompiled VR PLUS (v1.3.1) with a native Linux Arm64 build, Vulkan on SteamVR's own device, and frame pacing written for SteamVR on the Frame. No Lepton, no APK, no PC.

You need your own PAL Mario Kart Wii disc image. No game data is in this repository or its releases.

## What is new since 0.9.0-alpha.1

- **No double image.** Both eyes of a refresh share one swapchain image, which ended the right-eye doubling in races.
- **Grand Prix at 60 frames a second** at 1728 per eye and 90 Hz: the graphics work runs on its own thread and the renderer lock is narrow.
- **The Time Trial ghost in 3D**, one view per eye. Projected reflections stay on the track.
- **Controls:** the item you hold is drawn in your hand, a quick swing throws it forward or back, the game's rumble shakes both controllers, and the controllers and pointer show in front of the menu screen.
- **Defaults:** 90 Hz (120 Hz is a switch), GX thread on, 33 ms interpolation window.
- **Mario Kart Wii library art** for the Steam entry, and the game says KartFrame everywhere.

The full list, with the controls, is in the [README](https://github.com/simplysanatori/KartFrame#readme) and the changelog.

## Known issues

- The game slows down while the SteamVR dashboard is open over it.
- 120 Hz at 1728 per eye overruns the GPU, so 90 Hz is the default. Anything else using the Frame's CPUs while you play (compilers, container builds) makes it stutter.
- The heaviest sections of a course dip to about 58 frames a second.
- Full-view 2D effects (Blooper ink) do not cover the whole view; the laser pointer's dot is not visible on the 2D menu screen; two and four player races are not measured.
- Built but not yet confirmed in the headset: the held item, the swing to throw and the rumble. See `frame/UNTESTED.md`.

## Download

- `kartframe-1.0.0-linux-arm64.tar.gz`
- `kartframe-1.0.0-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, run `frame/install.sh` with the tarball as its argument, then `~/Games/KartFrame/app/setup.sh` with your disc image. The [README](https://github.com/simplysanatori/KartFrame#install) has the steps. If you are updating from 0.9.0-alpha.1, run both again: the kit changed, so the game is rebuilt (about 6 minutes). Your saves and settings stay where they are, but an older `Config.toml` keeps `refresh_rate = 120` until you change it.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/KartFrame/app/tools/collect-logs.sh`. It masks addresses, names and ids before anything leaves your Frame.
