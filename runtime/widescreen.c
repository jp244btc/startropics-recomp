/*
 * widescreen.c - the world beyond the edges of the picture, decoded from the
 * game's own map data.
 *
 * The NES picture is 256 pixels wide and the nametables hold nothing beyond
 * it, so a real widescreen view (and a screen filled from edge to edge) has to
 * be rebuilt from the level data in the ROM. This module knows the format of the field engine's maps (the island
 * overworld, Coralcola and its hut interiors; see docs/MAP_FORMATS.md): a
 * 16x16 grid of 256x240 screens held in RAM,
 * each screen a list of 15 run-length coded rows of 16x16 metatiles in a
 * swappable PRG bank, and a table of 6-byte metatiles (collision, flags +
 * palette, four CHR tiles). The bands are drawn with the CHR banks, scroll
 * and control register the PPU used for the frame and the current palette,
 * so they join the real picture without a seam.
 *
 * The field engine's objects (villagers and the like) are drawn too: the game
 * keeps every one of them in RAM but shows, and moves, only those within a
 * window around the player that is exactly its screen; the others stand
 * frozen where they were. Those are drawn here the way the game's sprite
 * routine would draw them, where the PPU did not. Any scene this module does
 * not understand, and any frame whose scroll does not agree with the player
 * position, gets black around the picture.
 */
#include "widescreen.h"
#include "nes.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

/* RAM locations of the overworld engine (StarTropics, USA) */
#define RAM_GAME_MODE   0x00    /* field engine: 0 = island overworld, 1 = town (same engine, another map set); 2+ = other scene types */
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
#define RAM_FRAME       0x23    /* frame counter: bit 4 steps the objects' walking animation */
#define RAM_SCENE_REQ   0x2A    /* bits 0-6: a scene change is pending */
#define RAM_MOVER       0x75    /* the object a script is moving: X ($75/$76), Y ($77/$78), tile ($79), facing ($7A) */
#define RAM_OBJECTS     0x300   /* 16 objects of 16 bytes: flags, group + palette, tile + facing, ..., X at +12, Y at +14 */
#define OBJ_TABLE_BANK  29      /* the tile offsets by facing and animation step, at $B145 in bank 29 ($A000) */
#define OBJ_TABLE       0x1145

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
   alternate from the map set's table. This is also how hut interiors work: inside,
   $5D is the interior's group and the exterior resolves to the black metatile. */
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
    if (ram[RAM_GAME_MODE] > 1 || ram[RAM_SCENE_MODE] != 0) return 0;
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

/* the tile of the CHR ROM image a pattern row pointer lies in (HD_TILE_NONE outside it) */
static uint32_t chr_tile(const uint8_t *row) {
    if (!cart.chr || row < cart.chr || row >= cart.chr + cart.chr_size) return HD_TILE_NONE;
    return (uint32_t)(row - cart.chr) >> 4;
}

/* Draws the map under the rectangle [px0, px0 + width) x [py0, py0 + height) of the picture's
   coordinates (the PPU's picture is 0-255 x 0-239; the rectangle may lie beyond it on any side,
   at most WS_W wide) into dst (stride bytes per line) the way the PPU draws the background: from
   the frame's CHR banks and pattern table and the current palette, with masks[r] as row r's
   $2001 bits (background enable, grayscale); hdst (or NULL) gets each pixel's source tile for the
   enhanced graphics. Returns 0 when the map data is unreadable. */
