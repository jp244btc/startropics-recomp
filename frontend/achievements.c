/*
 * achievements.c - RetroAchievements support for the StarTropics port, built on the
 * vendored rcheevos rc_client (third_party/rcheevos, MIT). achievements.h is the contract.
 *
 * Threading: every ach_* function, and therefore every rc_client call, runs on the main
 * thread. rc_client asks for HTTP through server_call(); the request is copied and run on
 * a detached SDL worker thread (WinHTTP on Windows; libcurl when RC_HTTP_CURL is defined, or
 * loaded at run time with RC_HTTP_CURL_DLOPEN (Linux); the Java side's HttpURLConnection on
 * Android; otherwise requests fail with a clear message). Finished requests land in a
 * mutex-protected queue that ach_pump() drains on the main thread, handing each response
 * to rc_client there. ach_shutdown() raises a flag so completions that arrive afterwards
 * are dropped instead of touching a destroyed client.
 *
 * Session: <data_dir>retroachievements.txt holds two lines, user name and session token.
 * The password only ever travels to the server inside ach_login(). Nothing secret is
 * logged: the "[ra] " lines are rc_client's own messages plus transport error codes.
 */
#include "achievements.h"
#include "nes.h"

#include "rc_client.h"
#include "rc_consoles.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(RC_HTTP_CURL) || defined(RC_HTTP_CURL_DLOPEN)
#  define CURL_DISABLE_TYPECHECK 1
#  include <curl/curl.h>
#  define ACH_HTTP_CURL 1
#  ifdef RC_HTTP_CURL_DLOPEN
#    include <dlfcn.h>
/* libcurl is loaded at run time, so the program also starts where it is not installed
   (RetroAchievements then says so instead of the program failing to launch) */
static struct {
    int loaded;
    CURLcode (*global_init)(long);
    CURL *(*easy_init)(void);
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode (*easy_perform)(CURL *);
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(CURLcode);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} dl_curl;

static int curl_load(void) {
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
#    define DL_CURL(field, sym) if (!(*(void **)&dl_curl.field = dlsym(h, sym))) return 0
    DL_CURL(global_init, "curl_global_init");
    DL_CURL(easy_init, "curl_easy_init");
    DL_CURL(easy_setopt, "curl_easy_setopt");
    DL_CURL(easy_perform, "curl_easy_perform");
    DL_CURL(easy_getinfo, "curl_easy_getinfo");
    DL_CURL(easy_cleanup, "curl_easy_cleanup");
    DL_CURL(easy_strerror, "curl_easy_strerror");
    DL_CURL(slist_append, "curl_slist_append");
    DL_CURL(slist_free_all, "curl_slist_free_all");
#    undef DL_CURL
    dl_curl.loaded = 1;
    return 1;
}
#    undef curl_easy_setopt      /* curl.h's pass-through argument-count macros */
#    undef curl_easy_getinfo
#    define curl_global_init    dl_curl.global_init
#    define curl_easy_init      dl_curl.easy_init
#    define curl_easy_setopt    dl_curl.easy_setopt
#    define curl_easy_perform   dl_curl.easy_perform
#    define curl_easy_getinfo   dl_curl.easy_getinfo
#    define curl_easy_cleanup   dl_curl.easy_cleanup
#    define curl_easy_strerror  dl_curl.easy_strerror
#    define curl_slist_append   dl_curl.slist_append
#    define curl_slist_free_all dl_curl.slist_free_all
#  endif
#elif defined(_WIN32)
#  include <winhttp.h>
#  define ACH_HTTP_WINHTTP 1
#elif defined(__ANDROID__)
#  include <jni.h>
#  define ACH_HTTP_ANDROID 1
#endif

#include <SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACH_USER_AGENT_PREFIX "StarTropicsPort/1.0 "
#define ACH_HTTP_TIMEOUT_MS   30000
#define ACH_TOAST_SECONDS     4.5
#define ACH_TOAST_MAX         16
#define ACH_DEFAULT_CONTENT_TYPE "application/x-www-form-urlencoded"

#if defined(__GNUC__)
#  define ACH_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#  define ACH_PRINTF(fmt_index, first_arg)
#endif

/* ------------------------------------------------------------------ */
/* State (main thread only unless noted)                               */
/* ------------------------------------------------------------------ */
enum { LOGIN_NONE, LOGIN_PENDING, LOGIN_FAILED };      /* "logged in" is read from rc_client itself */
enum { GAME_NONE, GAME_PENDING, GAME_FAILED };         /* "loaded" is read from rc_client itself */

static rc_client_t *g_client;
static unsigned     g_client_gen;          /* bumped by init/shutdown: responses for an older client are dropped */
static char         g_session_path[1280];
static char         g_user_agent[128];     /* immutable after ach_init(); read by worker threads */
static int          g_hardcore_wanted = 1; /* rc_client's own default */
static int          g_reset_request;

static int      g_login_state;
static unsigned g_login_gen;               /* identifies the login attempt a callback belongs to */
static int      g_login_with_token;
static char     g_login_user[128];
static char     g_login_error[256];

