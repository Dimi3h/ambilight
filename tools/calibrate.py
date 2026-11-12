#!/usr/bin/env python3
"""Zone calibration for the LG histogram, from a `ambilight --calib-dump` capture.

usage: calibrate.py <cal.txt> <leds.json> <out_header.h> [--preview out.png]

The dump holds exact histogram zone means ("H t <16x3 floats>") and, whenever the UI
plane changed, a 32x18 RGBA grid of it ("U t <576 x r,g,b,a>"), recorded while
patterns/calibrate.html showed reference colours, a white cell walking a 16x9 grid and a set
of validation images. Because the UI plane is the ground truth, no timing schedule is
needed.

Steps:
  1. pair every histogram sample with the UI frame that was stable >= SETTLE ms before;
  2. classify frames: uniform (references), walking cell, validation image;
  3. check channel order and measure levels (black/white/gamma per channel);
  4. derive each zone's footprint A[z][k] over the 16x9 cells;
  5. build reconstruction operators R (cells x zones) and evaluate them on the
     validation images exactly as HyperHDR will see them (mean over each LED rectangle);
  6. fit each zone's rectangle on the TV's sample grid and evaluate the non-negative solve
     the service uses (cells >= 0, solved per frame over the picture area) on the same images;
  7. write the level map and the zone rectangles to a C header.
Pure Python (no numpy); the linear systems are small.
"""
import bisect
import gzip
import json
import math
import struct
import sys
import zlib

SETTLE_MS = 150
UW, UH = 32, 18          # UI ground-truth grid
GW, GH = 16, 9           # calibration / reconstruction grid
NZ = 16
NNLS_LAMBDA = 0.001      # smoothness weight of the non-negative solve
NNLS_BLACK = 0.25        # a zone mean below this (raw levels above black) is a black zone
NNLS_PIN = 0.5           # ... and blacks out the cells it covers at least this far
NNLS_SWEEPS = 30         # coordinate-descent sweeps per frame in the service

# The TV samples the picture on a grid of SGW x SGH points; every zone is a rectangle of it.
# ZONE_SAMPLES are the bin totals per zone of preset 0 (columns x rows), read with the
# service's --hist-dump on the B4. With the measured footprints they fix each rectangle.
SGW, SGH = 162, 91
ZONE_SAMPLES = [7371, 1843, 2405, 931, 7371, 1457, 1584, 1666, 7371, 1519, 1584, 931, 931, 1666, 780, 777]


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def fit_zone_rects(An):
    """Per zone (first column, columns, first row, rows) on the sample grid: the factorisation
    of the zone's sample total and the position that best match the measured footprint's
    column and row profiles. A rectangle that ends within two samples of a screen edge is
    taken to touch it (assumption; the footprints are too coarse to tell)."""
    rects = []
    for z in range(NZ):
        colp = [sum(An[z][y * GW + x] for y in range(GH)) for x in range(GW)]
        rowp = [sum(An[z][y * GW + x] for x in range(GW)) for y in range(GH)]
        best = None
        for nc in range(8, SGW + 1):
            if ZONE_SAMPLES[z] % nc:
                continue
            nr = ZONE_SAMPLES[z] // nc
            if nr > SGH or nr < 8:
                continue
            ec, c0 = min((sum((overlap(c, c + nc, x * SGW / GW, (x + 1) * SGW / GW) / nc - colp[x]) ** 2
                              for x in range(GW)), c) for c in range(SGW - nc + 1))
            er, r0 = min((sum((overlap(r, r + nr, y * SGH / GH, (y + 1) * SGH / GH) / nr - rowp[y]) ** 2
                              for y in range(GH)), r) for r in range(SGH - nr + 1))
            if best is None or ec + er < best[0]:
                best = (ec + er, c0, nc, r0, nr)
        _, c0, nc, r0, nr = best
        if c0 <= 2: c0 = 0
        if SGW - (c0 + nc) <= 2: c0 = SGW - nc
        if r0 <= 2: r0 = 0
        if SGH - (r0 + nr) <= 2: r0 = SGH - nr
        rects.append((c0, nc, r0, nr))
    return rects


