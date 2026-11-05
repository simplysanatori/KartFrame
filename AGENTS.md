# KartFrame: notes for developers and AI agents

This file holds the technical detail moved out of the README.

## Where to look

- [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md): how the port works, the double-image investigation, settled facts about SteamVR on the Frame, and [building](docs/TECHNICAL_NOTES.md#building).
- [frame/DEV.md](frame/DEV.md): developer build steps and where the runtime looks for things.
- [frame/SETUP.md](frame/SETUP.md): the game kit, the first-run setup and the toolchain.
- [frame/UNTESTED.md](frame/UNTESTED.md): claims built but not yet confirmed in the headset.
- `frame/perf/`: per-session headset records.
- [CONTRIBUTING.md](CONTRIBUTING.md): contribution rules, including the rules for AI tools.
- [OPENXR.md](OPENXR.md): upstream's OpenXR VR design (Windows and Quest).
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md): every setting, test switch and file, for users.
- [docs/CHANGELOG.md](docs/CHANGELOG.md): version-by-version detail.
- [README.upstream.md](README.upstream.md): upstream's own README, kept untouched.

## What KartFrame is

KartFrame is a fork of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) (v1.3.1), the Mario Kart Wii VR port. The game in it is heurazy's static translation of the Wii game to C++, drawn by the aurora renderer on Dawn (WebGPU) and shown through OpenXR. KartFrame builds it as a Linux Arm64 program that runs in the Frame's own SteamVR. You do not need Lepton, an APK, adb or a PC.

## What this adds over Wiicompiled VR PLUS 1.3.1

Upstream runs on Windows and on the Meta Quest. On the Frame that would mean Lepton and an Android layer between the game and SteamVR. I changed the platform side and added a few things to the game:

- A native Linux Arm64 build on Dawn's Vulkan backend, bound to SteamVR's own Vulkan device (upstream's own Dawn patch).
- Frame pacing written for SteamVR on the Frame: both eyes on every refresh from that refresh's head pose, handed over only after the GPU has finished them. The game's own frame interpolation fills 90 Hz from its 60 frames a second.
- Both eyes of a refresh in one image, so the picture stays clean when you turn your head.
- The game's graphics work on its own CPU core, so Grand Prix holds 60 frames a second.
- The Frame controller layout in code, with no SteamVR binding file, and simplified Frame controller models converted at install from your own SteamVR files.
- Rumble on the Frame controllers.
- The controllers and the pointer in front of the menu screen, in 3D.
- The 2D menus and the race HUD on a flat quad layer in front of you.
- Refresh rate and eye size as settings, and a launch wrapper that applies SteamVR's per-app settings at every launch and logs CPU use and temperatures every second.
- A first run that extracts your own disc, translates the game and compiles it on the Frame with a bundled compiler, plus an installer with a Steam library entry, Mario Kart Wii library art and the KartFrame name.
- Features from Wiicompiled VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

## Performance internals

What the build does, so you know what the numbers in the [user guide](docs/USER_GUIDE.md#performance) mean:

- Each refresh locates the head once and renders both eyes from that pose, two refreshes ahead of the refresh that shows them.
- A set of eyes is handed to SteamVR only after the GPU has finished it, at a fixed point of the refresh against the compositor's own clock. If a set is not finished in time, the previous set is shown again and the miss is counted.

## Build from source and the patched kit

See the [technical notes](docs/TECHNICAL_NOTES.md#building). The patched Dawn is built once on the Frame, then the game and the release kit. `frame/tools/privacy-check.sh` checks the tree, the history and a release tarball for personal data before publishing, and `frame/boundary-check.sh` checks that nothing from the disc is in a release.

## Log collection: what is masked

The user-facing list is in the [user guide](docs/USER_GUIDE.md#reporting-a-problem). The script's header (`frame/collect-logs.sh`) has the exact list, which also covers UUIDs, tokens and certificate and key blocks. The archive goes to `$KARTFRAME_LOG_DIR`, or the home folder.

## Credits in full

- Built by [simplysanatori](https://github.com/simplysanatori).
- [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) by heurazy, which this is built on, and heurazy's [Mario Kart Wii VR port](https://github.com/heurazy/mario-kart-wii-VR-port) before it.
- [Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR) by iChris4, the OpenXR port of WiiCompiled that Wiicompiled VR PLUS started from, and the WiiCompiled translator that turns the Wii game into C++. The held item, the swing to throw, the 3D ghost, projected reflections, resident geometry and a shader fix in this release are ported from iChris4's fork.
- Library art from [SteamGridDB](https://www.steamgriddb.com/): AVEX (portrait), Sho. (wide) and MegaCrashFan96 (hero, an upscale of CaptainScarLeg's). Never shipped, downloaded at install; ids in `frame/art/CREDITS.md`. The logo font is Lilita One by Juan Montoreano (SIL OFL 1.1).
- [aurora](https://github.com/encounter/aurora), the renderer, and [Dawn](https://dawn.googlesource.com/dawn), the WebGPU implementation under it.
- [SDL3](https://github.com/libsdl-org/SDL) and the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK).
- [nod and nodtool](https://github.com/encounter/nod) by encounter, which extracts the disc.
- [LLVM](https://llvm.org/), the compiler bundled for the first run, and the [.NET runtime](https://github.com/dotnet/runtime), which the translator carries.
- Valve, for the Frame controller models. They are converted on your Frame from your own SteamVR files and never redistributed.
- [framecorder](https://github.com/simplysanatori/framecorder), my headset recorder, which recorded the panels the double-image work was measured on.
- bigmak94's [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) and the GalaxyFrame work, for what I learned about pacing for SteamVR and for the test tools (`xrmock`, and `xrprobe`, GPLv3).

[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) has the full list with licences.

## Licence detail

GPLv3, as upstream: see [LICENSE](LICENSE). Third-party parts keep their own licences: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/` folder. The test probe in `frame/tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

## How this was made, in full

I started this project and used AI (Claude Code with Fable 5.1, Opus 5.5 and Sonnet 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame.

I wanted Mario Kart Wii on my Frame without Lepton, so I started from Wiicompiled VR PLUS and built it natively for SteamVR: a Linux Arm64 build, Dawn's Vulkan backend on SteamVR's own device, pacing written for the Frame's compositor, the Frame controllers in code, and a first run that builds the game from your own disc on the Frame itself. The double image took the longest to fix; my records of that work are in `frame/perf/`.

Developer notes and what was learned about SteamVR on the Frame are in the [technical notes](docs/TECHNICAL_NOTES.md). The models and tools used are also listed in [its AI disclosure section](docs/TECHNICAL_NOTES.md#ai-disclosure).
