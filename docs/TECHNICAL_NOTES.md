# KartFrame: technical notes

KartFrame is a fork of [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) by heurazy. In that project the Wii game is statically translated to C++, drawn by the aurora renderer on Dawn (WebGPU) and shown through OpenXR. KartFrame builds the same game as a Linux Arm64 program that runs in the Frame's own SteamVR, without Lepton, an APK or adb. This document is for version 1.0.0.

Install, first run, controls, settings and how to report problems are in the [README](../README.md). This one says how the port works, how to build it, and what I learned about SteamVR on the Frame. Where a claim needs a verdict from the headset it carries `[to confirm in the headset]`, and the claim is listed in [frame/UNTESTED.md](../frame/UNTESTED.md).

Not affiliated with Valve, Nintendo or the upstream author.

## AI disclosure

I did not write this code by hand. The port, its scripts and these notes were written with Anthropic's Claude (Fable 5.1, Opus 5.5 and Sonnet 5.5, through Claude Code), running on my Steam Frame. I chose what to attempt, supplied the disc, reviewed the code and the scripts as they were written, tested in the headset and reported what I saw. Claude read the logs and changed the code. The upstream author has not reviewed this port. Read the code before trusting it.

## Status

1.0.0 (2026-10-28). The build has been through twenty-six headset sessions. Confirmed: native Linux Arm64 on
SteamVR at 1728 per eye and 90 Hz (120 Hz optional), Grand Prix at 60.0 game frames a second with no late
sets, Time Trial against a 3D ghost, no double image (fixed, see below), quad-layer menus with the controllers
in front of the screen, the HUD on the virtual screen, the Frame controls and models, the first run from a
disc in about 6 minutes. Open: the SteamVR dashboard slows the game while it is open over it; 120 Hz needs
about 1 ms less GPU time a refresh; the heaviest course sections dip to about 58; full-view 2D effects and the
pointer dot on the menu screen are pending. The per-session records are in `frame/perf/`.

## How it works

### Platform and process

The build is a plain Linux Arm64 executable, `kartframe`, linked against glibc and libstdc++ only. The libraries that SteamOS does not ship in its base image (abseil, zlib, libpng, FreeType) are built static from pinned sources (`AURORA_VENDOR_THIRD_PARTY`, on by default on Linux). SDL3 is built from source and loads its audio and video drivers at run time. The game takes no command line arguments.

The upstream desktop code is kept. The Linux changes are small: the OpenXR path is on by default, the shared device Vulkan bridge that upstream wrote for the PC is used on Linux, the executable directory comes from `/proc/self/exe`, and the blob assembly of the translated data is written in ELF syntax.

### Install layout

`KARTFRAME_HOME` (default `~/.local/share/KartFrame`) holds:

- `portable.txt`, an empty marker that makes the game use `KARTFRAME_HOME/UserData` as its root instead of `~/.local/share/WiiCompiled`.
- `UserData/`: the game's `Config.toml`, `NAND/`, `Cache/`, `Logs/`, and `controller_models/frame`.
- `game/`: the extracted disc (`main.dol`, `StaticR.rel`, `DATA/`).
- `build/`: the translation and the objects made by setup.
- `bin/`: `kartframe`, its resources (`dsp_coef.bin`, `initial_pipeline_cache.db`, `wii_bootstrap/`, `resources/`) and `kartframe.fingerprint`.
- `sessions/<date-time>/`: the wrapper's logs.

The program directory, `~/Games/KartFrame/app`, holds the wrapper, the tools and, from a release, the kit, the toolchain, the translator and `setup.sh`.

### Data pipeline

Nothing derived from the disc is committed or released. A release carries:

- a **kit**: the prebuilt runtime (aurora, patched Dawn, SDL3, OpenXR loader and the static third party libraries), the compile flags and link recipe read from the real build graph, a minimal sysroot made from the build Frame's own packages (glibc and libstdc++ headers, kernel headers, crt files and libgcc), and a manifest with a sha256 per file and a fingerprint;
- the **translator**, a self contained single file publish of upstream's `Translator.Cli` for linux-arm64;
- a **portable toolchain** made by upstream's own `prepare-portable-tools.sh`: LLVM 22 (clang, lld, llvm-ar), CMake and Ninja.

`setup.sh <iso>` then runs four steps, once, resumable:

1. **Extract.** nodtool (fetched once and checked against a pinned sha256) reads the game id, requires RMCP01, checks `main.dol` and `StaticR.rel` against the sha256 pins in `projects/mkwii/recomp.yml`, and extracts to `game/`.
2. **Translate.** `translate-recursive`, `emit-base-manifest`, `generate-data-init --target-os linux` and `emit-build-shards`, the same sequence as upstream's `local-build.sh`.
3. **Build.** The translated shards are compiled with the kit's flags and the bundled compiler, three at a time (about 2.5 GB of memory each), and linked against the kit with lld.
4. **Install.** The binary and its resources go to `bin/`, with the kit fingerprint beside it, and `Config.toml` is seeded from `Config.frame.toml` if missing.

The whole run took about 6 minutes on my Frame. The wrapper compares the installed kit's fingerprint with the one recorded beside the binary and refuses to start if they differ. `frame/boundary-check.sh` checks a directory or tarball for anything disc derived, and files over 1 MB that `frame/kit-allowlist.txt` does not allow. Details for maintainers are in [frame/SETUP.md](../frame/SETUP.md).

### OpenXR session and renderer

Dawn's Vulkan backend is patched with upstream's own patch set at upstream's pinned revision (Aurora Dawn Vulkan ABI 1). With it the OpenXR runtime creates Dawn's `VkInstance` and `VkDevice` through `xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR`, borrowed swapchain images are wrapped as Dawn textures, and the eyes are rendered straight into the acquired images with no extra copy. The patched Dawn is built once on the Frame as a static monolithic library for linux-aarch64 (`frame/build-dawn.sh`), and aurora links it as a package.

I did not change the patch. What the Frame needs on top, I did in the bridge (`openxr_vulkan_win32.cpp`, which upstream wrote for the PC and which Linux now uses):

- The release barrier submission that follows aurora's submit is followed by an empty queue submission that signals a timeline semaphore, under Dawn's device lock. The pacing thread waits for it before it releases the swapchain image.
- Single level swapchains, array size 1, an sRGB sibling format with mutable format.
- The Vulkan instance is kept at 1.2 or lower.
- Dawn's device lock is held around exactly `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage` and `xrReleaseSwapchainImage`, because SteamVR's client submits on the one queue inside those calls.
- Two sets of eye swapchains, so that two frames can be in flight.

### Frame loop

Every refresh (8.33 ms at 120 Hz):

1. `xrWaitFrame` and `xrBeginFrame`.
2. One `xrLocateViews`. Both eyes, the HUD screen, the settings panel and the pointer use those poses and no others.
3. The pose packet goes to aurora's frame worker, which interpolates the retained game frame to that refresh's display time, renders both eyes into the acquired set and submits on Dawn's queue.
4. In **inline** mode the pacing thread waits for the GPU to finish that set, releases it and ends the frame with the same poses. In **pipelined** mode the set stays acquired into the next refresh: the previous set is waited for, released and ended with its own poses, whose display time is this refresh's by construction. The price is one refresh of latency; the gain is CPU and GPU work that overlap.
5. If a set is not finished by the deadline, the previous layer is shown again with its own poses, and the miss is counted. An unfinished image is never released.

`[vr] frame_pacing` is `auto`, `inline` or `pipelined`; `auto` is pipelined, because inline did not fit encode plus GPU of both eyes into one refresh in the mock measurements. `KARTFRAME_PACING` overrides it. An optional fallback to new eyes every second refresh exists (`KARTFRAME_HALF_RATE`); it is off by default.

The game's clock is converted to `XrTime` with `XR_KHR_convert_timespec_time`, which SteamVR offers. The first frame's `CLOCK_MONOTONIC` anchor is kept as a fallback. Input keeps its state through a loss of focus shorter than a second, because SteamVR takes focus for a few milliseconds now and then.

Once a second the game logs one line: `vr pacing: t= hz= mode= ticks= rendered= interpolated= late= missed= repeats= gpu_ms= encode_ms=`. `frame/tools/session-report.py` reads it together with `vrtiming.log` and writes `report.txt` in the session folder.

### Interpolation

The game's own VR frame interpolation (aurora's stereo replay with matched previous and current transform uniforms, up to three interpolated frames) with `[vr] frame_interpolation_fps = 1`. It renders on every compositor tick, so a 120 Hz display gets 120 stereo presentations a second from the game's 60 frames a second, each interpolated to that tick's display time and drawn from that tick's head pose. Frame synthesis (`XR_EXT_frame_synthesis`) and `XR_FB_space_warp` are accepted by SteamVR on the Frame and ignored, so they are not used.

