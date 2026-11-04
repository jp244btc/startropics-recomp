/*
 * http.c - blocking HTTP(S) requests for worker threads (http.h): WinHTTP on Windows; libcurl
 * when HTTP_CURL is defined, or loaded at run time with HTTP_CURL_DLOPEN (Linux); the Java
 * side's HttpURLConnection on Android; otherwise every request fails with a clear message.
 */
#include "http.h"

#if defined(HTTP_CURL) || defined(HTTP_CURL_DLOPEN)
#  define CURL_DISABLE_TYPECHECK 1
#  include <curl/curl.h>
#  define HTTP_LIBCURL 1
#  ifdef HTTP_CURL_DLOPEN
#    include <dlfcn.h>
/* libcurl is loaded at run time, so the program also starts where it is not installed
   (the online features then say so instead of the program failing to launch) */
static struct {
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
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>
#  define HTTP_WINHTTP 1
#elif defined(__ANDROID__)
#  include <jni.h>
#  include <SDL.h>
#  define HTTP_ANDROID 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_ready;                 /* set once, on the main thread, before any worker runs */
static const char *g_why = "online features are not available in this build";

static void fail(http_response_t *rs, int retryable, const char *message) {
    free(rs->body);
    rs->body = NULL;
    rs->len = 0;
    rs->status = 0;
    rs->retryable = retryable;
    snprintf(rs->error, sizeof rs->error, "%s", message);
}

/* adds n bytes to the body, keeping it NUL terminated */
static int append(http_response_t *rs, const void *data, size_t n) {
    char *grown = (char *)realloc(rs->body, rs->len + n + 1);
    if (!grown) return 0;
    rs->body = grown;
    memcpy(rs->body + rs->len, data, n);
    rs->len += n;
    rs->body[rs->len] = '\0';
    return 1;
}

static int finish(http_response_t *rs, int status) {
    if (!rs->body && !append(rs, "", 0)) { fail(rs, 0, "out of memory"); return 0; }
    rs->status = status;
    return 1;
}

#ifdef HTTP_WINHTTP
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

static int perform(const http_request_t *rq, http_response_t *rs) {
    wchar_t *wurl = NULL, *wagent = NULL, *wpath = NULL, *wheaders = NULL;
    wchar_t host[256];
    HINTERNET session = NULL, conn = NULL, req = NULL;
    URL_COMPONENTSW uc;
    DWORD status = 0, size = sizeof status, length = 0, err;
    const char *stage = "connecting";
    long long total = -1;
    int ok = 0;

    wurl = utf8_to_wide(rq->url);
    wagent = utf8_to_wide(rq->user_agent ? rq->user_agent : "");
    if (!wurl || !wagent) { fail(rs, 0, "out of memory"); goto done; }

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;      /* NULL pointers + nonzero lengths: WinHttpCrackUrl points into wurl */
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.dwHostNameLength == 0 ||
        uc.dwHostNameLength >= sizeof host / sizeof host[0]) {
        fail(rs, 0, "invalid request URL");
        goto done;
    }
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof host[0]);
    host[uc.dwHostNameLength] = L'\0';
    {
        size_t plen = uc.dwUrlPathLength, elen = uc.dwExtraInfoLength, n = 0;
        wpath = (wchar_t *)malloc((plen + elen + 2) * sizeof *wpath);
        if (!wpath) { fail(rs, 0, "out of memory"); goto done; }
        if (plen) { memcpy(wpath, uc.lpszUrlPath, plen * sizeof *wpath); n = plen; }
        else wpath[n++] = L'/';
        if (elen) { memcpy(wpath + n, uc.lpszExtraInfo, elen * sizeof *wpath); n += elen; }
        wpath[n] = L'\0';
    }

    session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)   /* automatic proxy discovery needs Windows 8.1; fall back to the WinHTTP default proxy */
        session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto net_error;
    WinHttpSetTimeouts(session, rq->timeout_ms, rq->timeout_ms, rq->timeout_ms, rq->timeout_ms);

    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) goto net_error;
    req = WinHttpOpenRequest(conn, rq->post ? L"POST" : L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto net_error;

    if (rq->post || rq->accept) {
        char headers[512];
        int n = 0;
        if (rq->post) n += snprintf(headers + n, sizeof headers - n, "Content-Type: %s\r\n", rq->content_type);
        if (rq->accept) snprintf(headers + n, sizeof headers - n, "Accept: %s\r\n", rq->accept);
        wheaders = utf8_to_wide(headers);
        if (!wheaders) { fail(rs, 0, "out of memory"); goto done; }
    }

    stage = "sending the request";
    if (!WinHttpSendRequest(req, wheaders, wheaders ? (DWORD)-1 : 0,
                            rq->post_len ? (LPVOID)rq->post : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)rq->post_len, (DWORD)rq->post_len, 0))
        goto net_error;

    stage = "waiting for the response";
    if (!WinHttpReceiveResponse(req, NULL)) goto net_error;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX))
        goto net_error;
    size = sizeof length;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &length, &size, WINHTTP_NO_HEADER_INDEX))
        total = length;

    stage = "reading the response";
    for (;;) {
        DWORD avail = 0, got = 0;
        char *grown;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto net_error;
        if (avail == 0) break;
        grown = (char *)realloc(rs->body, rs->len + avail + 1);
        if (!grown) { fail(rs, 0, "out of memory"); goto done; }
        rs->body = grown;
        if (!WinHttpReadData(req, rs->body + rs->len, avail, &got)) goto net_error;
        if (got == 0) break;
        rs->len += got;
        rs->body[rs->len] = '\0';
        if (rq->progress) rq->progress(rq->progress_ctx, rs->len, total);
    }
    ok = finish(rs, (int)status);
    goto done;

