/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "apps/ping/ping_sock.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "bluetooth.h"
#include "http_server.h"
#include "led.h"
#include "netbench.h"
#include "netdiag.h"
#include "networking.h"
#include "wifi_known.h"
#include "p4minishell_config.h"

/* ---- Backward-compatibility aliases ---- */
#define NETWORKING_TAG                      P4_CONFIG_NETWORKING_TAG
#define NETWORKING_WIFI_DETAIL_BYTES        P4_CONFIG_WIFI_DETAIL_BYTES
#define NETWORKING_WIFI_ORIGIN_BYTES        P4_CONFIG_WIFI_ORIGIN_BYTES
#define NETWORKING_WIFI_INIT_TASK_STACK_BYTES P4_CONFIG_WIFI_INIT_TASK_STACK
#define NETWORKING_WIFI_RUNTIME_ENABLED     P4_CONFIG_WIFI_RUNTIME_ENABLED

typedef struct {
    bool use_defaults;
    char ssid[NETWORKING_WIFI_SSID_BYTES];
    char password[NETWORKING_WIFI_PASSWORD_BYTES];
} networking_wifi_connect_request_t;

typedef struct {
    bool start_runtime;
    bool connect_with_defaults;
    bool run_diagnostic;
    char origin[NETWORKING_WIFI_ORIGIN_BYTES];
    char ssid[NETWORKING_WIFI_SSID_BYTES];
    char password[NETWORKING_WIFI_PASSWORD_BYTES];
} networking_wifi_background_request_t;

static networking_host_ops_t s_host_ops;
static networking_wifi_state_t s_wifi_state = NETWORKING_WIFI_STATE_NOT_ATTEMPTED;
static esp_err_t s_wifi_last_error = ESP_OK;
/* True between a successful esp_wifi_init() and its matching deinit, so the
 * failure cleanup can release the driver without spurious NOT_INIT errors. */
static bool s_wifi_driver_inited;
static bool s_wifi_connected;
static bool s_wifi_connect_requested;
static bool s_wifi_boot_autoconnect = true;
static char s_wifi_target_ssid[NETWORKING_WIFI_SSID_BYTES];
static char s_wifi_target_password[NETWORKING_WIFI_PASSWORD_BYTES];
static char s_wifi_last_detail[NETWORKING_WIFI_DETAIL_BYTES];
static bool s_wifi_init_task_in_progress;
static bool s_networking_initialized;
static SemaphoreHandle_t s_wifi_mutex;  /* protects all shared Wi-Fi state */

/* Association uptime tracking: when the station last completed the 4-way
 * handshake (esp_timer_get_time(), 0 = not associated). `wifi status` uses
 * it to report how long the current association has been up. */
static int64_t s_wifi_associated_at_us;

/* ---- Persistent Wi-Fi watchdog ---- */
/* A single persistent task that monitors Wi-Fi connection state and retries
 * with exponential backoff. This is more reliable than spawning per-disconnect
 * tasks because it handles the C6's typical boot behavior: associate then
 * immediately disconnect during 4-way handshake or DHCP. */
#define WIFI_WATCHDOG_STACK_BYTES    8192
#define WIFI_WATCHDOG_DELAY_MS       1000      /* initial delay before first retry */
#define WIFI_WATCHDOG_MAX_DELAY_MS   30000     /* maximum backoff delay */
#define WIFI_WATCHDOG_TOTAL_TIMEOUT_MS 120000  /* stop retrying after this long */

static TaskHandle_t s_wifi_watchdog_task;       /* NULL when not running */
static int64_t s_wifi_watchdog_started_us;      /* when watchdog began */
static int s_wifi_watchdog_retry_count;         /* for exponential backoff */

#if NETWORKING_WIFI_RUNTIME_ENABLED
static esp_netif_t *s_wifi_sta_netif;
static esp_event_handler_instance_t s_wifi_event_any_id;
static esp_event_handler_instance_t s_wifi_got_ip_event;
#endif

static void networking_wifi_runtime_init(void);
static void networking_wifi_connect_task(void *arg);
static void networking_wifi_background_task(void *arg);
static void networking_wifi_watchdog_task(void *arg);
static void networking_wifi_start_watchdog(void);
static esp_err_t networking_wifi_connect_with_credentials(const char *ssid, const char *password);

/* Wi-Fi mutex helpers — forward declarations for use in event handler */
static void wifi_lock(void);
static void wifi_unlock(void);
static bool wifi_try_claim_init_task(void);
static void wifi_release_init_task(void);

static bool networking_text_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static int networking_split_args(char *text, char **argv, int max_args)
{
    int argc = 0;
    char *cursor = text;

    while (cursor != NULL && *cursor != '\0' && argc < max_args) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }
    }

    return argc;
}

static void networking_appendf(const char *format, ...)
{
    if (s_host_ops.transcript_append_ansi == NULL) {
        return;
    }

    char buffer[512];
    va_list args;
    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.transcript_append_ansi(buffer);
}

static void networking_schedulef_ansi(const char *format, ...)
{
    if (s_host_ops.transcript_append_ansi == NULL) {
        return;
    }

    char buffer[512];
    va_list args;
    va_start(args, format);
    /* Use ansi_vformat to convert @ tokens to real ANSI escape sequences */
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);

    s_host_ops.transcript_append_ansi(buffer);
}

static void networking_record_errorf(esp_err_t error, const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_error == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_error(NETWORKING_TAG, error, buffer);
}

static void networking_record_warningf(const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_warning == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_warning(NETWORKING_TAG, buffer);
}

static void networking_record_infof(const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_info == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_info(NETWORKING_TAG, buffer);
}

static void networking_notify_headerf(uint32_t timeout_ms, const char *format, ...)
{
    char buffer[160];
    va_list args;

    if (s_host_ops.notify_header == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.notify_header(buffer, timeout_ms);
}

static bool networking_wifi_defaults_available(void)
{
    return CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID[0] != '\0';
}

static void networking_wifi_set_detail(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(s_wifi_last_detail, sizeof(s_wifi_last_detail), format, args);
    va_end(args);
}

static const char *networking_wifi_state_string_internal(void)
{
    switch (s_wifi_state) {
    case NETWORKING_WIFI_STATE_STARTING:
        return "starting";
    case NETWORKING_WIFI_STATE_STARTED:
        return "started";
    case NETWORKING_WIFI_STATE_FAILED:
        return "failed";
    case NETWORKING_WIFI_STATE_SKIPPED_DISABLED:
        return "disabled";
    case NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED:
        return "unsupported";
    case NETWORKING_WIFI_STATE_NOT_ATTEMPTED:
    default:
        return "not_attempted";
    }
}

static void networking_wifi_append_step(const char *step)
{
    networking_schedulef_ansi("@C[wifi]@R %s\n", step);
    networking_record_infof("%s", step);
    networking_notify_headerf(3500, "%s", step);
}

static void networking_wifi_append_error(const char *step, esp_err_t error)
{
    s_wifi_state = NETWORKING_WIFI_STATE_FAILED;
    s_wifi_last_error = error;
    networking_schedulef_ansi("@C[wifi]@R @r%s failed@R: @r%s@R (0x%x)\n", step, esp_err_to_name(error), (unsigned int)error);
    networking_record_errorf(error, "%s failed", step);
    networking_notify_headerf(5000, "WiFi error: %s", step);
}

static void networking_wifi_cleanup_runtime_artifacts(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    if (s_wifi_event_any_id != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_any_id);
        s_wifi_event_any_id = NULL;
    }

    if (s_wifi_got_ip_event != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_wifi_got_ip_event);
        s_wifi_got_ip_event = NULL;
    }

    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }

    /* Release the Wi-Fi driver itself. Without this a runtime-init failure
     * leaves esp_wifi initialized, so every later retry fails at
     * esp_wifi_init() and the only recovery is a reboot. */
    if (s_wifi_driver_inited) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        s_wifi_driver_inited = false;
    }
#endif

    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    s_wifi_associated_at_us = 0;
    s_wifi_target_ssid[0] = '\0';
    s_wifi_target_password[0] = '\0';
}

static void networking_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            networking_wifi_append_step("event: station started");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            wifi_lock();
            if (event_data != NULL) {
                const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
                size_t copy_len = event->ssid_len;

                if (copy_len >= sizeof(s_wifi_target_ssid)) {
                    copy_len = sizeof(s_wifi_target_ssid) - 1;
                }

                memcpy(s_wifi_target_ssid, event->ssid, copy_len);
                s_wifi_target_ssid[copy_len] = '\0';
                s_wifi_associated_at_us = esp_timer_get_time();
                networking_schedulef_ansi("@C[wifi]@R event: @Gassociated@R with @W%s@R on channel @Z%u@R\n",
                                     s_wifi_target_ssid,
                                     (unsigned int)event->channel);
            }
            wifi_unlock();
            /* Associated but not yet assigned an IP; keep the status LED on
             * the connecting colour until DHCP completes. */
            led_notify(LED_EVENT_WIFI_CONNECTING);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            wifi_lock();
            s_wifi_connected = false;
            s_wifi_connect_requested = false;
            s_wifi_associated_at_us = 0;
            wifi_unlock();
            networking_schedulef_ansi("@C[wifi]@R event: @ydisconnected@R\n");
            networking_notify_headerf(4000, "WiFi disconnected");
            led_notify(LED_EVENT_WIFI_DISCONNECTED);
            /* The HTTP file server needs a reachable link; tear it down. */
            networking_httpd_maybe_stop();
            /* Start the persistent watchdog to attempt reconnection */
            networking_wifi_start_watchdog();
            break;

        default:
            break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && event_data != NULL) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ssid_copy[NETWORKING_WIFI_SSID_BYTES];
        char pass_copy[NETWORKING_WIFI_PASSWORD_BYTES];

        wifi_lock();
        s_wifi_connected = true;
        s_wifi_connect_requested = false;
        snprintf(ssid_copy, sizeof(ssid_copy), "%s", s_wifi_target_ssid);
        snprintf(pass_copy, sizeof(pass_copy), "%s", s_wifi_target_password);
        wifi_unlock();
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " event: got IP " SH_VAL IPSTR SH_RST "\n", IP2STR(&event->ip_info.ip));
        networking_notify_headerf(4000, "WiFi connected: " IPSTR, IP2STR(&event->ip_info.ip));
        led_notify(LED_EVENT_WIFI_CONNECTED);

        /* The HTTP file server rides the station link: auto-start it here. */
        networking_httpd_maybe_autostart();

        /* Remember this network on the SD known-list when autosave is on and
         * the card is present. Fully safe (and silent) without a card. */
        if (ssid_copy[0] != '\0' && P4_CONFIG_WIFI_KNOWN_AUTOSAVE) {
            networking_wifi_known_record_connect(ssid_copy, pass_copy);
        }
    }
}

static esp_err_t networking_wifi_register_event_handlers(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;

    error = esp_event_handler_instance_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                networking_wifi_event_handler,
                                                NULL,
                                                &s_wifi_event_any_id);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_instance_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                networking_wifi_event_handler,
                                                NULL,
                                                &s_wifi_got_ip_event);
    if (error != ESP_OK) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_any_id);
        s_wifi_event_any_id = NULL;
        return error;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t networking_wifi_validate_hosted_version(void)
{
    esp_hosted_coprocessor_fwver_t version = { 0 };
    esp_err_t error;

    error = esp_hosted_get_coprocessor_fwversion(&version);
    if (error != ESP_OK) {
        networking_wifi_set_detail("ESP-Hosted connected but the shell could not read the ESP32-C6 firmware version. Wi-Fi stays off because the hosted link is not trustworthy for remote Wi-Fi init. Rebuild or externally refresh coprocessor/esp32c6_slave and retry.");
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "failed to read C6 hosted firmware version:" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_WARN "Recovery:" SH_RST " flash a matching " SH_VAL "%u.x" SH_RST " ESP32-C6 image from " SH_PATH "coprocessor/esp32c6_slave" SH_RST " or use c6ota default with " SH_PATH "esp32c6_hosted_slave.bin" SH_RST "\n",
                             P4_CONFIG_HOSTED_COMPAT_MAJOR);
        networking_record_warningf("Failed to read hosted firmware version: %s", esp_err_to_name(error));
        return error;
    }

    /* Definitive co-processor firmware check: log the exact version fields the
     * C6 reported so the running firmware can be identified (vs the corrupted
     * reads c6ota sometimes sees). */
    networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " C6 hosted firmware version: " SH_VAL "%u.%u.%u" SH_RST "\n",
                         (unsigned int)version.major1,
                         (unsigned int)version.minor1,
                         (unsigned int)version.patch1);

    /* Major-only gate: esp_hosted 3.x froze its public compat version macros
     * at the 2.12.6 baseline, so ESP_HOSTED_VERSION_MAJOR_1/MINOR_1 no longer
     * track the real host version. Minor/patch float within a major (RPC-V2
     * is wire-stable there); only the C6-reported major must match
     * P4_CONFIG_HOSTED_COMPAT_MAJOR. */
    if (version.major1 != P4_CONFIG_HOSTED_COMPAT_MAJOR) {
#if P4_CONFIG_HOSTED_SKIP_VERSION_GATE
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_WARN "hosted version mismatch (gate skipped):" SH_RST " host " SH_VAL "%u.x" SH_RST ", C6 " SH_VAL "%u.%u.%u" SH_RST "\n",
                             P4_CONFIG_HOSTED_COMPAT_MAJOR,
                             version.major1,
                             version.minor1,
                             version.patch1);
        networking_record_warningf("Hosted version mismatch (gate skipped): host %u.x vs C6 %u.%u.%u",
                                   P4_CONFIG_HOSTED_COMPAT_MAJOR,
                                   version.major1,
                                   version.minor1,
                                   version.patch1);
