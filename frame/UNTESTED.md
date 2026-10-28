# Built but not confirmed in the headset

One line per claim. When the owner has tested one, delete its line here and complete the text in README.md
and the release notes. Twenty-six headset sessions have confirmed most of the build, including (1.0.0)
Time Trial against the 3D ghost, Grand Prix at 60 frames a second at 90 Hz, the packed swapchain, the GX
thread, the narrow lock, the 33 ms interpolation window and the menu controllers in front of the screen.
What is left:

## Controls

- The held item drawn in your hand in first person (`cockpit_item_hand`, `cockpit_item_size`): mock only (the item
  test switch, log lines); its offset on the Frame's grip pose was tuned on Quest controllers.
- Throwing the held item with a swing (`cockpit_item_throw`): mock only, with injected throws; the thresholds
  (18 cm forward, 15 cm back within 120 ms) were tuned on Quest controllers.
- The game's rumble on the Frame controllers in gamepad mode (strength 0.7, the on/off duty mapping): mock log
  lines only.

## Rendering and performance

- The item box (roulette) drawing in the right place during a race.
- Full-view 2D effects (Blooper ink, the black of a respawn) across the whole view. Deferred.
- Resident geometry (`KARTFRAME_RESIDENT_GEOMETRY=1`, off by default): mock only (-2 % GPU a set).
- Eye sizes above 1728 (`render_scale`, `KARTFRAME_EYE_PX`, `KARTFRAME_RENDER_SCALE`): not measured in the mock or
  the headset; at 2160 the packed image is 4320 wide, over SteamVR's 4096 maximum view rect (frame/perf/LANE_AG.md).
- Heavy courses while focused (560+ draws a frame while driving): the one measured stretch (589 to 615 draws) was with
  the dashboard open and ran about 59 frames a second.
- Two and four player races (heavier; measured in the mock only: 2p 113 sets/s at 120 Hz, 6 % late).
- 120 Hz as an option (`KARTFRAME_REFRESH_HZ=120`): known to overrun the GPU at 1728 per eye.

## Screen and menus

- The laser pointer's dot on the 2D menu screen (the ray shows, the dot on the screen does not yet).
- `hud_distance_meters` and `hud_width_meters` away from their defaults.
- `KARTFRAME_MENU_CONTROLLERS=projected` (screen in the room layer): only run in the mock, no image checked.

## Install and first run

- The Mario Kart Wii library art and the dock icon after a Steam restart, on a Frame that did not install them
  from this repository.
- Updating from 0.9.0-alpha.1 (new kit, so `setup.sh` runs again).
- setup.sh from the release tarball on a clean Frame, start to finish, by someone other than the owner.
- uninstall.sh on a real install.