static int      g_game_state;
static unsigned g_load_gen;                /* identifies the load attempt a callback belongs to */
static int      g_load_in_progress;
static int      g_game_request_valid;      /* ach_load_game() was called and not undone by ach_unload_game() */
static int      g_game_request_pending;    /* the request still has to be handed to rc_client (waits for a session) */
static char     g_game_path[1024];
static char     g_game_title[128];
static char     g_game_error[256];
static uint8_t *g_rom_data;                /* kept alive while rc_client may still hash it */
static size_t   g_rom_size;

static int  g_offline;
static char g_server_error[256];
static char g_status[512];

typedef struct { char title[96]; char text[192]; } ach_toast_t;
static ach_toast_t g_toasts[ACH_TOAST_MAX];
static int    g_toast_head, g_toast_count, g_toast_showing;
static Uint64 g_toast_started;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static char *ach_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void ach_strip_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static const char *ach_basename(const char *path) {
    const char *base = path;
    const char *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static uint8_t *read_whole_file(const char *path, size_t *size_out) {
    FILE *f = nes_fopen(path, "rb");
    long len;
    uint8_t *data;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    data = (uint8_t *)malloc((size_t)len);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); fclose(f); return NULL; }
    fclose(f);
    *size_out = (size_t)len;
    return data;
}

static void ACH_PRINTF(2, 3) toast_push(const char *title, const char *fmt, ...) {
    ach_toast_t *t;
    va_list ap;
    if (g_toast_count == ACH_TOAST_MAX) {              /* full: drop the oldest */
        g_toast_head = (g_toast_head + 1) % ACH_TOAST_MAX;
        g_toast_count--;
        g_toast_showing = 0;
    }
    t = &g_toasts[(g_toast_head + g_toast_count) % ACH_TOAST_MAX];
    snprintf(t->title, sizeof t->title, "%s", title ? title : "");
    va_start(ap, fmt);
    vsnprintf(t->text, sizeof t->text, fmt, ap);
    va_end(ap);
    g_toast_count++;
}

static void toast_clear(void) {
    g_toast_head = g_toast_count = g_toast_showing = 0;
}

static void toast_advance(void) {
    Uint64 now;
    if (!g_toast_count) { g_toast_showing = 0; return; }
    now = SDL_GetPerformanceCounter();
    if (!g_toast_showing) { g_toast_showing = 1; g_toast_started = now; return; }
    if ((double)(now - g_toast_started) / (double)SDL_GetPerformanceFrequency() >= ACH_TOAST_SECONDS) {
        g_toast_head = (g_toast_head + 1) % ACH_TOAST_MAX;
        g_toast_count--;
        if (g_toast_count) g_toast_started = now;
        else g_toast_showing = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Session file: "<user>\n<token>\n"                                   */
/* ------------------------------------------------------------------ */
static int session_load(char *user, size_t user_len, char *token, size_t token_len) {
    char line1[512], line2[512];
    FILE *f;
    if (!g_session_path[0]) return 0;
    f = nes_fopen(g_session_path, "rb");
    if (!f) return 0;
    line1[0] = line2[0] = '\0';
    if (!fgets(line1, sizeof line1, f) || !fgets(line2, sizeof line2, f)) { fclose(f); return 0; }
    fclose(f);
    ach_strip_newline(line1);
    ach_strip_newline(line2);
    if (!line1[0] || !line2[0]) return 0;
    snprintf(user, user_len, "%s", line1);
    snprintf(token, token_len, "%s", line2);
    return 1;
}

static void session_save(const char *user, const char *token) {
    FILE *f;
    if (!g_session_path[0] || !user || !token) return;
    f = nes_fopen(g_session_path, "wb");
    if (!f) { printf("[ra] could not write the session file\n"); return; }
    fprintf(f, "%s\n%s\n", user, token);
    fclose(f);
}

static void session_delete(void) {
    if (!g_session_path[0]) return;
#ifdef _WIN32
    {
        wchar_t wpath[1280];
        if (MultiByteToWideChar(CP_UTF8, 0, g_session_path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])))
            DeleteFileW(wpath);
    }
#else
    remove(g_session_path);
#endif
}

/* ------------------------------------------------------------------ */
/* HTTP: one detached worker thread per request, results queued for   */
/* the main thread                                                     */
/* ------------------------------------------------------------------ */
typedef struct http_job {
    char  *url;
    char  *post_data;            /* NULL for GET */
    size_t post_len;
    char  *content_type;
    rc_client_server_callback_t callback;
    void  *callback_data;
    unsigned client_gen;
    char  *body;                 /* response body, NUL terminated, or the transport error text */
    size_t body_len;
    int    status;               /* HTTP status, or an RC_API_SERVER_RESPONSE_* code */
    struct http_job *next;
} http_job_t;

static SDL_mutex  *g_queue_mutex;          /* protects the three fields below; lives for the whole process */
static http_job_t *g_done_head, *g_done_tail;
static int         g_shutting_down;

static void http_job_free(http_job_t *job) {
    if (!job) return;
    free(job->url);
    free(job->post_data);
    free(job->content_type);
    free(job->body);
    free(job);
}