#else
        networking_wifi_set_detail("ESP-Hosted host %u.x requires ESP32-C6 firmware %u.x, but the co-processor reports %u.%u.%u. Flash the matching hosted slave build before retrying Wi-Fi.",
                                   P4_CONFIG_HOSTED_COMPAT_MAJOR,
                                   P4_CONFIG_HOSTED_COMPAT_MAJOR,
                                   version.major1,
                                   version.minor1,
                                   version.patch1);
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "hosted version mismatch:" SH_RST " host " SH_VAL "%u.x" SH_RST ", C6 " SH_VAL "%u.%u.%u" SH_RST "\n",
                             P4_CONFIG_HOSTED_COMPAT_MAJOR,
                             version.major1,
                             version.minor1,
                             version.patch1);
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_WARN "Recovery:" SH_RST " flash a matching " SH_VAL "%u.x" SH_RST " ESP32-C6 image from " SH_PATH "coprocessor/esp32c6_slave" SH_RST " or use c6ota default with " SH_PATH "esp32c6_hosted_slave.bin" SH_RST "\n",
                             P4_CONFIG_HOSTED_COMPAT_MAJOR);
        networking_record_warningf("Hosted version mismatch: host %u.x vs C6 %u.%u.%u",
                                   P4_CONFIG_HOSTED_COMPAT_MAJOR,
                                   version.major1,
                                   version.minor1,
                                   version.patch1);
        return ESP_ERR_INVALID_STATE;
#endif
    }

    networking_record_infof("Hosted firmware compatible: host %u.x, C6 %u.%u.%u",
                            P4_CONFIG_HOSTED_COMPAT_MAJOR,
                            version.major1,
                            version.minor1,
                            version.patch1);
    return ESP_OK;
}

static esp_err_t networking_wifi_connect_with_credentials(const char *ssid, const char *password)
{
    esp_err_t error;
    wifi_config_t config = { 0 };

    wifi_lock();
    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        wifi_unlock();
        networking_appendf("wifi: stack is not ready (%s)\n", networking_wifi_state_string_internal());
        return ESP_ERR_INVALID_STATE;
    }
    wifi_unlock();

    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);

    networking_wifi_append_step("esp_wifi_set_config(WIFI_IF_STA)");
    error = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_set_config(WIFI_IF_STA)", error);
        return error;
    }

    error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        networking_wifi_append_error("esp_wifi_disconnect()", error);
        return error;
    }

    wifi_lock();
    snprintf(s_wifi_target_ssid, sizeof(s_wifi_target_ssid), "%s", ssid);
    snprintf(s_wifi_target_password, sizeof(s_wifi_target_password), "%s", password);
    s_wifi_connect_requested = true;
    s_wifi_connected = false;
    networking_schedulef_ansi("@C[wifi]@R @Gconnect requested@R for @W%s@R\n", s_wifi_target_ssid);
    wifi_unlock();

    led_notify(LED_EVENT_WIFI_CONNECTING);

    networking_wifi_append_step("esp_wifi_connect()");
    error = esp_wifi_connect();
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_connect()", error);
        return error;
    }

    return ESP_OK;
}

esp_err_t networking_wifi_shutdown(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        return ESP_OK;
    }

    error = esp_wifi_stop();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
        return error;
    }

    error = esp_wifi_deinit();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
        return error;
    }
    s_wifi_driver_inited = false;

    networking_wifi_cleanup_runtime_artifacts();
    s_wifi_state = NETWORKING_WIFI_STATE_NOT_ATTEMPTED;
    s_wifi_last_error = ESP_OK;
    s_wifi_last_detail[0] = '\0';
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static bool networking_wifi_begin_connect_request(const char *ssid, const char *password, bool use_defaults)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_connect_request_t *request;

    if (!wifi_try_claim_init_task()) {
        networking_appendf("wifi: initialization is already in progress\n");
        return false;
    }

    request = (networking_wifi_connect_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        networking_record_errorf(ESP_ERR_NO_MEM, "Failed to allocate Wi-Fi connect request");
        wifi_release_init_task();
        return false;
    }

    request->use_defaults = use_defaults;
    if (ssid != NULL) {
        snprintf(request->ssid, sizeof(request->ssid), "%s", ssid);
    }
    if (password != NULL) {
        snprintf(request->password, sizeof(request->password), "%s", password);
    }

    wifi_lock();
    s_wifi_state = NETWORKING_WIFI_STATE_STARTING;
    s_wifi_last_error = ESP_OK;
    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    wifi_unlock();

    if (xTaskCreate(networking_wifi_connect_task,
                    "wifi_connect",
                    NETWORKING_WIFI_INIT_TASK_STACK_BYTES,
                    request,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        free(request);
        wifi_lock();
        s_wifi_state = NETWORKING_WIFI_STATE_NOT_ATTEMPTED;
        wifi_unlock();
        wifi_release_init_task();
        networking_record_errorf(ESP_FAIL, "Failed to start Wi-Fi connect task");
        return false;
    }

    return true;
#else
    (void)ssid;
    (void)password;
    (void)use_defaults;
    return false;
#endif
}

static bool networking_wifi_begin_background_request(const networking_wifi_background_request_t *template_request)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t *request;

    if (template_request == NULL) {
        return false;
    }

    if (!wifi_try_claim_init_task()) {
        networking_appendf("wifi: initialization is already in progress\n");
        return false;
    }

    request = (networking_wifi_background_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        networking_record_errorf(ESP_ERR_NO_MEM, "Failed to allocate Wi-Fi background request");
        wifi_release_init_task();
        return false;
    }

    *request = *template_request;
    if (request->start_runtime) {
        wifi_lock();
        s_wifi_state = NETWORKING_WIFI_STATE_STARTING;
        s_wifi_last_error = ESP_OK;
        s_wifi_connected = false;
        s_wifi_connect_requested = false;
        wifi_unlock();
    }

    if (xTaskCreate(networking_wifi_background_task,
                    "wifi_bg",
                    NETWORKING_WIFI_INIT_TASK_STACK_BYTES,
                    request,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        free(request);
        if (template_request->start_runtime) {
            wifi_lock();
            s_wifi_state = NETWORKING_WIFI_STATE_NOT_ATTEMPTED;
            wifi_unlock();
        }
        wifi_release_init_task();
        networking_record_errorf(ESP_FAIL, "Failed to start Wi-Fi background task");
        return false;
    }

    return true;
#else
    (void)template_request;
    return false;
#endif
}

static void networking_wifi_connect_task(void *arg)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_connect_request_t *request = (networking_wifi_connect_request_t *)arg;

    networking_wifi_runtime_init();
    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
        if (request->use_defaults) {
            if (networking_wifi_defaults_available()) {
                (void)networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                               CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            } else {
                s_wifi_state = NETWORKING_WIFI_STATE_FAILED;
                s_wifi_last_error = ESP_ERR_INVALID_STATE;
                networking_appendf("wifi: sdkconfig default credentials are not configured\n");
            }
        } else {
            (void)networking_wifi_connect_with_credentials(request->ssid, request->password);
        }
    }

    wifi_release_init_task();
    free(request);
#else
    (void)arg;
#endif
    vTaskDelete(NULL);
}

static esp_err_t networking_wifi_run_diagnostic(const char *origin)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t ap_info;
    wifi_ap_record_t records[16];
    uint16_t record_count = (uint16_t)(sizeof(records) / sizeof(records[0]));
    uint16_t index;
    esp_netif_ip_info_t ip_info = { 0 };
    const char *label = (origin != NULL && origin[0] != '\0') ? origin : "runtime";

    char connected_str[24];
    char requested_str[24];
    /* Pre-colour the yes/no values with real SGR escapes (via ansi_format) so
     * they render correctly when passed as %s arguments — @-specifiers in a %s
     * argument are not converted by ansi_vformat. Real escapes pass through
     * safely and are stripped for the LVGL transcript by ansi_strip_to_plain. */
    ansi_format(connected_str, sizeof(connected_str), s_wifi_connected ? "@Gyes@R" : "@Kno@R");
    ansi_format(requested_str, sizeof(requested_str), s_wifi_connect_requested ? "@Gyes@R" : "@Kno@R");
    networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Cstate@R=@W%s@R @Cconnected@R=%s @Crequested@R=%s\n",
                         label,
                         networking_wifi_state_string_internal(),
                         connected_str,
                         requested_str);

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @yruntime not started@R\n", label);
        return ESP_ERR_INVALID_STATE;
    }

    error = esp_wifi_sta_get_ap_info(&ap_info);
    if (error == ESP_OK) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Cconnected_ssid@R=@W%s@R @Crssi@R=@Z%d@R @Cchannel@R=@Z%u@R\n",
                             ap_info.ssid[0] != '\0' ? (const char *)ap_info.ssid : "<hidden>",
                             ap_info.rssi,
                             (unsigned int)ap_info.primary);
    } else if (error == ESP_ERR_WIFI_NOT_CONNECT) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Knot connected to an AP@R\n", label);
    } else {
        networking_schedulef_ansi("@C[wifi.diag]@R @rap info failed@R: @r%s@R (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Cip@R=@W" IPSTR "@R\n", IP2STR(&ip_info.ip));
    } else {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Kip=not-assigned@R\n", label);
    }

    if (s_wifi_connect_requested && !s_wifi_connected) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @yscan skipped while station connect is in progress@R\n", label);
        return ESP_OK;
    }

    networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Nscanning for nearby networks...@R\n", label);
    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        networking_schedulef_ansi("@C[wifi.diag]@R @rscan failed@R: @r%s@R (0x%x)\n", esp_err_to_name(error), (unsigned int)error);
        networking_record_warningf("diagnostic scan failed from %s", label);
        return error;
    }

    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        networking_schedulef_ansi("@C[wifi.diag]@R @rreading scan results failed@R: @r%s@R (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
        networking_record_warningf("diagnostic scan result read failed from %s", label);
        return error;
    }

    networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Cnetworks@R=@Z%u@R\n", label, (unsigned int)record_count);
    if (record_count == 0) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Kno networks found@R\n", label);
        return ESP_OK;
    }

    for (index = 0; index < record_count; index++) {
        networking_schedulef_ansi("@C[wifi.diag][%u]@R @Wssid@R=@W%s@R @Crssi@R=@Z%d@R @Cchannel@R=@Z%u@R @Cauth@R=@Z%u@R\n",
                             (unsigned int)index,
                             records[index].ssid[0] != '\0' ? (const char *)records[index].ssid : "<hidden>",
                             records[index].rssi,
                             (unsigned int)records[index].primary,
                             (unsigned int)records[index].authmode);
    }

    networking_record_infof("diagnostic scan completed from %s with %u APs", label, (unsigned int)record_count);
    return ESP_OK;