static int render_block(mapset_t *m, int camx, int camy, int px0, int py0, int width, int height,
                        const uint8_t *masks, uint8_t *dst, uint32_t *hdst, int stride) {
    enum { MAX_COLS = WS_W / 16 + 2 };
    cell_t cells[MAX_COLS];
    if (width <= 0 || height <= 0) return 1;
    int wx0 = camx + px0, col0 = floordiv(wx0, 16);
    int ncols = floordiv(wx0 + width - 1, 16) - col0 + 1, cell_row = INT_MIN;
    if (ncols > MAX_COLS) return 0;

    const uint8_t *chr[8];
    for (int i = 0; i < 8; i++) chr[i] = mapper_chr_bank_ptr(ppu.frame_chr_bank[i]);
    unsigned pt_base = (ppu.frame_ctrl & 0x10) ? 0x1000 : 0;

    for (int y = 0; y < height; y++) {
        uint8_t mask = masks[y], *line = dst + y * stride;
        uint32_t *hline = hdst ? hdst + y * stride : NULL;
        uint8_t gray = (mask & 1) ? 0x30 : 0xFF, backdrop = ppu.palette[0] & gray;
        if (!(mask & 0x08)) {   /* background disabled on this line */
            memset(line, backdrop, (size_t)width);
            if (hline) memset(hline, 0, (size_t)width * sizeof *hline);
            continue;
        }
        int wy = camy + py0 + y, wrow = floordiv(wy, 16), ly = wy - wrow * 16;
        if (wrow != cell_row) {   /* the cells of a metatile row, looked up once for its 16 lines */
            for (int c = 0; c < ncols; c++) lookup_cell(m, col0 + c, wrow, &cells[c]);
            if (m->bad) return 0;
            cell_row = wrow;
        }
        int ty = (ly >> 3) & 1, frow = ly & 7;
        for (int x = 0; x < width;) {
            int wx = wx0 + x, wcol = floordiv(wx, 16), lx = wx - wcol * 16;
            int fine = lx & 7, run = 8 - fine;   /* the pixels of this tile on the line */
            if (run > width - x) run = width - x;
            const cell_t *cell = &cells[wcol - col0];
            if (!cell->valid) {
                memset(line + x, backdrop, (size_t)run);
                if (hline) memset(hline + x, 0, (size_t)run * sizeof *hline);
            } else {
                unsigned a = pt_base + cell->tiles[ty * 2 + (lx >> 3)] * 16u + frow;
                const uint8_t *pt = chr[(a >> 10) & 7] + (a & 0x3FF);
                const uint8_t *pal = ppu.palette + cell->pal * 4;
                uint32_t tile = chr_tile(pt);
                for (int i = 0; i < run; i++) {
                    int bit = 7 - fine - i;
                    int pix = ((pt[0] >> bit) & 1) | (((pt[8] >> bit) & 1) << 1);
                    line[x + i] = (pix ? pal[pix] : ppu.palette[0]) & gray;
                    if (hline) hline[x + i] = HD_PACK(tile, frow, fine + i, cell->pal, pix);
                }
            }
            x += run;
        }
    }
    return 1;
}

/* ---- field objects ---- */
static int objects_on = 1, objects_drawn;

/* One 8x16 sprite at (sx, sy) in the picture's coordinates (its top row is sy) into the part of the
   canvas the caller renders, through the row masks in emph (sprites enabled, grayscale). With
   outside_only, only pixels the PPU could not draw: beyond the 256 columns or on lines it left blank. */
static void draw_sprite(const uint8_t *const chr[8], uint8_t tile, uint8_t attr, int sx, int sy, int outside_only,
                        uint8_t *out, uint32_t *hspr, const uint8_t *emph, int px0, int py0, int w, int h) {
    for (int r = 0; r < 16; r++) {
        int line = sy + r, row = line - py0;
        if (row < 0 || row >= h || !(emph[row] & 0x10)) continue;
        int drawn_line = line >= 0 && line < 240 && (ppu.line_emph[line] & 0x10);
        int fr = (attr & 0x80) ? 15 - r : r;
        unsigned a = ((tile & 1) ? 0x1000u : 0) + ((tile & 0xFEu) + (fr >> 3)) * 16u + (fr & 7);
        const uint8_t *pt = chr[(a >> 10) & 7] + (a & 0x3FF);
        uint8_t gray = (emph[row] & 1) ? 0x30 : 0xFF;
        uint32_t src = chr_tile(pt), flips = ((attr & 0x40) ? HD_HFLIP : 0) | ((attr & 0x80) ? HD_VFLIP : 0);
        for (int c = 0; c < 8; c++) {
            int px = sx + c, col = px - px0;
            if (col < 0 || col >= w) continue;
            if (outside_only && drawn_line && px >= 0 && px < 256) continue;   /* the PPU drew this pixel */
            int bit = (attr & 0x40) ? c : 7 - c;
            int pix = ((pt[0] >> bit) & 1) | (((pt[8] >> bit) & 1) << 1);
            if (!pix) continue;
            out[(size_t)row * w + col] = ppu.palette[16 + (attr & 3) * 4 + pix] & gray;
            if (hspr) hspr[(size_t)row * w + col] = HD_PACK(src, fr & 7, 7 - bit, attr & 3, pix) | flips;
        }
    }
}

/* The objects as the field engine shows them (bank 29: visibility $AABB, sprites $B02E): an object
   is shown when it is active, its group matches the map layer in use ($5D; $5F as well when $5B
   bit 7 is set, any group when bit 1 is) and it lies within 128 / 135 pixels left / right of the
   player and 116 / 107 above / below, which is the screen. It is drawn as two 8x16 sprites side by
   side, 13 lines above its position, the tiles picked from its tile byte by facing and the walking
   step. Here the objects outside that window are drawn whole, and of those inside it what the
   hardware left out: the left half, which the game hides at the left edge, and the parts beyond
   the right edge and below the last line drawn. The game builds the sprites one frame ahead of
   the picture, with the camera the picture shows (camx, camy), so what the PPU drew follows from
   the window around that camera; the object variables may already be a frame further. */
