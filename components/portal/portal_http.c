/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file portal_http.c
 * @brief Captive-portal HTTP server.
 *
 * Listens on the SoftAP interface and serves a minimal HTML page that
 * collects the user's home Wi-Fi SSID and password.  Every request that
 * does not match /connect is redirected to the portal page (captive-portal
 * behavior).  On form submission the credentials are saved to the
 * known-network list and the portal shuts down.
 *
 * The HTML is embedded in flash (PROGMEM-style) so the portal works even
 * when no SD card is present — the exact first-run scenario.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "p4minishell_config.h"
#include "portal_internal.h"

/* The portal needs networking_wifi_known_upsert() to save credentials.
 * A direct include would create a circular dependency (networking ↔ portal),
 * so we declare the function via extern.  The linker resolves it. */
extern esp_err_t networking_wifi_known_upsert(const char *ssid, const char *password,
                                              int authmode, bool mark_connected);
#include "wifi_known.h"

#define PORTAL_HTTP_TAG "portal_http"

/* ---- Embedded portal HTML ----
 * Split in two so the handler can inject a scanned-SSID <datalist> between
 * them (empty when the scan fails: the form still works with manual typing). */

static const char portal_page_head[] =
"<!DOCTYPE html>"
"<html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>P4MiniShell Wi-Fi Setup</title>"
"<style>"
"body{font-family:system-ui,sans-serif;max-width:400px;margin:40px auto;padding:0 20px;"
"background:#f5f5f5;color:#333}"
"h1{font-size:1.4em;margin-bottom:8px}"
".card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
"label{display:block;margin-top:16px;font-weight:600;font-size:.9em}"
"input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid #ddd;"
"border-radius:6px;font-size:1em;box-sizing:border-box;margin-top:4px}"
"button{width:100%;margin-top:20px;padding:12px;background:#2563eb;color:#fff;"
"border:none;border-radius:6px;font-size:1em;font-weight:600;cursor:pointer}"
"button:hover{background:#1d4ed8}"
".note{font-size:.8em;color:#666;margin-top:16px;text-align:center}"
"</style></head><body>"
"<div class=\"card\">"
"<h1>P4MiniShell Wi-Fi Setup</h1>"
"<p>Connect this device to your home Wi-Fi network.</p>"
"<form method=\"POST\" action=\"/connect\">"
"<label>Network name (SSID)</label>"
"<input type=\"text\" name=\"ssid\" required placeholder=\"Your Wi-Fi name\" list=\"ssids\">"
"<datalist id=\"ssids\">";

static const char portal_page_tail[] =
"</datalist>"
"<label>Password</label>"
"<input type=\"password\" name=\"password\" placeholder=\"Wi-Fi password\">"
"<button type=\"submit\">Connect</button>"
"</form>"
"<p class=\"note\">Credentials are saved to the SD card for future boots.</p>"
"</div></body></html>";

/** Scanned SSIDs offered in the picker (bounded, strongest first). */
#define PORTAL_SCAN_MAX_SSIDS 16
/** SSID byte length (32) plus NUL. */
#define PORTAL_SCAN_SSID_BYTES 33
/** Cap for the generated <option> block. */
#define PORTAL_SCAN_HTML_BYTES 2048

/** HTML-escape @p in into @p out (out_size incl. NUL). Returns bytes used.
 *  Non-static so the unit tests can drive it directly. */