#else
    (void)origin;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * Boot-time auto-connect to a previously-used (known) network.
 *
 * Called from the background task once the STA runtime is up and only when the
 * boot autoconnect policy (WIFI_AUTOCONNECT=ON) is in force. Loads the
 * known-network list from the SD card (safe, empty on any failure), scans for
 * visible networks, and connects to the best one (preferred / highest priority
 * / strongest RSSI). The chosen network becomes the watchdog target, so the
 * existing exponential-backoff machinery retries it.
 *
 * @return true when a connect to a visible known network was initiated, false
 *         when the list is empty / SD is unavailable / no known network is
 *         visible (caller falls back to the single-credential path).
 */
static bool networking_wifi_auto_connect_known(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t *records = NULL;
    uint16_t requested = P4_CONFIG_WIFI_SCAN_LIMIT;
    uint16_t record_count;
    int best_index;

    if (!s_wifi_boot_autoconnect) {
        return false;
    }

    /* Load the known list (safe; any failure means "no known networks"). */
    if (networking_wifi_known_load() != ESP_OK) {
        return false;
    }
    if (networking_wifi_known_count() == 0) {
        return false;
    }

    records = (wifi_ap_record_t *)calloc(requested, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        networking_record_warningf("Auto-connect scan allocation failed");
        return false;
    }
    record_count = requested;

    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        networking_record_warningf("Auto-connect scan failed: %s", esp_err_to_name(error));
        free(records);
        return false;
    }
    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        networking_record_warningf("Auto-connect scan result read failed: %s", esp_err_to_name(error));
        free(records);
        return false;
    }

    best_index = networking_wifi_known_pick_visible(records, record_count);
    free(records);
    if (best_index < 0) {
        networking_schedulef_ansi("@C[wifi]@R @Yauto-connect:@R no known network in range\n");
        return false;
    }

    {
        networking_wifi_known_entry_t entry;
        esp_err_t connect_error;

        if (!networking_wifi_known_get(best_index, &entry)) {
            return false;
        }
        networking_schedulef_ansi("@C[wifi]@R @Yauto-connect:@R connecting to known network @W%s@R\n",
                                  entry.ssid);
        connect_error = networking_wifi_connect_with_credentials(entry.ssid, entry.password);
        if (connect_error != ESP_OK) {
            networking_record_warningf("Auto-connect to known network %s failed: %s",
                                       entry.ssid, esp_err_to_name(connect_error));
        }
    }
    return true;
#else
    return false;
#endif
}

static void networking_wifi_background_task(void *arg)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t *request = (networking_wifi_background_request_t *)arg;

    if (request->start_runtime) {
        networking_wifi_runtime_init();
    }

    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
        /* Boot auto-connect to a known network takes precedence when enabled;
         * fall back to the classic single-credential path otherwise. */
        bool connected_known = false;

        if (s_wifi_boot_autoconnect && !s_wifi_connect_requested) {
            connected_known = networking_wifi_auto_connect_known();
        }

        if (!connected_known) {
            if (request->connect_with_defaults) {
                if (networking_wifi_defaults_available()) {
                    esp_err_t connect_error = networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                                                       CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
                    if (connect_error != ESP_OK) {
                        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "%s default connect failed:" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                                             request->origin,
                                             esp_err_to_name(connect_error),
                                             (unsigned int)connect_error);
                        networking_record_warningf("%s default connect failed", request->origin);
                    }
                } else {
                    networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_WARN "%s: sdkconfig default credentials are not configured" SH_RST "\n", request->origin);
                    networking_record_warningf("%s requested default connect without configured credentials", request->origin);
                }
            } else if (request->ssid[0] != '\0') {
                esp_err_t connect_error = networking_wifi_connect_with_credentials(request->ssid, request->password);
                if (connect_error != ESP_OK) {
                    networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "%s connect failed for " SH_VAL "%s" SH_RST ":" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                                         request->origin,
                                         request->ssid,
                                         esp_err_to_name(connect_error),
                                         (unsigned int)connect_error);
                    networking_record_warningf("%s connect failed for %s", request->origin, request->ssid);
                }
            }
        }

        if (request->run_diagnostic) {
            esp_err_t diagnostic_error = networking_wifi_run_diagnostic(request->origin);
            if (diagnostic_error != ESP_OK && diagnostic_error != ESP_ERR_INVALID_STATE) {
                networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " %s diagnostic finished with " SH_ERR "%s" SH_RST " (0x%x)\n",
                                     request->origin,
                                     esp_err_to_name(diagnostic_error),
                                     (unsigned int)diagnostic_error);
            }
        }
    } else {
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "%s could not start the Wi-Fi runtime cleanly" SH_RST "\n",
                             request->origin[0] != '\0' ? request->origin : "runtime");
        networking_record_warningf("%s failed to start Wi-Fi runtime",
                                   request->origin[0] != '\0' ? request->origin : "runtime");
    }

    wifi_release_init_task();
    free(request);
#else
    (void)arg;
#endif
    vTaskDelete(NULL);
}

static void networking_wifi_request_boot_restore(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t request = {
        .start_runtime = true,
        .connect_with_defaults = networking_wifi_defaults_available(),
#if CONFIG_P4MINISHELL_WIFI_BOOT_DIAGNOSTIC
        .run_diagnostic = true,
#else
        .run_diagnostic = false,
#endif
    };

    snprintf(request.origin, sizeof(request.origin), "%s", "boot");
    if (!networking_wifi_begin_background_request(&request)) {
        networking_appendf("wifi: boot-time Wi-Fi startup request could not be queued\n");
    }
#endif
}

/** Queue a background Wi-Fi restore under @p origin (shared by the OTA and
 *  wake paths so both keep one request shape). */
static void networking_wifi_request_restore(const networking_wifi_restore_state_t *restore_state,
                                            const char *origin)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t request = {
        .start_runtime = true,
        .connect_with_defaults = false,
        .run_diagnostic = true,
    };

    snprintf(request.origin, sizeof(request.origin), "%s", origin);
    if (restore_state != NULL && restore_state->should_restore_runtime) {
        if (restore_state->should_restore_connection && restore_state->ssid[0] != '\0') {
            snprintf(request.ssid, sizeof(request.ssid), "%s", restore_state->ssid);
            snprintf(request.password, sizeof(request.password), "%s", restore_state->password);
        } else if (networking_wifi_defaults_available()) {
            request.connect_with_defaults = true;
        }
    } else if (networking_wifi_defaults_available()) {
        request.connect_with_defaults = true;
    }

    if (!networking_wifi_begin_background_request(&request)) {
        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " " SH_ERR "%s: failed to queue Wi-Fi restore" SH_RST "\n", request.origin);
    }
#else
    (void)restore_state;
    (void)origin;
#endif
}

void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state)
{
    networking_wifi_request_restore(restore_state, "c6ota-restore");
}

void networking_wifi_request_wake_restore(const networking_wifi_restore_state_t *restore_state)
{
    networking_wifi_request_restore(restore_state, "wake-restore");
}

void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *restore_state)
{
    if (restore_state == NULL) {
        return;
    }

    memset(restore_state, 0, sizeof(*restore_state));
    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        return;
    }

    restore_state->should_restore_runtime = true;
    if (s_wifi_target_ssid[0] == '\0') {
        return;
    }

    restore_state->should_restore_connection = s_wifi_connected || s_wifi_connect_requested;
    snprintf(restore_state->ssid, sizeof(restore_state->ssid), "%s", s_wifi_target_ssid);
    snprintf(restore_state->password, sizeof(restore_state->password), "%s", s_wifi_target_password);
}

esp_err_t networking_wifi_restore_after_ota_failure(const networking_wifi_restore_state_t *restore_state)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;

    if (restore_state == NULL || !restore_state->should_restore_runtime) {
        return ESP_OK;
    }

    networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " restoring Wi-Fi after OTA failure\n");
    networking_wifi_runtime_init();
    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        return s_wifi_last_error != ESP_OK ? s_wifi_last_error : ESP_FAIL;
    }

    if (!restore_state->should_restore_connection || restore_state->ssid[0] == '\0') {
        return ESP_OK;
    }

    error = networking_wifi_connect_with_credentials(restore_state->ssid, restore_state->password);
    if (error != ESP_OK) {
        return error;
    }

    return ESP_OK;
#else
    (void)restore_state;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t networking_wifi_wait_for_ota(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    TickType_t next_log = xTaskGetTickCount();

    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTING) {
        networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " waiting for Wi-Fi startup already in progress\n");
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        if (!networking_wifi_defaults_available()) {
            networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " OTA requires Wi-Fi. Run wifi connect first or configure sdkconfig defaults.\n");
            return ESP_ERR_INVALID_STATE;
        }

        networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " starting Wi-Fi with sdkconfig default credentials for OTA\n");
        networking_wifi_runtime_init();
        if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
            return s_wifi_last_error != ESP_OK ? s_wifi_last_error : ESP_ERR_INVALID_STATE;
        }
    }

    if (!s_wifi_connected && !s_wifi_connect_requested) {
        if (!networking_wifi_defaults_available()) {
            networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " OTA requires an active network link. Run wifi connect <ssid> <pass> first.\n");
            return ESP_ERR_INVALID_STATE;
        }

        networking_schedulef_ansi(SH_PROMPT "[wifi]" SH_RST " ota: connecting to default SSID " SH_VAL "%s" SH_RST "\n", CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
        if (networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                     CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD) != ESP_OK) {
            return s_wifi_last_error != ESP_OK ? s_wifi_last_error : ESP_FAIL;
        }
    }

    while (!s_wifi_connected) {
        TickType_t now = xTaskGetTickCount();

        if ((int32_t)(now - deadline) >= 0) {
            networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " Wi-Fi connection timeout before OTA download\n");
            return ESP_ERR_TIMEOUT;
        }

        if ((int32_t)(now - next_log) >= 0) {
            networking_schedulef_ansi(SH_OTA "[c6ota]" SH_RST " waiting for Wi-Fi IP before OTA download\n");
            next_log = now + pdMS_TO_TICKS(5000);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * Map a `wifi_auth_mode_t` to the short DOS-style label used by the scan
 * table and the status report.
 */
static const char *networking_wifi_auth_label(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "?";
    }
}

/**
 * Build a human-readable PHY descriptor for an AP record, e.g. "802.11n".
 * The AP record exposes the negotiated PHY generation flags but not a live
 * bitrate, so the mode plus the current STA bandwidth is the closest honest
 * "PHY rate" the hosted stack reports.
 */
static void networking_wifi_format_phy(const wifi_ap_record_t *ap,
                                       const char *bandwidth,
                                       char *out, size_t out_size)
{
    if (ap == NULL || out == NULL || out_size == 0) {
        return;
    }

    const char *bw = (bandwidth != NULL && bandwidth[0] != '\0') ? bandwidth : "";

    if (ap->phy_11ax) {
        snprintf(out, out_size, "802.11ax %s", bw);
    } else if (ap->phy_11ac) {
        snprintf(out, out_size, "802.11ac %s", bw);
    } else if (ap->phy_11n) {
        snprintf(out, out_size, "802.11n %s", bw);
    } else if (ap->phy_11g) {
        snprintf(out, out_size, "802.11g");
    } else if (ap->phy_11b) {
        snprintf(out, out_size, "802.11b");
    } else if (ap->phy_lr) {
        snprintf(out, out_size, "802.11 LR");
    } else {
        snprintf(out, out_size, "unknown");
    }
}

/**
 * Format a 6-byte BSSID as "xx:xx:xx:xx:xx:xx".
 */
