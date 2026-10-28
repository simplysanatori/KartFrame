# KartFrame 1.0.0

Mario Kart Wii in VR, running natively on the Valve Steam Frame. KartFrame is my fork of heurazy's Wiicompiled VR PLUS (v1.3.1) with a native Linux Arm64 build, Vulkan on SteamVR's own device, and frame pacing written for SteamVR on the Frame. No Lepton, no APK, no PC. You need your own PAL Mario Kart Wii disc image; no game data is included.

## Compared with the Lepton APK

Before KartFrame I played the upstream Quest APK through Lepton on my Frame. The Android build compiles the game's VR frame interpolation out, so it showed 60 frames a second with judder at 72 Hz and heavy blur at 120 Hz. I rebuilt it with interpolation at 90 Hz, which was very good but had small slowdowns. Its menus ran at 37 to 45 frames a second until I turned the menu shader off, and above 1728 per eye it had frequent late frames and 30 to 50 ms stalls, so I kept it at 1382 to 1728. Every launch needed adb and launcher workarounds, a re-signed APK and a patched dex, the controls were whatever SteamVR's remap of the Vive-wand bindings allowed, and the launch path through SteamVR broke easily.

KartFrame runs natively at 1728 per eye and 90 Hz with interpolation, with Grand Prix and Time Trial at the game's full 60 frames a second and no double image. The Frame controller layout is in the code, the game's rumble reaches the Frame controllers, and the controllers show in front of the menus. One Steam shortcut with its own art starts the game directly.

## What is new since 0.9.0-alpha.1

- **No double image.** I put both eyes of a refresh in one swapchain image, which ended the right-eye doubling in races.
- **Grand Prix at 60 frames a second** at 1728 per eye and 90 Hz: I moved the graphics work to its own thread and narrowed the renderer lock. It ran at 48 to 54 before.
- **Rumble on the Frame controllers**, and the controllers and pointer in front of the menu screen.
- **Settings:** refresh rate (90 or 120) and eye size, written into SteamVR's per-app settings by the wrapper at every launch, and a 33 ms interpolation window.
- **Logs:** CPU use and temperatures per second in each session folder, and a warning when builds are running.
- **Mario Kart Wii library art** for the Steam entry, and the game says KartFrame everywhere.
- Features from iChris4's Wiicompiled_VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

The full list is in the [changelog](https://github.com/simplysanatori/KartFrame/blob/native/docs/CHANGELOG.md), and the controls and settings are in the [README](https://github.com/simplysanatori/KartFrame#readme).

## Download

- `kartframe-1.0.0-linux-arm64.tar.gz`
- `kartframe-1.0.0-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, run `frame/install.sh` with the tarball as its argument, then `~/Games/KartFrame/app/setup.sh` with your disc image. The [README](https://github.com/simplysanatori/KartFrame#install) has the steps.

If you are updating from 0.9.0-alpha.1, run both again: I rebuilt the runtime kit, so the game is built again from your disc (about 6 minutes). Your saves and settings stay where they are, but an older `Config.toml` keeps `refresh_rate = 120` until you change it.

## Known issues

- Opening KartFrame's own settings panel over a running race slows the race (about 84 frames a second, some late frames) until you close it. The SteamVR dashboard costs nothing.
- 120 Hz at 1728 per eye needs about 1 ms more GPU time per refresh than the Frame has, so 90 Hz is the default.
- The heaviest sections of a course dip a little, to about 58 frames a second.
- Builds or other heavy work on the Frame while you play make the game stutter.
- Blooper ink and the black of a respawn do not cover the whole view. The pointer's dot does not show on the 2D menu screen; the ray does.

## Credits

iChris4's [Wiicompiled_VR](https://github.com/iChris4/Wiicompiled_VR) for the held item, the swing to throw, the 3D ghost and the reflection fix; heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), which KartFrame is built on; library art from SteamGridDB by AVEX, Sho. and MegaCrashFan96 (an upscale of CaptainScarLeg's), ids in `frame/art/CREDITS.md`.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/KartFrame/app/tools/collect-logs.sh`. It masks addresses, names and ids before anything leaves your Frame.
