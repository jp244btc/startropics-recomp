/*
 * sdl_main.c - desktop frontend for the StarTropics native port (SDL2).
 *
 * Launcher: a tabbed menu (Play / Graphics / Audio / Controls / ROM) where the
 * player selects their own cartridge dump; the same menu doubles as the pause
 * screen. Video: 256x240 frame presented with integer / fit / stretch scaling,
 * square-pixel / 4:3 / 16:9 aspect, nearest / linear / sharp filtering, in a
 * window or fullscreen. Audio: 48 kHz mono queued from the APU. Saves: battery
 * RAM and save states under saves/, screenshots under screenshots/.
 *
 * No game data is bundled: the user's own .nes file is chosen at run time.
 */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "nes.h"
#include "palette.h"
#include "png.h"
#include "font.h"
#include "ui.h"
#include "filedialog.h"

#define FB_W 256
#define FB_H 240
#define OVERSCAN 8
#define NES_FPS 60.0988
#define SAMPLE_RATE 48000
#define AUDIO_MAX_QUEUE_MS 150
#define AUDIO_PRIME_MS 80
#define FAST_FORWARD_FRAMES 8

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
enum { K_A, K_B, K_SELECT, K_START, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_COUNT };
static const char *const key_names[K_COUNT] = { "a", "b", "select", "start", "up", "down", "left", "right" };

typedef struct {
    int key[K_COUNT];
    int window_size;     /* 1..6 */
    int display_mode;    /* 0 windowed, 1 fullscreen */
    int scaling;         /* 0 integer, 1 fit, 2 stretch */
    int aspect;          /* 0 square pixels, 1 4:3, 2 16:9 */
    int filter;          /* 0 nearest, 1 linear, 2 sharp */
    int overscan;        /* crop 8 lines top/bottom */
    int volume;          /* 0..100 */
    char rom[2048];
} config_t;

static config_t cfg = {
    { SDLK_x, SDLK_z, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT },
    3, 0, 0, 1, 2, 1, 100, ""
};

static const char *const display_modes[] = { "Windowed", "Fullscreen" };
static const char *const window_sizes[]  = { "1x", "2x", "3x", "4x", "5x", "6x" };
static const char *const scalings[]      = { "Integer", "Fit window", "Stretch" };
static const char *const aspects[]       = { "Square pixels 8:7", "4:3 (CRT)", "16:9 (stretched)" };
static const char *const filters[]       = { "Nearest", "Linear", "Sharp" };
static const char *const onoff[]         = { "Off", "On" };

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
enum { ST_MENU, ST_GAME };
static int state = ST_MENU;
static int rom_ok;                    /* a verified StarTropics ROM is loaded */
static int game_started;              /* the machine has run since the ROM was loaded */
static int running = 1;

static char base_dir[1024];           /* directory of the executable, with trailing separator */
static char ini_path[1200];
static char rom_name[256];            /* ROM file name without extension, for save files */
static char sav_path[2048], state_path[2048];
static char rom_status[240];
static uint32_t palette[512];
static uint32_t frame_rgb[FB_W * FB_H];
static uint8_t wram_snapshot[8192];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *game_tex;         /* 256x240 game frame */
static SDL_Texture *sharp_tex;        /* integer-prescaled target for the sharp filter */
static int sharp_k;
static SDL_Texture *bg_tex;           /* optional launcher-background.bmp next to the executable */
static int bg_w, bg_h;
static int tex_filter = -1;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *controller;   /* XInput / mapped pads through the game-controller API */
static SDL_Joystick *joystick;           /* fallback: pads SDL has no mapping for */
static char controller_name[160] = "none detected";
static int window_size_idx = 2;       /* cfg.window_size - 1, for the menu */

/* menu */
enum { ACT_PLAY = 1, ACT_RESET, ACT_SAVE, ACT_LOAD, ACT_STOP, ACT_QUIT, ACT_CHOOSE, GFX_CHANGED, AUD_CHANGED, KEY_CHANGED };
static ui_item play_items[6], gfx_items[6], aud_items[2], ctl_items[K_COUNT + 2], rom_items[3];
static ui_tab tabs[5];
static ui_menu menu;

