# StarTropics map formats (overworld)

How the overworld (the scrolling top-down map: C-Island in chapter 1, the
title screen's beach) is stored and drawn, as reverse-engineered from the USA
cartridge for the widescreen renderer in `runtime/widescreen.c`. This file
describes formats and addresses only; the data itself stays in your ROM.

Addresses are CPU addresses. Bank numbers are 8 KiB PRG banks (the MMC6 maps
bank 30 at `$8000`, bank 31 at `$E000`, swappable banks at `$A000` and
`$C000`). Everything was confirmed with the headless harness: the decoder is
checked against the PPU's own nametables (`--widescreen`, below).

## 1. Scene state in RAM

| address | meaning |
|---|---|
| `$00` | game mode: 0 = island overworld (also the title beach), 1 = town (Coralcola: the same field engine with another map set), 2 and 3+ = other scene types (dungeons use the bank 23/25/26/27 routine set); the NMI dispatcher at `$80C2` runs the same per-frame routines for 0 and 1 |
| `$28` | scene / IRQ task mode 0-4 (`$2A` with bit 7 set requests a change); 0 = overworld |
| `$32/$33` | player X in world pixels (16-bit) |
| `$34/$35` | player Y: low byte 0-239 inside a screen row, high byte = screen row (Y = hi x 240 + lo; the helpers at `$9312`/`$9318` wrap the low byte at 240) |
| `$36`, `$37` | facing and movement direction: 0 down, 1 up, 2 right, 3 left |
| `$40/$41` | movement state (`$40` bit 7 = step in progress) |
| `$17/$18`, `$1B/$1C` | camera: X - 128 (9 bits) and Y - 128 (mod 240), from `$929C`/`$92BA` (bank 30); the player is always centred |
| `$13-$16` | scroll written to `$2005`/`$2000` by the NMI (`$8882`): X, nametable X bit, Y, nametable Y bit. Copied from `$17/$18/$1B/$1C` by the mode-0 IRQ task at scanline 223 (`$85E5`), so the frame drawn after an NMI uses the camera of the previous frame's line 223 |
| `$10`, `$11` | `$2000` / `$2001` values |
| `$0132` | PRG bank holding the map tables, mapped at `$C000` while they are read |
| `$0133` | offset of the map set's header inside that bank (0 for chapter 1) |
| `$0400-$04FF` | the screen grid: 16 columns x 16 rows of screen indices |
| `$0140-$0145` | CHR banks for MMC3 registers 0-5, reloaded every frame by the IRQ task |
| `$5B`, `$0B`, `$5D`, `$5F` | metatile variant selection (section 7) |
| `$0500-$0503` | the map probe position (X 16-bit, Y in the screen-row form) used by the strip drawer and by collision (`$A2B8`, bank 29) |
| `$0504` | screen index at the probe, `$0505` strip orientation, `$0506-$0508` metatile row / column / count |
| `$0509/$050A` | current VRAM address, `$050B` tiles left, `$050C` count byte index, `$050D` write index into the NMI buffer |
| `$0510-$052D` | scratch: the screen's 15 row pointers, then the decoded row (16 metatile ids) |
| `$052F-$057F` | NMI VRAM update buffer (section 2) |
| `$0580-$05FF` | shadow attribute tables for nametables 0 and 1 (64 bytes each) |

Chapter 1's island sets `$0132` = 1, `$0133` = 0 (bank 30, `$9593`); entering
Coralcola switches to game mode 1 with `$0132` = 2, `$0133` = 0 (the village
is its own 2 x 2-screen map set with the same grid layout). The title screen
and attract mode use the same engine with bank 4 and header offsets `$48` /
`$50` (bank 25, `$B919` / `$B715`), which is why the beach under the logo also
decodes.

## 2. NMI uploads

The NMI handler (`$8002`, bank 30) uploads three buffers:

* **VRAM update buffer** at `$052F`, copied by `$83CC`: records of
  `hi, lo, count, data...` where `count` bit 7 selects the +32 (vertical)
  increment and bits 6-0 give the number of bytes (counts of 8 or more are
  unrolled). A record whose `hi` byte is 0 ends the list; `$050D` is the
  write index, `$050E` bit 7 requests the upload, both are cleared afterwards.
* **Attribute tables**: `$8446` writes the whole 64-byte shadow table
  `$0580` to `$23C0` when `$050F` bit 1 is set and `$05C0` to `$27C0` for
  bit 2 (`$050F` bit 7 = request; in mode 0 this happens inside the IRQ task).
* **Palette**: 32 bytes at `$01A0` to `$3F00` by `$84A7` when `$01C0` bit 7 is set.

Then `$8882` writes `$2000` = `$10 & $FC | $14 | $16 << 1`, `$2005` = `$13`, `$15`,
`$2001` = `$11`.

## 3. The scanline IRQ

`$8638` arms the MMC6 counter with the latch from `$01CA` (per mode table
`$84E9`: `$DF $46 $B6 $BA $00`) and enables it when `$26` is 1. The IRQ
vector jumps through `$01CC` (`$8504`), filled from the vector table `$84EE`.

Mode 0 (overworld), latch `$DF` = scanline 223, task `$850F` -> `$8317`:
waits a few cycles, sets the VRAM address to 0, writes `$2001` = 0 (lines
224-239 are blanked, the visible map is 224 lines = 14 metatile rows),
uploads the attribute tables if requested, reloads CHR registers 0-5 from
`$0140-$0145` and copies `$17/$18/$1B/$1C` to `$13-$16` (`$85E5`).

Modes 1, 3 and 4 (`$8520`, `$8200`) are mid-frame scroll splits with their
own scroll (`$F8/$F9` = `$15 + $B8`, nametable bit flipped) and palette
rewrites from the table at `$84DD`: the status-bar scenes (dungeons). Their
room format is not decoded here.

## 4. Map set header

The bank in `$0132`, seen at `$C000`, starts with a header; `$0133` is added
to every pointer slot so one bank can hold several map sets:

| slot | contents |
|---|---|
| `$C002 + o` | pointer to the screen table: one 16-bit pointer per screen index |
| `$C004 + o` | pointer to the metatile table: 6 bytes per metatile |
| `$C006 + o` | pointer to the alternates table: 8 metatile ids for the variant mechanism |

`$C000`/`$C001` and the slots beyond `$C008` belong to other map sets or to
the walking scripts (bank 4: `$C004`/`$C00C` script tables used by bank 29's
`$A36E` interpreter, `$C008` object lists).

## 5. The screen grid

`$0400` holds 256 screen indices, row-major, 16 per row: the screen at grid
position (sx, sy) is `$0400[sy * 16 + sx]` (`$8C92`: `Y = $0503 << 4 | $0501`).
Every screen is 256 x 240 pixels = 16 x 15 metatiles. World pixel (X, Y) is on
screen (X / 256, Y / 240) at metatile (X % 256 / 16, Y % 240 / 16).

The grid is loaded by `$9AEA` (bank 30) from an RLE stream: control byte 0
ends it; bit 7 set = continue on the next grid row; otherwise the low nibble
is a count and the high nibble selects a literal run (0-1), an increasing run
(2: one byte, then +1 per cell) or a repeat (3+: one byte repeated). Chapter 1's
C-Island occupies five screens in a grid that is otherwise all screen 0 (sea).
Bank 30 also patches single entries at run time (`$9B07`-`$9B33`), so the
RAM copy is the truth, not the ROM stream.

## 6. Screens and rows

**Screen -> row pointers** (`$8EA7`, result in `$0510`): a stream of control
bytes producing 15 pointers, one per metatile row: 0 ends the list; high
nibble 2 = a 16-bit pointer repeated (low nibble) times; high nibble 0 or 1 =
a high byte followed by (low nibble) one-byte deltas that accumulate into a
running 16-bit pointer (8-bit add, carry into the high byte); anything else =
(low nibble) literal 16-bit pointers.

**Row -> 16 metatile ids** (`$9022` decodes a whole row into `$0510`, `$908C`
skips to one column): control byte 0 ends the row; bit 7 set = metatile
`(byte >> 3) & 15` repeated `byte & 7` times (0 = 8); high nibble 2 = (low
nibble, 0 = 16) literal ids; any other high nibble = the next byte repeated
(low nibble, 0 = 16) times.

## 7. Metatiles

Six bytes per metatile at `metatiles + id * 6` (`$91FE`):

| byte | meaning |
|---|---|
| 0 | collision / terrain: bit 0 set = walkable (`$A1D1`, bank 29; collision is tested on the raw metatile, before variants); bits 3-5 = zone event raised when stepped into (`$B175`: `$08` enter a town or hut, `$18` leave, `$30` with `$47` set); bits 1-2 water / cliff types |
| 1 | bits 7-5: variant group; bits 4-2: alternate index; bits 1-0: palette (attribute bits) |
| 2, 3 | top-left, top-right CHR tile |
| 4, 5 | bottom-left, bottom-right CHR tile |

Tiles come from the background pattern table selected by `$2000` bit 4 (the
game uses `$1000`), with the CHR banks the IRQ task loaded from `$0140-$0145`.

**Variants** (`$8D8D`-`$8E28`, bank 30): a metatile whose group (byte 1 bits
7-5) equals `$5D` is drawn as it is. Otherwise it is replaced by an entry of
the alternates table: entry `(byte 1 >> 2) & 7` normally (`$919B`), entry 0
when both `$0B` and `$5D` are nonzero (`$91AA`). When `$5B` bit 6 is set the
group in `$5F` is also accepted (`$916B`/`$9183`). The palette and tiles are
those of the metatile finally chosen.

**Attributes**: the palette of each metatile goes into the shadow attribute
tables (`$922F`), so every 16 x 16 attribute cell is exactly one metatile.

**Hut interiors are a group layer.** In Coralcola the inside of a hut sits at
the same world position as the hut: its floor, walls and door are group-`$20`
metatiles whose alternates (indices 1 and 2) are the roof and wall patterns,
and the exterior metatiles (group 0, alternate index 0) have the black
metatile as alternate 0. Walking through the gap in a hut's front wall steps
onto a group-`$20` cell: the game sets `$5B` bit 6 (both groups shown, the
interior appears over the exterior) and redraws the screen strip by strip,
then sets `$5D` = `$20` and redraws again, which turns everything outside the
hut black. Leaving reverses it. `$5D`/`$5F` are updated by every step
(`$A1DF`: `$5D` takes the group of the cell just left, `$5F` the flags of the
new cell).

## 8. Drawing strips while walking

Movement is scripted: bank 29's `$A36E` interprets a per-step script (bank 4
data) and calls the strip drawer `$8C00` (bank 30) through the vector
`$FF00`. `$8C00` copies the player position to `$0500-$0503` and places the
strip by the facing direction `$36`:

| facing | strip | start (relative to the player) |
|---|---|---|
| 0 down | horizontal row of 17 metatiles | X - 136, Y + 96 (the row just below the 224 visible lines) |
| 1 up | horizontal row | X - 136, Y - 144 (the row just above the top) |
| 2 right | vertical column of 16 metatiles | X + 128, Y - 136 (the column just right of the picture) |
| 3 left | vertical column | X - 144, Y - 136 |

A strip is split where it crosses a screen boundary (`$8C33`, `$8C51`). Each
part becomes two VRAM records, written by `$8CCF`: for a row, the top and
bottom tile rows (`count` = metatiles x 2, +32 flag clear, second record at
address + 32); for a column, the left and right tile columns (`+32` flag set,
second record at address + 1). The metatiles come from `$9022` (row decode)
and are resolved through section 7; `$91D6` / `$913E` put the tile bytes into
the two records and `$9227` updates the attribute shadow. The VRAM address is
`$2000 + (screen X & 1) * $400 + (Y % 240 / 8) * 32 + X % 256 / 8` (`$8E2A`):
vertical mirroring, horizontal scrolling across two nametables and vertical
wrap at 240 lines inside one.

A full redraw (`$9998`, scene start and after text boxes) draws 15 horizontal
strips from Y - 232 upwards with the same routine. Objects and events that
change tiles go through the NMI buffer directly (bank 28 `$A515` screen
loader, bank 29 `$A665` event scripts) and are not part of the map data.

