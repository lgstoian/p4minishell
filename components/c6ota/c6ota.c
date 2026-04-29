#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "networking.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"

#include "c6ota.h"
#include "p4minishell_config.h"

/* ---- Backward-compatibility aliases ---- */
#define C6OTA_TASK_STACK_BYTES          P4_CONFIG_C6OTA_TASK_STACK
#define C6OTA_HTTP_BLOCK_BYTES          P4_CONFIG_C6OTA_HTTP_BLOCK_BYTES
#define C6OTA_TRANSFER_CHUNK_BYTES      P4_CONFIG_C6OTA_TRANSFER_CHUNK
#define C6OTA_PROGRESS_STEP_PERCENT     P4_CONFIG_C6OTA_PROGRESS_STEP
#define C6OTA_URL_BYTES                 P4_CONFIG_C6OTA_URL_BYTES
#define C6OTA_DEFAULT_SOURCE            P4_CONFIG_C6OTA_DEFAULT_SOURCE
#define C6OTA_DEFAULT_PRIMARY_FILENAME  P4_CONFIG_C6OTA_DEFAULT_PRIMARY
#define C6OTA_DEFAULT_FALLBACK_FILENAME P4_CONFIG_C6OTA_DEFAULT_FALLBACK
#define C6OTA_EXPECTED_CHIP_ID          P4_CONFIG_C6OTA_EXPECTED_CHIP_ID
#define C6OTA_MIN_RELIABLE_MAJOR        P4_CONFIG_C6OTA_MIN_MAJOR
#define C6OTA_MIN_RELIABLE_MINOR        P4_CONFIG_C6OTA_MIN_MINOR
#define C6OTA_MIN_RELIABLE_PATCH        P4_CONFIG_C6OTA_MIN_PATCH
#define C6OTA_SD_FATFS_DRIVE            P4_CONFIG_C6OTA_SD_FATFS_DRIVE

typedef enum {
    C6OTA_MODE_HTTP = 0,
    C6OTA_MODE_SD,
} c6ota_mode_t;

typedef struct {
    c6ota_mode_t mode;
    char source[C6OTA_URL_BYTES];
} c6ota_request_t;

typedef struct {
    bool active;
    c6ota_request_t request;
} c6ota_confirmation_t;

// These host hooks are implemented in main/main.c so the OTA module can keep the
// exact transcript and debug-history behavior without owning the shell UI.
extern void c6ota_host_transcript_append_text(const char *text);
extern void c6ota_host_schedule_transcript_append_text(const char *text);
extern void c6ota_host_record_error(esp_err_t error, const char *message);
extern void c6ota_host_record_warning(const char *message);
extern void c6ota_host_record_info(const char *message);
extern void c6ota_host_notify_header(const char *text, uint32_t timeout_ms);

static c6ota_progress_callback_t s_progress_callback;
static bool s_update_in_progress;
static c6ota_confirmation_t s_confirmation;

static bool c6ota_text_equals_ignore_case(const char *left, const char *right)
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

static void c6ota_record_errorf(esp_err_t error, const char *format, ...)
{
    char buffer[192];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    c6ota_host_record_error(error, buffer);
}

static void c6ota_record_warningf(const char *format, ...)
{
    char buffer[192];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    c6ota_host_record_warning(buffer);
}

static void c6ota_record_infof(const char *format, ...)
{
    char buffer[192];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    c6ota_host_record_info(buffer);
}

static void c6ota_emit_syncf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (s_progress_callback != NULL) {
        s_progress_callback(-1, buffer);
        return;
    }

    c6ota_host_transcript_append_text(buffer);
}

static void c6ota_emit_asyncf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (s_progress_callback != NULL) {
        s_progress_callback(-2, buffer);
        return;
    }

    c6ota_host_schedule_transcript_append_text(buffer);
}

static void c6ota_notify_headerf(uint32_t timeout_ms, const char *format, ...)
{
    char buffer[160];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    c6ota_host_notify_header(buffer, timeout_ms);
}

static bool c6ota_source_is_http(const char *source)
{
    return source != NULL &&
           (strncmp(source, "http://", 7) == 0 || strncmp(source, "https://", 8) == 0);
}

static bool c6ota_source_is_default(const char *source)
{
    return source != NULL && c6ota_text_equals_ignore_case(source, C6OTA_DEFAULT_SOURCE);
}