### SteamVR per-app settings

SteamVR drops its per-app settings now and then, so the wrapper sets them at every launch through `vrcmd` and reads them back from `steamvr.vrsettings`, logging each difference:

```
preferredRefreshRate=90 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=1728
```

They only apply to a launch from the Steam library, where `SteamAppId` is set. The rate is `[vr] refresh_rate` or `KARTFRAME_REFRESH_HZ` (90 or 120). 90 is the default since headset session 24: at 1728 per eye the GPU overruns 120 Hz by about 1 ms a refresh and SteamVR skips frames in bursts, and the interpolated 90 Hz was judged good. The wrapper also sets `TU_DEBUG=sysmem` unless `KARTFRAME_SYSMEM=0`: on this GPU it was 7 to 8 percent faster for a draw bound game in the mock measurements. Valve's implicit layer `XR_APILAYER_VALVE_fdm_injection` sits in the Vulkan chain; `KARTFRAME_FDM_LAYER=0` sets `DISABLE_VULKAN_FDM_INJECTION_LAYER=1` so the two can be compared. Eye tracked foveation saved almost nothing and is not used.

### Resolution

1728 pixels per eye is what SteamVR's Auto resolution picks on the Frame (64 percent of the pixels of its 100 percent, which is the panel's 2160), and it is the floor for the shipped default (`render_scale = 1.0`). I measure lower values for the record but do not ship them.

### Controls

The layout is in code, in `runtime/include/vr/frame_controls.h`, as pure functions that a table test checks without OpenXR. The runtime enables `XR_VALVE_frame_controller_interaction`, suggests bindings for `/interaction_profiles/valve/frame_controller`, and latches the layout when `xrGetCurrentInteractionProfile` reports that profile. There is no SteamVR binding file. The controls table in the README is the layout as coded.

Since 1.0.0 the input layer also has the swing-to-throw detector (`openxr_item_throw.h`, from iChris4's fork): it watches the item hand's grip in the seat frame and, on a swing, plays an aimed throw on the virtual gamepad's port (stick Y fully forward or back, then the item button), because the default controller mode is the gamepad and not the Wii Remote path the fork used. The GameCube rumble motor (`PAD__ControlMotor_HLE`) is reported to a bridge and shaped into 50 ms haptic pulses on both controllers (`openxr_gamepad_rumble.h`). The held item is read at the race draw boundary and drawn by aurora in each eye's cockpit pass.

The View button has a small state machine: a tap goes back on release, a half second hold opens the VR settings panel, and a press while the panel is open closes it without also going back.

### Controller models

SteamVR's own Frame controller models have about 93,000 vertices a hand, and animating them on the CPU every frame made menus slow in earlier tests. `frame/frame-controller-models.py` therefore reads them from SteamVR's `frame_controller` driver on the user's Frame at install, simplifies them to about 4,000 triangles a hand, bakes the ambient occlusion into the base colour texture and writes the game's own model format into `UserData/controller_models/frame`. Valve's files are only read locally, never copied into the repository or a release. If the converted models are missing, the game draws its procedural controllers.

### Menus and the HUD

The game's 2D screen (menus, intro screens, the race HUD) is shown as a flat quad layer in front of the player instead of being drawn into the eyes, placed by `hud_distance_meters` and `hud_width_meters`.

### Packaging

`frame/release.sh` builds `kartframe-1.0.0-linux-arm64.tar.gz` from a kit directory (`frame/make-kit.sh`), the toolchain tarball directory, the published translator, the `frame/` scripts, `Config.frame.toml`, the controller model converter, the Steam shortcut helper, the two runtime tools (`vrtiming.py`, `session-report.py`), the README, the docs and the licences. It then runs `frame/boundary-check.sh` and `frame/tools/privacy-check.sh` on the tarball and writes its sha256.

## First headset session

The first session on a development build showed judder, half rate flapping and flickering menus. In more detail, from the logs and what I saw:

- Motion looked like 60 frames a second on a 120 Hz display, with ghosting when I turned my head.
- The 2D screen flickered together with the VR menu.
- The controller guide looked bad.
- The compositor itself was healthy: it reused almost no frames and its own time was small.
- The menus were drawn into the eyes with a projected surface. The intro and controller guide screens, together with the controllers, cost much more GPU time than the race path I had measured in the mock, and the fallback to every second refresh switched on and off repeatedly. A large share of the layers were repeats of an earlier set.
- The game asked for 90 Hz through `XR_FB_display_refresh_rate`, the only rate the runtime offers there, before the per-app setting gave 120 Hz.

Fixes made (confirmed in later sessions, except the item box):

- Menus move to a flat quad layer.
- The every second refresh fallback is no longer taken as a default.
- The game stops asking for a refresh rate that the runtime does not offer.
- `TU_DEBUG=sysmem` is checked against translucent overlays.
- The item box (the roulette) is drawn in the wrong place; reprojected 2D draws are being clipped to their frame rectangle. [to confirm in the headset]

## The double image: what twenty sessions found

The symptom: in races, on head turns, the image sat on two positions alternating every refresh, worse
in one eye, clean in the menus (a quad layer) and clean in GalaxyFrame on the same SteamVR.

Eliminated, each by a direct test (the records are `frame/perf/session-h*/README.md` and `LANE_*.md`):
pacing and late sets, retained repeats, end times, pose stamps, per-eye scene state, interpolation
evenness, eye poses and FOV, swapchain format and usage, pair counts 2 to 8, a quad beside the projection,
GalaxyFrame's first-write barrier, the prediction distance of the poses (fresh poses for the exact display
time did not help), the rendered images themselves (240 consecutive sets dumped from the mock: no jump),
the desktop window's presents, the release ordering against the GPU (a strict mock check), the size of the
GPU work, the refresh rate, and the thread priority boost.

