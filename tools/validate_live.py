#!/usr/bin/env python3
"""End-to-end check: compare HyperHDR's actual LED output with the ideal per-LED colours.

usage: validate_live.py <calib-dump.txt> <ledrec.jsonl> <leds.json>

Run app/validate.html on the TV while the service runs with --calib-dump (UI ground
truth) and tools/ledrec.py records HyperHDR's LED stream. Each image shown for >= 1.5 s
is aligned (dynamic programming, in order) with a stable plateau of the LED stream;
the report lists the uniform references and the mean absolute error per validation
image against what an ideal per-LED grabber would show. Add --no-ui-blend to the
service to test the histogram reconstruction alone (otherwise opaque UI dominates).
"""
import json, sys, socket, importlib.util, os
spec = importlib.util.spec_from_file_location("cal", os.path.join(os.path.dirname(os.path.abspath(__file__)), "calibrate.py"))
cal = importlib.util.module_from_spec(spec); spec.loader.exec_module(cal)
DUMP, REC, LEDS = sys.argv[1:4]
leds = json.load(open(LEDS))
inv = lambda v: v  # HyperHDR's LED stream is linear in the sent image (measured: grey 128 -> ~128)
# ground truth sequence from the TV dump: UI frames shown for >= 1.5 s, in order
H, U = cal.load(DUMP)
seq = []
for (t, f), (t2, _) in zip(U, U[1:] + [(U[-1][0] + 99999, None)]):
    if t2 - t >= 1500:
        seq.append(f)
print("ground-truth frames (>=1.5 s):", len(seq))
# LED plateaus from the HyperHDR stream
rec = [json.loads(l) for l in open(REC)]
frames = [r[1] for r in rec]
segs, cur = [], []
for a, b in zip(frames, frames[1:]):
    d = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    if d < 2.0: cur.append(b)
    else:
        if len(cur) >= 8: segs.append(cur)
        cur = []
if len(cur) >= 8: segs.append(cur)
def seg_leds(sg):
    tail = sg[int(len(sg) * 0.4):]
    return [[sum(fr[3 * i + c] for fr in tail) / len(tail) for c in range(3)] for i in range(len(leds))]
plateaus = [seg_leds(sg) for sg in segs]
print("LED plateaus (>=0.8 s):", len(plateaus))
# ideal per-LED colours of each ground-truth frame
def gt(f):
    img = [list(f[y][x][:3]) for y in range(18) for x in range(32)]
    return [cal.rect_mean(img, 32, 18, l) for l in leds]
gts = [gt(f) for f in seq]
uni = lambda f: all(abs(c[k] - f[0][0][k]) <= 4 for row in f for c in row for k in range(3))
# optimal monotonic alignment (DP): match plateaus to ground-truth frames in order,
# allowing either side to skip entries at a fixed penalty
err = lambda P, G: sum(abs(inv(p[c]) - g[c]) for p, g in zip(P, G) for c in range(3)) / (3 * len(G))
n, m, SKIP = len(plateaus), len(gts), 60.0
E = [[err(plateaus[i], gts[j]) for j in range(m)] for i in range(n)]
D = [[0.0] * (m + 1) for _ in range(n + 1)]; B = [[None] * (m + 1) for _ in range(n + 1)]
for i in range(n + 1):
    for j in range(m + 1):
        if i == 0 and j == 0: continue
        opts = []
        if i and j: opts.append((D[i-1][j-1] + E[i-1][j-1], "m"))
        if i: opts.append((D[i-1][j] + SKIP, "p"))
        if j: opts.append((D[i][j-1] + SKIP, "g"))
        D[i][j], B[i][j] = min(opts)
pairs = []; i, j = n, m
while i or j:
    b = B[i][j]
    if b == "m": pairs.append((plateaus[i-1], j-1)); i -= 1; j -= 1
    elif b == "p": i -= 1
    else: j -= 1
pairs.reverse()
print("\nreferences (uniform frames): expected UI colour -> LED output (avg over all LEDs)")
val_err = []
for P, k in pairs:
    f = seq[k]
    avg = [sum(inv(p[c]) for p in P) / len(P) for c in range(3)]
    if uni(f):
        print(f"  {str(tuple(f[0][0][:3])):17s} -> ({avg[0]:6.1f}, {avg[1]:6.1f}, {avg[2]:6.1f})")
    else:
        val_err.append(err(P, gts[k]))
print(f"\nvalidation images matched: {len(val_err)}; end-to-end MAE vs ideal per-LED: "
      f"{sum(val_err)/len(val_err):.2f}  (per image: {', '.join(f'{e:.1f}' for e in val_err)})")