static void networking_wifi_format_bssid(const uint8_t bssid[6], char *out, size_t out_size)
{
    snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

/**
 * `wifi scan [/b] [max]` — sorted by RSSI, column-aligned, optionally bare.
 *
 * The AP table is heap-allocated (a busy channel can return far more records
 * than the worker task stack should hold) and sorted strongest-signal first.
 * `bare` selects the machine-readable form (bare SSID lines, no colour), so
 * `wifi scan /b > ap.txt` produces a clean list for a batch file to consume.
 */
void networking_wifi_scan(bool bare)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t *records = NULL;
    uint16_t requested = P4_CONFIG_WIFI_SCAN_LIMIT;
    uint16_t record_count;
    uint16_t limit;
    uint16_t index;
    uint16_t inner;

    if (requested < 1) {
        requested = 1;
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_appendf(SH_WARN "wifi.scan:" SH_RST " the Wi-Fi runtime is not started (@K%s@R)\n",
                           networking_wifi_state_string_internal());
        networking_record_warningf("Scan rejected because Wi-Fi is not started");
        return;
    }

    records = (wifi_ap_record_t *)calloc(requested, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        networking_record_errorf(ESP_ERR_NO_MEM, "WiFi scan failed to allocate the AP table");
        return;
    }
    record_count = requested;

    networking_appendf(SH_PROMPT "wifi.scan:" SH_RST " scanning for access points...\n");
    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        networking_record_errorf(error, "WiFi scan failed - check sdkconfig or hosted link");
        free(records);
        return;
    }

    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        networking_record_errorf(error, "Failed to read WiFi scan results");
        free(records);
        return;
    }

    if (record_count == 0) {
        networking_appendf(SH_MUTE "wifi.scan:" SH_RST " no access points found\n");
        networking_record_infof("Scan completed with no visible APs");
        free(records);
        return;
    }

    /* Selection sort by RSSI, strongest first. The record set is bounded by
     * the driver's own caps, so the O(n^2) loop is cheap in practice. */
    for (index = 0; index < record_count; index++) {
        for (inner = index + 1; inner < record_count; inner++) {
            if (records[inner].rssi > records[index].rssi) {
                wifi_ap_record_t swap = records[index];
                records[index] = records[inner];
                records[inner] = swap;
            }
        }
    }

    limit = record_count;
    if (limit > P4_CONFIG_WIFI_SCAN_LIMIT) {
        limit = P4_CONFIG_WIFI_SCAN_LIMIT;
    }

    if (bare) {
        /* Machine-readable form: bare SSID lines, no colour. Redirectable
         * output that a batch file can loop over with `for /f`. */
        for (index = 0; index < limit; index++) {
            networking_appendf("%s\n", records[index].ssid[0] != '\0'
                                            ? (const char *)records[index].ssid
                                            : "<hidden>");
        }
        networking_record_infof("Scan completed with %u APs (bare)", (unsigned int)record_count);
        free(records);
        return;
    }

    networking_appendf(SH_HEAD "  %-32s %8s %4s %s" SH_RST "\n",
                       "SSID", "RSSI", "CH", "AUTH");
    for (index = 0; index < limit; index++) {
        const char *ssid = records[index].ssid[0] != '\0'
                               ? (const char *)records[index].ssid
                               : "<hidden>";
        /* Colours are applied around the width-specified field, so the
         * escape bytes never shift the column alignment. */
        networking_appendf("  " SH_VAL "%-32s" SH_RST " " SH_NUM "%8d" SH_RST " " SH_NUM "%4u" SH_RST " " SH_VAL "%s" SH_RST "\n",
                           ssid,
                           records[index].rssi,
                           (unsigned int)records[index].primary,
                           networking_wifi_auth_label(records[index].authmode));
    }
    networking_appendf(SH_MUTE "wifi.scan:" SH_RST " %u AP(s) shown (%u found, cap %u)\n",
                       (unsigned int)limit, (unsigned int)record_count, (unsigned int)P4_CONFIG_WIFI_SCAN_LIMIT);
    networking_record_infof("Scan completed with %u APs", (unsigned int)record_count);
    free(records);
#endif
}

/**
 * Read the DNS server list currently configured on the STA netif and join the
 * configured IPv4 servers into a single space-separated string for `wifi status`.
 */
static size_t networking_wifi_format_dns_servers(char *out, size_t out_size)
{
    esp_netif_dns_info_t dns_info;
    size_t count = 0;
    size_t written = 0;
    int index;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (s_wifi_sta_netif == NULL) {
        return 0;
    }

    for (index = 0; index < ESP_NETIF_DNS_MAX; index++) {
        if (esp_netif_get_dns_info(s_wifi_sta_netif, (esp_netif_dns_type_t)index, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4 && dns_info.ip.u_addr.ip4.addr != 0) {
            size_t need = (size_t)snprintf(out + written, out_size - written,
                                           "%s" IPSTR, count > 0 ? " " : "",
                                           IP2STR(&dns_info.ip.u_addr.ip4));
            if (need >= out_size - written) {
                break;
            }
            written += need;
            count++;
        }
    }

    return count;
}

void networking_wifi_status(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    char bssid_str[18];
    char phy_str[24];
    char dns_str[96];
    char uptime_str[24];
    wifi_bandwidth_t bandwidth = WIFI_BW_HT20;
    int64_t associated_us;

    networking_appendf(SH_HEAD "Wi-Fi Status" SH_RST "\n");
    networking_appendf("  " SH_LBL "state:" SH_RST " %s\n", networking_wifi_state_string_internal());

    if (networking_wifi_defaults_available()) {
        networking_appendf("  " SH_LBL "default profile:" SH_RST " " SH_OK "configured" SH_RST "\n");
        networking_appendf("  " SH_LBL "default SSID:" SH_RST " " SH_VAL "%s" SH_RST "\n",
                           CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
    } else {
        networking_appendf("  " SH_LBL "default profile:" SH_RST " " SH_WARN "missing" SH_RST "\n");
    }

    if (s_wifi_last_detail[0] != '\0') {
        networking_appendf("  " SH_LBL "note:" SH_RST " " SH_MUTE "%s" SH_RST "\n", s_wifi_last_detail);
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        if (s_wifi_state == NETWORKING_WIFI_STATE_FAILED) {
            networking_appendf("  " SH_LBL "last error:" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                               esp_err_to_name(s_wifi_last_error), (unsigned int)s_wifi_last_error);
        }
        if (s_wifi_state == NETWORKING_WIFI_STATE_STARTING) {
            networking_appendf("  " SH_LBL "progress:" SH_RST " " SH_WARN "ESP-Hosted probe is still running" SH_RST "\n");
        }
        return;
    }

    /* Colours must be literal in the format string for ansi_vformat to
     * convert them — @-specifiers passed as %s arguments are not converted.
     * Use a conditional format string for the value-dependent colour. */
    if (s_wifi_connected) {
        networking_appendf("  " SH_LBL "connected:" SH_RST " " SH_OK "yes" SH_RST "\n");
    } else {
        networking_appendf("  " SH_LBL "connected:" SH_RST " " SH_MUTE "no" SH_RST "\n");
    }

    if (s_wifi_connect_requested) {
        networking_appendf("  " SH_LBL "connect requested:" SH_RST " " SH_OK "yes" SH_RST "\n");
    }
    if (s_wifi_target_ssid[0] != '\0') {
        networking_appendf("  " SH_LBL "target SSID:" SH_RST " " SH_VAL "%s" SH_RST "\n", s_wifi_target_ssid);
    }

    memset(&ap_info, 0, sizeof(ap_info));
    error = esp_wifi_sta_get_ap_info(&ap_info);
    if (error == ESP_OK && ap_info.ssid[0] != '\0') {
        networking_appendf("  " SH_LBL "SSID:" SH_RST " " SH_VAL "%s" SH_RST "\n", (const char *)ap_info.ssid);
        networking_wifi_format_bssid(ap_info.bssid, bssid_str, sizeof(bssid_str));
        networking_appendf("  " SH_LBL "BSSID:" SH_RST " " SH_VAL "%s" SH_RST "\n", bssid_str);
        networking_appendf("  " SH_LBL "channel:" SH_RST " " SH_NUM "%u" SH_RST "\n", (unsigned int)ap_info.primary);
        networking_appendf("  " SH_LBL "RSSI:" SH_RST " " SH_NUM "%d" SH_RST " " SH_MUTE "dBm" SH_RST "\n", ap_info.rssi);
        if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth) == ESP_OK) {
            const char *bw = (bandwidth == WIFI_BW_HT40) ? "HT40"
                             : (bandwidth == WIFI_BW_HT20) ? "HT20"
                             : "20MHz";
            networking_wifi_format_phy(&ap_info, bw, phy_str, sizeof(phy_str));
            networking_appendf("  " SH_LBL "PHY:" SH_RST " " SH_VAL "%s" SH_RST "\n", phy_str);
        }
    } else if (error != ESP_ERR_WIFI_NOT_CONNECT) {
        networking_appendf("  " SH_LBL "AP info error:" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                           esp_err_to_name(error), (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        networking_appendf("  " SH_LBL "IPv4:" SH_RST " " SH_VAL IPSTR SH_RST "\n", IP2STR(&ip_info.ip));
        networking_appendf("  " SH_LBL "netmask:" SH_RST " " SH_VAL IPSTR SH_RST "\n", IP2STR(&ip_info.netmask));
        networking_appendf("  " SH_LBL "gateway:" SH_RST " " SH_VAL IPSTR SH_RST "\n", IP2STR(&ip_info.gw));
        if (networking_wifi_format_dns_servers(dns_str, sizeof(dns_str)) > 0) {
            networking_appendf("  " SH_LBL "DNS:" SH_RST " " SH_VAL "%s" SH_RST "\n", dns_str);
        }
    }

    wifi_lock();
    associated_us = s_wifi_associated_at_us;
    wifi_unlock();
    if (associated_us > 0 && s_wifi_connected) {
        uint32_t seconds = (uint32_t)((esp_timer_get_time() - associated_us) / 1000000);
        uint32_t hours = seconds / 3600;
        uint32_t mins = (seconds % 3600) / 60;
        uint32_t secs = seconds % 60;
        snprintf(uptime_str, sizeof(uptime_str), "%02u:%02u:%02u", (unsigned int)hours, (unsigned int)mins, (unsigned int)secs);
        networking_appendf("  " SH_LBL "association uptime:" SH_RST " " SH_NUM "%s" SH_RST "\n", uptime_str);
    }
#else
    networking_appendf("@Cwifi:@R @Ksdkconfig does not enable the Wi-Fi stack@R\n");
#endif
}

void networking_wifi_disconnect(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;

    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTING) {
        networking_appendf("@Cwifi:@R @yinitialization is in progress@R\n");
        return;
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_appendf("@Cwifi:@R @ystack is not ready@R (@K%s@R)\n", networking_wifi_state_string_internal());
        return;
    }

    s_wifi_connect_requested = false;
    s_wifi_connected = false;
    networking_wifi_append_step("esp_wifi_disconnect()");
    error = esp_wifi_disconnect();
    if (error == ESP_OK || error == ESP_ERR_WIFI_NOT_CONNECT) {
        networking_appendf("@Cwifi:@R @gdisconnect requested@R\n");
        return;
    }

    networking_appendf("@Cwifi:@R @rdisconnect failed@R with @r%s@R (0x%x)\n", esp_err_to_name(error), (unsigned int)error);
#endif
}

/* ========================================================================
 * PING AND DNS
 * ========================================================================
 * Classic DOS-style connectivity commands. Both live here — the single owner
 * of the lwIP / esp_ping surface — and both produce transcript output, so
 * redirection and pipes capture them like any other command. The return
 * value is mapped onto ERRORLEVEL by the dispatcher in components/command.
 */

/** Shared state between the esp_ping callbacks (ping task) and the caller. */
typedef struct {
    SemaphoreHandle_t done;       /**< Given once by the on_ping_end callback. */
    esp_ping_handle_t handle;     /**< Ping session handle for profile reads. */
    ip_addr_t target;             /**< Resolved target address (IPv4). */
    char target_str[48];          /**< Dotted IPv4 text of the target. */
    volatile bool any_reply;      /**< True once at least one echo reply landed. */
    uint32_t replies;             /**< Reply count (for the min/avg/max stats). */
    uint32_t min_rtt_ms;          /**< Smallest round-trip time seen. */
    uint32_t max_rtt_ms;          /**< Largest round-trip time seen. */
    uint32_t total_rtt_ms;        /**< Sum of round-trip times (for the average). */
} networking_ping_ctx_t;

static void networking_ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    networking_ping_ctx_t *ctx = (networking_ping_ctx_t *)args;
    uint32_t rtt_ms = 0;
    uint32_t seq = 0;
    uint32_t ttl = 0;
    uint32_t size = 0;

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt_ms, sizeof(rtt_ms));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));

    ctx->any_reply = true;
    if (ctx->replies == 0) {
        ctx->min_rtt_ms = rtt_ms;
        ctx->max_rtt_ms = rtt_ms;
    } else {
        if (rtt_ms < ctx->min_rtt_ms) {
            ctx->min_rtt_ms = rtt_ms;
        }
        if (rtt_ms > ctx->max_rtt_ms) {
            ctx->max_rtt_ms = rtt_ms;
        }
    }
    ctx->total_rtt_ms += rtt_ms;
    ctx->replies++;

    networking_appendf("Reply from " SH_VAL "%s" SH_RST ": " SH_LBL "bytes" SH_RST "=" SH_NUM "%" PRIu32 SH_RST
                       " " SH_LBL "time" SH_RST "=" SH_NUM "%" PRIu32 "ms" SH_RST
                       " " SH_LBL "TTL" SH_RST "=" SH_NUM "%" PRIu32 SH_RST "\n",
                       ctx->target_str, size, rtt_ms, ttl);
    (void)seq;
}

