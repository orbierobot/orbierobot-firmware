#!/usr/bin/env python3
"""Generate main/demo_expressions.h — heart / star / loader / rainbow eye animations.

Frames are 16x8 RGB, row-major, index = (y*16 + x)*3, exactly like the existing
boot_expressions.h. x 0-7 is the left eye, x 8-15 the right. led_display.c applies
the physical eye rotation, so everything here is in logical screen space.

The heart and rainbow reuse shapes already drawn in joystick_expressions.h so the
demo animations match the face the robot already has.

Usage:  python3 tools/gen_demo_expressions.py
"""
import re
import colorsys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "main" / "joystick_expressions.h"
OUT = ROOT / "main" / "demo_expressions.h"

W, H = 16, 8


def load_mask(name):
    """Pull a 384-byte array out of the existing header and reduce it to an on/off mask."""
    text = SRC.read_text()
    m = re.search(rf"const uint8_t {name}\[384\]\s*=\s*{{(.*?)}};", text, re.S)
    if not m:
        raise SystemExit(f"could not find {name} in {SRC}")
    vals = [int(v) for v in re.findall(r"\d+", re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))]
    if len(vals) != 384:
        raise SystemExit(f"{name}: expected 384 values, got {len(vals)}")
    return [1 if any(vals[(y * W + x) * 3 + c] for c in range(3)) else 0
            for y in range(H) for x in range(W)]


def mask_from_rows(rows8):
    """Build a 16x8 mask by mirroring one 8x8 glyph into both eyes."""
    mask = [0] * (W * H)
    for y, row in enumerate(rows8):
        for x, ch in enumerate(row):
            if ch != ".":
                mask[y * W + x] = 1
                mask[y * W + x + 8] = 1
    return mask


STAR = mask_from_rows([
    "...XX...",
    "...XX...",
    "XXXXXXXX",
    ".XXXXXX.",
    "..XXXX..",
    "..XXXX..",
    ".XX..XX.",
    ".X....X.",
])


def frame(mask, colour_at):
    """Render a mask to 384 bytes. colour_at(x, y) -> (r, g, b)."""
    buf = []
    for y in range(H):
        for x in range(W):
            buf.extend(colour_at(x, y) if mask[y * W + x] else (0, 0, 0))
    return buf


def scale(rgb, k):
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


# ---------------------------------------------------------------- animations

def heart_frames(mask):
    """Pulse red like a heartbeat: two quick beats then a rest."""
    beats = [0.45, 0.70, 1.00, 0.75, 0.50, 0.85, 1.00, 0.70, 0.50, 0.45]
    return [(frame(mask, lambda x, y, k=k: scale((255, 30, 40), k)), 90) for k in beats]


def star_frames(mask):
    """Warm yellow with a twinkle that sweeps brightness across the glyph."""
    out = []
    for i in range(12):
        phase = i / 12.0

        def colour(x, y, phase=phase):
            # diagonal shimmer travelling across the star
            d = ((x % 8) + y) / 14.0
            k = 0.55 + 0.45 * (0.5 + 0.5 * __import__("math").cos(2 * 3.14159 * (d - phase)))
            return scale((255, 190, 0), k)

        out.append((frame(mask, colour), 80))
    return out


def loader_frames():
    """Green arc rotating around a ring, with a fading tail."""
    import math
    cx = cy = 3.5
    radius = 2.9
    steps = 12
    ring = []
    for i in range(steps):
        a = 2 * math.pi * i / steps - math.pi / 2
        ring.append((round(cx + radius * math.cos(a)), round(cy + radius * math.sin(a))))

    out = []
    for head in range(steps):
        buf = [0] * (W * H * 3)
        for t in range(5):  # head plus 4 tail pixels
            px, py = ring[(head - t) % steps]
            if not (0 <= px < 8 and 0 <= py < H):
                continue
            k = 1.0 - t * 0.22
            r, g, b = scale((0, 255, 90), k)
            for eye in (0, 8):
                idx = (py * W + px + eye) * 3
                buf[idx:idx + 3] = [r, g, b]
        out.append((buf, 70))
    return out


def rainbow_frames(mask):
    """Keep the normal eye shape, sweep the full hue range."""
    out = []
    steps = 24
    for i in range(steps):
        hue = i / steps

        def colour(x, y, hue=hue):
            # slight horizontal offset so the two eyes aren't identical
            h = (hue + (x % 8) / 32.0) % 1.0
            r, g, b = colorsys.hsv_to_rgb(h, 1.0, 1.0)
            return (int(r * 255), int(g * 255), int(b * 255))

        out.append((frame(mask, colour), 60))
    return out


# ---------------------------------------------------------------- emit

def emit(f, name, frames):
    for i, (buf, _) in enumerate(frames):
        f.write(f"const uint8_t {name}_f{i}[384] = {{\n")
        for y in range(H):
            row = buf[y * W * 3:(y + 1) * W * 3]
            cells = ", ".join(f"{row[x*3]:3d},{row[x*3+1]:3d},{row[x*3+2]:3d}" for x in range(W))
            f.write(f"  /* y={y} */ {cells}{',' if y < H-1 else ''}\n")
        f.write("};\n\n")
    f.write(f"const ExprFrame {name}_anim[] = {{\n")
    for i, (_, dur) in enumerate(frames):
        f.write(f"  {{ {name}_f{i}, {dur} }},\n")
    f.write("};\n")
    f.write(f"#define {name.upper()}_FRAME_COUNT {len(frames)}\n\n")


def main():
    heart = load_mask("expr_joy_heart")
    eye = load_mask("expr_joy_center")

    with OUT.open("w") as f:
        f.write("""#pragma once
/* GENERATED by tools/gen_demo_expressions.py - do not edit by hand.
 *
 * Demo eye animations: red heart, yellow star, green loader, rainbow cycle.
 * Frames are 16x8 RGB (384 bytes), same format as boot_expressions.h.
 * ExprFrame is declared in led_display.h, which must be included first.
 */

""")
        emit(f, "expr_heart", heart_frames(heart))
        emit(f, "expr_star", star_frames(STAR))
        emit(f, "expr_loader", loader_frames())
        emit(f, "expr_rainbow", rainbow_frames(eye))

    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
