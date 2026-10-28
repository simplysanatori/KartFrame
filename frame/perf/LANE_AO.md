# Lane AO: the stutter in the ghost Time Trial (session 26, 2026-10-28)

Owner, headset (session `20261028-021654`, Time Trial against a ghost, defaults plus `KARTFRAME_PASS_DUMP=2
KARTFRAME_FRAME_LOG=1`): the ghost looked right, but the race stuttered every 2-5 s and felt slowed; the 2D menus
before it were slow too. Analysis: `session-h26/RESULTS.md`.

## Cause: other builds on the game's CPUs (c)

- Container builds of another project (DXVK and a mingw proxy) compiled through the whole launch on the same CPUs
  (user processes here run on CPUs 3-7, 5 CPUs). Journal: compile lines every minute 02:07-02:19.
- 97 % of the slow game frames (1074 of 1109 `hitch:` lines) waited 5 ms or more for a CPU, median 10-12 ms
  (`runq_ms`); session 25: 0.2-0.6 % of them, median 0.2-0.7 ms. Run-queue wait per second tracks late+repeats
  (r 0.66-0.69).
- The menus show the same before any ghost exists (221 ms/s of run-queue wait against 6-7 in session 25's menus).
- The machine started at 95 C (session 25: 68-78 C) and the big cores ran 15 % slower (2502 against 2950 MHz mean).
- Not (a), the per-eye ghost: its pipelines are resolved once a frame at the link, a set re-issues its ~21 recorded
  draws per eye with existing uniforms and skips the same draws where recorded and the composite quad; race encode
  and GPU medians are session 25's. Not (b), the logging: `FRAME_LOG` was on in session 25's smooth launches with more
  lines a second; `PASS_DUMP=2` wrote 12 lines.

## Change (frame/kartframe.sh only; no game code)

- At the launch, `launch.log` gets `WARNING: build processes running now (cc1plus x3, ninja x1, ...)` when compilers
  or build tools run anywhere on the machine (containers included).
- `thermal.tsv` has four more columns each second: busy CPUs of the wrapper's own CPUs (its affinity, e.g. 3-7),
  the game's part (process `kartframe`, found once it runs; utime+stime of all its threads), other processes' part,
  and the machine's runnable threads (`procs_running`). Shell builtins only, as before.
- `report.txt` ends with `machine (thermal.tsv, N s): other processes on the game's CPUs mean / max, seconds with
  1 or more; game CPUs; runnable threads; hottest zone mean / max`, and a WARNING line when other processes used a
  CPU or more on average.
- The per-eye ghost needed no change (see above).

## Verification

- `frame/test-wrapper-recorder.sh` (scratch dir): all passed; its sessions show the new columns and the report line.
- The sampler alone with a fake `kartframe` (a copy of `yes`) and one more busy loop: game 0.95-0.98 CPUs, other 2.3
  (the loop plus another project's build that was running), runnable 4-10; the build warning named a running
  `ninja` and `cc1plus`.
- ctest 37/37 pass (`kfbuild.sh` build of this branch, 0 warnings in the build log).
- The game binary is unchanged: this branch's `kartframe` and the `native` build (bc8196f) have byte-identical
  `.text` (md5 of the section), so a game regression is impossible; the mock runs check the tooling and show the
  machine's state.
- `hudrace.sh` Grand Prix, 90 s, at the defaults (GX thread, narrow lock, 90 Hz, packed, end phase after, interp 33,
  `MOCK_XR_BUSY_MS=1`), A = native build, B = this branch, interleaved; race = 12 s after the race presentation to
  the end minus 3 s. All 7 runs: mock 0 errors, 0 fatal lines. Other agents' work was running on the machine in
  some of them (A3-A4, B3 sampled with the new thermal.tsv columns: other processes 2.0-2.5 CPUs):

  | run | sets/s | late / repeats | GPU ms/set p50 / p99 | encode p50 / p99 | hitches | runq p50 ms (hitch frames) | other CPUs |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | A1 | 86.0 | 1 / 4 | 4.42 / 4.79 | 2.13 / 2.66 | 16 | 0.2 | - |
  | B1 | 77.6 | 75 / 63 | 4.53 / 4.91 | 2.19 / 2.81 | 189 | 29.8 | - |
  | A2 | 87.6 | 1 / 3 | 4.73 / 7.09 | 2.10 / 2.97 | 13 | 0.5 | - |
  | B2 | 87.4 | 0 / 2 | 4.85 / 5.89 | 2.62 / 3.65 | 70 | 11.2 | - |
  | A3 | 87.2 | 1 / 2 | 4.71 / 6.18 | 2.31 / 3.21 | 29 | 5.6 | 2.40 |
  | B3 | 80.3 | 45 / 26 | 4.64 / 5.03 | 2.23 / 2.75 | 90 | 16.8 | 2.02 |
  | A4 | 81.6 | 35 / 42 | 4.52 / 4.97 | 2.21 / 2.87 | 248 | 23.6 | 2.48 |

  GPU and encode are the same in every run (identical code); late sets and repeats follow the run-queue wait and
  hit either binary (A4 is the old one): the session-26 effect, reproduced by accident in the mock. With the machine
  quiet (A1, A2, B2) both binaries are at 0-1 late.

## Headset check (next session)

One Time Trial against a ghost with no extra switches, nothing else building (the machine idle and below about 80 C
at the launch). Expected: smooth, 0 late sets in the race, the ghost still one kart at the right depth;
`launch.log` without the build warning; `report.txt`'s `machine` line with other processes near 0 CPUs. If it
stutters with the machine idle, the `machine` line and the hitch lines' `runq_ms` say whether it is still CPU
contention (then by what: SteamVR's own processes count as "other" too) or the game.

## For the orchestrator

Other projects' builds (the container builds above) did not wait for `~/.cache/headset-in-use`; KartFrame's
`kfbuild.sh` does. Their build scripts should do the same (wait while the file exists and its pid is alive), or the
owner's sessions will keep being measured under load.