/* ------------------------------------------------------------------ */
/* small platform helpers                                              */
/* ------------------------------------------------------------------ */
static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int file_exists(const char *path) {
    FILE *f = nes_fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

#ifdef _WIN32
/* GUI-subsystem executable: reattach stdout/stderr when started from a console */
static void attach_parent_console(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}
#endif

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* configuration file                                                  */
/* ------------------------------------------------------------------ */
static void write_config(void) {
    FILE *f = nes_fopen(ini_path, "w");
    if (!f) return;
    fputs("; StarTropics native port - settings (also editable from the in-game menu, Esc)\n", f);
    fprintf(f, "rom=%s\n", cfg.rom);
    fputs("; key names are SDL key names: single letters/digits, or return, space, right shift, left ctrl, up, keypad 5 ...\n", f);
    for (int i = 0; i < K_COUNT; i++) fprintf(f, "%s=%s\n", key_names[i], SDL_GetKeyName((SDL_Keycode)cfg.key[i]));
    fprintf(f, "window_size=%d\n", cfg.window_size);
    fprintf(f, "display_mode=%d        ; 0 windowed, 1 fullscreen\n", cfg.display_mode);
    fprintf(f, "scaling=%d             ; 0 integer, 1 fit window, 2 stretch\n", cfg.scaling);
    fprintf(f, "aspect=%d              ; 0 square pixels, 1 4:3, 2 16:9\n", cfg.aspect);
    fprintf(f, "filter=%d              ; 0 nearest, 1 linear, 2 sharp\n", cfg.filter);
    fprintf(f, "overscan=%d            ; crop 8 lines at the top and bottom\n", cfg.overscan);
    fprintf(f, "volume=%d\n", cfg.volume);
    fputs(";\n; Fixed keys: Esc = menu, Tab = fast-forward (hold), P = pause, F5 = save state, F8 = load state,\n"
          ";             F11 or Alt+Enter = fullscreen, F12 = screenshot, Ctrl+R = reset\n", f);
    fclose(f);
}

static void load_config(void) {
    FILE *f = nes_fopen(ini_path, "r");
    if (!f) return;
    char line[2400];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0;
        char *key = trim(line), *val = eq + 1;
        /* a ';' starts a comment except inside the rom path */
        if (strcmp(key, "rom") != 0) { char *c = strpbrk(val, ";#"); if (c) *c = 0; }
        val = trim(val);
        for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        int handled = 0;
        for (int i = 0; i < K_COUNT; i++) {
            if (!strcmp(key, key_names[i])) {
                char buf[64]; snprintf(buf, sizeof buf, "%s", val);
                if (strlen(buf) == 1) buf[0] = (char)tolower((unsigned char)buf[0]);
                SDL_Keycode k = SDL_GetKeyFromName(buf);
                if (k != SDLK_UNKNOWN) cfg.key[i] = (int)k;
                handled = 1;
            }
        }
        if (handled) continue;
        if (!strcmp(key, "rom")) snprintf(cfg.rom, sizeof cfg.rom, "%s", val);
        else if (!strcmp(key, "window_size")) { int v = atoi(val); if (v >= 1 && v <= 6) cfg.window_size = v; }
        else if (!strcmp(key, "display_mode")) cfg.display_mode = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "scaling")) { int v = atoi(val); if (v >= 0 && v <= 2) cfg.scaling = v; }
        else if (!strcmp(key, "aspect")) { int v = atoi(val); if (v >= 0 && v <= 2) cfg.aspect = v; }
        else if (!strcmp(key, "filter")) { int v = atoi(val); if (v >= 0 && v <= 2) cfg.filter = v; }
        else if (!strcmp(key, "overscan")) cfg.overscan = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "volume")) { int v = atoi(val); if (v >= 0 && v <= 100) cfg.volume = v; }
        else if (!strcmp(key, "scale")) { int v = atoi(val); if (v >= 1 && v <= 6) cfg.window_size = v; }
    }
    fclose(f);
    window_size_idx = cfg.window_size - 1;
}

/* ------------------------------------------------------------------ */
/* video                                                               */
/* ------------------------------------------------------------------ */
static double target_aspect(int src_h) {
    switch (cfg.aspect) {
    case 1: return 4.0 / 3.0;
    case 2: return 16.0 / 9.0;
    default: return (double)FB_W / src_h;
    }
}

static void source_rect(SDL_Rect *src) {
    src->x = 0; src->y = cfg.overscan ? OVERSCAN : 0;
    src->w = FB_W; src->h = cfg.overscan ? FB_H - 2 * OVERSCAN : FB_H;
}

static void dest_rect(int ww, int wh, const SDL_Rect *src, SDL_Rect *dst) {
    double aspect = target_aspect(src->h);
    int w, h;
    if (cfg.scaling == 2) { dst->x = 0; dst->y = 0; dst->w = ww; dst->h = wh; return; }
    if (cfg.scaling == 0) {
        int k = wh / src->h;
        if (k < 1) k = 1;
        while (k > 1 && (int)(src->h * k * aspect + 0.5) > ww) k--;
        h = src->h * k;
        w = (int)(h * aspect + 0.5);
    } else {
        w = ww; h = (int)(w / aspect + 0.5);
        if (h > wh) { h = wh; w = (int)(h * aspect + 0.5); }
    }
    dst->x = (ww - w) / 2; dst->y = (wh - h) / 2; dst->w = w; dst->h = h;
}

static void ensure_textures(int k) {
    if (tex_filter != cfg.filter || !game_tex) {
        if (game_tex) SDL_DestroyTexture(game_tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.filter == 1 ? "1" : "0");
        game_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
        tex_filter = cfg.filter;
    }
    if (cfg.filter == 2 && (k != sharp_k || !sharp_tex)) {
        if (sharp_tex) SDL_DestroyTexture(sharp_tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        sharp_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, FB_W * k, FB_H * k);
        sharp_k = k;
    }
}

