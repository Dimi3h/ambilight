#!/usr/bin/env python3
"""Letterbox analysis: histogram presets, black-bar detector and reconstruction strategies.

usage: letterbox.py <lb.txt> <cal.txt> <leds.json> <profile_b4.h> [--emit header.h]

lb.txt  : `ambilight --calib-dump --calib-presets 0,1,2,3` capture of app/letterbox.html
          (64x36 UI ground truth, "B" dark fractions, "P" preset switches)
cal.txt : the preset-0 zone calibration capture (for the preset-0 footprints)

Target for letterboxed pictures: what a good grabber with black-border removal shows,
i.e. the picture area (bars cropped) stretched to the full frame, sampled at HyperHDR's
LED rectangles. For 16:9 pictures the normal per-LED colours.
"""
import bisect
import gzip
import importlib.util
import json
import math
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("cal", os.path.join(here, "calibrate.py"))
cal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cal)

NZ, GW, GH = 16, 16, 9
LW, LH = 64, 36            # letterbox capture UI grid (same as the pattern canvas)
LOW_T = [4, 8, 16, 24]
SETTLE_MS, PRESET_SETTLE_MS = 150, 400
LAM = 0.003


def load_lb(path):
    opener = gzip.open if path.endswith(".gz") else open
    H, B, U, P = [], {}, [], []
    for line in opener(path, "rt"):
        p = line.split()
        if not p:
            continue
        if p[0] == "H" and len(p) == 50:
            v = list(map(float, p[2:]))
            H.append((int(p[1]), [v[3 * z:3 * z + 3] for z in range(NZ)]))
        elif p[0] == "B" and len(p) == 2 + NZ * 3 * 4:
            v = list(map(float, p[2:]))
            B[int(p[1])] = [[v[(z * 3 + c) * 4:(z * 3 + c) * 4 + 4] for c in range(3)] for z in range(NZ)]
        elif p[0] == "U" and len(p) == 2 + LW * LH:
            cells = [tuple(map(int, c.split(","))) for c in p[2:]]
            U.append((int(p[1]), [cells[y * LW:(y + 1) * LW] for y in range(LH)]))
        elif p[0] == "P":
            P.append((int(p[1]), int(p[2])))
    return H, B, U, P


