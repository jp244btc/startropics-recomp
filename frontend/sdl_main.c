/*
 * sdl_main.c - desktop frontend for the StarTropics native port (SDL2).
 *
 * Video: 256x240 streaming texture, integer scaling, optional overscan crop.
 * Audio: 48 kHz mono, queued from the APU ring buffer.
 * Input: keyboard (remappable via startropics.ini) and SDL game controllers.
 * Saves: battery RAM and save states under saves/, screenshots under screenshots/.
 *
 * The ROM is never bundled: the user's own .nes file is located at run time.
 */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "nes.h"
#include "palette.h"
#include "png.h"

/* defined in runtime/cart.c but not exported through nes.h */
const char *cart_known_name(void);

#define FB_W 256
#define FB_H 240
#define OVERSCAN 8
#define NES_FPS 60.0988
#define SAMPLE_RATE 48000
#define AUDIO_MAX_QUEUE_MS 150
#define AUDIO_PRIME_MS 80
#define FAST_FORWARD_FRAMES 8

typedef struct {
    SDL_Keycode a, b, select, start, up, down, left, right;
    int scale;
    int overscan;
} config_t;

static config_t cfg = { SDLK_x, SDLK_z, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, 3, 1 };

static char base_dir[1024];          /* directory of the executable, with trailing separator */
static char rom_name[256];           /* ROM file name without extension, used for save files */
static char sav_path[2048], state_path[2048];
static uint32_t palette[512];
static uint32_t frame_rgb[FB_W * FB_H];
static uint8_t wram_snapshot[8192];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *controller;
static int paused;

/* ------------------------------------------------------------------ */
/* small platform helpers                                             */
/* ------------------------------------------------------------------ */
static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)name[n - e + i]) != tolower((unsigned char)ext[i])) return 0;
    return 1;
}

