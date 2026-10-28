# Built but not confirmed in the headset

One line per claim. When the owner has tested one, delete its line here and complete the text in README.md
and the release notes. Twenty headset sessions have confirmed most of the build; what is left:

## Rendering

- The item box (roulette) drawing in the right place during a race (the clip fix is in; the demo race has no HUD).
- Full-view 2D effects (Blooper ink, the black of a respawn) across the whole view. Deferred.
- The Time Trial ghost drawn in 3D (lane AE: each eye draws the ghost kart itself; its strength, 0.5, is a guess).
- The held item in the hand (lane AH): mock only (the item test switch, log lines); not seen in an image or the headset;
  its offset on the Frame's grip pose was tuned on Quest controllers.
- Resident geometry (`KARTFRAME_RESIDENT_GEOMETRY=1`, off by default): mock only (-2 % GPU a set).
- Two and four player races at 120 Hz (heavier; measured in the mock only: 2p 113 sets/s, 6 % late).

- Packed stereo swapchain (`KARTFRAME_STEREO_PACKED=1`, the wrapper default since session 21) over full 3-lap races: clean in one short session each with and without the end lock; the A B A B against per-eye is queued (session h22). Also: no seam or foveation artefact was reported in session 21, but not looked for over a long race.

- Eye sizes above 1728 (`render_scale`, `KARTFRAME_EYE_PX`, `KARTFRAME_RENDER_SCALE`): not measured in the mock or
  the headset; at 2160 the packed image is 4320 wide, over SteamVR's 4096 maximum view rect (frame/perf/LANE_AG.md).
- 90 Hz through `[vr] refresh_rate = 90` (the wrapper now passes it to SteamVR and the game) and the end lock
  following a 90 Hz compositor grid: code read only, not run.

## Performance defaults (session 24)

- The GX thread (`KARTFRAME_GX_THREAD=1`, the wrapper default now) over full 3-lap races: three short launches (57 to 76 s races) were clean, none went the distance.
- 90 Hz as the default: how it feels over a full race (the owner played short races at 90 Hz in session 24; 120 Hz is still optional).
- `KARTFRAME_INTERP_MAX_MS=33` over full races (no artefact seen in one short launch).
- `KARTFRAME_WORKER_LOCK=narrow` (the wrapper default since session 25) over long sessions: two launches of about two minutes of driving each were clean (0 faults, 0 late sets, no visual fault); a session of many races, and the heaviest courses (560+ draws a frame while driving), not yet.

## Screen and menus

- The laser pointer's dot on the 2D menu screen (the ray shows, the dot on the screen does not yet).
- hud_distance_meters and hud_width_meters away from their defaults.

- `KARTFRAME_MENU_CONTROLLERS=eyes|projected` (controllers in front of the screen; off by default): eyes relies on SteamVR
  honouring a quad's eye visibility, unseen; the mock ran eyes without errors, no image was checked.

## Controls

- Throwing the held item with a swing (thresholds tuned on Quest controllers): mock only, with injected throws.
- The game's rumble on the Frame controllers in gamepad mode (strength 0.7, the on/off duty mapping): mock log lines only.

## Install and first run

- setup.sh from the release tarball on a clean Frame, start to finish, by someone other than the owner.
- uninstall.sh on a real install.
