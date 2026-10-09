#!/usr/bin/env python3
"""Generates Axial.ico: a teal rounded square with a white three-axis glyph.

Standard library only. Small sizes are stored as 32-bit DIBs and large sizes as
PNG, which every Windows icon consumer understands. Run from any directory:
    python3 make-icon.py [output.ico]
"""
import math
import os
import struct
import sys
import zlib

TOP, BOTTOM = (0x36, 0xC2, 0xCF), (0x10, 0x5E, 0x78)
CENTER = (0.5, 0.56)
ARMS = [(0.5, 0.17), (0.19, 0.74), (0.81, 0.74)]


def segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def inside_triangle(px, py, a, b, c):
    def side(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])
    s1, s2, s3 = side((px, py), a, b), side((px, py), b, c), side((px, py), c, a)
    return not ((s1 < 0 or s2 < 0 or s3 < 0) and (s1 > 0 or s2 > 0 or s3 > 0))


def rounded_square(px, py, radius):
    qx, qy = abs(px - 0.5) - (0.5 - radius), abs(py - 0.5) - (0.5 - radius)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - radius <= 0


def glyph(size):
    stroke = max(0.062, 1.5 / size) / 2
    head = max(0.085, 2.6 / size)
    shapes = []
    for tip in ARMS:
        dx, dy = tip[0] - CENTER[0], tip[1] - CENTER[1]
        length = math.hypot(dx, dy)
        ux, uy = dx / length, dy / length
        base = (tip[0] - ux * head * 1.25, tip[1] - uy * head * 1.25)
        triangle = (tip, (base[0] - uy * head * 0.75, base[1] + ux * head * 0.75), (base[0] + uy * head * 0.75, base[1] - ux * head * 0.75))
        shapes.append((CENTER, base, triangle))
    return stroke, shapes


def render(size):
    samples = 4 if size >= 64 else 6
    stroke, shapes = glyph(size)
    radius = 0.225
    pixels = bytearray()
    for y in range(size):
        for x in range(size):
            fill = white = 0
            for sy in range(samples):
                for sx in range(samples):
                    px, py = (x + (sx + 0.5) / samples) / size, (y + (sy + 0.5) / samples) / size
                    if not rounded_square(px, py, radius):
                        continue
                    fill += 1
                    if math.hypot(px - CENTER[0], py - CENTER[1]) <= stroke * 1.9:
                        white += 1
                        continue
                    for start, end, triangle in shapes:
                        if segment_distance(px, py, start[0], start[1], end[0], end[1]) <= stroke or inside_triangle(px, py, *triangle):
                            white += 1
                            break
            total = samples * samples
            alpha = fill / total
            if alpha == 0:
                pixels += b"\0\0\0\0"
                continue
            t = (y + 0.5) / size
            base = [TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3)]
            mix = white / fill
            rgb = [round(base[i] + (255 - base[i]) * mix) for i in range(3)]
            pixels += bytes(rgb) + bytes([round(alpha * 255)])
    return bytes(pixels)


def png(size, rgba):
    rows = b"".join(b"\0" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def dib(size, rgba):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    color = bytearray()
    for y in reversed(range(size)):
        for x in range(size):
            r, g, b, a = rgba[(y * size + x) * 4:(y * size + x) * 4 + 4]
            color += bytes([b, g, r, a])
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in reversed(range(size)):
        row = bytearray(stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bytes(color) + bytes(mask)


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "Axial.ico")
    images = []
    for size in (16, 20, 24, 32, 40, 48, 64, 128, 256):
        rgba = render(size)
        images.append((size, png(size, rgba) if size >= 128 else dib(size, rgba)))
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, image in images:
        data += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    with open(output, "wb") as file:
        file.write(data + b"".join(image for _, image in images))
    print(f"Wrote {output}")


if __name__ == "__main__":
    main()
