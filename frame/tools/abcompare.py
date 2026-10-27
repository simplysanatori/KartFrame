#!/usr/bin/env python3
"""A/B comparison of two mock runs (lane AD): python3 -I abcompare.py [options] RUN_A RUN_B [RUN_A2 RUN_B2 ...]

Run dirs are frame/tools/abcompare.sh's: game.log, samples.txt (frame/perf/run.sh) and dump/ (MOCK_XR_DUMP with
index.txt, MOCK_XR_STAMP=1). Arguments come in A B pairs, one pair per round; metrics are pooled over rounds.

Timing: frame/perf/pacing-parts.py's "race" part (1-player race) over the pacing seconds outside the dump windows,
sets/s, late % of sets due, GPU ms per set p50/p95 from the per-set samples.
Pictures: each A eye image is paired with the B eye image of the same eye whose scene position (previous game frame +
weight x (current - previous), relative to "entered RaceScene", from the image's stamp) is nearest, within --pos-tol
game frames, each B image once. Compared outside the stamp corner: share of pixels with any channel off by more than
--tol/255 and PSNR. Prints one table and PASS/FAIL; exit 0 on PASS, 1 on FAIL, 2 on a setup problem."""
import argparse
import bisect
import importlib.util
import math
import os
import re
import struct
import sys
import zlib
from operator import sub

HERE = os.path.dirname(os.path.abspath(__file__))


