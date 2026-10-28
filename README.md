# KartFrame

Mario Kart Wii in VR, running natively on the Valve Steam Frame.

KartFrame is a fork of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) (v1.3.1), the Mario Kart Wii VR port. The game in it is heurazy's static translation of the Wii game to C++, drawn by the aurora renderer on Dawn (WebGPU) and shown through OpenXR. KartFrame builds it as a Linux Arm64 program that runs in the Frame's own SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Mario Kart Wii disc. No game data is in this repository or its releases.

## Status

Version 1.0.0. It plays on my Steam Frame at 1728 pixels per eye and 90 Hz, with both eyes rendered on every refresh from your head position at that refresh, and the game at its full 60 frames a second in Time Trial and Grand Prix, with no doubled image. Menus sit on a steady flat screen with the controllers and the pointer in front of it, and the race HUD on a virtual screen.

**Compared with the Lepton APK.** Before KartFrame I played the upstream Quest APK through Lepton. The Android build has the game's frame interpolation compiled out, so it judders at 72 Hz and blurs at 120 Hz; my rebuild with interpolation at 90 Hz was very good but had small slowdowns. Its menus ran at 37 to 45 frames a second with the menu shader on, and above 1728 per eye it had late frames and 30 to 50 ms stalls. Every launch needed adb workarounds, a re-signed APK and a patched dex, and the controls were limited to SteamVR's remap of the Vive-wand bindings. KartFrame runs natively at 1728 per eye and 90 Hz with interpolation, Grand Prix and Time Trial at a full 60 frames a second, the Frame controller layout in code, rumble on the Frame controllers, and one Steam shortcut that starts the game directly.

Known issues:

- Opening KartFrame's own settings panel over a running race slows the race (about 84 frames a second, some late frames) until you close it. The SteamVR dashboard costs nothing.
- 120 Hz at 1728 per eye needs about 1 ms more GPU time per refresh than the Frame has, so 90 Hz is the default and 120 Hz is a switch.
- The heaviest sections of a course dip a little, to about 58 frames a second.
- Anything else using the Frame's CPUs while you play (a compiler, a container build) makes the game stutter.
- One swapchain per eye (`KARTFRAME_STEREO_PACKED=0`) is retired: it brings the right-eye double image back. The history of that double image is in the [technical notes](docs/TECHNICAL_NOTES.md).
- Blooper ink and the black of a respawn do not cover the whole view. The pointer's dot does not show on the 2D menu screen; the ray does.

## What this adds over Wiicompiled VR PLUS 1.3.1

Upstream runs on Windows and on the Meta Quest. On the Frame that would mean Lepton and an Android layer between the game and SteamVR. I changed the platform side and added a few things to the game:

- A native Linux Arm64 build on Dawn's Vulkan backend, bound to SteamVR's own Vulkan device (upstream's own Dawn patch).
- Frame pacing written for SteamVR on the Frame: both eyes on every refresh from that refresh's head pose, eye images handed over only after the GPU has finished them, at a fixed point of the refresh. The game's own VR frame interpolation fills 90 Hz from its 60 frames a second.
- Both eyes of a refresh in one swapchain image, which ended the right-eye double image.
- The game's graphics work on its own thread (upstream's GX thread, switched on here and fixed for the nameplate hook) and a narrow renderer lock: Grand Prix at 60 frames a second where it was 48 to 54.
- The game's rumble on the Frame controllers.
- The controllers and the pointer in front of the menu screen, in 3D.
- The Frame controller layout in code, with no SteamVR binding file, and simplified Frame controller models converted at install from your own SteamVR files.
- The 2D menus and the race HUD on a flat quad layer in front of you.
- A first run that extracts your own disc, translates the game and compiles it on the Frame with a bundled compiler, plus an installer with a Steam library entry.
- Refresh rate and eye size settings, applied through SteamVR's per-app settings, which the launch wrapper sets at every launch. Each launch writes a session log folder with CPU, thermal and pacing numbers.
- The KartFrame name in the game, and Mario Kart Wii library art for the Steam entry.
- Features from iChris4's Wiicompiled_VR are in too: the item in your hand, swing to throw, the 3D ghost, the reflection fix.

## What changed since 0.9.0-alpha.1

- Fixed: the right-eye double image (packed stereo swapchain), the Grand Prix slowdown (GX thread, narrow lock), controllers hidden behind the menu screen.
- Added: rumble on the Frame controllers, refresh rate and eye size settings, CPU and thermal logging, KartFrame library art and name, and iChris4's held item, swing to throw, 3D ghost and reflection fix.
- Defaults changed: 90 Hz (was 120), GX thread on, narrow lock, packed swapchain, 33 ms interpolation window, menu controllers in front of the screen. The full list is the [changelog](docs/CHANGELOG.md).

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

