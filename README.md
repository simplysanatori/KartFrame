# Mario Kart Wii VR on the Steam Frame

This is a fork of [heurazy/Wiicompiled_VR-PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS),
the Mario Kart Wii VR port. It runs standalone on Valve's Steam Frame through Lepton, the Frame's
Android compatibility tool. No PC is involved.

Upstream's Quest APK installs on the Frame, but the game crashes as soon as it starts. With the
changes here it starts from the Steam library, runs at 90 Hz, and uses a gamepad-style layout on
the Frame controllers.

The branch is `steam-frame`. The fork's files are in [`steam-frame/`](steam-frame/), plus six
commits to `runtime/` and `aurora-main/`. Upstream's README is kept as
[README.upstream.md](README.upstream.md). Read it for what the port itself does. This file only
covers the Frame.

**Status, October 2026.** Release `v1.3.1-frame.3` works on my Steam Frame with SteamOS 0.4.5,
Lepton 2.8.14 and SteamVR. Tested: single player, the original and first-person cameras, the
menus and the VR settings panel. Read [Known problems](#known-problems) before you start.

**AI use.** The code and the docs in this fork were written with Claude Code (Anthropic's Claude
Opus 5.5 and Claude Fable 5.1), running on the Frame. I chose what to try, tested every build in
the headset and reported what I saw. Nobody from the upstream project has reviewed this fork.
Read the diffs before you trust them.

## Contents

- [What the fork changes](#what-the-fork-changes)
- [Requirements](#requirements)
- [Install](#install)
- [First run: import the disc and build the game](#first-run-import-the-disc-and-build-the-game)
- [Updating](#updating)
- [Controls](#controls)
- [Settings](#settings)
- [Known problems](#known-problems)
- [Troubleshooting](#troubleshooting)
- [Logs and reporting a problem](#logs-and-reporting-a-problem)
- [Building the APK yourself](#building-the-apk-yourself)
- [Where things are](#where-things-are)
- [How this was made](#how-this-was-made)
- [Licence and credits](#licence-and-credits)

## What the fork changes

Upstream's design stays the same. The APK carries a "game kit": precompiled runtime objects,
headers and a toolchain. The headset translates your own disc and links `libmain.so` against that
kit. No Nintendo code is in the APK or in this repository.

The Steam Frame APK is upstream's v1.3.1 Quest APK with four kit files replaced by ones compiled
from this branch, one patched `classes.dex`, and a new kit fingerprint. When the headset builds
your game, it links it against the Frame objects.

1. **The app starts.** Upstream's app registers a clipboard listener at startup. Lepton's Android
   has no clipboard service, so the app crashed with a `NullPointerException`. That call is
   replaced with no-ops in `classes.dex` ([`steam-frame/dexpatch.py`](steam-frame/dexpatch.py)).

2. **90 Hz.** Upstream turns VR frame interpolation off on Android. On the Frame that showed as
   judder and ghosting when turning the head. Interpolation is on again, so the eyes render at the
   headset's refresh rate with a fresh head pose each frame while the game runs at 60 fps.
   SteamVR's OpenXR in Lepton lacks `XR_KHR_convert_timespec_time`, so there is a
   `CLOCK_MONOTONIC` fallback. (`runtime/src/vr/openxr_integration.cpp`)

3. **Frame controller layout.** The game's OpenXR actions were written for Quest Touch
   controllers. On the Frame, SteamVR mapped A to three actions and left B, X and Y unused. The
   runtime now also has hand-independent actions (`frame_item`, `frame_drift`,
   `frame_look_back`, `frame_start`, `frame_back` and four D-pad actions).
   [`steam-frame/bindings_frame_controller.json`](steam-frame/bindings_frame_controller.json)
   binds the Frame buttons to them. When SteamVR reports them bound, the game uses the layout in
   [Controls](#controls) for the rest of the session. (`runtime/src/vr/openxr_input.cpp`)

4. **Menus at 90 Hz.** Upstream's animated menu background held the menus at 37 to 45 fps. On
   Android the menus are now always drawn into the eye images, and menu background quality 0
   gives a plain dark background. (`openxr_integration.cpp`, `menu_shader_quality = 0`)

5. **Item box (attempted, does not work yet).** The port now skips 2D HUD draws that lie wholly
   outside the game's scissor rectangle. The empty item box still shows in the headset. The
   change is harmless and stays in. (`aurora-main/lib/gfx/common.cpp`)

6. **Frame settings.** The launch wrapper seeds `Config.toml` from
   [`steam-frame/Config.frame.toml`](steam-frame/Config.frame.toml) on the first launch, and
   `install.sh` sets per-app SteamVR values. See [Settings](#settings).

7. **Launch wrapper.** The app's 2D launcher never shows in the headset on the Frame.
   [`steam-frame/wiicompiled-launch.sh`](steam-frame/wiicompiled-launch.sh) waits for the
   launcher and starts the game activity itself, once a game built for this APK's kit is
   installed. A launch from the library reaches the title screen in about ten seconds.

8. **Grips in the cockpit.** In the first-person cockpit the grips hold the steering wheel. In
   the other cameras they drift. The right bumper drifts in every camera.

9. **Frame controller models.** Inside Lepton the app gets no SteamVR render models, so upstream
   showed Quest controllers in the menus.
   [`steam-frame/frame-controller-models.py`](steam-frame/frame-controller-models.py) converts the
   Frame controller models from your own SteamVR install into the game's format and simplifies
   them to about 4,000 triangles per hand. `install.sh` runs it, and the launch wrapper copies the
   result over the Quest files on each launch. Valve's models are never included in the repository
   or the APK.

The cameras, hand IK, HUD and Retro Rewind support are upstream's work and are unchanged.

## Requirements

- A Steam Frame.
- **Lepton**, installed from Steam (Library > Tools).
- Your own clean PAL Mario Kart Wii disc image, game ID `RMCP01`. ISO works. Upstream's importer
  also lists RVZ, WBFS and other formats. Other regions and modified images are rejected.
- About 5 GB free.
- A terminal on the Frame: Konsole in desktop mode, or SSH from another computer.
- Steam running while you install. No root and no `pacman` are needed.

## Install

1. Get the repository:

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/Wiicompiled_VR-PLUS-steam-frame.git
   cd Wiicompiled_VR-PLUS-steam-frame
   ```

2. Download `WiiCompiled-VR-SteamFrame-*.apk` from the
   [Releases](https://github.com/simplysanatori/Wiicompiled_VR-PLUS-steam-frame/releases) page.
   Use `v1.3.1-frame.2` or newer. Each release lists the APK's SHA-256 in `SHA256SUMS.txt`:

   ```sh
   sha256sum ~/Downloads/WiiCompiled-VR-SteamFrame-*.apk
   ```

   You can also build the APK yourself ([Building the APK yourself](#building-the-apk-yourself)).
   The APK contains no game data.

3. Run the installer with Steam running. Use the `install.sh` from the same release tag or newer:

   ```sh
   steam-frame/install.sh ~/Downloads/WiiCompiled-VR-SteamFrame-*.apk
   ```

   The installer:

   - copies the APK, the launch wrapper and the seed config to `~/Games/WiiCompiledVR-SteamFrame/`;
   - converts the Frame controller models from SteamVR;
   - adds a **Mario Kart Wii VR** entry to the Steam library, set to the Lepton compatibility
     tool, through Steam's client API;
   - writes the controller binding and the 90 Hz settings into SteamVR for that entry;
   - prints the entry's appid. You need it for the first run.

   Running it again updates the same entry.

## First run: import the disc and build the game

1. Put the disc image in `~/Documents/` on the Frame. Lepton shows that folder to the app as
   `/sdcard/Documents`.

2. Put the headset on and launch **Mario Kart Wii VR** from the library. The first launch takes
   about a minute while Lepton installs the APK. You will only see SteamVR's empty space. The
   app's launcher is running but cannot be seen. Leave it running.

3. In the terminal, import the disc. Replace `<appid>` with the number from `install.sh` and
   `MarioKart.iso` with your file name:

   ```sh
   LEPTON=~/.local/share/Steam/steamapps/common/Lepton/lepton
   $LEPTON exec steamlaunch-<appid> am start-foreground-service -n org.wiicompiled.quest/.launcher.GameSetupService \
       -d file:///sdcard/Documents/MarioKart.iso --es task ExtractDisc
   ```

   This checks the disc and extracts it into the app's data folder. It takes a few minutes.

4. Build the game:

   ```sh
   $LEPTON exec steamlaunch-<appid> am start-foreground-service -n org.wiicompiled.quest/.launcher.GameSetupService \
       --es task BuildGame
   ```

   The headset translates and compiles the game against the Frame kit. This took about 13
   minutes on my Frame. Progress goes to
   `~/.local/share/Steam/steamapps/compatdata/<appid>/external/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/Logs/build_*.log`.
   You can also follow it over adb:

   ```sh
   /usr/lib/android-sdk/platform-tools/adb connect localhost:5555
   /usr/lib/android-sdk/platform-tools/adb -s localhost:5555 logcat | grep -iE 'GameSetup|DiscExtract|GameBuild|WiiCompiled'
   ```

5. When the build log says the game is installed, quit the app (Steam button, then exit) and
   launch it again. From now on every launch goes straight into the game. On the first start the
   game asks you to pick a camera with the pointer. Your choices are saved to `Config.toml`.

## Updating

- **From an older Frame release.** Run the newer `install.sh` with the new APK. It updates the
  same library entry. Lepton reinstalls the app on the next launch. A new release usually has a
  new kit fingerprint, so run the `BuildGame` command from the first run again. The disc import,
  saves and settings are kept. After an update the Frame controller models show from the second
  launch, because the app unpacks its own files again first.
- **From upstream's Quest APK on the Frame.** If your library entry has the same name, `install.sh`
  points it at the Frame APK. The extracted disc stays, but the old game was built against
  upstream's kit and will not run. Run `BuildGame` again. The disc import is not needed.
- **Newer upstream versions.** This fork is built on upstream v1.3.1. A newer upstream APK has a
  different kit, and the patches need porting first. `build.sh` stops if the kits do not match.

## Controls

`install.sh` installs the binding for the library entry. The game switches to this layout by
itself when SteamVR reports the Frame actions as bound. The layout is the same in every camera.

The Frame's left controller has the D-pad and View button. The right controller has A, B, X, Y
and the Menu button.

| Frame controller | Racing | Game menus |
| --- | --- | --- |
| Left stick | Steer | Move |
| Right trigger | Accelerate | Confirm |
| A | Accelerate | Confirm |
| B | Brake and reverse | Back |
| Left trigger | Brake and reverse (works with accelerate held) | Back |
| X | Use item (hold to drag it behind) | |
| Left bumper | Use item | |
| Y | Look back | |
| Right bumper | Hop and drift | |
| Grips (either) | Hop and drift. In the first-person cockpit: hold the wheel | |
| D-pad | GameCube D-pad. Up does a trick, or a wheelie on a bike | D-pad |
| Right stick | Same as the D-pad (push up for a trick) | |
| Right stick click | Next camera: original, first person, diorama | |
| Menu | Pause (Start) | Start |
| View, tap | Back | Back |
| View, hold 0.5 s | Open the VR settings panel | Open the VR settings panel |

Notes:

- The View tap is sent when you let go, so a hold never also goes back.
- The right stick click does not change the camera during the race intro. It can be turned off
  with `first_person_toggle_click = false` under `[vr]` in `Config.toml`.
- **Cockpit wheel.** In the first-person camera, reach for the wheel and squeeze a grip to hold
  it, then turn it to steer. Let go to steer with the stick again. With `hand_steering = false`
  under `[vr]`, the grips drift in every camera and you always steer with the stick.
- **VR settings panel.** Hold View for half a second to open it. Point with a controller. The hand
  whose trigger you pulled last does the pointing. A trigger or A selects, either stick scrolls,
  and a press of View closes the panel. While the panel is open the game gets no input.
- **Controller models.** The menus show Frame controllers when `install.sh` converted them. The
  tutorials still name Quest buttons. Use the table above.
- To change the layout, use SteamVR's binding UI. The binding is a normal SteamVR binding and shows
  up under Manage Controller Bindings for the entry.

## Settings

The launch wrapper copies these values into `Config.toml` on the first launch only. The VR
settings panel edits the same file. Upstream's README lists all settings under "VR settings".

| `Config.toml` | Seed value | What it does |
| --- | --- | --- |
| `[vr] frame_interpolation_fps` | `1` | Render at the headset's refresh rate (90 Hz), interpolated from the game's 60 fps. |
| `[vr] hand_steering` | `true` | Grips hold the wheel in the first-person cockpit. |
| `[vr] menu_shader_quality` | `0` | Plain dark menu background. Higher values cost frame rate on the Frame. |
| `[vr] render_scale` | `1.0` | Eye render scale. |
| `[video] resolution_multiplier` | `1.0` | Game resolution. 1.5 dropped frames on the Frame. |
| `[vr] hand_hud` | `false` | Upstream's hand HUD, off. |
| `[diagnostics] openxr_logging` | not set (off) | Logs frame timing once a second to `console.log`. Leave it off for play. It caused small hitches. |

`install.sh` also sets these SteamVR values for the entry: `preferredRefreshRate 90`,
`framesToThrottle 0` and `additionalFramesToPredict 0`. They are in the `steam.app.<appid>`
section of `~/.config/openvr/config/steamvr.vrsettings`.

Environment variables for the scripts:

| Variable | Script | Effect |
| --- | --- | --- |
| `WCF_FRAME_MODELS=0` | `install.sh` | Keep the Quest controller models. |
| `WCF_FRAME_MODEL_TRIANGLES` | `frame-controller-models.py` | Triangle target per hand (default 4000). |
| `WCF_SHORTCUT_NAME` | `install.sh` | Library entry name (default `Mario Kart Wii VR`). |
| `WCF_STAY_IN_LAUNCHER=1` | `wiicompiled-launch.sh` | Leave the invisible launcher up. Put it before the wrapper in the entry's launch options. |
| `WCF_TOOLS` | `build.sh` | Toolchain folder (default `~/.local/opt/android-build`). |
| `WCF_SIGNING` | `build.sh` | Signing key folder (default `steam-frame/signing`). |
| `WCF_LOG_DIR` | `collect-logs.sh` | Where the log archive goes (default your home folder). |

Two Android properties help when testing. Set them over adb before the game starts:
`setprop debug.wiicompiled.vr_interp 0|1|72|90|120` overrides the interpolation setting for one
launch, and `setprop debug.wiicompiled.fpslog 1` logs the game's frame rate.

## Known problems

- **The empty item box stays on screen** after the roulette slides up. Change 5 does not fix it.
- **Grip steering in the cockpit is rough.** Grabbing and steering work, but upstream tuned the
  grab range and steering response for Quest controllers. They have not been adjusted. Set
  `hand_steering = false` to steer with the stick.
- **Small hitches.** Races hold 90 fps on the counter, with brief hitches now and then. The logs
  do not show a cause yet.
- **The 2D launcher is invisible** in the headset, so the disc import and build use the terminal
  commands above. Upstream's "Select disc image" button needs a file picker Lepton does not have.
- **`v1.3.1-frame.1` does not work.** Its game build fails at the final link on the headset. Use
  `v1.3.1-frame.2` or newer.
- **Not tested:** Retro Rewind, online play, the diorama camera for more than a minute, USB wheels,
  multiplayer, passthrough, hand tracking, Lepton 3.x.
- **Not for Quest.** Use upstream's APK on a Quest. The interpolation change is what upstream
  turned off on Quest, and the Frame layout only works with the SteamVR binding.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Steam must be running. The script
  talks to Steam on `127.0.0.1:8080`, which SteamOS enables by default.
- **The launch shows only SteamVR's empty space.** No game is built yet, or the built game is for
  another kit. `/tmp/wiicompiled-launch.log` says which. Run the `BuildGame` command.
- **The disc is rejected.** The importer accepts only a clean PAL `RMCP01` image.
- **The game build fails at the last step with `undefined symbol: std::__ndk1::__hash_memory`.**
  You have the `v1.3.1-frame.1` APK. Install `v1.3.1-frame.2` or newer and build again.
- **The buttons act like a Quest layout** (A does several things, X and Y do nothing). The SteamVR
  binding is not applied. Run `install.sh` again, then check Manage Controller Bindings for the
  entry. The game's `console.log` has the line `Steam Frame button layout active` when the Frame
  layout is on.
- **The menus show Quest controllers.** Launch once more after an update. If they still show,
  run `install.sh` again and look for a warning about the controller models.
- **The menus or races stutter.** Check `Config.toml` for `menu_shader_quality = 0`,
  `resolution_multiplier = 1.0` and `openxr_logging` off. Higher resolution settings dropped
  frames on the Frame.
- **Settings are not saved after you copied `Config.toml` in by hand.** Run
  `chmod 664 Config.toml` on it. The app cannot write it otherwise.
- **Anything else.** Collect the logs (next section) and look at `console.log` and `lepton.log`.

## Logs and reporting a problem

| Log | Where |
| --- | --- |
| Launch wrapper | `/tmp/wiicompiled-launch.log` |
| Lepton | `~/.local/share/Steam/logs/lepton-steamlaunch-<appid>.log` |
| Game, one folder per run | `.../WiiCompiledOpenXRVR/Logs/<run>/console.log` |
| Disc import and build | `.../WiiCompiledOpenXRVR/Logs/build_*.log` |

`...` is `~/.local/share/Steam/steamapps/compatdata/<appid>/external/Android/data/org.wiicompiled.quest/files`.

To report a problem:

1. Run `steam-frame/collect-logs.sh` (or `steam-frame/collect-logs.sh <appid>`). It writes
   `~/wiicompiled-frame-logs-<date>.tar.gz` with the logs above, your `Config.toml` and a short
   summary (SteamOS, Lepton, APK and kit versions). It masks IP and MAC addresses, UUIDs, host
   names, Wi-Fi names, serial numbers, Steam IDs, email addresses, tokens and certificate blocks.
   Lepton's log includes the Frame's kernel log, which lists hardware serial numbers. Read the
   files before you post them. The masking can miss things.
2. Open an issue on this repository. Give the release tag, what you did, what happened and what
   you expected, and attach the archive.
3. If the problem also happens with upstream's APK on a Quest, report it upstream instead.

Pull requests are welcome. Say what you tested and on which headset. Upstream's
[CONTRIBUTING.md](CONTRIBUTING.md) rules apply. Never attach game files or disc images.

## Building the APK yourself

```sh
steam-frame/build.sh                  # downloads upstream's v1.3.1 APK and checks its SHA-256
steam-frame/build.sh path/to/WiiCompiled-VR-Quest3-v1.3.1.apk
```

The output is `steam-frame/release/WiiCompiled-VR-SteamFrame-<git tag>.apk`. On the Frame this
takes about 20 seconds once the toolchain is downloaded. It needs `curl`, `tar`, `python3` and
`openssl`, which SteamOS has. It needs no root.

1. It downloads Android NDK r29 into `~/.local/opt/android-build/`. Google ships no arm64 Linux
   NDK, so on the Frame it uses [HomuHomu833's r29 build](https://github.com/HomuHomu833/android-ndk-custom).
   On x86_64 it uses Google's r29, which is untested. The NDK must be r29, the version the kit
   was built with. Objects built with r30 fail the final link on the headset.
2. It compiles the two runtime unity groups that hold the patched files, and Aurora's
   `common.cpp`, with upstream's Android flags. The third-party headers they need are in
   [`steam-frame/build-deps/`](steam-frame/build-deps/) (see its `NOTICE.md`).
3. It compares each new object with the original in the upstream kit. The exported symbols must
   match, and the new object may not import a C++ symbol the original does not. Otherwise it stops.
4. [`make-apk.py`](steam-frame/make-apk.py) writes the APK: the four kit files, the new kit
   fingerprint in `kit.json`, the patched `classes.dex`, zip alignment and an APK Signature
   Scheme v2 signature made with `openssl`. No Java, apksigner or zipalign is needed.

The signing key is created in `steam-frame/signing/` on first use. Keep it. An APK signed with
another key will not install over the previous one. `steam-frame/signing/` is ignored by git, so
the key never ends up in the repository. `make-apk.py verify file.apk` checks an APK's signature.

The game itself (`libmain.so`) is never built here. The headset builds it from your disc.

## Where things are

- **Library entry:** a non-Steam shortcut whose executable is the APK and whose compatibility tool
  is `lepton`. Its launch options run `~/Games/WiiCompiledVR-SteamFrame/wiicompiled-launch.sh`.
  The empty file `lepton-show-flatscreen` next to the APK lets Lepton show the 2D window.
- **App data:** `~/.local/share/Steam/steamapps/compatdata/<appid>/`.
  `external/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/` holds `Config.toml`,
  `DATA` (the extracted disc), saves and `Logs/`.
  `internal/org.wiicompiled.quest/files/game/base/` holds the built `libmain.so` and `game.json`.
- **SteamVR settings:** the `steam.app.<appid>` section of
  `~/.config/openvr/config/steamvr.vrsettings`. While SteamVR runs, the installer writes them with
  `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-int`.
- **Shared folders:** Lepton's Android sees `~/Documents`, `~/Downloads` and `~/Videos` as
  `/sdcard/Documents`, `/sdcard/Download` and `/sdcard/Movies`.
- **Shell access:** `lepton exec steamlaunch-<appid> <command>` runs a command in the running
  container. `adb connect localhost:5555` reaches it too.

## How this was made

The work took about a week in October 2026, in Claude Code sessions on the Frame. I tested each
build in the headset and described what I saw. The AI read the logs and the code and made the
changes.

- The first attempt used upstream's v1.3 Quest APK. The clipboard crash showed in logcat and was
  patched. The game ran, but it flickered and the head tracking was shaky.
- Upstream's Windows build under Proton and a native Linux ARM64 build were also tried. The Proton
  build was too slow under x86 emulation. Both were dropped. This fork is the Android path only.
- Turning interpolation back on, with the clock fallback, fixed most of the flicker and judder.
  The controller layout, the menu change and the item box attempt followed.
- The first release was built with NDK r30 and failed to link on the headset. `build.sh` now uses
  r29 and checks the C++ imports of each object.
- A higher resolution for sharper menus caused dropped frames and was rolled back.
- After the first releases came the grips change and the Frame controller models. The full
  models slowed the menus to 45 to 60 fps. With the mesh simplifier the menus hold 90 Hz.

## Licence and credits

GPL-3.0, as upstream ([LICENSE](LICENSE)). The fork's additions use the same licence.
Third-party headers used for the build are listed with their licences in
[`steam-frame/build-deps/NOTICE.md`](steam-frame/build-deps/NOTICE.md). The rest of the
third-party code is listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

- The VR port is [heurazy's Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS),
  built on [iChris4's Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR) and the
  WiiCompiled static recompilation project. It uses
  [Aurora](https://github.com/encounter/aurora), Dawn, SDL3 and OpenXR. The full credits are in
  [README.upstream.md](README.upstream.md).
- The Android NDK build for arm64 Linux is
  [HomuHomu833's](https://github.com/HomuHomu833/android-ndk-custom).
- Lepton, SteamVR and the Frame controller models are Valve's. The models are read from your own
  SteamVR install and are not redistributed.
- Mario Kart Wii is Nintendo's. Nothing of Nintendo's is in this repository or in the APK. You
  need your own disc.
- Steam Frame port by simplysanatori, written with Claude Code as described above.

## Support

If you would like more Steam Frame ports, you can [buy me a coffee](https://ko-fi.com/sana748653). The credit for the games themselves belongs to the upstream projects named above.
