#!/usr/bin/env python3
# KartFrame lane W: where the app's frame end lands inside SteamVR's compositor frame, per launch.
#
#   python3 -I frame/tools/phase.py SESSION_DIR [SESSION_DIR ...] [--all] [--every S] [--hist all]
#
# Reads SESSION_DIR/vrframes.log (frame/tools/vrtiming.py with KARTFRAME_VRTIMING_FRAMES=1: one F line per
# compositor frame, a P line per poll) and SESSION_DIR/game.log (XRF lines, KARTFRAME_FRAME_LOG=1; the lane W
# fields m= wake-up and t= display time in CLOCK_MONOTONIC ms, r= release after the wake-up).
#
# Two views of the same question, printed per launch (race frames only unless --all):
#  A. SteamVR's own clock (no clock matching): per compositor frame, the app's frame-ready (its xrEndFrame
#     as SteamVR saw it) and poses-ready (its xrWaitFrame wake-up) relative to the frame's vsync reference
#     (m_flSystemTimeInSeconds), as SteamVR reports them (not folded into one refresh), and the margin from
#     frame-ready to the compositor's update start.
#  B. The app's clock: the XRF wake-up, end (m + e), release (m + r) and display time t, each minus the
#     nearest compositor anchor (vsync reference, update start, render start) mapped to CLOCK_MONOTONIC. The
#     offset sys -> CLOCK_MONOTONIC comes from the P lines (CLOCK_MONOTONIC read around GetTimeSinceLastVsync,
#     whose counter is checked against the F frame index); "end vs frame-ready" checks it (about 0 if right).
# All "minus the nearest anchor" values are signed, -period/2 .. +period/2: an end that lands close to an
# anchor shows up as a pile near 0; histograms in 0.25 ms bins. --every S: drift table every S seconds
# (circular mean of each phase, so a phase wrapping at +-period/2 averages correctly).
#  C (lane X). SteamVR's clock again: each app end against the compositor update window (start, end) of the
#     refresh it lands in, classified before / inside / after, with SteamVR's presents per class, the class
#     changes between consecutive refreshes, end minus update end, and the early-update-start episodes.
#  D (lane X). The game's KARTFRAME_END_PHASE lines: the lock's mode, fallbacks and achieved phase against
#     the grid the timing helper published.
import bisect, math, os, re, sys

BIN = 0.25

def median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else float('nan')

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))] if v else float('nan')

def wrap(x, period):
    return (x + period / 2) % period - period / 2

def cmean(v, period):
    if not v: return float('nan')
    s = sum(math.sin(2 * math.pi * x / period) for x in v)
    c = sum(math.cos(2 * math.pi * x / period) for x in v)
    return math.atan2(s, c) * period / (2 * math.pi)

def read_vrframes(path):
    F, P, gaps = {}, [], 0
    with open(path, errors='replace') as f:
        for line in f:
            t = line.split()
            if not t: continue
            if t[0] == 'F' and len(t) >= 20:
                F[int(t[1])] = dict(sys=float(t[2]), pres=int(t[3]), mis=int(t[4]), drop=int(t[5]), rp=int(t[6]),
                                    wgp=float(t[7]), pr=float(t[8]), fr=float(t[9]), us=float(t[10]),
                                    ue=float(t[11]), rs=float(t[12]))
            elif t[0] == 'P' and len(t) >= 9:
                P.append(dict(mono=float(t[1]), raw=float(t[2]), since=float(t[4]), counter=int(t[5]),
                              newest=int(t[7]), newest_sys=float(t[8])))
            elif t[0] == 'G':
                gaps += int(t[2]) - int(t[1]) + 1
    return F, P, gaps

XRF = re.compile(r'XRF c=(\d+) w=\S+ e=(-?[\d.]+) f=(\d) .* m=([\d.]+) r=(-?[\d.]+) t=([\d.]+)')

def read_game(path):
    rows, in_race, any_race = [], False, False
    with open(path, errors='replace') as f:
        for line in f:
            if 'entered RaceScene' in line or 'presentation=immersive-race' in line:
                in_race = any_race = True
            elif 'exited RaceScene' in line or 'presentation=virtual-screen' in line or 'presentation=desktop' in line:
                in_race = False
            m = XRF.search(line)
            if m:
                c, e, fresh, wake, r, t = m.groups()
                rows.append(dict(c=int(c), wake=float(wake), end=float(wake) + float(e), fresh=fresh == '1',
                                 rel=float(wake) + float(r) if float(r) >= 0 else None, disp=float(t), race=in_race))
    return rows, any_race

