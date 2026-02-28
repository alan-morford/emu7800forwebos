/*
 * updater.c
 *
 * Silent update checker via App Museum II web service.
 * Background pthread performs HTTP GET, parses JSON response,
 * compares versions. Main thread polls result via updater_has_update().
 *
 * Copyright (c) 2026 EMU7800
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/time.h>
#include <dlfcn.h>
#include "updater.h"

/* Must match appinfo.json version */
#define APP_VERSION "1.6.2"

/* App Museum II endpoint (plain HTTP, no SSL) */
#define UPDATE_HOST "appcatalog.webosarchive.org"
#define UPDATE_PORT 80
#define UPDATE_PATH "/WebService/getLatestVersionInfo.php?app=EMU7800/" APP_VERSION
#define DETAIL_PATH "/WebService/getMuseumDetails.php?id=1005823"

/* Buffer sizes */
#define HTTP_BUF_SIZE  4096
#define VERSION_SIZE   32
#define NOTE_SIZE      512
#define URI_SIZE       512

/* External logging */
extern void log_msg(const char *msg);

/* Thread-shared state — written by background thread, read by main thread.
 * Write order: strings first, then memory barrier, then g_update_available=1.
 * This matches the g_frame_ready pattern used in main.c. */
static volatile int g_update_available = 0;
static volatile int g_update_dismissed = 0;
static volatile int g_check_started = 0;
static volatile int g_check_done = 0;

static char g_new_version[VERSION_SIZE];
static char g_version_note[NOTE_SIZE];
static char g_download_uri[URI_SIZE];

static pthread_t g_check_thread;

