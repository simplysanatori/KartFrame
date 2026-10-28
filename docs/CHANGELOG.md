# Changelog

## 1.0.0 (2026-10-28)

The first full release. It plays Time Trial against a ghost and Grand Prix at 60 game frames a second, no doubled image, at 1728 pixels per eye. The only slowdown left that I know of is while the SteamVR dashboard is open over the running game.

### Fixed

- The right-eye double image in races is fixed at its cause. Both eyes of a refresh now share one swapchain image (`KARTFRAME_STEREO_PACKED=1`, the default), so SteamVR can no longer pair a right eye from one refresh with a left eye from another. In the headset A B A B (session 22) both packed launches were clean and both per-eye launches doubled. The timed hand-over (`KARTFRAME_END_PHASE=after`) stays on.
- Grand Prix speed. The game's graphics work runs on its own thread (GX thread), and the frame worker holds the renderer lock only for a set's setup instead of the whole set (`narrow`). Two Grand Prix races of about two minutes of driving each held 60.0 frames a second; with the old lock the same race averaged 53.9 and dipped to 42.
- The Time Trial ghost is drawn in 3D: each eye draws the ghost kart from its own view, translucent and depth-tested against the course, where the game blended one flat picture into both eyes. Adapted from iChris4's Wiicompiled_VR (f2f7c50, 7f9de4c, e89988f).
- Projected reflections (Rainbow Road, Coconut Mall glass) no longer follow your head (iChris4, 4aac0ec).
- Vertex and index staging grew (3 to 8 MB and 2 to 4 MB), and a draw that overflows is skipped and logged instead of aborting the game (iChris4, e4c98da).
- The game's rumble (bumps, hits) now reaches the controllers. In the default gamepad mode it did not reach them at all.
- In menus, the controllers and the pointer ray show in front of the menu screen, in 3D. They used to be hidden behind it.

### Added

- The item you hold is drawn in your hand in first person (iChris4: db30945, b181efa, 5f2ecc0, 7f5a849, 405ac27, 4489d4a). The models are read from your own disc at run time.
- Swing to throw: a quick swing of the item hand forward or back throws the item that way (iChris4: ebc97ef, 4489d4a).
- Rumble on both controllers, with a strength setting (`KARTFRAME_RUMBLE`).
- Refresh rate and eye size settings: `[vr] refresh_rate` or `KARTFRAME_REFRESH_HZ` (90 or 120), `render_scale` or `KARTFRAME_RENDER_SCALE`, `KARTFRAME_EYE_PX`. The wrapper sets the refresh rate in SteamVR for you and the game logs `view size:` at start.
- An interpolation window of 33 ms (`KARTFRAME_INTERP_MAX_MS`): one missed game frame glides instead of stepping.
- Resident geometry (`KARTFRAME_RESIDENT_GEOMETRY=1`, off by default): display lists cached on the GPU, drawn through native vertex input (iChris4, b5d9c19). About 2 % less GPU time in the mock.
- The position-matrix index is baked into compacted-layout shaders (iChris4, 236665c).
- Per-second CPU, thermal and load logging: `KARTFRAME_CPULOG=1`, `thermal.tsv` in each session folder, and a `machine` line in `report.txt`. The launch log warns when compilers or other builds are running.
- `KARTFRAME_PASS_DUMP=<n>` logs the game's render passes for a frame; `frame/tools/abcompare.sh` compares two builds in the mock runtime.
- KartFrame library art: the Mario Kart Wii key art from SteamGridDB (downloaded at install, never shipped), a plain KartFrame logo and a dock icon. `frame/install.sh` installs them; `--art DIR` uses your own images, `--no-art` skips them.
- The game says KartFrame everywhere you see it: runtime logs, settings panel, window title and installer. WiiCompiled stays in the credits and licences.

### Changed defaults

- 90 Hz (was 120). New installs seed `refresh_rate = 90`; an older `Config.toml` keeps its 120 until you change it.
- GX thread on, `KARTFRAME_WORKER_LOCK=narrow`, packed stereo swapchain, interpolation window 33 ms.
- Menu controllers in front of the screen (`KARTFRAME_MENU_CONTROLLERS=eyes`).
- Item hand: right (`cockpit_item_hand`). Item throw on (`cockpit_item_throw`).
- Eye size stays 1728 per eye and the timed hand-over stays on.