def classify(f):
    flat = [c for row in f for c in row]
    mean = [sum(c[k] for c in flat) / len(flat) for k in range(3)]
    if all(abs(c[k] - mean[k]) <= 4 for c in flat for k in range(3)):
        return ("uniform", tuple(round(x) for x in mean))
    bright = [(x, y) for y in range(LH) for x in range(LW) if min(f[y][x][:3]) > 200]
    if len(bright) == 16 and sum(1 for c in flat if max(c[:3]) < 20) == len(flat) - 16:
        x0, y0 = min(x for x, _ in bright), min(y for _, y in bright)
        if x0 % 4 == 0 and y0 % 4 == 0 and all((x - x0) < 4 and (y - y0) < 4 for x, y in bright):
            return ("cell", (y0 // 4) * GW + x0 // 4)
    black_row = lambda y: max(max(c[:3]) for c in f[y]) < 10
    top = 0
    while top < LH // 2 and black_row(top):
        top += 1
    bot = 0
    while bot < LH // 2 and black_row(LH - 1 - bot):
        bot += 1
    return ("image", (top, bot))


def bilinear(img, w, h, fx, fy):
    fx = min(max(fx, 0.0), w - 1.0); fy = min(max(fy, 0.0), h - 1.0)
    x0, y0 = int(fx), int(fy); x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1); u, v = fx - x0, fy - y0
    return [(img[y0 * w + x0][k] * (1 - u) + img[y0 * w + x1][k] * u) * (1 - v)
            + (img[y1 * w + x0][k] * (1 - u) + img[y1 * w + x1][k] * u) * v for k in range(3)]


def stretch(img, w, h, ht, hb, ow, oh):
    """Crop fraction ht (top) / hb (bottom) of a w x h image and stretch to ow x oh."""
    out = []
    for Y in range(oh):
        sy = ht + (Y + 0.5) / oh * (1 - ht - hb)
        for X in range(ow):
            out.append(bilinear(img, w, h, (X + 0.5) / ow * w - 0.5, sy * h - 0.5))
    return out


def main():
    args = sys.argv[1:]
    emit = None
    if "--emit" in args:
        i = args.index("--emit"); emit = args[i + 1]; del args[i:i + 2]
    lbp, calp, ledp, profp = args
    leds = json.load(open(ledp))
    hdr = open(profp).read()
    num = lambda s: list(map(float, re.findall(r"(-?\d+\.\d+)f", s)))
    R0 = [num(r) for r in re.findall(r"\{([^{}]*)\},\n", hdr.split("PROFILE_R[144][16]")[1])]
    blk = num(hdr.split("PROFILE_BLACK[3] = ")[1].split(";")[0])
    wht = num(hdr.split("PROFILE_WHITE[3] = ")[1].split(";")[0])
    gam = num(hdr.split("PROFILE_GAMMA[3] = ")[1].split(";")[0])
    lvl = lambda raw, c: 255.0 * min(max((raw - blk[c]) / (wht[c] - blk[c]), 0.0), 1.0) ** (1.0 / gam[c])

    # ---------- preset-0 footprints from the zone calibration
    H0, U0 = cal.load(calp)
    fr0 = cal.pair(H0, U0)
    uni0, cells0 = {}, {}
    for i, f, m, n in fr0:
        k, key = cal.classify(f)
        if k == "uniform": uni0.setdefault(key, []).append(m)
        elif k == "cell": cells0.setdefault(key, []).append(m)
    avg = lambda ms: [[sum(s[z][c] for s in ms) / len(ms) for c in range(3)] for z in range(NZ)]
    def footprints(uni, cells):
        bl = avg(uni[(0, 0, 0)]); wh = avg(uni[(255, 255, 255)])
        A = [[0.0] * (GW * GH) for _ in range(NZ)]
        for k, ms in cells.items():
            m = avg(ms)
            for z in range(NZ):
                A[z][k] = max(0.0, sum((m[z][c] - bl[z][c]) / max(1e-6, wh[z][c] - bl[z][c]) for c in range(3)) / 3)
        return A
    A0 = footprints(uni0, cells0)
    A0n = [[A0[z][k] / sum(A0[z]) for k in range(GW * GH)] for z in range(NZ)]

    # ---------- letterbox capture
    H, Bd, U, P = load_lb(lbp)
    pt = [t for t, _ in P]
    def preset_at(t):
        i = bisect.bisect_right(pt, t) - 1
        return (P[i][1], t - P[i][0]) if i >= 0 else (0, 10 ** 9)
    ut = [t for t, _ in U]
    groups = {}
    for t, m in H:
        pr, since = preset_at(t)
        if since < PRESET_SETTLE_MS:
            continue
        i = bisect.bisect_right(ut, t) - 1
        if i < 0 or t - ut[i] < SETTLE_MS or t not in Bd:
            continue
        groups.setdefault((i, pr), []).append((m, Bd[t]))
    print(f"lb capture: {len(H)} samples, {len(U)} UI frames, presets switched at {[p for _, p in P]}")
    per = {}
    for (i, pr), ss in sorted(groups.items()):
        m = [[sum(s[0][z][c] for s in ss) / len(ss) for c in range(3)] for z in range(NZ)]
        lo = [[[sum(s[1][z][c][j] for s in ss) / len(ss) for j in range(4)] for c in range(3)] for z in range(NZ)]
        per.setdefault(pr, []).append((i, U[i][1], m, lo, classify(U[i][1])))

    # ---------- what do presets 1..3 measure?
    A = {0: A0}
    for pr in (1, 2, 3):
        rows = per.get(pr, [])
        uni = {}; cells = {}
        for i, f, m, lo, (kind, key) in rows:
            if kind == "uniform": uni.setdefault(key, []).append(m)
            elif kind == "cell": cells.setdefault(key, []).append(m)
        if (0, 0, 0) not in uni or (255, 255, 255) not in uni or len(cells) < 100:
            print(f"preset {pr}: insufficient data (refs {sorted(uni)}, cells {len(cells)})"); continue
        A[pr] = footprints(uni, cells)
        cover = [sum(A[pr][z][k] for z in range(NZ)) for k in range(GW * GH)]
        print(f"\npreset {pr}: cells measured {len(cells)}/144; footprint sums "
              f"{[round(sum(A[pr][z]), 2) for z in range(NZ)]}")
        print("  coverage per cell (sum over zones; '#' >= 0.5 of a fully covered cell, '+' >= 0.1):")
        mx = max(cover)
        for r in range(GH):
            print("   " + "".join("#" if cover[r * GW + c] >= 0.5 * mx else "+" if cover[r * GW + c] >= 0.1 * mx else "."
                                  for c in range(GW)))

    # ---------- letterbox images under preset 0 and preset 1, paired by content
    def key(f):  # coarse content signature to pair the same image across cycles
        return tuple(tuple(c[:3]) for row in f[::6] for c in row[::8])
    imgs0 = [(f, m, lo, kk[1]) for i, f, m, lo, kk in per.get(0, []) if kk[0] == "image"]
    imgs1 = {key(f): m for i, f, m, lo, kk in per.get(1, []) if kk[0] == "image"}
    print(f"\nletterbox set: {len(imgs0)} images under preset 0, {len(imgs1)} under preset 1")

    def gt(f, top, bot):
        img = [list(f[y][x][:3]) for y in range(LH) for x in range(LW)]
        st = stretch(img, LW, LH, top / LH, bot / LH, LW, LH)
        return [cal.rect_mean(st, LW, LH, l) for l in leds]

    def recon(Rop, m, perm=(0, 1, 2)):
        cell = [[lvl(sum(Rop[k][z] * m[z][perm[c]] for z in range(NZ)), c) for c in range(3)] for k in range(GW * GH)]
        return cal.upsample2(cell, GW, GH)  # 32 x 18

    def led_err(out32, ht, hb, target):
        st = stretch(out32, 32, 18, ht, hb, 32, 18)
        return sum(abs(a - b) for l, g in zip(leds, target) for a, b in zip(cal.rect_mean(st, 32, 18, l), g)) / (3 * len(leds))

    L = cal.laplacian(GW, GH)
    def ls_op(Aw):
        AtA = [[sum(Aw[z][i] * Aw[z][j] for z in range(NZ)) for j in range(GW * GH)] for i in range(GW * GH)]
        return cal.solve([[AtA[i][j] + LAM * L[i][j] for j in range(GW * GH)] for i in range(GW * GH)],
                         [[Aw[z][i] for z in range(NZ)] for i in range(GW * GH)])

    # bar model: fraction of each 16x9 cell inside top/bottom bars of height ht/hb
    def bar_frac(ht, hb):
        ph = []
        for r in range(GH):
            y0, y1 = r / GH, (r + 1) / GH
            inside = max(0.0, min(y1, ht) - y0) + max(0.0, y1 - max(y0, 1 - hb))
            ph.append(min(1.0, inside * GH))
        return [ph[k // GW] for k in range(GW * GH)]
    op_cache = {}
    def bar_op(ht, hb):
        k = (round(ht, 4), round(hb, 4))
        if k not in op_cache:
            phi = bar_frac(ht, hb)
            Aw = [[A0n[z][c] * (1 - phi[c]) for c in range(GW * GH)] for z in range(NZ)]
            op_cache[k] = (ls_op(Aw), phi)
        return op_cache[k]
    def recon_bar(m, ht, hb):
        Rop, phi = bar_op(ht, hb)
        # remove the known black-bar contribution from each zone mean (raw domain)
        mm = [[m[z][c] - sum(A0n[z][k] * phi[k] for k in range(GW * GH)) * blk[c] for c in range(3)] for z in range(NZ)]
        return recon(Rop, mm)
    R1 = None
    if 1 in A:
        A1n = [[A[1][z][k] / max(1e-9, sum(A[1][z])) for k in range(GW * GH)] for z in range(NZ)]
        R1 = ls_op(A1n)

    # ---------- detector on preset-0 dark fractions
    def expected_dark(ht, hb):
        phi = bar_frac(ht, hb)
        return [sum(A0n[z][k] * phi[k] for k in range(GW * GH)) for z in range(NZ)]
    cand = [r / LH for r in range(0, 8)]
    def detect(lo, tidx, tol=0.06):
        beta = [min(lo[z][c][tidx] for c in range(3)) for z in range(NZ)]
        best = 0.0
        for h in cand[1:]:
            e = expected_dark(h, h)
            if all(beta[z] >= e[z] - tol for z in range(NZ)):
                best = h
        return best
    print("\ndetector (preset 0; symmetric bars; rows of 36): true -> detected for thresholds", LOW_T)
    for f, m, lo, (top, bot) in imgs0:
        det = [round(detect(lo, j) * LH) for j in range(4)]
        print(f"  bars {top},{bot}  ->  {det}")

    # ---------- strategies vs the stretched target (oracle bar heights)
    print("\nstrategy MAE per image vs stretched-picture target (oracle bar heights):")
    print("  bars | current(no crop) | crop only | preset0+bar model | preset1")
    tot = [0.0, 0.0, 0.0, 0.0]; nb = 0
    for f, m, lo, (top, bot) in imgs0:
        tg = gt(f, top, bot); ht, hb = top / LH, bot / LH
        e_cur = led_err(recon(R0, m), 0, 0, tg)
        e_crop = led_err(recon(R0, m), ht, hb, tg)
        e_bar = led_err(recon_bar(m, ht, hb), ht, hb, tg) if (top or bot) else e_crop
        m1 = imgs1.get(key(f))
        e_p1 = led_err(recon(R1, m1), ht, hb, tg) if (R1 and m1) else float("nan")
        print(f"  {top:2d},{bot:2d} | {e_cur:8.2f}         | {e_crop:8.2f}  | {e_bar:8.2f}          | {e_p1:8.2f}")
        if top or bot:
            for j, e in enumerate((e_cur, e_crop, e_bar, e_p1)):
                tot[j] += e
            nb += 1
    if nb:
        print("  mean over letterboxed images: current %.2f | crop %.2f | bar model %.2f | preset1 %.2f"
              % tuple(x / nb for x in tot))

    if emit:
        # operators for bar heights 1..7 rows of 36 (symmetric), raw domain, plus the
        # bar contribution per zone so the service can subtract it
        with open(emit, "w") as fo:
            fo.write("// Generated by tools/letterbox.py. Do not edit by hand.\n#pragma once\n")
            fo.write(f"#define LB_LEVELS 7\n// bar height of level i = (i + 1) / {LH} of the frame, top and bottom\n")
            fo.write("static const float LB_R[LB_LEVELS][144][16] = {\n")
            darks = []
            for lv in range(1, 8):
                h = lv / LH
                Rop, phi = bar_op(h, h)
                darks.append([sum(A0n[z][k] * phi[k] for k in range(GW * GH)) for z in range(NZ)])
                fo.write("  {\n" + "".join("    {" + ", ".join(f"{x:.5f}f" for x in row) + "},\n" for row in Rop) + "  },\n")
            fo.write("};\n// share of each zone covered by the bars at each level (expected dark fraction)\n")
            fo.write("static const float LB_BAR_SHARE[LB_LEVELS][16] = {\n")
            for d in darks:
                fo.write("    {" + ", ".join(f"{x:.5f}f" for x in d) + "},\n")
            fo.write("};\n")
        print("wrote", emit)


if __name__ == "__main__":
    main()