size_t portal_html_escape(const char *in, char *out, size_t out_size)
{
    size_t used = 0;

    if (in == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    while (*in != '\0' && used + 1 < out_size) {
        const char *entity = NULL;
        size_t entity_len = 0;

        switch (*in) {
        case '&': entity = "&amp;"; entity_len = 5; break;
        case '<': entity = "&lt;"; entity_len = 4; break;
        case '>': entity = "&gt;"; entity_len = 4; break;
        case '"': entity = "&quot;"; entity_len = 6; break;
        default: break;
        }
        if (entity != NULL) {
            if (used + entity_len >= out_size) {
                break;
            }
            memcpy(out + used, entity, entity_len);
            used += entity_len;
        } else {
            out[used++] = *in;
        }
        in++;
    }
    out[used] = '\0';
    return used;
}

/** Blocking STA scan returning deduplicated SSIDs (strongest first).
 *  Returns the count stored into @p ssids (each PORTAL_SCAN_SSID_BYTES).
 *  Empty on any failure: the page falls back to manual typing. */
static int portal_scan_ssids(char ssids[][PORTAL_SCAN_SSID_BYTES], int max)
{
    wifi_scan_config_t config;
    uint16_t ap_count = 0;
    wifi_ap_record_t *records = NULL;
    int stored = 0;
    int i;
    int j;

    if (max <= 0) {
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 200;
    if (esp_wifi_scan_start(&config, true) != ESP_OK) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return 0;
    }
    if (ap_count > 64) {
        ap_count = 64;
    }
    records = malloc(sizeof(*records) * ap_count);
    if (records == NULL) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        free(records);
        return 0;
    }
    /* Strongest-first, deduplicated, skipping hidden (empty) SSIDs. */
    for (i = 0; i < (int)ap_count && stored < max; i++) {
        int best = -1;

        for (j = 0; j < (int)ap_count; j++) {
            int k;
            bool seen = false;

            if (records[j].ssid[0] == '\0') {
                continue;
            }
            for (k = 0; k < stored; k++) {
                if (strcmp(ssids[k], (const char *)records[j].ssid) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen && (best < 0 || records[j].rssi > records[best].rssi)) {
                best = j;
            }
        }
        if (best < 0) {
            break;
        }
        snprintf(ssids[stored], PORTAL_SCAN_SSID_BYTES, "%s", records[best].ssid);
        stored++;
    }
    free(records);
    return stored;
}

static const char connect_ok[] =
"<!DOCTYPE html>"
"<html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Connected</title>"
"<style>body{font-family:system-ui,sans-serif;max-width:400px;margin:40px auto;padding:0 20px;"
"background:#f5f5f5;color:#333}"
".card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
"h1{font-size:1.4em;color:#16a34a}"
"</style></head><body>"
"<div class=\"card\">"
"<h1>Saved!</h1>"
"<p>The Wi-Fi credentials have been saved and the device is connecting now. "
"It will also connect automatically on future boots.</p>"
"</div></body></html>";

/* ---- Captive portal redirect handler ----
 * Every GET that is not / or /connect gets a 302 to / so the captive-portal
 * detection on phones/laptops fires. */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Portal page handler ---- */
static esp_err_t portal_page_handler(httpd_req_t *req)
{
    /* A fresh scan per page view keeps the picker current; on failure the
     * empty datalist degrades to the manual-typing form. Heap buffers: the
     * HTTP worker task stack is small (4 KiB). */
    char (*ssids)[PORTAL_SCAN_SSID_BYTES] = NULL;
    char *options = NULL;
    int count = 0;
    int i;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, portal_page_head, sizeof(portal_page_head) - 1);

    ssids = malloc(sizeof(*ssids) * PORTAL_SCAN_MAX_SSIDS);
    options = malloc(PORTAL_SCAN_HTML_BYTES);
    if (ssids != NULL && options != NULL) {
        size_t used = 0;

        count = portal_scan_ssids(ssids, PORTAL_SCAN_MAX_SSIDS);
        options[0] = '\0';
        for (i = 0; i < count; i++) {
            char esc[PORTAL_SCAN_SSID_BYTES * 6 + 1];
            int written;

            portal_html_escape(ssids[i], esc, sizeof(esc));
            written = snprintf(options + used, PORTAL_SCAN_HTML_BYTES - used,
                               "<option value=\"%s\">", esc);
            if (written < 0 || (size_t)written >= PORTAL_SCAN_HTML_BYTES - used) {
                break;
            }
            used += (size_t)written;
        }
        if (used > 0) {
            httpd_resp_send_chunk(req, options, used);
        }
    }
    free(ssids);
    free(options);

    httpd_resp_send_chunk(req, portal_page_tail, sizeof(portal_page_tail) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- Connect form handler ---- */
static esp_err_t portal_connect_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    /* Parse form fields: ssid=...&password=... */
    char ssid[P4_CONFIG_WIFI_SSID_BYTES] = {0};
    char password[P4_CONFIG_WIFI_PASSWORD_BYTES] = {0};

    {
        char *p;

        p = strstr(buf, "ssid=");
        if (p != NULL) {
            p += 5;
            /* URL-decode simple characters (+ -> space, %XX -> char). */
            {
                int j = 0;

                while (*p != '\0' && *p != '&' && j < (int)sizeof(ssid) - 1) {
                    if (*p == '+') {
                        ssid[j++] = ' ';
                    } else if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
                        char hex[3] = { p[1], p[2], '\0' };
                        ssid[j++] = (char)strtol(hex, NULL, 16);
                        p += 2;
                    } else {
                        ssid[j++] = *p;
                    }
                    p++;
                }
                ssid[j] = '\0';
            }
        }

        p = strstr(buf, "password=");
        if (p != NULL) {
            p += 9;
            {
                int j = 0;

                while (*p != '\0' && *p != '&' && j < (int)sizeof(password) - 1) {
                    if (*p == '+') {
                        password[j++] = ' ';
                    } else if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
                        char hex[3] = { p[1], p[2], '\0' };
                        password[j++] = (char)strtol(hex, NULL, 16);
                        p += 2;
                    } else {
                        password[j++] = *p;
                    }
                    p++;
                }
                password[j] = '\0';
            }
        }
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(PORTAL_HTTP_TAG, "credentials received: SSID=%s", ssid);

    /* Save to the known-network list (requires SD). */
    if (networking_wifi_known_upsert(ssid, password, -1, false) == ESP_OK) {
        ESP_LOGI(PORTAL_HTTP_TAG, "saved to known-network list");
    } else {
        ESP_LOGW(PORTAL_HTTP_TAG, "failed to save to known-network list");
    }

    /* Show the success page and schedule portal shutdown. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, connect_ok, sizeof(connect_ok) - 1);

    /* Schedule portal stop from the httpd task context (deferred so the
     * response is fully sent before tearing down the server), plus a STA
     * join right after the teardown so the user does not wait a reboot. */
    {
        extern void portal_request_stop(void);
        extern void portal_request_connect_after_stop(const char *ssid,
                                                      const char *password);
        portal_request_connect_after_stop(ssid, password);
        portal_request_stop();
    }

    return ESP_OK;
}