static void frame_to_rgb(void) {
    for (int y = 0; y < FB_H; y++) {
        const uint32_t *pal = palette + ((ppu.line_emph[y] >> 5) & 7) * 64;
        const uint8_t *src = ppu.fb + y * FB_W;
        uint32_t *dst = frame_rgb + y * FB_W;
        for (int x = 0; x < FB_W; x++) dst[x] = 0xFF000000u | pal[src[x] & 0x3F];
    }
}

/* draw the current game frame into the window (no present).
   backdrop = 1 renders it as the launcher hero: aspect-correct, filling the
   window, with the sharp filter regardless of the user's game settings. */
static void draw_game_ex(int backdrop) {
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    SDL_Rect src, dst;
    source_rect(&src);
    int saved_scaling = cfg.scaling, saved_filter = cfg.filter;
    if (backdrop) {
        /* cover the window: scale so the shorter side fills, centre the rest */
        double aspect = target_aspect(src.h);
        int w = ww, h = (int)(w / aspect + 0.5);
        if (h < wh) { h = wh; w = (int)(h * aspect + 0.5); }
        dst.x = (ww - w) / 2; dst.y = (wh - h) / 2; dst.w = w; dst.h = h;
        cfg.filter = 2;
    } else {
        dest_rect(ww, wh, &src, &dst);
    }
    int k = (dst.h + src.h - 1) / src.h;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    ensure_textures(k);
    frame_to_rgb();
    SDL_UpdateTexture(game_tex, NULL, frame_rgb, FB_W * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (cfg.filter == 2 && sharp_tex) {
        SDL_SetRenderTarget(renderer, sharp_tex);
        SDL_RenderCopy(renderer, game_tex, NULL, NULL);
        SDL_SetRenderTarget(renderer, NULL);
        SDL_Rect src2 = { 0, src.y * sharp_k, FB_W * sharp_k, src.h * sharp_k };
        SDL_RenderCopy(renderer, sharp_tex, &src2, &dst);
    } else {
        SDL_RenderCopy(renderer, game_tex, &src, &dst);
    }
    cfg.scaling = saved_scaling;
    cfg.filter = saved_filter;
}

static void draw_game(void) { draw_game_ex(0); }

static void render_game(void) {
    draw_game();
    SDL_RenderPresent(renderer);
}

/* optional hero image supplied by the user (an image they hold the rights to) */
static void load_backdrop(void) {
    char path[1200];
    snprintf(path, sizeof path, "%slauncher-background.bmp", base_dir);
    SDL_Surface *s = SDL_LoadBMP(path);
    if (!s) return;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    bg_tex = SDL_CreateTextureFromSurface(renderer, s);
    bg_w = s->w; bg_h = s->h;
    SDL_FreeSurface(s);
    if (bg_tex) printf("launcher background: %s\n", path);
}

static void draw_backdrop_image(void) {
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    double aspect = (double)bg_w / bg_h;
    int w = ww, h = (int)(w / aspect + 0.5);
    if (h < wh) { h = wh; w = (int)(h * aspect + 0.5); }
    SDL_Rect dst = { (ww - w) / 2, (wh - h) / 2, w, h };
    SDL_RenderCopy(renderer, bg_tex, NULL, &dst);
}

static void apply_graphics(void) {
    cfg.window_size = window_size_idx + 1;
    Uint32 flags = SDL_GetWindowFlags(window);
    int is_fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (cfg.display_mode && !is_fs) SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    else if (!cfg.display_mode && is_fs) SDL_SetWindowFullscreen(window, 0);
    if (!cfg.display_mode) {
        SDL_Rect src; source_rect(&src);
        int h = src.h * cfg.window_size;
        int w = (int)(h * target_aspect(src.h) + 0.5);
        int cw, ch;
        SDL_GetWindowSize(window, &cw, &ch);
        if (cw != w || ch != h) SDL_SetWindowSize(window, w, h);
    }
    write_config();
}

static void toggle_fullscreen(void) {
    cfg.display_mode = !cfg.display_mode;
    apply_graphics();
}

static int write_png_rgb32(const char *path, const uint32_t *px, int w, int h) {
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3] = (uint8_t)(px[i] >> 16); rgb[i * 3 + 1] = (uint8_t)(px[i] >> 8); rgb[i * 3 + 2] = (uint8_t)px[i];
    }
    int r = png_write_rgb(path, w, h, rgb);
    free(rgb);
    return r;
}

static int write_frame_png(const char *path) {
    frame_to_rgb();
    return write_png_rgb32(path, frame_rgb, FB_W, FB_H);
}

/* screenshot of the whole window (used for the UI self-test) */
static int write_window_png(const char *path) {
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    uint32_t *px = malloc((size_t)ww * wh * 4);
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, px, ww * 4) != 0) { free(px); return -1; }
    int r = write_png_rgb32(path, px, ww, wh);
    free(px);
    return r;
}