## 9. What the widescreen renderer does with this

`widescreen_render()` keeps the PPU's 256 x 240 frame in the centre of a
704 x 336 canvas (224 pixels on each side, 48 lines above and below) and
renders the part of it the frontend shows, decoding the map around the
camera. The "16:9 (widescreen)" aspect choice shows the 341 columns that make
a 16:9 picture at the 4:3 CRT pixel shape; Integer scaling in fullscreen shows
every line and column the screen holds at the multiple (342 x 268 on the
Steam Deck at 3x). Wherever the scene is not decoded, the normal picture sits
between black bars:

1. The scene must be the field engine in IRQ mode 0 (`$00` = 0 island or 1
   town, `$28` = 0) with the background enabled on line 0. Hut interiors need
   nothing extra: the variant rules above are applied with the current
   `$5B`/`$5D`/`$5F`, so inside a hut the bands are black like the game's own
   exterior. During the game's redraw wipe when a hut opens or closes (about
   40 frames) the centre is still being rewritten strip by strip while the bands
   already show the final state; the self-check reports those frames.
2. The camera is the player position minus 128 in both axes, corrected by the
   scroll the PPU actually used for the frame (snapshotted with the CHR banks
   and `$2000` at scanline 0): the step animation can leave the scroll one
   pixel behind the player variables. If they disagree by more than 32 pixels
   the scene is not the overworld as understood and the bands stay black.
3. For every 16 x 16 cell under the bands: screen index from `$0400`, row
   pointer, row decode, variant resolution, tiles and palette as above; the
   pixels are drawn from the frame's CHR banks and the current palette, with
   the line's grayscale/emphasis bits. Lines whose background was disabled
   (224-239 in mode 0) show the map too, with the bits of the last line the
   PPU drew; so do the lines above and below the picture.

The bands contain background only. The game does not simulate anything
beyond the edges of the picture: enemies, villagers and items are object slots that
exist only inside the picture, so there are no sprites to draw there, and a
tile the game changed in VRAM directly (opened passages, the landing boat)
appears when it scrolls into the centre, not in the bands. Scenes with an
unknown format (dungeons, menus, text screens, cutscenes) get black bands.
Verified with the harness on the island, in Coralcola (0 of 41,216 tiles
differ while walking around the village) and inside a hut (0 differ once
the game's redraw has finished).

Harness: `nes-headless --widescreen out.png` writes the whole canvas (also
every N frames with `--png-every`), checks that its centre is identical to
the normal frame on every line the PPU drew and compares the decoder's tiles under the centre 256 x 224
with the PPU's nametables; `--ram-dump-every N` and the bank columns in
`--trace` lines were added for this work.
