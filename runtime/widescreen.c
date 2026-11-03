/*
 * widescreen.c - side bands for a 16:9 view, decoded from the game's own
 * map data.
 *
 * The NES picture is 256 pixels wide and the nametables hold nothing beyond
 * it, so a real widescreen view has to be rebuilt from the level data in the
 * ROM. This module knows the format of the overworld (the scrolling top-down
 * map; see docs/MAP_FORMATS.md): a 16x16 grid of 256x240 screens held in RAM,
 * each screen a list of 15 run-length coded rows of 16x16 metatiles in a
 * swappable PRG bank, and a table of 6-byte metatiles (collision, flags +
 * palette, four CHR tiles). The bands are drawn with the CHR banks, scroll
 * and control register the PPU used for the frame and the current palette,
 * so they join the real picture without a seam.
 *
 * Only background tiles are decoded: the game has no objects beyond the
 * 256-pixel edge (they are not simulated there), so there is nothing else
 * to show. Any scene this module does not understand, and any frame whose
 * scroll does not agree with the player position, gets black bands.
 */
#include "widescreen.h"
#include "nes.h"
#include <stdio.h>
#include <string.h>

/* RAM locations of the overworld engine (StarTropics, USA) */
#define RAM_GAME_MODE   0x00    /* 0 = field engine (overworld), 2+ = other scene types */
#define RAM_SCENE_MODE  0x28    /* IRQ task mode: 0 = overworld (blank bottom 16 lines) */
#define RAM_PLAYER_X    0x32    /* 16-bit pixel position; the camera is 128 to the left */
#define RAM_PLAYER_Y    0x34    /* lo = 0..239 within a screen row, hi = screen row */
#define RAM_FLAG_0B     0x0B    /* with $5D: pick the metatile variants' first alternate */
#define RAM_FLAG_5B     0x5B    /* bit 6: also accept the group in $5F */
#define RAM_TILE_GROUP  0x5D    /* metatile group drawn as-is (bits 7-5 of the flag byte) */
#define RAM_TILE_GROUP2 0x5F
#define RAM_MAP_BANK    0x132   /* 8 KiB PRG bank with the map tables, mapped at $C000 */
#define RAM_MAP_SET     0x133   /* offset of the map set's three pointers inside the bank header */
#define RAM_SCREEN_MAP  0x400   /* 16x16 screen indices, row-major */

#define SCREEN_W 256
#define SCREEN_H 240
#define META_ROWS 15            /* metatile rows per screen */
#define VISIBLE_H 224           /* lines the overworld shows (the IRQ task blanks the rest) */

typedef struct {
    const uint8_t *win;         /* the bank as seen at $C000-$DFFF */
    uint16_t screens;           /* table of 16-bit pointers, one per screen */
    uint16_t metas;             /* 6 bytes per metatile */
    uint16_t alts;              /* replacement metatiles for the variant mechanism */
    int bad;                    /* a pointer left the bank window */
} mapset_t;

typedef struct { uint8_t tiles[4]; uint8_t pal; uint8_t valid; } cell_t;