static void save_screenshot(void) {
    char dir[1200], path[2048];
    snprintf(dir, sizeof dir, "%sscreenshots", base_dir);
    make_dir(dir);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(path, sizeof path, "%s/%s_%04d%02d%02d_%02d%02d%02d.png", dir, rom_name,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (write_frame_png(path) == 0) printf("screenshot: %s\n", path);
}

static void update_title(void) {
    char title[200];
    snprintf(title, sizeof title, "StarTropics (native port)%s", rom_ok && !sys.use_recomp ? " [interpreter]" : "");
    SDL_SetWindowTitle(window, title);
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */
static void audio_pump(int muted) {
    static int16_t buf[8192];
    uint32_t n = apu_samples_available();
    while (n > 0) {
        uint32_t got = apu_read_samples(buf, n < 8192 ? n : 8192);
        if (!got) break;
        n -= got;
        if (muted || !audio_dev) continue;
        uint32_t queued = SDL_GetQueuedAudioSize(audio_dev);
        if (queued > (uint32_t)(SAMPLE_RATE * AUDIO_MAX_QUEUE_MS / 1000) * sizeof(int16_t)) continue; /* drop */
        if (cfg.volume != 100) for (uint32_t i = 0; i < got; i++) buf[i] = (int16_t)(buf[i] * cfg.volume / 100);
        SDL_QueueAudio(audio_dev, buf, got * sizeof(int16_t));
    }
}

/* ------------------------------------------------------------------ */
/* saves                                                               */
/* ------------------------------------------------------------------ */
static void battery_check(int force) {
    if (!rom_ok || !cart.battery) return;
    size_t n = cart_battery_size();
    if (force || memcmp(wram_snapshot, cart.wram, n) != 0) {
        if (cart_save_battery(sav_path) == 0) memcpy(wram_snapshot, cart.wram, n);
        else fprintf(stderr, "could not write %s\n", sav_path);
    }
}

static void state_save(void) {
    if (!rom_ok) return;
    size_t len = nes_state_size();
    void *buf = malloc(len);
    if (nes_state_save(buf, len) == 0) {
        FILE *f = nes_fopen(state_path, "wb");
        if (f) { fwrite(buf, 1, len, f); fclose(f); printf("state saved: %s\n", state_path); }
        else fprintf(stderr, "could not write %s\n", state_path);
    }
    free(buf);
}

static void state_load(void) {
    if (!rom_ok) return;
    size_t len = nes_state_size();
    void *buf = malloc(len);
    FILE *f = nes_fopen(state_path, "rb");
    if (!f) { fprintf(stderr, "no save state at %s\n", state_path); free(buf); return; }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    if (n == len && nes_state_load(buf, len) == 0) {
        printf("state loaded: %s\n", state_path);
        if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
        game_started = 1;
    } else fprintf(stderr, "save state %s is invalid for this build\n", state_path);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */
static int key_down(const Uint8 *ks, int k) {
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)k);
    return sc != SDL_SCANCODE_UNKNOWN && ks[sc];
}

static uint8_t read_pad(void) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    for (int i = 0; i < K_COUNT; i++) if (key_down(ks, cfg.key[i])) b |= (uint8_t)(1 << i);
    if (controller) {
        const int dead = 10000;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)) b |= 0x01;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)) b |= 0x02;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X)) b |= 0x02;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)) b |= 0x04;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) b |= 0x08;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= 0x10;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= 0x20;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= 0x40;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= 0x80;
        int ax = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
        if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
    } else if (joystick) {
        /* generic layout: buttons 0/1/2 = A/B/B, 6 = select, 7 = start, hat 0 = d-pad, axes 0/1 = stick */
        const int dead = 10000;
        int nb = SDL_JoystickNumButtons(joystick);
        if (nb > 0 && SDL_JoystickGetButton(joystick, 0)) b |= 0x01;
        if (nb > 1 && SDL_JoystickGetButton(joystick, 1)) b |= 0x02;
        if (nb > 2 && SDL_JoystickGetButton(joystick, 2)) b |= 0x02;
        if (nb > 6 && SDL_JoystickGetButton(joystick, 6)) b |= 0x04;
        if (nb > 7 && SDL_JoystickGetButton(joystick, 7)) b |= 0x08;
        if (SDL_JoystickNumHats(joystick) > 0) {
            Uint8 h = SDL_JoystickGetHat(joystick, 0);
            if (h & SDL_HAT_UP) b |= 0x10;
            if (h & SDL_HAT_DOWN) b |= 0x20;
            if (h & SDL_HAT_LEFT) b |= 0x40;
            if (h & SDL_HAT_RIGHT) b |= 0x80;
        }
        if (SDL_JoystickNumAxes(joystick) > 1) {
            int ax = SDL_JoystickGetAxis(joystick, 0), ay = SDL_JoystickGetAxis(joystick, 1);
            if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
            if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
        }
    }
    return b;
}

static void refresh_controls_info(void);

static void open_controller(void) {
    if (controller || joystick) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                snprintf(controller_name, sizeof controller_name, "%s", SDL_GameControllerName(controller));
                printf("controller: %s\n", controller_name);
                refresh_controls_info();
                return;
            }
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        joystick = SDL_JoystickOpen(i);
        if (joystick) {
            snprintf(controller_name, sizeof controller_name, "%s (generic mapping)", SDL_JoystickName(joystick));
            printf("joystick: %s\n", controller_name);
            refresh_controls_info();
            return;
        }
    }
}

