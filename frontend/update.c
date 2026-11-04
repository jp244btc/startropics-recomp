/*
 * update.c - the Updates tab (update.h).
 *
 * Check: api.github.com/repos/<ST_REPO>/releases/latest, the newest published release (drafts and
 * pre-releases never are) of the repository the build came from (the Makefile and build_apk.py
 * take it from the git remote); its tag is compared with ST_VERSION (git describe: the newest v*
 * tag, plus the commits after it).
 *
 * Install (Windows, Linux, Steam Deck): download the release's package for this system
 * (ST_PACKAGE, the name `make dist` gives it), check its size and the SHA-256 digest GitHub lists
 * for it, unpack it with the system's tar (Windows 10 1803 and later have one that reads zip
 * files) into an "update" folder beside the program, then move the running program aside to
 * <name>.old (Windows and Linux both allow renaming a running executable) and the new one, with
 * the other files of the package, into its place. Settings, saves, the ROM and remaster.pak are
 * not in the package, so they stay as they are.
 *
 * Restart: the program shuts down as for Quit (the battery save and the settings are written),
 * then on Linux exec()s the new version in the same process, so a Steam game session on the
 * Deck carries on; on Windows it starts a new process.
 *
 * Android cannot replace its own package; the menu opens the release page there instead.
 * Checks and installs run on a worker thread; the menu reads the state on the main thread.
 */
#include "update.h"
#include "http.h"

#include <SDL.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char **environ;
#endif

#ifndef ST_VERSION
#  define ST_VERSION "dev"
#endif
#ifndef ST_REPO
#  define ST_REPO ""          /* owner/name on GitHub; empty when the build did not know it */
#endif
#ifndef ST_PACKAGE
#  define ST_PACKAGE ""       /* the release asset for this system (desktop builds) */
#endif

#ifdef _WIN32
#  define SEP "\\"
#  define BINARY "startropics.exe"
#else
#  define SEP "/"
#  define BINARY "startropics"
#endif
#define PACKAGE_DIR "startropics"  /* the folder inside every package */
#define TIMEOUT_MS  20000

#if defined(__GNUC__)
#  define UPD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define UPD_PRINTF(f, a)
#endif

static SDL_mutex *g_lock;            /* created by the first call on the main thread */
static update_info g_info;           /* guarded by g_lock, like the three below */
static char g_url[512], g_digest[80];
static long long g_size;
static SDL_atomic_t g_got, g_total;  /* download progress; g_got -1 once the download is done */
static char g_pretend[64];           /* --update-test */
static char g_exe[1024];             /* this program, found on the main thread before any worker runs */
static int g_touch;

static void lock(void) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (g_lock) SDL_LockMutex(g_lock);
}

static void unlock(void) {
    if (g_lock) SDL_UnlockMutex(g_lock);
}

static void UPD_PRINTF(2, 3) set_state(int state, const char *fmt, ...) {
    va_list ap;
    lock();
    g_info.state = state;
    va_start(ap, fmt);
    vsnprintf(g_info.text, sizeof g_info.text, fmt, ap);
    va_end(ap);
    unlock();
}

const char *update_version(void) {
    const char *v = g_pretend[0] ? g_pretend : ST_VERSION;
    return (v[0] == 'v' || v[0] == 'V') && isdigit((unsigned char)v[1]) ? v + 1 : v;
}

const char *update_edition(void) {
#if defined(__ANDROID__)
    return g_touch ? "Android, phone" : "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(DECK_EDITION)
    return "Steam Deck";
#else
    return "Linux";
#endif
}

void update_set_touch(int touch) { g_touch = touch; }

static const char *package_name(void) {
#ifdef __ANDROID__
    return g_touch ? "startropics-android-phone.apk" : "startropics-android-handheld.apk";   /* android/build_apk.py */
#else
    return ST_PACKAGE;
#endif
}

static const char *user_agent(void) {
    static char ua[96];
    if (!ua[0]) snprintf(ua, sizeof ua, "StarTropicsPort/%s", update_version());
    return ua;
}

/* ---- versions ---- */

