# KartFrame user guide

The full reference for playing KartFrame on the Steam Frame: install details, every control, every setting, performance, files and troubleshooting. The short version is in the [README](../README.md).

## Status

Version 1.0.0, the first release. It plays on my Steam Frame at 1728 pixels per eye and 90 Hz, with both eyes rendered on every refresh from your head position at that refresh, and the game at its full 60 frames a second in Time Trial and Grand Prix. Menus sit on a steady flat screen with the controllers and the pointer in front of it, and the race HUD is on a virtual screen.

**Compared with the Lepton APK.** Before KartFrame I played the upstream Quest APK through Lepton. There the game showed its 60 frames at 72 Hz with judder, or blurred at 120 Hz, because the Android build leaves the game's frame interpolation out. The image ghosted and doubled when I turned my head, menus ran at 37 to 45 frames a second, and anything above 1728 per eye brought stalls of 30 to 50 ms. Every launch needed adb workarounds, a re-signed APK and a patched dex, and the controls were whatever SteamVR's remap of the Vive-wand bindings allowed. Here the game's own interpolation runs at 90 Hz, the picture is clean when you turn your head, menus run at the display rate, the controls are made for the Frame controllers, and one Steam shortcut starts it. The chip does less work for the same game, with native code and no Android system underneath, so battery life should be better too.

Known issues:

- The VR menu over a running race slows the race until you close it.
- 120 Hz is there if you want to try it, but at 1728 per eye the GPU runs out of time and SteamVR skips frames.
- The heaviest parts of a few courses dip a little below 60.
- Anything else heavy running on the Frame while you play (a compile, a download) makes it stutter.

## Install details

The steps are in the [README](../README.md#install). What each step does:

**`frame/install.sh`.** The installer unpacks the program into `~/Games/KartFrame/app`. It adds **KartFrame** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It converts the Frame's controller models from SteamVR's own files. The script talks to Steam through its local debugging port, which SteamOS has on by default.

The installer also gives the entry its library art: Mario Kart Wii key art from SteamGridDB for the portrait, wide and hero images (downloaded at install from fixed links, never shipped; ffmpeg is needed for the hero; credits in `frame/art/CREDITS.md`), a plain KartFrame logo and the dock icon. Without a network or ffmpeg, a slot gets the text-free backdrop that ships in `frame/art`. Art it replaces is saved under `~/.local/share/icon-backups`. `--no-art` skips all of it, and `--art DIR` installs your own images (`p.png`, `wide.png`, `hero.png`, `logo.png`, `icon.png`). If Steam does not take the dock icon while it runs, stop Steam and run `python3 frame/art/art.py --vdf ~/.local/share/Steam/userdata/<id>/config/shortcuts.vdf --appid <appid>`.

**`setup.sh`.** Setup checks that the disc is RMCP01, extracts it to `~/.local/share/KartFrame/game`, translates the game, compiles it with the bundled compiler and installs the finished program. It took about 6 minutes on my Frame. It can be run again after an interruption and picks up where it stopped.

**Updating.** To update, run `frame/install.sh` with the new tarball. If the launch wrapper then reports a kit mismatch, run `setup.sh` again.

**Uninstalling.** The installer changes nothing outside the folders listed under [Files](#files), the Steam shortcut and the controller models. To remove KartFrame, run `~/Games/KartFrame/app/tools/uninstall.sh`. It keeps your data folder unless you pass `--purge`.

### Install with Frametanium

If you use [Frametanium](https://github.com/Cogential/frametanium), add `simplysanatori/KartFrame` there. It downloads the release, runs its `install.sh` and updates it when I publish a new version. Run the one time disc setup (step 3 of the README install) yourself, and again after an update if the launch wrapper reports a kit mismatch.

### Install with FrameDrop

[FrameDrop](https://framedropvr.com) can install this too: [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FKartFrame%2Freleases%2Flatest%2Fdownload%2Fkartframe.framedrop.json). The release has a `kartframe-<version>-framedrop-linux-arm64.zip` and a `kartframe.framedrop.json` for it. FrameDrop unpacks the zip on the Frame and adds its own library entry, which runs the `run.sh` in the zip. The first time it runs, and again after an update, `run.sh` runs `install.sh` with the Steam shortcut step turned off, so you get FrameDrop's entry and no second one from the installer, and then starts KartFrame through its launch wrapper. The one time disc setup is still yours to run, with `~/Games/KartFrame/app/setup.sh`.

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

Also:

- **Held item.** In first person the item you hold is drawn in your hand (`cockpit_item_hand`: `"right"` by default, `"left"` or `"off"`; `cockpit_item_size`, 0.2 m).
- **Swing to throw.** With an item, a quick swing of that hand forward or back throws it that way. The hand must be off the wheel (`cockpit_item_throw`, on by default). A swing counts at 18 cm forward or 15 cm back within 120 ms; `KARTFRAME_THROW_*` changes that, and tell me if it feels too stiff or too loose.
- **Rumble.** The game's bumps and hits shake both controllers (`KARTFRAME_RUMBLE`, strength 0.7 by default, `0` turns it off).
- **Menu controllers.** The controllers and the pointer ray sit in front of the menu screen (`KARTFRAME_MENU_CONTROLLERS=eyes`).

## Settings

Hold left View for half a second. The VR settings panel opens. Everything on it is also in `~/.local/share/KartFrame/UserData/Config.toml`, which is seeded from `Config.frame.toml` on the first run. The keys that matter:

| Key | Default | What it does |
|---|---|---|
| `[vr] render_scale` | 1.0 | Eye size relative to the size SteamVR gives the game: 1728 per eye, what the wrapper asks for (SteamVR's Auto setting on the Frame picks the same; SteamVR's 100 % is 2160, the panel's own resolution). 1.111 is 1920, 1.185 is 2048, 1.25 is 2160. The panel's "Resolution per eye" slider writes it; it applies at the next launch. Above 1.0 costs GPU time, see [Performance](#performance) |
| `[vr] frame_interpolation_fps` | 1 | 1 renders at the headset's refresh rate, interpolated from the game's 60 frames a second |
| `[vr] frame_pacing` | `"auto"` | `auto`, `inline` or `pipelined`. Auto picks pipelined |
| `[vr] refresh_rate` | 90 | 90 or 120. The launch wrapper sets it as SteamVR's rate for this game at every launch, and the game follows. 90 is the default: at 1728 per eye the GPU overruns 120 Hz and SteamVR skips frames in bursts. At 90 Hz each of the game's 60 frames a second spans 1.5 refreshes instead of 2 (interpolated). |
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

### Test switches

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

How the build paces frames is in [AGENTS.md](../AGENTS.md#performance-internals). For reading your own numbers:

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

It gathers the newest session's logs into one archive, `kartframe-logs-<date-time>.tar.gz` in your home folder, and masks IP and MAC addresses, host names, network names, serial numbers, Steam ids, email addresses and user names in paths. The masking is a best effort. Read the files before you attach them.