static void networking_ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    networking_ping_ctx_t *ctx = (networking_ping_ctx_t *)args;

    (void)hdl;
    networking_appendf("Request to " SH_VAL "%s" SH_RST " " SH_WARN "timed out" SH_RST ".\n", ctx->target_str);
}

static void networking_ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    networking_ping_ctx_t *ctx = (networking_ping_ctx_t *)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t loss_pct = 0;

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    if (transmitted > 0) {
        loss_pct = ((transmitted - received) * 100) / transmitted;
    }

    networking_appendf("\n--- " SH_VAL "%s" SH_RST " ping statistics ---\n", ctx->target_str);
    networking_appendf("    " SH_LBL "Packets:" SH_RST " " SH_LBL "Sent" SH_RST " = " SH_NUM "%" PRIu32 SH_RST
                       ", " SH_LBL "Received" SH_RST " = " SH_NUM "%" PRIu32 SH_RST
                       ", " SH_LBL "Lost" SH_RST " = " SH_NUM "%" PRIu32 SH_RST
                       " (" SH_NUM "%" PRIu32 "%%" SH_RST " loss),\n",
                       transmitted, received, transmitted - received, loss_pct);
    networking_appendf(SH_LBL "Approximate round trip times in milli-seconds:" SH_RST "\n");
    if (ctx->replies > 0) {
        networking_appendf("    " SH_LBL "Minimum" SH_RST " = " SH_NUM "%" PRIu32 "ms" SH_RST
                           ", " SH_LBL "Maximum" SH_RST " = " SH_NUM "%" PRIu32 "ms" SH_RST
                           ", " SH_LBL "Average" SH_RST " = " SH_NUM "%" PRIu32 "ms" SH_RST "\n",
                           ctx->min_rtt_ms, ctx->max_rtt_ms, ctx->total_rtt_ms / ctx->replies);
    } else {
        networking_appendf("    " SH_LBL "Minimum" SH_RST " = 0ms, " SH_LBL "Maximum" SH_RST
                           " = 0ms, " SH_LBL "Average" SH_RST " = 0ms\n");
    }

    xSemaphoreGive(ctx->done);
}

esp_err_t networking_wifi_ping(const char *host, int count)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_ping_ctx_t ctx;
    esp_ping_config_t config;
    esp_ping_callbacks_t callbacks;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    uint32_t request_count;
    uint32_t wait_ms;
    esp_err_t error;

    if (host == NULL || host[0] == '\0') {
        networking_appendf(SH_ERR "ping:" SH_RST " missing host argument\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_appendf(SH_ERR "ping:" SH_RST " Wi-Fi is not started (" SH_MUTE "%s" SH_RST ")\n",
                           networking_wifi_state_string_internal());
        return ESP_ERR_INVALID_STATE;
    }
    if (!networking_wifi_is_connected()) {
        networking_appendf(SH_ERR "ping:" SH_RST " no active connection; run " SH_CMD "wifi connect" SH_RST " first\n");
        return ESP_ERR_INVALID_STATE;
    }

    request_count = (count > 0) ? (uint32_t)count : (uint32_t)P4_CONFIG_PING_COUNT_DEFAULT;
    if (request_count > (uint32_t)P4_CONFIG_PING_COUNT_MAX) {
        networking_appendf(SH_WARN "ping:" SH_RST " count clamped from " SH_NUM "%u" SH_RST " to " SH_NUM "%u" SH_RST "\n",
                           (unsigned int)request_count, (unsigned int)P4_CONFIG_PING_COUNT_MAX);
        request_count = (uint32_t)P4_CONFIG_PING_COUNT_MAX;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    /* Resolve a hostname or dotted IPv4 to a single A record. */
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL || res->ai_addr == NULL) {
        networking_appendf(SH_ERR "ping:" SH_RST " cannot resolve host " SH_VAL "%s" SH_RST "\n", host);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return ESP_ERR_NOT_FOUND;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.done = xSemaphoreCreateBinary();
    if (ctx.done == NULL) {
        freeaddrinfo(res);
        networking_appendf(SH_ERR "ping:" SH_RST " out of memory\n");
        return ESP_ERR_NO_MEM;
    }

    {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        ip4_addr_t ip4;

        inet_addr_to_ip4addr(&ip4, &sin->sin_addr);
        ip_addr_copy_from_ip4(ctx.target, ip4);
        snprintf(ctx.target_str, sizeof(ctx.target_str), IPSTR, IP2STR(&ip4));
    }
    freeaddrinfo(res);

    config = (esp_ping_config_t)ESP_PING_DEFAULT_CONFIG();
    config.count = request_count;
    config.interval_ms = P4_CONFIG_PING_INTERVAL_MS;
    config.timeout_ms = P4_CONFIG_PING_TIMEOUT_MS;
    config.data_size = P4_CONFIG_PING_DATA_BYTES;
    config.target_addr = ctx.target;
    /* The default ping task stack is small and overflows once the ping
     * callbacks printf() (which recurses through newlib lock/malloc) and run
     * the transcript/ANSI formatting path. Give it real headroom so `ping`
     * works reliably on this build. */
    config.task_stack_size = 8192;

    callbacks.cb_args = &ctx;
    callbacks.on_ping_success = networking_ping_success_cb;
    callbacks.on_ping_timeout = networking_ping_timeout_cb;
    callbacks.on_ping_end = networking_ping_end_cb;

    error = esp_ping_new_session(&config, &callbacks, &ctx.handle);
    if (error != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        networking_appendf(SH_ERR "ping:" SH_RST " session creation failed (" SH_ERR "%s" SH_RST ")\n",
                           esp_err_to_name(error));
        return error;
    }

    error = esp_ping_start(ctx.handle);
    if (error != ESP_OK) {
        (void)esp_ping_delete_session(ctx.handle);
        vSemaphoreDelete(ctx.done);
        networking_appendf(SH_ERR "ping:" SH_RST " failed to start (" SH_ERR "%s" SH_RST ")\n",
                           esp_err_to_name(error));
        return error;
    }

    /* Block the worker task only for a strictly bounded budget: each request
     * can take at most timeout_ms (the raw-socket receive timeout) followed by
     * interval_ms of spacing, so count * (timeout + interval) plus margin is a
     * hard ceiling. A ping is supposed to block the shell, like DOS. */
    wait_ms = request_count * (P4_CONFIG_PING_TIMEOUT_MS + P4_CONFIG_PING_INTERVAL_MS) + 1000;
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        networking_appendf(SH_WARN "ping:" SH_RST " session exceeded its time budget, stopping\n");
        (void)esp_ping_stop(ctx.handle);
        /* Let the ping task reach its on_ping_end callback so ctx is no
         * longer referenced before we return. */
        (void)xSemaphoreTake(ctx.done, pdMS_TO_TICKS(P4_CONFIG_PING_TIMEOUT_MS + 1000));
        (void)esp_ping_delete_session(ctx.handle);
        vSemaphoreDelete(ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    (void)esp_ping_delete_session(ctx.handle);
    vSemaphoreDelete(ctx.done);
    return ctx.any_reply ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    (void)host;
    (void)count;
    networking_appendf(SH_ERR "ping:" SH_RST " the Wi-Fi stack is not enabled in sdkconfig\n");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t networking_wifi_dns_lookup(const char *hostname)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *cur;
    char ip_str[INET_ADDRSTRLEN];
    int count = 0;

    if (hostname == NULL || hostname[0] == '\0') {
        networking_appendf(SH_ERR "dns:" SH_RST " missing hostname argument\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_appendf(SH_ERR "dns:" SH_RST " Wi-Fi is not started (" SH_MUTE "%s" SH_RST ")\n",
                           networking_wifi_state_string_internal());
        return ESP_ERR_INVALID_STATE;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    networking_appendf(SH_PROMPT "dns:" SH_RST " resolving " SH_VAL "%s" SH_RST "...\n", hostname);
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || res == NULL) {
        networking_appendf(SH_ERR "dns:" SH_RST " could not resolve " SH_VAL "%s" SH_RST "\n", hostname);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return ESP_ERR_NOT_FOUND;
    }

    for (cur = res; cur != NULL && count < P4_CONFIG_DNS_RESULT_LIMIT; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET && cur->ai_addr != NULL) {
            struct sockaddr_in *sin = (struct sockaddr_in *)cur->ai_addr;

            if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str)) != NULL) {
                networking_appendf("  " SH_LBL "%s" SH_RST "  " SH_VAL "%s" SH_RST "\n",
                                   count == 0 ? "IPv4 A:" : "        ", ip_str);
                count++;
            }
        }
    }
    freeaddrinfo(res);

    if (count == 0) {
        networking_appendf(SH_ERR "dns:" SH_RST " no A records for " SH_VAL "%s" SH_RST "\n", hostname);
        return ESP_ERR_NOT_FOUND;
    }

    networking_appendf(SH_OK "dns:" SH_RST " " SH_VAL "%s" SH_RST " resolved to " SH_NUM "%d" SH_RST " address(es)\n",
                       hostname, count);
    return ESP_OK;
#else
    (void)hostname;
    networking_appendf(SH_ERR "dns:" SH_RST " the Wi-Fi stack is not enabled in sdkconfig\n");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ========================================================================
 * HTTP CLIENT (httpget / wget)
 * ========================================================================
 * A deliberately minimal HTTPS/HTTP GET built on the same esp_http_client
 * stack c6ota uses for firmware downloads — no new HTTP library, no POST, no
 * sockets outside this module. The response body is buffered in PSRAM (capped
 * by P4_CONFIG_HTTP_MAX_BODY_BYTES) and handed to the command layer to print
 * or save to SD. Errorlevel is decided by the caller from the return code.
 */

/** Read chunk size for the body loop. Kept modest so the esp_http_client
 *  transport buffer stays small while large bodies stream through PSRAM. */
#define HTTP_READ_CHUNK_BYTES               2048

