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
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "p4minishell_config.h"
#include "portal_internal.h"

/* The portal needs networking_wifi_known_upsert() to save credentials.
 * A direct include would create a circular dependency (networking ↔ portal),
 * so we declare the function via extern.  The linker resolves it. */
extern esp_err_t networking_wifi_known_upsert(const char *ssid, const char *password,
                                              int authmode, bool mark_connected);
#include "wifi_known.h"

#define PORTAL_HTTP_TAG "portal_http"

/* ---- Embedded portal HTML ---- */

static const char portal_page[] =
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
"<input type=\"text\" name=\"ssid\" required placeholder=\"Your Wi-Fi name\">"
"<label>Password</label>"
"<input type=\"password\" name=\"password\" placeholder=\"Wi-Fi password\">"
"<button type=\"submit\">Connect</button>"
"</form>"
"<p class=\"note\">Credentials are saved to the SD card for future boots.</p>"
"</div></body></html>";

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
"<p>The Wi-Fi credentials have been saved. The device will connect "
"automatically on the next boot.</p>"
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, portal_page, sizeof(portal_page) - 1);
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
     * response is fully sent before tearing down the server). */
    {
        extern void portal_request_stop(void);
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
