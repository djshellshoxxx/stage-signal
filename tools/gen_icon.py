#!/usr/bin/env python3
"""Procedurally generates src/app/res/app.ico (16/32/48 px, 32-bit BMP-based so it also loads on pre-Vista Windows).
Design: dark rounded square with five audio-level bars. No third-party artwork."""
import struct, sys

def render(n):
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]
    r = max(2, n // 6)
    def inside(x, y):
        cx = min(max(x, r), n - 1 - r); cy = min(max(y, r), n - 1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    heights = [0.35, 0.60, 0.90, 0.55, 0.40]
    cols = [(0, 200, 255), (60, 220, 160), (255, 255, 255), (60, 220, 160), (0, 200, 255)]
    m = n // 8; usable = n - 2 * m; bw = max(1, usable // 7)
    for y in range(n):
        for x in range(n):
            if not inside(x, y): continue
            px[y][x] = (24, 30, 58, 255)
            for i in range(5):
                bx = m + bw // 2 + i * (bw + max(1, bw // 2) if n > 16 else bw + 1)
                top = int(n - m - heights[i] * (n - 2 * m))
                if bx <= x < bx + bw and top <= y < n - m: px[y][x] = cols[i] + (255,)
    return px

def dib(n):
    px = render(n)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = px[y][x]; body += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4; mask = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if px[y][x][3] == 0: row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return hdr + bytes(body) + bytes(mask)

sizes = [16, 32, 48]; imgs = [dib(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes)); off = 6 + 16 * len(sizes)
for n, d in zip(sizes, imgs):
    out += struct.pack('<BBBBHHII', n, n, 0, 0, 1, 32, len(d), off); off += len(d)
for d in imgs: out += d
open(sys.argv[1] if len(sys.argv) > 1 else 'src/app/res/app.ico', 'wb').write(out)
