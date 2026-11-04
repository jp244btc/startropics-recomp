/*
 * remaster.h - the remastered graphics: StarTropics' own pixels recolored and shaded the way the
 * Pixel Remaster games redid their NES and SNES art (the same pixel grid, many more colors per
 * tile, shading in hue-shifted steps). Every pixel takes its color from the "ink" its source tile
 * gives that pixel (see ppu.hd_bg / hd_spr): one of the tile's own palette colors, a number of
 * steps darker or lighter. Being relative to the palette, inks follow the game's palette swaps,
 * fades and color cycling. They come from a pack of hand-made tiles where one exists
 * (remaster.pak, made with tools/remaster), else from shading worked out from the tile itself.
 */
#ifndef REMASTER_H
#define REMASTER_H
#include <stdint.h>

/* Ink byte: 0 = the pixel as the NES draws it; else bits 0-1 = which of the tile's four colors
   (0-3; for sprites 1-3), bits 2-4 = the step on that color's ramp, 1 darkest, 4 the color
   itself, 7 lightest, and bit 5 = half a step lighter (for soft textures). */
#define REMASTER_INK(color, step) ((uint8_t)((color) | ((step) << 2)))
#define REMASTER_HALF 0x20

/* Builds the automatic inks from the cartridge's CHR ROM and loads the pack at pack_path, if
   there is one (NULL: none). Returns the number of tiles the pack replaces, -1 without CHR ROM. */
int  remaster_init(const char *pack_path);
int  remaster_ready(void);
void remaster_forget(void);   /* another ROM was loaded: init again before use */

/* Colors w x h pixels as ARGB into out (stride pixels per line) from their sources (bg / spr,
   stride src_stride), falling back to the NES color in idx (6-bit, stride src_stride) where a
   pixel has none; emph = the $2001 bits of each row (emphasis, grayscale). */
void remaster_frame(uint32_t *out, int stride, const uint8_t *idx, const uint32_t *bg, const uint32_t *spr,
                    int src_stride, const uint8_t *emph, int w, int h);

#endif