static void http_job_fail(http_job_t *job, int status, const char *message) {
    free(job->body);
    job->body = ach_strdup(message);
    job->body_len = job->body ? strlen(job->body) : 0;
    job->status = status;
}

/* worker thread (or the main thread when no worker could be started) */
static void http_job_complete(http_job_t *job) {
    SDL_LockMutex(g_queue_mutex);
    if (g_shutting_down) {
        SDL_UnlockMutex(g_queue_mutex);
        http_job_free(job);
        return;
    }
    job->next = NULL;
    if (g_done_tail) g_done_tail->next = job;
    else g_done_head = job;
    g_done_tail = job;
    SDL_UnlockMutex(g_queue_mutex);
}

#ifdef ACH_HTTP_WINHTTP
static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static const char *winhttp_error_text(DWORD err) {
    switch (err) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server name could not be resolved";
    case ERROR_WINHTTP_CANNOT_CONNECT:    return "a connection could not be established";
    case ERROR_WINHTTP_TIMEOUT:           return "the request timed out";
    case ERROR_WINHTTP_SECURE_FAILURE:    return "the TLS connection failed";
    case ERROR_WINHTTP_CONNECTION_ERROR:  return "the connection was reset";
    default:                              return "the request failed";
    }
}

/* runs on the worker thread: fills job->body / body_len / status */
static void http_perform(http_job_t *job) {
    wchar_t *wurl = NULL, *wagent = NULL, *wpath = NULL, *wheaders = NULL;
    wchar_t host[256];
    HINTERNET session = NULL, conn = NULL, req = NULL;
    URL_COMPONENTSW uc;
    DWORD status = 0, status_size = sizeof status, err;
    const char *stage = "connecting";
    char message[256];

    free(job->body);
    job->body = NULL;
    job->body_len = 0;

    wurl = utf8_to_wide(job->url);
    wagent = utf8_to_wide(g_user_agent);
    if (!wurl || !wagent) { http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory"); goto done; }

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;      /* NULL pointers + nonzero lengths: WinHttpCrackUrl points into wurl */
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.dwHostNameLength == 0 ||
        uc.dwHostNameLength >= sizeof host / sizeof host[0]) {
        http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "invalid request URL");
        goto done;
    }
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof host[0]);
    host[uc.dwHostNameLength] = L'\0';
    {
        size_t plen = uc.dwUrlPathLength, elen = uc.dwExtraInfoLength, n = 0;
        wpath = (wchar_t *)malloc((plen + elen + 2) * sizeof *wpath);
        if (!wpath) { http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory"); goto done; }
        if (plen) { memcpy(wpath, uc.lpszUrlPath, plen * sizeof *wpath); n = plen; }
        else wpath[n++] = L'/';
        if (elen) { memcpy(wpath + n, uc.lpszExtraInfo, elen * sizeof *wpath); n += elen; }
        wpath[n] = L'\0';
    }

    session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)   /* automatic proxy discovery needs Windows 8.1; fall back to the WinHTTP default proxy */
        session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto fail;
    WinHttpSetTimeouts(session, ACH_HTTP_TIMEOUT_MS, ACH_HTTP_TIMEOUT_MS, ACH_HTTP_TIMEOUT_MS, ACH_HTTP_TIMEOUT_MS);

    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) goto fail;
    req = WinHttpOpenRequest(conn, job->post_data ? L"POST" : L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto fail;

    if (job->post_data) {
        char header[256];
        snprintf(header, sizeof header, "Content-Type: %s\r\n", job->content_type);
        wheaders = utf8_to_wide(header);
        if (!wheaders) { http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory"); goto done; }
    }

    stage = "sending the request";
    if (!WinHttpSendRequest(req, wheaders, wheaders ? (DWORD)-1 : 0,
                            job->post_len ? (LPVOID)job->post_data : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)job->post_len, (DWORD)job->post_len, 0))
        goto fail;

    stage = "waiting for the response";
    if (!WinHttpReceiveResponse(req, NULL)) goto fail;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        goto fail;

    stage = "reading the response";
    for (;;) {
        DWORD avail = 0, got = 0;
        char *grown;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto fail;
        if (avail == 0) break;
        grown = (char *)realloc(job->body, job->body_len + avail + 1);
        if (!grown) { http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory"); goto done; }
        job->body = grown;
        if (!WinHttpReadData(req, job->body + job->body_len, avail, &got)) goto fail;
        if (got == 0) break;
        job->body_len += got;
    }
    if (!job->body) {
        job->body = (char *)calloc(1, 1);
        if (!job->body) { http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory"); goto done; }
    }
    job->body[job->body_len] = '\0';
    job->status = (int)status;
    goto done;

fail:
    err = GetLastError();
    snprintf(message, sizeof message, "Could not reach the RetroAchievements server while %s: %s (WinHTTP error %lu)",
             stage, winhttp_error_text(err), (unsigned long)err);
    http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, message);

done:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    free(wheaders);
    free(wpath);
    free(wagent);
    free(wurl);
}
#endif /* ACH_HTTP_WINHTTP */

#ifdef ACH_HTTP_CURL
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_job_t *job = (http_job_t *)userdata;
    size_t n = size * nmemb;
    char *grown = (char *)realloc(job->body, job->body_len + n + 1);
    if (!grown) return 0;
    job->body = grown;
    memcpy(job->body + job->body_len, ptr, n);
    job->body_len += n;
    job->body[job->body_len] = '\0';
    return n;
}

