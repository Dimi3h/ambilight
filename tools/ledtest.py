#!/usr/bin/env python3
"""Dim test patterns for a real LED strip through HyperHDR's JSON API (no TV needed).

usage:
  ledtest.py color R G B [--seconds 30]    one colour on every LED of the layout
  ledtest.py edges [--level 64] [--seconds 60]
        left = red, top = green, right = blue, bottom = yellow; prints, from HyperHDR's own
        LED layout, what you should see along the strip from its data input
  ledtest.py leds SPEC [--level 64] [--seconds 60]
        light single LEDs, counted from the data input (1 = first). SPEC is a comma list of
        index[:colour] or first-last[:colour], colours r g b y w, e.g. 38:r,39:g,155-156:b
        (default colour r). Uses HyperHDR's layout: neighbours whose areas overlap, as in
        the corners, glow faintly; the printout says which.
  ledtest.py blink N [N ...] [--seconds 60]
        blink exactly these LEDs in turn (1 = first), everything else off. Uses HyperHDR's
        own identify command, which addresses the strip directly and ignores the layout:
        the way to find an LED hidden in a fold
  ledtest.py clear                          remove the test colour now
options: --host 192.168.129.249 --port 19444 --priority 10

Every pattern except blink is sent as an image, the way ambilight sends its frames, so it
passes through HyperHDR's LED layout: LEDs disabled there stay dark. (HyperHDR's own solid
`color` command skips the layout and lights disabled LEDs too; measured on 22.0.0.)

The test runs at a high priority (10, lower wins) and expires by itself after --seconds, so
whatever HyperHDR showed before comes back. Values pass through HyperHDR's own adjustment
(gamma, brightness), so the LEDs get less than the number sent: keep levels low while the
strip is on its reel.
"""
import argparse
import base64
import json
import socket
import struct
import time
import zlib

ORIGIN = "ledtest"