### Known issues

- Opening the SteamVR dashboard over a running game slows it and drops frames. Expected.
- 120 Hz at 1728 per eye needs about 1 ms less GPU time a refresh than the Frame has; SteamVR skips frames in bursts. It is still a switch (`KARTFRAME_REFRESH_HZ=120`).
- The heaviest sections of a course (600 draws or more) dip to about 58 frames a second.
- Other work on the Frame's CPUs while you play (compilers, container builds) makes the game stutter, and the clocks drop when the Frame is hot.
- `KARTFRAME_STEREO_PACKED=0` (one swapchain per eye) is retired. It only stays as a comparison switch and brings the right-eye double image back. Menu controllers in front of the screen need the packed layout.
- Full-view 2D effects (Blooper ink, the black of a respawn) do not cover the whole view. The laser pointer's dot is not visible on the 2D menu screen (the ray is). Two and four player races are not measured in the headset.
- Built but not yet seen in the headset: the held item, the swing to throw, the rumble strength, resident geometry, eye sizes above 1728. [frame/UNTESTED.md](../frame/UNTESTED.md) has the list.

### Credits

- iChris4's [Wiicompiled_VR](https://github.com/iChris4/Wiicompiled_VR) for the ported commits named above (GPL-3.0).
- heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), which KartFrame is a fork of.
- Library art from SteamGridDB: AVEX (portrait), Sho. (wide), MegaCrashFan96 (hero, an upscale of CaptainScarLeg's). Asset ids in `frame/art/CREDITS.md`.

## 0.9.0-alpha.1 (October 2026), pre-release

The first public build of the native port. Everything below has been played on a Steam Frame unless marked
otherwise. One known issue remains open, the double image in races (see the README); this build carries
the mitigation that removes it.

### Confirmed in the headset

- Runs natively on the Frame in SteamVR at 120 Hz, 1728 pixels per eye, both eyes rendered on every refresh: races hold 119-120 sets a second with under 1 % late.
- The game itself runs at its full 60 frames a second (the first session's "half speed" was a window-sync call sleeping 10 ms every game frame under gamescope; fixed).
- Menus and the intro screens as a flat quad layer in front of you, sharp and steady; the race HUD on the virtual screen with correct clipping; the thin line beside the map removed.
- The Frame controller layout, hand steering with the grips, the simplified Frame controller models in the hands.
- First run from your own disc in about 6 minutes: extract, translate, compile with the bundled compiler, link against the kit.
- The installer, the Steam library entry, the launch wrapper with SteamVR's per-app settings and the session logs.

### Rendering and pacing work

- Dawn's Vulkan backend on SteamVR's own device; eye images released only after a GPU fence (timeline semaphore under Dawn's lock).
- Pipelined pacing: each set of eyes rendered two refreshes ahead, every frame handed over at a fixed phase against SteamVR's compositor (the double-image mitigation, `KARTFRAME_END_PHASE`), released only when finished; retained repeat when late; half-rate never by default.
- An sRGB sink and a cut of the native render pass: eyes to swapchain 2.3 ms -> 0.47 ms.
- Turnip's sysmem mode by default (7-8 % faster for this draw-bound game).
- Thread priority (RealtimeKit) and a utilisation clamp so the game thread and the frame worker stay on the big cores.
- Input kept through SteamVR's short focus losses; refresh rate requested only when offered.

### Tools (for developers)

- A mock OpenXR runtime (`frame/tools/xrmock`) with exact checks: finished releases, pose stamps, end times, layer lists, per-eye probes, image dumps.
- `xrprobe --replay`: SteamVR shown the game's own recorded eye images through a minimal loop.
- Compositor timing per frame (`tools/vrtiming.py`), `frame/tools/phase.py`, panel recordings and their analysis.

### Known issues

- The double image in races: see the README. This build removes it by timing the hand-over; the right eye still runs one set ahead of the left, which reads as extra sensitivity to head movement.
- The Time Trial ghost is flat. Full-view 2D effects (Blooper ink) do not cover the whole view. The laser pointer's dot is not visible on the 2D menu screen.