/* runs on the worker thread: fills job->body / body_len / status */
static void http_perform(http_job_t *job) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    char errbuf[CURL_ERROR_SIZE];
    CURLcode rc;

    free(job->body);
    job->body = NULL;
    job->body_len = 0;
    errbuf[0] = '\0';

#ifdef RC_HTTP_CURL_DLOPEN
    if (!dl_curl.loaded) {
        http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR,
                      "libcurl (libcurl.so.4) is not installed, so RetroAchievements cannot connect");
        return;
    }
#endif
    curl = curl_easy_init();
    if (!curl) { http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, "could not initialise libcurl"); return; }

    curl_easy_setopt(curl, CURLOPT_URL, job->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, job);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)ACH_HTTP_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)ACH_HTTP_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (job->post_data) {
        char header[256];
        snprintf(header, sizeof header, "Content-Type: %s", job->content_type);
        headers = curl_slist_append(headers, header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job->post_data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)job->post_len);
    }

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (!job->body) job->body = (char *)calloc(1, 1);
        if (!job->body) http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory");
        else job->status = (int)code;
    } else {
        char message[256];
        snprintf(message, sizeof message, "Could not reach the RetroAchievements server: %s",
                 errbuf[0] ? errbuf : curl_easy_strerror(rc));
        http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, message);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
#endif /* ACH_HTTP_CURL */

#ifdef ACH_HTTP_ANDROID
/* HTTP through the app's Java side (HttpURLConnection). StarTropicsActivity.httpRequest returns
   the status as 4 big-endian bytes, then the body; status 0 means the body is an error message. */
static jclass g_http_class;
static jmethodID g_http_method;

/* main thread: a native worker thread cannot look up the app's own classes */
static void android_http_init(void) {
    if (g_http_class) return;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) return;
    jclass cls = (*env)->FindClass(env, "org/startropics/nativeport/StarTropicsActivity");
    if (!cls) { (*env)->ExceptionClear(env); return; }
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "httpRequest",
                                            "(Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;I)[B");
    if (!m) { (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, cls); return; }
    g_http_class = (jclass)(*env)->NewGlobalRef(env, cls);
    g_http_method = m;
    (*env)->DeleteLocalRef(env, cls);
}

/* runs on the worker thread (SDL attaches it to the Java VM): fills job->body / body_len / status */
static void http_perform(http_job_t *job) {
    free(job->body);
    job->body = NULL;
    job->body_len = 0;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !g_http_class) {
        http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, "the network bridge is not available");
        return;
    }
    jstring jurl = (*env)->NewStringUTF(env, job->url);
    jstring jct = (*env)->NewStringUTF(env, job->content_type);
    jstring jua = (*env)->NewStringUTF(env, g_user_agent);
    jbyteArray jpost = NULL;
    if (job->post_data) {
        jpost = (*env)->NewByteArray(env, (jsize)job->post_len);
        if (jpost) (*env)->SetByteArrayRegion(env, jpost, 0, (jsize)job->post_len, (const jbyte *)job->post_data);
    }
    jbyteArray res = (jbyteArray)(*env)->CallStaticObjectMethod(env, g_http_class, g_http_method, jurl, jpost, jct, jua,
                                                                (jint)ACH_HTTP_TIMEOUT_MS);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); res = NULL; }
    jsize n = res ? (*env)->GetArrayLength(env, res) : 0;
    if (n < 4) {
        http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, "Could not reach the RetroAchievements server");
    } else {
        jbyte *b = (*env)->GetByteArrayElements(env, res, NULL);
        int status = (int)(((uint32_t)(uint8_t)b[0] << 24) | ((uint32_t)(uint8_t)b[1] << 16) |
                           ((uint32_t)(uint8_t)b[2] << 8) | (uint32_t)(uint8_t)b[3]);
        size_t blen = (size_t)(n - 4);
        if (status <= 0) {
            char message[300];
            snprintf(message, sizeof message, "Could not reach the RetroAchievements server: %.*s",
                     (int)(blen < 200 ? blen : 200), (const char *)b + 4);
            http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, message);
        } else {
            job->body = (char *)malloc(blen + 1);
            if (!job->body) {
                http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory");
            } else {
                memcpy(job->body, b + 4, blen);
                job->body[blen] = '\0';
                job->body_len = blen;
                job->status = status;
            }
        }
        (*env)->ReleaseByteArrayElements(env, res, b, JNI_ABORT);
    }
    if (res) (*env)->DeleteLocalRef(env, res);
    if (jpost) (*env)->DeleteLocalRef(env, jpost);
    (*env)->DeleteLocalRef(env, jua);
    (*env)->DeleteLocalRef(env, jct);
    (*env)->DeleteLocalRef(env, jurl);
}
#endif /* ACH_HTTP_ANDROID */

