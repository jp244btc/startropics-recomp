#ifndef PALETTE_H
#define PALETTE_H
#include <stdint.h>
/* out: 8 emphasis sets x 64 colours, 0x00RRGGBB */
void nes_palette_build(uint32_t *out);
#endif