2. Download `kartframe-1.0.0-linux-arm64.tar.gz` and its `.sha256` file from the [Releases](https://github.com/simplysanatori/KartFrame/releases) page. Check the download and install it:

   ```sh
   sha256sum -c kartframe-1.0.0-linux-arm64.tar.gz.sha256
   frame/install.sh ~/Downloads/kartframe-1.0.0-linux-arm64.tar.gz
   ```

   The installer unpacks the program into `~/Games/KartFrame/app`. It adds **KartFrame** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It converts the Frame's controller models from SteamVR's own files. The script talks to Steam through its local debugging port, which SteamOS has on by default.

   The installer also gives the entry its library art: Mario Kart Wii key art from SteamGridDB for the portrait, wide and hero images (downloaded at install from fixed links, never shipped; ffmpeg is needed for the hero; credits in `frame/art/CREDITS.md`), a plain KartFrame logo and the dock icon. Without a network or ffmpeg, a slot gets the text-free backdrop that ships in `frame/art`. Art it replaces is saved under `~/.local/share/icon-backups`. `--no-art` skips all of it, and `--art DIR` installs your own images (`p.png`, `wide.png`, `hero.png`, `logo.png`, `icon.png`). If Steam does not take the dock icon while it runs, stop Steam and run `python3 frame/art/art.py --vdf ~/.local/share/Steam/userdata/<id>/config/shortcuts.vdf --appid <appid>`.

3. Build the game from your disc, once:

   ```sh
   ~/Games/KartFrame/app/setup.sh /path/to/MarioKartWii.iso
   ```

   Setup checks that the disc is RMCP01, extracts it to `~/.local/share/KartFrame/game`, translates the game, compiles it with the bundled compiler and installs the finished program. It took about 6 minutes on my Frame. It can be run again after an interruption and picks up where it stopped.

4. Put the headset on and launch **KartFrame** from your Steam library.

To update from 0.9.0-alpha.1, download the new tarball and run `frame/install.sh` again, then `setup.sh` again: the kit changed, and the launch wrapper refuses to start a game built for another kit. Old `Config.toml` files keep `refresh_rate = 120`; set `refresh_rate = 90` in it if you want the new default. Your game files, saves and settings stay where they are.

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
| Left stick | Steer, and move in menus |
| Right stick | Directional tricks |
| Left D-pad | Menu navigation; directional tricks, like the right stick |
| Both grips, right bumper | Drift |
| Right stick click | Switch between the first-person cockpit and the game's own camera |
| Right **Menu** | Start |
| Left **View** | Back. Held for half a second, it opens the VR settings panel. Pressed while the panel is open, it closes it |

In menus and in the settings panel, point with a controller: the ray and the controllers are drawn in front of the screen, the trigger or **A** selects and **B** goes back.

Hand steering (`hand_steering`, on by default): in the first-person cockpit, squeeze a grip near the wheel to hold it and steer by turning your hands, with one hand or both. The right bumper drifts there. Release both grips and the left stick steers again. Elsewhere the grips drift. The left trigger always brakes and reverses.

New since 0.9.0-alpha.1:

- **Held item.** In first person the item you hold is drawn in your hand (`cockpit_item_hand`: `"right"` by default, `"left"` or `"off"`; `cockpit_item_size`, 0.2 m).
- **Swing to throw.** With an item, a quick swing of that hand forward or back throws it that way. The hand must be off the wheel (`cockpit_item_throw`, on by default). A swing counts at 18 cm forward or 15 cm back within 120 ms; `KARTFRAME_THROW_*` changes that, and tell me if it feels too stiff or too loose.
- **Rumble.** The game's bumps and hits shake both controllers (`KARTFRAME_RUMBLE`, strength 0.7 by default, `0` turns it off).
- **Menu controllers.** The controllers and the pointer ray now sit in front of the menu screen (`KARTFRAME_MENU_CONTROLLERS=eyes`).
- The sticks, the right stick click and the grip wheel were already in 0.9.0-alpha.1; they were just missing from this table.

## Settings

Hold left View for half a second. The VR settings panel opens. Everything on it is also in `~/.local/share/KartFrame/UserData/Config.toml`, which is seeded from `Config.frame.toml` on the first run. The keys that matter:

| Key | Default | What it does |
|---|---|---|
| `[vr] render_scale` | 1.0 | Eye size relative to the size SteamVR gives the game: 1728 per eye, what the wrapper asks for (SteamVR's Auto setting on the Frame picks the same; SteamVR's 100 % is 2160, the panel's own resolution). 1.111 is 1920, 1.185 is 2048, 1.25 is 2160. The panel's "Resolution per eye" slider writes it; it applies at the next launch. Above 1.0 costs GPU time, see Performance |
| `[vr] frame_interpolation_fps` | 1 | 1 renders at the headset's refresh rate, interpolated from the game's 60 frames a second |
| `[vr] frame_pacing` | `"auto"` | `auto`, `inline` or `pipelined`. Auto picks pipelined |
| `[vr] refresh_rate` | 90 | 90 or 120. The launch wrapper sets it as SteamVR's rate for this game at every launch, and the game follows. 90 is the default: at 1728 per eye the GPU overruns 120 Hz and SteamVR skips frames in bursts. At 90 Hz each of the game's 60 frames a second spans 1.5 refreshes instead of 2 (interpolated). A Config.toml from an older install keeps its own `refresh_rate = 120` until you change it |
| `[vr] menu_shader_quality` | 0 | 0 is a plain dark background behind the menus |
| `[vr] hand_steering` | true | Grips hold the wheel in the first-person cockpit |
| `[vr] cockpit_item_hand` | `"right"` | The hand that holds your item in the first-person cockpit (`"left"`, `"right"` or `"off"`), and the hand whose swing throws it |
| `[vr] cockpit_item_size` | 0.2 | The held item's size in metres |
| `[vr] cockpit_item_throw` | true | A quick swing of the item hand forward or back throws the item that way |
| `[vr] first_person_toggle_click` | true | The right stick click switches the first-person cockpit on and off |
| `[vr] hud_virtual_screen` | true | Menus and the race HUD on a virtual screen |
| `[vr] hud_distance_meters` | 2.0 | How far away that screen is |
| `[vr] hud_width_meters` | 2.4 | How wide it is |
| `[video] resolution_multiplier` | 1.0 | Resolution of the flat desktop mirror |

I do not ship a default below 1728 per eye. Lower values of `render_scale` work, but the picture gets soft.

Switches for testing go in `~/.local/share/KartFrame/session.env`, one `NAME=value` per line, read at the next launch only:

| Name | What it does |
|---|---|
| `KARTFRAME_PACING` | `auto`, `inline` or `pipelined`, overriding the config |
| `KARTFRAME_REFRESH_HZ` | 90 or 120 for this launch, instead of `[vr] refresh_rate` (default 90) |
| `KARTFRAME_GX_THREAD` | `1` (default) runs the game's graphics work on its own thread: game-thread CPU 13.2 to 8.5 ms a frame, 8 to 11 frames a second more in Grand Prix. `0` or `off` goes back to one thread, to compare |
| `KARTFRAME_INTERP_MAX_MS` | Milliseconds of gap across which the interpolation keeps the previous frame (default 33, so one missed retrace glides instead of stepping; sets drawn without a previous frame went from 4.9 % to 0). `0` or `off` goes back to the game's own 1.5 intervals |
| `KARTFRAME_WORKER_LOCK` | `narrow` (default) or `full`: how long the frame worker holds the renderer lock per refresh set. `narrow` holds it for the set's setup only and encodes the eyes outside it, so the graphics thread blocks less: its lock wait fell from 5.0 to 1.7-2.6 ms a frame and Grand Prix went from 53.9 to 60.0 frames a second, with no fault or late set. `full` holds it for the whole set, as before, to compare |
| `KARTFRAME_EYE_PX` | The per-eye size the wrapper asks of SteamVR, 1024 to 4096 (default 1728; SteamVR's 100 % is 2160); `render_scale` multiplies it |
| `KARTFRAME_RENDER_SCALE` | `render_scale` for this launch, e.g. 1.25 for 2160 per eye |
| `KARTFRAME_SYSMEM` | 0 turns off the GPU's direct memory rendering mode (`TU_DEBUG=sysmem`); it is on by default |
| `KARTFRAME_FDM_LAYER` | 0 disables Valve's foveation layer in the Vulkan chain, to compare |
| `KARTFRAME_END_PHASE` | `after` (default), `before`, or `off`: when in the refresh each frame is handed to SteamVR, against the compositor grid the wrapper's helper publishes. `off` hands over at the wake-up |
| `KARTFRAME_STEREO_PACKED` | `1` (default) puts both eyes of a set in one 3456x1728 swapchain image, released once. `0` or `off` goes back to one swapchain per eye. That layout is retired: it brings the right-eye double image back, and `KARTFRAME_MENU_CONTROLLERS=eyes` needs the packed one |
| `KARTFRAME_END_DELAY_US` | A fixed delay after the wake-up instead of the grid, in microseconds; also the fallback when the grid is missing (4000) |
| `KARTFRAME_MENU_CONTROLLERS` | `eyes` (default) draws the controllers and the ray in front of the menu screen in 3D, one screen quad per eye (needs the packed default, else off is used). `off` goes back to the old look, where they sit behind the screen; `projected` draws the screen into the room layer instead (softer text) |
| `KARTFRAME_RUMBLE` | `0` turns off the game's rumble on the controllers; a strength such as `0.4` or `1.0` sets it (default 0.7; `1` alone means on at the default) |
| `KARTFRAME_THROW` | `0` turns off the swing to throw for this launch (`[vr] cockpit_item_throw`); `KARTFRAME_THROW_FORWARD_CM`, `_BACK_CM` and `_WINDOW_MS` change its thresholds (defaults 18, 15, 120) |
| `KARTFRAME_GHOST_MONO` | `1` brings back the game's flat Time Trial ghost; `KARTFRAME_GHOST_ALPHA` (default 0.5) sets how strong the 3D ghost is |
| `KARTFRAME_RESIDENT_GEOMETRY` | `1` caches display lists on the GPU (off by default; about 2 % less GPU time) |
| `KARTFRAME_CPULOG` | `1` logs one `cpulog:` line a second (game CPU, run-queue wait, lock waits, core clocks); `thermal.tsv` is always written |
| `KARTFRAME_VRTIMING_FRAMES` | 1 logs every compositor frame to `vrframes.log` in the session folder |
| `KARTFRAME_HOME` | The data folder, if it is not `~/.local/share/KartFrame` |

## Performance

Measured on my Frame from the session logs (Grand Prix at the defaults, driving seconds):

| | |
|---|---|
| Display | 90 Hz (120 Hz optional), 1728 x 1728 per eye |
| Game | 60.0 frames a second, 5 s minimum 59.5 to 59.9 |
| Eyes | 90.7 sets a second, 0 late, 0 repeated |
| GPU per set | about 8.7 to 8.9 ms of the 11.1 ms a refresh at 90 Hz (99th percentile 9.8 to 9.9) |
| Heaviest stretch measured | 589 to 615 draws a frame, with SteamVR's dashboard in front and the game running behind it: about 59 frames a second (5 s minimum 58.1) |
| First run | about 6 minutes from the disc: extract 1 minute, translate 50 seconds, compile 5 minutes |

What the build does, so you know what the numbers mean:

- Each refresh locates the head once and renders both eyes from that pose, two refreshes ahead of the refresh that shows them.
- A set of eyes is handed to SteamVR only after the GPU has finished it, at a fixed point of the refresh against the compositor's own clock. If a set is not finished in time, the previous set is shown again and the miss is counted.
- Every second, the game logs one `vr pacing:` line with the refresh rate, the sets rendered, interpolated, late and missed, and GPU and encode times. `report.txt` in each session folder sums them up, and `tools/session-report.py` lays several sessions side by side. `thermal.tsv` has temperatures, clocks and how busy the game's CPUs were.
- Keep builds and other heavy work off the Frame while you play. `launch.log` warns if compilers are running at launch, and `report.txt` has a `machine` line with the other processes' CPU use.

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
- **Motion is uneven.** `report.txt` shows late and repeated sets per second and, on its `machine` line, how much CPU other processes used. A few percent late is normal; more usually means something else is using the GPU or the CPUs (a recording, a stream, a compiler), or KartFrame's settings panel is open (the SteamVR dashboard itself costs nothing).
- **The world doubles when you turn your head.** Check `launch.log` for `KARTFRAME_STEREO_PACKED=1`; `0` brings the old double image back.

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

Mario Kart Wii, Nintendo and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve or the upstream author.

## Licence

GPLv3, as upstream: see [LICENSE](LICENSE). Third-party parts keep their own licences: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/` folder. The test probe in `frame/tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

Upstream's own README is kept as [README.upstream.md](README.upstream.md).

## How this was made

I wanted Mario Kart Wii on my Frame without Lepton, so I started from Wiicompiled VR PLUS and had it built natively for SteamVR: a Linux Arm64 build, Dawn's Vulkan backend on SteamVR's own device, pacing written for the Frame's compositor, the Frame controllers in code, and a first run that builds the game from your own disc on the Frame itself.

This is vibe coded, with manual review throughout. I wrote it with Claude Code running on the Frame itself. The double image took the longest to fix; my records of that work are in `frame/perf/`.

Developer notes and what was learned about SteamVR on the Frame are in the [technical notes](docs/TECHNICAL_NOTES.md).