#if defined(ACH_HTTP_WINHTTP) || defined(ACH_HTTP_CURL) || defined(ACH_HTTP_ANDROID)
static int SDLCALL http_worker(void *arg) {
    http_job_t *job = (http_job_t *)arg;
    http_perform(job);
    http_job_complete(job);
    return 0;
}
#endif

/* rc_client -> us: issue a request. The request strings only live until we return, so copy them. */
static void RC_CCONV server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                                 void *callback_data, rc_client_t *client) {
    http_job_t *job = (http_job_t *)calloc(1, sizeof *job);
    if (!job) return;
    job->callback = callback;
    job->callback_data = callback_data;
    job->client_gen = g_client_gen;
    job->url = ach_strdup(request->url ? request->url : "");
    if (request->post_data) {
        job->post_data = ach_strdup(request->post_data);
        job->post_len = strlen(request->post_data);
    }
    job->content_type = ach_strdup(request->content_type && request->content_type[0] ?
                                   request->content_type : ACH_DEFAULT_CONTENT_TYPE);
    if (!job->url || !job->content_type || (request->post_data && !job->post_data)) {
        http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory");
        http_job_complete(job);
        return;
    }
#if defined(ACH_HTTP_WINHTTP) || defined(ACH_HTTP_CURL) || defined(ACH_HTTP_ANDROID)
    {
        SDL_Thread *thread = SDL_CreateThread(http_worker, "ra-http", job);
        if (!thread) {
            http_job_fail(job, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, "could not start a network thread");
            http_job_complete(job);
            return;
        }
        SDL_DetachThread(thread);
    }
#else
    http_job_fail(job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "online features are not available in this build");
    http_job_complete(job);
#endif
}

/* main thread: hand every finished request to rc_client */
static void deliver_responses(void) {
    http_job_t *list;
    SDL_LockMutex(g_queue_mutex);
    list = g_done_head;
    g_done_head = g_done_tail = NULL;
    SDL_UnlockMutex(g_queue_mutex);
    while (list) {
        http_job_t *job = list;
        list = job->next;
        if (g_client && job->client_gen == g_client_gen) {
            rc_api_server_response_t response;
            response.body = job->body;
            response.body_length = job->body_len;
            response.http_status_code = job->status;
            job->callback(&response, job->callback_data);
        }
        http_job_free(job);
    }
}

/* ------------------------------------------------------------------ */
/* rc_client callbacks                                                 */
/* ------------------------------------------------------------------ */
static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client) {
    uint32_t i;
    for (i = 0; i < num_bytes; i++) {
        uint32_t a = address + i;
        if (a >= 0x10000) break;
        buffer[i] = nes_peek((uint16_t)a);
    }
    return i;
}

static void RC_CCONV log_message(const char *message, const rc_client_t *client) {
    printf("[ra] %s\n", message);
    fflush(stdout);
}

static void RC_CCONV event_handler(const rc_client_event_t *event, rc_client_t *client) {
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement)
            toast_push("Achievement Unlocked", "%s (+%u)", event->achievement->title ? event->achievement->title : "",
                       (unsigned)event->achievement->points);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t *game = rc_client_get_game_info(client);
        toast_push("Mastered!", "%s: every achievement unlocked", game && game->title ? game->title : g_game_title);
        break;
    }
    case RC_CLIENT_EVENT_RESET:
        g_reset_request = 1;
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR: {
        const char *msg = event->server_error && event->server_error->error_message ?
                          event->server_error->error_message : "unknown error";
        const char *api = event->server_error && event->server_error->api ? event->server_error->api : NULL;
        if (api) snprintf(g_server_error, sizeof g_server_error, "%s (%s)", msg, api);
        else snprintf(g_server_error, sizeof g_server_error, "%s", msg);
        toast_push("Server error", "%s", msg);
        break;
    }
    case RC_CLIENT_EVENT_DISCONNECTED:
        g_offline = 1;
        toast_push("Offline", "unlocks will be sent when the connection returns");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        g_offline = 0;
        toast_push("Reconnected", "pending unlocks have been sent");
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        if (event->leaderboard)
            toast_push("Leaderboard attempt started", "%s", event->leaderboard->title ? event->leaderboard->title : "");
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        if (event->leaderboard) {
            const char *title = event->leaderboard->title ? event->leaderboard->title : "";
            if (event->leaderboard->tracker_value && event->leaderboard->tracker_value[0])
                toast_push("Leaderboard submitted", "%s: %s", title, event->leaderboard->tracker_value);
            else
                toast_push("Leaderboard submitted", "%s", title);
        }
        break;
    default:
        break;
    }
}