/* "v1.2.3-4-gabcdef1" -> {1, 2, 3} and the 4 commits after the tag; 0 when it does not begin with a number */
static int parse_version(const char *s, long v[3], long *after) {
    char *end;
    v[0] = v[1] = v[2] = 0;
    *after = 0;
    if (*s == 'v' || *s == 'V') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    for (int i = 0; i < 3 && isdigit((unsigned char)*s); i++) {
        v[i] = strtol(s, &end, 10);
        s = end;
        if (*s != '.') break;
        s++;
    }
    if (*s == '-' && isdigit((unsigned char)s[1])) *after = strtol(s + 1, NULL, 10);
    return 1;
}

/* > 0 when version a is newer than b, < 0 when older; a build without a version is older than any */
static int compare_versions(const char *a, const char *b) {
    long va[3], vb[3], aa, ab;
    int pa = parse_version(a, va, &aa), pb = parse_version(b, vb, &ab);
    if (!pa || !pb) return pa - pb;
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    return (aa > ab) - (aa < ab);
}

/* ---- the bits of JSON a release needs ---- */

static const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *skip_string(const char *p, const char *e) {   /* p at the opening quote */
    for (p++; p < e; p++) {
        if (*p == '\\') p++;
        else if (*p == '"') return p + 1;
    }
    return e;
}

static const char *skip_value(const char *p, const char *e) {
    p = ws(p, e);
    if (p >= e) return e;
    if (*p == '"') return skip_string(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { p = skip_string(p, e); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return e;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;   /* number, true, false, null */
    return p;
}

/* the value of key among the object's own members (not those of objects inside it), or NULL */
static const char *member(const char *p, const char *e, const char *key) {
    size_t klen = strlen(key);
    p = p ? ws(p, e) : e;
    if (p >= e || *p != '{') return NULL;
    for (p++;;) {
        p = ws(p, e);
        if (p >= e || *p != '"') return NULL;
        const char *k = p + 1, *after = skip_string(p, e);
        int match = (size_t)(after - 1 - k) == klen && !memcmp(k, key, klen);
        p = ws(after, e);
        if (p >= e || *p != ':') return NULL;
        p = ws(p + 1, e);
        if (match) return p;
        p = ws(skip_value(p, e), e);
        if (p >= e || *p != ',') return NULL;
        p++;
    }
}

static void string_value(const char *p, const char *e, char *out, size_t cap) {
    size_t n = 0;
    if (p && p < e && *p == '"') {
        for (p++; p < e && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p + 1 < e) {
                c = *++p;
                if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') c = ' ';
                else if (c == 'u' && e - p > 4) {
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    unsigned long u = strtoul(hex, NULL, 16);
                    c = u < 128 ? (char)u : '?';
                    p += 4;
                }
            }
            if (n + 1 < cap) out[n++] = c;
        }
    }
    if (cap) out[n] = '\0';
}

/* ---- SHA-256, for the digest GitHub lists ---- */

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}

static void sha256_hex(const uint8_t *data, size_t len, char hex[65]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128] = { 0 };
    size_t full = len / 64 * 64, rest = len - full, tlen = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, data + i);
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tlen; i += 64) sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", (unsigned)h[i]);
}

/* ---- files (UTF-8 paths; wide-character calls on Windows) ---- */

static int find_exe(void) {   /* main thread */
    if (g_exe[0]) return 1;
#if defined(_WIN32)
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n == 0 || n >= 1024 || WideCharToMultiByte(CP_UTF8, 0, w, -1, g_exe, sizeof g_exe, NULL, NULL) <= 0) g_exe[0] = '\0';
#elif !defined(__ANDROID__)
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    g_exe[n > 0 ? n : 0] = '\0';
#endif
    return g_exe[0] != '\0';
}

#ifdef _WIN32
static int wide(const char *s, wchar_t *w, int cap) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0; }
static int f_rename(const char *from, const char *to) {   /* replaces `to` */
    wchar_t a[1100], b[1100];
    return wide(from, a, 1100) && wide(to, b, 1100) && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}