/* ---- Server lifecycle ---- */

/** 404 error handler: redirect every unmatched request to the portal page. */
static esp_err_t portal_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t s_portal_server;

void portal_http_start(void)
{
    if (s_portal_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = P4_CONFIG_PORTAL_HTTP_PORT;
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.lru_purge_enable = true;

    if (httpd_start(&s_portal_server, &config) != ESP_OK) {
        ESP_LOGE(PORTAL_HTTP_TAG, "failed to start portal HTTP server");
        return;
    }

    /* POST /connect — form submission. */
    httpd_uri_t connect_uri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = portal_connect_handler,
    };
    httpd_register_uri_handler(s_portal_server, &connect_uri);

    /* GET / — portal page. */
    httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_page_handler,
    };
    httpd_register_uri_handler(s_portal_server, &page_uri);

    /* GET /generate_204 — Android captive-portal probe. */
    httpd_uri_t probe_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_register_uri_handler(s_portal_server, &probe_204);

    /* GET /hotspot-detect.html — Apple captive-portal probe. */
    httpd_uri_t probe_apple = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_register_uri_handler(s_portal_server, &probe_apple);

    /* GET /ncsi.txt — Windows captive-portal probe. */
    httpd_uri_t probe_win = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_register_uri_handler(s_portal_server, &probe_win);

    /* GET any other path - catch-all redirect (registered last, acts as
     * default handler for all unmatched URIs). */
    httpd_uri_t catchall = {
        .uri = "*",
        .method = HTTP_GET,
        .handler = portal_redirect_handler,
    };
    httpd_register_uri_handler(s_portal_server, &catchall);

    /* Catch-all redirect for unmatched URIs (captive portal behavior). */
    httpd_register_err_handler(s_portal_server, HTTPD_404_NOT_FOUND, portal_404_handler);

    ESP_LOGI(PORTAL_HTTP_TAG, "captive portal HTTP server started on port %d",
             P4_CONFIG_PORTAL_HTTP_PORT);
}

void portal_http_stop(void)
{
    if (s_portal_server == NULL) {
        return;
    }
    httpd_stop(s_portal_server);
    s_portal_server = NULL;
    ESP_LOGI(PORTAL_HTTP_TAG, "captive portal HTTP server stopped");
}
