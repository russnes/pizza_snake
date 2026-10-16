# Pizza Snake (SNES)

A from-scratch classic snake SNES game,
written in C for the 65816 using the [pvsneslib](https://github.com/alekmaul/pvsneslib)
homebrew devkit.

## Features

- Continuous, angle-based movement (turn left/right, not grid-based Snake)
- Effectively unlimited snake length: the most recent 30 segments are
  smooth-moving sprites, and everything older is permanently baked onto the
  background layer as the snake grows past the SNES's sprite limits — see
  "How the long tail works" below
- Pizzas of two sizes/point values, randomly spawned
- 20-entry high score table with on-screen name entry (8 characters: A–Z, 0–9
  and space), **saved to battery-backed SRAM so it survives power-off**
- After every game you land on the high score table (your new entry blinks);
  it can also be opened from the title screen
- Title screen, game-over screen, name-entry screen, high-score screen

## Directory contents

| File                  | Purpose                                                          |
|------------------------|-------------------------------------------------------------------|
| `psnake.c`             | Main game source                                                  |
| `hdr.asm`               | SNES ROM header (title, mapping mode, etc.)                       |
| `data.asm`              | Links the compiled graphics/palette data into the ROM              |
| `Makefile`              | Build script                                                      |
| `gen_sprites.py`        | Generates `sprites.bmp` (snake head/body/pizza sprite sheet)        |
| `gen_tiles.py`          | Generates `tiles1.bmp` (font, wall, and baked-tail background tiles) |
| `gen_trig.py`           | Generates `trig_table.h` (sin/cos lookup table for movement)       |

Running `make` regenerates `sprites.bmp`, `tiles1.bmp`, `trig_table.h`,
`sprite_tiles.h`, and `tiles_charmap.h` automatically via the three Python
scripts above, then converts the bitmaps to SNES tile data and compiles the
ROM — you don't need to run the Python scripts yourself unless you want to
edit the generated art/tables directly.

## Prerequisites

- **Python 3** with **Pillow** (`pip install pillow`) — used only at build
  time to generate the sprite sheet, background tileset, and trig table.
- **pvsneslib** (includes the 65816 C compiler, assembler, and linker).
  Prebuilt Linux binaries are the easiest route:

  ```bash
  curl -L -o pvsneslib.zip \
    https://github.com/alekmaul/pvsneslib/releases/download/4.3.0/pvsneslib_430_64b_linux_release.zip
  mkdir pvsneslib_install && cd pvsneslib_install
  unzip ../pvsneslib.zip
  chmod +x pvsneslib/devkitsnes/bin/*
  ```

  This gives you a `pvsneslib/` folder containing `pvsneslib/` (headers/libs)
  and `devkitsnes/` (compiler toolchain). Windows/macOS users, or anyone
  wanting a newer version, should follow pvsneslib's own installation guide:
  <https://github.com/alekmaul/pvsneslib/wiki/Installation>

## Building

From this directory, with the prerequisites above installed:

```bash
export PVSNESLIB_HOME=/path/to/pvsneslib_install/pvsneslib
export PATH=$PATH:$PVSNESLIB_HOME/devkitsnes/bin
make
```

This produces `pizzasnake.sfc` — a standard LoROM SNES ROM you can run in
any SNES emulator (snes9x, Mesen-S, bsnes/higan, RetroArch's snes9x/bsnes
cores) or on real hardware via a flashcart (SD2SNES/FXPak, Super Everdrive,
etc).

`make clean` removes build artifacts (does not delete the generated
`.bmp`/`.h` asset files — delete those manually if you want a fully fresh
regeneration from the Python scripts).

## Controls

- **D-pad Left/Right** — turn
- **Start** — begin game / confirm on menus
- **Select** — view high scores (from the title screen)
- On the name-entry screen: **Left/Right** move the cursor, **Up/Down**
  cycle the character (hold to auto-repeat), **Start** confirms. The name
  starts as `A` followed by spaces, so a short name needs no cleanup.
- On the high score table: **Start**, **A** or **B** returns to the title

## High scores and saving

Scores are stored in the cartridge's battery-backed SRAM (declared in
`hdr.asm`: `CARTRIDGETYPE $02`, `SRAMSIZE $01` = 2 KB; about 210 bytes are
used). Emulators keep this in a `.srm` file next to the ROM (e.g.
`pizzasnake.srm`); flashcarts such as the FXPak/SD2SNES read the SRAM size from
the ROM header and keep it on the SD card.

The save is written whenever a new high score is entered, and read once at
power-on. Layout (see `saveHighScores()` in `psnake.c`):

