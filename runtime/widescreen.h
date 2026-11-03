/*
 * widescreen.h - the world beyond the edges of the picture for the widescreen
 * modes, decoded from the game's own map data (see docs/MAP_FORMATS.md).
 */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>

/* The widescreen canvas: the PPU's 256x240 picture with up to WS_BAND pixels of
   map on each side and WS_VBAND lines above and below it (Integer scaling fills
   a whole screen from it). */
#define WS_BAND  224
#define WS_VBAND 48
#define WS_W     (256 + 2 * WS_BAND)
#define WS_H     (240 + 2 * WS_VBAND)

/* Renders the part [x, x + w) x [y, y + h) of the canvas (the picture's top left
   corner is at WS_BAND, WS_VBAND) into out (w x h 6-bit palette indices, the
   format of ppu.fb) and emph (h bytes: the $2001 value each row is shown with,
   for its emphasis and grayscale bits). When the current scene uses a map format
   this module knows, everything the PPU did not draw is the map around the
   camera: the canvas beyond the picture, and the picture's lines whose
   background was off (the overworld blanks its bottom 16), with the objects the
   game keeps there. Otherwise the picture is as the PPU drew it and the rest is
   black.
   Returns 1 when the map was decoded, 0 when the scene is not understood. */
int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h);

/* The field objects (villagers and the like) the game keeps beyond its screen, drawn into the
   canvas by widescreen_render (on by default; the harness turns them off to compare the centre
   with the PPU's frame), and how many of those the last render drew. */
void widescreen_objects(int on);
int  widescreen_objects_drawn(void);

/* Self-check: decodes the tiles under the centre 256x224 of the current frame the
   way the bands are decoded and compares them with the PPU's nametables, then
   re-renders that area and compares the pixels with the PPU's frame (sprites and
   tiles the game changed directly in VRAM are the only legitimate differences).
   Returns the number of tiles that differ (0 = the decoder reproduces the game's
   own output), -1 when the scene is not understood. */
int widescreen_check(int *total_tiles, int *bad_pixels, int *total_pixels);

/* Width of the decoded picture to show for a display of the given height
   (in NES lines) and pixel aspect ratio (1.0 = square pixels): the part of
   the WS_W-wide canvas that makes a picture of the given ratio (16/9, 16/10),
   at most WS_W and at least the normal 256. */
int widescreen_visible_width(int lines, double pixel_aspect, double ratio);

#endif
