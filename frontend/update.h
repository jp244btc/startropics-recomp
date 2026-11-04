/*
 * update.h - the menu's Updates tab: compares this build with the newest release of the GitHub
 * repository it was built from and, where the program can replace itself (Windows, Linux, the
 * Steam Deck), downloads, checks and installs it (update.c). Android opens the release page.
 */
#ifndef UPDATE_H
#define UPDATE_H

enum { UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLED, UPD_FAILED };

typedef struct {
    int state;                /* UPD_* */
    int can_install;          /* UPD_AVAILABLE: this program can install it itself */
    char latest[64];          /* the newest release's version, once a check found it */
    char page[256];           /* its page on GitHub */
    char text[400];           /* what the menu shows */
} update_info;

const char *update_version(void);    /* this build: "1.2.0", "1.2.0-3-gabc1234" (commits after the tag) or "dev" */
const char *update_edition(void);    /* "Windows", "Linux", "Steam Deck", "Android" */
void update_set_touch(int touch);    /* Android: the phone edition, which has its own package */

/* main thread: start a check, or the installation of what the check found, in the background */
void update_check(void);
void update_install(void);
void update_poll(update_info *out);  /* the state, any time */

/* After the program has shut down for "Restart now": starts the installed version with argv. On
   Linux the new version replaces this process (a Steam session carries on) and this only returns
   when that failed; on Windows it returns 1 once the new process runs (the caller then exits). */
int update_restart(char **argv);

/* --update-check: prints what a check finds. --update-test: the same as if this build were version
   `pretend`, then installs into this program's own folder and restarts into the new version with
   argv `then` (for tests in a scratch folder). Both return the exit status. */
int update_selftest(const char *pretend, char **then);

#endif