| Bytes     | Contents                                              |
|-----------|-------------------------------------------------------|
| 0–3       | magic `"PZSN"`                                        |
| 4         | `NAME_LEN` (8)                                        |
| 5         | `NUM_SCORES` (20)                                     |
| 6–205     | 20 entries of: 8 name bytes, score low, score high    |
| 206–207   | checksum of everything before it (low, high)          |

If the SRAM is blank, corrupt, or was written with different `NAME_LEN` /
`NUM_SCORES` values, the game starts with an empty table (and writes it back)
rather than misreading the data. If no SRAM is available at all (e.g. a
flashcart set up without it) the game still works; scores just last until
power-off.

To **reset the table**, delete the `.srm` file.

## How the long tail works

The SNES can only display 128 hardware sprites at once -- and, more
restrictively, only **32 on any one scanline** -- so a snake that's allowed to
grow indefinitely can't be *all* sprites forever. The most recent 30 segments
(nearest the head) are rendered as sprites and move smoothly every frame
(`SPRITE_WINDOW`). Once a segment ages further back than that, it's permanently
"baked" onto the background tilemap — and since the snake's length only
changes when it eats (not with distance travelled), the *oldest* baked
segment is erased at the same rate new ones are added, keeping the total
length correctly matched to your score.

Each baked tile is chosen by exactly where the path enters and exits that
8×8 tile — packed as (edge, position-along-edge) rather than a raw pixel
coordinate, since a point sitting exactly at a tile's corner is genuinely
ambiguous as a bare coordinate. Every one of the resulting 32×32 possible
touch-point combinations has its own generated connector shape (reduced to
144 unique physical tiles via hflip/vflip, the only symmetries SNES
actually gives for free), looked up with a single table index — so the
baked tail matches the snake's real recorded path pixel-for-pixel instead
of snapping every turn to one of a handful of fixed shapes, while staying
just as cheap at runtime as the simpler version: one lookup, no drawing.
Whichever cell is entered borrows its touch point from the cell being
exited, so the two always agree exactly on the shared boundary; the rare
case where the path clips a tile corner between two samples (skipping the
cell that would normally connect them) is resolved by interpolating the
skipped crossing instead of leaving a gap.

## Performance notes (keeping 60 fps)

The whole game step has to fit in one video frame (about 262 scanlines of CPU
time). Two things burn that budget quickly on a 65816, and both are easy to
reintroduce by accident:

- **There is no divide (or hardware multiply) instruction.** A `u16 %` or `/`
  is a software routine costing hundreds of cycles, and `s32` multiply/divide
  is far worse. In per-frame code use masks and shifts instead
  (`& TRAIL_MASK`, `>> 5`, `div10()`), and keep arithmetic 16-bit where the
  values allow it. This is why `TRAILSIZE` **must be a power of two**.
- **Cost scales with snake length.** `drawSprites()` runs once per body sprite
  per frame (`SPRITE_WINDOW`) and the self-collision loop once per segment
  (`COLLIDE_WINDOW`).
- **The 32-sprites-per-scanline limit is a hard cap, not a hint.** A long snake
  running horizontally puts *every* body sprite on the same scanlines; past 32
  the PPU silently drops the highest-numbered ones (the oldest segments), and
  which ones it drops changes as the snake's shape does -- a hole that
  flickers. Real hardware and accurate emulators (bsnes) do this; a lenient
  emulator may not, which can hide the problem. So `SPRITE_WINDOW + head +
  pizza <= 32` is checked at compile time: it can't be exceeded whatever shape
  the snake makes. Collisions deliberately use a *separate* window
  (`COLLIDE_WINDOW`), so dying doesn't depend on how many sprites we can draw.
  Baking starts `SPRITE_OVERLAP` segments before the sprites run out, because
  sprites sit behind BG tiles and the overlap hides the lag in writing the
  newest baked tile.

Before this was fixed, every `% TRAILSIZE`/`% 32` in the per-segment and
per-tile paths, the 32-bit diagonal-tile maths, and the number formatting
added up to more than one frame once the snake passed ~45 segments, and the
game dropped to half speed for the rest of the round. The worst frame now
uses roughly 190 of ~257 available scanlines with a long snake. If you add
per-frame work, `lag_frame_counter` (pvsneslib) counts frames that overran.

## Known limitations

- No sound
- No two-player mode
- No in-game option to clear the high score table (delete the `.srm` file)
- A power cut in the instant the table is being written fails the checksum and
  resets the table on the next boot
