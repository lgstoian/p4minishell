/**
 * @file http_server.c
 * @brief Lightweight read-only HTTP file server (esp_http_server).
 *
 * Serves files and directory listings from the SD card over the Wi-Fi link.
 * The design mirrors c6ota's SD patterns (VFS fopen/fread chunk streaming,
 * guarded SD sessions) and networking.c's output helpers. Every request runs
 * on the dedicated httpd task and is fully bounded:
 *   - auth check (optional Basic auth, constant-ish compare)
 *   - path traversal rejection (`..` segments are refused)
 *   - directory listings capped at P4_CONFIG_HTTPD_LISTING_MAX entries
 *   - file streaming through a heap read buffer with send timeouts
 *
 * Resource limits come from the httpd_config_t overrides in
 * networking_httpd_start(); all are tunable in p4minishell_config.h.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "ansi.h"
#include "ansi_palette.h"
#include "bsp/esp-bsp.h"
#include "http_server.h"
#include "led.h"
#include "networking.h"
#include "p4minishell_config.h"
#include "storage.h"

/* ---- Backward-compatibility aliases ---- */
#define HTTPD_PORT              P4_CONFIG_HTTPD_PORT
#define HTTPD_MAX_OPEN_SOCKETS  P4_CONFIG_HTTPD_MAX_OPEN_SOCKETS
#define HTTPD_BACKLOG           P4_CONFIG_HTTPD_BACKLOG
#define HTTPD_STACK_BYTES       P4_CONFIG_HTTPD_STACK_BYTES
#define HTTPD_TASK_PRIORITY     P4_CONFIG_HTTPD_TASK_PRIORITY
#define HTTPD_RECV_TIMEOUT_S    P4_CONFIG_HTTPD_RECV_TIMEOUT_S
#define HTTPD_SEND_TIMEOUT_S    P4_CONFIG_HTTPD_SEND_TIMEOUT_S
#define HTTPD_BLOCK_BYTES       P4_CONFIG_HTTPD_BLOCK_BYTES
#define HTTPD_LISTING_MAX       P4_CONFIG_HTTPD_LISTING_MAX
#define HTTPD_AUTOSTART         P4_CONFIG_HTTPD_AUTOSTART
#define HTTPD_AUTH_USERNAME     P4_CONFIG_HTTPD_AUTH_USERNAME
#define HTTPD_AUTH_PASSWORD     P4_CONFIG_HTTPD_AUTH_PASSWORD
#define NETDIAG_TAG             P4_CONFIG_SHELL_TAG

static httpd_handle_t s_server;
static networking_httpd_state_t s_state;
static esp_err_t s_last_error;
static uint32_t s_request_count;
/* Cooldown for failed auto-starts: a failed `httpd_start()` (e.g. task
 * creation returning ESP_ERR_HTTPD_TASK while the heap is tight) must not
 * retry on every Wi-Fi event. Each attempt prints transcript lines and grows
 * spans, which is exactly the wrong thing to do under memory pressure. */
static int64_t s_autostart_cooldown_until_us;

/* ---- Transcript output through the networking host ops ---- */

static const networking_host_ops_t *httpd_ops(void)
{
    return networking_get_host_ops();
}

static void httpd_appendf(const char *format, ...)
{
    const networking_host_ops_t *ops = httpd_ops();
    char buffer[512];
    va_list args;

    if (ops == NULL || ops->transcript_append_ansi == NULL) {
        return;
    }
    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);
    ops->transcript_append_ansi(buffer);
}

static void httpd_record_warningf(const char *format, ...)
{
    const networking_host_ops_t *ops = httpd_ops();
    char buffer[256];
    va_list args;

    if (ops == NULL || ops->record_warning == NULL) {
        return;
    }
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    ops->record_warning(NETDIAG_TAG, buffer);
}

/* ---- Auth ---- */

/** True when Basic auth is enabled by configuration. */
static bool httpd_auth_enabled(void)
{
    return HTTPD_AUTH_USERNAME[0] != '\0';
}

/** Constant-time compare of two fixed-size strings. */
static bool httpd_secure_equals(const char *left, const char *right)
{
    size_t i;
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);

    if (left_len != right_len) {
        return false;
    }

    unsigned char diff = 0;
    for (i = 0; i < left_len; i++) {
        diff |= (unsigned char)left[i] ^ (unsigned char)right[i];
    }
    return diff == 0;
}

