# Lane AA: both eyes in one swapchain per set (KARTFRAME_STEREO_PACKED, test only)

Why: SteamVR on the Frame shows an image as soon as it is released, and its compositor takes each eye's
LATEST RELEASED image when it reaches that eye. The race used one swapchain per eye per set, released one
after the other, with several sets in flight, so the compositor could pair a right eye from one set with a
left eye from another (the right-eye double image of sessions h19/h20). The menus already pack their
environment layer side by side and are clean.

What `KARTFRAME_STEREO_PACKED=1` does (default off; read by the OpenXR backend and by Aurora's sink):
- Each eye pair is ONE swapchain, 2 x eye wide (3456x1728 at 1728), single mip, arraySize 1, the eyes'
  format and usage. It is acquired and waited once and released once per set, after the GPU-complete wait
  as before. Sets in flight, pair rotation, retained repeats, poses, fov, end phase are unchanged.
- The race's projection views both name that swapchain, eye N with imageRect offset x = N * eye width.
- Aurora's sink gets the same image as both eye targets and writes both halves in one render pass (the
  sRGB draw, one viewport and scissor per eye); KARTFRAME_SINK=copy copies to the half's offset; frame
  stamps go into each half's corner. The release barrier is issued once for the shared image.
- Menus: the screen image goes into the left half of the pair's image and the quad takes that half
  (1728x972 rect); the environment layer is unchanged (already packed).
- Startup line: `stereo packed: on (3456x1728, one swapchain per set)`.

## Mock results (hudrace.sh 100 s, Luigi Circuit, render scale 1.0 at 120 Hz, TU_DEBUG=sysmem,
`MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KF_MOCKRUN_END_GATE=2 KARTFRAME_FRAME_LOG=1 MOCK_XR_SWAY=17.19,2,51`,
full-resolution dumps at 65 and 75 s; run order A1 P2 A2 P3, P1 was a cold-cache first run, left out)

Race window t = 45-98 s, leaving out the 6 seconds with missed refreshes (the synchronous full-size dumps
at 65 and 75 s stall the loop in both modes):

| run | packed | mock errors | sets/s | late | GPU ms p50 / p99 (mean of seconds) | encode p50 | ends > 1 ms after wake-up (whole run) | releases | XRS sets with both eyes from the set's own pose |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | no | 0 | 119.1 | 0 % | 5.36 / 6.82 | 2.18 | 1.3 % | 21369 | 6750 / 6750 |
| P2 | yes | 0 | 117.7 | 0 % | 4.38 / 4.80 | 2.01 | 0.6 % | 14547 | 6632 / 6632 |
| A2 | no | 0 | 119.2 | 0 % | 4.41 / 4.88 | 2.06 | 0.7 % | 21399 | 6744 / 6744 |
| P3 | yes | 0 | 118.8 | 0 % | 4.36 / 4.85 | 1.99 | 0.4 % | 14643 | 6730 / 6730 |

- 0 errors, 0 releases before the GPU finished, end times all equal to the predicted time, in every run.
- GPU per set: packed is no dearer (4.36-4.38 against 4.41 back to back; A1's 5.36 is the first warm run).
- One release per set instead of two (about 14.6k against 21.4k releases a run).
- Each half holds its own eye: all 20 full-size packed dumps of P1-P3 decoded: left half's stamp eye 0,
  right half's eye 1, both with the same display time and the same pose time (the set's own locate), and
  the halves differ (mean |L-R| 21-33 of 255, the stereo disparity). The image looks right (two eye views
  side by side). A menu dump (M1, 20 s) shows the screen in the left half, letterboxed as before.
- Mock blind spots in packed mode: the mock treats a projection layer whose two views share one swapchain
  as the menus' environment ("E" in its layer list), so its stamp check, pose-stamp check, late-end model and
  compositor model skip the race layer (they report 0 checked). The evidence above comes from the dumps and
  the game's own XRS lines instead. In per-eye runs: stamps 13524 / 13510 eye images rendered for their
  submitted locate, 0 for another; pose stamps per set: 0 sets with eyes from different packets.
- ctest 32/32. mkw_openxr_vulkan_replay_tests assumes two eye releases per set and fails with the switch on
  (not run that way by ctest).

## For the headset
Session A/B: `KARTFRAME_STEREO_PACKED=1` in session.env against the default. Expected if the per-eye
pick-up is the cause: the right eye's double image on head turns gone in races, menus unchanged.
Risks: Valve's implicit FDM layer (XR_APILAYER_VALVE_fdm_injection) may handle a 2 x eye image as one eye's
(keep DISABLE_VULKAN_FDM_INJECTION_LAYER=1 as the A/B); the compositor may filter a column across the seam
between the halves; SteamVR's own pick-up of one wide image is untested on the Frame for the race.
