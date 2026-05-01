#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "ansi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "bluetooth.h"
#include "networking.h"
extern bool shell_text_equals_ignore_case(const char *left, const char *right);
extern int shell_split_args(char *text, char **argv, int max_args);

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
static bool s_wifi_connected;
static bool s_wifi_connect_requested;
static char s_wifi_target_ssid[NETWORKING_WIFI_SSID_BYTES];
static char s_wifi_target_password[NETWORKING_WIFI_PASSWORD_BYTES];
static char s_wifi_last_detail[NETWORKING_WIFI_DETAIL_BYTES];
static bool s_wifi_init_task_in_progress;
static bool s_networking_initialized;
static SemaphoreHandle_t s_wifi_mutex;  /* protects all shared Wi-Fi state */

/* ---- Persistent Wi-Fi watchdog ---- */
/* A single persistent task that monitors Wi-Fi connection state and retries
 * with exponential backoff. This is more reliable than spawning per-disconnect
 * tasks because it handles the C6's typical boot behavior: associate then
 * immediately disconnect during 4-way handshake or DHCP. */
#define WIFI_WATCHDOG_STACK_BYTES    4096
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

static void networking_schedulef(const char *format, ...)
{
    char buffer[512];
    va_list args;

    if (s_host_ops.schedule_transcript_append_text == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.schedule_transcript_append_text(buffer);
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
#endif

    s_wifi_connected = false;
    s_wifi_connect_requested = false;
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
                networking_schedulef_ansi("@C[wifi]@R event: @Gassociated@R with @W%s@R on channel @Z%u@R\n",
                                     s_wifi_target_ssid,
                                     (unsigned int)event->channel);
            }
            wifi_unlock();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            wifi_lock();
            s_wifi_connected = false;
            s_wifi_connect_requested = false;
            wifi_unlock();
            networking_schedulef_ansi("@C[wifi]@R event: @ydisconnected@R\n");
            networking_notify_headerf(4000, "WiFi disconnected");
            /* Start the persistent watchdog to attempt reconnection */
            networking_wifi_start_watchdog();
            break;

        default:
            break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && event_data != NULL) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        wifi_lock();
        s_wifi_connected = true;
        s_wifi_connect_requested = false;
        wifi_unlock();
        networking_schedulef("[wifi] event: got IP " IPSTR "\n", IP2STR(&event->ip_info.ip));
        networking_notify_headerf(4000, "WiFi connected: " IPSTR, IP2STR(&event->ip_info.ip));
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
        networking_schedulef("[wifi] failed to read C6 hosted firmware version: %s (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
        networking_schedulef("[wifi] Recovery: flash a matching %u.%u.x ESP32-C6 image from coprocessor/esp32c6_slave or use c6ota default with esp32c6_hosted_slave.bin\n",
                             ESP_HOSTED_VERSION_MAJOR_1,
                             ESP_HOSTED_VERSION_MINOR_1);
        networking_record_warningf("Failed to read hosted firmware version: %s", esp_err_to_name(error));
        return error;
    }

    if (version.major1 != ESP_HOSTED_VERSION_MAJOR_1 || version.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
        networking_wifi_set_detail("ESP-Hosted host %u.%u.%u requires ESP32-C6 firmware %u.%u.x, but the co-processor reports %u.%u.%u. Flash the matching hosted slave build before retrying Wi-Fi.",
                                   ESP_HOSTED_VERSION_MAJOR_1,
                                   ESP_HOSTED_VERSION_MINOR_1,
                                   ESP_HOSTED_VERSION_PATCH_1,
                                   ESP_HOSTED_VERSION_MAJOR_1,
                                   ESP_HOSTED_VERSION_MINOR_1,
                                   version.patch1);
        networking_schedulef("[wifi] hosted version mismatch: host %u.%u.%u, C6 %u.%u.%u\n",
                             ESP_HOSTED_VERSION_MAJOR_1,
                             ESP_HOSTED_VERSION_MINOR_1,
                             ESP_HOSTED_VERSION_PATCH_1,
                             version.major1,
                             version.minor1,
                             version.patch1);
        networking_schedulef("[wifi] Recovery: flash a matching %u.%u.x ESP32-C6 image from coprocessor/esp32c6_slave or use c6ota default with esp32c6_hosted_slave.bin\n",
                             ESP_HOSTED_VERSION_MAJOR_1,
                             ESP_HOSTED_VERSION_MINOR_1);
        networking_record_warningf("Hosted version mismatch: host %u.%u.%u vs C6 %u.%u.%u",
                                   ESP_HOSTED_VERSION_MAJOR_1,
                                   ESP_HOSTED_VERSION_MINOR_1,
                                   ESP_HOSTED_VERSION_PATCH_1,
                                   version.major1,
                                   version.minor1,
                                   version.patch1);
        return ESP_ERR_INVALID_STATE;
    }

    networking_record_infof("Hosted firmware compatible: host %u.%u.%u, C6 %u.%u.%u",
                            ESP_HOSTED_VERSION_MAJOR_1,
                            ESP_HOSTED_VERSION_MINOR_1,
                            ESP_HOSTED_VERSION_PATCH_1,
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

    networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @Cstate@R=@W%s@R @Cconnected@R=%s @Crequested@R=%s\n",
                         label,
                         networking_wifi_state_string_internal(),
                         s_wifi_connected ? "@Gyes@R" : "@kno@R",
                         s_wifi_connect_requested ? "@Gyes@R" : "@kno@R");

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
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @knot connected to an AP@R\n", label);
    } else {
        networking_schedulef_ansi("@C[wifi.diag]@R @rap info failed@R: @r%s@R (0x%x)\n",
                             esp_err_to_name(error),
                             (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        networking_schedulef_ansi("@C[wifi.diag]@R @Cip@R=@W" IPSTR "@R\n", IP2STR(&ip_info.ip));
    } else {
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @kip=not-assigned@R\n", label);
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
        networking_schedulef_ansi("@C[wifi.diag]@R @Corigin@R=@W%s@R @kno networks found@R\n", label);
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

static void networking_wifi_background_task(void *arg)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t *request = (networking_wifi_background_request_t *)arg;

    if (request->start_runtime) {
        networking_wifi_runtime_init();
    }

    if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
        if (request->connect_with_defaults) {
            if (networking_wifi_defaults_available()) {
                esp_err_t connect_error = networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                                                   CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
                if (connect_error != ESP_OK) {
                    networking_schedulef("[wifi] %s default connect failed: %s (0x%x)\n",
                                         request->origin,
                                         esp_err_to_name(connect_error),
                                         (unsigned int)connect_error);
                    networking_record_warningf("%s default connect failed", request->origin);
                }
            } else {
                networking_schedulef("[wifi] %s: sdkconfig default credentials are not configured\n", request->origin);
                networking_record_warningf("%s requested default connect without configured credentials", request->origin);
            }
        } else if (request->ssid[0] != '\0') {
            esp_err_t connect_error = networking_wifi_connect_with_credentials(request->ssid, request->password);
            if (connect_error != ESP_OK) {
                networking_schedulef("[wifi] %s connect failed for %s: %s (0x%x)\n",
                                     request->origin,
                                     request->ssid,
                                     esp_err_to_name(connect_error),
                                     (unsigned int)connect_error);
                networking_record_warningf("%s connect failed for %s", request->origin, request->ssid);
            }
        }

        if (request->run_diagnostic) {
            esp_err_t diagnostic_error = networking_wifi_run_diagnostic(request->origin);
            if (diagnostic_error != ESP_OK && diagnostic_error != ESP_ERR_INVALID_STATE) {
                networking_schedulef("[wifi] %s diagnostic finished with %s (0x%x)\n",
                                     request->origin,
                                     esp_err_to_name(diagnostic_error),
                                     (unsigned int)diagnostic_error);
            }
        }
    } else {
        networking_schedulef("[wifi] %s could not start the Wi-Fi runtime cleanly\n",
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
        .run_diagnostic = true,
    };

    snprintf(request.origin, sizeof(request.origin), "%s", "boot");
    if (!networking_wifi_begin_background_request(&request)) {
        networking_appendf("wifi: boot-time Wi-Fi startup request could not be queued\n");
    }
#endif
}

void networking_wifi_request_post_ota_restore(const networking_wifi_restore_state_t *restore_state)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    networking_wifi_background_request_t request = {
        .start_runtime = true,
        .connect_with_defaults = false,
        .run_diagnostic = true,
    };

    snprintf(request.origin, sizeof(request.origin), "%s", "c6ota-restore");
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
        networking_schedulef("[wifi] %s: failed to queue post-OTA Wi-Fi restore\n", request.origin);
    }
#else
    (void)restore_state;
#endif
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

    networking_schedulef("%s", "c6ota: restoring Wi-Fi after OTA failure\n");
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
        networking_schedulef("%s", "c6ota: waiting for Wi-Fi startup already in progress\n");
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        if (!networking_wifi_defaults_available()) {
            networking_schedulef("%s", "c6ota: OTA requires Wi-Fi. Run wifi connect first or configure sdkconfig defaults.\n");
            return ESP_ERR_INVALID_STATE;
        }

        networking_schedulef("%s", "c6ota: starting Wi-Fi with sdkconfig default credentials for OTA\n");
        networking_wifi_runtime_init();
        if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
            return s_wifi_last_error != ESP_OK ? s_wifi_last_error : ESP_ERR_INVALID_STATE;
        }
    }

    if (!s_wifi_connected && !s_wifi_connect_requested) {
        if (!networking_wifi_defaults_available()) {
            networking_schedulef("%s", "c6ota: OTA requires an active network link. Run wifi connect <ssid> <pass> first.\n");
            return ESP_ERR_INVALID_STATE;
        }

        networking_schedulef("[wifi] ota: connecting to default SSID %s\n", CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
        if (networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                     CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD) != ESP_OK) {
            return s_wifi_last_error != ESP_OK ? s_wifi_last_error : ESP_FAIL;
        }
    }

    while (!s_wifi_connected) {
        TickType_t now = xTaskGetTickCount();

        if ((int32_t)(now - deadline) >= 0) {
            networking_schedulef("%s", "c6ota: Wi-Fi connection timeout before OTA download\n");
            return ESP_ERR_TIMEOUT;
        }

        if ((int32_t)(now - next_log) >= 0) {
            networking_schedulef("%s", "c6ota: waiting for Wi-Fi IP before OTA download\n");
            next_log = now + pdMS_TO_TICKS(5000);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void networking_wifi_scan(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t records[16];
    uint16_t record_count = (uint16_t)(sizeof(records) / sizeof(records[0]));
    uint16_t index;

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        networking_appendf("@Cwifi:@R @yscan requires the Wi-Fi runtime to be started first@R\n");
        networking_record_warningf("Scan rejected because Wi-Fi is not started");
        return;
    }

    networking_appendf("@Cwifi:@R @Nscanning for access points...@R\n");
    error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        networking_record_errorf(error, "WiFi scan failed - check sdkconfig or hosted link");
        return;
    }

    error = esp_wifi_scan_get_ap_records(&record_count, records);
    if (error != ESP_OK) {
        networking_record_errorf(error, "Failed to read WiFi scan results");
        return;
    }

    if (record_count == 0) {
        networking_appendf("@Cwifi.scan:@R @kno access points found@R\n");
        networking_record_infof("Scan completed with no visible APs");
        return;
    }

    for (index = 0; index < record_count; index++) {
        networking_appendf("@Cwifi.scan[%u]:@R @Wssid@R=@W%s@R @Crssi@R=@Z%d@R @Cauth@R=@Z%u@R @Cchannel@R=@Z%u@R\n",
                           (unsigned int)index,
                           records[index].ssid,
                           records[index].rssi,
                           (unsigned int)records[index].authmode,
                           (unsigned int)records[index].primary);
    }
    networking_record_infof("Scan completed with %u APs", (unsigned int)record_count);
#endif
}

