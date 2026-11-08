#!/usr/bin/env python3
"""Render the PicoSwitch2 Companion icon.

The icon is generated rather than hand-drawn so it is reproducible from source:
a rounded indigo tile with a white gamepad glyph. Drawn at 4x and downsampled
for clean edges.

    python make_icon.py <out.png> [size]

Requires Pillow. The committed PNGs were produced with this script; re-run it
after changing the design and commit the outputs together.
"""

import sys

from PIL import Image, ImageDraw

SCALE = 4
BASE = 256


def render(size: int) -> Image.Image:
    s = BASE * SCALE
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # Tile: vertical gradient, masked to a rounded square.
    top, bottom = (79, 91, 213), (44, 48, 140)
    tile = Image.new("RGBA", (s, s))
    px = tile.load()
    for y in range(s):
        t = y / (s - 1)
        colour = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(s):
            px[x, y] = colour
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, s - 1, s - 1), radius=56 * SCALE, fill=255)
    image.paste(tile, (0, 0), mask)

    d = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    ink = (44, 48, 140, 255)
    u = SCALE

    # Gamepad body: a bar with two grips.
    d.rounded_rectangle((44 * u, 92 * u, 212 * u, 158 * u), radius=33 * u, fill=white)
    d.ellipse((40 * u, 104 * u, 112 * u, 198 * u), fill=white)
    d.ellipse((144 * u, 104 * u, 216 * u, 198 * u), fill=white)

    # D-pad.
    d.rounded_rectangle((70 * u, 114 * u, 82 * u, 150 * u), radius=3 * u, fill=ink)
    d.rounded_rectangle((58 * u, 126 * u, 94 * u, 138 * u), radius=3 * u, fill=ink)

    # Face buttons.
    for cx, cy in ((186, 120), (186, 144), (174, 132), (198, 132)):
        r = 7
        d.ellipse(((cx - r) * u, (cy - r) * u, (cx + r) * u, (cy + r) * u), fill=ink)

    # The bridge: a small link between the halves, for "translation".
    d.ellipse((118 * u, 122 * u, 138 * u, 142 * u), fill=ink)

    return image.resize((size, size), Image.LANCZOS)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    size = int(sys.argv[2]) if len(sys.argv) > 2 else BASE
    render(size).save(sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