net_error:
    err = GetLastError();
    {
        char message[200];
        snprintf(message, sizeof message, "%s while %s (WinHTTP error %lu)", winhttp_error_text(err), stage, (unsigned long)err);
        fail(rs, 1, message);
    }

done:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    free(wheaders);
    free(wpath);
    free(wagent);
    free(wurl);
    return ok;
}
#endif /* HTTP_WINHTTP */

#ifdef HTTP_LIBCURL
typedef struct {
    const http_request_t *rq;
    http_response_t *rs;
} curl_job;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_job *job = (curl_job *)userdata;
    size_t n = size * nmemb;
    return append(job->rs, ptr, n) ? n : 0;
}

static int curl_progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    curl_job *job = (curl_job *)userdata;
    (void)ultotal; (void)ulnow;
    if (dlnow > 0) job->rq->progress(job->rq->progress_ctx, (size_t)dlnow, dltotal > 0 ? (long long)dltotal : -1);
    return 0;
}

static int perform(const http_request_t *rq, http_response_t *rs) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    char errbuf[CURL_ERROR_SIZE], header[256];
    curl_job job = { rq, rs };
    CURLcode rc;
    int ok = 0;

    errbuf[0] = '\0';
    curl = curl_easy_init();
    if (!curl) { fail(rs, 1, "could not initialise libcurl"); return 0; }

    curl_easy_setopt(curl, CURLOPT_URL, rq->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, rq->user_agent ? rq->user_agent : "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &job);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)rq->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);   /* gives up after timeout_ms without data */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)(rq->timeout_ms + 999) / 1000);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (rq->progress) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &job);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    if (rq->accept) {
        snprintf(header, sizeof header, "Accept: %s", rq->accept);
        headers = curl_slist_append(headers, header);
    }
    if (rq->post) {
        snprintf(header, sizeof header, "Content-Type: %s", rq->content_type);
        headers = curl_slist_append(headers, header);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, rq->post);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)rq->post_len);
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        ok = finish(rs, (int)code);
    } else {
        fail(rs, 1, errbuf[0] ? errbuf : curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}
#endif /* HTTP_LIBCURL */

#ifdef HTTP_ANDROID
/* StarTropicsActivity.httpRequest returns the status as 4 big-endian bytes, then the body;
   status 0 means the body is an error message. */
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

/* the worker thread is attached to the Java VM by SDL */
static int perform(const http_request_t *rq, http_response_t *rs) {
    int ok = 0;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !g_http_class) { fail(rs, 1, "the network bridge is not available"); return 0; }
    jstring jurl = (*env)->NewStringUTF(env, rq->url);
    jstring jct = (*env)->NewStringUTF(env, rq->content_type ? rq->content_type : "");
    jstring jua = (*env)->NewStringUTF(env, rq->user_agent ? rq->user_agent : "");
    jbyteArray jpost = NULL;
    if (rq->post) {
        jpost = (*env)->NewByteArray(env, (jsize)rq->post_len);
        if (jpost) (*env)->SetByteArrayRegion(env, jpost, 0, (jsize)rq->post_len, (const jbyte *)rq->post);
    }
    jbyteArray res = (jbyteArray)(*env)->CallStaticObjectMethod(env, g_http_class, g_http_method, jurl, jpost, jct, jua,
                                                                (jint)rq->timeout_ms);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); res = NULL; }
    jsize n = res ? (*env)->GetArrayLength(env, res) : 0;
    if (n < 4) {
        fail(rs, 1, "no answer");
    } else {
        jbyte *b = (*env)->GetByteArrayElements(env, res, NULL);
        int status = (int)(((uint32_t)(uint8_t)b[0] << 24) | ((uint32_t)(uint8_t)b[1] << 16) |
                           ((uint32_t)(uint8_t)b[2] << 8) | (uint32_t)(uint8_t)b[3]);
        size_t blen = (size_t)(n - 4);
        if (status <= 0) {
            char message[220];
            snprintf(message, sizeof message, "%.*s", (int)(blen < 200 ? blen : 200), (const char *)b + 4);
            fail(rs, 1, message);
        } else if (!append(rs, b + 4, blen)) {
            fail(rs, 0, "out of memory");
        } else {
            ok = finish(rs, status);
            if (rq->progress) rq->progress(rq->progress_ctx, blen, (long long)blen);
        }
        (*env)->ReleaseByteArrayElements(env, res, b, JNI_ABORT);
    }
    if (res) (*env)->DeleteLocalRef(env, res);
    if (jpost) (*env)->DeleteLocalRef(env, jpost);
    (*env)->DeleteLocalRef(env, jua);
    (*env)->DeleteLocalRef(env, jct);
    (*env)->DeleteLocalRef(env, jurl);
    return ok;
}
#endif /* HTTP_ANDROID */

int http_init(const char **why) {
    if (!g_ready) {
#if defined(HTTP_CURL_DLOPEN)
        g_why = "libcurl (libcurl.so.4) is not installed";
        if (curl_load()) { curl_global_init(CURL_GLOBAL_DEFAULT); g_ready = 1; }
#elif defined(HTTP_LIBCURL)
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_ready = 1;
#elif defined(HTTP_WINHTTP)
        g_ready = 1;
#elif defined(HTTP_ANDROID)
        g_why = "the network bridge is not available";
        android_http_init();
        g_ready = g_http_class != NULL;
#endif
    }
    if (why) *why = g_ready ? "" : g_why;
    return g_ready;
}

int http_request(const http_request_t *rq, http_response_t *rs) {
    memset(rs, 0, sizeof *rs);
#if defined(HTTP_WINHTTP) || defined(HTTP_LIBCURL) || defined(HTTP_ANDROID)
    if (g_ready) return perform(rq, rs);
#endif
    fail(rs, 0, g_why);
    return 0;
}
