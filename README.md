# KartFrame

Mario Kart Wii in VR, running natively on the Valve Steam Frame.

A fork of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) (v1.3.1). No game data is included; bring your own disc.

## Features

- 90 Hz, 1728 pixels per eye, 60 game frames a second
- Frame controller layout, with rumble
- Hand steering on the cockpit wheel
- Held item, swing to throw, 3D ghost
- No Lepton, APK or adb needed

## How it works

- The game runs on the Frame itself as a Linux program, inside SteamVR.
- You supply the disc image. A one time setup builds the game from it.
- Nothing is streamed from a PC.

## Requirements

- A Steam Frame with SteamVR. Steam running while you install.
- About 1 GB free, plus room for the extracted disc.
- Your own clean PAL Mario Kart Wii disc image (RMCP01).
- A terminal on the Frame: SSH, or Konsole from desktop mode.

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

3. Build the game from your disc (once, about 6 minutes):

   ```sh
   ~/Games/KartFrame/app/setup.sh /path/to/MarioKartWii.iso
   ```

Other installers (then run step 3 yourself):

- [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/KartFrame`.
- [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FKartFrame%2Freleases%2Flatest%2Fdownload%2Fkartframe.framedrop.json).

## First run

1. Put the headset on and launch **KartFrame** from your Steam library.
2. In menus, point with a controller and press the trigger or **A**.
3. Hold left **View** for half a second for the VR settings panel.

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

In the cockpit, grip the wheel to steer by hand. Full list: [user guide](docs/USER_GUIDE.md#controls).

## Update

Download the new tarball, then in the `KartFrame` folder run:

```sh
git pull
frame/install.sh ~/Downloads/kartframe-<version>-linux-arm64.tar.gz
```

If the game then reports a kit mismatch, run `setup.sh` again.

## Uninstall

Add `--purge` to also delete your data folder:

```sh
~/Games/KartFrame/app/tools/uninstall.sh
```

## Troubleshooting

- **Debugging port not reachable:** start Steam, run `install.sh` again.
- **Game won't start:** read `~/.local/share/KartFrame/sessions/latest/launch.log`.
- **Disc files not found:** run `~/Games/KartFrame/app/setup.sh status`.
- **Motion is uneven:** close the VR settings panel and other heavy apps.

More fixes: [user guide](docs/USER_GUIDE.md#troubleshooting).

## Reporting a problem

```sh
~/Games/KartFrame/app/tools/collect-logs.sh
```

It saves `kartframe-logs-<date-time>.tar.gz` in your home folder, with personal details masked. Check the files, then attach it to an [issue](https://github.com/simplysanatori/KartFrame/issues).

## More

- [User guide](docs/USER_GUIDE.md)
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](docs/TECHNICAL_NOTES.md#building)
- [Changelog](docs/CHANGELOG.md)

## Support

Support heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) first: star it, report bugs there, send game fixes upstream.

For more Steam Frame ports, [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- heurazy: [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) and the [Mario Kart Wii VR port](https://github.com/heurazy/mario-kart-wii-VR-port), the base.
- iChris4: [Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR), the OpenXR port and translator.
- [SteamGridDB](https://www.steamgriddb.com/) artists: library art.

Built by [simplysanatori](https://github.com/simplysanatori). Libraries and tools: [full credits](AGENTS.md#credits-in-full).

Mario Kart Wii, Nintendo and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve or the upstream author.

## Licence

GPLv3, as upstream: [LICENSE](LICENSE). Third-party licences: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## How this was made

I started this project and used AI (Claude Code with Fable 5.1, Opus 5.5 and Sonnet 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame.