static void RC_CCONV login_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
    if ((unsigned)(uintptr_t)userdata != g_login_gen) return;    /* a newer login attempt superseded this one */
    if (result == RC_OK) {
        const rc_client_user_t *user = rc_client_get_user_info(client);
        g_login_state = LOGIN_NONE;
        g_login_error[0] = '\0';
        g_server_error[0] = '\0';
        if (user) {
            const char *name = user->display_name && user->display_name[0] ? user->display_name : user->username;
            unsigned score = rc_client_get_hardcore_enabled(client) ? user->score : user->score_casual;
            session_save(user->username, user->token);
            toast_push("Logged in", "%s, %u points", name ? name : "", score);
        }
        if (g_game_request_valid) g_game_request_pending = 1;   /* the pump starts the load */
    } else if (result == RC_ABORTED) {
        g_login_state = LOGIN_NONE;                                /* ach_logout() during the request */
    } else {
        g_login_state = LOGIN_FAILED;
        snprintf(g_login_error, sizeof g_login_error, "%s", error_message ? error_message : rc_error_str(result));
        /* a saved token the server rejected will not work next time either */
        if (g_login_with_token && (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED))
            session_delete();
    }
}

static void RC_CCONV load_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
    if ((unsigned)(uintptr_t)userdata != g_load_gen) return;     /* a newer load superseded this one */
    g_load_in_progress = 0;
    free(g_rom_data);
    g_rom_data = NULL;
    g_rom_size = 0;
    if (result == RC_OK) {
        const rc_client_game_t *game = rc_client_get_game_info(client);
        rc_client_user_game_summary_t summary;
        rc_client_get_user_game_summary(client, &summary);
        g_game_state = GAME_NONE;
        g_game_error[0] = '\0';
        g_server_error[0] = '\0';
        toast_push(game && game->title ? game->title : g_game_title, "%u of %u achievements unlocked, %u points",
                   (unsigned)summary.num_unlocked_achievements, (unsigned)summary.num_promoted_achievements,
                   (unsigned)summary.points_unlocked);
    } else if (result == RC_NO_GAME_LOADED) {
        g_game_state = GAME_FAILED;
        snprintf(g_game_error, sizeof g_game_error, "this ROM is not in the RetroAchievements database");
        toast_push("RetroAchievements", "this ROM is not in the database");
    } else if (result == RC_ABORTED) {
        g_game_state = GAME_NONE;
    } else if (result == RC_LOGIN_REQUIRED) {
        g_game_state = GAME_NONE;
        g_game_request_pending = 1;                                /* try again once a session exists */
    } else {
        g_game_state = GAME_FAILED;
        snprintf(g_game_error, sizeof g_game_error, "could not load the achievement set: %s",
                 error_message ? error_message : rc_error_str(result));
    }
}

/* ------------------------------------------------------------------ */
/* Internal state transitions                                          */
/* ------------------------------------------------------------------ */
static void forget_rom_buffer(void) {
    free(g_rom_data);
    g_rom_data = NULL;
    g_rom_size = 0;
}

/* rc_client_logout() also unloads the game: mirror that, and re-arm the load request for the next session */
static void after_logout(void) {
    g_game_state = GAME_NONE;
    g_game_error[0] = '\0';
    g_load_in_progress = 0;
    g_load_gen++;
    forget_rom_buffer();
    g_game_request_pending = g_game_request_valid;
    g_offline = 0;
    g_server_error[0] = '\0';
}

static void start_pending_load(void) {
    uint8_t *data;
    size_t size;
    if (!g_client || !g_game_request_pending || g_load_in_progress) return;
    if (!rc_client_get_user_info(g_client)) return;              /* no session yet: keep waiting */
    g_game_request_pending = 0;
    data = read_whole_file(g_game_path, &size);
    if (!data) {
        g_game_state = GAME_FAILED;
        snprintf(g_game_error, sizeof g_game_error, "could not read %.200s", ach_basename(g_game_path));
        return;
    }
    forget_rom_buffer();
    g_rom_data = data;                                             /* freed in load_callback */
    g_rom_size = size;
    g_game_state = GAME_PENDING;
    g_game_error[0] = '\0';
    g_load_in_progress = 1;
    g_load_gen++;
    rc_client_begin_identify_and_load_game(g_client, RC_CONSOLE_NINTENDO, g_game_path, g_rom_data, g_rom_size,
                                           load_callback, (void *)(uintptr_t)g_load_gen);
}

static void begin_login(const char *user, const char *secret, int with_token) {
    if (rc_client_get_user_info(g_client) || g_login_state == LOGIN_PENDING) {
        rc_client_logout(g_client);                                /* ends the old session; its callback is ignored */
        after_logout();
    }
    g_login_gen++;
    g_login_state = LOGIN_PENDING;
    g_login_with_token = with_token;
    g_login_error[0] = '\0';
    snprintf(g_login_user, sizeof g_login_user, "%s", user);
    if (with_token)
        rc_client_begin_login_with_token(g_client, user, secret, login_callback, (void *)(uintptr_t)g_login_gen);
    else
        rc_client_begin_login_with_password(g_client, user, secret, login_callback, (void *)(uintptr_t)g_login_gen);
}

