#!/usr/bin/env python3
"""Generates apps/asm3d.ico (the ASM3D program icon) procedurally: a rounded
blue square with a white isometric cube. No external tools or assets."""
import struct, math, os

def render(size):
    px = []
    r = size * 0.18
    for y in range(size):
        for x in range(size):
            # rounded square mask (anti-aliased)
            cx = min(max(x + 0.5, r), size - r); cy = min(max(y + 0.5, r), size - r)
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            a = max(0.0, min(1.0, r - d + 0.5))
            t = y / size
            col = (int(40 + 30 * t), int(110 + 40 * t), int(230 - 30 * t))
            # isometric cube: three faces
            u = (x + 0.5) / size - 0.5; v = (y + 0.5) / size - 0.52
            s = 0.30
            face = None
            top = abs(u) / 0.866 + abs(v + s * 0.5) * 2 <= s * 1.0 and v < 0
            if abs(u) <= s * 0.866:
                if v <= 0 and abs(u) / 0.866 * 0.5 + (-v) <= s and abs(u) / 0.866 * 0.5 - v >= 0:
                    face = 'top' if -v >= abs(u) * 0.577 else None
                ylow = abs(u) * 0.577
                if face is None and v >= -ylow and v <= s - ylow + s * 0 and v <= s + ylow * 0 - abs(u) * 0.577 + s:
                    if v <= s * 1.0 - (abs(u) * 0.577) + 0.0 and v >= -(abs(u) * 0.577):
                        face = 'left' if u < 0 else 'right'
            if face == 'top' and -v > s - abs(u) * 0.577: face = None
            if face in ('left', 'right') and v > s - abs(u) * 0.577 + s * 0.0 + (s - s): pass
            if face == 'top': col = (250, 250, 255)
            elif face == 'left': col = (205, 220, 250)
            elif face == 'right': col = (160, 185, 240)
            px.append((col[2], col[1], col[0], int(a * 255)))
    return px

def bmp_entry(size):
    px = render(size)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = b''.join(bytes(c for p in px[y * size:(y + 1) * size] for c in p) for y in reversed(range(size)))
    mask = b'\x00' * (((size + 31) // 32) * 4 * size)
    return header + rows + mask

sizes = [16, 32, 48, 64]
images = [bmp_entry(s) for s in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
for s, img in zip(sizes, images):
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
    offset += len(img)
out += b''.join(images)
path = os.path.join(os.path.dirname(__file__), '..', 'apps', 'asm3d.ico')
open(path, 'wb').write(out)
print('wrote', os.path.normpath(path), len(out), 'bytes')