static int f_remove(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && DeleteFileW(w); }
static int f_rmdir(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && RemoveDirectoryW(w); }
static int f_mkdir(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS); }
static int f_exists(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES; }
static FILE *f_open(const char *p, const char *mode) {
    wchar_t w[1100], m[8];
    return wide(p, w, 1100) && wide(mode, m, 8) ? _wfopen(w, m) : NULL;
}
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name, int is_dir, void *ctx), void *ctx) {
    char pattern[1100], name[600];
    wchar_t w[1100];
    WIN32_FIND_DATAW fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    if (!wide(pattern, w, 1100)) return;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL) <= 0) continue;
        fn(dir, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static int f_rename(const char *from, const char *to) { return rename(from, to) == 0; }
static int f_remove(const char *p) { return unlink(p) == 0; }
static int f_rmdir(const char *p) { return rmdir(p) == 0; }
static int f_mkdir(const char *p) { return mkdir(p, 0755) == 0 || errno == EEXIST; }
static int f_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static FILE *f_open(const char *p, const char *mode) { return fopen(p, mode); }
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name, int is_dir, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char path[1300];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        fn(dir, de->d_name, lstat(path, &st) == 0 && S_ISDIR(st.st_mode), ctx);   /* symbolic links are not followed */
    }
    closedir(d);
}
#endif

static void remove_entry(const char *dir, const char *name, int is_dir, void *ctx);
static void remove_tree(const char *dir) {
    list_dir(dir, remove_entry, NULL);
    f_rmdir(dir);
}
static void remove_entry(const char *dir, const char *name, int is_dir, void *ctx) {
    char path[1300];
    (void)ctx;
    snprintf(path, sizeof path, "%s" SEP "%s", dir, name);
    if (is_dir) remove_tree(path);
    else f_remove(path);
}

/* the package's other files go beside the program; never anything of the player's */
static void move_entry(const char *dir, const char *name, int is_dir, void *ctx) {
    static const char *const keep[] = { BINARY, "startropics.ini", "retroachievements.txt", "remaster.pak", "args.txt",
                                        "launcher-background.bmp" };
    char from[1300], to[1300];
    if (is_dir) return;
    for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) if (!strcmp(name, keep[i])) return;
    snprintf(from, sizeof from, "%s" SEP "%s", dir, name);
    snprintf(to, sizeof to, "%s%s", (const char *)ctx, name);
    f_rename(from, to);
}

/* unpacks archive into dir with the system's tar */
static int run_tar(const char *archive, const char *dir, char *why, size_t cap) {
#if defined(_WIN32)
    wchar_t tar[MAX_PATH + 16], warchive[1100], wdir[1100], cmd[2400];
    UINT n = GetSystemDirectoryW(tar, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !wide(archive, warchive, 1100) || !wide(dir, wdir, 1100)) {
        snprintf(why, cap, "the system folder was not found");
        return 0;
    }
    wcscat(tar, L"\\tar.exe");
    if (GetFileAttributesW(tar) == INVALID_FILE_ATTRIBUTES) {
        snprintf(why, cap, "this Windows has no tar.exe to unpack it (Windows 10 version 1803 and later do)");
        return 0;
    }
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, warchive, wdir);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(tar, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        snprintf(why, cap, "tar.exe could not be started (error %lu)", (unsigned long)GetLastError());
        return 0;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) { snprintf(why, cap, "tar.exe could not unpack the package (exit code %lu)", (unsigned long)code); return 0; }
    return 1;
#elif defined(__ANDROID__)
    (void)archive; (void)dir;
    snprintf(why, cap, "not on Android");
    return 0;
#else
    /* without LD_PRELOAD: Steam puts its overlay library there, which would be loaded into tar too */
    size_t count = 0, k = 0;
    while (environ[count]) count++;
    char **env = (char **)calloc(count + 1, sizeof *env);
    char *argv[] = { "tar", "-xzf", (char *)archive, "-C", (char *)dir, NULL };
    pid_t pid;
    int status = 0, rc;
    if (!env) { snprintf(why, cap, "out of memory"); return 0; }
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "LD_PRELOAD=", 11)) env[k++] = environ[i];
    rc = posix_spawnp(&pid, "tar", NULL, NULL, argv, env);
    free(env);
    if (rc != 0) { snprintf(why, cap, "tar could not be started (%s)", strerror(rc)); return 0; }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { snprintf(why, cap, "tar could not unpack the package"); return 0; }
    return 1;