/**
 * Validate the `Authorization: Basic ...` header. The expected credentials
 * are decoded from the request and compared constant-time against the
 * configured user:password pair.
 */
static bool httpd_check_auth(const char *authorization)
{
    const char *encoded;
    unsigned char decoded[96];
    size_t decoded_len = 0;
    char expected[96];
    int written;

    if (authorization == NULL || strncasecmp(authorization, "Basic ", 6) != 0) {
        return false;
    }
    encoded = authorization + 6;
    while (*encoded == ' ') {
        encoded++;
    }

    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)encoded, strlen(encoded)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    written = snprintf(expected, sizeof(expected), "%s:%s",
                       HTTPD_AUTH_USERNAME, HTTPD_AUTH_PASSWORD);
    if (written < 0 || (size_t)written >= sizeof(expected)) {
        return false;
    }

    return httpd_secure_equals((const char *)decoded, expected);
}

/* ---- URL decoding and path normalization ---- */

/** Hex nibble value, or -1 for a non-hex character. */
static int httpd_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * Decode and normalize a request URI into a path under the SD root.
 *
 * Strips the query/fragment, percent-decodes into @p out, and refuses any
 * `..` segment (path traversal). On success @p out holds the decoded path
 * ("/", "/dir", "/dir/file") and true is returned.
 */
static bool httpd_normalize_uri(const char *uri, char *out, size_t out_size)
{
    size_t i;
    size_t o = 0;
    size_t seg_len = 0;   /* characters in the current path segment */

    if (uri == NULL || out == NULL || out_size == 0) {
        return false;
    }

    for (i = 0; uri[i] != '\0' && uri[i] != '?' && uri[i] != '#'; i++) {
        char c = uri[i];

        if (c == '%') {
            int hi;
            int lo;
            if (uri[i + 1] == '\0' || uri[i + 2] == '\0') {
                return false;               /* truncated escape */
            }
            hi = httpd_hex_value(uri[i + 1]);
            lo = httpd_hex_value(uri[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;               /* bad escape */
            }
            c = (char)((hi << 4) | lo);
            i += 2;
        }

        if (o + 1 >= out_size) {
            return false;
        }

        if (c == '/') {
            if (o > 0 && out[o - 1] == '/') {
                continue;                   /* collapse duplicate slashes */
            }
            seg_len = 0;
        } else {
            if (seg_len == 1 && out[o - 1] == '.' &&
                (o == 1 || out[o - 2] == '/')) {
                if (c == '.') {
                    return false;           /* ".." segment refused */
                }
                /* A single "." segment is dropped. */
                if (c == '/') {
                    out[o - 1] = '/';
                    seg_len = 0;
                    continue;
                }
            }
            seg_len++;
        }
        out[o++] = c;
    }

    if (o == 0) {
        /* Empty URI: treat as the document root. */
        if (out_size < 2) {
            return false;
        }
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    out[o] = '\0';
    return true;
}

/** Map a file extension to a Content-Type string. */
static const char *httpd_content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
        return "text/html";
    }
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0 ||
        strcasecmp(dot, ".bat") == 0 || strcasecmp(dot, ".sys") == 0 ||
        strcasecmp(dot, ".ini") == 0 || strcasecmp(dot, ".md") == 0) {
        return "text/plain";
    }
    if (strcasecmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(dot, ".js") == 0) {
        return "application/javascript";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".bmp") == 0) {
        return "image/bmp";
    }
    return "application/octet-stream";
}

/** Escape an HTML-unsafe string into a bounded output buffer. */
static void httpd_html_escape(const char *input, char *out, size_t out_size)
{
    static const struct {
        const char *raw;
        const char *escaped;
    } table[] = {
        { "&", "&amp;" }, { "<", "&lt;" }, { ">", "&gt;" },
        { "\"", "&quot;" }, { "'", "&#39;" },
    };
    size_t i;
    size_t o = 0;
    size_t t;

    if (out_size == 0) {
        return;
    }
    for (i = 0; input[i] != '\0' && o + 1 < out_size; i++) {
        for (t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
            if (input[i] == table[t].raw[0]) {
                size_t len = strlen(table[t].escaped);
                if (o + len + 1 > out_size) {
                    out[o] = '\0';
                    return;
                }
                memcpy(out + o, table[t].escaped, len);
                o += len;
                goto next;
            }
        }
        out[o++] = input[i];
    next:
        ;
    }
    out[o] = '\0';
}

