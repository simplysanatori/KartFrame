# KartFrame development notes (lane 0)

## Build

    frame/build-dawn.sh                      # once: patched static Dawn into ~/.cache/kartframe/dawn/<rev>-<patch hash>/
    ~/Games/KartFrame/kfbuild.sh <worktree> WiiCompiled      # the game, binary build-linux/kartframe
    KF_CONFIGURE_ONLY=1 ~/Games/KartFrame/kfbuild.sh <worktree>   # configure only (no lock)

`kfbuild.sh` takes the shared build lock, uses ccache (~/.cache/ccache-kf), links the worktree's
`generated` to the development translation (`~/Games/KartFrame/data/generated-dev`, never committed),
and passes the newest Dawn package (or `KARTFRAME_DAWN_DIR`) as `-DMKW_DAWN_PACKAGE_DIR`. The project
only accepts `CMAKE_BUILD_TYPE=Release`. The translated shards are the heavy part;
`MKW_TRANSLATED_COMPILE_JOBS` (default 3 in kfbuild) caps how many compile at once.

The CMake wiring for Linux: OpenXR on by default, pinned Vulkan headers and the pinned static OpenXR
loader (vendored jsoncpp) are fetched, the patched Dawn package is linked statically (its
`AuroraDawnVulkan*` ABI resolves at link time), SDL3 is built from source (audio: ALSA, JACK, PipeWire,
PulseAudio loaded at run time, plus dummy; video: Wayland, X11, KMS/DRM, offscreen, dummy). The shard
manifest and the blob assembly of a translation made elsewhere are relocated at configure time.

## Where the runtime looks for things

Executable `kartframe` (target `WiiCompiled`). Its resources (`dsp_coef.bin`, `initial_pipeline_cache.db`,
`wii_bootstrap/`, `resources/`) are copied next to it in the build directory and are found next to the
executable.

User state, from `runtime_config.h`:

- Normal install: `$XDG_DATA_HOME/WiiCompiled` (default `~/.local/share/WiiCompiled`), holding
  `Config.toml`, `NAND/`, `Cache/`, `Logs/`.
- Portable: a file `portable.txt` in the executable's directory or up to a few parents makes
  `<that dir>/UserData` the root instead.
- `Config.toml` is created with defaults on first start. The game files are not found automatically:
  `[paths] dvd_root` must name the extracted DATA directory (the one holding `sys/fst.bin` and
  `files/`); a relative value is relative to the Config.toml. `[paths] nand_root` is optional; when it
  is absent the managed NAND is `<state dir>/NAND` (the licence is made in-game on first boot).
- `[vr] enabled = true, required = false` (defaults) try OpenXR and fall back to the desktop renderer.
  The OpenXR loader reads `~/.config/openxr/1/active_runtime.json` (SteamVR) unless `XR_RUNTIME_JSON`
  is set. `[vr] force_steamvr` does nothing on Linux.
- The program takes no command line arguments.

## Tools and runs

`frame/tools/mockrun.sh [--flat] [--seconds N]` runs the game against the mock runtime (xrmock) or flat,
headless: a private Xvfb for the window (Dawn needs a window surface to pick the adapter),
`SDL_AUDIO_DRIVER=dummy`, and `KARTFRAME_HEADLESS=1`, which makes aurora skip configuring and
presenting the desktop swapchain (it renders into an offscreen texture and logs a frame count). The
scratch user state is `~/.cache/kf-mockrun/data/WiiCompiled`. It aborts when the mock runtime file is
missing or empty and when a game is already running. Never run the game with the real runtime from an
agent. Known facts: Valve's implicit OpenXR layer `XrApiLayer_VALVE_fdm_injection` is loaded in front of
the mock; the mock offers no `XR_KHR_convert_timespec_time` yet, so the game logs "no clock
conversion"; abseil, zlib, libpng and freetype are built static from pinned sources (AURORA_VENDOR_THIRD_PARTY); the binary needs only glibc, libstdc++ and libgcc_s.