#endif
}

/* ---- check and install ---- */

static void check(void) {
    char url[320], tag[64] = "", page[256] = "", date[40] = "", asset[512] = "", digest[80] = "", name[128];
    const char *pkg = package_name(), *mine = update_version();
    long long size = -1;
    http_request_t rq;
    http_response_t rs;
    if (!ST_REPO[0]) {
        set_state(UPD_FAILED, "This build does not know where its releases are published (it was built without a GitHub remote).");
        return;
    }
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", ST_REPO);
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.accept = "application/vnd.github+json";
    rq.user_agent = user_agent();
    rq.timeout_ms = TIMEOUT_MS;
    if (!http_request(&rq, &rs)) { set_state(UPD_FAILED, "Could not reach GitHub: %s.", rs.error); return; }
    if (rs.status != 200) {
        free(rs.body);
        if (rs.status == 404) set_state(UPD_CURRENT, "No release has been published yet.");
        else if (rs.status == 403 || rs.status == 429) set_state(UPD_FAILED, "GitHub is turning away requests from this network for now; try again later.");
        else set_state(UPD_FAILED, "GitHub answered with status %d.", rs.status);
        return;
    }
    const char *b = rs.body, *e = rs.body + rs.len, *list = member(b, e, "assets");
    string_value(member(b, e, "tag_name"), e, tag, sizeof tag);
    string_value(member(b, e, "html_url"), e, page, sizeof page);
    string_value(member(b, e, "published_at"), e, date, sizeof date);
    date[10] = '\0';                                         /* 2026-11-03T20:15:00Z: the day */
    for (const char *p = list && *list == '[' ? ws(list + 1, e) : e; p < e && *p == '{';) {
        string_value(member(p, e, "name"), e, name, sizeof name);
        if (pkg[0] && !strcmp(name, pkg)) {
            const char *sz = member(p, e, "size");
            string_value(member(p, e, "browser_download_url"), e, asset, sizeof asset);
            string_value(member(p, e, "digest"), e, digest, sizeof digest);
            if (sz && isdigit((unsigned char)*sz)) size = strtoll(sz, NULL, 10);
            break;
        }
        p = ws(skip_value(p, e), e);
        p = p < e && *p == ',' ? ws(p + 1, e) : e;
    }
    free(rs.body);
    if (!tag[0]) { set_state(UPD_FAILED, "GitHub's answer could not be read."); return; }

    const char *latest = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    int cmp = compare_versions(tag, mine), installable = 0;
    lock();
    snprintf(g_info.latest, sizeof g_info.latest, "%s", latest);
    snprintf(g_info.page, sizeof g_info.page, "%s", page);
    snprintf(g_url, sizeof g_url, "%s", asset);
    snprintf(g_digest, sizeof g_digest, "%s", digest);
    g_size = size;
#ifndef __ANDROID__
    installable = asset[0] && g_exe[0];
#endif
    g_info.can_install = installable;
    unlock();
    if (cmp < 0) set_state(UPD_CURRENT, "This build (%s) is newer than the latest release (%s).", mine, latest);
    else if (cmp == 0) set_state(UPD_CURRENT, "You have the newest version (%s).", latest);
#ifdef __ANDROID__
    else set_state(UPD_AVAILABLE, "Version %s is out (published %s). Open the download page, download %s and open it: "
                   "Android installs it over this app and keeps its saves and settings.", latest, date, pkg);
#else
    else if (!installable) set_state(UPD_AVAILABLE, "Version %s is out (published %s), but without a package this program can "
                                     "install for itself: see %s", latest, date, page);
    else set_state(UPD_AVAILABLE, "Version %s is out (published %s). Installing downloads %.1f MB from GitHub and replaces this "
                   "program; settings, saves, the ROM and texture packs stay as they are.", latest, date,
                   (size > 0 ? size : 0) / 1048576.0);
#endif
}

