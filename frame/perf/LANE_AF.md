# Lane AF: controllers in front of the menu screen (KARTFRAME_MENU_CONTROLLERS)

Owner, headset 2026-10-27: in the menus the controllers appear BEHIND the virtual screen. The menus are the
screen's quad layer plus an environment projection layer (room, controller models, rays) that goes UNDER the quad,
because SteamVR on the Frame composites a projection layer before a quad whatever the submission order (H4.md). So
every pixel of a controller that overlaps the screen is covered by the quad, even when the controller is in front.

## The candidates

- (b) quad transparent where a controller is in front, and (c) other alpha flags on the environment layer: dead.
  H4's evidence is about ORDER, not blending: with the environment listed over the quad (SOURCE_ALPHA, premultiplied)
  the screen stayed fully visible while the ray on it vanished. Whatever SteamVR did with the alpha, an environment
  composited over the quad would have shown the ray (alpha 1) or blacked out the screen (the hole is 0,0,0,0 and
  would be black if alpha were ignored); neither happened, so the projection layer was under the quad. No flag
  combination (UNPREMULTIPLIED_ALPHA_BIT included) changes the order, so no switch was added for it.
- (a) as stated (controllers pasted into the one mono screen image): wrong in stereo. The quad shows the same image
  to both eyes, so pasted pixels sit at the screen's depth: a controller 0.4 m away would show in front of the screen
  as a flat cut-out with the screen's disparity, offset from the 3D controller around it by several centimetres per
  eye (a visible seam and double outline at the screen's edge). Not implemented.
- (a') **eyes** (implemented): the same composite, but ONE IMAGE PER EYE. The packed pair (lane AA) already holds an
  unused right half in the menus; the screen now goes into both halves, and each half gets that eye's view of the
  controllers and rays, projected from that eye onto the screen's plane (an off-axis frustum whose far plane is the
  screen: only what lies between the eye and the screen is drawn, so a controller pushed through the screen is cut
  at its surface). Two quads, XR_EYE_VISIBILITY_LEFT with the left half, _RIGHT with the right half. Each pixel of
  an eye's quad shows what that eye sees along that ray, so the controller is stereo-correct and meets its own
  environment part at the screen's edge exactly (same eye poses, same set). The screen stays a quad layer: sampled
  once by the compositor, as sharp as today.
- (d) **projected** (implemented): no screen quad; the environment layer draws the screen's image (the same image
  the quad would show, settings panel and pointer marks included) as a textured rectangle, then the rays and the
  controllers over it. Works with any layer order and any eye-visibility handling. Cost: the picture is resampled
  twice (into the eye image, then by the compositor), so the menu text is softer than with the quad.

## Switch values (KARTFRAME_MENU_CONTROLLERS, read by the OpenXR backend and by Aurora)

| value | layers in the menus | needs | owner should see |
| --- | --- | --- | --- |
| off (default, unset) | env projection, screen quad | - | today's: controllers and ray hidden where they overlap the screen |
| eyes | env projection, quad LEFT (left half), quad RIGHT (right half) | KARTFRAME_STEREO_PACKED=1 (else off, logged) | controllers and ray in front of the screen in 3D, screen as sharp as today |
| projected | env projection only (+ lane Q's tiny race quad) | - | controllers and ray in front of the screen; menu text a little softer |

The environment-ready log line names the mode, e.g. `OpenXR menu environment layer ready (3456x1728, under the
screen); menu controllers: eyes (a screen quad per eye)`.

Risk in eyes mode: it relies on SteamVR honouring a quad's eyeVisibility. If it does not, both eyes see one of the
two quads (likely the right one, listed last): the screen itself is identical in both halves, so only the
controller pixels on the screen would be off for one eye (a controller drawn where the other eye sees it). That is
what to look for in the headset. Hence default off until the owner has seen it.

GPU cost (eyes): one 1728x1728 screen pass into a scratch image (as today's target 0), two copies of it into the
halves, and one more controller pass per eye into the screen's picture rectangle; the controller vertex work is
done twice per eye on the CPU (about 9000 triangles a hand). Projected: one textured rectangle more in the env pass.

## Verification status (2026-10-27)

The build compiles and links (kfbuild.sh, exit 0, no errors). NOT yet run: ctest (stopped unfinished when the
usage budget ran out), and the mock menu runs. The planned check: mockrun with MOCK_XR_HANDS=high (right hand at
eye height in front of the screen), KARTFRAME_STEREO_PACKED=1, and each of off / eyes / projected, with MOCK_XR_DUMP
at about 20 and 30 s (before the demo race starts): the eyes dumps should show the controller over the screen in
both halves of the packed screen image, shifted between the halves by the parallax; projected dumps should show the
screen inside the environment image; 0 mock errors; menu sets/s the same as off.
