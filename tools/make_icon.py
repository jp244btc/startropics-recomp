"""Draws the port's icon (original artwork: a sunset over the sea with a palm
silhouette) and writes it as a Windows .ico (16/32/48/256), a PNG preview and
a C array for SDL_SetWindowIcon. Pure Python, no image libraries.

usage: python tools/make_icon.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_ICO = os.path.join(ROOT, "frontend", "startropics.ico")
OUT_PNG = os.path.join(ROOT, "frontend", "icon_preview.png")
OUT_C = os.path.join(ROOT, "frontend", "icon_data.c")


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def dist_seg(px, py, ax, ay, bx, by):
    """distance from P to segment AB"""
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    l2 = vx * vx + vy * vy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / l2))
    dx, dy = px - (ax + t * vx), py - (ay + t * vy)
    return math.hypot(dx, dy)


def trunk_point(t):
    """quadratic curve for the palm trunk, from the ground to the crown"""
    x0, y0, x1, y1, cx, cy = 0.74, 0.98, 0.70, 0.40, 0.84, 0.70
    x = (1 - t) ** 2 * x0 + 2 * (1 - t) * t * cx + t * t * x1
    y = (1 - t) ** 2 * y0 + 2 * (1 - t) * t * cy + t * t * y1
    return x, y


TRUNK = [trunk_point(i / 24) for i in range(25)]
CROWN = (0.70, 0.40)
FRONDS = []
for ang in (200, 235, 270, 305, 340, 160):
    a = math.radians(ang)
    length = 0.26 if ang not in (160, 340) else 0.22
    FRONDS.append((CROWN[0] + math.cos(a) * length, CROWN[1] + math.sin(a) * length * 0.75 + 0.06))


def color_at(u, v):
    """RGBA (0..1) of the artwork at normalised coordinates"""
    # rounded-square badge
    r = 0.22
    cx, cy = min(max(u, r), 1 - r), min(max(v, r), 1 - r)
    if math.hypot(u - cx, v - cy) > r:
        return (0, 0, 0, 0)
    # sky: deep blue at the top to orange at the horizon (v = 0.62)
    horizon = 0.62
    if v < horizon:
        t = v / horizon
        col = lerp((0.09, 0.13, 0.36), (0.96, 0.48, 0.16), t ** 1.6)
    else:
        t = (v - horizon) / (1 - horizon)
        col = lerp((0.04, 0.40, 0.56), (0.02, 0.18, 0.36), t)
        # light ripples on the water
        if int((v - horizon) * 60) % 4 == 0 and (int(u * 40 + v * 90) % 3 == 0):
            col = lerp(col, (0.95, 0.75, 0.45), 0.35)
    # sun
    if math.hypot((u - 0.40) * 1.0, (v - 0.50) * 1.0) < 0.17 and v < horizon:
        col = (1.0, 0.86, 0.35) if math.hypot(u - 0.40, v - 0.50) < 0.14 else (1.0, 0.72, 0.28)
    # palm silhouette
    ink = (0.06, 0.06, 0.08)
    for i in range(len(TRUNK) - 1):
        ax, ay = TRUNK[i]
        bx, by = TRUNK[i + 1]
        if dist_seg(u, v, ax, ay, bx, by) < 0.030 - 0.012 * (i / len(TRUNK)):
            return (*ink, 1.0)
    for fx, fy in FRONDS:
        # curved frond: bend the segment slightly downwards along its length
        for k in range(8):
            t0, t1 = k / 8, (k + 1) / 8
            ax, ay = CROWN[0] + (fx - CROWN[0]) * t0, CROWN[1] + (fy - CROWN[1]) * t0 + 0.05 * t0 * t0
            bx, by = CROWN[0] + (fx - CROWN[0]) * t1, CROWN[1] + (fy - CROWN[1]) * t1 + 0.05 * t1 * t1
            if dist_seg(u, v, ax, ay, bx, by) < 0.028 * (1 - 0.6 * t0):
                return (*ink, 1.0)
    if math.hypot(u - CROWN[0], v - CROWN[1]) < 0.045:
        return (*ink, 1.0)
    return (*col, 1.0)


def render(size, ss=4):
    px = bytearray()
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    c = color_at(u, v)
                    acc[0] += c[0] * c[3]; acc[1] += c[1] * c[3]; acc[2] += c[2] * c[3]; acc[3] += c[3]
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                rgb = [acc[i] / acc[3] for i in range(3)]
            else:
                rgb = [0, 0, 0]
            px += bytes(int(max(0, min(1, c)) * 255 + 0.5) for c in rgb) + bytes([int(a * 255 + 0.5)])
    return bytes(px)


def png(size, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_entry(size, rgba):
    """32-bit DIB with an empty AND mask, rows bottom-up, BGRA"""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = rgba[y * size * 4:(y + 1) * size * 4]
        rows.append(b"".join(bytes((row[i + 2], row[i + 1], row[i], row[i + 3])) for i in range(0, len(row), 4)))
    mask_row = b"\x00" * (((size + 31) // 32) * 4)
    return header + b"".join(rows) + mask_row * size


def main():
    sizes = (16, 32, 48, 256)
    images = {}
    for s in sizes:
        images[s] = render(s, ss=6 if s <= 48 else 2)
        print(f"rendered {s}x{s}")
    entries = []
    for s in sizes:
        data = png(s, images[s]) if s == 256 else bmp_entry(s, images[s])
        entries.append((s, data))
    offset = 6 + 16 * len(entries)
    ico = struct.pack("<HHH", 0, 1, len(entries))
    body = b""
    for s, data in entries:
        ico += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    with open(OUT_ICO, "wb") as f:
        f.write(ico + body)
    with open(OUT_PNG, "wb") as f:
        f.write(png(256, images[256]))
    w = 64
    data = render(w, ss=4)
    with open(OUT_C, "w", newline="\n") as f:
        f.write("/* generated by tools/make_icon.py: the window icon, 64x64 RGBA (original artwork) */\n")
        f.write("#include <stdint.h>\n")
        f.write(f"const int icon_width = {w}, icon_height = {w};\n")
        f.write(f"const uint8_t icon_rgba[{w * w * 4}] = {{\n")
        for i in range(0, len(data), 32):
            f.write("    " + ",".join(str(b) for b in data[i:i + 32]) + ",\n")
        f.write("};\n")
    print("wrote", OUT_ICO, OUT_PNG, OUT_C)


if __name__ == "__main__":
    main()
