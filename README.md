# KartFrame

Mario Kart Wii in VR, running natively on the Valve Steam Frame.

KartFrame is a fork of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), the Mario Kart Wii VR port. The game in it is heurazy's static translation of the Wii game to C++, drawn by the aurora renderer on Dawn (WebGPU) and shown through OpenXR. KartFrame builds it as a Linux Arm64 program that runs in the Frame's own SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Mario Kart Wii disc. No game data is in this repository or its releases.

What it does:

- Plays Mario Kart Wii in VR on the Steam Frame, in the Frame's SteamVR.
- Renders both eyes on every refresh of the 120 Hz display, from your head position at that refresh, at 1728 pixels per eye.
- Uses the Steam Frame controllers through SteamVR's own controller profile.
- Builds the game on your Frame from your own disc, once, in about 6 minutes.

All of that has been played on a Frame. One problem is still being worked on, the double image in races; this pre-release carries the mitigation that removes it. See [The double image](#the-double-image-known-issue).

## What this adds to Wiicompiled VR PLUS

Upstream runs on Windows and on the Meta Quest. On the Frame that would mean Lepton and an Android layer between the game and SteamVR. KartFrame changes the platform side and leaves the game as it is:

- A native Linux Arm64 build.
- Dawn's Vulkan backend bound to SteamVR's own Vulkan device, through upstream's own Dawn patch.
- Eye images handed to SteamVR only after the GPU has finished them.
- Both eyes rendered on every refresh from that refresh's head pose, with the game's own VR frame interpolation filling the display rate from the game's 60 frames a second.
- Pipelined frame pacing, with the older single stage mode kept as a switch.
- The Frame controller layout in code, with no SteamVR binding file.
- Simplified models of the Frame's controllers, converted at install from the Frame's own SteamVR files.
- The game's 2D screen shown as a flat quad layer in front of you.
- A first run that extracts your disc, translates the game and compiles it on the Frame with a bundled compiler, using a kit of prebuilt runtime parts.
- A launch wrapper that sets SteamVR's per-app settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) at every launch and writes a session log folder.

## Status

Version 0.9.0-alpha.1 is a pre-release. It plays, at 120 Hz and 1728 pixels per eye, through twenty headset sessions so far. Version numbers stay below 1.0.0 until the double image (next section) is fixed at its cause rather than worked around.

Confirmed in the headset: the native build in SteamVR; 119 to 120 sets of eyes a second in races with under 1 % late; the game at its full 60 frames a second; the menus as a steady flat screen; the race HUD on the virtual screen with correct clipping; the Frame controls, hand steering and the controller models; the first run from a disc; the installer, the library entry, the launch wrapper and the session logs.

Known issues: the Time Trial ghost is flat; full-view 2D effects such as Blooper ink do not cover the whole view; the laser pointer's dot is not visible on the 2D menu screen (the ray is); two and four player races are heavier and not yet measured in the headset. The short list of what is built but not yet confirmed is in [frame/UNTESTED.md](frame/UNTESTED.md).

## The double image (known issue)

In races, turning your head could show the world twice, one copy about two refreshes of head motion ahead of the other, alternating every refresh, worse in one eye. Menus were always clean, and so was GalaxyFrame on the same SteamVR.

Twenty sessions of measurement, each one eliminating something by a direct test, ended with this: SteamVR's compositor on the Frame takes each eye's most recently handed-over image at its own point inside the refresh, the right eye after the left. The game was handing frames over right where those points sit, so from one refresh to the next the two eyes did not always get the same set. A probe that showed SteamVR the game's own recorded race images was clean in every variation, and logging every compositor frame beside the game's own timeline showed the clean launches keeping one side of the pick-up every frame and the doubled ones flipping.

What this build does about it: the launch wrapper runs a small helper that reads SteamVR's compositor timing and publishes its grid, and the game hands every frame over at a fixed point against that grid, just after the compositor's update (`KARTFRAME_END_PHASE=after`, the default). The doubling is gone with that. What remains, and why this is a mitigation rather than a fix: the right eye runs one set ahead of the left, which reads as the right eye being a touch more sensitive to head movement, and the compositor shows a frame twice now and then. The probe hands its frames over at the same point as the old default and is clean, so the point itself is not the cause; the remaining difference is the 4 to 7 ms of GPU work the game submits right after each hand-over on the GPU the compositor also composes on. That is the next experiment. The records of every session are in `frame/perf/`, and the mechanism is written up in the [technical notes](docs/TECHNICAL_NOTES.md).

Session 21 added a second switch, now on by default: `KARTFRAME_STEREO_PACKED=1` puts both eyes of a set in one swapchain image, so the compositor can only ever show the two eyes of the same set. With it the race was clean in the headset together with the end lock, and also clean with the lock off, where the old right-eye doubling did not show. That is one session each; a longer A B A B comparison against one swapchain per eye is still to come, so treat it as promising, not settled.

If you want the old behaviour to compare, put `KARTFRAME_END_PHASE=off` and/or `KARTFRAME_STEREO_PACKED=0` in `session.env` for one launch.

## What has been done so far

The work since the first headset session, in the order it mattered:

- The game ran at 38 frames a second under gamescope: a window-sync call slept 10 ms every game frame. Fixed; the game runs at its full 60.
- The 2D menus moved from projected panels to a quad layer, as upstream does on PC, which ended the flicker and the 5 to 7 ms of GPU the menus cost.
- An sRGB sink and a cut of the native render pass: copying the eyes to the swapchain went from 2.3 ms to 0.47 ms, which is what lets races hold 120 Hz at 1728.
- Pipelined pacing with the sets of eyes rendered two refreshes ahead, every frame handed over at the same point of the refresh, each set released only after a GPU fence, a retained repeat when a set is late, half rate never by default.
- Turnip's sysmem mode by default: 7 to 8 % faster for this draw-bound game.
- Thread priority through RealtimeKit and a utilisation clamp, so the game thread and the frame worker stay on the big cores.
- The race HUD clipped to its frame on the virtual screen, the thin line beside the map removed, full-view fades only for untextured colour fades.
- Input kept through SteamVR's short focus losses; the refresh rate requested only when SteamVR offers it.
- The double-image work above, with the tools it needed: a mock OpenXR runtime with exact checks, a probe that replays recorded eye images, per-frame compositor timing, panel recordings and their analysis.

## Requirements

- A Steam Frame with SteamVR, and about 1 GB free for the toolchain and kit, plus the extracted disc.
- Your own clean PAL Mario Kart Wii disc image (RMCP01).
- A terminal on the Frame for the one time setup: SSH, or Konsole from desktop mode.
- Steam running on the Frame while you install, so the installer can add the library entry.

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/KartFrame.git
   cd KartFrame
   ```

2. Download `kartframe-0.9.0-alpha.1-linux-arm64.tar.gz` and its `.sha256` file from the [Releases](https://github.com/simplysanatori/KartFrame/releases) page. Check the download and install it:

   ```sh
   sha256sum -c kartframe-0.9.0-alpha.1-linux-arm64.tar.gz.sha256
   frame/install.sh ~/Downloads/kartframe-0.9.0-alpha.1-linux-arm64.tar.gz
   ```

   The installer unpacks the program into `~/Games/KartFrame/app`. It adds **KartFrame** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It converts the Frame's controller models from SteamVR's own files. The script talks to Steam through its local debugging port, which SteamOS has on by default.

3. Build the game from your disc, once:

   ```sh
   ~/Games/KartFrame/app/setup.sh /path/to/MarioKartWii.iso
   ```

   Setup checks that the disc is RMCP01, extracts it to `~/.local/share/KartFrame/game`, translates the game, compiles it with the bundled compiler and installs the finished program. It took about 6 minutes on my Frame. It can be run again after an interruption and picks up where it stopped.

4. Put the headset on and launch **KartFrame** from your Steam library.

To update, download the new tarball and run `frame/install.sh` again, then `setup.sh` again if it tells you the kit changed. Your game files, saves and settings stay where they are.

The installer changes nothing outside the folders listed under [Files](#files), the Steam shortcut and the controller models. To remove KartFrame, run `~/Games/KartFrame/app/tools/uninstall.sh`. It keeps your data folder unless you pass `--purge`.

## First run

The launch wrapper checks that the installed program was built for the installed kit. If it was not, it stops and writes the reason to the session log; run `setup.sh` again.

On the very first start the game creates its own settings file and a fresh Wii save area. The game makes the Wii licence in-game on first boot.

If the game cannot find the disc files, the setup did not finish. Run `setup.sh status` to see which step is missing.

## Controls

The game sees a Wii wheel and pad. The Frame controllers are mapped like this.

| Steam Frame | In the game |
|---|---|
| Right trigger, **A** | Accelerate |
| Left trigger | Reverse |
| **B** | Brake, and back in menus |
| **X**, left bumper | Use item |
| **Y** | Look back |
| Both grips, right bumper | Drift |
| Left D-pad | D-pad |
| Right **Menu** | Start |
| Left **View** | Back. Held for half a second, it opens the VR settings panel. Pressed while the panel is open, it closes it |

Hand steering: in the first-person cockpit the grips hold the wheel, and the right bumper drifts there. Elsewhere the grips drift.

## Settings

Hold left View for half a second. The VR settings panel opens. Everything on it is also in `~/.local/share/KartFrame/UserData/Config.toml`, which is seeded from `Config.frame.toml` on the first run. The keys that matter:

| Key | Default | What it does |
|---|---|---|
| `[vr] render_scale` | 1.0 | Eye size relative to the size SteamVR gives the game: 1728 per eye, which is SteamVR's own 100 % on the Frame and what the wrapper asks for. 1.111 is 1920, 1.185 is 2048, 1.25 is 2160 (the panel's own resolution). The panel's "Resolution per eye" slider writes it; it applies at the next launch. Above 1.0 costs GPU time, see Performance |
| `[vr] frame_interpolation_fps` | 1 | 1 renders at the headset's refresh rate, interpolated from the game's 60 frames a second |
| `[vr] frame_pacing` | `"auto"` | `auto`, `inline` or `pipelined`. Auto picks pipelined |
| `[vr] refresh_rate` | 120 | 120 or 90. The launch wrapper sets it as SteamVR's rate for this game at every launch, and the game follows. At 90 Hz each of the game's 60 frames a second spans 1.5 refreshes instead of 2 (interpolated); that felt less smooth to me, so 120 is the default |
| `[vr] menu_shader_quality` | 0 | 0 is a plain dark background behind the menus |
| `[vr] hand_steering` | true | Grips hold the wheel in the first-person cockpit |
| `[vr] hud_virtual_screen` | true | Menus and the race HUD on a virtual screen |
| `[vr] hud_distance_meters` | 2.0 | How far away that screen is |
| `[vr] hud_width_meters` | 2.4 | How wide it is |
| `[video] resolution_multiplier` | 1.0 | Resolution of the flat desktop mirror |

I do not ship a default below 1728 per eye. Lower values of `render_scale` work, but the picture gets soft.

Switches for testing go in `~/.local/share/KartFrame/session.env`, one `NAME=value` per line, read at the next launch only:

| Name | What it does |
|---|---|
| `KARTFRAME_PACING` | `auto`, `inline` or `pipelined`, overriding the config |
| `KARTFRAME_REFRESH_HZ` | 90 or 120 for this launch, instead of `[vr] refresh_rate` |
| `KARTFRAME_EYE_PX` | The per-eye size the wrapper asks of SteamVR, 1024 to 4096 (default 1728, SteamVR's 100 % here); `render_scale` multiplies it |
| `KARTFRAME_RENDER_SCALE` | `render_scale` for this launch, e.g. 1.25 for 2160 per eye |
| `KARTFRAME_SYSMEM` | 0 turns off the GPU's direct memory rendering mode (`TU_DEBUG=sysmem`); it is on by default |
| `KARTFRAME_FDM_LAYER` | 0 disables Valve's foveation layer in the Vulkan chain, to compare |
| `KARTFRAME_END_PHASE` | `after` (default), `before`, or `off`: when in the refresh each frame is handed to SteamVR, against the compositor grid the wrapper's helper publishes. `off` hands over at the wake-up, which doubles the image |
| `KARTFRAME_STEREO_PACKED` | `1` (default) puts both eyes of a set in one 3456x1728 swapchain image, released once. `0` or `off` goes back to one swapchain per eye, to compare; that is the layout with the right-eye double image |
| `KARTFRAME_END_DELAY_US` | A fixed delay after the wake-up instead of the grid, in microseconds; also the fallback when the grid is missing (4000) |
| `KARTFRAME_VRTIMING_FRAMES` | 1 logs every compositor frame to `vrframes.log` in the session folder |
| `KARTFRAME_HOME` | The data folder, if it is not `~/.local/share/KartFrame` |

## Performance

Measured on my Frame, from the session logs:

| | |
|---|---|
| Display | 120 Hz, 1728 x 1728 per eye |
| Races | 119 to 120 sets of eyes a second, 0.2 to 1 % late, 2 to 3 % retained repeats |
| Menus | 110 to 113 sets a second (the quad layer and the intro screens) |
| GPU per set | about 5 ms in races (about 7 with the hand-over timing of this build, since the next set starts before the hand-over), 1.5 to 2.5 ms in menus |
| Game thread | 60 frames a second; about 8 % of race frames take 20 to 33 ms (the game's own course loading and item logic); a 144 ms hitch at race start |
| First run | about 6 minutes from the disc: extract 1 minute, translate 50 seconds, compile 5 minutes |

What the build does, so you know what the numbers mean:

- At 120 Hz there are about 8.3 ms per refresh, and about 7 ms of that is the GPU's for both eyes beside SteamVR's compositor.
- Each refresh locates the head once and renders both eyes from that pose, two refreshes ahead of the refresh that shows them.
- A set of eyes is handed to SteamVR only after the GPU has finished it, at a fixed point of the refresh against the compositor's own clock. If a set is not finished in time, the previous set is shown again and the miss is counted.
- Every second, the game logs one `vr pacing:` line with the refresh rate, the sets rendered, interpolated, late and missed, and GPU and encode times; every 10 seconds an `end phase:` line with the achieved hand-over phase. `report.txt` in each session folder sums them up, and `tools/session-report.py` lays several sessions side by side.

## Files

| What | Where |
|---|---|
| Program, wrapper, kit, toolchain, translator | `~/Games/KartFrame/app` |
| Game files, built game, saves, settings | `~/.local/share/KartFrame` (`game/`, `build/`, `bin/`, `UserData/`) |
| Controller models | `~/.local/share/KartFrame/UserData/controller_models/frame` |
| Session logs | `~/.local/share/KartFrame/sessions/<date-time>/` (`launch.log`, `stdout.log`, `game.log`, `vrtiming.log`, `report.txt`); `sessions/latest` points at the newest |
| Downloaded tools | `~/.cache/kartframe` (nodtool, fetched by `setup.sh`) |
| One launch's test settings | `~/.local/share/KartFrame/session.env` |

The game does not write to `~/.local/share/WiiCompiled`.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again.
- **The game does not start and `launch.log` says it refuses.** Read the reason in `~/.local/share/KartFrame/sessions/latest/launch.log`. If it names a kit mismatch, run `setup.sh` again.
- **`setup.sh` says the disc is not Mario Kart Wii RMCP01.** Only the clean PAL disc is supported.
- **SteamVR's per-app settings were not applied.** `launch.log` lists each one and what SteamVR holds after the write. They are only set when the game is started from the Steam library.
- **The world doubles when you turn your head in a race.** That is the known issue above. Check `launch.log` says `KARTFRAME_END_PHASE=after` and `game.log` has `end phase: locked to the compositor grid`; if it says it fell back to the fixed delay, the helper did not get SteamVR's timing (it needs SteamVR's `libopenvr_api.so`, which the wrapper finds under `/opt/steamvr`).
- **The right eye feels more sensitive to head movement than the left.** The known issue's remainder: the right eye runs one set ahead. Not fixed yet.
- **Motion is uneven.** `report.txt` shows late and repeated sets per second. A few percent is normal; more usually means something else is using the GPU (a recording, a stream).

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/KartFrame/issues) with what you did, what you expected and what happened, and the output of:

```sh
~/Games/KartFrame/app/tools/collect-logs.sh
```

It gathers the newest session's logs into one archive and masks IP and MAC addresses, host names, network names, serial numbers, Steam ids, email addresses and user names in paths. The masking is a best effort. Read the files before you attach them.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md). The patched Dawn is built once on the Frame, then the game and the release kit. `frame/tools/privacy-check.sh` checks the tree, the history and a release tarball for personal data before publishing, and `frame/boundary-check.sh` checks that nothing from the disc is in a release.

## Support

KartFrame exists because of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS). If you want to support someone, support them first: star and follow it, report what you find there, and send improvements to the game upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). It pays for the Claude API credits these ports are built with, spent as carefully as I can.

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori).
- [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) by heurazy, which this is built on, and heurazy's [Mario Kart Wii VR port](https://github.com/heurazy/mario-kart-wii-VR-port) before it.
- [Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR) by iChris4, the OpenXR port of WiiCompiled that Wiicompiled VR PLUS started from, and the WiiCompiled translator that turns the Wii game into C++.
- [aurora](https://github.com/encounter/aurora), the renderer, and [Dawn](https://dawn.googlesource.com/dawn), the WebGPU implementation under it.
- [SDL3](https://github.com/libsdl-org/SDL) and the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK).
- [nod and nodtool](https://github.com/encounter/nod) by encounter, which extracts the disc.
- [LLVM](https://llvm.org/), the compiler bundled for the first run, and the [.NET runtime](https://github.com/dotnet/runtime), which the translator carries.
- Valve, for the Frame controller models. They are converted on your Frame from your own SteamVR files and never redistributed.
- [framecorder](https://github.com/simplysanatori/framecorder), my headset recorder, which recorded the panels the double-image work was measured on.
- bigmak94's [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) and the GalaxyFrame work, for what I learned about pacing for SteamVR and for the test tools (`xrmock`, and `xrprobe`, GPLv3).

[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) has the full list with licences.

Mario Kart Wii, Nintendo and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve or the upstream author.

## Licence

GPLv3, as upstream: see [LICENSE](LICENSE). Third-party parts keep their own licences: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/` folder. The test probe in `frame/tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

Upstream's own README is kept as [README.upstream.md](README.upstream.md).

## How this was made

I wanted Mario Kart Wii on my Frame without Lepton, so I started from Wiicompiled VR PLUS and had it built natively for SteamVR: a Linux Arm64 build, Dawn's Vulkan backend on SteamVR's own device, pacing written for the Frame's compositor, the Frame controllers in code, and a first run that builds the game from your own disc on the Frame itself.

This is vibe coded, with manual review throughout. I wrote it with Claude Code, using Anthropic's Fable 5.1, Opus 5.5 and Sonnet 5.5 models running on the Frame itself, in parallel lanes with a mock OpenXR runtime so that almost everything could be proven without putting the headset on. The double image took twenty headset sessions of about a minute each, every one of them a test of one thing, and the records of those are in `frame/perf/`.

Developer notes and what was learned about SteamVR on the Frame are in the [technical notes](docs/TECHNICAL_NOTES.md).