static void on_progress(void *ctx, size_t received, long long total) {
    (void)ctx;
    SDL_AtomicSet(&g_got, (int)received);
    if (total > 0) SDL_AtomicSet(&g_total, (int)total);
}

/* worker thread: download, verify, unpack, swap; why says what failed */
static int install(char *why, size_t cap) {
    char url[512], digest[80], dir[1100], upd[1120], archive[1400], pkg[1200], newbin[1300], backup[1100];
    long long size;
    const char *slash;
    http_request_t rq;
    http_response_t rs;
    FILE *f;
    int ok = 0;

    lock();
    snprintf(url, sizeof url, "%s", g_url);
    snprintf(digest, sizeof digest, "%s", g_digest);
    size = g_size;
    unlock();
    slash = strrchr(g_exe, SEP[0]);
    if (!url[0] || !slash) { snprintf(why, cap, "nothing to install"); return 0; }
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - g_exe + 1), g_exe);      /* with the separator */
    snprintf(upd, sizeof upd, "%supdate", dir);
    remove_tree(upd);                                                       /* what an earlier attempt left */
    if (!f_mkdir(upd)) { snprintf(why, cap, "the program's folder (%.300s) cannot be written", dir); return 0; }

    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.user_agent = user_agent();
    rq.timeout_ms = TIMEOUT_MS;
    rq.progress = on_progress;
    if (!http_request(&rq, &rs)) { snprintf(why, cap, "the download failed: %s", rs.error); goto done; }
    SDL_AtomicSet(&g_got, -1);
    if (rs.status != 200) { snprintf(why, cap, "the download failed (GitHub answered with status %d)", rs.status); goto done; }
    if (size >= 0 && (long long)rs.len != size) { snprintf(why, cap, "the download was incomplete"); goto done; }
    if (!strncmp(digest, "sha256:", 7)) {
        char hex[65];
        sha256_hex((const uint8_t *)rs.body, rs.len, hex);
        if (strcmp(hex, digest + 7)) { snprintf(why, cap, "the download does not match the checksum GitHub lists for it"); goto done; }
    }
    snprintf(archive, sizeof archive, "%s" SEP "%s", upd, package_name());
    f = f_open(archive, "wb");
    if (!f || fwrite(rs.body, 1, rs.len, f) != rs.len) {
        if (f) fclose(f);
        snprintf(why, cap, "the download could not be saved in the update folder");
        goto done;
    }
    if (fclose(f) != 0) { snprintf(why, cap, "the download could not be saved in the update folder"); goto done; }
    free(rs.body);
    rs.body = NULL;

    if (!run_tar(archive, upd, why, cap)) goto done;
    snprintf(pkg, sizeof pkg, "%s" SEP PACKAGE_DIR, upd);
    snprintf(newbin, sizeof newbin, "%s" SEP BINARY, pkg);
    if (!f_exists(newbin)) { snprintf(why, cap, "the package holds no %s", BINARY); goto done; }
    list_dir(pkg, move_entry, dir);
    snprintf(backup, sizeof backup, "%s.old", g_exe);
    f_remove(backup);
    if (!f_rename(g_exe, backup)) { snprintf(why, cap, "the running program could not be moved aside"); goto done; }
    if (!f_rename(newbin, g_exe)) {
        f_rename(backup, g_exe);
        snprintf(why, cap, "the new program could not be moved into place");
        goto done;
    }
#ifndef _WIN32
    chmod(g_exe, 0755);
#endif
    ok = 1;
done:
    free(rs.body);
    remove_tree(upd);
    return ok;
}

static int SDLCALL check_worker(void *arg) {
    (void)arg;
    check();
    return 0;
}

static int SDLCALL install_worker(void *arg) {
    char why[400], latest[64];
    const char *name = strrchr(g_exe, SEP[0]);
    (void)arg;
    lock();
    snprintf(latest, sizeof latest, "%s", g_info.latest);
    unlock();
    if (install(why, sizeof why))
        set_state(UPD_INSTALLED, "Version %s is installed. Restart now to use it (as with Quit, play since the game last saved is "
                  "not kept), or it starts next time. The previous version is kept as %s.old.", latest, name ? name + 1 : BINARY);
    else
        set_state(UPD_FAILED, "Version %s was not installed: %s.", latest, why);
    return 0;
}

