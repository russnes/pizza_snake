#!/usr/bin/env python3
"""Generate tiles1.bmp: 8x8 BG tileset with a full hand-authored 5x7 pixel font
(A-Z, 0-9), a wall tile, and a set of body-connector tiles used to render
the baked (permanent) portion of the snake's tail -- one for every possible
combination of where the path enters and exits an 8x8 cell, so the baked
tail matches the snake's actual path pixel-for-pixel instead of snapping
every turn to one of a handful of fixed shapes.
tile0 = blank (floor/space), tiles1..N = chars, then wall, then the body
connector tiles.
"""
import math
from PIL import Image, ImageDraw

# --- body-connector touch points and tile generation -----------------------
# Every touch point where the snake's path crosses a tile boundary is
# represented as (edge, along) -- which of the 4 edges (0=left,1=right,
# 2=top,3=bottom), and which of its 8 pixel positions -- packed into a
# single 0-31 index, rather than a raw (x,y). A point sitting exactly at a
# tile corner is genuinely ambiguous as a bare coordinate (simultaneously
# "column 0" and "row 0"), and the two readings need the connector to
# reach in different directions, so the edge has to be explicit rather
# than re-derived later.
#
# Every one of the 32x32 possible (entry,exit) touch-point pairs gets its
# own generated connector shape, reduced to the handful of unique
# physical tiles needed via hflip/vflip (the only symmetries SNES
# tilemap attributes actually give for free -- NOT a runtime transpose,
# since the hardware can't rotate a tile, only mirror it). The result is
# a 32x32 lookup table mapping any raw touch-point pair straight to a
# tile id + flip bits, emitted below as tailConnectorTile[].
THICK = 2.0   # half-thickness of the body band, matches the sprite's width
PULL = 0.55   # how far the curve's control point is pulled toward the tile centre
STUB = THICK


def _edge_xy(edge, along):
    if edge == 0: return (0, along)
    if edge == 1: return (7, along)
    if edge == 2: return (along, 0)
    return (along, 7)


def _inward(edge, along, dist):
    x, y = _edge_xy(edge, along)
    if edge == 0: return (x + dist, y)
    if edge == 1: return (x - dist, y)
    if edge == 2: return (x, y + dist)
    return (x, y - dist)


def _point_index(edge, along):
    return edge * 8 + along


def _point_unpack(idx):
    return idx // 8, idx % 8


def _hflip_point(idx):
    e, a = _point_unpack(idx)
    if e == 0: return _point_index(1, a)
    if e == 1: return _point_index(0, a)
    return _point_index(e, 7 - a)  # top/bottom: column mirrors


def _vflip_point(idx):
    e, a = _point_unpack(idx)
    if e == 2: return _point_index(3, a)
    if e == 3: return _point_index(2, a)
    return _point_index(e, 7 - a)  # left/right: row mirrors


def _connector_mask(a_idx, b_idx):
    """Rasterise a constant-thickness curve between two boundary touch
    points. Interior pixels use distance to a bezier body curve that
    bulges toward the tile centre; each of the 4 boundary lines is
    instead rendered SEPARATELY, as a simple band around its own touch
    point (if it has one), so a touch on one edge can never leak into a
    different edge's rendering. That matters because two neighbouring
    tiles are compared edge-to-edge: any such leak would show up as a
    mismatch against a neighbour that has no corresponding feature at
    all (this is also why a band never reaches all the way to position 0
    or 7 -- those pixels are shared with the perpendicular edge)."""
    ea, aa = _point_unpack(a_idx)
    eb, ab = _point_unpack(b_idx)
    a_body = _inward(ea, aa, STUB)
    b_body = _inward(eb, ab, STUB)

    cx, cy = (a_body[0] + b_body[0]) / 2, (a_body[1] + b_body[1]) / 2
    ctrlx = cx + (3.5 - cx) * PULL
    ctrly = cy + (3.5 - cy) * PULL

    curve_pts = []
    BODY_STEPS = 32
    for s in range(BODY_STEPS + 1):
        t = s / BODY_STEPS
        qx = (1-t)**2 * a_body[0] + 2*(1-t)*t * ctrlx + t**2 * b_body[0]
        qy = (1-t)**2 * a_body[1] + 2*(1-t)*t * ctrly + t**2 * b_body[1]
        curve_pts.append((qx, qy))

    mask = [[False]*8 for _ in range(8)]
    for yy in range(1, 7):
        for xx in range(1, 7):
            px, py = xx + 0.5, yy + 0.5
            mind = min((px-qx)**2 + (py-qy)**2 for (qx, qy) in curve_pts)
            mask[yy][xx] = mind <= THICK*THICK

    for edge, along in ((ea, aa), (eb, ab)):
        lo = int(math.ceil(max(1, along - THICK)))
        hi = int(math.floor(min(6, along + THICK)))
        if edge == 0:
            for y in range(lo, hi+1): mask[y][0] = True
        elif edge == 1:
            for y in range(lo, hi+1): mask[y][7] = True
        elif edge == 2:
            for x in range(lo, hi+1): mask[0][x] = True
        else:
            for x in range(lo, hi+1): mask[7][x] = True

    return mask