/* first *.nes file in dir (alphabetical is not guaranteed; the first one the OS lists) */
static int find_first_nes(const char *dir, char *out, size_t out_len) {
#ifdef _WIN32
    char pattern[2048];
    snprintf(pattern, sizeof pattern, "%s\\*.nes", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(out, out_len, "%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (has_ext(e->d_name, ".nes")) {
            snprintf(out, out_len, "%s/%s", dir, e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
#endif
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

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
static void write_default_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("; StarTropics native port - settings\n"
          "; Key names are SDL key names (https://wiki.libsdl.org/SDL2/SDL_Keycode):\n"
          ";   single letters/digits, or names such as return, space, right shift, left ctrl, up, keypad 5\n"
          "a=x\n"
          "b=z\n"
          "select=right shift\n"
          "start=return\n"
          "up=up\n"
          "down=down\n"
          "left=left\n"
          "right=right\n"
          "; window scale factor at startup (integer)\n"
          "scale=3\n"
          "; crop 8 pixels of overscan at the top and bottom (1 = yes, 0 = no); F2 toggles at run time\n"
          "overscan=1\n"
          ";\n"
          "; Fixed keys: Tab = fast-forward (hold), P = pause, F2 = overscan, F5 = save state, F8 = load state,\n"
          ";             F11 or Alt+Enter = fullscreen, F12 = screenshot, Ctrl+R = reset, Esc = quit\n", f);
    fclose(f);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static SDL_Keycode key_from_name(const char *name) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", name);
    if (strlen(buf) == 1) buf[0] = (char)tolower((unsigned char)buf[0]);
    SDL_Keycode k = SDL_GetKeyFromName(buf);
    return k;
}

static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { write_default_config(path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strpbrk(line, ";#");
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!*key || !*val) continue;
        for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        SDL_Keycode *target = NULL;
        if (!strcmp(key, "a")) target = &cfg.a;
        else if (!strcmp(key, "b")) target = &cfg.b;
        else if (!strcmp(key, "select")) target = &cfg.select;
        else if (!strcmp(key, "start")) target = &cfg.start;
        else if (!strcmp(key, "up")) target = &cfg.up;
        else if (!strcmp(key, "down")) target = &cfg.down;
        else if (!strcmp(key, "left")) target = &cfg.left;
        else if (!strcmp(key, "right")) target = &cfg.right;
        else if (!strcmp(key, "scale")) { int s = atoi(val); if (s >= 1 && s <= 10) cfg.scale = s; continue; }
        else if (!strcmp(key, "overscan")) { cfg.overscan = atoi(val) != 0; continue; }
        if (target) {
            SDL_Keycode k = key_from_name(val);
            if (k != SDLK_UNKNOWN) *target = k;
            else fprintf(stderr, "startropics.ini: unknown key name '%s' for %s\n", val, key);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* video                                                               */
/* ------------------------------------------------------------------ */
static void frame_to_rgb(void) {
    for (int y = 0; y < FB_H; y++) {
        const uint32_t *pal = palette + ((ppu.line_emph[y] >> 5) & 7) * 64;
        const uint8_t *src = ppu.fb + y * FB_W;
        uint32_t *dst = frame_rgb + y * FB_W;
        for (int x = 0; x < FB_W; x++) dst[x] = 0xFF000000u | pal[src[x] & 0x3F];
    }
}

static void render_frame(void) {
    frame_to_rgb();
    SDL_UpdateTexture(texture, NULL, frame_rgb, FB_W * sizeof(uint32_t));
    int ww, wh;
    SDL_GetRendererOutputSize(renderer, &ww, &wh);
    SDL_Rect src = { 0, cfg.overscan ? OVERSCAN : 0, FB_W, cfg.overscan ? FB_H - 2 * OVERSCAN : FB_H };
    int scale = ww / src.w;
    if (wh / src.h < scale) scale = wh / src.h;
    if (scale < 1) scale = 1;
    SDL_Rect dst = { (ww - src.w * scale) / 2, (wh - src.h * scale) / 2, src.w * scale, src.h * scale };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &src, &dst);
    SDL_RenderPresent(renderer);
}

static int write_png(const char *path) {
    frame_to_rgb();
    uint8_t *rgb = malloc(FB_W * FB_H * 3);
    for (int i = 0; i < FB_W * FB_H; i++) {
        rgb[i * 3] = (uint8_t)(frame_rgb[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(frame_rgb[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)frame_rgb[i];
    }
    int r = png_write_rgb(path, FB_W, FB_H, rgb);
    free(rgb);
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
    if (write_png(path) == 0) printf("screenshot: %s\n", path);
}

static void update_title(void) {
    char title[160];
    snprintf(title, sizeof title, "StarTropics (native port)%s%s",
             sys.use_recomp ? "" : " [interpreter]", paused ? " [paused]" : "");
    SDL_SetWindowTitle(window, title);
}

static void toggle_fullscreen(void) {
    Uint32 flags = SDL_GetWindowFlags(window);
    int fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
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
        SDL_QueueAudio(audio_dev, buf, got * sizeof(int16_t));
    }
}

/* ------------------------------------------------------------------ */
/* saves                                                               */
/* ------------------------------------------------------------------ */
static size_t battery_size(void) { return cart.mmc6 ? 1024 : 8192; }

static void battery_check(int force) {
    if (!cart.battery) return;
    size_t n = battery_size();
    if (force || memcmp(wram_snapshot, cart.wram, n) != 0) {
        if (cart_save_battery(sav_path) == 0) memcpy(wram_snapshot, cart.wram, n);
        else fprintf(stderr, "could not write %s\n", sav_path);
    }
}

static void state_save(void) {
    size_t len = nes_state_size();
    void *buf = malloc(len);
    if (nes_state_save(buf, len) == 0) {
        FILE *f = fopen(state_path, "wb");
        if (f) { fwrite(buf, 1, len, f); fclose(f); printf("state saved: %s\n", state_path); }
        else fprintf(stderr, "could not write %s\n", state_path);
    }
    free(buf);
}

static void state_load(void) {
    size_t len = nes_state_size();
    void *buf = malloc(len);
    FILE *f = fopen(state_path, "rb");
    if (!f) { fprintf(stderr, "no save state at %s\n", state_path); free(buf); return; }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    if (n == len && nes_state_load(buf, len) == 0) {
        printf("state loaded: %s\n", state_path);
        if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
    } else fprintf(stderr, "save state %s is invalid for this build\n", state_path);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */
static int key_down(const Uint8 *ks, SDL_Keycode k) {
    SDL_Scancode sc = SDL_GetScancodeFromKey(k);
    return sc != SDL_SCANCODE_UNKNOWN && ks[sc];
}

static uint8_t read_pad(void) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (key_down(ks, cfg.a)) b |= 0x01;
    if (key_down(ks, cfg.b)) b |= 0x02;
    if (key_down(ks, cfg.select)) b |= 0x04;
    if (key_down(ks, cfg.start)) b |= 0x08;
    if (key_down(ks, cfg.up)) b |= 0x10;
    if (key_down(ks, cfg.down)) b |= 0x20;
    if (key_down(ks, cfg.left)) b |= 0x40;
    if (key_down(ks, cfg.right)) b |= 0x80;
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
    }
    return b;
}

static void open_controller(void) {
    if (controller) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) { printf("controller: %s\n", SDL_GameControllerName(controller)); return; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void fatal_box(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "StarTropics", msg, NULL);
}

static void usage(void) {
    printf("usage: startropics [rom.nes] [--selftest N out.png]\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    attach_parent_console();
#endif
    const char *rom_arg = NULL;
    int selftest_frames = 0;
    const char *selftest_png = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest") && i + 2 < argc) { selftest_frames = atoi(argv[i + 1]); selftest_png = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else rom_arg = argv[i];
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) fprintf(stderr, "game controllers unavailable: %s\n", SDL_GetError());

    char *bp = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", bp ? bp : "./");
    if (bp) SDL_free(bp);

    char ini[1200];
    snprintf(ini, sizeof ini, "%sstartropics.ini", base_dir);
    load_config(ini);

    /* ---- locate the user's ROM ---- */
    char rom_path[2048] = "";
    if (rom_arg) {
        snprintf(rom_path, sizeof rom_path, "%s", rom_arg);
    } else {
        char dir[1200];
        snprintf(dir, sizeof dir, "%srom", base_dir);
        if (!find_first_nes(dir, rom_path, sizeof rom_path) &&
            !find_first_nes("rom", rom_path, sizeof rom_path) &&
            !find_first_nes(".", rom_path, sizeof rom_path)) {
            char msg[1600];
            snprintf(msg, sizeof msg,
                     "No ROM found.\n\nThis port contains no game data. Put your own legally obtained\n"
                     "StarTropics (USA) .nes cartridge dump in:\n\n    %srom\\\n\n"
                     "or pass its path on the command line.", base_dir);
            fatal_box(msg);
            SDL_Quit();
            return 1;
        }
    }
    if (!file_exists(rom_path)) {
        char msg[1600];
        snprintf(msg, sizeof msg, "Cannot open ROM file:\n%s", rom_path);
        fatal_box(msg);
        SDL_Quit();
        return 1;
    }

    /* rom name for save files */
    {
        const char *slash = strrchr(rom_path, '/');
        const char *bslash = strrchr(rom_path, '\\');
        const char *base = rom_path;
        if (slash && slash + 1 > base) base = slash + 1;
        if (bslash && bslash + 1 > base) base = bslash + 1;
        snprintf(rom_name, sizeof rom_name, "%s", base);
        char *dot = strrchr(rom_name, '.');
        if (dot) *dot = 0;
    }

    /* ---- bring up the machine ---- */
    apu_set_sample_rate(SAMPLE_RATE);
    if (nes_load(rom_path) != 0) {
        char msg[1600];
        snprintf(msg, sizeof msg, "Could not load ROM:\n%s\n\nIs it a valid iNES file?", rom_path);
        fatal_box(msg);
        SDL_Quit();
        return 1;
    }
    const char *known = cart_known_name();
    if (recomp_rom_sha1 && strcmp(recomp_rom_sha1, cart.sha1_hex) == 0) {
        sys.use_recomp = true;
    } else {
        sys.use_recomp = false;
        if (recomp_rom_sha1)
            fprintf(stderr, "warning: the native code was generated from ROM %s but this ROM is %s;\n"
                            "         running the interpreter instead (rebuild with 'make recomp' for this dump)\n",
                    recomp_rom_sha1, cart.sha1_hex);
        else
            fprintf(stderr, "warning: this build has no recompiled code; running the interpreter\n");
    }
    printf("ROM: %s (%s) sha1 %s | mode: %s\n", rom_name, known ? known : "unknown dump", cart.sha1_hex,
           sys.use_recomp ? "native (recompiled)" : "interpreter");
    fflush(stdout);

    /* saves */
    {
        char dir[1200];
        snprintf(dir, sizeof dir, "%ssaves", base_dir);
        make_dir(dir);
        snprintf(sav_path, sizeof sav_path, "%s/%s.sav", dir, rom_name);
        snprintf(state_path, sizeof state_path, "%s/%s.state", dir, rom_name);
        if (cart.battery && cart_load_battery(sav_path) == 0) printf("battery RAM loaded from %s\n", sav_path);
        memcpy(wram_snapshot, cart.wram, battery_size());
    }
    nes_palette_build(palette);

    /* ---- window ---- */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    int vis_h = cfg.overscan ? FB_H - 2 * OVERSCAN : FB_H;
    window = SDL_CreateWindow("StarTropics (native port)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FB_W * cfg.scale, vis_h * cfg.scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { fatal_box(SDL_GetError()); SDL_Quit(); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) { fatal_box(SDL_GetError()); SDL_Quit(); return 1; }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    if (!texture) { fatal_box(SDL_GetError()); SDL_Quit(); return 1; }
    update_title();

    /* ---- self test: run N frames, dump a PNG, exit ---- */
    if (selftest_frames > 0) {
        for (int i = 0; i < selftest_frames; i++) { nes_run_frame(); audio_pump(1); }
        render_frame();
        int rc = write_png(selftest_png);
        printf("selftest: %d frames, %s -> %s\n", selftest_frames, rc == 0 ? "wrote" : "FAILED to write", selftest_png);
        SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
        SDL_Quit();
        return rc == 0 ? 0 : 1;
    }

    /* ---- audio ---- */
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = SAMPLE_RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio_dev) fprintf(stderr, "could not open audio: %s\n", SDL_GetError());
        else {
            static int16_t silence[SAMPLE_RATE * AUDIO_PRIME_MS / 1000];
            SDL_QueueAudio(audio_dev, silence, sizeof silence);   /* latency cushion */
            SDL_PauseAudioDevice(audio_dev, 0);
        }
    }
    open_controller();

    /* ---- main loop ---- */
    const Uint64 perf_freq = SDL_GetPerformanceFrequency();
    const double period = (double)perf_freq / NES_FPS;
    double next = (double)SDL_GetPerformanceCounter();
    int running = 1, frames = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_F11: toggle_fullscreen(); break;
                case SDLK_RETURN: if (ev.key.keysym.mod & KMOD_ALT) toggle_fullscreen(); break;
                case SDLK_F2: cfg.overscan = !cfg.overscan; break;
                case SDLK_F5: state_save(); break;
                case SDLK_F8: state_load(); break;
                case SDLK_F12: save_screenshot(); break;
                case SDLK_p: paused = !paused; update_title(); if (audio_dev) SDL_ClearQueuedAudio(audio_dev); break;
                case SDLK_r: if (ev.key.keysym.mod & KMOD_CTRL) { nes_reset(); printf("reset\n"); } break;
                default: break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: open_controller(); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (controller && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
                    SDL_GameControllerClose(controller); controller = NULL; open_controller();
                }
                break;
            default: break;
            }
        }
        if (!running) break;

        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        int fast = ks[SDL_SCANCODE_TAB] != 0;

        if (paused) {
            render_frame();
            SDL_Delay(16);
            next = (double)SDL_GetPerformanceCounter();
            continue;
        }

        int steps = fast ? FAST_FORWARD_FRAMES : 1;
        for (int s = 0; s < steps; s++) {
            nes_set_pad(0, read_pad());
            nes_run_frame();
            audio_pump(fast);
            if (++frames % 60 == 0) battery_check(0);
        }
        render_frame();

        if (fast) {
            next = (double)SDL_GetPerformanceCounter();
            continue;
        }
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
    if (controller) SDL_GameControllerClose(controller);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