def load_pacing_parts():
    spec = importlib.util.spec_from_file_location("pacing_parts", os.path.join(HERE, "..", "perf", "pacing-parts.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", help="run dirs, A B [A B ...]")
    ap.add_argument("--out", required=True, help="where the report and the worst-pair image go")
    ap.add_argument("--windows", default="", help="dump times (s) whose seconds are left out of the timing")
    ap.add_argument("--step", type=int, default=6, help="the dumps' MOCK_XR_DUMP_STEP (6), to size the stamp corner left out")
    ap.add_argument("--tol", type=int, default=2, help="per-channel tolerance in 1/255 (2)")
    ap.add_argument("--pos-tol", type=float, default=0.05, help="max scene position difference of a pair, game frames (0.05)")
    ap.add_argument("--max-pairs", type=int, default=120, help="compare at most this many pairs (120)")
    ap.add_argument("--min-pairs", type=int, default=20, help="FAIL with fewer matched pairs (20)")
    ap.add_argument("--max-diff-pct", type=float, default=0.5, help="FAIL when the mean pair differs in more %% of pixels (0.5)")
    ap.add_argument("--max-worst-pct", type=float, default=5.0, help="FAIL when the worst pair differs in more %% of pixels (5)")
    ap.add_argument("--max-errors", type=int, default=0, help="FAIL with more errors on either side (0)")
    ap.add_argument("--gpu-delta-max", type=float, default=None, help="optional: FAIL unless GPU p50 B - A <= this many ms")
    ap.add_argument("--late-delta-max", type=float, default=None, help="optional: FAIL unless late %% B - A <= this")
    ap.add_argument("--names", default="A,B", help="labels for the two sides")
    return ap.parse_args()


# ---------------------------------------------------------------- timing

def window_seconds(windows):
    skip = set()
    for w in windows.split(","):
        if w.strip():
            t = float(w)
            skip.update(range(int(t) - 1, int(t) + 7))
    return skip


def timing(pp, d, skip, scratch):
    """pacing-parts.run on a copy of the run's log and samples without the seconds in skip."""
    os.makedirs(scratch, exist_ok=True)
    with open(os.path.join(d, "game.log"), errors="replace") as f, open(os.path.join(scratch, "game.log"), "w") as g:
        for line in f:
            m = re.search(r"vr pacing: t=(\d+) ", line)
            if not (m and int(m.group(1)) in skip):
                g.write(line)
    sp = os.path.join(d, "samples.txt")
    if os.path.exists(sp):
        with open(sp) as f, open(os.path.join(scratch, "samples.txt"), "w") as g:
            for line in f:
                m = re.match(r"t=(\d+) ", line)
                if not (m and int(m.group(1)) in skip):
                    g.write(line)
    parts, totals, mock, half = pp.run(scratch)
    return parts["race"], mock


def errors(d):
    """Mock errors (summary and ERROR trace lines), fatal dialogs and crashes, error-level game log lines."""
    n, found = 0, False
    for line in open(os.path.join(d, "game.log"), errors="replace"):
        m = re.search(r"xrmock: summary: (\d+) errors", line)
        if m:
            n += int(m.group(1))
            found = True
        elif re.search(r"fatal dialog|Signal 11|\[(error|critical)\]|VUID-", line):
            n += 1
    return n if found else n + 1000  # no mock summary: the run did not finish properly


# ---------------------------------------------------------------- pictures

def ppm(path):
    b = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", b)
    return int(m.group(1)), int(m.group(2)), b[m.end():m.end() + int(m.group(1)) * int(m.group(2)) * 3]


def race_frame0(d):
    for line in open(os.path.join(d, "game.log"), errors="replace"):
        m = re.search(r"entered RaceScene at frame (\d+)", line)
        if m:
            return int(m.group(1))
    return None


def scene_images(d):
    """[(eye, relative scene position, ppm path)] of the race images in d/dump/index.txt."""
    f0 = race_frame0(d)
    idx = os.path.join(d, "dump", "index.txt")
    if f0 is None or not os.path.exists(idx):
        return []
    out = []
    for line in open(idx):
        p = line.split()
        if len(p) < 7:
            continue
        a = int(p[5])
        if not (a >> 48) & 1:  # no scene (menus)
            continue
        w, prev, cur = (a & 0xffff) / 65535.0, (a >> 16) & 0xffff, (a >> 32) & 0xffff
        rel = lambda x: ((x - f0 + 0x8000) & 0xffff) - 0x8000  # the stamp keeps the low 16 bits of the frame
        pv = rel(prev)
        pos = pv + w * (((cur - prev + 0x8000) & 0xffff) - 0x8000)
        path = os.path.join(d, "dump", p[0])
        if os.path.exists(path):
            out.append((int(p[3]), pos, path))
    return out


def pair_images(ia, ib, pos_tol, max_pairs):
    pairs = []
    for eye in (0, 1):
        a = sorted(x for x in ia if x[0] == eye)
        b = sorted(x for x in ib if x[0] == eye)
        keys = [x[1] for x in b]
        used = set()
        cand = []
        for ea, pa, fa in a:
            k = bisect.bisect_left(keys, pa)
            for j in (k - 1, k, k + 1):
                if 0 <= j < len(b):
                    cand.append((abs(b[j][1] - pa), fa, b[j][2], j, pa))
        for dpos, fa, fb, j, pa in sorted(cand):
            if dpos > pos_tol:
                break
            if j in used or any(p[0] == fa for p in pairs):
                continue
            used.add(j)
            pairs.append((fa, fb, dpos, pa, eye))
    pairs.sort(key=lambda p: (p[4], p[3]))
    if len(pairs) > max_pairs:  # an even spread
        pairs = [pairs[int(i * len(pairs) / max_pairs)] for i in range(max_pairs)]
    return pairs


def align(ia, ib, span=4.0, probes=12):
    """The scene position offset (game frames, B minus A) at which A's pictures best match B's: the two runs' race
    starts differ by a frame or two. Probe pictures spread over A, nearest B picture of the same eye, mean absolute
    difference of every 7th byte; offsets in quarter frames. Returns (offset, mean abs diff, probes used)."""
    if not ia or not ib:
        return 0.0, None, 0
    ia = sorted(ia, key=lambda x: x[1])
    pr = [ia[int(i * len(ia) / probes)] for i in range(probes)]
    keys = {e: sorted((x[1], x[2]) for x in ib if x[0] == e) for e in (0, 1)}
    cache, best = {}, (None, None, 0)
    for q in range(-int(span * 4), int(span * 4) + 1):
        off, tot, n = q / 4.0, 0.0, 0
        for e, pos, f in pr:
            ks = keys[e]
            if not ks:
                continue
            j = bisect.bisect_left([k[0] for k in ks], pos + off)
            c = [ks[i] for i in (j - 1, j) if 0 <= i < len(ks)]
            pb, fb = min(c, key=lambda k: abs(k[0] - pos - off))
            if abs(pb - pos - off) > 0.3:
                continue
            for path in (f, fb):
                if path not in cache:
                    cache[path] = ppm(path)[2][::7]
            tot += sum(map(abs, map(sub, cache[f], cache[fb]))) / max(1, len(cache[f]))
            n += 1
        if n >= max(3, probes // 2) and (best[1] is None or tot / n < best[1]):
            best = (off, tot / n, n)
    return best if best[1] is not None else (0.0, None, 0)


SQ = [i * i for i in range(256)] + [(511 - i) ** 2 for i in range(256, 511)]  # square of a signed difference by index


def compare(pa, pb, tol, stamp_w, stamp_h):
    """(percent of pixels off by more than tol, psnr dB, worst abs diff, diff bytes) for two PPMs; the stamp corner is
    taken out. Returns None for different sizes."""
    wa, ha, a = ppm(pa)
    wb, hb, b = ppm(pb)
    if (wa, ha) != (wb, hb):
        return None
    # rows below the stamp corner and the columns right of it; the corner (top left) is skipped
    keep = bytearray(b"\1") * (wa * ha)
    for y in range(min(stamp_h, ha)):
        keep[y * wa:y * wa + min(stamp_w, wa)] = bytes(min(stamp_w, wa))
    n = sum(keep)
    d = list(map(sub, a, b))
    big = bytes(1 if (x > tol or x < -tol) else 0 for x in d)
    pix = bytearray(wa * ha)
    for c in range(3):
        for i, v in enumerate(big[c::3]):
            if v:
                pix[i] = 1
    bad = sum(1 for i in range(wa * ha) if pix[i] and keep[i])
    mse = sum(SQ[x] for i, x in enumerate(d) if keep[i // 3]) / (3.0 * n)
    psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 * 255.0 / mse)
    mag = bytes(min(255, abs(x) * 16) if keep[i // 3] else 0 for i, x in enumerate(d))
    return 100.0 * bad / n, psnr, max((abs(x) for x in d), default=0), (wa, ha, a, b, mag)


def png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, x: struct.pack(">I", len(x)) + t + x + struct.pack(">I", zlib.crc32(t + x) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def side_by_side(path, w, h, a, b, mag):
    rows = b"".join(a[y * w * 3:(y + 1) * w * 3] + b[y * w * 3:(y + 1) * w * 3] + mag[y * w * 3:(y + 1) * w * 3]
                    for y in range(h))
    png(path, 3 * w, h, rows)


# ---------------------------------------------------------------- report

def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def merge(parts):
    m = {"s": 0, "ticks": 0, "rendered": 0, "late": 0, "repeats": 0, "gpu": [], "enc": [], "frames": 0, "fsecs": 0.0}
    for p in parts:
        for k in ("s", "ticks", "rendered", "late", "repeats", "frames", "fsecs"):
            m[k] += p[k]
        m["gpu"] += p["gpu"]
        m["enc"] += p["enc"]
    return m


def stats(m):
    due = m["rendered"] + m["late"]
    return {"race seconds": m["s"], "sets/s": m["rendered"] / m["s"] if m["s"] else None,
            "late %": 100.0 * m["late"] / due if due else None,
            "repeats %": 100.0 * m["repeats"] / m["ticks"] if m["ticks"] else None,
            "GPU ms p50": pct(m["gpu"], .5), "GPU ms p95": pct(m["gpu"], .95),
            "encode ms p50": pct(m["enc"], .5), "game fps": m["frames"] / m["fsecs"] if m["fsecs"] else None}


def cell(v, digits=2):
    return "-" if v is None else ("%d" % v if isinstance(v, int) else ("%." + str(digits) + "f") % v)


def main():
    a = parse_args()
    if len(a.runs) % 2:
        print("abcompare: run dirs come in A B pairs", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    names = a.names.split(",")
    pp = load_pacing_parts()
    skip = window_seconds(a.windows)
    pairs_dirs = [(a.runs[i], a.runs[i + 1]) for i in range(0, len(a.runs), 2)]
    side = {0: {"parts": [], "err": 0}, 1: {"parts": [], "err": 0}}
    pairs, offsets = [], []
    for r, dirs in enumerate(pairs_dirs):
        for s, d in enumerate(dirs):
            part, _ = timing(pp, d, skip, os.path.join(a.out, "timing-%d%s" % (r, "AB"[s])))
            side[s]["parts"].append(part)
            side[s]["err"] += errors(d)
        ia, ib = scene_images(dirs[0]), scene_images(dirs[1])
        off, mad, n = align(ia, ib)
        offsets.append(off)
        ib = [(e, p - off, f) for e, p, f in ib]
        for fa, fb, dpos, pos, eye in pair_images(ia, ib, a.pos_tol, a.max_pairs):
            pairs.append((fa, fb, dpos, pos, eye))
    # the stamp block is 64 x 15 px in a corner of the full-size image; the dumps are every n-th pixel
    sw, sh = 64 // a.step + 3, 15 // a.step + 3
    results = []
    for fa, fb, dpos, pos, eye in pairs:
        r = compare(fa, fb, a.tol, sw, sh)
        if r:
            results.append((r[0], r[1], r[2], pos, eye, fa, fb, r[3]))
    sa, sb = stats(merge(side[0]["parts"])), stats(merge(side[1]["parts"]))
    rows = [("errors", side[0]["err"], side[1]["err"], 0)]
    for k in ("race seconds", "sets/s", "late %", "repeats %", "GPU ms p50", "GPU ms p95", "encode ms p50", "game fps"):
        rows.append((k, sa[k], sb[k], 2))
    verdict = []
    if results:
        mean_pct = sum(x[0] for x in results) / len(results)
        worst = max(results, key=lambda x: x[0])
        mean_psnr = sum(x[1] for x in results) / len(results)
        low = min(x[1] for x in results)
        rows.append(("image pairs (same eye, same scene position)", len(results), len(results), 0))
        rows.append(("scene offset B - A found, game frames", sum(offsets) / len(offsets), None, 2))
        rows.append(("pixels off by more than %d/255, mean %%" % a.tol, mean_pct, None, 3))
        rows.append(("pixels off, worst pair %", worst[0], None, 3))
        rows.append(("PSNR dB, mean / lowest", mean_psnr, low, 1))
        wr = worst[7]
        side_by_side(os.path.join(a.out, "worst.png"), wr[0], wr[1], wr[2], wr[3], wr[4])
    else:
        mean_pct = worst = None
    if side[0]["err"] > a.max_errors or side[1]["err"] > a.max_errors:
        verdict.append("errors")
    if len(results) < a.min_pairs:
        verdict.append("only %d matched image pairs (need %d)" % (len(results), a.min_pairs))
    elif mean_pct > a.max_diff_pct:
        verdict.append("mean image diff %.3f %% > %.3f %%" % (mean_pct, a.max_diff_pct))
    elif worst[0] > a.max_worst_pct:
        verdict.append("worst pair %.3f %% > %.3f %%" % (worst[0], a.max_worst_pct))
    if a.gpu_delta_max is not None and sa["GPU ms p50"] is not None and sb["GPU ms p50"] is not None \
            and sb["GPU ms p50"] - sa["GPU ms p50"] > a.gpu_delta_max:
        verdict.append("GPU p50 delta %+.2f ms > %.2f" % (sb["GPU ms p50"] - sa["GPU ms p50"], a.gpu_delta_max))
    if a.late_delta_max is not None and sa["late %"] is not None and sb["late %"] is not None \
            and sb["late %"] - sa["late %"] > a.late_delta_max:
        verdict.append("late %% delta %+.2f > %.2f" % (sb["late %"] - sa["late %"], a.late_delta_max))
    w = max(len(r[0]) for r in rows)
    lines = ["%-*s | %10s | %10s | %10s" % (w, "metric", names[0], names[1], "delta")]
    lines.append("-" * w + "-+-" + "-+-".join(["-" * 10] * 3))
    for r in rows:
        k, va, vb, dg = r[0], r[1], r[2], r[3]
        if k.startswith("PSNR") or "worst" in k or "mean %" in k or "offset" in k:
            lines.append("%-*s | %10s | %10s | %10s" % (w, k, cell(va, dg), cell(vb, dg) if vb is not None else "", ""))
            continue
        delta = "-" if va is None or vb is None else ("%+d" % (vb - va) if isinstance(va, int) and isinstance(vb, int)
                                                      else "%+.*f" % (dg, vb - va))
        lines.append("%-*s | %10s | %10s | %10s" % (w, k, cell(va, dg), cell(vb, dg), delta))
    lines.append("")
    if results:
        lines.append("worst pair: %s vs %s (eye %d, scene position %.2f); side by side A | B | 16x diff: %s" % (
            os.path.relpath(worst[5], a.out), os.path.relpath(worst[6], a.out), worst[4], worst[3], os.path.join(a.out, "worst.png")))
    lines.append("%s%s" % ("FAIL: " + "; ".join(verdict) if verdict else "PASS", ""))
    text = "\n".join(lines)
    print(text)
    open(os.path.join(a.out, "report.txt"), "w").write(text + "\n")
    return 1 if verdict else 0


if __name__ == "__main__":
    sys.exit(main())