def area_model(rects, bars, gw=GW, gh=GH):
    """Footprints of the zones over a gw x gh grid laid on the picture area. bars = (left,
    right, top, bottom) in samples. Returns A[z][k], the share of zone z's samples that fall
    in cell k, and cover[z][k], the share of cell k that zone z covers."""
    l, r, t, b = bars
    x0, y0 = l, t
    cw, ch = (SGW - l - r) / gw, (SGH - t - b) / gh
    A, cover = [], []
    for c0, nc, r0, nr in rects:
        ra, rc = [], []
        for j in range(gh):
            oy = overlap(r0, r0 + nr, y0 + j * ch, y0 + (j + 1) * ch)
            for i in range(gw):
                o = oy * overlap(c0, c0 + nc, x0 + i * cw, x0 + (i + 1) * cw)
                ra.append(o / (nc * nr)); rc.append(o / (cw * ch))
        A.append(ra); cover.append(rc)
    return A, cover


def ls_operator(A, gw=GW, gh=GH, lam=NNLS_LAMBDA):
    """R[k][z]: unconstrained least-squares answer (A'A + lam L) x = A'h, the solve's start value."""
    n = gw * gh
    L = laplacian(gw, gh)
    AtA = [[sum(A[z][i] * A[z][j] for z in range(len(A))) + lam * L[i][j] for j in range(n)] for i in range(n)]
    return solve(AtA, [[A[z][i] for z in range(len(A))] for i in range(n)])


def nnls_area(A, cover, R, h, gw=GW, gh=GH, lam=NNLS_LAMBDA, sweeps=NNLS_SWEEPS):
    """What src/ambilight.c does per frame and channel: start from the linear answer with the
    negative parts cut off, then coordinate descent on |A x - h|^2 + lam x'Lx with x >= 0.
    Cells that a black zone covers for the most part are black and leave the smoothness term."""
    n = gw * gh
    nb = [[] for _ in range(n)]
    for y in range(gh):
        for x in range(gw):
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if 0 <= x + dx < gw and 0 <= y + dy < gh:
                    nb[y * gw + x].append((y + dy) * gw + x + dx)
    cols = [[(z, A[z][k]) for z in range(len(A)) if A[z][k]] for k in range(n)]
    black = [any(h[z] < NNLS_BLACK and cover[z][k] >= NNLS_PIN for z in range(len(A))) for k in range(n)]
    free = [[j for j in nb[k] if not black[j]] for k in range(n)]
    x = [0.0 if black[k] else max(0.0, sum(R[k][z] * h[z] for z in range(len(A)))) for k in range(n)]
    r = list(h)
    for k in range(n):
        if x[k]:
            for z, w in cols[k]:
                r[z] -= w * x[k]
    for _ in range(sweeps):
        moved = 0.0
        for k in range(n):
            if black[k]:
                continue
            g = sum(w * r[z] for z, w in cols[k]) - lam * (len(free[k]) * x[k] - sum(x[j] for j in free[k]))
            nx = max(0.0, x[k] + g / (sum(w * w for _, w in cols[k]) + lam * len(free[k])))
            d = nx - x[k]
            if d:
                x[k] = nx
                for z, w in cols[k]:
                    r[z] -= w * d
                moved = max(moved, abs(d))
        if moved < 0.01:
            break
    return x


