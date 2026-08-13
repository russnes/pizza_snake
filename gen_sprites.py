#!/usr/bin/env python3
"""Generate sprites.bmp: simple classic-snake-style dots, no face/rotation,
no per-segment direction (avoids jitter -- position-only updates every
frame). Single row layout so gfxoffset = starting column."""
from PIL import Image, ImageDraw

TILE = 8
LAYOUT = [
    ("head",       0, 8),
    ("pizza_big",  1, 16),
    ("pizza_small",3, 8),
    ("body",       4, 8),
]
TOTAL_COLS = 5
W, H = TILE * TOTAL_COLS, 16

PALETTE = [
    (255, 0, 255),   # 0 transparent
    (20, 20, 24),    # 1 outline/black
    (94, 220, 90),   # 2 head bright green
    (46, 140, 60),   # 3 body green
    (232, 194, 128), # 4 pizza crust tan
    (176, 126, 64),  # 5 crust edge brown
    (206, 60, 40),   # 6 sauce red
    (250, 214, 96),  # 7 cheese yellow
    (150, 34, 24),   # 8 pepperoni dark
]

im = Image.new("P", (W, H), 0)
flat = []
for c in PALETTE:
    flat.extend(c)
flat.extend([0, 0, 0] * (256 - len(PALETTE)))
im.putpalette(flat)
draw = ImageDraw.Draw(im)


def origin(name):
    for n, col, size in LAYOUT:
        if n == name:
            return col * TILE, 0, size
    raise KeyError(name)


def draw_pizza(name):
    ox, oy, size = origin(name)
    cx, cy = ox + size // 2, oy + size // 2
    r = size // 2 - 1
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=5, outline=1)
    draw.ellipse([cx - r + 1, cy - r + 1, cx + r - 1, cy + r - 1], fill=7)
    if size >= 16:
        for (dx_, dy_) in [(-3, -2), (2, -3), (0, 2), (-2, 3), (3, 2)]:
            px, py = cx + dx_, cy + dy_
            draw.ellipse([px - 1.3, py - 1.3, px + 1.3, py + 1.3], fill=8)
    else:
        draw.point([(cx - 1, cy - 1), (cx + 1, cy + 1)], fill=8)


def draw_dot(name, fill, outline=1, r=2):
    ox, oy, size = origin(name)
    cx, cy = ox + size // 2, oy + size // 2
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline)


def draw_body_dot(name, fill):
    """Precise 5x5 rounded-square dot -- avoids PIL ellipse's unpredictable
    rounding at tiny sizes, and matches the 4px-thick H/V/corner BG tiles
    closely enough that the sprite and background portions read as the
    same width."""
    ox, oy, size = origin(name)
    cx, cy = ox + size // 2, oy + size // 2
    for dy_ in range(-2, 3):
        for dx_ in range(-2, 3):
            if abs(dx_) == 2 and abs(dy_) == 2:
                continue  # round off the corners
            im.putpixel((cx + dx_, cy + dy_), fill)


draw_dot("head", fill=2)
draw_pizza("pizza_big")
draw_pizza("pizza_small")
draw_body_dot("body", fill=3)

im.save("sprites.bmp")
print("wrote sprites.bmp", im.size)

names = {
    "SPR_HEAD": "head",
    "SPR_PIZZA_BIG": "pizza_big",
    "SPR_PIZZA_SMALL": "pizza_small",
    "SPR_BODY": "body",
}
with open("sprite_tiles.h", "w") as f:
    f.write("// Auto-generated OBJ tile offsets (single-row layout, offset = start column)\n")
    for define, name in names.items():
        col, _, _ = origin(name)
        f.write(f"#define {define} {col // TILE}\n")