What SteamVR itself showed: the game's own recorded race images, handed over by a minimal probe loop
(`frame/tools/xrprobe --replay`) in six variations including the game's own order, were clean.

The mechanism (the first account of it, kept as measured; the cause is the packed-swapchain paragraph further down), from logging every compositor frame beside the game's own timeline (`tools/vrtiming.py`
frame mode, `frame/tools/phase.py`): the compositor's update runs at about vsync + 3.0 to + 5.3 ms and
takes each eye's newest released image when it reaches that eye; the game handed its frame over near those
points, and whether a hand-over fell before or after an eye's pick-up changed from refresh to refresh. A
launch whose hand-over kept one side every frame was clean; one whose compositor updates started early in
episodes was doubled. Ending each frame at a fixed phase against the published grid (`KARTFRAME_END_PHASE`,
lane X) keeps one side and removes the doubling. It leaves the right eye one set ahead of the left and the
compositor reusing a frame now and then, so it was a mitigation. The probe ends its frames at the same phase
as the doubled default and is clean, so the phase is not the cause; the remaining difference is the 4-7 ms
of GPU work the game submits right after each hand-over on the GPU the compositor composes on. That experiment (the probe with synthetic GPU load) was abandoned when the packed swapchain, below,
ended the problem.

Session 21 (2026-10-27): with both eyes in one swapchain image per set (`KARTFRAME_STEREO_PACKED=1`) the
race was clean with the end lock, and also clean with the lock off, where the old right-eye double image did
not appear. That fits the per-eye pick-up model: the compositor takes each eye's newest released image at its
own point in the refresh, and one image per set removes the chance of pairing a right eye from one set with a
left eye from another. Clean in one session each, so a follow-up A B A B of full races (packed against
per-eye) is pending (`frame/perf/session-h22/`). The wrapper now defaults to packed with the end lock kept.

**Fixed (session 22, 2026-10-27).** The A B A B of about one minute of racing per launch
(`frame/perf/session-h22/RESULTS.md`) settled it: the two packed launches were "perfect" and "good, with slight
jitter", the two per-eye launches "bad, doubling in the right eye" and "terrible, right eye true doubling every
moment". The end phase was locked to "after" in all four launches (the per-eye launch 04 had no flips and still
doubled), so the hand-over point was never the cause. What fits: with one swapchain per eye, SteamVR's compositor
can take the right eye from one set and the left from another, and a head turn shows that as a doubled world.
With both eyes of a set in one image, the pair cannot be split. Packed (`KARTFRAME_STEREO_PACKED=1`) is the
default; one swapchain per eye is retired and kept only as a comparison switch. The end lock stays on. The
packed layout held in Grand Prix (session 23 onwards) with no doubling reported in any launch.

## Grand Prix speed (sessions 23 to 25)