static void draw_objects(int camx, int camy, uint8_t *out, uint32_t *hspr, const uint8_t *emph, int px0, int py0, int w, int h) {
    objects_drawn = 0;
    if (!objects_on || !(ppu.frame_ctrl & 0x20) || (ram[RAM_SCENE_REQ] & 0x7F)) return;   /* 8x16 sprites; no scene change pending */
    if (cart.prg_size < (OBJ_TABLE_BANK + 1) * 8192u) return;
    const uint8_t *table = cart.prg + OBJ_TABLE_BANK * 8192u + OBJ_TABLE;
    const uint8_t *chr[8];
    for (int i = 0; i < 8; i++) chr[i] = mapper_chr_bank_ptr(ppu.frame_chr_bank[i]);
    int plx = ram[RAM_PLAYER_X] | (ram[RAM_PLAYER_X + 1] << 8);
    int ply = ram[RAM_PLAYER_Y + 1] * SCREEN_H + ram[RAM_PLAYER_Y];
    uint8_t grp_a = ram[RAM_TILE_GROUP], grp_b = ram[RAM_TILE_GROUP2] & 0xE0, sel = ram[RAM_FLAG_5B];
    for (int o = 15; o >= 0; o--) {   /* the first object ends up on top */
        const uint8_t *ob = ram + RAM_OBJECTS + o * 16;
        uint8_t f = ob[0];
        if ((f & 0xA0) != 0x80) continue;   /* inactive, or one the game never draws */
        int moving = f & 1, ox, oy;
        if (moving) {
            ox = ram[RAM_MOVER] | (ram[RAM_MOVER + 1] << 8);
            oy = ram[RAM_MOVER + 3] * SCREEN_H + ram[RAM_MOVER + 2];
        } else {
            uint8_t grp = ob[1] & 0xE0;
            if (!(sel & 0x02) && grp != grp_a && !((sel & 0x80) && grp == grp_b)) continue;   /* another layer (a hut's inside or outside) */
            ox = ob[12] | (ob[13] << 8);
            oy = ob[15] * SCREEN_H + ob[14];
        }
        int dx = ox - plx, dy = oy - ply;
        if (dx >= -128 && dx <= 135 && dy >= -116 && dy <= 107 && !(f & 0x40)) continue;   /* within the window, kept hidden */
        int sx = ox - camx, sy = oy - camy - 12;   /* screen position; the sprites' top row */
        /* the PPU drew it: within the window of the frame shown, and with its sprites on the screen
           (a top row on line 0 means sprite Y 255, which the PPU does not show) */
        int shown = sx >= 0 && sx <= 263 && sy >= 1 && sy <= 223;
        if (sx + 8 <= px0 || sx - 8 >= px0 + w || sy + 16 <= py0 || sy >= py0 + h) continue;

        /* the tiles, as the sprite routine picks them */
        uint8_t pal = ob[1] & 3, tile, ta, tb, attr;
        int direct = -1, flip = 0, base = ob[2], anim = (ram[RAM_FRAME] & 0x10) >> 1;
        if (moving && ram[RAM_MOVER + 4]) {   /* the mover's own tile: as it is when it faces a way, else by the table */
            uint8_t facing = ram[RAM_MOVER + 5], t = ram[RAM_MOVER + 4];
            if (facing) { direct = t; flip = (facing & 0x80) != 0; }
            else { base = t; anim = 0; }
        } else if ((moving || (f & 2)) && ob[2]) {   /* a fixed tile */
            direct = ob[2];
        }
        if (direct >= 0) {
            tile = (uint8_t)direct;
        } else {
            uint8_t e = table[(base & 7) | anim];
            flip = (e & 0x80) != 0;
            tile = (uint8_t)((e & 0x7F) + (base & 0xF8));
        }
        if (flip) { tb = tile; ta = (uint8_t)(tile + 2); attr = pal | 0x40; }
        else { ta = tile; tb = (uint8_t)(tile + 2); attr = pal; }
        int a_drawn = shown && sx >= 8, b_drawn = shown && sx < 256;
        draw_sprite(chr, ta, attr, sx - 8, sy, a_drawn, out, hspr, emph, px0, py0, w, h);
        draw_sprite(chr, tb, attr, sx, sy, b_drawn, out, hspr, emph, px0, py0, w, h);
        if (!a_drawn || !b_drawn) objects_drawn++;
    }
}

void widescreen_objects(int on) { objects_on = on; }
int widescreen_objects_drawn(void) { return objects_drawn; }

int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h) {
    return widescreen_render_hd(out, emph, NULL, NULL, x, y, w, h);
}

