/*
 * palette.c - NTSC NES palette generated from the composite video model
 * (the well-known 12-phase square wave decoding), for all 8 emphasis
 * combinations. Nothing here is taken from the ROM.
 */
#include "palette.h"
#include <math.h>

static double gammafix(double f, double gamma) { return f <= 0 ? 0 : pow(f, 2.2 / gamma); }
static double clampd(double v) { return v > 255 ? 255 : (v < 0 ? 0 : v); }

static uint32_t ntsc_color(int pixel, double saturation, double hue_shift, double gamma) {
    static const double levels[8] = { 0.350, 0.518, 0.962, 1.550, 1.094, 1.506, 1.962, 1.962 };
    const double black = 0.518, white = 1.962;
    int color = pixel & 0x0F;
    int level = (pixel >> 4) & 3;
    if (color > 13) level = 1;
    double lo = levels[level], hi = levels[level + 4];
    if (color == 0) lo = hi;
    if (color > 12) hi = lo;
    double y = 0, i = 0, q = 0;
    for (int p = 0; p < 12; p++) {
        int in_phase = ((color + p + 8) % 12) < 6;   /* +8 rotates hue so $x6 is red, $x2 blue, $xA green */
        double spot = in_phase ? hi : lo;
        if (((pixel & 0x40) && (((0xC + p + 8) % 12) < 6)) ||
            ((pixel & 0x80) && (((0x4 + p + 8) % 12) < 6)) ||
            ((pixel & 0x100) && (((0x8 + p + 8) % 12) < 6)))
            spot *= 0.746;
        double v = (spot - black) / (white - black) / 12.0;
        y += v;
        i += v * cos(M_PI * (p + hue_shift) / 6.0) * saturation;
        q += v * sin(M_PI * (p + hue_shift) / 6.0) * saturation;
    }
    double r = clampd(255.95 * gammafix(y + 0.946882 * i + 0.623557 * q, gamma));
    double g = clampd(255.95 * gammafix(y - 0.274788 * i - 0.635691 * q, gamma));
    double b = clampd(255.95 * gammafix(y - 1.108545 * i + 1.709007 * q, gamma));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void nes_palette_build(uint32_t *out) {
    for (int emph = 0; emph < 8; emph++)
        for (int c = 0; c < 64; c++)
            out[emph * 64 + c] = ntsc_color(c | (emph << 6), 1.0, 0.0, 2.0);
}