/* ------------------------------------------------------------------ */
/* Minimal HTTP GET via POSIX sockets                                  */
/* Returns bytes in buf (body only), or -1 on failure.                */
/* ------------------------------------------------------------------ */
static int http_get(const char *host, int port, const char *path,
                    char *buf, int buf_size)
{
    struct hostent *he;
    struct sockaddr_in addr;
    struct timeval tv;
    int sock, sent, total, n;
    char request[512];
    char raw[HTTP_BUF_SIZE];
    int raw_len;
    char *body;

    he = gethostbyname(host);
    if (!he) return -1;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* 10-second receive timeout */
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    /* Send HTTP/1.0 GET (Connection: close — no chunked encoding) */
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    sent = 0;
    total = (int)strlen(request);
    while (sent < total) {
        n = send(sock, request + sent, total - sent, 0);
        if (n <= 0) { close(sock); return -1; }
        sent += n;
    }

    /* Receive full response */
    raw_len = 0;
    while (raw_len < (int)sizeof(raw) - 1) {
        n = recv(sock, raw + raw_len, sizeof(raw) - 1 - raw_len, 0);
        if (n <= 0) break;
        raw_len += n;
    }
    raw[raw_len] = '\0';
    close(sock);

    if (raw_len == 0) return -1;

    /* Find body after \r\n\r\n */
    body = strstr(raw, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    n = raw_len - (int)(body - raw);
    if (n <= 0) return -1;
    if (n >= buf_size) n = buf_size - 1;
    memcpy(buf, body, n);
    buf[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON string field extractor                                 */
/* Finds "key":"value" and copies value into out.                     */
/* Returns 1 on success, 0 on failure.                                */
/* ------------------------------------------------------------------ */
static int json_get_string(const char *json, const char *key,
                           char *out, int out_size)
{
    char pattern[64];
    const char *p, *start, *end;
    int len;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;

    /* Skip past key and find colon */
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    /* Expect opening quote */
    if (*p != '"') return 0;
    p++;

    /* Copy value, unescaping JSON sequences */
    len = 0;
    while (*p && *p != '"' && len < out_size - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n':  out[len++] = '\n'; break;
                case 'r':  break;  /* skip \r */
                case 't':  out[len++] = '\t'; break;
                case '"':  out[len++] = '"';  break;
                case '\\': out[len++] = '\\'; break;
                case '/':  out[len++] = '/';  break;
                default:   out[len++] = *p;   break;
            }
        } else {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Version comparison (major.minor.build)                              */
/* Returns 1 if remote is newer than local, 0 otherwise.              */
/* ------------------------------------------------------------------ */
static int parse_version(const char *str, int *major, int *minor, int *build)
{
    if (sscanf(str, "%d.%d.%d", major, minor, build) != 3)
        return 0;
    return 1;
}

static int is_version_newer(const char *local_ver, const char *remote_ver)
{
    int lmaj, lmin, lbld;
    int rmaj, rmin, rbld;

    if (!parse_version(local_ver, &lmaj, &lmin, &lbld)) return 0;
    if (!parse_version(remote_ver, &rmaj, &rmin, &rbld)) return 0;

    if (rmaj > lmaj) return 1;
    if (rmaj == lmaj && rmin > lmin) return 1;
    if (rmaj == lmaj && rmin == lmin && rbld > lbld) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Background thread function                                          */
/* ------------------------------------------------------------------ */
static void *update_check_thread(void *arg)
{
    char buf[HTTP_BUF_SIZE];
    char version[VERSION_SIZE];
    char note[NOTE_SIZE];
    char uri[URI_SIZE];
    int n;

    (void)arg;

    n = http_get(UPDATE_HOST, UPDATE_PORT, UPDATE_PATH, buf, sizeof(buf));
    if (n <= 0) {
        g_check_done = 1;
        return NULL;
    }

    /* Extract fields */
    if (!json_get_string(buf, "version", version, sizeof(version))) {
        g_check_done = 1;
        return NULL;
    }

    if (!is_version_newer(APP_VERSION, version)) {
        g_check_done = 1;
        return NULL;
    }

    /* Update found — get downloadURI from version check response */
    json_get_string(buf, "downloadURI", uri, sizeof(uri));

    /* Fetch full details for complete version notes */
    note[0] = '\0';
    n = http_get(UPDATE_HOST, UPDATE_PORT, DETAIL_PATH, buf, sizeof(buf));
    if (n > 0) {
        json_get_string(buf, "versionNote", note, sizeof(note));
    }
    /* Fall back to truncated note from version check if details fetch failed */
    if (!note[0]) {
        n = http_get(UPDATE_HOST, UPDATE_PORT, UPDATE_PATH, buf, sizeof(buf));
        if (n > 0)
            json_get_string(buf, "versionNote", note, sizeof(note));
    }

    strncpy(g_new_version, version, VERSION_SIZE - 1);
    g_new_version[VERSION_SIZE - 1] = '\0';
    strncpy(g_version_note, note, NOTE_SIZE - 1);
    g_version_note[NOTE_SIZE - 1] = '\0';
    strncpy(g_download_uri, uri, URI_SIZE - 1);
    g_download_uri[URI_SIZE - 1] = '\0';

    /* Memory barrier then flag — main thread sees strings before flag */
    __sync_synchronize();
    g_update_available = 1;
    g_check_done = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void updater_check_start(void)
{
    if (g_check_started) return;
    g_check_started = 1;
    g_update_available = 0;
    g_update_dismissed = 0;
    g_check_done = 0;

    pthread_create(&g_check_thread, NULL, update_check_thread, NULL);
}

int updater_has_update(void)
{
    return g_update_available && !g_update_dismissed;
}

const char *updater_get_version(void)
{
    return g_new_version;
}

const char *updater_get_note(void)
{
    return g_version_note;
}

void updater_install(void)
{
    typedef int (*ServiceCallFunc)(const char *, const char *);
    ServiceCallFunc fn;
    char params[768];

    if (!g_update_available || g_download_uri[0] == '\0') return;

    fn = (ServiceCallFunc)dlsym(RTLD_DEFAULT, "PDL_ServiceCall");
    if (!fn) return;

    snprintf(params, sizeof(params),
             "{\"id\":\"org.webosinternals.preware\","
             "\"params\":{\"type\":\"install\",\"file\":\"%s\"}}",
             g_download_uri);

    fn("palm://com.palm.applicationManager/open", params);
    g_update_dismissed = 1;

    /* Exit so the app doesn't linger in the background during install */
    exit(0);
}

void updater_dismiss(void)
{
    g_update_dismissed = 1;
}

void updater_shutdown(void)
{
    if (g_check_started) {
        pthread_join(g_check_thread, NULL);
        g_check_started = 0;
    }
}
