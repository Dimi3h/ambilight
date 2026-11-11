#!/usr/bin/env python3
"""Decode the PNG written by tests/test_png.c with an independent decoder (zlib + chunk
CRCs) and compare it with the image the test encoded."""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
pos, chunks = 8, []
while pos < len(data):
    (n,) = struct.unpack(">I", data[pos:pos + 4])
    kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
    (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
    assert crc == zlib.crc32(kind + body) & 0xFFFFFFFF, f"crc of {kind}"
    chunks.append((kind, body))
    pos += 12 + n
assert [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"], "chunk order"
w, h, depth, colour, comp, flt, lace = struct.unpack(">IIBBBBB", chunks[0][1])
assert (depth, colour, comp, flt, lace) == (8, 2, 0, 0, 0), "8-bit RGB, no interlace"
raw = zlib.decompress(chunks[1][1])  # also verifies the adler32
assert len(raw) == h * (1 + w * 3), "size"
want = bytes((i * 7 + (i >> 5)) & 255 for i in range(w * h * 3))
got = b"".join(raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)] for y in range(h))
assert all(raw[y * (1 + w * 3)] == 0 for y in range(h)), "filter type none"
assert got == want, "pixels"
print(f"png decoded independently: {w}x{h}, pixels match")