def _canon(a, b):
    """The lexicographically smallest of (a,b)'s 8 hflip/vflip/swap
    variants, plus which flips produced it -- applying those SAME flips
    to the canonical tile's art reproduces (a,b)'s intended shape, since
    hflip and vflip are each their own inverse."""
    variants = []
    for pa, pb in ((a, b), (b, a)):
        for fx in (False, True):
            for fy in (False, True):
                p1, p2 = pa, pb
                if fx: p1, p2 = _hflip_point(p1), _hflip_point(p2)
                if fy: p1, p2 = _vflip_point(p1), _vflip_point(p2)
                key = (p1, p2) if p1 <= p2 else (p2, p1)
                variants.append((key, fx, fy))
    variants.sort(key=lambda v: v[0])
    return variants[0]


_canonical_tile_of = {}          # canonical (a,b) key -> local tile index
_connector_lookup = [None] * (32*32)   # raw a*32+b -> (local tile index, hflip, vflip)
for _a in range(32):
    for _b in range(32):
        _ckey, _fx, _fy = _canon(_a, _b)
        if _ckey not in _canonical_tile_of:
            _canonical_tile_of[_ckey] = len(_canonical_tile_of)
        _connector_lookup[_a*32 + _b] = (_canonical_tile_of[_ckey], _fx, _fy)
BODY_TILE_COUNT = len(_canonical_tile_of)

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
N_TILES = 1 + len(CHARS) + 1 + BODY_TILE_COUNT  # blank + chars + wall + body-connector tiles
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

# Body connector tiles: one generated tile per canonical touch-point pair
# (see _connector_mask / _canon above), indexed contiguously from
# body_base_idx. tailConnectorTile[] (emitted below) maps any raw
# (entry,exit) pair straight to one of these, so there's no per-tile
# special-casing here -- just render whichever pair each canonical slot
# was assigned.
body_base_idx = wall_idx + 1
_tile_pair_of = {v: k for k, v in _canonical_tile_of.items()}
for _tile_id in range(BODY_TILE_COUNT):
    _pa, _pb = _tile_pair_of[_tile_id]
    _mask = _connector_mask(_pa, _pb)
    _ox, _oy = cell_origin(body_base_idx + _tile_id)
    for _y in range(8):
        for _x in range(8):
            if _mask[_y][_x]:
                im.putpixel((_ox + _x, _oy + _y), 5)

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
    f.write(f"#define TILE_BODY_BASE {body_base_idx}\n")
    f.write(f"#define TILE_BODY_COUNT {BODY_TILE_COUNT}\n")
    f.write("static const u8 asciiTile[128] = {\n    ")
    f.write(", ".join(str(v) for v in ascii_table))
    f.write("\n};\n\n")

    f.write("// entry_point*32 + exit_point -> body tile index | hflip(0x4000) | vflip(0x8000)\n")
    f.write("static const u16 tailConnectorTile[1024] = {\n")
    for row in range(32):
        vals = []
        for col in range(32):
            tile_id, fx, fy = _connector_lookup[row*32 + col]
            v = body_base_idx + tile_id
            if fx: v |= 0x4000
            if fy: v |= 0x8000
            vals.append(str(v))
        f.write("    " + ", ".join(vals) + ",\n")
    f.write("};\n")