static bool c6ota_source_is_sd(const char *source)
{
    if (source == NULL) {
        return false;
    }

    if (c6ota_source_is_default(source)) {
        return true;
    }

    return strncmp(source, "sd:/", 4) == 0 ||
           strcmp(source, BSP_SD_MOUNT_POINT) == 0 ||
           strncmp(source, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0;
}

static esp_err_t c6ota_wait_for_wifi_ready(void)
{
    return networking_wifi_wait_for_ota();
}

static esp_err_t c6ota_resolve_sd_path(const char *input, char *output, size_t output_size)
{
    int written;
    const char *source = input;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source == NULL || source[0] == '\0') {
        source = BSP_SD_MOUNT_POINT;
    }

    if (strncmp(source, "sd:/", 4) == 0) {
        written = snprintf(output, output_size, "%s%s", BSP_SD_MOUNT_POINT, source + 3);
    } else if (strncmp(source, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0 ||
               strcmp(source, BSP_SD_MOUNT_POINT) == 0) {
        written = snprintf(output, output_size, "%s", source);
    } else if (source[0] == '/') {
        written = snprintf(output, output_size, "%s", source);
    } else {
        written = snprintf(output, output_size, "%s/%s", BSP_SD_MOUNT_POINT, source);
    }

    if (written < 0) {
        output[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t c6ota_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size)
{
    const char *relative_path;
    int written;

    if (vfs_path == NULL || fatfs_path == NULL || fatfs_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(vfs_path, BSP_SD_MOUNT_POINT) == 0) {
        relative_path = "/";
    } else if (strncmp(vfs_path, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        relative_path = vfs_path + strlen(BSP_SD_MOUNT_POINT);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(fatfs_path, fatfs_path_size, "%s%s", C6OTA_SD_FATFS_DRIVE, relative_path);
    if (written < 0) {
        fatfs_path[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= fatfs_path_size) {
        fatfs_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

#if CONFIG_ESP_HOSTED_ENABLED
static esp_err_t c6ota_parse_header(const uint8_t *buffer,
                                    size_t buffer_size,
                                    char *version,
                                    size_t version_size)
{
    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    esp_app_desc_t app_desc;
    const size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);

    if (buffer == NULL || version == NULL || version_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (buffer_size < app_desc_offset + sizeof(app_desc)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&image_header, buffer, sizeof(image_header));
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (image_header.chip_id != C6OTA_EXPECTED_CHIP_ID) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memcpy(&app_desc, buffer + app_desc_offset, sizeof(app_desc));
    snprintf(version, version_size, "%s", app_desc.version);
    return ESP_OK;
}

static bool c6ota_activate_supported(const esp_hosted_coprocessor_fwver_t *version)
{
    if (version == NULL) {
        return false;
    }

    return version->major1 > 2 || (version->major1 == 2 && version->minor1 > 5);
}

static esp_err_t c6ota_read_slave_version(esp_hosted_coprocessor_fwver_t *version)
{
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(version, 0, sizeof(*version));
    return esp_hosted_get_coprocessor_fwversion(version);
}

static bool c6ota_reliable_supported(const esp_hosted_coprocessor_fwver_t *version)
{
    if (version == NULL) {
        return false;
    }

    if (version->major1 > C6OTA_MIN_RELIABLE_MAJOR) {
        return true;
    }

    if (version->major1 < C6OTA_MIN_RELIABLE_MAJOR) {
        return false;
    }

    if (version->minor1 > C6OTA_MIN_RELIABLE_MINOR) {
        return true;
    }

    if (version->minor1 < C6OTA_MIN_RELIABLE_MINOR) {
        return false;
    }

    return version->patch1 >= C6OTA_MIN_RELIABLE_PATCH;
}

static uint8_t *c6ota_alloc_transfer_buffer(void)
{
    uint8_t *payload = heap_caps_malloc(C6OTA_TRANSFER_CHUNK_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (payload == NULL) {
        payload = heap_caps_malloc(C6OTA_TRANSFER_CHUNK_BYTES, MALLOC_CAP_8BIT);
    }

    return payload;
}

static void c6ota_free_transfer_buffer(uint8_t *payload)
{
    if (payload != NULL) {
        heap_caps_free(payload);
    }
}

static void c6ota_report_progress(size_t transferred_bytes,
                                  size_t total_bytes,
                                  size_t *last_reported_percent)
{
    char message[128];
    size_t percent;

    if (total_bytes == 0 || last_reported_percent == NULL) {
        return;
    }

    percent = (transferred_bytes * 100U) / total_bytes;
    if (percent < *last_reported_percent + C6OTA_PROGRESS_STEP_PERCENT &&
        transferred_bytes < total_bytes) {
        return;
    }

    *last_reported_percent = percent;
    snprintf(message,
             sizeof(message),
             "C6 OTA: %u%% (%u KB / %u KB)\n",
             (unsigned int)percent,
             (unsigned int)((transferred_bytes + 1023U) / 1024U),
             (unsigned int)((total_bytes + 1023U) / 1024U));

    if (s_progress_callback != NULL) {
        s_progress_callback((int)percent, message);
        return;
    }

    c6ota_host_schedule_transcript_append_text(message);
}

static esp_err_t c6ota_reset_hosted_transport(char *failure_hint, size_t failure_hint_size)
{
    esp_err_t error;

    c6ota_emit_asyncf("%s", "c6ota: reinitializing ESP-Hosted transport for OTA recovery\n");
    error = esp_hosted_deinit();
    if (error != ESP_OK) {
        c6ota_emit_asyncf("c6ota: hosted deinit returned %s (0x%x)\n",
                          esp_err_to_name(error),
                          (unsigned int)error);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    error = esp_hosted_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        snprintf(failure_hint, failure_hint_size, "failed to reinitialize hosted transport before OTA");
        return error;
    }

    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        snprintf(failure_hint, failure_hint_size, "hosted transport is unavailable after OTA recovery reset");
        return error;
    }

    return ESP_OK;
}

static esp_err_t c6ota_prepare_session(esp_hosted_coprocessor_fwver_t *version,
                                       bool *activate_supported,
                                       networking_wifi_restore_state_t *restore_state,
                                       char *failure_hint,
                                       size_t failure_hint_size)
{
    esp_err_t error;

    if (version == NULL || activate_supported == NULL || restore_state == NULL ||
        failure_hint == NULL || failure_hint_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (networking_wifi_is_starting()) {
        snprintf(failure_hint, failure_hint_size, "Wi-Fi startup is in progress - retry in a moment");
        return ESP_ERR_INVALID_STATE;
    }

    networking_wifi_capture_restore_state(restore_state);
    if (restore_state->should_restore_runtime) {
        c6ota_emit_asyncf("%s", "c6ota: stopping Wi-Fi completely before OTA\n");
        error = networking_wifi_shutdown();
        if (error != ESP_OK) {
            snprintf(failure_hint, failure_hint_size, "failed to stop Wi-Fi before OTA");
            return error;
        }
    }

    c6ota_emit_asyncf("%s", "c6ota: switching ESP-Hosted to Wi-Fi-off OTA mode\n");

    // c6ota module integration: OTA flow lives in components/c6ota with stable public API.
    error = esp_hosted_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        snprintf(failure_hint, failure_hint_size, "failed to initialize hosted transport before OTA");
        return error;
    }

    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        snprintf(failure_hint, failure_hint_size, "hosted transport is unavailable - recover the C6 link and retry");
        return error;
    }

    *activate_supported = false;
    error = c6ota_read_slave_version(version);
    if (error != ESP_OK) {
        c6ota_emit_asyncf("%s", "c6ota: could not read current C6 firmware version on the reused transport\n");

        error = c6ota_reset_hosted_transport(failure_hint, failure_hint_size);
        if (error == ESP_OK) {
            error = c6ota_read_slave_version(version);
        }

        if (error != ESP_OK) {
            c6ota_emit_asyncf("%s", "c6ota: current C6 version is still unreadable after transport reset; continuing in recovery mode without activate support\n");
            c6ota_record_warningf("Proceeding without current C6 version after hosted transport reset");
            memset(version, 0, sizeof(*version));
            return ESP_OK;
        }
    }

    *activate_supported = c6ota_activate_supported(version);
    c6ota_emit_asyncf("c6ota: current C6 hosted firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
                      version->major1,
                      version->minor1,
                      version->patch1);

    if (version->major1 == 2 && version->minor1 == 3 && version->patch1 == 0) {
        snprintf(failure_hint,
                 failure_hint_size,
                 "Factory v2.3.0 requires one-time standalone tool from https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota first.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!c6ota_reliable_supported(version)) {
        c6ota_emit_asyncf("%s", "c6ota: legacy C6 firmware detected; using Wi-Fi-off SDIO-only OTA path\n");
    }

    return ESP_OK;
}

static esp_err_t c6ota_write_chunk(uint8_t *payload,
                                   size_t payload_len,
                                   size_t *transferred_bytes,
                                   size_t total_bytes,
                                   size_t *last_reported_percent,
                                   char *failure_hint,
                                   size_t failure_hint_size)
{
    esp_err_t error;

    if (payload == NULL || transferred_bytes == NULL || last_reported_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = esp_hosted_slave_ota_write(payload, (uint32_t)payload_len);
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "hosted OTA write failed - recover the transport and retry");
        return error;
    }

    *transferred_bytes += payload_len;
    c6ota_report_progress(*transferred_bytes, total_bytes, last_reported_percent);
    return ESP_OK;
}

static esp_err_t c6ota_finish_session(bool *ota_started,
                                      bool activate_supported,
                                      size_t transferred_bytes,
                                      char *failure_hint,
                                      size_t failure_hint_size)
{
    esp_err_t error = esp_hosted_slave_ota_end();

    (void)transferred_bytes;
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "OTA finalize failed - retry after restoring the hosted link");
        return error;
    }

    if (ota_started != NULL) {
        *ota_started = false;
    }

    if (!activate_supported) {
        return ESP_OK;
    }

    error = esp_hosted_slave_ota_activate();
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "activate failed - power cycle the board and retry");
        return error;
    }

    c6ota_emit_asyncf("%s", "c6ota: activate requested; the C6 will reboot now\n");
    return ESP_OK;
}

static void c6ota_abort_session(bool ota_started)
{
    if (ota_started) {
        esp_err_t end_error = esp_hosted_slave_ota_end();

        if (end_error != ESP_OK) {
            c6ota_emit_asyncf("c6ota: OTA cleanup warning: %s (0x%x)\n",
                              esp_err_to_name(end_error),
                              (unsigned int)end_error);
        }
    }
}

static esp_err_t c6ota_transfer_image_buffer(const uint8_t *image_data,
                                             size_t image_size,
                                             bool activate_supported,
                                             char *failure_hint,
                                             size_t failure_hint_size)
{
    bool ota_started = false;
    size_t transferred_bytes = 0;
    size_t last_reported_percent = 0;
    esp_err_t error;

    if (image_data == NULL || image_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    error = esp_hosted_slave_ota_begin();
    if (error != ESP_OK) {
        c6ota_emit_asyncf("%s", "c6ota: OTA begin failed on the current hosted link; retrying after transport reset\n");
        error = c6ota_reset_hosted_transport(failure_hint, failure_hint_size);
        if (error != ESP_OK) {
            return error;
        }

        error = esp_hosted_slave_ota_begin();
        if (error != ESP_OK) {
            snprintf(failure_hint, failure_hint_size, "OTA begin failed - check hosted transport health");
            return error;
        }
    }
    ota_started = true;

    while (transferred_bytes < image_size) {
        size_t chunk_size = MIN(image_size - transferred_bytes, (size_t)C6OTA_TRANSFER_CHUNK_BYTES);

        error = c6ota_write_chunk((uint8_t *)(image_data + transferred_bytes),
                                  chunk_size,
                                  &transferred_bytes,
                                  image_size,
                                  &last_reported_percent,
                                  failure_hint,
                                  failure_hint_size);
        if (error != ESP_OK) {
            c6ota_abort_session(ota_started);
            return error;
        }
    }

    error = c6ota_finish_session(&ota_started,
                                 activate_supported,
                                 transferred_bytes,
                                 failure_hint,
                                 failure_hint_size);
    if (error != ESP_OK) {
        c6ota_abort_session(ota_started);
    }

    return error;
}

static esp_err_t c6ota_download_http_image(const char *url,
                                           uint8_t **image_data,
                                           size_t *image_size,
                                           char *incoming_version,
                                           size_t incoming_version_size,
                                           char *failure_hint,
                                           size_t failure_hint_size)
{
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = C6OTA_HTTP_BLOCK_BYTES,
        .buffer_size_tx = C6OTA_HTTP_BLOCK_BYTES,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t error;
    int64_t remote_length;
    int http_status;
    size_t content_length;
    size_t total_read = 0;
    uint8_t *download_buffer = NULL;

    if (!c6ota_source_is_http(url) || image_data == NULL || image_size == NULL ||
        incoming_version == NULL || incoming_version_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *image_data = NULL;
    *image_size = 0;

    error = c6ota_wait_for_wifi_ready();
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "network unavailable - connect Wi-Fi and retry");
        return error;
    }

    if (strncmp(url, "https://", 8) == 0) {
        http_config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    c6ota_emit_asyncf("c6ota: downloading %s\n", url);
    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        snprintf(failure_hint, failure_hint_size, "failed to allocate HTTP client");
        return ESP_ERR_NO_MEM;
    }

    error = esp_http_client_open(client, 0);
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "download open failed - check URL or Wi-Fi routing");
        goto cleanup;
    }

    remote_length = esp_http_client_fetch_headers(client);
    http_status = esp_http_client_get_status_code(client);
    if (http_status != 200) {
        snprintf(failure_hint, failure_hint_size, "server returned HTTP status %d", http_status);
        error = ESP_FAIL;
        goto cleanup;
    }

    if (remote_length <= 0) {
        snprintf(failure_hint, failure_hint_size, "server did not return a valid Content-Length");
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    content_length = (size_t)remote_length;
    download_buffer = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (download_buffer == NULL) {
        download_buffer = heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
    }
    if (download_buffer == NULL) {
        snprintf(failure_hint, failure_hint_size, "out of memory buffering HTTP OTA image");
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (total_read < content_length) {
        int bytes_read = esp_http_client_read(client,
                                              (char *)(download_buffer + total_read),
                                              (int)MIN((size_t)C6OTA_HTTP_BLOCK_BYTES, content_length - total_read));

        if (bytes_read < 0) {
            snprintf(failure_hint, failure_hint_size, "HTTP read failed before download completed");
            error = ESP_FAIL;
            goto cleanup;
        }

        if (bytes_read == 0) {
            break;
        }

        total_read += (size_t)bytes_read;
    }

    if (total_read != content_length) {
        snprintf(failure_hint, failure_hint_size, "HTTP download ended before the full image was received");
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    error = c6ota_parse_header(download_buffer,
                               total_read,
                               incoming_version,
                               incoming_version_size);
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "downloaded image is not a valid ESP32-C6 ESP-IDF app image");
        goto cleanup;
    }

    c6ota_emit_asyncf("c6ota: HTTP image header OK, version=%s\n", incoming_version);
    *image_data = download_buffer;
    *image_size = total_read;
    error = ESP_OK;

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (error != ESP_OK && download_buffer != NULL) {
        heap_caps_free(download_buffer);
    }

    return error;
}

static esp_err_t c6ota_resolve_sd_source(const char *source,
                                         char *resolved_path,
                                         size_t resolved_path_size,
                                         bool *used_default,
                                         char *failure_hint,
                                         size_t failure_hint_size)
{
    char candidate[320];
    char fatfs_candidate[320];
    const char *default_names[] = {
        C6OTA_DEFAULT_PRIMARY_FILENAME,
        C6OTA_DEFAULT_FALLBACK_FILENAME,
    };
    size_t index;
    FILINFO file_info;
    FRESULT result;

    if (source == NULL || resolved_path == NULL || resolved_path_size == 0 || used_default == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *used_default = false;
    if (!c6ota_source_is_default(source)) {
        return c6ota_resolve_sd_path(source, resolved_path, resolved_path_size);
    }

    *used_default = true;
    for (index = 0; index < sizeof(default_names) / sizeof(default_names[0]); index++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", BSP_SD_MOUNT_POINT, default_names[index]);
        if (c6ota_vfs_to_fatfs_path(candidate, fatfs_candidate, sizeof(fatfs_candidate)) != ESP_OK) {
            continue;
        }

        memset(&file_info, 0, sizeof(file_info));
        result = f_stat(fatfs_candidate, &file_info);
        if (result == FR_OK) {
            snprintf(resolved_path, resolved_path_size, "%s", candidate);
            return ESP_OK;
        }
    }

    snprintf(failure_hint,
             failure_hint_size,
             "default firmware not found on SD root (%s or %s)",
             C6OTA_DEFAULT_PRIMARY_FILENAME,
             C6OTA_DEFAULT_FALLBACK_FILENAME);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t c6ota_run_http_source(const char *url, char *failure_hint, size_t failure_hint_size)
{
    esp_err_t error;
    networking_wifi_restore_state_t restore_state = { 0 };
    esp_hosted_coprocessor_fwver_t current_version = { 0 };
    uint8_t *image_data = NULL;
    char incoming_version[32] = "unknown";
    size_t image_size = 0;
    bool activate_supported = false;

    if (!c6ota_source_is_http(url)) {
        snprintf(failure_hint, failure_hint_size, "use c6ota http[s]://host/path/to/firmware.bin");
        return ESP_ERR_INVALID_ARG;
    }

    error = c6ota_download_http_image(url,
                                      &image_data,
                                      &image_size,
                                      incoming_version,
                                      sizeof(incoming_version),
                                      failure_hint,
                                      failure_hint_size);
    if (error != ESP_OK) {
        return error;
    }

    error = c6ota_prepare_session(&current_version,
                                  &activate_supported,
                                  &restore_state,
                                  failure_hint,
                                  failure_hint_size);
    if (error != ESP_OK) {
        goto cleanup;
    }

    c6ota_emit_asyncf("c6ota: transferring HTTP image version %s over SDIO-only OTA\n", incoming_version);
    error = c6ota_transfer_image_buffer(image_data,
                                        image_size,
                                        activate_supported,
                                        failure_hint,
                                        failure_hint_size);
    if (error == ESP_OK) {
        networking_wifi_request_post_ota_restore(&restore_state);
    }

cleanup:
    if (error != ESP_OK) {
        esp_err_t restore_error = networking_wifi_restore_after_ota_failure(&restore_state);

        if (restore_error != ESP_OK) {
            c6ota_emit_asyncf("c6ota: Wi-Fi restore failed: %s (0x%x)\n",
                              esp_err_to_name(restore_error),
                              (unsigned int)restore_error);
            c6ota_record_warningf("Wi-Fi restore failed: %s", esp_err_to_name(restore_error));
        }
    }
    if (image_data != NULL) {
        heap_caps_free(image_data);
    }
    return error;
}

static esp_err_t c6ota_run_sd_source(const char *source, char *failure_hint, size_t failure_hint_size)
{
    char normalized_path[320];
    FILE *firmware = NULL;
    esp_err_t error = ESP_OK;
    esp_hosted_coprocessor_fwver_t current_version = { 0 };
    networking_wifi_restore_state_t restore_state = { 0 };
    uint8_t *payload = NULL;
    char incoming_version[32] = "unknown";
    bool mounted_here = false;
    bool used_default = false;
    bool ota_started = false;
    bool activate_supported = false;
    long file_size_long = 0;
    size_t file_size;
    size_t transferred_bytes = 0;
    size_t last_reported_percent = 0;
    size_t first_chunk;

    if (!c6ota_source_is_sd(source)) {
        snprintf(failure_hint, failure_hint_size, "use c6ota sd:/path/to/firmware.bin or c6ota default");
        return ESP_ERR_INVALID_ARG;
    }

    c6ota_emit_asyncf("c6ota: mounting SD card for %s\n", source);
    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        mounted_here = true;
    } else if (error != ESP_ERR_INVALID_STATE) {
        snprintf(failure_hint, failure_hint_size, "SD mount failed - insert the card and retry");
        return error;
    }

    error = c6ota_resolve_sd_source(source,
                                    normalized_path,
                                    sizeof(normalized_path),
                                    &used_default,
                                    failure_hint,
                                    failure_hint_size);
    if (error != ESP_OK) {
        goto cleanup;
    }

    if (used_default) {
        c6ota_emit_asyncf("c6ota: default source resolved to %s\n", normalized_path);
    }

    firmware = fopen(normalized_path, "rb");
    if (firmware == NULL) {
        snprintf(failure_hint, failure_hint_size, "firmware file not found on SD");
        error = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (fseek(firmware, 0, SEEK_END) != 0) {
        snprintf(failure_hint, failure_hint_size, "failed to seek the SD image");
        error = ESP_FAIL;
        goto cleanup;
    }

    file_size_long = ftell(firmware);
    if (file_size_long <= 0) {
        snprintf(failure_hint, failure_hint_size, "image file is empty or invalid");
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (fseek(firmware, 0, SEEK_SET) != 0) {
        snprintf(failure_hint, failure_hint_size, "failed to rewind the SD image");
        error = ESP_FAIL;
        goto cleanup;
    }

    file_size = (size_t)file_size_long;
    payload = c6ota_alloc_transfer_buffer();
    if (payload == NULL) {
        snprintf(failure_hint, failure_hint_size, "out of memory allocating OTA buffer");
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    c6ota_emit_asyncf("c6ota: SD source=%s size=%u bytes\n",
                      normalized_path,
                      (unsigned int)file_size);

    first_chunk = fread(payload, 1, MIN(file_size, (size_t)C6OTA_TRANSFER_CHUNK_BYTES), firmware);
    if (first_chunk == 0) {
        snprintf(failure_hint, failure_hint_size, "failed to read the OTA image header from SD");
        error = ESP_FAIL;
        goto cleanup;
    }

    error = c6ota_parse_header(payload, first_chunk, incoming_version, sizeof(incoming_version));
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "SD image is not a valid ESP-IDF app image");
        goto cleanup;
    }
    c6ota_emit_asyncf("c6ota: SD image header OK, version=%s\n", incoming_version);

    error = c6ota_prepare_session(&current_version,
                                  &activate_supported,
                                  &restore_state,
                                  failure_hint,
                                  failure_hint_size);
    if (error != ESP_OK) {
        goto cleanup;
    }

    error = esp_hosted_slave_ota_begin();
    if (error != ESP_OK) {
        snprintf(failure_hint, failure_hint_size, "OTA begin failed - check hosted transport health");
        goto cleanup;
    }
    ota_started = true;

    error = c6ota_write_chunk(payload,
                              first_chunk,
                              &transferred_bytes,
                              file_size,
                              &last_reported_percent,
                              failure_hint,
                              failure_hint_size);
    if (error != ESP_OK) {
        goto cleanup;
    }

    while (transferred_bytes < file_size) {
        size_t chunk = MIN(file_size - transferred_bytes, (size_t)C6OTA_TRANSFER_CHUNK_BYTES);
        size_t bytes_read = fread(payload, 1, chunk, firmware);

        if (bytes_read != chunk) {
            snprintf(failure_hint, failure_hint_size, "SD read failed before OTA transfer completed");
            error = ESP_FAIL;
            goto cleanup;
        }

        error = c6ota_write_chunk(payload,
                                  bytes_read,
                                  &transferred_bytes,
                                  file_size,
                                  &last_reported_percent,
                                  failure_hint,
                                  failure_hint_size);
        if (error != ESP_OK) {
            goto cleanup;
        }
    }

    error = c6ota_finish_session(&ota_started,
                                 activate_supported,
                                 transferred_bytes,
                                 failure_hint,
                                 failure_hint_size);
    if (error == ESP_OK) {
        networking_wifi_request_post_ota_restore(&restore_state);
    }

cleanup:
    c6ota_abort_session(ota_started);
    if (error != ESP_OK) {
        esp_err_t restore_error = networking_wifi_restore_after_ota_failure(&restore_state);

        if (restore_error != ESP_OK) {
            c6ota_emit_asyncf("c6ota: Wi-Fi restore failed: %s (0x%x)\n",
                              esp_err_to_name(restore_error),
                              (unsigned int)restore_error);
            c6ota_record_warningf("Wi-Fi restore failed: %s", esp_err_to_name(restore_error));
        }
    }
    if (firmware != NULL) {
        fclose(firmware);
    }
    if (mounted_here) {
        esp_err_t unmount_error = bsp_sdcard_unmount();

        if (unmount_error != ESP_OK) {
            c6ota_emit_asyncf("c6ota: SD unmount warning: %s (0x%x)\n",
                              esp_err_to_name(unmount_error),
                              (unsigned int)unmount_error);
        }
    }
    c6ota_free_transfer_buffer(payload);
    return error;
}
#endif

static void c6ota_task(void *arg)
{
    c6ota_request_t *request = (c6ota_request_t *)arg;
    esp_err_t error = ESP_OK;
    bool update_succeeded = false;
    char failure_hint[192] = "hosted transport is unavailable - recover the C6 link and retry";

    if (request == NULL) {
        s_update_in_progress = false;
        c6ota_record_errorf(ESP_ERR_INVALID_ARG, "Background OTA request was null");
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_ESP_HOSTED_ENABLED
    c6ota_emit_asyncf("c6ota: preparing %s\n", request->source);
    if (request->mode == C6OTA_MODE_SD) {
        error = c6ota_run_sd_source(request->source, failure_hint, sizeof(failure_hint));
    } else {
        error = c6ota_run_http_source(request->source, failure_hint, sizeof(failure_hint));
    }

    if (error == ESP_OK) {
        c6ota_emit_asyncf("%s", "C6 OTA completed successfully! Type reboot to activate new firmware.\n");
        c6ota_record_infof("C6 OTA completed for %s", request->source);
        c6ota_notify_headerf(5000, "C6 OTA complete - reboot to activate");
        update_succeeded = true;
    }
#else
    error = ESP_ERR_NOT_SUPPORTED;
    snprintf(failure_hint, sizeof(failure_hint), "ESP-Hosted OTA is disabled in sdkconfig");
#endif

    if (!update_succeeded) {
        c6ota_emit_asyncf("C6 OTA failed: %s - %s\n", esp_err_to_name(error), failure_hint);
        c6ota_record_warningf("C6 OTA failed: %s - %s", esp_err_to_name(error), failure_hint);
        c6ota_notify_headerf(5000, "C6 OTA failed");
    }

    free(request);
    s_update_in_progress = false;
    vTaskDelete(NULL);
}

void c6ota_init(void)
{
    memset(&s_confirmation, 0, sizeof(s_confirmation));
    s_update_in_progress = false;
}

void c6ota_register_progress_callback(c6ota_progress_callback_t cb)
{
    s_progress_callback = cb;
}

bool c6ota_try_handle_input(const char *input)
{
    c6ota_request_t *request;

    if (!s_confirmation.active || input == NULL) {
        return false;
    }

    if (c6ota_text_equals_ignore_case(input, "YES")) {
        request = (c6ota_request_t *)calloc(1, sizeof(*request));
        if (request == NULL) {
            c6ota_emit_syncf("%s", "c6ota: out of memory\n");
            c6ota_record_errorf(ESP_ERR_NO_MEM, "Out of memory allocating OTA request");
            s_confirmation.active = false;
            return true;
        }

        memcpy(request, &s_confirmation.request, sizeof(*request));
        s_confirmation.active = false;
        s_update_in_progress = true;
        c6ota_emit_syncf("%s", "c6ota: confirmation accepted - starting OTA task\n");
        c6ota_notify_headerf(3500, "C6 OTA starting");
        if (xTaskCreate(c6ota_task,
                        "c6ota_task",
                        C6OTA_TASK_STACK_BYTES,
                        request,
                        tskIDLE_PRIORITY + 2,
                        NULL) != pdPASS) {
            s_update_in_progress = false;
            free(request);
            c6ota_emit_syncf("%s", "c6ota: failed to start background OTA task\n");
            c6ota_record_errorf(ESP_FAIL, "Failed to start background OTA task");
        }
        return true;
    }

    if (c6ota_text_equals_ignore_case(input, "NO")) {
        s_confirmation.active = false;
        c6ota_emit_syncf("%s", "c6ota: cancelled before rebooting the C6\n");
        c6ota_record_infof("User cancelled OTA confirmation");
        c6ota_notify_headerf(3000, "C6 OTA cancelled");
        return true;
    }

    c6ota_emit_syncf("%s", "c6ota: type YES to continue or NO to cancel\n");
    c6ota_emit_syncf("%s", "WARNING: This will reboot the C6. Type YES to continue\n");
    return true;
}

void c6ota_perform(const char *source)
{
#if !CONFIG_ESP_HOSTED_ENABLED
    (void)source;
    c6ota_emit_syncf("%s", "c6ota: unavailable because ESP-Hosted is disabled in sdkconfig\n");
    c6ota_record_warningf("Rejected because ESP-Hosted is disabled");
    return;
#else
    if (source == NULL || source[0] == '\0') {
        c6ota_emit_syncf("%s", "Usage: c6ota <sd:/path/to/firmware.bin|http[s]://host/path.bin|default>\n");
        c6ota_record_warningf("Usage error for c6ota command");
        return;
    }

    if (s_update_in_progress || s_confirmation.active) {
        c6ota_emit_syncf("%s", "c6ota: another C6 update is already running or awaiting confirmation\n");
        c6ota_record_warningf("Rejected because an update is already running or pending confirmation");
        return;
    }

    memset(&s_confirmation, 0, sizeof(s_confirmation));
    if (c6ota_source_is_sd(source)) {
        s_confirmation.request.mode = C6OTA_MODE_SD;
    } else if (c6ota_source_is_http(source)) {
        s_confirmation.request.mode = C6OTA_MODE_HTTP;
    } else {
        c6ota_emit_syncf("%s", "Usage: c6ota <sd:/path/to/firmware.bin|http[s]://host/path.bin|default>\n");
        c6ota_record_warningf("Unsupported OTA source: %s", source);
        return;
    }

    snprintf(s_confirmation.request.source, sizeof(s_confirmation.request.source), "%s", source);
    s_confirmation.active = true;
    c6ota_emit_syncf("c6ota: queued source %s\n", source);
    c6ota_notify_headerf(3500, "C6 OTA queued");
    c6ota_emit_syncf("%s", "Factory v2.3.0 requires one-time standalone tool from https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota first.\n");
    c6ota_emit_syncf("%s", "WARNING: This will reboot the C6. Type YES to continue\n");
    c6ota_record_infof("Awaiting confirmation for %s", source);
#endif
}

bool c6ota_is_busy(void)
{
    return s_update_in_progress;
}

bool c6ota_is_confirmation_pending(void)
{
    return s_confirmation.active;
}