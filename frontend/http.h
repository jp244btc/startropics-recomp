/*
 * http.h - blocking HTTP(S) requests, made on worker threads by RetroAchievements
 * (achievements.c) and the update check (update.c). WinHTTP on Windows; libcurl elsewhere,
 * linked (HTTP_CURL) or loaded at run time (HTTP_CURL_DLOPEN), so the program also starts
 * where it is not installed; the Java side's HttpURLConnection on Android. Redirects are
 * followed.
 */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

typedef struct {
    const char *url;
    const char *post;             /* request body, or NULL for a GET */
    size_t post_len;
    const char *content_type;     /* of the body */
    const char *accept;           /* Accept header, or NULL (not sent on Android) */
    const char *user_agent;
    int timeout_ms;               /* for connecting, and for each wait for more data */
    /* called on the worker thread as the body arrives (total -1 when the server did not say) */
    void (*progress)(void *ctx, size_t received, long long total);
    void *progress_ctx;
} http_request_t;

typedef struct {
    int status;                   /* the HTTP status, or 0 when no response arrived */
    int retryable;                /* with status 0: a network problem rather than a missing transport */
    char *body;                   /* the response, NUL terminated (free() it); NULL with status 0 */
    size_t len;
    char error[256];              /* with status 0: what went wrong */
} http_response_t;

/* Main thread, before requests are made: loads libcurl (Linux) or finds the Java side (Android).
   Returns 1 when requests can be made, else 0 with the reason in *why (when why is not NULL). */
int http_init(const char **why);

/* Makes the request; returns 1 when a response arrived (whatever its status). */
int http_request(const http_request_t *rq, http_response_t *rs);

#endif