Grand Prix ran the game at 43 to 51 frames a second, which the interpolation turned into uneven stepping. The
cause was the game thread's CPU (13 to 14.5 ms a frame), not the eye size, heat or scheduling. Two changes
fixed it (`frame/perf/session-h23` to `session-h25`): the GX thread (`KARTFRAME_GX_THREAD=1`, upstream's switch,
now on) took the graphics work off the game thread (13.2 to 8.5 ms a frame; the nameplate hook had to post its
depth and anchor calls to that thread, as they drain aurora's command queue), and a narrow renderer lock
(`KARTFRAME_WORKER_LOCK=narrow`, 2026-10-27) lets the frame worker hold the lock only for a set's setup, not its
encode, which cut the graphics thread's lock wait from 5.0 to 1.7-2.6 ms a frame: 60.0 against 53.9 frames a
second, and 59.9 against 47.6 on the same paused frame. The interpolation keeps its previous frame across gaps of
up to 33 ms. The limits left are the graphics thread's decode per draw in the heaviest sections, the lock time
that remains and the per-set encode.

## Time Trial ghost (2026-10-28)

The game draws the ghost alone into the cleared EFB, copies the picture out (colour and depth), clears the part it
copied, draws the race and blends the copy back with one quad. That quad was a flat picture in both eyes. Each eye
now re-draws the ghost's own 3D draws at the composite, translucent (0.5) and depth-tested. The first version
never engaged, because the clear after the copy is a scissored clear of the copied rectangle, not a whole-target
load op; the link now accepts a colour and depth clear covering the depth copy's area. `KARTFRAME_PASS_DUMP=<n>`
logs the passes of the first n such frames. Confirmed in the headset: one kart at the right depth.



## Building

Needs an Arm64 Linux machine with a compiler, CMake and Ninja (the Frame needs them installed; the release does not). The steps for developers, in short, are in [frame/DEV.md](../frame/DEV.md):

1. `frame/build-dawn.sh`, once: the patched static Dawn goes to `~/.cache/kartframe/dawn/<revision>-<patch hash>/`. The first build took about 12 minutes here.
2. Translate the game from your own disc (`frame/setup.sh translate`) to get a `generated` tree, then configure and build `runtime/` in Release mode with `-DMKW_DAWN_PACKAGE_DIR` pointing at that Dawn. The binary is `build-linux/kartframe`.
3. `frame/install.sh --dev` copies a build into the data folder and adds a separate shortcut, for testing.
4. `frame/make-kit.sh <dir>` makes the kit, `frame/toolchain.sh` the toolchain, `frame/publish-translator.sh <dir>` the translator, and `frame/release.sh` the tarball.

Games are run only against the mock runtime during development: `frame/tools/mockrun.sh` uses a private Xvfb, a dummy audio driver and aborts if the mock runtime file is missing.

## SteamVR on the Frame: settled facts

These came from probes and runs on the Frame, and I build on them:

- SteamVR shows a swapchain image the moment it is released, finished or not, in Vulkan too. Release only after a fence or timeline wait shows the GPU is done. It costs no latency.
- One waited, unreleased image per swapchain. Pipelining two frames needs two sets of eye swapchains.
- SteamVR allocates only mip 0 of a Vulkan swapchain, so swapchains have a single level.
- Locate the views once per refresh and render both eyes from exactly those poses; release and end the frame with those same poses. A right eye rendered a refresh later got random placement error.
- The Vulkan instance must be 1.0 to 1.2; 1.3 features come as extensions. There is one queue, and the SteamVR client submits on it inside the four calls above.
- `XR_EXT_frame_synthesis` and `XR_FB_space_warp` are accepted and ignored. Depth submission works.
- SteamVR takes focus for a few milliseconds now and then. That is not a pause.
- The runtime offers `XR_KHR_vulkan_enable2`, `XR_KHR_convert_timespec_time`, `XR_FB_display_refresh_rate`, `XR_KHR_composition_layer_depth`, `XR_EXT_eye_gaze_interaction` and `XR_VALVE_frame_controller_interaction`.
- At 120 Hz the GPU budget for both eyes including the composite is about 7.1 ms; SteamVR's compositor takes about 0.8 ms.

The probes and the mock runtime come from my GalaxyFrame work with bigmak94's GalaxyQuest (`xrmock`, and `xrprobe`, which is GPLv3 and not part of the game).

## How this was made

See "How this was made" in the [README](../README.md).

## Licence

GPLv3, as upstream. Third party parts keep their licences: see [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).