static void start(int (SDLCALL *fn)(void *), int state, const char *text) {
    SDL_Thread *t;
    set_state(state, "%s", text);
    t = SDL_CreateThread(fn, "update", NULL);
    if (t) SDL_DetachThread(t);
    else set_state(UPD_FAILED, "Could not start a background thread.");
}

void update_check(void) {
    const char *why;
    lock();
    int busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    unlock();
    if (busy) return;
    if (!http_init(&why)) { set_state(UPD_FAILED, "Could not check: %s.", why); return; }
    find_exe();
    start(check_worker, UPD_CHECKING, "Asking GitHub for the newest version...");
}

void update_install(void) {
    update_info cur;
    char text[120];
    update_poll(&cur);
    if (cur.state != UPD_AVAILABLE || !cur.can_install) return;
    SDL_AtomicSet(&g_got, 0);
    SDL_AtomicSet(&g_total, 0);
    snprintf(text, sizeof text, "Downloading version %s...", cur.latest);
    start(install_worker, UPD_DOWNLOADING, text);
}

void update_poll(update_info *out) {
    lock();
    *out = g_info;
    unlock();
    if (out->state == UPD_DOWNLOADING) {
        int got = SDL_AtomicGet(&g_got), total = SDL_AtomicGet(&g_total);
        if (got < 0) snprintf(out->text, sizeof out->text, "Installing version %s...", out->latest);
        else if (total > 0) snprintf(out->text, sizeof out->text, "Downloading version %s: %.1f of %.1f MB...", out->latest,
                                     got / 1048576.0, total / 1048576.0);
    }
}

/* ---- restart ---- */

#ifdef _WIN32
/* one argument for CommandLineToArgvW / the C runtime's parser */
static void quote_arg(char *out, size_t cap, size_t *n, const char *arg) {
#  define PUT(c) do { if (*n + 1 < cap) out[(*n)++] = (c); } while (0)
    if (*n) PUT(' ');
    PUT('"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (!*p) { while (slashes--) { PUT('\\'); PUT('\\'); } break; }
        if (*p == '"') { while (slashes--) { PUT('\\'); PUT('\\'); } PUT('\\'); PUT('"'); }
        else { while (slashes--) PUT('\\'); PUT(*p); }
    }
    PUT('"');
    out[*n] = '\0';
#  undef PUT
}
#endif

int update_restart(char **argv) {
    if (!find_exe()) return 0;
    fflush(NULL);
#if defined(_WIN32)
    char line[4096];
    size_t n = 0;
    wchar_t wexe[1100], wline[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    quote_arg(line, sizeof line, &n, g_exe);
    for (int i = 1; argv && argv[i]; i++) quote_arg(line, sizeof line, &n, argv[i]);
    if (!wide(g_exe, wexe, 1100) || !wide(line, wline, 4096)) return 0;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(wexe, wline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#elif defined(__ANDROID__)
    (void)argv;
    return 0;
#else
    execv(g_exe, argv);
    fprintf(stderr, "restart: %s: %s\n", g_exe, strerror(errno));
    return 0;
#endif
}

int update_selftest(const char *pretend, char **then) {
    update_info info;
    char why[400];
    const char *reason;
    if (pretend) snprintf(g_pretend, sizeof g_pretend, "%s", pretend);
    printf("update: this build %s (%s), package %s, repository %s\n", update_version(), update_edition(),
           package_name()[0] ? package_name() : "-", ST_REPO[0] ? ST_REPO : "-");
    if (!http_init(&reason)) { printf("update: %s\n", reason); return 1; }
    find_exe();
    check();
    update_poll(&info);
    printf("update: %s\n", info.text);
    if (!then) return info.state == UPD_FAILED;
    if (info.state != UPD_AVAILABLE || !info.can_install) return 1;
    if (!install(why, sizeof why)) { printf("update: not installed: %s\n", why); return 1; }
    printf("update: installed %s as %s\n", info.latest, g_exe);
    fflush(stdout);
    return update_restart(then) ? 0 : 1;
}
