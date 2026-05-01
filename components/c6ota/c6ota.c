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
#include "storage.h"
extern bool shell_text_equals_ignore_case(const char *left, const char *right);

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
    return source != NULL && shell_text_equals_ignore_case(source, C6OTA_DEFAULT_SOURCE);
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
        return storage_resolve_path(source, resolved_path, resolved_path_size);
    }

    *used_default = true;
    for (index = 0; index < sizeof(default_names) / sizeof(default_names[0]); index++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", BSP_SD_MOUNT_POINT, default_names[index]);
        if (storage_vfs_to_fatfs_path(candidate, fatfs_candidate, sizeof(fatfs_candidate)) != ESP_OK) {
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

#if CONFIG_ESP_HOSTED_ENABLED

static esp_err_t c6ota_parse_header(const uint8_t *buffer,
                                    size_t buffer_size,
                                    char *version,
                                    size_t version_size)
{
    if (buffer == NULL || version == NULL || version_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(version, version_size, "unknown");
    return ESP_FAIL;
}

static esp_err_t c6ota_prepare_session(esp_hosted_coprocessor_fwver_t *ver,
                                        bool *activate_supported,
                                        networking_wifi_restore_state_t *restore_state,
                                        char *failure_hint,
                                        size_t failure_hint_size)
{
    (void)ver;
    (void)activate_supported;
    (void)restore_state;
    (void)failure_hint;
    (void)failure_hint_size;
    return ESP_FAIL;
}

static esp_err_t c6ota_write_chunk(uint8_t *payload, size_t bytes,
                                    size_t *transferred, size_t total,
                                    size_t *last_percent,
                                    char *failure_hint, size_t failure_hint_size)
{
    (void)payload;
    (void)bytes;
    (void)transferred;
    (void)total;
    (void)last_percent;
    (void)failure_hint;
    (void)failure_hint_size;
    return ESP_FAIL;
}

static void c6ota_abort_session(bool started) { (void)started; }

static esp_err_t c6ota_finish_session(bool *ota_started,
                                       bool activate_supported,
                                       size_t transferred,
                                       char *failure_hint,
                                       size_t failure_hint_size)
{
    (void)ota_started;
    (void)activate_supported;
    (void)transferred;
    (void)failure_hint;
    (void)failure_hint_size;
    return ESP_FAIL;
}

static uint8_t *c6ota_alloc_transfer_buffer(void) { return NULL; }
static void c6ota_free_transfer_buffer(uint8_t *buf) { (void)buf; }

static esp_err_t c6ota_download_http_image(const char *url,
                                            uint8_t **image_data,
                                            size_t *image_size,
                                            char *version, size_t version_size,
                                            char *failure_hint, size_t failure_hint_size)
{
    (void)url;
    (void)image_data;
    (void)image_size;
    (void)version;
    (void)version_size;
    (void)failure_hint;
    (void)failure_hint_size;
    return ESP_FAIL;
}

static esp_err_t c6ota_transfer_image_buffer(uint8_t *data, size_t size,
                                              bool activate_supported,
                                              char *failure_hint, size_t failure_hint_size)
{
    (void)data;
    (void)size;
    (void)activate_supported;
    (void)failure_hint;
    (void)failure_hint_size;
    return ESP_FAIL;
}

static bool c6ota_activate_supported(const esp_hosted_coprocessor_fwver_t *version)
{
    (void)version;
    return false;
}

static esp_err_t c6ota_read_slave_version(esp_hosted_coprocessor_fwver_t *version)
{
    (void)version;
    return ESP_FAIL;
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

    if (shell_text_equals_ignore_case(input, "YES")) {
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

    if (shell_text_equals_ignore_case(input, "NO")) {
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