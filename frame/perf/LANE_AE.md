# Lane AE: the Time Trial ghost in stereo (2026-10-27)

Owner, headset: the Time Trial ghost appears doubled, one copy left and one right. Cause: Mario Kart Wii draws the
ghost alone into the cleared EFB, copies colour and depth out, clears, draws the race and blends the copies back with
one orthographic quad (depth from the depth copy). That quad is a native framebuffer effect, replayed as recorded:
the same flat picture at the same place in both eyes (no disparity).

Change (commit ed8931d, approach from iChris4/Wiicompiled_VR f2f7c50, 7f9de4c, e89988f): the quad's draw records the
fresh full-size depth copy it samples (shader_info); as it is recorded, the pass that drew the copied picture is linked
(common.cpp link_composite_source, only if that picture is cleared in colour and depth before the composite); each eye
draws that pass's perspective draws in the quad's place with its own uniforms, constant-blend siblings of their eye
pipelines (stereoStencil bit 2; configs remembered in gfx::pipeline_ref, up to 4096, about 2.8 KB each) and its own
depth. Falls back to the flat quad while a sibling pipeline is not ready. KARTFRAME_GHOST_MONO=1: old behaviour;
KARTFRAME_GHOST_ALPHA (0.5).

Files outside the composite path (lane AC overlap): gx.cpp build_pipeline (+10 lines, blend override for the bit),
pipeline.hpp valid_pipeline_config (one line: stereoStencil may be 3).

Verified: builds (kartframe links, 0 warnings in the build log). NOT verified (stopped for the owner's usage budget):
ctest, any mock run, whether the mock can reach a Time Trial with a staff ghost (route: Single Player, down to Time
Trials, character, kart, cup, course, ghost race; MOCK_XR_PRESS has left dpad_down/click), whether the link fires
("Time Trial ghost: pass N ... is drawn by each eye" in the log, or "... left flat (<reason>)"), eye disparity in
dumps, GPU cost.