void networking_wifi_status(void)
{
#if NETWORKING_WIFI_RUNTIME_ENABLED
    esp_err_t error;
    wifi_ap_record_t ap_info;
    char ap_ssid[NETWORKING_WIFI_SSID_BYTES];
    esp_netif_ip_info_t ip_info;

    networking_appendf("@Cwifi.state:@R %s\n", networking_wifi_state_string_internal());
    networking_appendf("@Cwifi.default_profile:@R %s\n", networking_wifi_defaults_available() ? "@Gconfigured@R" : "@ymissing@R");
    if (networking_wifi_defaults_available()) {
        networking_appendf("@Cwifi.default_ssid:@R @W%s@R\n", CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID);
    }
    if (s_wifi_last_detail[0] != '\0') {
        networking_appendf("@Cwifi.note:@R @k%s@R\n", s_wifi_last_detail);
    }

    if (s_wifi_state != NETWORKING_WIFI_STATE_STARTED) {
        if (s_wifi_state == NETWORKING_WIFI_STATE_FAILED) {
            networking_appendf("@Cwifi.last_error:@R @r%s@R (0x%x)\n", esp_err_to_name(s_wifi_last_error), (unsigned int)s_wifi_last_error);
        }
        if (s_wifi_state == NETWORKING_WIFI_STATE_STARTING) {
            networking_appendf("@Cwifi.progress:@R @yESP-Hosted probe is still running@R\n");
        }
        return;
    }

    networking_appendf("@Cwifi.connect_requested:@R %s\n", s_wifi_connect_requested ? "@Gyes@R" : "@kno@R");
    networking_appendf("@Cwifi.connected:@R %s\n", s_wifi_connected ? "@Gyes@R" : "@kno@R");
    if (s_wifi_target_ssid[0] != '\0') {
        networking_appendf("@Cwifi.target_ssid:@R @W%s@R\n", s_wifi_target_ssid);
    }

    error = esp_wifi_sta_get_ap_info(&ap_info);
    if (error == ESP_OK) {
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", (const char *)ap_info.ssid);
        networking_appendf("@Cwifi.ap:@R @W%s@R, @Crssi@R=@Z%d@R, @Cchannel@R=@Z%u@R\n", ap_ssid, ap_info.rssi, (unsigned int)ap_info.primary);
    } else if (error != ESP_ERR_WIFI_NOT_CONNECT) {
        networking_appendf("@Cwifi.ap_info_error:@R @r%s@R (0x%x)\n", esp_err_to_name(error), (unsigned int)error);
    }

    if (s_wifi_sta_netif != NULL && esp_netif_get_ip_info(s_wifi_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        networking_appendf("@Cwifi.ip:@R @W" IPSTR "@R\n", IP2STR(&ip_info.ip));
    }
#else
    networking_appendf("@Cwifi:@R @ksdkconfig does not enable the Wi-Fi stack@R\n");
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
        networking_appendf("@Cwifi:@R @ystack is not ready@R (@k%s@R)\n", networking_wifi_state_string_internal());
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

void networking_handle_wifi_command(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);

    if (argc <= 1 || shell_text_equals_ignore_case(argv[1], "help")) {
        networking_appendf("@Y@BWi-Fi Commands:@R\n");
        networking_appendf("  @Gwifi status@R                 Show Wi-Fi runtime state and IP info\n");
        networking_appendf("  @Gwifi scan@R                   Scan for nearby SSIDs after Wi-Fi starts\n");
        networking_appendf("  @Gwifi diag@R                   Run a diagnostic status + scan report in the transcript\n");
        networking_appendf("  @Gwifi connect@R                Connect using sdkconfig default credentials\n");
        networking_appendf("  @Gwifi connect@R @T<ssid>@R @T<pass>@R  Connect using runtime credentials\n");
        networking_appendf("  @Gwifi disconnect@R             Disconnect the current station session\n");
        networking_appendf("  @kWi-Fi now starts in the background on normal boot and after successful c6ota restore@R\n");
        networking_appendf("  @kwifi connect still probes ESP-Hosted in a background task so the shell remains responsive@R\n");
        networking_appendf("  @kwifi connect passwords are masked in transcript history and not stored in command recall@R\n");
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        networking_wifi_status();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "scan")) {
        networking_wifi_scan();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "diag")) {
        networking_wifi_diag();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "disconnect")) {
        networking_wifi_disconnect();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "connect")) {
        if (argc == 2) {
            if (!networking_wifi_defaults_available()) {
                networking_appendf("@Cwifi:@R @ysdkconfig default credentials are not configured@R\n");
                return;
            }

            if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
                (void)networking_wifi_connect_with_credentials(CONFIG_P4MINISHELL_WIFI_DEFAULT_SSID,
                                                               CONFIG_P4MINISHELL_WIFI_DEFAULT_PASSWORD);
            } else {
                networking_appendf("@Cwifi:@R @Nstarting stack on demand@R\n");
                (void)networking_wifi_begin_connect_request(NULL, NULL, true);
            }
            return;
        }

        if (argc == 4) {
            if (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) {
                (void)networking_wifi_connect_with_credentials(argv[2], argv[3]);
            } else {
                networking_appendf("@Cwifi:@R @Nstarting stack on demand@R\n");
                (void)networking_wifi_begin_connect_request(argv[2], argv[3], false);
            }
            return;
        }

        networking_appendf("@yUsage: wifi connect or wifi connect <ssid> <pass>@R\n");
        return;
    }

    networking_appendf("@rUnknown wifi subcommand:@R %s\n", argv[1]);
}

