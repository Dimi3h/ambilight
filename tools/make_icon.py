#!/usr/bin/env python3
"""Draw the app icon: a dark screen with a coloured ambient glow around it.

usage: make_icon.py <size> <out.png>
Generated at build time so the repository holds no binary assets.
"""
import colorsys
import math
import struct
import sys
import zlib


def icon(size, ss=3):
    n = size * ss
    cx = cy = n / 2
    hw, hh = n * 0.27, n * 0.16          # screen half-width/height (16:9-ish)
    radius = n * 0.2                     # rounded corners of the tile
    sigma = n * 0.11                     # glow falloff
    bg = (14, 16, 19)
    screen = (27, 31, 38)
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]
    for y in range(n):
        py = y + 0.5
        for x in range(n):
            px = x + 0.5
            # rounded tile mask
            qx = max(abs(px - cx) - (n / 2 - radius), 0)
            qy = max(abs(py - cy) - (n / 2 - radius), 0)
            if qx * qx + qy * qy > radius * radius:
                continue
            dx = max(abs(px - cx) - hw, 0)
            dy = max(abs(py - cy) - hh, 0)
            if dx == 0 and dy == 0:
                inner = min(hw - abs(px - cx), hh - abs(py - cy))
                c = screen if inner > n * 0.012 else (70, 76, 86)  # thin bezel
            else:
                d = math.hypot(dx, dy)
                g = math.exp(-(d / sigma) ** 2)
                hue = (math.atan2(py - cy, px - cx) / (2 * math.pi) + 0.08) % 1.0
                r, gg, b = colorsys.hsv_to_rgb(hue, 0.8, 1.0)
                c = tuple(min(255, bg[i] + g * 235 * (r, gg, b)[i]) for i in range(3))
            a = acc[(y // ss) * size + x // ss]
            a[0] += c[0]; a[1] += c[1]; a[2] += c[2]; a[3] += 255
    k = ss * ss
    rows = []
    for y in range(size):
        row = bytearray(b"\x00")
        for x in range(size):
            r, g, b, al = acc[y * size + x]
            # colours were accumulated only for covered samples: un-premultiply
            cov = al / 255
            if cov:
                row += bytes((round(r / cov), round(g / cov), round(b / cov), round(al / k)))
            else:
                row += b"\x00\x00\x00\x00"
        rows.append(bytes(row))
    return b"".join(rows)


def png(size, raw):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    s = int(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(png(s, icon(s)))
