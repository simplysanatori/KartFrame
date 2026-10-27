# Lane AH: the held item in the hand (WIP: built and unit-tested, not yet run in the mock)

What: iChris4's Wiicompiled_VR fork's held item (db30945, b181efa, 5f2ecc0, 7f5a849, 405ac27, 4489d4a; no
multiview), drawn in the cockpit pass of each eye after the hands. Keys: `[vr] cockpit_item_hand`
("left"|"right"|"off", default right), `[vr] cockpit_item_size` (0.05-0.60 m, default 0.2); env overrides
KARTFRAME_COCKPIT_ITEM_HAND / _SIZE; KARTFRAME_COCKPIT_ITEM_TEST=<id>[:<count>] for tests (frame/DEV.md).

Evidence so far:
- kfbuild.sh clean; ctest 34/34 (32 + mkw_vr_item_tests + mkw_cockpit_item_data_tests).
- mkw_cockpit_item_data_tests on the disc's Race/Common.szs (by hand, nothing committed): all 15 item
  models parse (vertices, textures, materials listed).
- git merge-tree against lane/AC and lane/AI: no conflicts.

Not verified yet (the run was stopped for the usage budget):
- Mock race in first person (frame/perf/hudrace.sh with `default_camera = 1` in KF_MOCKRUN_VR, MOCK_XR_HANDS=1,
  KARTFRAME_COCKPIT_ITEM_TEST=16:3 and a real pick-up), per-eye dumps showing the item with disparity,
  0 mock errors, the archive parse time (log "cockpit item: ... parsed in X ms"), the item pass's GPU cost
  per set (KARTFRAME_FPSLOG=2, Cockpit segment, item on against cockpit_item_hand = "off"; expected <= 0.3 ms),
  no regression with "off".
- The character-hand change: a hand the body IK draws now stays tracked with characterHand set
  (openxr_integration.cpp BuildCockpit); Aurora skips drawing it. Check the hands look as before.
- Headset: the item offset on the Frame's grip pose (tuned on Quest Touch), the item with stick steering
  (needs hand_steering or body_ik on: openxr_input.cpp, lane AI's file, gates hands[].tracked on them).