def rpc(args, cmd):
    s = socket.create_connection((args.host, args.port), timeout=5)
    s.sendall((json.dumps(cmd) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.split(b"\n")[0])


def png(w, h, px):
    raw = b"".join(b"\x00" + bytes(c for x in range(w) for c in px[y * w + x]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def edges_image(level, leds, w=128, h=72):
    """Each edge band as deep as HyperHDR's LEDs on that edge; corners black so that no LED
    sees two edge colours."""
    side = [l["hmax"] for l in leds if l["hmin"] <= 0.001 and 0.1 < (l["vmin"] + l["vmax"]) / 2 < 0.9]
    top = [l["vmax"] for l in leds if l["vmin"] <= 0.001 and 0.1 < (l["hmin"] + l["hmax"]) / 2 < 0.9]
    bx = max(1, round((max(side) if side else 0.05) * w))
    by = max(1, round((max(top) if top else 0.08) * h))
    red, green, blue, yellow = (level, 0, 0), (0, level, 0), (0, 0, level), (level, level, 0)
    px = []
    for y in range(h):
        for x in range(w):
            in_x = x < bx or x >= w - bx
            in_y = y < by or y >= h - by
            if in_x and in_y:
                px.append((0, 0, 0))  # corner
            elif y < by:
                px.append(green)
            elif y >= h - by:
                px.append(yellow)
            elif x < bx:
                px.append(red)
            elif x >= w - bx:
                px.append(blue)
            else:
                px.append((0, 0, 0))
    return w, h, px


def expected(leds, w, h, px):
    """Mean colour of each LED's area in the image, in HyperHDR's output (= strip) order."""
    out = []
    for l in leds:
        x0, x1 = int(l["hmin"] * w), max(int(l["hmin"] * w) + 1, int(round(l["hmax"] * w)))
        y0, y1 = int(l["vmin"] * h), max(int(l["vmin"] * h) + 1, int(round(l["vmax"] * h)))
        cells = [px[y * w + x] for y in range(y0, min(y1, h)) for x in range(x0, min(x1, w))]
        out.append(tuple(sum(c[i] for c in cells) // len(cells) for i in range(3)))
    return out


COLOURS = {"r": (1, 0, 0), "g": (0, 1, 0), "b": (0, 0, 1), "y": (1, 1, 0), "w": (1, 1, 1)}


def parse_spec(spec, n, level):
    """'38:r,39:g,155-156:b' -> {index0: (r, g, b)}"""
    want = {}
    for part in spec.split(","):
        rng, _, col = part.strip().partition(":")
        col = col or "r"
        if col not in COLOURS:
            raise SystemExit(f"unknown colour '{col}' (use r g b y w)")
        a, _, b = rng.partition("-")
        first, last = int(a), int(b or a)
        if not 1 <= first <= last <= n:
            raise SystemExit(f"LED range {rng} outside 1-{n}")
        for i in range(first, last + 1):
            want[i - 1] = tuple(level * c for c in COLOURS[col])
    return want


def leds_image(leds, want, w=512, h=288):
    """Paint each wanted LED's own area in HyperHDR's layout; everything else black."""
    px = [(0, 0, 0)] * (w * h)
    for i, col in want.items():
        l = leds[i]
        x0, x1 = int(l["hmin"] * w), max(int(l["hmin"] * w) + 1, int(round(l["hmax"] * w)))
        y0, y1 = int(l["vmin"] * h), max(int(l["vmin"] * h) + 1, int(round(l["vmax"] * h)))
        for y in range(y0, min(y1, h)):
            for x in range(x0, min(x1, w)):
                px[y * w + x] = col
    return w, h, px


def name(c, level):
    m = max(c)
    if m < level * 0.15:
        return "off"
    on = tuple(v >= m * 0.5 for v in c)
    return {(True, False, False): "red", (False, True, False): "green", (False, False, True): "blue",
            (True, True, False): "yellow"}.get(on, "mixed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["color", "edges", "leds", "blink", "clear"])
    ap.add_argument("rgb", nargs="*")
    ap.add_argument("--host", default="192.168.129.249")
    ap.add_argument("--port", type=int, default=19444)
    ap.add_argument("--priority", type=int, default=10)
    ap.add_argument("--level", type=int, default=64)
    ap.add_argument("--seconds", type=int, default=60)
    a = ap.parse_args()
    ms = a.seconds * 1000

    if a.what == "clear":
        print(rpc(a, {"command": "clear", "priority": a.priority}))
        return
    if a.what == "color":
        rgb = [int(v) for v in a.rgb] if len(a.rgb) == 3 and all(v.isdigit() for v in a.rgb) else []
        if len(rgb) != 3 or not all(0 <= v <= 255 for v in rgb):
            raise SystemExit("color needs R G B (0-255)")
        w, h = 128, 72
        img = base64.b64encode(png(w, h, [tuple(rgb)] * (w * h))).decode()
        r = rpc(a, {"command": "image", "imagedata": img, "format": "auto", "priority": a.priority,
                    "duration": ms, "origin": ORIGIN})
        print(r.get("success"), f"colour {rgb} for {a.seconds} s at priority {a.priority}")
        return

    if a.what == "blink":
        if not a.rgb or not all(v.isdigit() and int(v) >= 1 for v in a.rgb):
            raise SystemExit("blink needs LED numbers, counted from 1")
        end, n = time.time() + a.seconds, 0
        while time.time() < end:
            i = int(a.rgb[n % len(a.rgb)])
            n += 1
            rpc(a, {"command": "leddevice", "subcommand": "identify", "ledDeviceType": "blink",
                    "params": {"blinkIndex": i - 1}})  # HyperHDR counts from 0
            time.sleep(2.0)
        print(f"blinked LED(s) {' '.join(a.rgb)} in turn for {a.seconds} s")
        return

    if a.what == "leds":
        if len(a.rgb) != 1:
            raise SystemExit("leds needs one SPEC, e.g. 38:r,39:g,155-156:b")
        leds = rpc(a, {"command": "serverinfo"})["info"]["leds"]
        want = parse_spec(a.rgb[0], len(leds), a.level)
        w, h, px = leds_image(leds, want)
        exp = expected(leds, w, h, px)
        r = rpc(a, {"command": "image", "imagedata": base64.b64encode(png(w, h, px)).decode(), "format": "auto",
                    "priority": a.priority, "duration": ms, "origin": ORIGIN})
        print(r.get("success"), f"{len(want)} LED(s) of {len(leds)} for {a.seconds} s at priority {a.priority}")
        for i, c in enumerate(exp):
            m = max(c)
            if i in want:
                print(f"  LED {i + 1:3d}: {name(c, a.level)}" + ("" if m >= a.level * 0.6 else f" (only {100 * m // a.level}% - area shared with a neighbour)"))
            elif m >= a.level * 0.15:
                print(f"  LED {i + 1:3d}: faint {name(c, a.level)} ({100 * m // a.level}%), overlaps a lit LED's area")
        return

    info = rpc(a, {"command": "serverinfo"})["info"]
    leds = info["leds"]
    w, h, px = edges_image(a.level, leds)
    runs = []
    for c in expected(leds, w, h, px):
        n = name(c, a.level)
        if runs and runs[-1][0] == n:
            runs[-1][1] += 1
        else:
            runs.append([n, 1])
    r = rpc(a, {"command": "image", "imagedata": base64.b64encode(png(w, h, px)).decode(), "format": "auto",
                "priority": a.priority, "duration": ms, "origin": ORIGIN})
    print(r.get("success"), f"edge pattern (level {a.level}) for {a.seconds} s at priority {a.priority}")
    print(f"{len(leds)} LEDs configured. Expected from the strip's data input onwards:")
    pos = 1
    for n, k in runs:
        print(f"  LED {pos:3d}-{pos + k - 1:3d}: {n} ({k})")
        pos += k
    print(f"  LED {pos:3d}-...: off (beyond the {len(leds)} configured LEDs)")


if __name__ == "__main__":
    main()
