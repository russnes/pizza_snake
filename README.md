# Pizza Snake (SNES)

A from-scratch classic snake SNES game,
written in C for the 65816 using the [pvsneslib](https://github.com/alekmaul/pvsneslib)
homebrew devkit.

## Features

- Continuous, angle-based movement (turn left/right, not grid-based Snake)
- Effectively unlimited snake length: the most recent ~55 segments are
  smooth-moving sprites, and everything older is permanently baked onto the
  background layer as the snake grows past the SNES's 128-sprite hardware
  limit — see "How the long tail works" below
- Pizzas of two sizes/point values, randomly spawned
- 20-entry high score table with on-screen name entry (A–Z, 3 letters)
- Title screen, game-over screen, high-score screen

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
  cycle the letter, **Start** confirms

## How the long tail works

The SNES can only display 128 hardware sprites at once, so a snake that's
allowed to grow indefinitely can't be *all* sprites forever. The most recent
~55 segments (nearest the head) are rendered as sprites and move smoothly
every frame. Once a segment ages further back than that, it's permanently
"baked" onto the background tilemap — and since the snake's length only
changes when it eats (not with distance travelled), the *oldest* baked
segment is erased at the same rate new ones are added, keeping the total
length correctly matched to your score.

Each baked tile is chosen by classifying which two edges of that 8×8 tile
the path entered and exited through, and picking one of three fixed
connector shapes (straight, or a corner) already drawn to touch each edge
at a consistent point. That's what keeps the tail's width even and lets it
connect seamlessly into the sprite-rendered head, regardless of how tightly
the snake is turning.

## Known limitations

- No sound
- No two-player mode
- High scores don't persist across power-off (no SRAM save implemented)