## Pacing knobs (lane A)
Config `[vr] frame_pacing = auto|inline|pipelined` (auto = pipelined, measured) and `refresh_rate`
(120; requested in-app only when SteamVR offers exactly that rate, else the wrapper's per-app key
decides). Menus are a quad layer fixed in the room (menu_shader_quality 0), the first-run guide is
projected. Env: KARTFRAME_PACING overrides the mode; KARTFRAME_REFRESH_HZ the rate;
KARTFRAME_PACING_MARGIN_US (1000) is kept free before the deadline for release + xrEndFrame;
every refresh always by default (a late set repeats the retained layer, which SteamVR reprojects;
half rate is 60 Hz content shown twice, the judder H1 rejected); KARTFRAME_HALF_RATE=1 always every
second refresh (measurements), =auto drops after more than 5 % late over 30 s and stays there for
the session; KARTFRAME_PACING_DEBUG=1 per-set timelines;
KARTFRAME_NO_GPU_WAIT=1 negative test only. KARTFRAME_RELEASE=idle (lane U, headset A/B) drains Dawn's
queue (vkQueueWaitIdle under the queue guard) right before each set's releases and its xrEndFrame, so
SteamVR's client submissions there never wait behind our next set's GPU work; logs `release idle
drain` every 10 s (frame/perf/LANE_U.md). The wrapper sets TU_DEBUG=sysmem unless
KARTFRAME_SYSMEM=0 (the race holds 120 Hz at 1728 only with it). Log line per second:
`vr pacing: t= hz= mode= ticks= rendered= interpolated= late= missed= repeats= gpu_ms=p50/p99 encode_ms=p50/p99`
(format in runtime/include/vr/openxr_diagnostics.h); session-report.py reads it with vrtiming.log.
Mock: MOCK_XR_BUSY_MS (exact released-unfinished check; lane U: plus the strict check, bytes at the
release against the next acquire, and `release queue scN` lines: how long a submission made inside
xrReleaseSwapchainImage waits behind the app's queued GPU work, per swapchain and frame parity), MOCK_XR_FOCUS_BLIP=<ms>[@<s>],
MOCK_XR_PROFILE, MOCK_XR_EYE, MOCK_XR_TIME_OFFSET_S, MOCK_XR_DUMP=<dir> (released images as PPM at
MOCK_XR_DUMP_AT seconds); mockrun seeds welcome/tutorial done so the demo race starts by itself
(KF_MOCKRUN_INTRO=1 shows them). KARTFRAME_FPSLOG=1 logs the game frame rate every 5 s;
KARTFRAME_SYNC_WINDOW=1 restores aurora's per-frame SDL_SyncWindow (10 ms a game frame on X11);
KARTFRAME_HUD_CLIP=0 stops clipping the race's 2D layer to the frame and the game's scissor.

## Lane G switches
KARTFRAME_SCENE_CLOCK=0 restores the old interpolation rule (scene sampled at the display time;
with pipelined pacing that pinned the weight to the newest frame). KARTFRAME_END_TIME=cycle ends
layers at the cycle's predicted time instead of their poses' locate time. KARTFRAME_MENU_ENV=over
puts the menu environment layer (controllers, lasers) over the screen quad; =0 turns it off.
KARTFRAME_FRAME_STAMP=1 (tests) stamps each eye image with its locate time. Mock: MOCK_XR_STAMP,
MOCK_XR_PREDICT_JITTER, MOCK_XR_FOV (default Frame-like asymmetric), MOCK_XR_HANDS, MOCK_XR_PRESS
(scripted presses; frame/perf/hudrace.sh drives a real Grand Prix race). Log lines: "vr display
times" and "VR interpolation weights" every 10 s, eye fields of view once. Details in frame/perf/H2.md.

## Lane H switches
KARTFRAME_END_TIME=locate (lane G's rule; default now: each frame's own predicted time).
KARTFRAME_THREADPRIO=high|nice|off (game, pacing, frame worker, GX and GPU watcher threads at up to
nice -15 through RealtimeKit), KARTFRAME_UCLAMP=0 (no utilization clamp; by default the game thread
runs on the big core). KARTFRAME_FULL_VIEW_FADES=0 keeps full-frame fades on the screen (default:
across the whole eye; about 0.3 ms GPU a set). KARTFRAME_COPY_TRIM=0 keeps the minimap copy's last
columns (default trims them: removes the thin line by the map). KARTFRAME_MENU_ENV defaults to
over (controllers and laser over the screen quad, alpha blended); =under is lane G's order.
KARTFRAME_LOG_2D_QUADS logs large 2D rectangles (to find the Blooper ink later). Mock: MOCK_XR_LEAD
(display lead in refreshes, default 3 like SteamVR), MOCK_XR_DUMP_STEP, MOCK_XR_DUMP_ALPHA,
MOCK_XR_HANDS=high. Details in frame/perf/H3.md.

## Lane I switches
KARTFRAME_SINK=srgb|copy|blit: how eyes reach the swapchain (default srgb: an sRGB-decoding draw
through an sRGB view, so the swapchain needs no MUTABLE_FORMAT and the driver can compress it; copy is
the old path). KARTFRAME_DIRECT_EYES=0|1 forces rendering straight into the swapchain images (by default
measured per session; never applies with the srgb sink). KARTFRAME_MONO_FULL=1 and
KARTFRAME_NATIVE_CLIP=0 restore the full native race render (by default it stops after the last copy
the eyes sample and is clipped to it; the desktop window is black during races). KARTFRAME_FPSLOG=2
logs a per-segment GPU profile (frame/perf/gpuprof.py tabulates it). Mock: MOCK_XR_MUTABLE=0.
Details and the profile in frame/perf/RACE120.md; A/B pairs with frame/perf/abgp.sh.

## Lane J switches
KARTFRAME_EYE_PAIRS=2..8 (lane J set 6; back to 2 for the old loop, 3 for sets ahead): eye swapchain pairs the per-refresh sets rotate through; a pair is
acquired again only after the frames that showed it are well past SteamVR's composition (the sixth
session's recording showed one eye at a time re-showing an image 3 to 4 refreshes old with two pairs).
KARTFRAME_FRAME_LOG=1 writes one XRF line per frame (wake-up, end after wake-up, pair, image per eye,
display step). Mock: MOCK_XR_COMPOSITE_MS and MOCK_XR_COMPOSITE_SELECT=spec|lazyrel|lazyacq model when
SteamVR picks each eye's image. frame/perf/panels/analyse.sh SESSION_DIR analyses a KARTFRAME_RECORD=raw
recording per eye (stale and held refreshes, eyes out of step) in about 4 minutes; see frame/perf/PANELS.md.
The wrapper takes one-launch settings from KARTFRAME_HOME/session.queue/*.env in name order when no
session.env is waiting.

## Lane K switches
The default `pipelined` loop renders each set two refreshes ahead and ends every frame at SteamVR's
wake-up with the newest finished set (else the retained layer), never waiting inside the cycle.
KARTFRAME_PIPELINED=wait restores the old one-set-at-a-time loop. KARTFRAME_EYE_PAIRS defaults to 3 with
sets ahead (3 to 8) and 2 with wait. KARTFRAME_AHEAD_START_US delays each set's start after the wake-up.
KARTFRAME_END_DELAY_US (test only, default 0, max 8000) sleeps that long after the wake-up before the frame is ended (same
set; the next set starts at the wake-up, before the sleep); the XRF e= field and end_late= then show the lateness by design.
The pacing line ends with dropped= (finished sets released unshown) and end_late= (ends more than 1 ms
after the wake-up). Mock: MOCK_XR_LATE_END_MS (1.5) models late ends; KF_MOCKRUN_END_GATE=<percent>
fails a run with more late ends. Details: frame/perf/H7.md.

## Lane L and M switches
KARTFRAME_FRAME_LOG=1 now also writes an XRS line per set (the scene state each eye's draws used:
weight, game-frame pair, draw counts; the mock compares the eyes) and an XRE line per locate (each
eye's offset from the head and its change since the last locate), plus a per-second "vr eyes" summary.
KARTFRAME_EYE_OFFSETS=located|fixed (default located): fixed renders and submits each eye at the head
pose with the median eye-to-head offset and field of view of the first 120 focused locates, so
per-refresh eye motion from the runtime cannot reach the images. KARTFRAME_AHEAD_REFRESHES=1|2
(default 2): locate sets one refresh ahead (sets then do not fit a refresh in races: about half late).
Session plan: frame/perf/session-h9/README.md.

## Lane O switches (the hand-over GalaxyFrame's way)
KARTFRAME_XR_FORMAT=rgba|bgra (default rgba: eye swapchains VkFormat 43, the sRGB draw swizzles),
KARTFRAME_XR_USAGE=galaxy|kart (default galaxy: COLOR_ATTACHMENT | SAMPLED), KARTFRAME_END_LOCATE=1|0
(default 1: one more locate for the frame's own time before ending it, race frames), and
KARTFRAME_XR_EXTENSIONS=galaxy (GalaxyFrame's extension list: drops hand steering, other headsets'
profiles and the clock conversion; default stays KartFrame's). Mock: MOCK_XR_TRACE_DETAIL=1.
Comparison and remaining candidates in frame/perf/H9.md.

## Lane P instrumentation
With KARTFRAME_FRAME_LOG=1 the XRS line carries XRP fields (per eye: the packet used, the pose
written, the frustum, draws filled from another packet; same=1 when the eyes agree), "VR eye poses
(10 s)" and "VR eye uniform probe (10 s)" summaries (KARTFRAME_UNIFORM_PROBE=1 for the probe alone);
the stamp is a 64x15 one-pixel-per-bit block; the mock reads back and decodes before its xrWaitFrame
sleep so end-after-wake-up is not inflated. In the mock both eyes always used the same packet and pose.

## Lane Q switches
KARTFRAME_RACE_QUAD=1|cycle|0 (default 1: a transparent 16x16 quad listed after every projection-only
layer; cycle also acquires and releases one of its images every frame), KARTFRAME_FIRST_WRITE_BARRIER=
all|dawn (default all: GalaxyFrame's ALL_COMMANDS / MEMORY_WRITE to MEMORY_READ|WRITE barrier on every
image of a set before its writes, submitted by the bridge). XRF lines end in n=<layers>; xrmock prints
"layer lists per xrEndFrame". Plan: frame/perf/session-h11/README.md.

## Lane R switches (the pose check, and the two levers)
GalaxyFrame's pose check (notePoses) runs at every xrEndFrame with located views: lane O's end locate
(one xrLocateViews for the frame's own display time, in the layer's space, after the release) is now
that check's locate, made in race and menu frames alike (lane O: race only); KARTFRAME_END_LOCATE=0
still leaves it out. Every 5 s, and when the scene changes, in GalaxyFrame's words plus the scene and
the locate's own cost: `vr: views of N sets: M for the display time they went out at (off X ms avg, Y
max); against a locate then: left eye A mm B deg avg, C mm D deg max; right eye ...; eye distance
E-F mm; race|menus, S s; that locate G ms avg H max` (frames that show a new set, each once, as
GalaxyFrame counts them). `vr: poses predicted ahead (N sets, S s, race|menus): display time minus the
set's locate p50 X ms, max Y` (every set, at its start; sets ahead: two refreshes plus SteamVR's lead).
XRF lines end in aL=/aR= (degrees between each eye's rendered pose and the check's locate, every frame;
-1 not checked). session-report.py shows both per scene. KARTFRAME_AHEAD_REFRESHES=1 now starts the
next set at the wake-up before the frame is ended (GalaxyFrame's order) without waiting for Aurora
(a set still being encoded: the start waits after the end as before), with 4 eye pairs by default.
KARTFRAME_SUBMIT_POSE=fresh|rendered (default rendered): fresh submits race frames that show a new set
with the check's poses (the images stay as rendered; repeats of the retained layer keep their own).
Mock: MOCK_XR_STAMP=fresh checks such layers (poses from a locate for their own display time; each
image against the locate of its own stamp: "fresh-pose stamps"). Plan: frame/perf/session-h12/README.md.

## Lane S switches (mock only)
MOCK_XR_SWAY=<deg>,<period s>[,<from s>] sizes the head's sway (default 17.19,2) and holds the head still
until <from>. MOCK_XR_PRESS=<path>@<start>/<every>/<hold s>/<until s> holds a press longer (1000/1000:
held from <start> on, e.g. A to drive) and stops it at <until>. MOCK_XR_DUMP_QUEUE=<slots> dumps eye
images without stalling (blitted down to 1/MOCK_XR_DUMP_STEP, written when the dumps pause for 0.3 s);
every MOCK_XR_DUMP writes <dir>/index.txt (file, swapchain, frame, and with MOCK_XR_STAMP the eye, the
display time and the scene words). frame/perf/mockdump/dumprace.sh NAME records 240-set windows of a race
(head still, standing with the sway, driving); frame/perf/mockdump/zigzag.py DIR applies H7's zigzag
rule, a betweenness test and a lagdiff per eye (ZZ_INJECT=2: positive control, ZZ_TRACK=1: positions).
Results: frame/perf/LANE_S.md; next: frame/perf/session-h13/README.md.

## Lane T (session 13: the recorded race images through xrprobe)
`frame/tools/xrprobe/xrprobe.c --replay DIR` plays a dumprace.sh recording's eye images at their recorded
poses and fov (one set per refresh by display time; `--replay-window standing|driving`, `--replay-offset`,
`--replay-pairs 3` KartFrame's sets ahead, `--replay-quad` the race quad, `--replay-hold 2`). Build:
`cmake -S frame/tools/xrprobe -B ~/.cache/kf-xrprobe -G Ninja -DCMAKE_C_COMPILER=clang` (OpenXR headers from
GalaxyFrame's tree, or -DOPENXR_INCLUDE=...), then the build under the KartFrame lock. `replay-case.sh N`
runs case N (1..6); `frame/perf/session-h13/queue.sh BINARY RECORDING` queues the six as KartFrame launches
(KARTFRAME_EXE in a session.queue file, recorded). Headless: `PROBE_MOCK=1 PROBE_SET=replay run-probes.sh`
(the mock runtime, detailed trace per case), then `frame/perf/session-h13/replaycheck.py DIR`.

## Lane W (the compositor's phase beside the app's timeline)
KARTFRAME_VRTIMING_FRAMES=1 (wrapper session.env) makes frame/tools/vrtiming.py also write vrframes.log in the
session folder: one F line per SteamVR compositor frame (index, m_flSystemTimeInSeconds, presents, mispresented,
dropped, reprojection flags, then waitgetposes / posesready / frameready / update start / update end / render
start in ms after that frame's system time, compositor cpu and gpu ms), a P line per poll (CLOCK_MONOTONIC,
_RAW, BOOTTIME read around IVRSystem::GetTimeSinceLastVsync, its counter, the newest frame), G lines for frames
missed between polls (polls every 0.25 s, 128 frames kept by SteamVR: none expected). About 45 MB an hour.
XRF lines now end in m=<wake-up, CLOCK_MONOTONIC ms> r=<release after the wake-up, ms, -1 none> t=<predicted
display time, CLOCK_MONOTONIC ms via XR_KHR_convert_timespec_time>; the end is m + e. One "XRF clock:" line
says how t= was converted. `python3 -I frame/tools/phase.py SESSION_DIR...` prints, per launch, (A) SteamVR's
own app timings against its vsync reference (frame ready, poses ready, the margin to the compositor's update
start) and (B) the app's wake-up, end, release and display time minus the nearest compositor anchor (vsync,
update start, render start), with histograms in 0.25 ms bins and a drift table (--every S, --all, --hist all).
Headless check of the logger: a stub libopenvr_api.so via KARTFRAME_STEAMVR_DIR (frame/perf/LANE_W.md).
The lock design and its risks: frame/perf/LANE_W.md; next session: frame/perf/session-h19/README.md.

## Lane X (the end locked to the compositor's grid, test only)
KARTFRAME_END_PHASE=after[:us]|before[:us] (default off; in the wrapper's session.env, which also reaches
frame/tools/vrtiming.py) times each frame's end against SteamVR's compositor instead of the wake-up: the helper polls
every 0.1 s and publishes the newest vsync (CLOCK_MONOTONIC), the period and the update window's start p5/p50 and end
p50/p95 as a 64-byte record (atomic rename) at KARTFRAME_PHASE_FILE (default $XDG_RUNTIME_DIR/kartframe-phase.bin);
the pacing thread re-reads it at most every 50 ms (about 1 us a read) and ends at vsync + update end p95 + us (after,
300) or vsync + update start p50 - us (before, 1500), bounded by the cycle's deadline, keeping lane V's order (next
set started at the wake-up, 4 pairs). No fresh grid (missing, older than 0.5 s, period 1 % off): KARTFRAME_END_DELAY_US,
or 4000 us. Log lines "end phase:" (mode, every switch, the achieved phase every 10 s). phase.py view C classifies
every end against its refresh's update window (before / inside / after, SteamVR's presents per class) and view D
prints the game's lines. Mock: mockrun.sh runs the helper against frame/tools/openvr-stub.c (STUB_UPDATE_MS,
KF_MOCKRUN_STUB_UPDATE_MS, KF_MOCKRUN_PHASE_SECONDS) with MOCK_XR_ALIGN_US. Analysis: frame/perf/LANE_X.md; next
session: frame/perf/session-h20/README.md.

## Wrapper recorder (KARTFRAME_RECORD)
The recorder starts 8 s after the game and only if the game is still alive then; it runs in its own
session (setsid) and the wrapper stops it on every exit path (game exit, Steam's TERM/INT/HUP): SIGINT
and up to 30 s for the file, then TERM, then KILL, then a pkill on anything writing into the session's
panels.mkv. The log says "recorder stopped" or "recorder was never started"; a leftover
panels.mkv.part is logged with its size and left for the owner. frame/test-wrapper-recorder.sh checks
the four paths with a fake game and a fake framecorder (about 45 s).

## A/B tool (lane AD)
`frame/tools/abcompare.sh [--env-a K=V] [--env-b K=V] [--env K=V] A B` runs two builds (worktree, build dir or
kartframe binary) through frame/perf/run.sh and mockrun.sh (shared lock, headset and running-game checks), same
scripted Grand Prix (A presses until `--menu-until` 43 s, head still, hands fixed, kart standing), 100 s each, then
`abcompare.py` prints metric | A | B | delta: errors, sets/s, late %, GPU ms per set p50/p95 (race seconds outside the
dump windows, frame/perf/pacing-parts.py's rules), and the eye images: stamped dumps (MOCK_XR_STAMP, MOCK_XR_DUMP_QUEUE,
ten 1-2 s windows) paired by eye and scene position (the stamp's game frames since "entered RaceScene", plus the offset
at which the pictures best match: the race starts differ by a frame or two), per pair the share of pixels off by more than
`--tol` 2/255 and PSNR, the stamp corner left out, worst pair in worst.png (A | B | 16x diff). PASS needs 0 errors,
mean pair diff < `--max-diff-pct` 0.5, worst pair < `--max-worst-pct` 5, >= 20 pairs; `--gpu-delta-max MS` and
`--late-delta-max PCT` check a timing claim; `--rounds N` interleaves A B A B; `--keep` keeps the dumps (400 MB a side).
Limits: the mock cannot make the race frame-exact. Menu loading varies by multiples of 1.5 s (GO at about 44 s or 52 s),
the stamp's interpolation weights differ per run (hence the nearest-position pairing, +-0.05 frames), AI karts and the
kart's first coasting are only nearly repeatable, so moving scenes carry a noise floor: run `abcompare.sh X X` first and
read a change against it. GPU/pacing deltas of a few percent need `--rounds 2` or more (clock and thermals drift).
