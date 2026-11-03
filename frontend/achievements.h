/*
 * achievements.h - RetroAchievements support built on rcheevos (third_party/rcheevos,
 * MIT). frontend/achievements.c implements it when the library is vendored;
 * achievements_stub.c provides no-op versions otherwise, so the player always builds.
 *
 * Threading: every function is called from the main thread. HTTP requests run on
 * worker threads; their results are handed back on the main thread inside ach_pump().
 *
 * Credentials: the password only ever goes to ach_login(), which sends it to the
 * RetroAchievements server once to obtain a session token. The user name and that
 * token are what gets stored (retroachievements.txt in the data folder) so the
 * session resumes next start. Nothing is logged.
 */
#ifndef ACHIEVEMENTS_H
#define ACHIEVEMENTS_H
#include <stddef.h>

int  ach_available(void);                       /* 1 when built with rcheevos */
int  ach_init(const char *data_dir);            /* creates the client; resumes a saved session if there is one. 0 = ok */
void ach_shutdown(void);

void ach_login(const char *username, const char *password);   /* asynchronous; ach_status() reports the outcome */
void ach_logout(void);                          /* ends the session and forgets the saved token */
int  ach_logged_in(void);
int  ach_token_saved(void);                     /* 1 while a session token is kept for the next start */
const char *ach_username(void);                 /* "" when not logged in */

/* Identify the cartridge dump the RetroAchievements way (the file hashed with its iNES
   header skipped) and load its achievement set. Needs a session: when none is active
   yet (login still running, or the user logs in later) the request is kept and
   completed as soon as a session exists. */
void ach_load_game(const char *rom_path, const char *title);
void ach_unload_game(void);
int  ach_game_loaded(void);

void ach_reset(void);                           /* the machine was reset or power-cycled */
void ach_frame(void);                           /* once per real emulated frame (not run-ahead, rewind or launcher attract frames) */
void ach_pump(void);                            /* every main-loop iteration: completes requests, fires events, ages the popup */

void ach_set_hardcore(int on);                  /* hardcore: the frontend must then refuse state loads, rewind and slow motion */
int  ach_hardcore(void);
int  ach_take_reset_request(void);              /* 1 once when the client wants the game reset (hardcore toggled with a game loaded) */

const char *ach_status(void);                   /* one line for the menu: session, game, progress, or the last error */
int  ach_game_progress(int *unlocked, int *total, int *points);   /* 1 when a game set is loaded */

/* The popup to draw: 1 and title/text filled while one is on screen; age_s = seconds it has been showing. */
int  ach_toast(char *title, size_t title_len, char *text, size_t text_len, double *age_s);

#endif