static bool networking_http_url_is_supported(const char *url)
{
    return url != NULL &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

/**
 * Report whether a URL carries a `user:pass@` prefix before the path.
 *
 * The credentials are consumed by esp_http_client's userinfo parsing; this
 * gate decides whether the client should advertise Basic auth so the header
 * is actually sent.
 */
static bool networking_http_url_has_userinfo(const char *url)
{
    const char *authority;
    const char *slash;
    const char *at;

    if (url == NULL) {
        return false;
    }
    authority = strstr(url, "://");
    if (authority == NULL) {
        return false;
    }
    authority += 3;
    at = strchr(authority, '@');
    if (at == NULL) {
        return false;
    }
    slash = strchr(authority, '/');
    return slash == NULL || at < slash;
}

void networking_http_result_free(networking_http_result_t *result)
{
    if (result != NULL && result->body != NULL) {
        heap_caps_free(result->body);
        result->body = NULL;
        result->body_size = 0;
    }
}

static esp_err_t networking_http_get_internal(const char *url, networking_http_result_t *result, bool quiet)
{
    esp_http_client_config_t http_config = { 0 };
    esp_http_client_handle_t client = NULL;
    networking_http_result_t *out = result;
    esp_err_t error = ESP_FAIL;
    int64_t remote_length;
    int http_status;
    char *content_type = NULL;
    uint8_t probe;
    size_t total_read = 0;
    int chunk_read;

    if (url == NULL || url[0] == '\0' || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Zero the result on entry so every early-return path leaves a clean
     * struct whose body pointer is NULL — the caller always runs
     * networking_http_result_free() on the error path. */
    memset(out, 0, sizeof(*out));

    if (!networking_http_url_is_supported(url)) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " unsupported URL scheme; use http:// or https://\n");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!networking_wifi_is_connected()) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " no active connection; run " SH_CMD "wifi connect" SH_RST " first\n");
        }
        return ESP_ERR_INVALID_STATE;
    }

    http_config.url = url;
    /* A `user:pass@` prefix in the URL enables HTTP Basic auth; esp_http_client
     * parses the credentials from the userinfo and sends the header only when
     * auth_type selects it. */
    if (networking_http_url_has_userinfo(url)) {
        http_config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    http_config.timeout_ms = P4_CONFIG_HTTP_TIMEOUT_MS;
    http_config.buffer_size = HTTP_READ_CHUNK_BYTES;
    http_config.buffer_size_tx = 1024;
    http_config.user_agent = P4_CONFIG_HTTP_USER_AGENT;
    /* Follow redirects only when configured. disable_auto_redirect is the
     * master switch; max_redirection_count bounds the chain. */
    http_config.disable_auto_redirect = !P4_CONFIG_HTTP_FOLLOW_REDIRECTS;
    http_config.max_redirection_count = P4_CONFIG_HTTP_FOLLOW_REDIRECTS ? 3 : 0;

    if (strncmp(url, "https://", 8) == 0) {
        http_config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    if (!quiet) {
        networking_appendf(SH_PROMPT "httpget:" SH_RST " downloading " SH_VAL "%s" SH_RST "\n", url);
    }

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " failed to allocate the HTTP client\n");
        }
        return ESP_ERR_NO_MEM;
    }

    error = esp_http_client_open(client, 0);
    if (error != ESP_OK) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " connection failed (" SH_ERR "%s" SH_RST
                               ") - check the URL or Wi-Fi routing\n", esp_err_to_name(error));
        }
        goto cleanup;
    }

    remote_length = esp_http_client_fetch_headers(client);
    http_status = esp_http_client_get_status_code(client);
    if (http_status < 200 || http_status >= 300) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " server returned HTTP status " SH_NUM "%d" SH_RST "\n",
                               http_status);
        }
        error = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK &&
        content_type != NULL && content_type[0] != '\0') {
        snprintf(out->content_type, sizeof(out->content_type), "%s", content_type);
    }
    if (remote_length > 0) {
        out->content_length = (size_t)remote_length;
    }
    if (out->content_length > P4_CONFIG_HTTP_MAX_BODY_BYTES) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " response of " SH_NUM "%u" SH_RST
                               " bytes exceeds the " SH_NUM "%u" SH_RST " byte limit\n",
                               (unsigned int)out->content_length, (unsigned int)P4_CONFIG_HTTP_MAX_BODY_BYTES);
        }
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    /* Large response buffer from PSRAM first, internal RAM as a fallback. */
    out->body = heap_caps_malloc(P4_CONFIG_HTTP_MAX_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out->body == NULL) {
        out->body = heap_caps_malloc(P4_CONFIG_HTTP_MAX_BODY_BYTES, MALLOC_CAP_8BIT);
    }
    if (out->body == NULL) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " out of memory buffering the response\n");
        }
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (total_read < P4_CONFIG_HTTP_MAX_BODY_BYTES) {
        size_t wanted = P4_CONFIG_HTTP_MAX_BODY_BYTES - total_read;

        if (wanted > HTTP_READ_CHUNK_BYTES) {
            wanted = HTTP_READ_CHUNK_BYTES;
        }
        chunk_read = esp_http_client_read(client, (char *)(out->body + total_read), (int)wanted);
        if (chunk_read < 0) {
            if (!quiet) {
                networking_appendf(SH_ERR "httpget:" SH_RST " read failed mid-response\n");
            }
            error = ESP_FAIL;
            goto cleanup;
        }
        if (chunk_read == 0) {
            break;
        }
        total_read += (size_t)chunk_read;
    }
    out->body_size = total_read;

    /* If we filled the cap exactly, probe once more to tell a truncated body
     * from a body that ends right at the limit. */
    if (total_read == P4_CONFIG_HTTP_MAX_BODY_BYTES &&
        esp_http_client_read(client, (char *)&probe, 1) > 0) {
        if (!quiet) {
            networking_appendf(SH_ERR "httpget:" SH_RST " response exceeds the " SH_NUM "%u" SH_RST " byte limit\n",
                               (unsigned int)P4_CONFIG_HTTP_MAX_BODY_BYTES);
        }
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (!quiet) {
        networking_appendf(SH_OK "httpget:" SH_RST " " SH_LBL "status" SH_RST "=" SH_NUM "%d" SH_RST
                           " " SH_LBL "type" SH_RST "=" SH_VAL "%s" SH_RST
                           " " SH_LBL "bytes" SH_RST "=" SH_NUM "%u" SH_RST "\n",
                           http_status,
                           out->content_type[0] != '\0' ? out->content_type : "n/a",
                           (unsigned int)total_read);
    }
    error = ESP_OK;

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (error != ESP_OK && out->body != NULL) {
        heap_caps_free(out->body);
        out->body = NULL;
        out->body_size = 0;
    }
    return error;
}

esp_err_t networking_http_get(const char *url, networking_http_result_t *result)
{
    return networking_http_get_internal(url, result, false);
}

/** Skip leading spaces/tabs and a UTF-8 BOM. */
static const char *networking_skip_ws(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s += 3;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r') {
        s++;
    }
    return s;
}

esp_err_t networking_time_detect(int *offset_seconds_out, char *iana_out, size_t iana_size)
{
    static const char k_url[] = P4_CONFIG_TIMEZONE_URL;
    networking_http_result_t res;
    char *body;
    char line[64];
    size_t line_len = 0;
    int parsed_offset = 0;
    bool have_offset = false;
    size_t i;

    if (offset_seconds_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (networking_http_get_internal(k_url, &res, true) != ESP_OK) {
        return ESP_FAIL;
    }

    /* The /line/ endpoint emits one value per line (timezone name, then the
     * UTC offset in seconds). Field order is not guaranteed, so classify each
     * non-empty line: a leading sign/digit is the offset, otherwise the name. */
    body = (char *)res.body;
    for (i = 0; i <= res.body_size; i++) {
        char c = (i < res.body_size) ? body[i] : '\n';

        if (c != '\n' && c != '\r') {
            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
            continue;
        }
        line[line_len] = '\0';
        line_len = 0;

        {
            const char *v = networking_skip_ws(line);
            if (v == NULL || v[0] == '\0') {
                continue;
            }
            if ((v[0] == '-' || v[0] == '+' || (v[0] >= '0' && v[0] <= '9'))) {
                char *end = NULL;
                long seconds = strtol(v, &end, 10);
                if (end != NULL && *networking_skip_ws(end) == '\0') {
                    /* Plausible UTC offset: -12h .. +14h. */
                    if (seconds >= -12 * 3600 && seconds <= 14 * 3600) {
                        parsed_offset = (int)seconds;
                        have_offset = true;
                    }
                }
            } else if (iana_out != NULL && iana_size > 0) {
                snprintf(iana_out, iana_size, "%s", v);
            }
        }
    }

    networking_http_result_free(&res);

    if (!have_offset) {
        return ESP_ERR_NOT_FOUND;
    }
    *offset_seconds_out = parsed_offset;
    return ESP_OK;
}

void networking_wifi_set_boot_credentials(const char *ssid, const char *password)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    char masked[NETWORKING_WIFI_SSID_BYTES + 16];
    char ssid_copy[NETWORKING_WIFI_SSID_BYTES];
    char pass_copy[NETWORKING_WIFI_PASSWORD_BYTES];

    if (ssid == NULL) {
        ssid = "";
    }
    if (password == NULL) {
        password = "";
    }

    /* The CONFIG.SYS parser calls this once for WIFI_SSID= and once for
     * WIFI_PASSWORD=. Update the field that was actually provided and keep the
     * other, so the pair is assembled across the two directives. An empty ssid
     * with an empty password clears the whole target. */
    wifi_lock();
    if (ssid[0] != '\0') {
        snprintf(s_wifi_target_ssid, sizeof(s_wifi_target_ssid), "%s", ssid);
    }
    if (password[0] != '\0') {
        snprintf(s_wifi_target_password, sizeof(s_wifi_target_password), "%s", password);
    }
    if (ssid[0] == '\0' && password[0] == '\0') {
        s_wifi_target_ssid[0] = '\0';
        s_wifi_target_password[0] = '\0';
    }
    snprintf(ssid_copy, sizeof(ssid_copy), "%s", s_wifi_target_ssid);
    snprintf(pass_copy, sizeof(pass_copy), "%s", s_wifi_target_password);
    if (ssid_copy[0] != '\0') {
        s_wifi_connect_requested = true;
        s_wifi_connected = false;
        snprintf(masked, sizeof(masked), "%s ********", ssid_copy);
        networking_schedulef_ansi("@C[wifi]@R @Gboot credentials set@R for @W%s@R\n", masked);
    } else {
        s_wifi_connect_requested = false;
        networking_schedulef_ansi("@C[wifi]@R @Kboot target cleared@R\n");
    }
    wifi_unlock();

    /* Seed the persistent known-network list from CONFIG.SYS when a target
     * SSID is configured and the SD card is present. Fully safe if absent. */
    if (ssid_copy[0] != '\0' && P4_CONFIG_WIFI_KNOWN_AUTOSAVE) {
        (void)networking_wifi_known_upsert(ssid_copy, pass_copy, -1, false);
    }
#endif
}

void networking_wifi_set_boot_autoconnect(bool enabled)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    wifi_lock();
    s_wifi_boot_autoconnect = enabled;
    wifi_unlock();
    networking_schedulef_ansi("@C[wifi]@R @Gboot autoconnect policy@R set to @W%s@R\n", enabled ? "ON" : "OFF");
#endif
}

bool networking_wifi_get_boot_autoconnect(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    bool enabled;

    wifi_lock();
    enabled = s_wifi_boot_autoconnect;
    wifi_unlock();
    return enabled;
#else
    return true;
#endif
}

void networking_wifi_diag(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t request = {
        .start_runtime = (s_wifi_state != NETWORKING_WIFI_STATE_STARTED),
        .connect_with_defaults = false,
        .run_diagnostic = true,
    };

    snprintf(request.origin, sizeof(request.origin), "%s", "diag");
    (void)networking_wifi_begin_background_request(&request);
#endif
}

/**
 * `wifi throughput tx <host> [port=N] [mb=N] [udp]`
 * `wifi throughput rx [port=N] [mb=N] [udp]`
 *
 * Runs the netbench probe on the command worker (blocking) and prints the
 * measured rate. The host endpoint is `tools/wifi_bench.py`. Used to compare
 * the ESP-Hosted SDIO transport at the negotiated clock (see the 40 MHz trial
 * in changelog v0.38.0).
 */