# ------------------------------------------------------------------ parsing
def load(path):
    opener = gzip.open if path.endswith(".gz") else open
    H, U = [], []
    for line in opener(path, "rt"):
        p = line.split()
        if not p:
            continue
        if p[0] == "H" and len(p) == 2 + 48:
            v = list(map(float, p[2:]))
            H.append((int(p[1]), [v[3 * z:3 * z + 3] for z in range(NZ)]))
        elif p[0] == "U" and len(p) - 2 in (32 * 18, 64 * 36):
            w = 32 if len(p) - 2 == 32 * 18 else 64
            cells = [tuple(map(int, c.split(","))) for c in p[2:]]
            U.append((int(p[1]), [cells[y * w:(y + 1) * w] for y in range(len(cells) // w)]))
    return H, U


def pair(H, U):
    """frame index -> list of zone-mean samples taken while that frame was stable."""
    ut = [t for t, _ in U]
    by_frame = {}
    for t, m in H:
        i = bisect.bisect_right(ut, t) - 1
        if i < 0 or t - ut[i] < SETTLE_MS:
            continue
        by_frame.setdefault(i, []).append(m)
    out = []
    for i, ms in sorted(by_frame.items()):
        avg = [[sum(s[z][c] for s in ms) / len(ms) for c in range(3)] for z in range(NZ)]
        out.append((i, U[i][1], avg, len(ms)))
    return out


# ------------------------------------------------------------------ classification
def classify(frame):
    flat = [c for row in frame for c in row]
    mean = [sum(c[k] for c in flat) / len(flat) for k in range(3)]
    if all(abs(c[k] - mean[k]) <= 4 for c in flat for k in range(3)):
        return ("uniform", tuple(round(x) for x in mean))
    bright = [(x, y) for y in range(UH) for x in range(UW) if min(frame[y][x][:3]) > 200]
    dark = sum(1 for c in flat if max(c[:3]) < 20)
    if len(bright) == 4 and dark == len(flat) - 4:
        xs = sorted({x for x, _ in bright}); ys = sorted({y for _, y in bright})
        if len(xs) == 2 and len(ys) == 2 and xs[0] % 2 == 0 and ys[0] % 2 == 0:
            return ("cell", (ys[0] // 2) * GW + xs[0] // 2)
    return ("image", None)


# ------------------------------------------------------------------ small linear algebra
def solve(Am, B):
    """Solve Am X = B (Am n x n, B n x m) by Gauss-Jordan with partial pivoting."""
    n, m = len(Am), len(B[0])
    M = [Am[i][:] + B[i][:] for i in range(n)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(M[r][col]))
        M[col], M[piv] = M[piv], M[col]
        p = M[col][col]
        if abs(p) < 1e-15:
            raise ValueError("singular")
        inv = 1.0 / p
        row = M[col]
        for j in range(col, n + m):
            row[j] *= inv
        for r in range(n):
            if r != col and M[r][col] != 0.0:
                f = M[r][col]
                rr = M[r]
                for j in range(col, n + m):
                    rr[j] -= f * row[j]
    return [M[i][n:] for i in range(n)]


def laplacian(w, h):
    n = w * h
    L = [[0.0] * n for _ in range(n)]
    for y in range(h):
        for x in range(w):
            i = y * w + x
            for dx, dy in ((1, 0), (0, 1)):
                if x + dx < w and y + dy < h:
                    j = (y + dy) * w + (x + dx)
                    L[i][i] += 1; L[j][j] += 1; L[i][j] -= 1; L[j][i] -= 1
    return L


# ------------------------------------------------------------------ LED sampling
def rect_mean(img, w, h, led):
    """Area-weighted mean of a w x h RGB image over a HyperHDR LED rectangle."""
    x0, x1, y0, y1 = led["hmin"] * w, led["hmax"] * w, led["vmin"] * h, led["vmax"] * h
    acc = [0.0, 0.0, 0.0]; wsum = 0.0
    for y in range(max(0, int(y0)), min(h, math.ceil(y1))):
        fy = min(y + 1, y1) - max(y, y0)
        if fy <= 0:
            continue
        for x in range(max(0, int(x0)), min(w, math.ceil(x1))):
            fx = min(x + 1, x1) - max(x, x0)
            if fx <= 0:
                continue
            a = fx * fy; wsum += a
            p = img[y * w + x]
            for k in range(3):
                acc[k] += a * p[k]
    return [v / wsum for v in acc] if wsum else [0.0, 0.0, 0.0]


def upsample2(img, w, h):
    """Bilinear 2x upsample (pixel-centre aligned), w x h -> 2w x 2h."""
    out = []
    for Y in range(2 * h):
        fy = min(max((Y + 0.5) / 2 - 0.5, 0), h - 1)
        y0 = int(fy); y1 = min(y0 + 1, h - 1); v = fy - y0
        for X in range(2 * w):
            fx = min(max((X + 0.5) / 2 - 0.5, 0), w - 1)
            x0 = int(fx); x1 = min(x0 + 1, w - 1); u = fx - x0
            p = [0.0, 0.0, 0.0]
            for k in range(3):
                p[k] = ((img[y0 * w + x0][k] * (1 - u) + img[y0 * w + x1][k] * u) * (1 - v)
                        + (img[y1 * w + x0][k] * (1 - u) + img[y1 * w + x1][k] * u) * v)
            out.append(p)
    return out


# ------------------------------------------------------------------ PNG (for the preview)
def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(v for px in row for v in px) for row in rows)
    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))


def main():
    args = sys.argv[1:]
    preview = None
    if "--preview" in args:
        i = args.index("--preview"); preview = args[i + 1]; del args[i:i + 2]
    cal, leds_path, out_h = args
    leds = json.load(open(leds_path))
    H, U = load(cal)
    frames = pair(H, U)
    print(f"histogram samples {len(H)}, UI frames {len(U)}, stable frames {len(frames)}")

    # alpha must be opaque for the UI to equal the displayed picture
    alphas = {c[3] for _, f, _, _ in frames for row in f for c in row}
    print("UI alpha values seen:", sorted(alphas)[:5], "..." if len(alphas) > 5 else "")

    uniform, cells, images = {}, {}, []
    kinds = [(i, f, m, classify(f)) for i, f, m, n in frames]
    last_cell = max((i for i, f, m, (kind, _) in kinds if kind == "cell"), default=-1)
    for i, f, m, (kind, key) in kinds:
        if kind == "uniform":
            uniform.setdefault(key, []).append(m)
        elif kind == "cell":
            cells.setdefault(key, []).append(m)
        elif i > last_cell:  # validation images follow the walking cell in the pattern
            images.append((f, m))
    avgm = lambda ms: [[sum(s[z][c] for s in ms) / len(ms) for c in range(3)] for z in range(NZ)]
    uniform = {k: avgm(v) for k, v in uniform.items()}
    cells = {k: avgm(v) for k, v in cells.items()}
    print(f"uniform colours {len(uniform)}, walking cells {len(cells)}/144, validation images {len(images)}")

    # --- channel order and levels (zone-averaged)
    print("\nreference colour (UI)   ->  histogram mean over zones (ch0, ch1, ch2)   [zone spread]")
    zavg = {}
    for col in sorted(uniform, key=lambda c: (sum(c), c)):
        m = uniform[col]
        za = [sum(m[z][c] for z in range(NZ)) / NZ for c in range(3)]
        spread = max(max(m[z][c] for z in range(NZ)) - min(m[z][c] for z in range(NZ)) for c in range(3))
        zavg[col] = za
        print(f"  {str(col):17s} -> ({za[0]:6.1f}, {za[1]:6.1f}, {za[2]:6.1f})   [{spread:5.1f}]")
    def find(c):
        best = min(uniform, key=lambda k: sum((a - b) ** 2 for a, b in zip(k, c)))
        return best if sum((a - b) ** 2 for a, b in zip(best, c)) < 300 else None
    k_black, k_white, k_red, k_green, k_blue = (find(c) for c in
        [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255)])
    perm = [0, 1, 2]
    if k_red and k_green and k_blue:
        perm = [max(range(3), key=lambda c: zavg[k][c]) for k in (k_red, k_green, k_blue)]
        print("histogram channel carrying UI R,G,B:", perm, "(identity = R,G,B)" if perm == [0, 1, 2] else "(SWAPPED)")
    if not (k_black and k_white):
        sys.exit("black/white references missing")
    black = [[uniform[k_black][z][c] for c in range(3)] for z in range(NZ)]
    white = [[uniform[k_white][z][c] for c in range(3)] for z in range(NZ)]
    # gamma per channel from the grey references (zone-averaged, perm-corrected)
    gam = [1.0, 1.0, 1.0]
    for c in range(3):
        hc = perm[c]
        b = sum(black[z][hc] for z in range(NZ)) / NZ; w = sum(white[z][hc] for z in range(NZ)) / NZ
        gs = []
        for v in (64, 128):
            k = find((v, v, v))
            if k:
                frac = (zavg[k][hc] - b) / (w - b)
                if 0 < frac < 1:
                    gs.append(math.log(frac) / math.log(v / 255))
        if gs:
            gam[c] = sum(gs) / len(gs)
    lvl_black = [sum(black[z][perm[c]] for z in range(NZ)) / NZ for c in range(3)]
    lvl_white = [sum(white[z][perm[c]] for z in range(NZ)) / NZ for c in range(3)]
    print("levels per UI channel: black", [round(x, 1) for x in lvl_black],
          "white", [round(x, 1) for x in lvl_white], "gamma", [round(g, 3) for g in gam])

    def to_ui(hval, c):  # histogram (raw) -> UI 0..255 for UI channel c
        f = (hval - lvl_black[c]) / (lvl_white[c] - lvl_black[c])
        f = min(max(f, 0.0), 1.0)
        return 255.0 * f ** (1.0 / gam[c])

    # --- footprints: fraction of each zone in each 16x9 cell (luma-like average of channels)
    A = [[0.0] * (GW * GH) for _ in range(NZ)]
    for k, m in cells.items():
        for z in range(NZ):
            d = [(m[z][c] - black[z][c]) / max(1e-6, white[z][c] - black[z][c]) for c in range(3)]
            A[z][k] = max(0.0, sum(d) / 3)
    missing = [k for k in range(GW * GH) if k not in cells]
    for k in missing:  # fill from measured neighbours (should be none)
        y, x = divmod(k, GW); nb = [(y + dy) * GW + x + dx for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0))
                                    if 0 <= y + dy < GH and 0 <= x + dx < GW and (y + dy) * GW + x + dx in cells]
        for z in range(NZ):
            A[z][k] = sum(A[z][j] for j in nb) / len(nb) if nb else 0.0
    sums = [sum(A[z]) for z in range(NZ)]
    print("\nfootprint sums per zone (1.0 = linear, fully tiled):", [round(s, 2) for s in sums])
    print("missing cells:", missing)
    An = [[A[z][k] / sums[z] for k in range(GW * GH)] for z in range(NZ)]

    # --- reconstruction operators R[k][z] (applied in the raw histogram domain, per channel)
    def op_argmax():
        R = [[0.0] * NZ for _ in range(GW * GH)]
        for k in range(GW * GH):
            R[k][max(range(NZ), key=lambda z: An[z][k])] = 1.0
        return R
    def op_weighted(p):
        R = []
        for k in range(GW * GH):
            w = [An[z][k] ** p for z in range(NZ)]; s = sum(w) or 1.0
            R.append([x / s for x in w])
        return R
    L = laplacian(GW, GH)
    AtA = [[sum(An[z][i] * An[z][j] for z in range(NZ)) for j in range(GW * GH)] for i in range(GW * GH)]
    At = [[An[z][i] for z in range(NZ)] for i in range(GW * GH)]
    def op_ls(lam):
        M = [[AtA[i][j] + lam * L[i][j] for j in range(GW * GH)] for i in range(GW * GH)]
        return solve(M, At)

    def predict(R, m, up):
        cell = []
        for k in range(GW * GH):
            raw = [sum(R[k][z] * m[z][perm[c]] for z in range(NZ)) for c in range(3)]
            cell.append([to_ui(raw[c], c) for c in range(3)])
        if up:
            return upsample2(cell, GW, GH), 2 * GW, 2 * GH
        return cell, GW, GH

    def gt_leds(f):
        img = [list(f[y][x][:3]) for y in range(UH) for x in range(UW)]
        return [rect_mean(img, UW, UH, l) for l in leds]

    val = [(f, m) for f, m in images]
    letterbox = [v for v in val if all(max(c[:3]) < 20 for c in v[0][0]) and all(max(c[:3]) < 20 for c in v[0][-1])]
    main_val = [v for v in val if v not in letterbox]
    gts = [gt_leds(f) for f, _ in main_val]

    def evaluate(R, up):
        err = 0.0; n = 0
        for (f, m), gt in zip(main_val, gts):
            img, w, h = predict(R, m, up)
            for l, g in zip(leds, gt):
                e = rect_mean(img, w, h, l)
                err += sum(abs(a - b) for a, b in zip(e, g)); n += 3
        return err / n if n else float("nan")

    # baselines
    def flat_err():
        err = 0.0; n = 0
        for (f, m), gt in zip(main_val, gts):
            mean = [to_ui(sum(m[z][perm[c]] for z in range(NZ)) / NZ, c) for c in range(3)]
            for g in gt:
                err += sum(abs(a - b) for a, b in zip(mean, g)); n += 3
        return err / n
    print(f"\nvalidation images used: {len(main_val)} (+{len(letterbox)} letterbox reported separately)")
    print(f"  baseline: whole-screen average      MAE {flat_err():6.2f}")
    results = []
    for name, R in [("argmax (hard, current)", op_argmax()), ("weighted p=1", op_weighted(1)),
                    ("weighted p=2", op_weighted(2)), ("weighted p=4", op_weighted(4))]:
        for up in (False, True):
            e = evaluate(R, up); results.append((e, name, up, R)); print(f"  {name:28s} up={int(up)}  MAE {e:6.2f}")
    for lam in (1e-5, 3e-5, 1e-4, 3e-4, 1e-3, 3e-3, 1e-2):
        R = op_ls(lam)
        for up in (False, True):
            e = evaluate(R, up); results.append((e, f"least-squares lam={lam:g}", up, R))
            print(f"  least-squares lam={lam:<9g}        up={int(up)}  MAE {e:6.2f}")
    # The non-negative solve of the service: exact zone rectangles, no cell below black.
    rects = fit_zone_rects(An)
    print("\nzone rectangles on the %dx%d sample grid (first column, columns, first row, rows):" % (SGW, SGH))
    for z, (c0, nc, r0, nr) in enumerate(rects):
        fa, _ = area_model([rects[z]], (0, 0, 0, 0))
        worst = max(abs(a - b) for a, b in zip(fa[0], An[z])) / max(An[z])
        print(f"  zone {z:2d}: {c0:3d} {nc:3d} {r0:2d} {nr:2d}   largest difference with the measured footprint {100 * worst:4.1f} % of its peak")
    Af, Cf = area_model(rects, (0, 0, 0, 0))
    Rf = ls_operator(Af)
    def nnls_eval(sweeps):
        err = 0.0; n = 0
        for (f, m), gt in zip(main_val, gts):
            cell = [[0.0, 0.0, 0.0] for _ in range(GW * GH)]
            for c in range(3):
                x = nnls_area(Af, Cf, Rf, [max(0.0, m[z][perm[c]] - lvl_black[c]) for z in range(NZ)], sweeps=sweeps)
                for k in range(GW * GH):
                    cell[k][c] = to_ui(x[k] + lvl_black[c], c)
            img = upsample2(cell, GW, GH)
            for l, g in zip(leds, gt):
                e = rect_mean(img, 2 * GW, 2 * GH, l)
                err += sum(abs(a - b) for a, b in zip(e, g)); n += 3
        return err / n
    nnls_mae = nnls_eval(NNLS_SWEEPS)
    print(f"  non-negative solve on the zone rectangles, lam={NNLS_LAMBDA:g}, {NNLS_SWEEPS} sweeps, up=1  MAE {nnls_mae:6.2f}"
          f"  (settled: {nnls_eval(400):.2f})")
    # Negative weights amplify read-to-read histogram noise (the double-buffered reads
    # alternate by a few units on moving video). Noise gain = largest row L1 norm.
    gain = lambda R: max(sum(abs(x) for x in row) for row in R)
    print("\nnoise gain (max row L1) per candidate:")
    for e, name, up, R in results:
        if not up:
            print(f"  {name:32s} gain {gain(R):5.2f}  (MAE {e:.2f})")
    # Pick the lowest noise gain among candidates within 1 % of the best MAE.
    results.sort(key=lambda r: r[0])
    tol = results[0][0] * 1.01
    best_e, best_name, best_up, best_R = min((r for r in results if r[0] <= tol), key=lambda r: gain(r[3]))
    print(f"\nbest (within 1 % of min MAE, lowest noise gain): {best_name} up={int(best_up)}  "
          f"MAE {best_e:.2f}  gain {gain(best_R):.2f}")
    neg = sum(1 for row in best_R for x in row if x < -1e-3)
    print("negative weights in best operator:", neg, " row-sum range:",
          round(min(sum(r) for r in best_R), 3), round(max(sum(r) for r in best_R), 3))

    # --- header
    with open(out_h, "w") as f:
        f.write("// Generated by tools/calibrate.py from a --calib-dump capture. Do not edit by hand.\n")
        f.write("// Levels from the reference colours; zone geometry for the per-frame solve in src/ambilight.c.\n")
        f.write("#pragma once\n")
        f.write(f"#define PROFILE_COLS {GW}\n#define PROFILE_ROWS {GH}\n#define PROFILE_UPSAMPLE {int(best_up)}\n")
        f.write(f"// histogram channel that carries UI R, G, B\nstatic const int PROFILE_PERM[3] = {{{perm[0]}, {perm[1]}, {perm[2]}}};\n")
        f.write("// raw histogram -> 0..255: f = clamp((h - black) / (white - black)); out = 255 * f^(1/gamma)\n")
        fmt = lambda v: ", ".join(f"{x:.3f}f" for x in v)
        f.write(f"static const float PROFILE_BLACK[3] = {{{fmt(lvl_black)}}};\n")
        f.write(f"static const float PROFILE_WHITE[3] = {{{fmt(lvl_white)}}};\n")
        f.write(f"static const float PROFILE_GAMMA[3] = {{{fmt(gam)}}};\n")
        f.write("// The TV samples the picture on a PROFILE_GRID_W x PROFILE_GRID_H grid; zone z is the rectangle\n")
        f.write("// {first column, columns, first row, rows} of it (sample totals + measured footprints, see calibrate.py).\n")
        f.write(f"// Non-negative solve over the picture area, smoothness PROFILE_NNLS_LAMBDA: validation MAE {nnls_mae:.2f}/255.\n")
        f.write(f"#define PROFILE_GRID_W {SGW}\n#define PROFILE_GRID_H {SGH}\n")
        f.write(f"#define PROFILE_NNLS_LAMBDA {NNLS_LAMBDA:g}\n")
        f.write("static const int PROFILE_ZONE[16][4] = {\n")
        for c0, nc, r0, nr in rects:
            f.write(f"    {{{c0}, {nc}, {r0}, {nr}}},\n")
        f.write("};\n")
    print("wrote", out_h)

    # --- preview: ground truth | current (argmax) | best, as screen + LED ring
    if preview:
        cur = next(r for r in results if r[1].startswith("argmax") and not r[2])
        picks = main_val[:: max(1, len(main_val) // 7)][:7] + letterbox[:1]
        SC, RING, GAP = 10, 14, 8
        pw, ph = UW * SC + 2 * RING, UH * SC + 2 * RING
        cols = 3
        Wt, Ht = cols * pw + (cols + 1) * GAP, len(picks) * (ph + GAP) + GAP
        canvas = [[(24, 24, 24)] * Wt for _ in range(Ht)]
        def draw_panel(ox, oy, f, ledcols):
            for y in range(UH * SC):
                for x in range(UW * SC):
                    canvas[oy + RING + y][ox + RING + x] = tuple(f[y // SC][x // SC][:3])
            for l, c in zip(leds, ledcols):
                c = tuple(int(min(255, max(0, v))) for v in c)
                x0 = ox + RING + int(l["hmin"] * UW * SC); x1 = ox + RING + max(int(l["hmax"] * UW * SC), int(l["hmin"] * UW * SC) + 1)
                y0 = oy + RING + int(l["vmin"] * UH * SC); y1 = oy + RING + max(int(l["vmax"] * UH * SC), int(l["vmin"] * UH * SC) + 1)
                cx, cy = (l["hmin"] + l["hmax"]) / 2, (l["vmin"] + l["vmax"]) / 2
                if cy >= 0.8:   ys, xs = range(oy + ph - RING, oy + ph - 2), range(x0, x1)
                elif cy <= 0.2: ys, xs = range(oy + 2, oy + RING), range(x0, x1)
                elif cx <= 0.2: ys, xs = range(y0, y1), range(ox + 2, ox + RING)
                else:           ys, xs = range(y0, y1), range(ox + pw - RING, ox + pw - 2)
                for yy in ys:
                    for xx in xs:
                        canvas[yy][xx] = c
        for i, (f, m) in enumerate(picks):
            oy = GAP + i * (ph + GAP)
            draw_panel(GAP, oy, f, gt_leds(f))
            for j, R, up in ((1, cur[3], cur[2]), (2, best_R, best_up)):
                img, w, h = predict(R, m, up)
                draw_panel(GAP + j * (pw + GAP), oy, f, [rect_mean(img, w, h, l) for l in leds])
        write_png(preview, Wt, Ht, canvas)
        print("preview:", preview, "(columns: ideal per-LED | current argmax | new)")


if __name__ == "__main__":
    main()
