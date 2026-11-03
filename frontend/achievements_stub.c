/* achievements_stub.c - used when rcheevos is not vendored: everything is a no-op */
#include "achievements.h"

int  ach_available(void) { return 0; }
int  ach_init(const char *data_dir) { (void)data_dir; return 0; }
void ach_shutdown(void) {}
void ach_login(const char *username, const char *password) { (void)username; (void)password; }
void ach_logout(void) {}
int  ach_logged_in(void) { return 0; }
const char *ach_username(void) { return ""; }
void ach_load_game(const char *rom_path, const char *title) { (void)rom_path; (void)title; }
void ach_unload_game(void) {}
int  ach_game_loaded(void) { return 0; }
void ach_reset(void) {}
void ach_frame(void) {}
void ach_pump(void) {}
void ach_set_hardcore(int on) { (void)on; }
int  ach_hardcore(void) { return 0; }
int  ach_take_reset_request(void) { return 0; }
const char *ach_status(void) { return "RetroAchievements support is not built into this executable"; }
int  ach_game_progress(int *unlocked, int *total, int *points) { *unlocked = *total = *points = 0; return 0; }
int  ach_toast(char *title, size_t title_len, char *text, size_t text_len, double *age_s) {
    (void)title; (void)title_len; (void)text; (void)text_len; (void)age_s; return 0;
}
