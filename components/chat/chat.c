/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file chat.c
 * @brief Chat transport implementation (leaf module, portal-style).
 *
 * Mirrors the captive portal's APSTA + HTTP server pattern:
 *  - APSTA mode FIRST (esp_wifi_set_mode before set_config)
 *  - DHCP server auto-starts via esp_netif_create_default_wifi_ap()
 *  - TX power cap applied to avoid USB brownout (verified on hardware)
 *  - Deferred-stop pattern: HTTP handler sets flag, event loop task calls chat_stop()
 *  - All esp_netif/esp_wifi/esp_http_server calls live in this leaf; wifi_known
 *    calls are via extern to avoid circular deps with networking/.
 */

#include "chat.h"
#include "p4minishell_config.h"
#include "strutil.h"
#include "networking.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *CHAT_TAG = "chat";

/* --- Message ring buffer --- */
typedef struct {
    uint32_t seq;
    char from_ip[16];  /* IPv4 dotted decimal + null */
    char text[P4_CONFIG_CHAT_MSG_MAX_BYTES];
} chat_msg_t;

struct chat_instance_s {
    chat_config_t config;
    esp_netif_t *ap_netif;
    httpd_handle_t httpd;
    int8_t saved_tx_power;
    bool active;
    bool stop_requested;

    /* Message storage */
    chat_msg_t *inbox;      /* ring: latest P4_CONFIG_CHAT_INBOX_MAX per peer? For simplicity: global ring */
    chat_msg_t *log;        /* ring: latest P4_CONFIG_CHAT_LOG_MAX */
    size_t inbox_cap;
    size_t log_cap;
    size_t inbox_head;
    size_t log_head;
    size_t inbox_count;
    size_t log_count;
    uint32_t next_seq;

    /* Thread safety */
    SemaphoreHandle_t mutex;

    /* Callback */
    chat_onmsg_cb_t onmsg_cb;
};

/* --- Forward declarations --- */
static esp_err_t chat_httpd_start(chat_handle_t h);
static void chat_httpd_stop(chat_handle_t h);
static esp_err_t chat_http_post_handler(httpd_req_t *req);
static esp_err_t chat_http_get_handler(httpd_req_t *req);
static void chat_msg_add(chat_handle_t h, const char *from_ip, const char *text);

void chat_config_init_default(chat_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->ap_ssid = P4_CONFIG_CHAT_AP_SSID;
    config->ap_channel = P4_CONFIG_CHAT_AP_CHANNEL;
    config->ap_max_conn = P4_CONFIG_CHAT_AP_MAX_CONN;
    config->ap_tx_power_qdbm = P4_CONFIG_CHAT_AP_TX_POWER_QDBM;
    config->http_port = P4_CONFIG_CHAT_HTTP_PORT;
    config->msg_max_bytes = P4_CONFIG_CHAT_MSG_MAX_BYTES;
    config->inbox_max = P4_CONFIG_CHAT_INBOX_MAX;
    config->log_max = P4_CONFIG_CHAT_LOG_MAX;
    config->timeout_secs = P4_CONFIG_CHAT_TIMEOUT_SECS;
}