void networking_append_sysinfo_summary(void)
{
    switch (s_wifi_state) {
    case NETWORKING_WIFI_STATE_STARTING:
        networking_appendf("@Cwifi:@R @yruntime initialization is in progress@R\n");
        break;
    case NETWORKING_WIFI_STATE_STARTED:
        networking_appendf("@Cwifi:@R @Gruntime initialized@R in STA mode from sdkconfig, @Cconnected@R=%s\n", s_wifi_connected ? "@Gyes@R" : "@kno@R");
        break;
    case NETWORKING_WIFI_STATE_FAILED:
        networking_appendf("@Cwifi:@R @rruntime initialization failed@R with @r%s@R (0x%x)\n",
                           esp_err_to_name(s_wifi_last_error),
                           (unsigned int)s_wifi_last_error);
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_DISABLED:
        networking_appendf("@Cwifi:@R @kskipped because sdkconfig does not enable native or ESP-Hosted Wi-Fi@R\n");
        break;
    case NETWORKING_WIFI_STATE_SKIPPED_UNSUPPORTED:
        networking_appendf("@Cwifi:@R @kunsupported on current target/SoC caps@R\n");
        break;
    case NETWORKING_WIFI_STATE_NOT_ATTEMPTED:
    default:
        networking_appendf("@Cwifi:@R @kruntime initialization not attempted yet@R\n");
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

    networking_wifi_append_step("esp_hosted_connect_to_slave()");
    error = esp_hosted_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        networking_wifi_append_error("esp_hosted_init()", error);
        return;
    }

    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        networking_wifi_append_error("esp_hosted_connect_to_slave()", error);
        return;
    }

    networking_wifi_append_step("esp_hosted_get_coprocessor_fwversion()");
    error = networking_wifi_validate_hosted_version();
    if (error != ESP_OK) {
        s_wifi_state = NETWORKING_WIFI_STATE_FAILED;
        s_wifi_last_error = error;
        return;
    }

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
    networking_wifi_append_step("expected CONFIG_ESP_WIFI_ENABLED, CONFIG_ESP_HOST_WIFI_ENABLED, or CONFIG_ESP_HOSTED_ENABLED from sdkconfig");
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
    if (!s_networking_initialized) {
        s_networking_initialized = true;
        networking_wifi_request_boot_restore();
    }
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
            break;
        }

        wifi_lock();
        bool should_retry = (s_wifi_state == NETWORKING_WIFI_STATE_STARTED) &&
                            !s_wifi_connected &&
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