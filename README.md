# KartFrame

Mario Kart Wii in VR on the Valve Steam Frame.

Based on heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) v1.3.1.

## Features

- 90 Hz, 1728 pixels per eye, 60 game frames a second
- Frame controller layout, with rumble
- Grip the cockpit wheel to steer by hand
- Held item, swing to throw, 3D ghost

## How it works

- The game runs on the Frame itself, in SteamVR. Nothing is streamed from a PC.
- A one-time setup builds the game from your disc image.

## Requirements

- A Steam Frame with SteamVR, and about 1 GB free plus room for the extracted disc.
- Your own clean PAL Mario Kart Wii disc image (RMCP01).
- A terminal on the Frame: SSH, or Konsole in desktop mode.

## Install

1. Get the installer:

   ```sh
   git clone https://github.com/simplysanatori/KartFrame.git
   cd KartFrame
   ```

2. Download `kartframe-1.0.1-linux-arm64.tar.gz` and its `.sha256` from [Releases](https://github.com/simplysanatori/KartFrame/releases). Check and install it:

   ```sh
   (cd ~/Downloads && sha256sum -c kartframe-1.0.1-linux-arm64.tar.gz.sha256)
   frame/install.sh ~/Downloads/kartframe-1.0.1-linux-arm64.tar.gz
   ```

3. Build the game from your disc (about 6 minutes):

   ```sh
   ~/Games/KartFrame/app/setup.sh /path/to/MarioKartWii.iso
   ```

Other installers (then run step 3 yourself):

- [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/KartFrame`.
- [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FKartFrame%2Freleases%2Flatest%2Fdownload%2Fkartframe.framedrop.json).

## First run

1. Launch **KartFrame** from your Steam library.
2. In menus, point with a controller and press the trigger or **A**.

## Controls

| Steam Frame | In the game |
|---|---|
| Right trigger, **A** | Accelerate |
| Left trigger | Reverse |
| **B** | Brake, back |
| **X**, left bumper | Use item |
| **Y** | Look back |
| Left stick | Steer |
| Both grips, right bumper | Drift |
| Right stick | Tricks; click for cockpit view |
| Right **Menu** | Start |
| Left **View**, held | VR settings panel |

## Update

Download the new tarball, then in the `KartFrame` folder run:

```sh
git pull
frame/install.sh ~/Downloads/kartframe-<version>-linux-arm64.tar.gz
```

If the game then reports a kit mismatch, run `setup.sh` again.

## Uninstall

```sh
~/Games/KartFrame/app/tools/uninstall.sh
```

Add `--purge` to also delete your saves, settings and game files.

## Troubleshooting

- **Steam's debugging port is not reachable:** start Steam, run `install.sh` again.
- **Game won't start:** read `~/.local/share/KartFrame/sessions/latest/launch.log`.
- **Disc files not found:** run `~/Games/KartFrame/app/setup.sh status`.
- **Motion is uneven:** close the VR settings panel and other heavy apps.

## Reporting a problem

```sh
~/Games/KartFrame/app/tools/collect-logs.sh
```

It saves `kartframe-logs-<date-time>.tar.gz` in your home folder, with personal details masked. Check the files, then attach it to an [issue](https://github.com/simplysanatori/KartFrame/issues).

## More

- [User guide](docs/USER_GUIDE.md): all controls, settings and fixes
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](docs/TECHNICAL_NOTES.md#building)
- [Changelog](docs/CHANGELOG.md)

## Support

Star heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) and send game fixes upstream.

For more Steam Frame ports, [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- [simplysanatori](https://github.com/simplysanatori): built KartFrame.
- heurazy: [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), the base.
- iChris4: [Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR), the OpenXR port and translator.
- [SteamGridDB](https://www.steamgriddb.com/) artists: library art.
- Libraries and tools: [full credits](AGENTS.md#credits-in-full).

## Licence

GPLv3, as upstream ([LICENSE](LICENSE)); third-party licences in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Mario Kart Wii, Nintendo and their logos are trademarks of Nintendo. This is an unofficial fan project, not affiliated with or endorsed by Nintendo, Valve or the upstream author.

## How this was made

I started this project and used AI (Claude Code with Fable 5.1, Opus 5.5 and Sonnet 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame.