static void close_controllers(void) {
    if (controller) { SDL_GameControllerClose(controller); controller = NULL; }
    if (joystick) { SDL_JoystickClose(joystick); joystick = NULL; }
    snprintf(controller_name, sizeof controller_name, "none detected");
    refresh_controls_info();
}

/* diagnostic: --list-controllers [file] (a GUI-subsystem exe cannot always print to a console) */
static void list_controllers(const char *out_path) {
    FILE *out = out_path ? nes_fopen(out_path, "w") : stdout;
    if (!out) out = stdout;
    int n = SDL_NumJoysticks();
    fprintf(out, "%d input device(s)\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(out, "  %d: %s  [%s]\n", i, SDL_JoystickNameForIndex(i),
                SDL_IsGameController(i) ? "game controller mapping available" : "no mapping, generic joystick fallback");
        char *m = SDL_GameControllerMappingForDeviceIndex(i);
        if (m) { fprintf(out, "     %s\n", m); SDL_free(m); }
    }
    if (out != stdout) fclose(out);
}

/* absolute path for the remembered ROM, so a relative path given on the command line survives a double-click launch */
static void absolute_path(const char *in, char *out, size_t n) {
#ifdef _WIN32
    wchar_t win[4096], wfull[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, in, -1, win, 4096) && GetFullPathNameW(win, 4096, wfull, NULL) &&
        WideCharToMultiByte(CP_UTF8, 0, wfull, -1, out, (int)n, NULL, NULL)) return;
#else
    char *r = realpath(in, NULL);
    if (r) { snprintf(out, n, "%s", r); free(r); return; }
#endif
    snprintf(out, n, "%s", in);
}

/* ------------------------------------------------------------------ */
/* ROM selection and validation                                        */
/* ------------------------------------------------------------------ */
static void set_rom_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(rom_name, sizeof rom_name, "%s", base);
    char *dot = strrchr(rom_name, '.');
    if (dot) *dot = 0;
}

static void refresh_rom_items(void);

/* loads and verifies a cartridge dump; returns 1 when it is a StarTropics ROM */
static int load_rom_file(const char *path) {
    rom_ok = 0; game_started = 0;
    if (!path || !*path) { snprintf(rom_status, sizeof rom_status, "No ROM selected"); refresh_rom_items(); return 0; }
    if (!file_exists(path)) { snprintf(rom_status, sizeof rom_status, "File not found"); refresh_rom_items(); return 0; }
    if (nes_load(path) != 0) {
        snprintf(rom_status, sizeof rom_status, "Not a valid .nes file");
        refresh_rom_items();
        return 0;
    }
    const char *known = cart_known_name();
    int native = recomp_rom_sha1 && strcmp(recomp_rom_sha1, cart.sha1_hex) == 0;
    if (!known && !native) {
        snprintf(rom_status, sizeof rom_status, "Not a StarTropics ROM (sha1 %.12s...)", cart.sha1_hex);
        refresh_rom_items();
        return 0;
    }
    sys.use_recomp = native != 0;
    rom_ok = 1;
    set_rom_name(path);
    char dir[1200];
    snprintf(dir, sizeof dir, "%ssaves", base_dir);
    make_dir(dir);
    snprintf(sav_path, sizeof sav_path, "%s/%s.sav", dir, rom_name);
    snprintf(state_path, sizeof state_path, "%s/%s.state", dir, rom_name);
    if (cart.battery && cart_load_battery(sav_path) == 0) printf("battery RAM loaded from %s\n", sav_path);
    memcpy(wram_snapshot, cart.wram, cart_battery_size());
    if (native) snprintf(rom_status, sizeof rom_status, "%s: verified, running native code", known ? known : "StarTropics");
    else snprintf(rom_status, sizeof rom_status, "%s: known dump, interpreter mode (native code is for the USA version)", known);
    {
        char full[2048];
        absolute_path(path, full, sizeof full);
        if (strcmp(cfg.rom, full) != 0) { snprintf(cfg.rom, sizeof cfg.rom, "%s", full); write_config(); }
    }
    printf("ROM: %s (%s) sha1 %s | mode: %s\n", rom_name, known ? known : "unknown", cart.sha1_hex,
           sys.use_recomp ? "native (recompiled)" : "interpreter");
    fflush(stdout);
    update_title();
    refresh_rom_items();
    return 1;
}

/* ------------------------------------------------------------------ */
/* menu                                                                */
/* ------------------------------------------------------------------ */
static void refresh_controls_info(void) {
    snprintf(ctl_items[K_COUNT].info, sizeof ctl_items[K_COUNT].info,
             "Select a row and press Enter, then the new key.\nGamepad: %.70s\n(A = A, B or X = B, Start, Back = Select, D-pad / left stick)",
             controller_name);
}

static void refresh_rom_items(void) {
    snprintf(rom_items[0].info, sizeof rom_items[0].info, "This program contains no game data. It needs your own\n"
             "StarTropics (USA) cartridge dump (.nes) to run.");
    if (rom_ok) snprintf(rom_items[2].info, sizeof rom_items[2].info, "Current: %.80s\n%.150s", rom_name, rom_status);
    else snprintf(rom_items[2].info, sizeof rom_items[2].info, "Current: none\n%.150s", rom_status);
    play_items[0].label = game_started ? "Resume game" : "Start game";
    snprintf(menu.status, sizeof menu.status, "%s", rom_ok ? rom_status : "No valid ROM loaded - open the ROM tab to choose your cartridge dump");
}

