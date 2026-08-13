#!/usr/bin/env python3
"""Generate tiles1.bmp: 8x8 BG tileset with a full hand-authored 5x7 pixel font
(A-Z, 0-9), a wall tile, and 3 body-connector tiles (H, V, corner) used to
render the baked (permanent) portion of the snake's tail.
tile0 = blank (floor/space), tiles1..N = chars, then wall, then H/V/corner.
"""
import math
from PIL import Image, ImageDraw

FONT5x7 = {
    '0': [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    '1': ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    '2': [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    '3': [".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."],
    '4': ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    '5': ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    '6': ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    '7': ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    '8': [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    '9': [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'B': ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    'C': [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    'D': ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    'E': ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    'F': ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    'G': [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    'H': ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'I': [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    'J': ["..###", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##.."],
    'K': ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    'L': ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    'O': [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'P': ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    'Q': [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    'R': ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    'U': ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'V': ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    'W': ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    'X': ["#...#", ".#.#.", "..#..", "..#..", "..#..", ".#.#.", "#...#"],
    'Y': ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    'Z': ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
}
CHARS = list(FONT5x7.keys())

TILE = 8
N_TILES = 1 + len(CHARS) + 1 + 3  # blank + chars + wall + H/V/corner body-connector tiles
COLS = 7
ROWS = (N_TILES + COLS - 1) // COLS
W, H = TILE * COLS, TILE * ROWS

PALETTE = [
    (8, 8, 14),      # 0 background/transparent
    (255, 235, 110), # 1 text fill (pizza yellow, bright for contrast)
    (40, 20, 16),    # 2 text outline (also body dot outline)
    (176, 60, 40),   # 3 wall red
    (232, 200, 150), # 4 wall tan
    (46, 140, 60),   # 5 baked body green - matches sprite body
]

im = Image.new("P", (W, H), 0)
flat = []
for c in PALETTE:
    flat.extend(c)
flat.extend([0, 0, 0] * (256 - len(PALETTE)))
im.putpalette(flat)
draw = ImageDraw.Draw(im)


def cell_origin(idx):
    return (idx % COLS) * TILE, (idx // COLS) * TILE


for i, ch in enumerate(CHARS):
    idx = 1 + i
    ox, oy = cell_origin(idx)
    rows = FONT5x7[ch]
    for ry, row in enumerate(rows):
        for rx, cell in enumerate(row):
            if cell == '#':
                im.putpixel((ox + 1 + rx, oy + ry), 1)

wall_idx = 1 + len(CHARS)
ox, oy = cell_origin(wall_idx)
draw.rectangle([ox, oy, ox + 7, oy + 7], fill=3)
draw.rectangle([ox, oy, ox + 7, oy + 7], outline=2)
draw.rectangle([ox + 1, oy + 1, ox + 6, oy + 6], fill=4)
draw.rectangle([ox + 2, oy + 2, ox + 5, oy + 5], fill=3)

# Body connector tiles: H (straight, connects left-mid to right-mid), V
# (straight, connects top-mid to bottom-mid), and a corner (connects
# top-mid to left-mid; the other 3 corner orientations are the same art
# with hflip/vflip applied in-game). All three are 4px thick and touch
# their edges at the SAME band (columns/rows 2-5), so whichever
# combination of tiles ends up adjacent, they connect with consistent,
# constant width -- and picking a pre-made tile is far cheaper at runtime
# than computing a custom shape per tile.
body_h_idx = wall_idx + 1
ox, oy = cell_origin(body_h_idx)
draw.rectangle([ox, oy + 2, ox + 7, oy + 5], fill=5)

body_v_idx = wall_idx + 2
ox, oy = cell_origin(body_v_idx)
draw.rectangle([ox + 2, oy, ox + 5, oy + 7], fill=5)

body_corner_idx = wall_idx + 3
ox, oy = cell_origin(body_corner_idx)
for _y in range(8):
    for _x in range(8):
        _d = math.sqrt(_x * _x + _y * _y)
        if 1.5 <= _d <= 5.2:
            im.putpixel((ox + _x, oy + _y), 5)

im.save("tiles1.bmp")
print("wrote tiles1.bmp", im.size, "tiles:", N_TILES, "wall_idx:", wall_idx)

ascii_table = [0] * 128
for i, ch in enumerate(CHARS):
    ascii_table[ord(ch)] = 1 + i
ascii_table[ord(' ')] = 0

with open("tiles_charmap.h", "w") as f:
    f.write("// Auto-generated ascii -> tile index table\n")
    f.write("#define TILE_BLANK 0\n")
    f.write(f"#define TILE_WALL {wall_idx}\n")
    f.write(f"#define TILE_BODY_H {body_h_idx}\n")
    f.write(f"#define TILE_BODY_V {body_v_idx}\n")
    f.write(f"#define TILE_BODY_CORNER {body_corner_idx}\n")
    f.write("static const u8 asciiTile[128] = {\n    ")
    f.write(", ".join(str(v) for v in ascii_table))
    f.write("\n};\n")
