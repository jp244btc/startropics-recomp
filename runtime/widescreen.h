/*
 * widescreen.h - side bands for a 16:9 view, decoded from the game's own
 * map data (see docs/MAP_FORMATS.md).
 */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>

#define WS_BAND 96                 /* pixels decoded on each side of the 256-pixel picture */
#define WS_W    (256 + 2 * WS_BAND)
#define WS_H    240

/* Fills out[WS_W * WS_H] with 6-bit palette indices (the same format as
   ppu.fb): the PPU's frame in the centre and, when the current scene uses a
   map format this module knows, the surrounding map in the side bands. The
   per-line emphasis in ppu.line_emph applies to the whole row.
   Returns 1 when the bands hold decoded map, 0 when they were blanked
   (pillarbox) because the scene is not understood. */
int widescreen_render(uint8_t *out);

/* Self-check: decodes the tiles under the centre 256x224 of the current frame the
   way the bands are decoded and compares them with the PPU's nametables, then
   re-renders that area and compares the pixels with the PPU's frame (sprites and
   tiles the game changed directly in VRAM are the only legitimate differences).
   Returns the number of tiles that differ (0 = the decoder reproduces the game's
   own output), -1 when the scene is not understood. */
int widescreen_check(int *total_tiles, int *bad_pixels, int *total_pixels);

/* Width of the decoded picture to show for a display of the given height
   (in NES lines) and pixel aspect ratio (1.0 = square pixels): the part of
   the WS_W-wide frame that makes a picture of the given ratio (16/9, 16/10),
   at most WS_W and at least the normal 256. */
int widescreen_visible_width(int lines, double pixel_aspect, double ratio);

#endif