static esp_err_t networking_wifi_throughput(int argc, char **argv)
{
    netbench_config_t cfg;
    netbench_result_t result;
    esp_err_t error;
    int i;

    if (argc < 3) {
        goto usage;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.port = P4_CONFIG_WIFI_BENCH_PORT;
    cfg.megabytes = P4_CONFIG_WIFI_BENCH_DEFAULT_MB;
    cfg.timeout_ms = P4_CONFIG_WIFI_BENCH_TIMEOUT_MS;

    if (networking_text_equals_ignore_case(argv[2], "tx")) {
        cfg.direction = NETBENCH_DIR_TX;
        if (argc < 4) {
            goto usage;
        }
        cfg.host = argv[3];
        i = 4;
    } else if (networking_text_equals_ignore_case(argv[2], "rx")) {
        cfg.direction = NETBENCH_DIR_RX;
        i = 3;
    } else {
        goto usage;
    }

    for (; i < argc; i++) {
        const char *a = argv[i];

        if (networking_text_equals_ignore_case(a, "udp")) {
            cfg.udp = true;
        } else if (strncasecmp(a, "port=", 5) == 0) {
            long v = strtol(a + 5, NULL, 10);

            if (v < 1 || v > 65535) {
                goto usage;
            }
            cfg.port = (uint16_t)v;
        } else if (strncasecmp(a, "mb=", 3) == 0) {
            long v = strtol(a + 3, NULL, 10);

            if (v < 1 || v > P4_CONFIG_WIFI_BENCH_MAX_MB) {
                goto usage;
            }
            cfg.megabytes = (uint32_t)v;
        } else {
            goto usage;
        }
    }

    if (!networking_wifi_is_connected()) {
        networking_appendf("@Cwifi:@R @yconnect to a network before running the throughput bench@R\n");
        return ESP_ERR_INVALID_STATE;
    }

    networking_appendf("@Cwifi throughput:@R %s%s %s%u MiB on port %u ...\n",
                       cfg.direction == NETBENCH_DIR_TX ? "tx " : "rx ",
                       cfg.udp ? "udp" : "tcp",
                       cfg.direction == NETBENCH_DIR_TX ? cfg.host : "",
                       (unsigned)cfg.megabytes, (unsigned)cfg.port);

    error = netbench_run(&cfg, &result);
    if (error == ESP_OK) {
        if (cfg.udp) {
            networking_appendf("@G  UDP: %.2f Mbit/s (%llu bytes, %u datagrams, %u ms)@R\n",
                               result.mbps, (unsigned long long)result.bytes,
                               (unsigned)result.datagrams, (unsigned)result.elapsed_ms);
        } else {
            networking_appendf("@G  TCP: %.2f Mbit/s (%llu bytes, %u ms)@R\n",
                               result.mbps, (unsigned long long)result.bytes,
                               (unsigned)result.elapsed_ms);
        }
    } else {
        networking_appendf("@R  bench failed: %s (%llu/%llu bytes in %u ms)@R\n",
                           esp_err_to_name(error),
                           (unsigned long long)result.bytes,
                           (unsigned long long)((uint64_t)cfg.megabytes * 1024u * 1024u),
                           (unsigned)result.elapsed_ms);
    }

    return error;

usage:
    networking_appendf("@yUsage: wifi throughput tx <host> [port=N] [mb=N] [udp]@R\n");
    networking_appendf("@y       wifi throughput rx [port=N] [mb=N] [udp]@R\n");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t networking_handle_wifi_command(char *command)
{
    char *argv[6];
    int argc = networking_split_args(command, argv, 6);

    if (argc <= 1 || networking_text_equals_ignore_case(argv[1], "help")) {
        networking_appendf("@Y@BWi-Fi Commands:@R\n");
        networking_appendf("  @Gwifi status@R                 Full association and IP report (SSID/BSSID/PHY/DNS)\n");
        networking_appendf("  @Gwifi scan@R                   Scan for nearby SSIDs, sorted by RSSI\n");
        networking_appendf("  @Gwifi scan /b@R                Bare scan output (names only, redirectable)\n");
        networking_appendf("  @Gwifi diag@R                   Run a diagnostic status + scan report in the transcript\n");
        networking_appendf("  @Gwifi throughput tx@R @T<host>@R [port=N] [mb=N] [udp]  Send bench, Mbit/s\n");
        networking_appendf("  @Gwifi throughput rx@R [port=N] [mb=N] [udp]              Receive bench, Mbit/s\n");
        networking_appendf("  @Gwifi connect@R                Connect using sdkconfig default credentials\n");
        networking_appendf("  @Gwifi connect@R @T<ssid>@R @T<pass>@R  Connect using runtime credentials\n");
        networking_appendf("  @Gwifi disconnect@R             Disconnect the current station session\n");
        networking_appendf("  @Gwifi known@R                  Show saved networks (SSIDs only; SD known-list)\n");
        networking_appendf("  @Gwifi save@R [@T<ssid>@R]      Save the connected (or given) network to the known-list\n");
        networking_appendf("  @Gwifi forget@R @T<ssid>@R      Forget one saved network (@Gforget all@R clears)\n");
        networking_appendf("  @Gwifi preferred@R @T<ssid>@R  Mark a saved network as preferred for auto-connect\n");
        networking_appendf("  @Gping@R @T<host>@R [@T<count>@R]         Classic ICMP ping (sets ERRORLEVEL)\n");
        networking_appendf("  @Gdns@R @T<hostname>@R            Resolve A records (alias: nslookup)\n");
        networking_appendf("  @KWi-Fi now starts in the background on normal boot and after successful c6ota restore@R\n");
        networking_appendf("  @Kwifi connect passwords are masked in transcript history and not stored in command recall@R\n");
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "status")) {
        networking_wifi_status();
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "scan")) {
        bool bare = false;

        /* Parse the optional `wifi scan [/b]` argument. A leading `/b`
         * selects the bare machine-readable form; the result cap always comes
         * from P4_CONFIG_WIFI_SCAN_LIMIT. */
        if (argc > 2) {
            if (argc == 3 && argv[2][0] == '/' &&
                (argv[2][1] == 'b' || argv[2][1] == 'B') &&
                argv[2][2] == '\0') {
                bare = true;
            } else {
                networking_appendf("@yUsage: wifi scan [/b]@R\n");
                return ESP_ERR_INVALID_ARG;
            }
        }
        networking_wifi_scan(bare);
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "diag")) {
        networking_wifi_diag();
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "throughput") ||
        networking_text_equals_ignore_case(argv[1], "bench")) {
        return networking_wifi_throughput(argc, argv);
    }

    if (networking_text_equals_ignore_case(argv[1], "disconnect")) {
        networking_wifi_disconnect();
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "connect")) {
        if (argc == 2) {
            if (!networking_wifi_defaults_available()) {
                networking_appendf("@Cwifi:@R @ysdkconfig default credentials are not configured@R\n");
                return ESP_ERR_INVALID_STATE;
            }

            if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
                (void)networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                               CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            } else {
                networking_appendf("@Cwifi:@R @Nstarting stack on demand@R\n");
                (void)networking_wifi_begin_connect_request(NULL, NULL, true);
            }
            return ESP_OK;
        }

        if (argc == 4) {
            if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
                (void)networking_wifi_connect_with_credentials(argv[2], argv[3]);
            } else {
                networking_appendf("@Cwifi:@R @Nstarting stack on demand@R\n");
                (void)networking_wifi_begin_connect_request(argv[2], argv[3], false);
            }
            return ESP_OK;
        }

        networking_appendf("@yUsage: wifi connect or wifi connect <ssid> <pass>@R\n");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Persistent known-network list management ----
     * `wifi known`, `wifi save [ssid]`, `wifi forget <ssid>|all`,
     * `wifi clear known`, `wifi preferred <ssid>`. Each sets ERRORLEVEL via
     * the returned esp_err_t: ESP_OK = success, ESP_ERR_NOT_FOUND = SD
     * known-list unavailable, ESP_ERR_INVALID_ARG = usage, other = I/O error. */

    if (networking_text_equals_ignore_case(argv[1], "known") ||
        (networking_text_equals_ignore_case(argv[1], "list") &&
         argc >= 3 && networking_text_equals_ignore_case(argv[2], "known"))) {
        return networking_wifi_known_list();
    }

    if (networking_text_equals_ignore_case(argv[1], "save")) {
        char ssid[NETWORKING_WIFI_SSID_BYTES];
        char password[NETWORKING_WIFI_PASSWORD_BYTES];
        int authmode = -1;
        esp_err_t save_error;
        wifi_ap_record_t ap_info;

        ssid[0] = '\0';
        password[0] = '\0';

        if (argc >= 3) {
            snprintf(ssid, sizeof(ssid), "%s", argv[2]);
        } else {
            /* No argument: use the current connect target / connected SSID. */
            wifi_lock();
            if (s_wifi_target_ssid[0] != '\0') {
                snprintf(ssid, sizeof(ssid), "%s", s_wifi_target_ssid);
                snprintf(password, sizeof(password), "%s", s_wifi_target_password);
            }
            wifi_unlock();
        }

        if (ssid[0] == '\0') {
            networking_appendf("@Cwifi:@R @yUsage: wifi save [ssid]@R (no active target to save)\n");
            return ESP_ERR_INVALID_ARG;
        }

        /* Password: current target password if the SSID matches, else the
         * existing known entry's password, else empty (open network). */
        if (password[0] == '\0') {
            wifi_lock();
            if (networking_text_equals_ignore_case(ssid, s_wifi_target_ssid)) {
                snprintf(password, sizeof(password), "%s", s_wifi_target_password);
            }
            wifi_unlock();
        }
        if (password[0] == '\0') {
            networking_wifi_known_entry_t entry;
            int index;
            /* Load so existing entries are visible (and preserved on save). */
            (void)networking_wifi_known_load();
            for (index = 0; index < networking_wifi_known_count(); index++) {
                if (networking_wifi_known_get(index, &entry) &&
                    networking_text_equals_ignore_case(entry.ssid, ssid)) {
                    snprintf(password, sizeof(password), "%s", entry.password);
                    break;
                }
            }
        }

        memset(&ap_info, 0, sizeof(ap_info));
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            authmode = (int)ap_info.authmode;
        }

        save_error = networking_wifi_known_upsert(ssid, password, authmode, false);
        if (save_error != ESP_OK) {
            networking_appendf("@Cwifi:@R @yknown-list unavailable@R (no SD card or write failed)\n");
            return ESP_ERR_NOT_FOUND;
        }
        networking_appendf("@Cwifi:@R @Gsaved@R @W%s@R to the known-list\n", ssid);
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "forget") ||
        networking_text_equals_ignore_case(argv[1], "delete")) {
        if (argc < 3) {
            networking_appendf("@yUsage: wifi forget <ssid> | wifi forget all@R\n");
            return ESP_ERR_INVALID_ARG;
        }
        if (networking_text_equals_ignore_case(argv[2], "all")) {
            esp_err_t clear_error = networking_wifi_known_clear();
            if (clear_error != ESP_OK) {
                networking_appendf("@Cwifi:@R @yknown-list unavailable@R (no SD card or write failed)\n");
                return ESP_ERR_NOT_FOUND;
            }
            networking_appendf("@Cwifi:@R @Gknown-list cleared@R\n");
            return ESP_OK;
        }
        {
            esp_err_t remove_error = networking_wifi_known_remove(argv[2]);
            if (remove_error != ESP_OK) {
                networking_appendf("@Cwifi:@R @yknown-list unavailable@R (no SD card or write failed)\n");
                return ESP_ERR_NOT_FOUND;
            }
            networking_appendf("@Cwifi:@R @Gforgot@R @W%s@R\n", argv[2]);
            return ESP_OK;
        }
    }

    if (networking_text_equals_ignore_case(argv[1], "clear") &&
        argc >= 3 && networking_text_equals_ignore_case(argv[2], "known")) {
        esp_err_t clear_error = networking_wifi_known_clear();
        if (clear_error != ESP_OK) {
            networking_appendf("@Cwifi:@R @yknown-list unavailable@R (no SD card or write failed)\n");
            return ESP_ERR_NOT_FOUND;
        }
        networking_appendf("@Cwifi:@R @Gknown-list cleared@R\n");
        return ESP_OK;
    }

    if (networking_text_equals_ignore_case(argv[1], "preferred")) {
        if (argc < 3) {
            networking_appendf("@yUsage: wifi preferred <ssid>@R\n");
            return ESP_ERR_INVALID_ARG;
        }
        {
            esp_err_t pref_error = networking_wifi_known_set_preferred(argv[2], true);
            if (pref_error == ESP_ERR_NOT_FOUND) {
                networking_appendf("@Cwifi:@R @y%s is not in the known-list; use wifi save first@R\n", argv[2]);
                return ESP_ERR_NOT_FOUND;
            }
            if (pref_error != ESP_OK) {
                networking_appendf("@Cwifi:@R @yknown-list unavailable@R (no SD card or write failed)\n");
                return ESP_ERR_NOT_FOUND;
            }
            networking_appendf("@Cwifi:@R @Gmarked@R @W%s@R @Gpreferred@R\n", argv[2]);
            return ESP_OK;
        }
    }

    networking_appendf("@rUnknown wifi subcommand:@R %s\n", argv[1]);
    return ESP_ERR_INVALID_ARG;
}

void networking_append_sysinfo_summary(void)
{
    switch (s_wifi_state) {
    case NETWORKING_WIFI_STATE_STARTING:
        networking_appendf("@Cwifi:@R @yruntime initialization is in progress@R\n");
        break;
    case NETWORKING_WIFI_STATE_STARTED:
        if (s_wifi_connected) {
            networking_appendf("@Cwifi:@R @Gruntime initialized@R in STA mode from sdkconfig, @Cconnected@R=@Gyes@R\n");
        } else {
            networking_appendf("@Cwifi:@R @Gruntime initialized@R in STA mode from sdkconfig, @Cconnected@R=@Kno@R\n");
        }
        break;
    case NETWORKING_WIFI_STATE_FAILED:
        networking_appendf("@Cwifi:@R @rruntime initialization failed@R with @r%s@R (0x%x)\n",
                           esp_err_to_name(s_wifi_last_error),
                           (unsigned int)s_wifi_last_error);
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_DISABLED:
        networking_appendf("@Cwifi:@R @Kskipped because sdkconfig does not enable native or ESP-Hosted Wi-Fi@R\n");
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED:
        networking_appendf("@Cwifi:@R @Kunsupported on current target/SoC caps@R\n");
        break;
    case NETWORKING_WIFI_STATE_NOT_ATTEMPTED:
    default:
        networking_appendf("@Cwifi:@R @Kruntime initialization not attempted yet@R\n");
        break;
    }
}

