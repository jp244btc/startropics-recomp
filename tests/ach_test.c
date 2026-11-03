/*
 * ach_test.c - console test for frontend/achievements.c (RetroAchievements support).
 *
 * It needs the network: it logs in with a deliberately wrong password to prove the
 * HTTPS transport, the worker thread, the main-thread delivery and the rc_api parsing all
 * work end to end, then hashes the ROM the RetroAchievements way and asks the server
 * which game that hash belongs to. No account is created and nothing is stored.
 *
 * Build (MSYS2 MinGW64 shell, from the repository root), see tests/run_ach_test.sh.
 * Usage: ach_test [rom path] [scratch data dir with trailing slash]
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"
#include "achievements.h"
#include "rc_hash.h"
#include "rc_consoles.h"

/* test hooks in achievements.c (ACH_TESTING build) */
int      ach_test_http_get(const char *url, char **body, size_t *body_len, int *http_status);
uint32_t ach_test_read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes);
void     ach_test_toast(const char *title, const char *text);

static int passes, failures;
static void report(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passes++; else failures++;
}

static int starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

/* pump until the status line stops starting with "Logging in" or the time runs out */
static void pump_until_login_settles(double max_seconds) {
    Uint64 start = SDL_GetPerformanceCounter();
    for (;;) {
        ach_pump();
        if (!starts_with(ach_status(), "Logging in")) return;
        if ((double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency() > max_seconds) return;
        SDL_Delay(10);
    }
}

static int extract_json_int(const char *json, const char *key) {
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') p++;
    return atoi(p);
}

int main(int argc, char **argv) {
    const char *rom = argc > 1 ? argv[1] : "rom/StarTropics (USA).nes";
    const char *data_dir = argc > 2 ? argv[2] : "build-ach/";
    char session_file[1024], hash[33];
    uint8_t *data;
    size_t size;
    FILE *f;
    const char *status;
    int network_ok = 1;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_EVENTS) != 0) { printf("FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);

    /* (a) the ROM, so memory reads have something behind them */
    apu_set_sample_rate(48000);
    if (nes_load(rom) != 0) { report(0, "nes_load() of the ROM"); return 1; }
    report(1, "nes_load() of the ROM");
    {
        uint16_t reset_vector = (uint16_t)(nes_peek(0xFFFC) | (nes_peek(0xFFFD) << 8));
        int i;
        report(reset_vector == cpu.pc, "nes_peek($FFFC/$FFFD) matches the CPU's power-on PC");
        for (i = 0; i < 120; i++) nes_run_frame();          /* let the game initialise its RAM */
        printf("      RAM after 120 frames: ");
        for (i = 0; i < 16; i++) printf("%02X ", nes_peek((uint16_t)i));
        printf("\n      $0100: ");
        for (i = 0; i < 8; i++) printf("%02X ", nes_peek((uint16_t)(0x100 + i)));
        printf("  $8000: %02X %02X  $FFFC: %02X %02X\n", nes_peek(0x8000), nes_peek(0x8001), nes_peek(0xFFFC), nes_peek(0xFFFD));
    }

    /* (b) the client, with no session file present */
    snprintf(session_file, sizeof session_file, "%sretroachievements.txt", data_dir);
    remove(session_file);
    report(ach_available() == 1, "ach_available() reports the real implementation");
    report(ach_init(data_dir) == 0, "ach_init() creates the client");
    ach_set_hardcore(0);
    report(ach_hardcore() == 0, "ach_set_hardcore(0) is forwarded");
    status = ach_status();
    printf("      status: %s\n", status);
    report(strcmp(status, "Not logged in") == 0 && !ach_logged_in() && ach_username()[0] == '\0', "fresh client: not logged in");
    {
        uint8_t buf[64];
        uint32_t n;
        memset(buf, 0xAA, sizeof buf);
        n = ach_test_read_memory(0xFFF0, buf, 32);
        report(n == 16 && buf[12] == nes_peek(0xFFFC) && buf[13] == nes_peek(0xFFFD),
               "memory reader stops at $10000 and copies the bytes below it");
        n = ach_test_read_memory(0x0000, buf, 8);
        report(n == 8 && memcmp(buf, ram, 8) == 0, "memory reader copies zero page from RAM");
        n = ach_test_read_memory(0x10000, buf, 8);
        report(n == 0, "memory reader returns 0 bytes above the CPU bus");
    }

    /* (c) a login that must fail server-side: exercises HTTPS, the worker thread, main-thread delivery, rc_api parsing */
    ach_login("startropics_port_test", "definitely-wrong");
    printf("      status: %s\n", ach_status());
    report(starts_with(ach_status(), "Logging in"), "ach_login() is asynchronous (status says logging in)");
    pump_until_login_settles(20.0);
    status = ach_status();
    printf("      status after the wrong-password login: %s\n", status);
    if (strstr(status, "Could not reach") || strstr(status, "not available in this build")) {
        network_ok = 0;
        report(0, "server round trip (the network or the server is unreachable; result not conclusive)");
    } else {
        report(starts_with(status, "Login failed: ") && !ach_logged_in(), "wrong password is rejected by the server with its message");
    }
    f = fopen(session_file, "rb");
    report(f == NULL, "no session file is written after a failed login");
    if (f) fclose(f);

    /* (d) the RetroAchievements hash of the dump, and the server's game id for it */
    data = NULL;
    size = 0;
    f = nes_fopen(rom, "rb");
    if (f) {
        long len;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0) {
            data = (uint8_t *)malloc((size_t)len);
            if (data && fread(data, 1, (size_t)len, f) == (size_t)len) size = (size_t)len;
        }
        fclose(f);
    }
    report(data && size, "ROM read into memory for hashing");
    hash[0] = '\0';
    if (data && size) {
        int ok = rc_hash_generate_from_buffer(hash, RC_CONSOLE_NINTENDO, data, size);
        printf("      rc_hash (headerless MD5): %s\n", hash);
        printf("      cart.c md5 of the same image: %s\n", cart.md5_hex);
        report(ok && strlen(hash) == 32, "rc_hash_generate_from_buffer() produced a hash");
        report(ok && strcmp(hash, cart.md5_hex) == 0, "rc_hash agrees with cart.c's headerless MD5");
    }
    if (hash[0] && network_ok) {
        char url[256], *body = NULL;
        size_t body_len = 0;
        int http_status = 0, got;
        snprintf(url, sizeof url, "https://retroachievements.org/dorequest.php?r=gameid&m=%s", hash);
        got = ach_test_http_get(url, &body, &body_len, &http_status);
        printf("      GET gameid -> HTTP %d, %u bytes: %s\n", http_status, (unsigned)body_len, body ? body : "(null)");
        if (got && body) {
            int game_id = extract_json_int(body, "\"GameID\"");
            report(http_status == 200 && strstr(body, "\"Success\":true") != NULL, "gameid lookup answered with Success:true");
            report(game_id > 0, "the server knows this dump (GameID > 0)");
            if (game_id > 0) printf("      RetroAchievements game id: %d (https://retroachievements.org/game/%d)\n", game_id, game_id);
        } else {
            report(0, "gameid lookup (transport failure)");
        }
        free(body);
    }
    free(data);

    /* (e) the toast queue ages and advances */
    {
        char title[96], text[192];
        double age = -1;
        report(ach_toast(title, sizeof title, text, sizeof text, &age) == 0, "no toast showing before any is queued");
        ach_test_toast("First", "one");
        ach_test_toast("Second", "two");
        ach_pump();
        report(ach_toast(title, sizeof title, text, sizeof text, &age) == 1 && strcmp(title, "First") == 0 && age >= 0.0 && age < 1.0,
               "first toast shows right after ach_pump() with a fresh age");
        SDL_Delay(200);
        ach_pump();
        ach_toast(title, sizeof title, text, sizeof text, &age);
        report(strcmp(title, "First") == 0 && age >= 0.15, "the toast keeps showing and ages");
    }

    /* (f) logout with no session, unload with no game: harmless */
    ach_logout();
    ach_unload_game();
    report(!ach_logged_in() && !ach_game_loaded() && ach_take_reset_request() == 0, "logout/unload without a session are no-ops");
    ach_shutdown();
    ach_pump();                                               /* must be safe after shutdown */
    report(1, "ach_shutdown() then ach_pump() is safe");

    printf("\n%d passed, %d failed%s\n", passes, failures, network_ok ? "" : " (network unreachable: online checks could not run)");
    SDL_Quit();
    return failures ? 1 : 0;
}