/* the PPU's own sources for n rows of the picture's columns [c0, c1), into the canvas rows at dst */
static void copy_sources(uint32_t *hbg, uint32_t *hspr, int w, int ly, int n, int c0, int c1, int px0) {
    if (!hbg) return;
    for (int i = 0; i < n; i++) {
        size_t d = (size_t)i * w + (c0 - px0), sidx = (size_t)(ly + i) * 256 + c0;
        memcpy(hbg + d, ppu.hd_bg + sidx, (size_t)(c1 - c0) * sizeof *hbg);
        memcpy(hspr + d, ppu.hd_spr + sidx, (size_t)(c1 - c0) * sizeof *hspr);
    }
}

int widescreen_render_hd(uint8_t *out, uint8_t *emph, uint32_t *hbg, uint32_t *hspr, int x, int y, int w, int h) {
    int px0 = x - WS_BAND, py0 = y - WS_VBAND;   /* the rectangle in the picture's coordinates */
    if (hbg) { memset(hbg, 0, (size_t)w * h * sizeof *hbg); memset(hspr, 0, (size_t)w * h * sizeof *hspr); }
    int c0 = px0 > 0 ? px0 : 0, c1 = px0 + w < 256 ? px0 + w : 256;   /* the picture's columns in it */
    if (c0 > px0 + w) c0 = px0 + w;
    if (c1 < c0) c1 = c0;
    mapset_t m;
    int camx, camy;
    if (overworld_setup(&m, &camx, &camy)) {
        /* the $2001 bits of each row: the PPU's own where it drew the background, else those of the
           nearest line above that shows it (line 0's above the picture) */
        uint8_t ref = ppu.line_emph[0];
        for (int ly = 0; ly < py0 && ly < 240; ly++) if (ppu.line_emph[ly] & 0x08) ref = ppu.line_emph[ly];
        for (int r = 0; r < h; r++) {
            int ly = py0 + r;
            if (ly >= 0 && ly < 240 && (ppu.line_emph[ly] & 0x08)) ref = ppu.line_emph[ly];
            emph[r] = ref;
        }
        int ok = 1;
        for (int r = 0; r < h && ok;) {
            int ly = py0 + r, drawn = ly >= 0 && ly < 240 && (ppu.line_emph[ly] & 0x08), n = 1;
            while (r + n < h) {   /* the rows of the same kind: the PPU drew their background or it did not */
                int l2 = py0 + r + n;
                if ((l2 >= 0 && l2 < 240 && (ppu.line_emph[l2] & 0x08)) != drawn) break;
                n++;
            }
            uint8_t *dst = out + (size_t)r * w;
            uint32_t *hdst = hbg ? hbg + (size_t)r * w : NULL, *hsdst = hspr ? hspr + (size_t)r * w : NULL;
            if (!drawn) {   /* the map across the whole width */
                ok = render_block(&m, camx, camy, px0, ly, w, n, emph + r, dst, hdst, w);
            } else {        /* the PPU's pixels, the map beside them */
                if (px0 < c0) ok = render_block(&m, camx, camy, px0, ly, c0 - px0, n, emph + r, dst, hdst, w);
                if (ok && c1 < px0 + w)
                    ok = render_block(&m, camx, camy, c1, ly, px0 + w - c1, n, emph + r, dst + (c1 - px0), hdst ? hdst + (c1 - px0) : NULL, w);
                for (int i = 0; i < n && c0 < c1; i++)
                    memcpy(dst + (size_t)i * w + (c0 - px0), ppu.fb + (ly + i) * 256 + c0, (size_t)(c1 - c0));
                if (c0 < c1) copy_sources(hdst, hsdst, w, ly, n, c0, c1, px0);
            }
            r += n;
        }
        if (ok) {
            draw_objects(camx, camy, out, hspr, emph, px0, py0, w, h);
            return 1;
        }
        if (hbg) { memset(hbg, 0, (size_t)w * h * sizeof *hbg); memset(hspr, 0, (size_t)w * h * sizeof *hspr); }
    }
    objects_drawn = 0;
    /* not understood: the picture as the PPU drew it, black around it */
    for (int r = 0; r < h; r++) {
        int ly = py0 + r;
        uint8_t *dst = out + (size_t)r * w;
        memset(dst, 0x0F, (size_t)w);
        emph[r] = 0;
        if (ly >= 0 && ly < 240) {
            if (c0 < c1) {
                memcpy(dst + (c0 - px0), ppu.fb + ly * 256 + c0, (size_t)(c1 - c0));
                copy_sources(hbg ? hbg + (size_t)r * w : NULL, hspr ? hspr + (size_t)r * w : NULL, w, ly, 1, c0, c1, px0);
            }
            emph[r] = ppu.line_emph[ly];
        }
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
    static uint8_t centre[256 * 240];
    if (render_block(&m, camx, camy, 0, 0, 256, 240, ppu.line_emph, centre, NULL, 256)) {
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
