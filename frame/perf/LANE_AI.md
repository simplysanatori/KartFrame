# Lane AI: throwing the held item with a swing, and GameCube rumble on the controllers

Files: runtime/src/vr/openxr_input.cpp, runtime/include/vr/openxr_input.h, runtime/src/hle/input/pad.cpp,
new runtime/include/vr/openxr_item_throw.h (ported), runtime/include/vr/openxr_gamepad_rumble.h,
runtime/include/vr/mkw_vr_item.h (byte-identical copy of iChris4's ic HEAD), tests
vr_item_throw_tests.cpp (ported + KartFrame cases) and vr_gamepad_rumble_tests.cpp (both in ctest).

## Item throw (parity with iChris4/Wiicompiled_VR, GPL-3.0: ebc97ef, 4489d4a)

In the first-person cockpit, a quick swing of the item hand forward or back throws the item that
way: the stick is pushed fully forward or back, the item button pressed and released while it stays
pushed (0.05 s aim, 0.10 s press, 0.10 s release), then the stick is handed back. A swing counts when
the grip covers the travel along the seat's forward axis within the window, mostly along that axis
and still moving that way; the hand must not hold the wheel and must have been free 0.25 s; 0.6 s
cooldown. A swing throws only with an item in the hand (HeldItem valid, count > 0) or items already
out (tethered).

The one real adaptation: ic plays the sequence on hands[0] (the Wii Remote / virtual SDL gamepad).
KartFrame's default controller mode is "gamepad", where pad.cpp maps the published QuestInput over
GameCube port 0 (MapQuestInput), so hands[0] never reaches the game. The sequence is played on that
port after frame::Map (steering_y = +-1, item = 1, the GameCube's L; the trick field with
swap_item_trick). A held wheel only replaces stick X, so steering continues. Wii Remote mode keeps
ic's hands[0] path.

Settings (env wins over Config.toml [vr]; read once when the controllers attach):

| env | [vr] key | default |
|---|---|---|
| KARTFRAME_THROW=0/1 | cockpit_item_throw | true |
| KARTFRAME_THROW_HAND=left/right/off | cockpit_item_hand | left |
| KARTFRAME_THROW_FORWARD_CM (5..60) | item_throw_forward_cm | 18 |
| KARTFRAME_THROW_BACK_CM (5..60) | item_throw_back_cm | 15 |
| KARTFRAME_THROW_WINDOW_MS (40..250) | item_throw_window_ms | 120 |

KARTFRAME_THROW_INJECT=<start s>/<every s>[/f|b|a] (tests) plays throws without a swing or the item
check. Log: one "item throw: ..." line with the settings, "Held item thrown forward (controller, 21 cm
in 110 ms) on the port" per throw, "Swing forward ignored: nothing to throw (...)" per swing without an
item. A 40 ms pulse at 0.5 on the item hand confirms a throw (off with the wheel's haptics setting).

### The held-item read and lane AH
mkw_vr_item.h is the same file lane AH copies (same SHA-256, so the merge is clean). AH implements
MkwVRFirstPersonGetHeldItem in mkw_vr_first_person.cpp. Until that lands, openxr_input.cpp has a weak
fallback that reports no item (block marked LANE-AH-STUB): a real swing then logs "ignored: nothing to
throw". It does not conflict at merge: the two files are in different unity groups (6 and 0), and the
linker takes AH's strong definition over the weak one. Delete the block after the merge. The item hand
setting is read from [vr] cockpit_item_hand (ic's key); AH should draw the item in the hand that key
names.

## GameCube rumble in gamepad mode (new; neither upstream has it)

Before: PAD__ControlMotor_HLE -> PADControlMotor -> SDL_RumbleGamepad on the virtual joystick, which
has no rumble callback, while UpdateRumble (the OpenXR haptics) only followed WPADControlMotor
(Wii Remote mode). PADRead also did not advertise a motor on port 0 (the virtual pad reports none,
and statuses[0] is replaced by the VR controllers anyway). So in the default mode no collision or
item-hit rumble ever reached the Frame controllers.

Now: pad.cpp sets PAD_CHAN0_BIT in PADRead's rumble mask while the VR controllers own port 0, and hands
chan 0 motor commands to a bridge (a USB wheel that owns the port keeps them; [controller] rumble =
false still turns them into stops). The pacing thread samples the motor every XR frame and
gamepad_rumble::Shaper turns the game's on/off pattern into an amplitude: base x (0.4 + 0.6 x share of
the last 12 samples on, 100 ms at 120 Hz), 50 ms pulses re-sent while on, a stop once 100 ms passed
without it. Both controllers. KARTFRAME_RUMBLE=0/off disables it, 1/on is the default base 0.7, or a
base amplitude 0.05..1. Log: "gamepad rumble #n on, amplitude a" / "off" (first 20, then every 100th).

## Evidence

- kfbuild.sh (all targets, incl. WiiCompiled) rc=0; ctest 34/34 (the previous 32 plus
  mkw_vr_item_throw_tests and mkw_vr_gamepad_rumble_tests).
- No mock race was run (the owner's usage budget ran out). xrmock cannot script controller swings and
  does not log haptics, so the planned evidence was KARTFRAME_THROW_INJECT log lines and the
  "gamepad rumble #n" lines in a hudrace-style Grand Prix run. Neither has been run yet.

## Untested

- Real swing thresholds on the Frame's controllers (Quest-tuned defaults; tune with the log's cm/ms).
- That the game aims the throw from port 0 in a real race with an item (no item in the mock runs).
- Rumble feel and strength on the Frame's haptics (base 0.7, the duty mapping).