def clock_offset(F, P, period):
    """CLOCK_MONOTONIC (s) minus SteamVR's m_flSystemTimeInSeconds at each poll; how it was found.
    Lane X: SteamVR's system time runs on CLOCK_MONOTONIC_RAW (vsync_raw minus the frame's system time is
    constant to +-10 us, while CLOCK_MONOTONIC drifts 14-33 ppm against _RAW, about 1 ms a minute), so the
    offset is (mono - raw) at each poll plus one constant: the vsync in _RAW minus the frame's system time,
    folded to one refresh."""
    raw = [p for p in P if p['since'] >= 0 and p.get('raw', 0) > 0]
    if len(raw) >= 3:
        k = [(p['mono'] - p['since']) - (p['mono'] - p['raw']) - p['newest_sys'] for p in raw]
        c = median(k)
        k = median([x - round((x - c) / period) * period for x in k])
        return [(p['mono'], p['mono'] - p['raw'] + k) for p in raw], 'system time = CLOCK_MONOTONIC_RAW (vsync folded)'
    direct, folded = [], []
    for p in P:
        if p['since'] < 0: continue
        vs = p['mono'] - p['since']
        if p['counter'] in F: direct.append((p['mono'], vs - F[p['counter']]['sys']))
        folded.append((p['mono'], vs - p['newest_sys']))
    if len(direct) >= max(3, len(folded) // 2):
        return direct, 'vsync counter = frame index (exact)'
    if folded:
        # The counter is not the frame index: the offset is known modulo one refresh, which is all a phase needs.
        c = median([o for _, o in folded])
        return [(m, o - round((o - c) / period) * period) for m, o in folded], 'vsync, modulo one refresh'
    coarse = [(p['mono'], p['mono'] - p['newest_sys']) for p in P]
    return coarse, 'NO VSYNC ANCHOR: poll time minus the newest frame (coarse, up to a refresh off)'

def offset_at(offs, mono):
    i = bisect.bisect_left(offs, (mono,))
    lo, hi = max(0, i - 4), min(len(offs), i + 5)
    return median([o for _, o in offs[lo:hi]])

def hist(name, vals, period):
    if not vals: return
    lo = math.floor(min(vals) / BIN) * BIN
    counts = {}
    for v in vals: counts[math.floor(v / BIN)] = counts.get(math.floor(v / BIN), 0) + 1
    top = max(counts.values())
    print('  %s (n %d), 0.25 ms bins:' % (name, len(vals)))
    keys = sorted(counts)
    prev = None
    for k in keys:
        if prev is not None and k > prev + 1: print('     ...')
        print('    %+6.2f %6d %s' % (k * BIN, counts[k], '#' * max(1, round(40 * counts[k] / top))))
        prev = k

def stats(name, v):
    if v: print('  %-44s p5 %+6.2f  p50 %+6.2f  p95 %+6.2f  (n %d)' % (name, pct(v, 5), pct(v, 50), pct(v, 95), len(v)))
    else: print('  %-44s -' % name)

def window_view(F, cidx, period):
    """Lane X: per app frame (SteamVR's own clock, no matching), its end against the compositor update window of
    the refresh it lands in: frame ready of compositor frame k is relative to k's vsync, so its end lies at
    fr(k) + (sys(k) - sys(k-1)) after vsync k-1, and k-1's update start/end are the window it falls in.
    Frames whose compositor logged no update times (start = end = 0) are left out."""
    rows = []
    for k in cidx:
        a = F.get(k - 1)
        if a is None or F[k]['fr'] == 0 or (a['us'] == 0 and a['ue'] == 0) or a['ue'] > period: continue
        e = F[k]['fr'] + (F[k]['sys'] - a['sys']) * 1e3
        c = 'before' if e < a['us'] else ('inside' if e <= a['ue'] else 'after')
        rows.append((k, e, a['us'], a['ue'], c, F[k]['pres'], F[k]['mis']))
    if not rows: return
    print(' C. the app\'s end against the update window of the refresh it lands in (%d frames):' % len(rows))
    print('    class    frames         presented 0x  2x+  mispresented  end-update start p50  end-update end p50')
    for c in ('before', 'inside', 'after'):
        r = [x for x in rows if x[4] == c]
        if not r: print('    %-7s       0' % c); continue
        print('    %-7s %6d (%4.1f%%)  %6d %5d %8d %18s %19s' % (
            c, len(r), 100.0 * len(r) / len(rows), sum(1 for x in r if x[5] == 0), sum(1 for x in r if x[5] > 1),
            sum(x[6] for x in r), '%+.2f' % median([x[1] - x[2] for x in r]), '%+.2f' % median([x[1] - x[3] for x in r])))
    sw = sum(1 for a, b in zip(rows, rows[1:]) if b[0] == a[0] + 1 and a[4] != b[4])
    print('    class changes between consecutive refreshes: %d (%.1f%%); the edge that matters is the update END' % (
        sw, 100.0 * sw / len(rows)))
    stats('end after vsync (ms)', [x[1] for x in rows])
    stats('end minus update end', [x[1] - x[3] for x in rows])
    # Re-phase episodes: the compositor's update starting within 1.5 ms of the vsync (it does so for 0.3-3 s
    # after a discontinuity, then creeps back to about +3.0).
    run, eps = [], []
    for k in cidx:
        f = F[k]
        if f['us'] == 0 and f['ue'] == 0: continue
        if f['us'] < 1.5: run.append(k)
        else:
            if len(run) >= 3: eps.append(run)
            run = []
    if len(run) >= 3: eps.append(run)
    s0 = F[cidx[0]]['sys']
    print('    early update starts (< vsync + 1.5 ms): %d frames in %d episodes%s' % (
        sum(len(r) for r in eps), len(eps), ': ' + ', '.join('t %.1f s for %.2f s' % (
            F[r[0]]['sys'] - s0, F[r[-1]]['sys'] - F[r[0]]['sys']) for r in eps[:12]) if eps else ''))
    hist('C: end minus the update end of its refresh', [x[1] - x[3] for x in rows], period)

PHASE = re.compile(r'end phase: (.*)')

def end_phase_lines(path):
    """The game's KARTFRAME_END_PHASE log lines (mode, fallbacks, achieved phase every 10 s)."""
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            m = PHASE.search(line)
            if m: out.append(m.group(1).strip())
    if out:
        print(' D. the game\'s end lock (KARTFRAME_END_PHASE), %d lines:' % len(out))
        for l in out[:40]: print('    ' + l)
        if len(out) > 40: print('    ... %d more' % (len(out) - 40))

def launch(d, opts):
    print('=== %s' % d)
    vf, gl = os.path.join(d, 'vrframes.log'), os.path.join(d, 'game.log')
    if not os.path.exists(gl): gl = os.path.join(d, 'stdout.log')
    if not os.path.exists(vf):
        print('  no vrframes.log (KARTFRAME_VRTIMING_FRAMES=1 was not set)'); return
    F, P, gaps = read_vrframes(vf)
    idx = sorted(F)
    if len(idx) < 10: print('  vrframes.log has %d frames' % len(idx)); return
    steps = [F[b]['sys'] - F[a]['sys'] for a, b in zip(idx, idx[1:]) if b == a + 1]
    period = median(steps) * 1e3
    rows, any_race = read_game(gl) if os.path.exists(gl) else ([], False)
    use_race = any_race and not opts['all']
    sel = [r for r in rows if r['race'] or not use_race]
    print('  compositor frames %d (missed between polls %d), refresh %.3f ms; XRF lines with clock fields %d '
          '(%s %d)' % (len(idx), gaps, period, len(rows), 'race' if use_race else 'all', len(sel)))
    offs, how = clock_offset(F, P, period / 1e3)
    offs.sort()
    ov = [o for _, o in offs]
    if ov:
        print('  SteamVR system time = CLOCK_MONOTONIC - %.6f s (%s; p5-p95 spread %.1f us over %d polls%s)' % (
            median(ov), how, (pct(ov, 95) - pct(ov, 5)) * 1e6, len(ov),
            '; drift %+.1f us/min' % ((median(ov[-20:]) - median(ov[:20])) * 1e6 /
                                       max(1e-9, (offs[-1][0] - offs[0][0]) / 60)) if len(ov) > 60 else ''))
    # Race window on the compositor side: frames whose reference falls inside the selected XRF wake-ups.
    if sel and ov:
        t0, t1 = sel[0]['wake'] / 1e3 - 0.05, sel[-1]['end'] / 1e3 + 0.05
        cidx = [k for k in idx if t0 <= F[k]['sys'] + offset_at(offs, F[k]['sys'] + ov[0]) <= t1]
    else:
        cidx = [k for k in idx if F[k]['fr'] > 0]

    # A. Inside SteamVR's clock.
    print(' A. SteamVR\'s own timings, ms after each compositor frame\'s vsync reference (%d frames):' % len(cidx))
    stats('app poses ready (xrWaitFrame wake-up)', [F[k]['pr'] for k in cidx if F[k]['pr'] != 0])
    stats('app frame ready (xrEndFrame)', [F[k]['fr'] for k in cidx if F[k]['fr'] != 0])
    stats('compositor update start', [F[k]['us'] for k in cidx])
    stats('compositor update end', [F[k]['ue'] for k in cidx])
    stats('compositor render start', [F[k]['rs'] for k in cidx])
    margin = [F[k]['us'] - F[k]['fr'] for k in cidx if F[k]['fr'] != 0]
    stats('update start minus frame ready (margin)', margin)
    pres = [F[k]['pres'] for k in cidx]
    print('  frames presented 0x %d, 2x+ %d; mispresented %d; dropped %d' % (
        sum(1 for p in pres if p == 0), sum(1 for p in pres if p > 1), sum(F[k]['mis'] for k in cidx),
        sum(F[k]['drop'] for k in cidx)))
    hist('A: frame ready, ms after the vsync reference', [F[k]['fr'] for k in cidx if F[k]['fr'] != 0], period)
    hist('A: update start minus frame ready', margin, period)
    window_view(F, cidx, period)
    if os.path.exists(gl): end_phase_lines(gl)

    if not sel or not ov:
        print(' B. no XRF m=/r=/t= lines (build without lane W, or KARTFRAME_FRAME_LOG unset): app-clock view skipped')
        return
    # B. The app's clock against the compositor's anchors (CLOCK_MONOTONIC ms).
    anchors = {}
    for name, key in (('vsync', None), ('update start', 'us'), ('render start', 'rs'), ('frame ready', 'fr')):
        a = []
        for k in cidx:
            base = (F[k]['sys'] + offset_at(offs, F[k]['sys'] + ov[0])) * 1e3
            if key is None: a.append(base)
            elif key != 'fr' or F[k]['fr'] != 0: a.append(base + F[k][key])
        anchors[name] = sorted(a)

    def near(t, name):
        a = anchors[name]
        i = bisect.bisect_left(a, t)
        best = min((abs(t - a[j]), t - a[j]) for j in (i - 1, i) if 0 <= j < len(a)) if a else (0, float('nan'))
        return best[1]

    ends = [r for r in sel if r['fresh']]
    print(' B. the app\'s times minus the nearest compositor anchor, ms (signed, +-%.2f):' % (period / 2))
    stats('end vs frame ready (clock check, about 0)', [near(r['end'], 'frame ready') for r in sel])
    out = {}
    for ev, pick in (('wake-up', lambda r: r['wake']), ('end', lambda r: r['end']),
                     ('release', lambda r: r['rel']), ('display time', lambda r: r['disp'])):
        for an in ('vsync', 'update start', 'render start'):
            src = sel if ev in ('wake-up', 'display time') else ends
            v = [near(pick(r), an) for r in src if pick(r) is not None]
            v = [x for x in v if not math.isnan(x)]
            out[(ev, an)] = v
            stats('%s vs %s' % (ev, an), v)
    which = [('end', a) for a in ('vsync', 'update start', 'render start')] + [('wake-up', 'vsync')]
    if opts['hist'] == 'all': which = list(out)
    for key in which:
        hist('B: %s minus the nearest %s' % key, out[key], period)

    # Drift: circular means per window.
    every = opts['every']
    print('  drift, every %g s (circular means, ms): t  end-vsync  end-update  end-render  wake-vsync  '
          'margin(A)  frames' % every)
    t0 = sel[0]['wake']
    bucket = {}
    for r in sel:
        b = int((r['wake'] - t0) / 1e3 / every)
        bucket.setdefault(b, []).append(r)
    cb = {}
    for k in cidx:
        m = (F[k]['sys'] + ov[0]) * 1e3
        cb.setdefault(int((m - t0) / 1e3 / every), []).append(k)
    for b in sorted(bucket):
        rs = bucket[b]
        fe = [r for r in rs if r['fresh']]
        mg = [F[k]['us'] - F[k]['fr'] for k in cb.get(b, []) if F[k]['fr'] > 0]
        print('  %6.1f  %+6.2f  %+6.2f  %+6.2f  %+6.2f  %+6.2f  %d' % (
            b * every, cmean([near(r['end'], 'vsync') for r in fe], period),
            cmean([near(r['end'], 'update start') for r in fe], period),
            cmean([near(r['end'], 'render start') for r in fe], period),
            cmean([near(r['wake'], 'vsync') for r in rs], period), median(mg) if mg else float('nan'), len(rs)))

def main(argv):
    opts = dict(all=False, every=1.0, hist='')
    dirs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--all': opts['all'] = True
        elif a == '--every': opts['every'] = float(argv[i + 1]); i += 1
        elif a == '--hist': opts['hist'] = argv[i + 1]; i += 1
        elif a.startswith('-'): sys.exit('usage: phase.py SESSION_DIR [...] [--all] [--every S] [--hist all]')
        else: dirs.append(a)
        i += 1
    if not dirs: sys.exit('usage: phase.py SESSION_DIR [...] [--all] [--every S] [--hist all]')
    for d in dirs: launch(d, opts)

if __name__ == '__main__':
    main(sys.argv[1:])