/** Send a chunk of formatted text (bounded by a stack scratch buffer). */
static esp_err_t httpd_send_chunkf(httpd_req_t *req, const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return httpd_resp_send_chunk(req, buffer, HTTPD_RESP_USE_STRLEN);
}

/** Send a plain-text error response with a matching status line. */
static void httpd_send_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

/** Build an HTML directory listing bounded by P4_CONFIG_HTTPD_LISTING_MAX. */
static esp_err_t httpd_serve_directory(httpd_req_t *req, const char *full_path,
                                       const char *url_path)
{
    DIR *dir;
    struct dirent *entry;
    char escaped[160];
    char size_buf[32];
    char href[CONFIG_HTTPD_MAX_URI_LEN + P4_CONFIG_LFN_BYTES + 2];
    char full_entry[CONFIG_HTTPD_MAX_URI_LEN + 64 + P4_CONFIG_LFN_BYTES + 2];
    size_t count = 0;
    esp_err_t error = ESP_OK;

    dir = opendir(full_path);
    if (dir == NULL) {
        httpd_send_error(req, "404 Not Found", "Directory not found\n");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    if (httpd_resp_send_chunk(req,
                              "<!DOCTYPE html><html><head><title>P4MiniShell SD share</title>"
                              "<meta charset=\"utf-8\"></head><body>"
                              "<h1>P4MiniShell SD share</h1><pre>",
                              HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        closedir(dir);
        return ESP_FAIL;
    }

    /* A link back to the root when not already at the root. */
    if (strcmp(url_path, "/") != 0) {
        httpd_resp_send_chunk(req, "<a href=\"/\">/</a><br>\n", HTTPD_RESP_USE_STRLEN);
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        bool is_dir = false;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count >= HTTPD_LISTING_MAX) {
            httpd_send_chunkf(req, "&hellip; (listing truncated at %d entries)\n",
                              (int)HTTPD_LISTING_MAX);
            break;
        }

        snprintf(full_entry, sizeof(full_entry), "%s/%s", full_path, entry->d_name);
        if (stat(full_entry, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                is_dir = true;
                snprintf(size_buf, sizeof(size_buf), "&lt;DIR&gt;");
            } else {
                shell_sd_format_size((uint64_t)st.st_size, size_buf, sizeof(size_buf));
            }
        } else {
            is_dir = false;
            snprintf(size_buf, sizeof(size_buf), "-");
        }

        httpd_html_escape(entry->d_name, escaped, sizeof(escaped));
        if (strcmp(url_path, "/") == 0) {
            snprintf(href, sizeof(href), "/%s", escaped);
        } else {
            snprintf(href, sizeof(href), "%s/%s", url_path, escaped);
        }

        if (httpd_send_chunkf(req, "<a href=\"%s\">%s</a>%s %s\n",
                              href, escaped, is_dir ? "/" : "", size_buf) != ESP_OK) {
            error = ESP_FAIL;
            break;
        }
        count++;
    }
    closedir(dir);

    if (error == ESP_OK) {
        httpd_resp_send_chunk(req, "</pre></body></html>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return error;
}

/** Stream a file with a heap read buffer and a Content-Type by extension. */
static esp_err_t httpd_serve_file(httpd_req_t *req, const char *full_path)
{
    FILE *file;
    uint8_t *buffer;
    size_t bytes_read;
    esp_err_t error = ESP_OK;

    file = fopen(full_path, "rb");
    if (file == NULL) {
        httpd_send_error(req, "404 Not Found", "File not found\n");
        return ESP_OK;
    }

    buffer = heap_caps_malloc(HTTPD_BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(HTTPD_BLOCK_BYTES, MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        fclose(file);
        httpd_send_error(req, "500 Internal Server Error", "Out of memory\n");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, httpd_content_type_for(full_path));

    while ((bytes_read = fread(buffer, 1, HTTPD_BLOCK_BYTES, file)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buffer, (ssize_t)bytes_read) != ESP_OK) {
            error = ESP_FAIL;
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(buffer);
    fclose(file);
    return error;
}

/** Root URI handler: auth gate, then directory listing or file streaming. */
static esp_err_t httpd_root_handler(httpd_req_t *req)
{
    shell_sd_session_t session;
    struct stat st;
    char url_path[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char full_path[CONFIG_HTTPD_MAX_URI_LEN + 64];
    char auth[128];
    bool is_dir;

    s_request_count++;

    if (httpd_auth_enabled()) {
        if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
            !httpd_check_auth(auth)) {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"P4MiniShell\"");
            httpd_resp_send(req, "401 Unauthorized\n", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    if (!httpd_normalize_uri(req->uri, url_path, sizeof(url_path))) {
        httpd_send_error(req, "400 Bad Request", "Invalid path\n");
        return ESP_OK;
    }

    if (strcmp(url_path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", BSP_SD_MOUNT_POINT);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s", BSP_SD_MOUNT_POINT, url_path);
    }

    /* Guard the SD session for the whole request (mount check + card present). */
    if (shell_sd_begin(&session) != ESP_OK) {
        httpd_send_error(req, "503 Service Unavailable", "SD card unavailable\n");
        return ESP_OK;
    }

    if (stat(full_path, &st) != 0) {
        shell_sd_end(&session, "httpd");
        httpd_send_error(req, "404 Not Found", "Path not found\n");
        return ESP_OK;
    }
    is_dir = S_ISDIR(st.st_mode);
    shell_sd_end(&session, "httpd");

    if (is_dir) {
        return httpd_serve_directory(req, full_path, url_path);
    }
    return httpd_serve_file(req, full_path);
}

/* ---- Session tracking (open socket count for `httpd status`) ---- */

static uint16_t s_open_sockets;

static esp_err_t httpd_session_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
    if (s_open_sockets < 0xFFFF) {
        s_open_sockets++;
    }
    return ESP_OK;
}

static void httpd_session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
    if (s_open_sockets > 0) {
        s_open_sockets--;
    }
}

/* ---- Lifecycle ---- */

esp_err_t networking_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = httpd_root_handler,
        .user_ctx = NULL,
    };
    esp_err_t error;

    if (s_server != NULL) {
        return ESP_OK;
    }

    if (!networking_wifi_is_connected()) {
        httpd_appendf(SH_LBL "httpd:" SH_RST " " SH_ERR "not started - Wi-Fi is not connected" SH_RST "\n");
        httpd_record_warningf("httpd start rejected because Wi-Fi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    config.server_port = HTTPD_PORT;
    config.max_open_sockets = HTTPD_MAX_OPEN_SOCKETS;
    config.backlog_conn = HTTPD_BACKLOG;
    config.stack_size = HTTPD_STACK_BYTES;
    config.task_priority = HTTPD_TASK_PRIORITY;
    config.recv_wait_timeout = HTTPD_RECV_TIMEOUT_S;
    config.send_wait_timeout = HTTPD_SEND_TIMEOUT_S;
    config.lru_purge_enable = true;
    config.open_fn = httpd_session_open;
    config.close_fn = httpd_session_close;
    config.uri_match_fn = httpd_uri_match_wildcard;
    s_open_sockets = 0;

    s_state = NETWORKING_HTTPD_STATE_STARTING;
    s_last_error = ESP_OK;

    error = httpd_start(&s_server, &config);
    if (error != ESP_OK) {
        s_server = NULL;
        s_state = NETWORKING_HTTPD_STATE_FAILED;
        s_last_error = error;
        httpd_appendf(SH_LBL "httpd:" SH_RST " " SH_ERR "start failed" SH_RST " (" SH_WARN "%s" SH_RST ")\n",
                      esp_err_to_name(error));
        httpd_record_warningf("httpd start failed: %s", esp_err_to_name(error));
        return error;
    }

    error = httpd_register_uri_handler(s_server, &uri);
    if (error != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        s_state = NETWORKING_HTTPD_STATE_FAILED;
        s_last_error = error;
        httpd_appendf(SH_LBL "httpd:" SH_RST " " SH_ERR "handler registration failed" SH_RST " (%s)\n",
                      esp_err_to_name(error));
        httpd_record_warningf("httpd handler registration failed: %s", esp_err_to_name(error));
        return error;
    }

    s_state = NETWORKING_HTTPD_STATE_RUNNING;
    s_last_error = ESP_OK;
    led_notify(LED_EVENT_HTTPD_STARTED);
    httpd_appendf(SH_LBL "httpd:" SH_RST " listening on port " SH_NUM "%u" SH_RST
                  " (auth %s, docroot " SH_PATH "%s" SH_RST ")\n",
                  (unsigned int)HTTPD_PORT,
                  httpd_auth_enabled() ? "on" : "off",
                  BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t networking_httpd_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    httpd_stop(s_server);
    s_server = NULL;
    s_state = NETWORKING_HTTPD_STATE_STOPPED;
    s_last_error = ESP_OK;
    led_notify(LED_EVENT_HTTPD_STOPPED);
    httpd_appendf(SH_LBL "httpd:" SH_RST " server stopped\n");
    return ESP_OK;
}

void networking_httpd_status(void)
{
    networking_httpd_status_t status;

    status.state = s_state;
    status.auth_enabled = httpd_auth_enabled();
    status.sd_mounted = storage_sd_is_mounted();
    status.port = HTTPD_PORT;
    status.max_sockets = HTTPD_MAX_OPEN_SOCKETS;
    status.request_count = s_request_count;
    status.last_error = s_last_error;
    status.open_sockets = (s_server != NULL) ? s_open_sockets : 0;

    httpd_appendf(SH_HEAD "HTTP Server" SH_RST "\n");
    httpd_appendf("  " SH_LBL "state:" SH_RST " %s\n",
                  status.state == NETWORKING_HTTPD_STATE_RUNNING ? "running" :
                  status.state == NETWORKING_HTTPD_STATE_STARTING ? "starting" :
                  status.state == NETWORKING_HTTPD_STATE_FAILED ? "failed" : "stopped");
    httpd_appendf("  " SH_LBL "port:" SH_RST " " SH_NUM "%u" SH_RST "\n", (unsigned int)status.port);
    httpd_appendf("  " SH_LBL "auth:" SH_RST " %s\n", status.auth_enabled ? "on" : "off");
    httpd_appendf("  " SH_LBL "docroot:" SH_RST " " SH_PATH "%s" SH_RST "\n", BSP_SD_MOUNT_POINT);
    httpd_appendf("  " SH_LBL "sockets:" SH_RST " " SH_NUM "%u/%u" SH_RST "\n",
                  (unsigned int)status.open_sockets, (unsigned int)status.max_sockets);
    httpd_appendf("  " SH_LBL "requests:" SH_RST " " SH_NUM "%u" SH_RST "\n",
                  (unsigned int)status.request_count);
    httpd_appendf("  " SH_LBL "sd:" SH_RST " %s\n", status.sd_mounted ? "mounted" : "not mounted");
    if (status.state == NETWORKING_HTTPD_STATE_FAILED && status.last_error != ESP_OK) {
        httpd_appendf("  " SH_LBL "last error:" SH_RST " " SH_ERR "%s" SH_RST "\n",
                      esp_err_to_name(status.last_error));
    }
}

bool networking_httpd_is_running(void)
{
    return s_server != NULL;
}

void networking_httpd_maybe_autostart(void)
{
#if HTTPD_AUTOSTART
    if (s_server == NULL && networking_wifi_is_connected()) {
        /* Respect the post-failure cooldown so a tight heap (the usual cause
         * of ESP_ERR_HTTPD_TASK from httpd_start's task creation) is not
         * hammered with a retry — and a transcript error line — on every
         * Wi-Fi event. Manual `httpd start` bypasses the cooldown. */
        if (esp_timer_get_time() < s_autostart_cooldown_until_us) {
            return;
        }
        esp_err_t error = networking_httpd_start();

        if (error != ESP_OK) {
            s_autostart_cooldown_until_us =
                esp_timer_get_time() + (int64_t)P4_CONFIG_HTTPD_AUTOSTART_RETRY_SECS * 1000000LL;
            httpd_record_warningf("HTTP server auto-start failed: %s", esp_err_to_name(error));
        }
    }
#else
    /* Auto-start disabled by configuration. */
#endif
}

void networking_httpd_maybe_stop(void)
{
    if (s_server != NULL) {
        networking_httpd_stop();
        httpd_record_warningf("HTTP server stopped on Wi-Fi disconnect");
    }
}