esp_err_t chat_start(const chat_config_t *config, chat_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_ssid == NULL || config->ap_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_channel < 1 || config->ap_channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_max_conn < 1 || config->ap_max_conn > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->msg_max_bytes < 64 || config->msg_max_bytes > 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->inbox_max < 8 || config->inbox_max > 256) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->log_max < 32 || config->log_max > 1024) {
        return ESP_ERR_INVALID_ARG;
    }

    chat_handle_t h = calloc(1, sizeof(struct chat_instance_s));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    h->config = *config;
    h->inbox_cap = config->inbox_max;
    h->log_cap = config->log_max;
    h->inbox = calloc(config->inbox_max, sizeof(chat_msg_t));
    h->log = calloc(config->log_max, sizeof(chat_msg_t));
    if (h->inbox == NULL || h->log == NULL) {
        free(h->inbox);
        free(h->log);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    h->mutex = xSemaphoreCreateMutex();
    if (h->mutex == NULL) {
        free(h->inbox);
        free(h->log);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    /* Create the AP netif. The DHCP server starts automatically. */
    h->ap_netif = esp_netif_create_default_wifi_ap();
    if (h->ap_netif == NULL) {
        ESP_LOGE(CHAT_TAG, "failed to create AP netif");
        vSemaphoreDelete(h->mutex);
        free(h->inbox);
        free(h->log);
        free(h);
        return ESP_FAIL;
    }

    /* Switch to APSTA mode FIRST: esp_wifi_set_config(AP) refuses with
     * ESP_ERR_WIFI_MODE while the stack runs STA-only. The STA config is
     * preserved across the mode switch, so a connected station survives. */
    {
        esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (error != ESP_OK) {
            ESP_LOGE(CHAT_TAG, "wifi_set_mode(APSTA): %s", esp_err_to_name(error));
            esp_netif_destroy_default_wifi(h->ap_netif);
            vSemaphoreDelete(h->mutex);
            free(h->inbox);
            free(h->log);
            free(h);
            return error;
        }
    }

    /* Configure the AP. */
    {
        wifi_config_t ap_config = {
            .ap = {
                .max_connection = config->ap_max_conn,
                .channel = config->ap_channel,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        strlcpy((char *)ap_config.ap.ssid, config->ap_ssid,
                sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(config->ap_ssid);

        esp_err_t error = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (error != ESP_OK) {
            ESP_LOGE(CHAT_TAG, "wifi_set_config(AP): %s", esp_err_to_name(error));
            esp_netif_destroy_default_wifi(h->ap_netif);
            vSemaphoreDelete(h->mutex);
            free(h->inbox);
            free(h->log);
            free(h);
            return error;
        }
    }

    /* Cap the RF TX power while the AP runs: AP beacons are a continuous
     * current draw and brownout-reboot USB-powered boards at full power
     * (verified on hardware). Restored on stop. Best-effort on hosted
     * stacks that do not implement the RPC. */
    {
        int8_t current = 0;
        if (esp_wifi_get_max_tx_power(&current) == ESP_OK) {
            h->saved_tx_power = current;
        }
        if (esp_wifi_set_max_tx_power(config->ap_tx_power_qdbm) != ESP_OK) {
            ESP_LOGW(CHAT_TAG, "TX power cap not applied; AP runs at full power");
        }
    }

    /* Start the HTTP server on the AP netif. */
    esp_err_t error = chat_httpd_start(h);
    if (error != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "httpd start failed: %s", esp_err_to_name(error));
        esp_wifi_set_max_tx_power(h->saved_tx_power);
        esp_netif_destroy_default_wifi(h->ap_netif);
        vSemaphoreDelete(h->mutex);
        free(h->inbox);
        free(h->log);
        free(h);
        return error;
    }

    h->active = true;
    h->stop_requested = false;
    *out_handle = h;
    ESP_LOGI(CHAT_TAG, "chat host started on SSID %s", config->ap_ssid);
    return ESP_OK;
}

esp_err_t chat_stop(chat_handle_t h)
{
    if (h == NULL || !h->active) {
        return ESP_ERR_INVALID_STATE;
    }

    h->active = false;

    /* Stop HTTP server first. */
    chat_httpd_stop(h);

    /* Restore TX power. */
    if (esp_wifi_set_max_tx_power(h->saved_tx_power) != ESP_OK) {
        ESP_LOGW(CHAT_TAG, "TX power restore failed (non-fatal)");
    }

    /* Destroy AP netif (also stops DHCP server). */
    if (h->ap_netif != NULL) {
        esp_netif_destroy_default_wifi(h->ap_netif);
        h->ap_netif = NULL;
    }

    /* Return to STA-only mode if station is connected or configured. */
    /* The portal does this implicitly by just destroying the AP netif;
     * the Wi-Fi stack reverts to the previous mode. We mirror that. */

    /* Free resources. */
    if (h->mutex != NULL) {
        vSemaphoreDelete(h->mutex);
        h->mutex = NULL;
    }
    free(h->inbox);
    free(h->log);
    h->inbox = NULL;
    h->log = NULL;

    ESP_LOGI(CHAT_TAG, "chat host stopped");
    return ESP_OK;
}

bool chat_is_active(chat_handle_t h)
{
    return (h != NULL && h->active);
}

void chat_set_onmsg_cb(chat_handle_t h, chat_onmsg_cb_t cb)
{
    if (h == NULL) return;
    h->onmsg_cb = cb;
}

uint32_t chat_get_seq(chat_handle_t h)
{
    if (h == NULL) return 0;
    return h->next_seq;
}

/* --- Message storage --- */
static void chat_msg_add(chat_handle_t h, const char *from_ip, const char *text)
{
    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    uint32_t seq = h->next_seq++;
    chat_msg_t *msg = &h->log[h->log_head];
    msg->seq = seq;
    strlcpy(msg->from_ip, from_ip, sizeof(msg->from_ip));
    strlcpy(msg->text, text, sizeof(msg->text));

    h->log_head = (h->log_head + 1) % h->log_cap;
    if (h->log_count < h->log_cap) {
        h->log_count++;
    }

    /* Also add to inbox (global inbox for simplicity). */
    msg = &h->inbox[h->inbox_head];
    msg->seq = seq;
    strlcpy(msg->from_ip, from_ip, sizeof(msg->from_ip));
    strlcpy(msg->text, text, sizeof(msg->text));
    h->inbox_head = (h->inbox_head + 1) % h->inbox_cap;
    if (h->inbox_count < h->inbox_cap) {
        h->inbox_count++;
    }

    xSemaphoreGive(h->mutex);

    /* Invoke onmsg callback if registered. */
    if (h->onmsg_cb != NULL) {
        h->onmsg_cb(from_ip, text, seq);
    }
}

esp_err_t chat_send(chat_handle_t h, const char *peer_ip, const char *text)
{
    if (h == NULL || !h->active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || strlen(text) > h->config.msg_max_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (peer_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* networking_http_request() is the firmware's ONE POST/WebDAV entry point
     * (components/networking owns every esp_http_client call). The previous
     * code declared a local `extern` with a different signature, which both
     * failed to compile and shadowed the real API. */
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%u/chat", peer_ip, h->config.http_port);

    networking_http_result_t result;
    esp_err_t err = networking_http_request(HTTP_METHOD_POST, url, "text/plain",
                                            (const uint8_t *)text, strlen(text),
                                            NULL, NULL, NULL, &result);
    networking_http_result_free(&result);
    return err;
}

esp_err_t chat_poll(chat_handle_t h, uint32_t since_seq, char **out_messages)
{
    if (h == NULL || !h->active || out_messages == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Calculate required buffer size. */
    size_t needed = 1;  /* null terminator */
    for (size_t i = 0; i < h->log_count; i++) {
        size_t idx = (h->log_head + h->log_cap - h->log_count + i) % h->log_cap;
        if (h->log[idx].seq > since_seq) {
            /* text is a fixed array member, so it is never NULL; the old
             * `a + b ? c : d` also bound as `(a + b) ? c : d`. */
            needed += strlen(h->log[idx].from_ip) + strlen(h->log[idx].text) + 16;
        }
    }

    char *buf = malloc(needed);
    if (buf == NULL) {
        xSemaphoreGive(h->mutex);
        return ESP_ERR_NO_MEM;
    }

    char *p = buf;
    *p = '\0';
    for (size_t i = 0; i < h->log_count; i++) {
        size_t idx = (h->log_head + h->log_cap - h->log_count + i) % h->log_cap;
        if (h->log[idx].seq > since_seq) {
            int written = snprintf(p, buf + needed - p, "%" PRIu32 "\t%s\t%s\n",
                                   h->log[idx].seq, h->log[idx].from_ip, h->log[idx].text);
            if (written > 0) {
                p += written;
            }
        }
    }

    xSemaphoreGive(h->mutex);
    *out_messages = buf;
    return ESP_OK;
}

esp_err_t chat_get_log(chat_handle_t h, char **out_log)
{
    if (h == NULL || !h->active || out_log == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t needed = 1;
    for (size_t i = 0; i < h->log_count; i++) {
        size_t idx = (h->log_head + h->log_cap - h->log_count + i) % h->log_cap;
        needed += strlen(h->log[idx].from_ip) + strlen(h->log[idx].text) + 16;
    }

    char *buf = malloc(needed);
    if (buf == NULL) {
        xSemaphoreGive(h->mutex);
        return ESP_ERR_NO_MEM;
    }

    char *p = buf;
    *p = '\0';
    for (size_t i = 0; i < h->log_count; i++) {
        size_t idx = (h->log_head + h->log_cap - h->log_count + i) % h->log_cap;
        int written = snprintf(p, buf + needed - p, "%" PRIu32 "\t%s\t%s\n",
                               h->log[idx].seq, h->log[idx].from_ip, h->log[idx].text);
        if (written > 0) {
            p += written;
        }
    }

    xSemaphoreGive(h->mutex);
    *out_log = buf;
    return ESP_OK;
}

esp_err_t chat_clear(chat_handle_t h)
{
    if (h == NULL || !h->active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    h->inbox_head = 0;
    h->inbox_count = 0;
    h->log_head = 0;
    h->log_count = 0;
    h->next_seq = 0;
    memset(h->inbox, 0, h->inbox_cap * sizeof(chat_msg_t));
    memset(h->log, 0, h->log_cap * sizeof(chat_msg_t));

    xSemaphoreGive(h->mutex);
    return ESP_OK;
}

esp_err_t chat_get_status(chat_handle_t h, char **out_status)
{
    if (h == NULL || !h->active || out_status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    char ip_buf[16];
    const char *ap_ip = "0.0.0.0";
    if (esp_netif_get_ip_info(h->ap_netif, &ip_info) == ESP_OK) {
        /* ip_info.ip is esp_ip4_addr_t; ip4addr_ntoa() takes lwIP ip4_addr_t.
         * esp_ip4addr_ntoa() is the matching helper. */
        esp_ip4addr_ntoa(&ip_info.ip, ip_buf, sizeof(ip_buf));
        ap_ip = ip_buf;
    }

    size_t needed = 256;
    char *buf = malloc(needed);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(buf, needed,
             "chat host active\n"
             "  AP SSID: %s\n"
             "  AP IP: %s\n"
             "  channel: %u\n"
             "  max connections: %u\n"
             "  TX power cap: %d qdbm\n"
             "  HTTP port: %u\n"
             "  next seq: %" PRIu32 "\n"
             "  inbox: %zu/%zu\n"
             "  log: %zu/%zu\n",
             h->config.ap_ssid,
             ap_ip,
             h->config.ap_channel,
             h->config.ap_max_conn,
             h->config.ap_tx_power_qdbm,
             h->config.http_port,
             h->next_seq,
             h->inbox_count, h->inbox_cap,
             h->log_count, h->log_cap);

    *out_status = buf;
    return ESP_OK;
}

/* --- HTTP server handlers --- */
static esp_err_t chat_http_post_handler(httpd_req_t *req)
{
    chat_handle_t h = (chat_handle_t)req->user_ctx;
    if (h == NULL || !h->active) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (req->content_len > h->config.msg_max_bytes) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "message too large");
        return ESP_FAIL;
    }

    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[received] = '\0';

    /* Get client IP. */
    char client_ip[16] = "unknown";
    httpd_req_to_sockfd(req);  /* ensures socket context */
    /* httpd doesn't expose peer IP directly in public API; use a workaround */
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0 && getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)&addr;
            inet_ntop(AF_INET, &in->sin_addr, client_ip, sizeof(client_ip));
        }
    }

    chat_msg_add(h, client_ip, buf);
    free(buf);

    /* Respond with sequence number. */
    char resp[32];
    snprintf(resp, sizeof(resp), "ok %" PRIu32, h->next_seq - 1);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t chat_http_get_handler(httpd_req_t *req)
{
    chat_handle_t h = (chat_handle_t)req->user_ctx;
    if (h == NULL || !h->active) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Parse ?since=<seq> query. */
    uint32_t since_seq = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char *since_str = strstr(query, "since=");
        if (since_str != NULL) {
            since_str += 6;
            since_seq = (uint32_t)strtoul(since_str, NULL, 10);
        }
    }

    char *messages = NULL;
    esp_err_t err = chat_poll(h, since_seq, &messages);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, messages, strlen(messages));
    free(messages);
    return ESP_OK;
}

static esp_err_t chat_httpd_start(chat_handle_t h)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = h->config.http_port;
    config.max_open_sockets = h->config.ap_max_conn + 2;
    config.backlog_conn = h->config.ap_max_conn + 2;
    config.lru_purge_enable = true;
    config.stack_size = P4_CONFIG_HTTPD_STACK_BYTES;
    config.task_priority = P4_CONFIG_HTTPD_TASK_PRIORITY;
    config.recv_wait_timeout = P4_CONFIG_HTTPD_RECV_TIMEOUT_S;
    config.send_wait_timeout = P4_CONFIG_HTTPD_SEND_TIMEOUT_S;

    httpd_uri_t post_uri = {
        .uri = "/chat",
        .method = HTTP_POST,
        .handler = chat_http_post_handler,
        .user_ctx = h
    };

    httpd_uri_t get_uri = {
        .uri = "/chat",
        .method = HTTP_GET,
        .handler = chat_http_get_handler,
        .user_ctx = h
    };

    esp_err_t err = httpd_start(&h->httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(CHAT_TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(h->httpd, &post_uri);
    if (err != ESP_OK) {
        httpd_stop(h->httpd);
        h->httpd = NULL;
        return err;
    }

    err = httpd_register_uri_handler(h->httpd, &get_uri);
    if (err != ESP_OK) {
        httpd_stop(h->httpd);
        h->httpd = NULL;
        return err;
    }

    ESP_LOGI(CHAT_TAG, "chat HTTP server started on port %u", h->config.http_port);
    return ESP_OK;
}

static void chat_httpd_stop(chat_handle_t h)
{
    if (h->httpd != NULL) {
        httpd_stop(h->httpd);
        h->httpd = NULL;
        ESP_LOGI(CHAT_TAG, "chat HTTP server stopped");
    }
}

/* ========================================================================
 * COMMAND-FACING HOST SINGLETON
 * ========================================================================
 * See chat.h for why these live here instead of in components/networking.
 * Exactly one host can be active; the command layer never sees the handle.
 */

static chat_handle_t s_host = NULL;

/** Guard for host start/stop. */
static SemaphoreHandle_t s_host_lock = NULL;

static void chat_host_lock(void)
{
    if (s_host_lock == NULL) {
        s_host_lock = xSemaphoreCreateMutex();
    }
    if (s_host_lock != NULL) {
        xSemaphoreTake(s_host_lock, portMAX_DELAY);
    }
}

static void chat_host_unlock(void)
{
    if (s_host_lock != NULL) {
        xSemaphoreGive(s_host_lock);
    }
}

bool chat_host_is_active(void)
{
    return s_host != NULL;
}

chat_handle_t chat_host_handle(void)
{
    return s_host;
}

esp_err_t chat_host_on(const char *ssid, char *out_ssid, size_t out_ssid_size,
                       char *out_url, size_t out_url_size)
{
    const char *effective;
    chat_config_t config;
    chat_handle_t handle = NULL;
    esp_err_t error;

    if (out_ssid != NULL && out_ssid_size > 0) {
        out_ssid[0] = '\0';
    }
    if (out_url != NULL && out_url_size > 0) {
        out_url[0] = '\0';
    }

    effective = (ssid != NULL && ssid[0] != '\0') ? ssid : P4_CONFIG_CHAT_AP_SSID;

    /* chat_start() mutates netif/Wi-Fi state, so the init-task claim is
     * mandatory here (same contract as the captive portal SoftAP). */
    if (!networking_wifi_try_claim_init_task()) {
        ESP_LOGW(CHAT_TAG, "host start refused: Wi-Fi init in progress");
        return ESP_ERR_INVALID_STATE;
    }

    chat_host_lock();
    if (s_host != NULL) {
        chat_host_unlock();
        networking_wifi_release_init_task();
        ESP_LOGW(CHAT_TAG, "host start refused: already active");
        return ESP_ERR_INVALID_STATE;
    }

    chat_config_init_default(&config);
    config.ap_ssid = effective;
    error = chat_start(&config, &handle);
    if (error == ESP_OK) {
        s_host = handle;
    }
    chat_host_unlock();
    networking_wifi_release_init_task();

    if (error != ESP_OK || handle == NULL) {
        ESP_LOGE(CHAT_TAG, "host start failed: %s", esp_err_to_name(error));
        return error;
    }

    if (out_ssid != NULL && out_ssid_size > 0) {
        snprintf(out_ssid, out_ssid_size, "%s", effective);
    }
    if (out_url != NULL && out_url_size > 0) {
        /* Report the AP address so the peer knows where to POST. The netif is
         * owned by the instance; a failure here is cosmetic only. */
        esp_netif_ip_info_t ip_info;

        if (esp_netif_get_ip_info(handle->ap_netif, &ip_info) == ESP_OK) {
            char ip_buf[16];

            esp_ip4addr_ntoa(&ip_info.ip, ip_buf, sizeof(ip_buf));
            snprintf(out_url, out_url_size, "http://%s:%u",
                     ip_buf, (unsigned)config.http_port);
        } else {
            snprintf(out_url, out_url_size, "http://0.0.0.0:%u",
                     (unsigned)config.http_port);
        }
    }
    ESP_LOGI(CHAT_TAG, "host active: SSID %s", effective);
    return ESP_OK;
}

esp_err_t chat_host_off(void)
{
    chat_handle_t handle;
    esp_err_t error;

    if (!networking_wifi_try_claim_init_task()) {
        ESP_LOGW(CHAT_TAG, "host stop refused: Wi-Fi init in progress");
        return ESP_ERR_INVALID_STATE;
    }

    chat_host_lock();
    handle = s_host;
    s_host = NULL;
    chat_host_unlock();

    if (handle == NULL) {
        networking_wifi_release_init_task();
        return ESP_ERR_INVALID_STATE;
    }
    error = chat_stop(handle);
    networking_wifi_release_init_task();
    return error;
}

/** Guard used by the accessors below so a concurrent stop cannot free the
 *  instance mid-call. */
static chat_handle_t chat_host_acquire(void)
{
    chat_handle_t handle;

    chat_host_lock();
    handle = s_host;
    chat_host_unlock();
    return handle;
}

esp_err_t chat_host_send(const char *peer_ip, const char *text)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return chat_send(handle, peer_ip, text);
}

esp_err_t chat_host_poll(uint32_t since_seq, char **out_messages)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return chat_poll(handle, since_seq, out_messages);
}

esp_err_t chat_host_log(char **out_log)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return chat_get_log(handle, out_log);
}

esp_err_t chat_host_clear(void)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return chat_clear(handle);
}

esp_err_t chat_host_status(char **out_status)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return chat_get_status(handle, out_status);
}

esp_err_t chat_host_onmsg(const char *line, chat_onmsg_cb_t cb)
{
    chat_handle_t handle = chat_host_acquire();

    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    chat_set_onmsg_cb(handle, cb);
    ESP_LOGI(CHAT_TAG, "onmsg hook %s", (line != NULL && line[0] != '\0') ? "set" : "cleared");
    return ESP_OK;
}