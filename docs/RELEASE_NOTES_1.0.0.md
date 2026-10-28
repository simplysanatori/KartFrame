# KartFrame 1.0.0

Mario Kart Wii in VR, running natively on the Valve Steam Frame. KartFrame is my fork of heurazy's Wiicompiled VR PLUS (v1.3.1), built as a native Arm64 Linux program for SteamVR on the Frame. If you have been playing the Quest APK through Lepton, this is the same game without the Android layer underneath, and it runs better. You need your own PAL Mario Kart Wii disc image; no game data is included.

## The game on the Frame

- A native Arm64 Linux build that runs in the Frame's own SteamVR through OpenXR, on SteamVR's Vulkan device. No Android layer, no Lepton, no APK re-signing, no adb.
- 90 Hz at 1728 pixels per eye, filled by the game's own frame interpolation from its 60 frames a second.
- The game's graphics work runs on its own thread, and both eyes of a refresh go to SteamVR in one image.
- The Frame controller layout in code, with no SteamVR binding file.
- A first run that builds the game from your own disc on the Frame, and one Steam shortcut that starts it directly.

## What is better than running the APK through Lepton

- Under Lepton the Quest build showed its 60 game frames at 72 Hz with judder, or blurred at 120 Hz. The Android build leaves the game's frame interpolation out. Here the interpolation runs at 90 Hz, and the game holds a full 60 frames a second in Time Trial and Grand Prix.
- Grand Prix holds 60 because the game's graphics work runs on its own CPU core.
- Under Lepton the image ghosted and doubled when I turned my head. Here both eyes of a refresh go to SteamVR in one image, and the picture is clean.
- Menus ran at 37 to 45 frames a second under Lepton. Here they run at the display rate.
- Under Lepton anything above 1728 per eye brought late frames and stalls of 30 to 50 ms, so I played at 1382 to 1728. Here 1728 is the default, at 90 Hz.
- The chip does less work for the same game: native code, no Android system running underneath it. Battery life should be better for it.
- Nothing to work around at launch: no adb, no launcher taps, no re-signed APK or patched dex. It is one shortcut in Steam.
- The controls are made for the Frame controllers, so every button does what it should. Under Lepton I had whatever SteamVR's remap of the Vive-wand bindings allowed.

## Added

- Rumble on the Frame controllers. The game's bumps and hits shake both of them. `KARTFRAME_RUMBLE=0` turns it off.
- Refresh rate and eye size are settings: `[vr] refresh_rate` and `render_scale` in `Config.toml`.
- A log of CPU use and temperatures every second, in each session folder.
- Mario Kart Wii library art for the Steam entry, and the KartFrame name throughout.
- Features from Wiicompiled VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

## Known issues

- The VR menu over a running race slows the race until you close it.
- 120 Hz is there if you want to try it, but at 1728 per eye the GPU runs out of time and SteamVR skips frames.
- The heaviest parts of a few courses dip a little below 60.
- Anything else heavy running on the Frame while you play (a compile, a download) makes it stutter.

## Download

- `kartframe-1.0.0-linux-arm64.tar.gz`
- `kartframe-1.0.0-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, run `frame/install.sh` with the tarball as its argument, then `~/Games/KartFrame/app/setup.sh` with your disc image. Setup builds the game on the Frame in about 6 minutes. The [README](https://github.com/simplysanatori/KartFrame#install) has the steps, the controls and the settings, and the [changelog](https://github.com/simplysanatori/KartFrame/blob/native/docs/CHANGELOG.md) has this list.

## Credits

iChris4's [Wiicompiled_VR](https://github.com/iChris4/Wiicompiled_VR) for the item in your hand, swing to throw, the 3D ghost and the reflection fix; heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), which KartFrame is built on; library art from SteamGridDB by AVEX, Sho. and MegaCrashFan96 (an upscale of CaptainScarLeg's), ids in `frame/art/CREDITS.md`.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/KartFrame/app/tools/collect-logs.sh`. It masks addresses, names and ids before anything leaves your Frame.
