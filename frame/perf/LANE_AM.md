# Lane AM: the Time Trial ghost still doubled (2026-10-28)

Owner, headset (session 2026-10-27 20:15): the ghost kart is still two flat copies. The log said 4x
`Time Trial ghost: composite of a depth copy left flat (its picture is not cleared before the composite)`: lane AE's
link (commit ed8931d) never engaged.

## The real pass layout (headset log, `Native render of an immersive frame` lines, every race frame of that session)

    [0 efb 939x528 draws 4 (3D 0)   -> copy fmt 6 47x32]
    [1 efb 939x528 draws 0          -> copy fmt 6 47x32]
    [2 efb 939x528 draws 21 (3D 21) -> copy fmt 22 (Z24X8) 892x456]   the ghost alone (16-21 draws, by character)
    [3 efb 939x528 draws 0          -> copy fmt 4 (RGB565) 892x456]   its colour
    [4 efb 939x528 draws ~160 (3D)  -> copy fmt 22 6x4]                the race; the composite is a 2D draw in here
    [5 efb 939x528 draws 50-80      -> copy fmt 6 893x456 display]     HUD
    [6 efb 939x528 draws 0]

The composite (the "VR large 2D rectangle" line just before the first "left flat"): 892x456 at 0,0, blend 1 (src
alpha / inv src alpha), material alpha 0.50, depth compare on (func 1) and depth update on, 2 TEV stages, 1 texgen:
the colour copy blended at half strength with its depth from the Z24X8 copy (z texture). So AE's model holds (ghost
alone, depth then colour copied out, race, one composite quad) except for the erase: the copies cover 892x456 of a
939x528 EFB target, so the clear that comes with the colour copy is not a whole-target attachment clear but a
scissored clear draw (`resolve_pass`: `clearFullTarget` false), which link_composite_source did not count. (If the
rectangle is in fact the whole target, the other way to miss the load op is the alpha: on the race's RGB8 EFB the copy
clears colour without alpha, and `useAttachmentColorClear` needs both; the new check reads the copy's own colour and
depth flags, so it covers either case.) Nothing
else in the frame needed AE's assumption; iChris4's fork (f2f7c50, e89988f) links by the depth copy alone, with no
clear check.

## Change (aurora common.cpp only)

- `RenderPass` remembers the clear of the GX copy it continues from (`copyClearColor`, `copyClearDepth`,
  `copyClearRect`, set in `resolve_pass` whether it became a load op or a scissored draw).
- `link_composite_source`: the picture counts as erased by a later EFB pass (not a display copy's reset) with an
  attachment clear of colour and depth, or with a copy clear of colour and depth whose rectangle covers the depth
  copy's rectangle. The rest is AE's: each eye skips pass 2's perspective draws where recorded and draws them at the
  composite with its own uniforms, the constant-blend pipeline siblings (KARTFRAME_GHOST_ALPHA, 0.5) and the eye's
  depth; KARTFRAME_GHOST_MONO=1 keeps the flat composite.
- `KARTFRAME_PASS_DUMP=<n>`: the first n frames with a depth-copy composite log one `Pass dump:` line per pass so far
  (target, attachment clears, the copy clear and its rectangle, first viewport, draws by kind 3D / 2D / efb-fx /
  other, every clear draw with its scissor, composites, the copy it ends with and its texture), at the composite.
- `frame/perf/ttghost.sh`: a Time Trial route for the mock (not working yet, see its header).

## Evidence and what is not verified

- Builds clean (0 warnings); ctest 37/37 pass; three mock route runs (menus, a VS race) and an 80 s mock Grand Prix
  (hudrace.sh) 0 errors, 0 fatal lines; no depth-copy composite there (no ghost), so no link or dump lines.
- The mock never reached a Time Trial with a ghost (3 route attempts, see ttghost.sh), so the pass dump, the link
  line and the per-eye images were NOT seen. The layout above comes from the headset log alone; the scissored-clear
  diagnosis follows from `resolve_pass` (copy rect 892x456 < target 939x528) and is the one condition AE's check
  rejected ("not cleared"), but it is inferred, not dumped.
- Not measured: disparity in eye dumps, the GPU cost (expected small or negative: each eye already drew the ghost's
  ~21 draws in pass 2 before the clear erased them, and now skips the full-eye composite quad instead), a 2-minute
  Time Trial soak.

## Headset check (next session)

session.env `KARTFRAME_PASS_DUMP=2`; a Time Trial against a ghost. The log must say `Time Trial ghost: pass 2 (N
perspective draws, 0 other; copies in passes 2..2) is drawn by each eye at the composite in pass 4 (alpha 0.50)`
and no `left flat`; the `Pass dump:` lines should show pass 4 (or 3) with `copy-clear c1 d1 0,0 892x456` and a
`[clear#0 copy scissor ...]`. In the headset: one translucent ghost at its true depth, hidden by walls and hills.
If it says `left flat (<reason>)` instead, the dump lines say which part of the layout differs.

## Risks

- A copy clear of colour without alpha counts as erasing (Mario Kart Wii's EFB is RGB8 in races); the ghost's alpha
  left behind is not used by an eye.
- The eye's copy clear is scissored to the copy rectangle mapped into the eye; if a ghost draw reached outside it, the
  native picture kept those pixels and the eye now drops them (none expected: the copy is the full picture).
- Lane AE's other limits stand (flat fallback while a sibling pipeline compiles; 4096 remembered eye configs).