static void build_menu(void) {
    ui_item *p = play_items;
    p[0] = (ui_item){ "Start game", UI_ACTION, ACT_PLAY, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    p[1] = (ui_item){ "Reset game", UI_ACTION, ACT_RESET, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    p[2] = (ui_item){ "Save state  (F5)", UI_ACTION, ACT_SAVE, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    p[3] = (ui_item){ "Load state  (F8)", UI_ACTION, ACT_LOAD, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    p[4] = (ui_item){ "End game", UI_ACTION, ACT_STOP, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    p[5] = (ui_item){ "Quit", UI_ACTION, ACT_QUIT, NULL, NULL, 0, 0, 0, 0, NULL, "" };

    ui_item *g = gfx_items;
    g[0] = (ui_item){ "Display mode", UI_CHOICE, GFX_CHANGED, &cfg.display_mode, display_modes, 2, 0, 0, 0, NULL, "" };
    g[1] = (ui_item){ "Window size", UI_CHOICE, GFX_CHANGED, &window_size_idx, window_sizes, 6, 0, 0, 0, NULL, "" };
    g[2] = (ui_item){ "Scaling", UI_CHOICE, GFX_CHANGED, &cfg.scaling, scalings, 3, 0, 0, 0, NULL, "" };
    g[3] = (ui_item){ "Aspect ratio", UI_CHOICE, GFX_CHANGED, &cfg.aspect, aspects, 3, 0, 0, 0, NULL, "" };
    g[4] = (ui_item){ "Filter", UI_CHOICE, GFX_CHANGED, &cfg.filter, filters, 3, 0, 0, 0, NULL, "" };
    g[5] = (ui_item){ "Overscan crop", UI_CHOICE, GFX_CHANGED, &cfg.overscan, onoff, 2, 0, 0, 0, NULL, "" };

    ui_item *a = aud_items;
    a[0] = (ui_item){ "Volume", UI_INT, AUD_CHANGED, &cfg.volume, NULL, 0, 0, 100, 5, "%", "" };
    a[1] = (ui_item){ "", UI_INFO, 0, NULL, NULL, 0, 0, 0, 0, NULL, "Audio: 48 kHz, NES mixer with the console's output filters." };

    static const char *const ctl_labels[K_COUNT] = { "A button", "B button", "Select", "Start", "Up", "Down", "Left", "Right" };
    for (int i = 0; i < K_COUNT; i++)
        ctl_items[i] = (ui_item){ ctl_labels[i], UI_KEY, KEY_CHANGED, &cfg.key[i], NULL, 0, 0, 0, 0, NULL, "" };
    ctl_items[K_COUNT] = (ui_item){ "", UI_INFO, 0, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    refresh_controls_info();
    ctl_items[K_COUNT + 1] = (ui_item){ "", UI_INFO, 0, NULL, NULL, 0, 0, 0, 0, NULL,
        "In game: Esc menu, Tab fast-forward, P pause, F5/F8 states,\nF11 fullscreen, F12 screenshot, Ctrl+R reset." };

    rom_items[0] = (ui_item){ "", UI_INFO, 0, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    rom_items[1] = (ui_item){ "Choose ROM file...", UI_ACTION, ACT_CHOOSE, NULL, NULL, 0, 0, 0, 0, NULL, "" };
    rom_items[2] = (ui_item){ "", UI_INFO, 0, NULL, NULL, 0, 0, 0, 0, NULL, "" };

    tabs[0] = (ui_tab){ "Play", play_items, 6, 0 };
    tabs[1] = (ui_tab){ "Graphics", gfx_items, 6, 0 };
    tabs[2] = (ui_tab){ "Audio", aud_items, 2, 0 };
    tabs[3] = (ui_tab){ "Controls", ctl_items, K_COUNT + 2, 0 };
    tabs[4] = (ui_tab){ "ROM", rom_items, 3, 1 };
    memset(&menu, 0, sizeof menu);
    menu.tabs = tabs; menu.ntabs = 5;
    menu.title = "STARTROPICS";
    menu.subtitle = "native port";
    menu.hint = "Arrows: navigate   Enter: select   Esc: back / resume   Drop a .nes file here to load it";
    menu.hover_tab = menu.hover_item = -1;
    refresh_rom_items();
}

static void open_menu(void) {
    state = ST_MENU;
    menu.tab = 0; menu.focus = 1; menu.capture = 0;
    ui_select_first(&menu);
    refresh_rom_items();
    if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
}

static void start_game(void) {
    if (!rom_ok) { menu.tab = 4; menu.focus = 1; ui_select_first(&menu); return; }
    if (!game_started) nes_power();      /* the launcher's attract mode was running: begin fresh */
    game_started = 1;
    state = ST_GAME;
    if (audio_dev) {
        SDL_ClearQueuedAudio(audio_dev);
        static int16_t silence[SAMPLE_RATE * AUDIO_PRIME_MS / 1000];
        SDL_QueueAudio(audio_dev, silence, sizeof silence);
    }
}

static void choose_rom(void) {
    char path[2048];
    if (!file_dialog_available()) {
        snprintf(rom_status, sizeof rom_status, "No file dialog on this system: drop the .nes file onto the window");
        refresh_rom_items();
        return;
    }
    if (file_dialog_open(window, path, sizeof path)) load_rom_file(path);
}

static void menu_action(ui_event e) {
    if (e.type == UI_EV_BACK) {
        if (rom_ok && game_started) start_game();
        return;
    }
    if (e.type == UI_EV_CHANGED) {
        if (e.id == GFX_CHANGED) apply_graphics();
        else write_config();
        return;
    }
    if (e.type != UI_EV_ACTIVATE) return;
    switch (e.id) {
    case ACT_PLAY: start_game(); break;
    case ACT_RESET: if (rom_ok) { nes_reset(); game_started = 1; start_game(); } break;
    case ACT_SAVE: state_save(); break;
    case ACT_LOAD: state_load(); if (rom_ok) start_game(); break;
    case ACT_STOP: if (rom_ok) { battery_check(1); nes_power(); game_started = 0; refresh_rom_items(); ui_select_first(&menu); } break;
    case ACT_QUIT: running = 0; break;
    case ACT_CHOOSE: choose_rom(); break;
    default: break;
    }
}

/* The menu sits on a hero backdrop: the user's own image if launcher-background.bmp
   exists, otherwise the game itself (attract mode in the launcher, the frozen frame
   when paused), upscaled and dimmed. */
static void draw_menu(void) {
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    SDL_SetRenderDrawColor(renderer, 14, 18, 22, 255);
    SDL_RenderClear(renderer);
    int have_backdrop = 0;
    if (bg_tex) { draw_backdrop_image(); have_backdrop = 1; }
    else if (rom_ok) { draw_game_ex(!game_started); have_backdrop = 1; }
    if (have_backdrop) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 8, 12, 16, game_started ? 185 : 105);
        SDL_Rect all = { 0, 0, ww, wh };
        SDL_RenderFillRect(renderer, &all);
    }
    menu.backdrop = have_backdrop;
    ui_draw(renderer, &menu, ww, wh);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void fatal_box(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "StarTropics", msg, NULL);
}

static void usage(void) {
    printf("usage: startropics [rom.nes] [--selftest N out.png] [--ui-shot out.png [tab [frames]]]\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    attach_parent_console();
#endif
    const char *rom_arg = NULL, *selftest_png = NULL, *uishot_png = NULL;
    int selftest_frames = 0, uishot_tab = 0, uishot_frames = 0, list_pads = 0;
    const char *list_pads_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest") && i + 2 < argc) { selftest_frames = atoi(argv[i + 1]); selftest_png = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--ui-shot") && i + 1 < argc) {
            uishot_png = argv[++i];
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) uishot_tab = atoi(argv[++i]);
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) uishot_frames = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--list-controllers")) { list_pads = 1; if (i + 1 < argc) list_pads_file = argv[++i]; }
        else rom_arg = argv[i];
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) fprintf(stderr, "game controllers unavailable: %s\n", SDL_GetError());
    if (list_pads) { list_controllers(list_pads_file); SDL_Quit(); return 0; }

    char *bp = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", bp ? bp : "./");
    if (bp) SDL_free(bp);
    snprintf(ini_path, sizeof ini_path, "%sstartropics.ini", base_dir);
    load_config();
    if (!file_exists(ini_path)) write_config();

    apu_set_sample_rate(SAMPLE_RATE);
    nes_palette_build(palette);

    /* ---- window ---- */
    SDL_Rect src0; source_rect(&src0);
    int win_h = src0.h * cfg.window_size, win_w = (int)(win_h * target_aspect(src0.h) + 0.5);
    window = SDL_CreateWindow("StarTropics (native port)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                              (cfg.display_mode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!window) { fatal_box(SDL_GetError()); SDL_Quit(); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) { fatal_box(SDL_GetError()); SDL_Quit(); return 1; }
    font_init(renderer);
    load_backdrop();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    build_menu();
    update_title();

    /* ---- ROM: command line, else the remembered path, else rom/ next to the exe ---- */
    {
        char path[2048] = "";
        if (rom_arg) snprintf(path, sizeof path, "%s", rom_arg);
        else if (cfg.rom[0]) snprintf(path, sizeof path, "%s", cfg.rom);
        if (path[0]) load_rom_file(path);
        else { snprintf(rom_status, sizeof rom_status, "No ROM selected"); refresh_rom_items(); }
    }
    menu.tab = rom_ok ? 0 : 4;
    menu.focus = 1;
    ui_select_first(&menu);

    /* ---- self tests ---- */
    if (uishot_png) {
        menu.tab = uishot_tab < 5 ? uishot_tab : 0; ui_select_first(&menu);
        for (int i = 0; i < uishot_frames && rom_ok; i++) { nes_run_frame(); audio_pump(1); }
        draw_menu();
        SDL_RenderPresent(renderer);
        draw_menu();
        int rc = write_window_png(uishot_png);
        printf("ui-shot: %s -> %s\n", rc == 0 ? "wrote" : "FAILED", uishot_png);
        font_quit(); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
        return rc == 0 ? 0 : 1;
    }
    if (selftest_frames > 0) {
        if (!rom_ok) { fprintf(stderr, "selftest: no valid ROM (%s)\n", rom_status); SDL_Quit(); return 1; }
        for (int i = 0; i < selftest_frames; i++) { nes_run_frame(); audio_pump(1); }
        draw_game();
        SDL_RenderPresent(renderer);
        int rc = write_frame_png(selftest_png);
        printf("selftest: %d frames, %s -> %s\n", selftest_frames, rc == 0 ? "wrote" : "FAILED to write", selftest_png);
        font_quit(); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
        return rc == 0 ? 0 : 1;
    }

    /* ---- audio ---- */
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = SAMPLE_RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio_dev) fprintf(stderr, "could not open audio: %s\n", SDL_GetError());
        else SDL_PauseAudioDevice(audio_dev, 0);
    }
    open_controller();

    /* ---- main loop ---- */
    const Uint64 perf_freq = SDL_GetPerformanceFrequency();
    const double period = (double)perf_freq / NES_FPS;
    double next = (double)SDL_GetPerformanceCounter();
    int frames = 0, paused = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type == SDL_DROPFILE) {
                load_rom_file(ev.drop.file);
                SDL_free(ev.drop.file);
                if (state == ST_GAME && !rom_ok) open_menu();
                menu.tab = rom_ok ? 0 : 4; menu.focus = 1; ui_select_first(&menu);
                continue;
            }
            if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_JOYDEVICEADDED) {
                /* prefer a mapped controller over a generic joystick when one appears */
                if (joystick && ev.type == SDL_CONTROLLERDEVICEADDED) close_controllers();
                open_controller();
                continue;
            }
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED || ev.type == SDL_JOYDEVICEREMOVED) {
                SDL_JoystickID mine = -1;
                if (controller) mine = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
                else if (joystick) mine = SDL_JoystickInstanceID(joystick);
                if (ev.cdevice.which == mine) { close_controllers(); open_controller(); }
                continue;
            }
            if (state == ST_MENU) {
                if (ev.type == SDL_KEYDOWN && !menu.capture && ev.key.keysym.sym == SDLK_F11) { toggle_fullscreen(); continue; }
                if (ev.type == SDL_KEYDOWN && !menu.capture && ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) { toggle_fullscreen(); continue; }
                menu_action(ui_handle_event(&menu, &ev));
                continue;
            }
            /* in game */
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: open_menu(); break;
                case SDLK_F11: toggle_fullscreen(); break;
                case SDLK_RETURN: if (ev.key.keysym.mod & KMOD_ALT) toggle_fullscreen(); break;
                case SDLK_F2: cfg.overscan = !cfg.overscan; apply_graphics(); break;
                case SDLK_F5: state_save(); break;
                case SDLK_F8: state_load(); break;
                case SDLK_F12: save_screenshot(); break;
                case SDLK_p: paused = !paused; if (audio_dev) SDL_ClearQueuedAudio(audio_dev); break;
                case SDLK_r: if (ev.key.keysym.mod & KMOD_CTRL) { nes_reset(); printf("reset\n"); } break;
                default: break;
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
                       (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE || ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
                open_menu();   /* Guide (Xbox) button or LB opens the menu from the pad */
            }
        }
        if (!running) break;

        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        int fast = 0;
        if (state == ST_MENU) {
            /* launcher: let the game's own intro / title / demo play with sound until a game starts */
            if (rom_ok && !game_started) { nes_set_pad(0, 0); nes_run_frame(); audio_pump(0); }
            draw_menu();
            SDL_RenderPresent(renderer);
        } else if (paused) {
            render_game();
            SDL_Delay(16);
            next = (double)SDL_GetPerformanceCounter();
            continue;
        } else {
            fast = ks[SDL_SCANCODE_TAB] != 0;
            int steps = fast ? FAST_FORWARD_FRAMES : 1;
            for (int s = 0; s < steps; s++) {
                nes_set_pad(0, read_pad());
                nes_run_frame();
                audio_pump(fast);
                if (++frames % 60 == 0) battery_check(0);
            }
            render_game();
        }
        if (fast) { next = (double)SDL_GetPerformanceCounter(); continue; }
        /* pace to the NES frame rate; resync if we fall far behind */
        next += period;
        Uint64 now = SDL_GetPerformanceCounter();
        if ((double)now > next + 4 * period) next = (double)now;
        while ((double)now < next) {
            double remaining_ms = (next - (double)now) * 1000.0 / (double)perf_freq;
            if (remaining_ms > 2.5) SDL_Delay((Uint32)(remaining_ms - 1.5));
            now = SDL_GetPerformanceCounter();
        }
    }

    battery_check(1);
    write_config();
    close_controllers();
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    font_quit();
    if (sharp_tex) SDL_DestroyTexture(sharp_tex);
    if (game_tex) SDL_DestroyTexture(game_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