const char *networking_wifi_state_string(void)
{
    return networking_wifi_state_string_internal();
}

networking_wifi_state_t networking_wifi_state(void)
{
    return s_wifi_state;
}

esp_err_t networking_wifi_last_error(void)
{
    return s_wifi_last_error;
}

bool networking_wifi_is_connected(void)
{
    bool connected;
    wifi_lock();
    connected = s_wifi_connected;
    wifi_unlock();
    return connected;
}

bool networking_wifi_get_rssi(int *rssi_out)
{
    wifi_ap_record_t ap_info;

    if (rssi_out == NULL) {
        return false;
    }

    /* Only query the driver while associated. Calling esp_wifi_sta_get_ap_info()
     * when disconnected returns an error and, on the hosted path, costs a
     * needless SDIO round trip on every header refresh. */
    if (!networking_wifi_is_connected()) {
        return false;
    }

    memset(&ap_info, 0, sizeof(ap_info));
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    *rssi_out = ap_info.rssi;
    return true;
}

bool networking_wifi_is_starting(void)
{
    bool starting;
    wifi_lock();
    starting = (s_wifi_state == NETWORKING_WIFI_STATE_STARTING);
    wifi_unlock();
    return starting;
}

static void networking_wifi_runtime_init(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
        return;
    }

    s_wifi_state = NETWORKING_WIFI_STATE_STARTING;
    s_wifi_last_error = ESP_OK;
    s_wifi_last_detail[0] = '\0';

    networking_wifi_append_step("queued background boot workflow");
    networking_wifi_append_step("runtime Wi-Fi initialization requested from sdkconfig");
    networking_wifi_append_step("ESP-Hosted SDIO backend: ESP32-C6 on CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");

    /* ---- Step 1: bring up the hosted transport ----
     * The SDIO link to the C6 comes up first. Everything below depends on it:
     * esp_wifi_init() is the esp_wifi_remote shim, and it has nothing to talk
     * to until the slave is connected. Bringing the transport up before NVS
     * also means a dead or mismatched co-processor is reported as a transport
     * fault rather than surfacing later as a confusing Wi-Fi init error. */
    networking_wifi_append_step("esp_hosted_init()");
    error = esp_hosted_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        networking_wifi_append_error("esp_hosted_init()", error);
        return;
    }

    networking_wifi_append_step("esp_hosted_connect_to_slave()");
    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        /* First bring-up attempt failed. Tear the hosted transport down and
         * retry once: a shared-SDMMC-bus glitch (N1) can leave the link wedged
         * on the first probe, and the second attempt recovers cleanly. */
        networking_wifi_append_step("retrying esp_hosted transport");
        esp_err_t deinit_error = esp_hosted_deinit();
        if (deinit_error != ESP_OK) {
            ESP_LOGW(NETWORKING_TAG, "hosted deinit before retry: %s (0x%x)",
                     esp_err_to_name(deinit_error), (unsigned int)deinit_error);
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        networking_wifi_append_step("esp_hosted_init() (retry)");
        error = esp_hosted_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            networking_wifi_append_error("esp_hosted_init() retry", error);
            return;
        }

        networking_wifi_append_step("esp_hosted_connect_to_slave() (retry)");
        error = esp_hosted_connect_to_slave();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            networking_wifi_append_error("esp_hosted_connect_to_slave() retry", error);
            return;
        }
    }

    /* ---- Step 2: version compatibility gate ----
     * Refuse to go any further when the C6 firmware does not match the host's
     * expected range. Running esp_wifi_remote against an incompatible slave
     * produces failures that are much harder to diagnose than this message. */
    networking_wifi_append_step("esp_hosted_get_coprocessor_fwversion()");
    error = networking_wifi_validate_hosted_version();
    if (error != ESP_OK) {
        s_wifi_state = NETWORKING_WIFI_STATE_FAILED;
        s_wifi_last_error = error;
        return;
    }

    /* ---- Step 3: NVS ----
     * esp_wifi_init() persists calibration and configuration to NVS, so the
     * partition must be mounted before it runs. Erase-and-retry recovers a
     * partition left in a bad state by a previous firmware version. */
    error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    if (error != ESP_OK) {
        networking_wifi_append_error("nvs_flash_init()", error);
        return;
    }
    networking_wifi_append_step("nvs_flash_init()");

    /* ---- Step 4: TCP/IP stack and event loop ---- */
    networking_wifi_append_step("esp_netif_init()");
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        networking_wifi_append_error("esp_netif_init()", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    networking_wifi_append_step("esp_event_loop_create_default()");
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        networking_wifi_append_error("esp_event_loop_create_default()", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }

    networking_wifi_append_step("esp_netif_create_default_wifi_sta()");
    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        networking_wifi_append_error("esp_netif_create_default_wifi_sta()", ESP_FAIL);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    networking_wifi_append_step("esp_wifi_init()");
    error = esp_wifi_init(&wifi_init_cfg);
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_init()", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }
    s_wifi_driver_inited = true;

    networking_wifi_append_step("esp_event_handler_instance_register(...)");
    error = networking_wifi_register_event_handlers();
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_event_handler_instance_register(...)", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    networking_wifi_append_step("esp_wifi_set_storage(WIFI_STORAGE_RAM)");
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_set_storage(WIFI_STORAGE_RAM)", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    networking_wifi_append_step("esp_wifi_set_mode(WIFI_MODE_STA)");
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_set_mode(WIFI_MODE_STA)", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    networking_wifi_append_step("esp_wifi_start()");
    error = esp_wifi_start();
    if (error != ESP_OK) {
        networking_wifi_append_error("esp_wifi_start()", error);
        networking_wifi_cleanup_runtime_artifacts();
        return;
    }

    s_wifi_state = NETWORKING_WIFI_STATE_STARTED;
    s_wifi_last_error = ESP_OK;
    s_wifi_connected = false;
    s_wifi_connect_requested = false;
    s_wifi_target_ssid[0] = '\0';
    networking_wifi_append_step("Wi-Fi runtime started in STA mode");
    if (networking_wifi_defaults_available()) {
        networking_schedulef_ansi("@C[wifi]@R @Gdefault sdkconfig profile ready@R for ssid @W%s@R\n", CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
    } else {
        networking_wifi_append_step("no default sdkconfig credentials configured; use wifi connect <ssid> <pass>");
    }
#elif SOC_WIRELESS_HOST_SUPPORTED
    s_wifi_state = NETWORKING_WIFI_STATE_SKIPPED_DISABLED;
    networking_wifi_append_step("skipped: sdkconfig does not enable native Wi-Fi or ESP-Hosted Wi-Fi");
    networking_wifi_append_step("expected CONFIG_ESP_WIFI_ENABLED, CONFIG_ESP_HOST_WIFI_ENABLED, or CONFIG_ESP_HOSTED from sdkconfig");
#else
    s_wifi_state = NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED;
    networking_wifi_append_step("skipped: current target does not expose Wi-Fi support");
#endif
}

void networking_init(const networking_host_ops_t *ops)
{
    if (ops != NULL) {
        s_host_ops = *ops;
    }

    /* Create the Wi-Fi state mutex for thread-safe access to shared state.
     * This protects s_wifi_state, s_wifi_connected, s_wifi_target_ssid, etc.
     * from concurrent access by the event handler, init task, and shell commands. */
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
    }

    bluetooth_init(&s_host_ops);
    networking_wifi_known_init(&s_host_ops);
    if (!s_networking_initialized) {
        s_networking_initialized = true;
        networking_wifi_request_boot_restore();
    }
}

const networking_host_ops_t *networking_get_host_ops(void)
{
    return &s_host_ops;
}

/* ========================================================================
 * WI-FI MUTEX HELPERS
 * ======================================================================== */

static void wifi_lock(void)
{
    if (s_wifi_mutex != NULL) {
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (s_wifi_mutex != NULL) {
        xSemaphoreGive(s_wifi_mutex);
    }
}

static bool wifi_try_claim_init_task(void)
{
    bool claimed = false;

    wifi_lock();
    if (!s_wifi_init_task_in_progress) {
        s_wifi_init_task_in_progress = true;
        claimed = true;
    }
    wifi_unlock();

    return claimed;
}

static void wifi_release_init_task(void)
{
    wifi_lock();
    s_wifi_init_task_in_progress = false;
    wifi_unlock();
}

/* ========================================================================
 * WI-FI PERSISTENT WATCHDOG
 * ========================================================================
 * A single persistent task that monitors Wi-Fi connection state and retries
 * with exponential backoff. This handles the C6's typical boot behavior:
 * associate then immediately disconnect during 4-way handshake or DHCP.
 * The watchdog runs for WIFI_WATCHDOG_TOTAL_TIMEOUT_MS then exits. */

static void networking_wifi_start_watchdog(void)
{
    /* Only start if not already running */
    wifi_lock();
    if (s_wifi_watchdog_task != NULL) {
        wifi_unlock();
        return;
    }
    wifi_unlock();

    s_wifi_watchdog_started_us = esp_timer_get_time();
    s_wifi_watchdog_retry_count = 0;

    if (xTaskCreate(networking_wifi_watchdog_task,
                    "wifi_wdog",
                    WIFI_WATCHDOG_STACK_BYTES,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    &s_wifi_watchdog_task) != pdPASS) {
        s_wifi_watchdog_task = NULL;
        networking_record_warningf("Failed to start Wi-Fi watchdog task");
    }
}

static void networking_wifi_watchdog_task(void *arg)
{
    (void)arg;
    int64_t total_elapsed_ms;
    int delay_ms = WIFI_WATCHDOG_DELAY_MS;

    /* Initial delay before first check */
    vTaskDelay(pdMS_TO_TICKS(WIFI_WATCHDOG_DELAY_MS));

    while (1) {
        total_elapsed_ms = (esp_timer_get_time() - s_wifi_watchdog_started_us) / 1000;

        /* Stop if total timeout exceeded */
        if (total_elapsed_ms >= WIFI_WATCHDOG_TOTAL_TIMEOUT_MS) {
            networking_schedulef_ansi("@C[wifi]@R @Ywatchdog:@R @rgiving up@R after @Z%d@R seconds\n",
                                (int)(total_elapsed_ms / 1000));
            led_notify(LED_EVENT_WIFI_ERROR);
            break;
        }

        wifi_lock();
        bool should_retry = (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) &&
                            !s_wifi_connected &&
                            s_wifi_connect_requested &&
                            s_wifi_boot_autoconnect &&
                            s_wifi_target_ssid[0] != '\0';
        char ssid_copy[NETWORKING_WIFI_SSID_BYTES];
        char pass_copy[NETWORKING_WIFI_PASSWORD_BYTES];
        if (should_retry) {
            snprintf(ssid_copy, sizeof(ssid_copy), "%s", s_wifi_target_ssid);
            snprintf(pass_copy, sizeof(pass_copy), "%s", s_wifi_target_password);
        }
        wifi_unlock();

        if (!should_retry) {
            /* Connected or no target — stop watchdog */
            break;
        }

        networking_schedulef_ansi("@C[wifi]@R @Ywatchdog:@R retry @Z%d@R for @W%s@R (delay @Z%d@R ms)\n",
                            s_wifi_watchdog_retry_count + 1, ssid_copy, delay_ms);

        esp_err_t error = networking_wifi_connect_with_credentials(ssid_copy, pass_copy);
        if (error == ESP_OK) {
            /* Connection attempt started — wait and check if it succeeded */
            vTaskDelay(pdMS_TO_TICKS(3000));

            wifi_lock();
            bool connected = s_wifi_connected;
            wifi_unlock();

            if (connected) {
                networking_schedulef_ansi("@C[wifi]@R @Ywatchdog:@R @Gconnected successfully@R\n");
                break;
            }
        }

        s_wifi_watchdog_retry_count++;

        /* Exponential backoff with cap */
        delay_ms *= 2;
        if (delay_ms > WIFI_WATCHDOG_MAX_DELAY_MS) {
            delay_ms = WIFI_WATCHDOG_MAX_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS((TickType_t)delay_ms));
    }

    /* Clean up */
    wifi_lock();
    s_wifi_watchdog_task = NULL;
    wifi_unlock();

    vTaskDelete(NULL);
}