static inline uint8_t ms_rd(mapset_t *m, unsigned a) {
    if (a < 0xC000 || a > 0xDFFF) { m->bad = 1; return 0; }
    return m->win[a - 0xC000];
}
static inline uint16_t ms_rd16(mapset_t *m, unsigned a) {
    return (uint16_t)(ms_rd(m, a) | (ms_rd(m, a + 1) << 8));
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* ---- per-frame caches (the map set can change between scenes) ---- */
typedef struct { int key; int n; uint16_t rows[META_ROWS + 1]; } screen_cache_t;
typedef struct { int key; int n; uint8_t ids[16]; } row_cache_t;
static screen_cache_t screen_cache[16];
static row_cache_t row_cache[64];

/* The list of row pointers of a screen, as the game's $8EA7 decodes it into $0510:
   control byte 0 ends the list; high nibble 2 = one 16-bit pointer repeated (low
   nibble) times; high nibble 0/1 = a high byte followed by (low nibble) deltas that
   accumulate into a running 16-bit pointer; anything else = literal 16-bit pointers. */
static const screen_cache_t *screen_rows(mapset_t *m, int screen) {
    screen_cache_t *e = &screen_cache[screen & 15];
    if (e->key == screen) return e;
    e->key = screen; e->n = 0;
    unsigned p = ms_rd16(m, m->screens + 2u * (unsigned)screen);
    unsigned y = 0;
    for (int guard = 0; guard < 64 && e->n < META_ROWS + 1; guard++) {
        uint8_t b = ms_rd(m, p + y);
        if (!b || m->bad) break;
        y++;
        int cnt = b & 0x0F, hi = b >> 4;
        if (hi == 2) {
            uint16_t v = ms_rd16(m, p + y); y += 2;
            while (cnt-- > 0 && e->n < META_ROWS + 1) e->rows[e->n++] = v;
        } else if (hi < 2) {
            unsigned acc = (unsigned)ms_rd(m, p + y) << 8; y++;
            while (cnt-- > 0 && e->n < META_ROWS + 1) {
                acc = (acc + ms_rd(m, p + y)) & 0xFFFF;   /* 8-bit add with carry into the high byte */
                y++;
                e->rows[e->n++] = (uint16_t)acc;
            }
        } else {
            while (cnt-- > 0 && e->n < META_ROWS + 1) { e->rows[e->n++] = ms_rd16(m, p + y); y += 2; }
        }
    }
    return e;
}

/* One row of 16 metatile ids, as the game's $9022 decodes it: control byte 0 ends it;
   bit 7 set = metatile (byte >> 3) & 15 repeated (byte & 7, 0 = 8) times; high nibble 2 =
   (low nibble, 0 = 16) literal ids; otherwise the next byte repeated (low nibble, 0 = 16) times. */
static const row_cache_t *row_ids(mapset_t *m, int screen, int row) {
    int key = screen * 16 + row;
    row_cache_t *e = &row_cache[key & 63];
    if (e->key == key) return e;
    e->key = key; e->n = 0;
    const screen_cache_t *s = screen_rows(m, screen);
    if (row >= s->n) return e;
    unsigned p = s->rows[row], y = 0;
    for (int guard = 0; guard < 32 && e->n < 16; guard++) {
        uint8_t b = ms_rd(m, p + y);
        if (!b || m->bad) break;
        y++;
        if (b & 0x80) {
            int cnt = (b & 7) ? (b & 7) : 8;
            uint8_t id = (b & 0x78) >> 3;
            while (cnt-- > 0 && e->n < 16) e->ids[e->n++] = id;
        } else if ((b >> 4) == 2) {
            int cnt = (b & 15) ? (b & 15) : 16;
            while (cnt-- > 0 && e->n < 16) e->ids[e->n++] = ms_rd(m, p + y++);
        } else {
            int cnt = (b & 15) ? (b & 15) : 16;
            uint8_t id = ms_rd(m, p + y++);
            while (cnt-- > 0 && e->n < 16) e->ids[e->n++] = id;
        }
    }
    return e;
}

/* Metatile variants, as the game's strip drawer ($8D8D in bank 30) picks them: a
   metatile whose group (flag bits 7-5) is not the current one is replaced by an
   alternate from the map set's table. */
static uint8_t resolve_variant(mapset_t *m, uint8_t id) {
    uint8_t flags = ms_rd(m, m->metas + id * 6u + 1), grp = flags & 0xE0;
    uint8_t cur = ram[RAM_TILE_GROUP], cur2 = ram[RAM_TILE_GROUP2] & 0xE0;
    if (grp == cur) return id;
    if (ram[RAM_FLAG_5B] & 0x40) {
        if (grp == cur2) return id;
        if (ram[RAM_FLAG_0B] && cur && cur2) return ms_rd(m, m->alts);
        return ms_rd(m, m->alts + ((flags >> 2) & 7));
    }
    if (ram[RAM_FLAG_0B] && cur) return ms_rd(m, m->alts);
    return ms_rd(m, m->alts + ((flags >> 2) & 7));
}

/* world metatile (col, row) -> tiles + palette */
static void lookup_cell(mapset_t *m, int wcol, int wrow, cell_t *c) {
    int sx = floordiv(wcol, 16), mx = wcol - sx * 16;
    int sy = floordiv(wrow, META_ROWS), my = wrow - sy * META_ROWS;
    int screen = ram[RAM_SCREEN_MAP + ((sy & 15) << 4) + (sx & 15)];
    const row_cache_t *r = row_ids(m, screen, my);
    c->valid = 0;
    if (mx >= r->n) return;
    uint8_t id = resolve_variant(m, r->ids[mx]);
    unsigned base = m->metas + id * 6u;
    c->pal = ms_rd(m, base + 1) & 3;
    for (int i = 0; i < 4; i++) c->tiles[i] = ms_rd(m, base + 2 + i);
    c->valid = !m->bad;
}

/* The scene check and the camera, shared by the renderer and the self-check. */
static int overworld_setup(mapset_t *m, int *camx_out, int *camy_out) {
    if (ram[RAM_GAME_MODE] != 0 || ram[RAM_SCENE_MODE] != 0) return 0;
    if (!(ppu.line_emph[0] & 0x08)) return 0;              /* background off (fades, blanked frames) */
    unsigned banks = cart.prg_size / 8192;
    if (ram[RAM_MAP_BANK] >= banks) return 0;
    m->win = cart.prg + ram[RAM_MAP_BANK] * 8192u;
    m->bad = 0;
    unsigned off = ram[RAM_MAP_SET];
    m->screens = ms_rd16(m, 0xC002 + off);
    m->metas = ms_rd16(m, 0xC004 + off);
    m->alts = ms_rd16(m, 0xC006 + off);
    if (m->bad) return 0;

    /* camera: the player is centred, but the scroll the PPU used for this frame may lag
       the player variables by a step of the walking animation, so derive the camera from
       the scroll and take only the high bits from the player position */
    if (ram[RAM_PLAYER_Y] >= SCREEN_H) return 0;
    int px = ram[RAM_PLAYER_X] | (ram[RAM_PLAYER_X + 1] << 8);
    int py = ram[RAM_PLAYER_Y + 1] * SCREEN_H + ram[RAM_PLAYER_Y];
    int camx0 = px - 128, camy0 = py - 128;
    int scrollx = ((ppu.frame_t >> 10) & 1) * 256 + (ppu.frame_t & 0x1F) * 8 + ppu.frame_x;
    int scrolly = ((ppu.frame_t >> 11) & 1) * 240 + ((ppu.frame_t >> 5) & 0x1F) * 8 + (ppu.frame_t >> 12);
    int dx = (scrollx - camx0) & 511; if (dx > 255) dx -= 512;
    int dy = ((scrolly - camy0) % SCREEN_H + SCREEN_H) % SCREEN_H; if (dy > 119) dy -= SCREEN_H;
    if (dx < -32 || dx > 32 || dy < -32 || dy > 32) return 0;
    *camx_out = camx0 + dx; *camy_out = camy0 + dy;

    for (int i = 0; i < 16; i++) screen_cache[i].key = -1;
    for (int i = 0; i < 64; i++) row_cache[i].key = -1;
    return 1;
}

/* Draws the background of the screen-space columns [sx0, sx0 + width) (sx0 may be
   negative) into dst (stride bytes per line, WS_H lines) the way the PPU drew the
   centre: from the frame's CHR banks and pattern table, the current palette and the
   line's $2001 bits. Width is at most 256. Returns 0 when the map data is unreadable. */
static int render_region(mapset_t *m, int camx, int camy, int sx0, int width, uint8_t *dst, int stride) {
    cell_t cells[17][18];
    int row0 = floordiv(camy, 16), nrows = floordiv(camy + WS_H - 1, 16) - row0 + 1;
    int col0 = floordiv(camx + sx0, 16), ncols = floordiv(camx + sx0 + width - 1, 16) - col0 + 1;
    if (nrows > 17) nrows = 17;
    if (ncols > 18) ncols = 18;
    for (int r = 0; r < nrows; r++)
        for (int c = 0; c < ncols; c++) lookup_cell(m, col0 + c, row0 + r, &cells[r][c]);
    if (m->bad) return 0;

    const uint8_t *chr[8];
    for (int i = 0; i < 8; i++) chr[i] = mapper_chr_bank_ptr(ppu.frame_chr_bank[i]);
    unsigned pt_base = (ppu.frame_ctrl & 0x10) ? 0x1000 : 0;

    for (int y = 0; y < WS_H; y++) {
        uint8_t mask = ppu.line_emph[y];
        uint8_t *line = dst + y * stride;
        uint8_t backdrop = (mask & 1) ? (ppu.palette[0] & 0x30) : ppu.palette[0];
        if (!(mask & 0x08)) { memset(line, backdrop, (size_t)width); continue; }   /* background disabled on this line */
        int wy = camy + y, wrow = floordiv(wy, 16), ly = wy - wrow * 16;
        const cell_t *crow = cells[wrow - row0];
        int ty = (ly >> 3) & 1, frow = ly & 7;
        for (int x = 0; x < width; x++) {
            int wx = camx + sx0 + x, wcol = floordiv(wx, 16), lx = wx - wcol * 16;
            const cell_t *cell = &crow[wcol - col0];
            if (!cell->valid) { line[x] = backdrop; continue; }
            unsigned a = pt_base + cell->tiles[ty * 2 + (lx >> 3)] * 16u + frow;
            const uint8_t *pt = chr[(a >> 10) & 7] + (a & 0x3FF);
            int bit = 7 - (lx & 7);
            int pix = ((pt[0] >> bit) & 1) | (((pt[8] >> bit) & 1) << 1);
            uint8_t color = pix ? ppu.palette[cell->pal * 4 + pix] : ppu.palette[0];
            if (mask & 1) color &= 0x30;
            line[x] = color;
        }
    }
    return 1;
}

int widescreen_render(uint8_t *out) {
    for (int y = 0; y < WS_H; y++) memcpy(out + y * WS_W + WS_BAND, ppu.fb + y * 256, 256);
    mapset_t m;
    int camx, camy;
    if (overworld_setup(&m, &camx, &camy) &&
        render_region(&m, camx, camy, -WS_BAND, WS_BAND, out, WS_W) &&
        render_region(&m, camx, camy, 256, WS_BAND, out + WS_BAND + 256, WS_W))
        return 1;
    for (int y = 0; y < WS_H; y++) {
        memset(out + y * WS_W, 0x0F, WS_BAND);
        memset(out + y * WS_W + WS_BAND + 256, 0x0F, WS_BAND);
    }
    return 0;
}

int widescreen_check(int *total_tiles, int *bad_pixels, int *total_pixels) {
    mapset_t m;
    int camx, camy, bad = 0, n = 0;
    *total_tiles = *bad_pixels = *total_pixels = 0;
    if (!overworld_setup(&m, &camx, &camy)) return -1;

    /* 1. every 8x8 tile whose top-left corner is inside the centre 256x224 window: the
          decoder's tile id against the nametable byte the game wrote there */
    for (int wy = camy - (camy & 7) + ((camy & 7) ? 8 : 0); wy < camy + VISIBLE_H; wy += 8) {
        for (int wx = camx - (camx & 7) + ((camx & 7) ? 8 : 0); wx < camx + 256; wx += 8) {
            cell_t c;
            int wcol = floordiv(wx, 16), wrow = floordiv(wy, 16);
            lookup_cell(&m, wcol, wrow, &c);
            n++;
            if (!c.valid) { bad++; continue; }
            uint8_t tile = c.tiles[((wy - wrow * 16) >> 3) * 2 + ((wx - wcol * 16) >> 3)];
            int nt = (wx >> 8) & 1, col = (wx >> 3) & 31;
            int row = (((wy % SCREEN_H) + SCREEN_H) % SCREEN_H) >> 3;
            uint8_t have = ppu.vram[ppu.nt_offset[nt] + row * 32 + col];
            if (have != tile) {
                bad++;
                if (sys.debug_log)
                    fprintf((FILE *)sys.debug_log, "widescreen frame %llu: world (%d,%d) screen (%d,%d) decoded tile %02X, nametable %02X\n",
                            (unsigned long long)ppu.frame, wx, wy, wx - camx, wy - camy, tile, have);
            }
        }
    }
    *total_tiles = n;
    if (m.bad) return -1;

    /* 2. the centre re-rendered with the band renderer against the PPU's pixels: only
          sprites and tiles the game changed directly in VRAM may differ */
    static uint8_t centre[256 * WS_H];
    if (render_region(&m, camx, camy, 0, 256, centre, 256)) {
        int bp = 0;
        for (int y = 0; y < VISIBLE_H; y++)
            for (int x = 0; x < 256; x++)
                if (centre[y * 256 + x] != ppu.fb[y * 256 + x]) bp++;
        *bad_pixels = bp;
        *total_pixels = 256 * VISIBLE_H;
    }
    return bad;
}

int widescreen_visible_width(int lines, double pixel_aspect, double ratio) {
    int w = (int)(lines * ratio / pixel_aspect + 0.5);
    if (w > WS_W) w = WS_W;
    if (w < 256) w = 256;
    return w;
}