static void reset_state(void) {
    g_login_state = LOGIN_NONE;
    g_login_with_token = 0;
    g_login_user[0] = g_login_error[0] = '\0';
    g_game_state = GAME_NONE;
    g_load_in_progress = 0;
    g_game_error[0] = '\0';
    forget_rom_buffer();
    g_offline = 0;
    g_server_error[0] = '\0';
    g_reset_request = 0;
    toast_clear();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
int ach_available(void) { return 1; }

int ach_init(const char *data_dir) {
    char user[128], token[256], clause[64];
    if (g_client) return 0;
    if (!data_dir) data_dir = "";
    if ((size_t)snprintf(g_session_path, sizeof g_session_path, "%sretroachievements.txt", data_dir) >= sizeof g_session_path) {
        g_session_path[0] = '\0';
        return -1;
    }
    if (!g_queue_mutex) {
        g_queue_mutex = SDL_CreateMutex();
        if (!g_queue_mutex) return -1;
    }
    SDL_LockMutex(g_queue_mutex);
    g_shutting_down = 0;
    SDL_UnlockMutex(g_queue_mutex);
    g_client_gen++;
#ifdef ACH_HTTP_CURL
    {
        static int curl_ready;
        if (!curl_ready) {
#ifdef RC_HTTP_CURL_DLOPEN
            if (curl_load()) curl_global_init(CURL_GLOBAL_DEFAULT);
            else printf("[ra] libcurl.so.4 not found: RetroAchievements cannot connect\n");
#else
            curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
            curl_ready = 1;
        }
    }
#endif
#ifdef ACH_HTTP_ANDROID
    android_http_init();
#endif
    g_client = rc_client_create(read_memory, server_call);
    if (!g_client) return -1;
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_event_handler(g_client, event_handler);
    rc_client_set_hardcore_enabled(g_client, g_hardcore_wanted);
    clause[0] = '\0';
    rc_client_get_user_agent_clause(g_client, clause, sizeof clause);
    snprintf(g_user_agent, sizeof g_user_agent, ACH_USER_AGENT_PREFIX "%s", clause);
    reset_state();
    g_game_request_pending = g_game_request_valid;                 /* a ROM registered before init loads once a session exists */
    if (session_load(user, sizeof user, token, sizeof token))
        begin_login(user, token, 1);
    return 0;
}

void ach_shutdown(void) {
    http_job_t *list = NULL;
    if (g_queue_mutex) {
        SDL_LockMutex(g_queue_mutex);
        g_shutting_down = 1;                                       /* workers still running free their own results */
        list = g_done_head;
        g_done_head = g_done_tail = NULL;
        SDL_UnlockMutex(g_queue_mutex);
    }
    while (list) {
        http_job_t *job = list;
        list = job->next;
        http_job_free(job);
    }
    if (g_client) {
        rc_client_destroy(g_client);
        g_client = NULL;
    }
    g_client_gen++;
    reset_state();
}

void ach_login(const char *username, const char *password) {
    if (!g_client) return;
    if (!username || !username[0] || !password || !password[0]) {
        g_login_state = LOGIN_FAILED;
        snprintf(g_login_error, sizeof g_login_error, "a user name and password are required");
        return;
    }
    begin_login(username, password, 0);
}

void ach_logout(void) {
    if (g_client) {
        rc_client_logout(g_client);
        after_logout();
    }
    g_login_state = LOGIN_NONE;
    g_login_error[0] = '\0';
    g_login_user[0] = '\0';
    session_delete();
}

int ach_logged_in(void) {
    return g_client && rc_client_get_user_info(g_client) != NULL;
}

const char *ach_username(void) {
    const rc_client_user_t *user = g_client ? rc_client_get_user_info(g_client) : NULL;
    return user && user->username ? user->username : "";
}

void ach_load_game(const char *rom_path, const char *title) {
    if (!rom_path || !rom_path[0]) { ach_unload_game(); return; }
    snprintf(g_game_path, sizeof g_game_path, "%s", rom_path);
    snprintf(g_game_title, sizeof g_game_title, "%s", title && title[0] ? title : ach_basename(rom_path));
    g_game_request_valid = 1;
    g_game_request_pending = 1;
    g_load_in_progress = 0;                                        /* an earlier load still running is superseded */
    g_load_gen++;
    g_game_state = GAME_NONE;
    g_game_error[0] = '\0';
    start_pending_load();                                          /* immediately when a session exists, else from ach_pump() */
}

void ach_unload_game(void) {
    g_game_request_valid = g_game_request_pending = 0;
    g_load_in_progress = 0;
    g_load_gen++;
    g_game_state = GAME_NONE;
    g_game_error[0] = '\0';
    g_game_title[0] = '\0';
    g_game_path[0] = '\0';
    if (g_client) rc_client_unload_game(g_client);
    forget_rom_buffer();
}

int ach_game_loaded(void) {
    return g_client && rc_client_is_game_loaded(g_client);
}

void ach_reset(void) {
    if (g_client) rc_client_reset(g_client);
}

void ach_frame(void) {
    if (g_client) rc_client_do_frame(g_client);
}

void ach_pump(void) {
    if (g_queue_mutex) deliver_responses();
    if (g_client) {
        rc_client_idle(g_client);
        start_pending_load();
    }
    toast_advance();
}

void ach_set_hardcore(int on) {
    g_hardcore_wanted = on ? 1 : 0;
    if (g_client) rc_client_set_hardcore_enabled(g_client, g_hardcore_wanted);
}

int ach_hardcore(void) {
    return g_client ? rc_client_get_hardcore_enabled(g_client) : g_hardcore_wanted;
}

int ach_take_reset_request(void) {
    int r = g_reset_request;
    g_reset_request = 0;
    return r;
}

const char *ach_status(void) {
    const rc_client_user_t *user;
    const char *name;
    unsigned score;
    if (!g_client) {
        snprintf(g_status, sizeof g_status, "RetroAchievements client is not running");
        return g_status;
    }
    if (g_offline) {
        snprintf(g_status, sizeof g_status, "Offline: unlocks will be sent when the connection returns");
        return g_status;
    }
    if (g_server_error[0]) {
        snprintf(g_status, sizeof g_status, "Server error: %s", g_server_error);
        return g_status;
    }
    user = rc_client_get_user_info(g_client);
    if (!user) {
        if (g_login_state == LOGIN_PENDING) {
            if (g_login_user[0]) snprintf(g_status, sizeof g_status, "Logging in as %s...", g_login_user);
            else snprintf(g_status, sizeof g_status, "Logging in...");
        } else if (g_login_state == LOGIN_FAILED) {
            snprintf(g_status, sizeof g_status, "Login failed: %s", g_login_error);
        } else {
            snprintf(g_status, sizeof g_status, "Not logged in");
        }
        return g_status;
    }
    name = user->display_name && user->display_name[0] ? user->display_name : (user->username ? user->username : "");
    score = rc_client_get_hardcore_enabled(g_client) ? user->score : user->score_casual;
    if (rc_client_is_game_loaded(g_client)) {
        const rc_client_game_t *game = rc_client_get_game_info(g_client);
        rc_client_user_game_summary_t summary;
        rc_client_get_user_game_summary(g_client, &summary);
        snprintf(g_status, sizeof g_status, "Logged in as %s - %s: %u/%u achievements, %u points", name,
                 game && game->title ? game->title : g_game_title, (unsigned)summary.num_unlocked_achievements,
                 (unsigned)summary.num_promoted_achievements, (unsigned)summary.points_unlocked);
    } else if (g_game_state == GAME_PENDING) {
        snprintf(g_status, sizeof g_status, "Logged in as %s (%u points) - identifying %s...", name, score, g_game_title);
    } else if (g_game_state == GAME_FAILED) {
        snprintf(g_status, sizeof g_status, "Logged in as %s (%u points) - %s", name, score, g_game_error);
    } else {
        snprintf(g_status, sizeof g_status, "Logged in as %s (%u points) - no game loaded", name, score);
    }
    return g_status;
}

int ach_game_progress(int *unlocked, int *total, int *points) {
    rc_client_user_game_summary_t summary;
    if (!g_client || !rc_client_is_game_loaded(g_client)) {
        if (unlocked) *unlocked = 0;
        if (total) *total = 0;
        if (points) *points = 0;
        return 0;
    }
    rc_client_get_user_game_summary(g_client, &summary);
    if (unlocked) *unlocked = (int)summary.num_unlocked_achievements;
    if (total) *total = (int)summary.num_promoted_achievements;
    if (points) *points = (int)summary.points_unlocked;
    return 1;
}

int ach_toast(char *title, size_t title_len, char *text, size_t text_len, double *age_s) {
    const ach_toast_t *t;
    if (!g_toast_showing || !g_toast_count) return 0;
    t = &g_toasts[g_toast_head];
    if (title && title_len) snprintf(title, title_len, "%s", t->title);
    if (text && text_len) snprintf(text, text_len, "%s", t->text);
    if (age_s) *age_s = (double)(SDL_GetPerformanceCounter() - g_toast_started) / (double)SDL_GetPerformanceFrequency();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Test hooks (tests/ach_test.c, compiled with -DACH_TESTING only)     */
/* ------------------------------------------------------------------ */
#ifdef ACH_TESTING
/* Synchronous GET through the same transport the worker threads use. The caller frees *body. */
int ach_test_http_get(const char *url, char **body, size_t *body_len, int *http_status) {
    http_job_t job;
    memset(&job, 0, sizeof job);
    job.url = ach_strdup(url);
    job.content_type = ach_strdup(ACH_DEFAULT_CONTENT_TYPE);
    if (!job.url || !job.content_type) {
        http_job_fail(&job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "out of memory");
    } else {
#if defined(ACH_HTTP_WINHTTP) || defined(ACH_HTTP_CURL)
        http_perform(&job);
#else
        http_job_fail(&job, RC_API_SERVER_RESPONSE_CLIENT_ERROR, "online features are not available in this build");
#endif
    }
    *body = job.body;
    *body_len = job.body_len;
    *http_status = job.status;
    free(job.url);
    free(job.content_type);
    return job.status > 0;
}

/* The memory reader rc_client uses, for checking the address range handling. */
uint32_t ach_test_read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes) {
    return read_memory(address, buffer, num_bytes, g_client);
}

/* Lets the test exercise the toast queue without a live achievement set. */
void ach_test_toast(const char *title, const char *text) {
    toast_push(title, "%s", text);
}
#endif
