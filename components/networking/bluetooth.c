#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_misc.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#endif

#include "bluetooth.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"

/* ---- Backward-compatibility aliases ---- */
#define BLUETOOTH_TAG               P4_CONFIG_BLUETOOTH_TAG
#define BLUETOOTH_DEVICE_NAME       P4_CONFIG_BLUETOOTH_DEVICE_NAME
#define BLUETOOTH_DISCOVERY_LIMIT   P4_CONFIG_BLUETOOTH_DISCOVERY_LIMIT
#define BLUETOOTH_NAME_BYTES        P4_CONFIG_BLUETOOTH_NAME_BYTES
#define BLUETOOTH_ADDR_BYTES        P4_CONFIG_BLUETOOTH_ADDR_BYTES

typedef struct {
    char address[BLUETOOTH_ADDR_BYTES];
    char name[BLUETOOTH_NAME_BYTES];
    int rssi;
} bluetooth_device_t;

typedef struct {
    bool hosted_ready;
    bool controller_enabled;
    bool nimble_initialized;
    bool synced;
    bool advertising_requested;
    bool advertising_active;
    bool scan_requested;
    bool scan_active;
    uint8_t own_addr_type;
    esp_err_t last_error;
    bool fw_version_valid;
    esp_hosted_coprocessor_fwver_t fw_version;
    bluetooth_device_t discovered[BLUETOOTH_DISCOVERY_LIMIT];
    size_t discovered_count;
} bluetooth_state_t;

static networking_host_ops_t s_host_ops;
static bluetooth_state_t s_bluetooth_state;

#if CONFIG_BT_NIMBLE_ENABLED
static esp_err_t bluetooth_start_scan(void);
static esp_err_t bluetooth_start_advertising(void);
#endif

static bool bluetooth_text_equals_ignore_case(const char *left, const char *right)
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

static int bluetooth_split_args(char *text, char **argv, int max_args)
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

/**
 * Append plain text to the transcript.
 *
 * Routed through the ANSI hook when the host provides one so palette macros
 * embedded in the format string are interpreted, matching every other
 * subsystem. Falls back to the plain hook when only that is registered.
 */
static void bluetooth_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    if (s_host_ops.transcript_append_ansi == NULL && s_host_ops.transcript_append_text == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (s_host_ops.transcript_append_ansi != NULL) {
        s_host_ops.transcript_append_ansi(buffer);
        return;
    }

    s_host_ops.transcript_append_text(buffer);
}

static void bluetooth_schedulef(const char *format, ...)
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

static void bluetooth_record_errorf(esp_err_t error, const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_error == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_error(BLUETOOTH_TAG, error, buffer);
}

static void bluetooth_record_warningf(const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_warning == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_warning(BLUETOOTH_TAG, buffer);
}

static void bluetooth_record_infof(const char *format, ...)
{
    char buffer[256];
    va_list args;

    if (s_host_ops.record_info == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    s_host_ops.record_info(BLUETOOTH_TAG, buffer);
}

static void bluetooth_notify_headerf(uint32_t timeout_ms, const char *format, ...)
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

#if CONFIG_BT_NIMBLE_ENABLED
static int bluetooth_gap_event(struct ble_gap_event *event, void *arg);

static void bluetooth_format_address(const ble_addr_t *address, char *output, size_t output_size)
{
    snprintf(output,
             output_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             address->val[5],
             address->val[4],
             address->val[3],
             address->val[2],
             address->val[1],
             address->val[0]);
}

static void bluetooth_extract_name(const struct ble_hs_adv_fields *fields, char *output, size_t output_size)
{
    size_t copy_len;

    output[0] = '\0';
    if (fields == NULL || fields->name == NULL || fields->name_len == 0) {
        return;
    }

    copy_len = fields->name_len;
    if (copy_len >= output_size) {
        copy_len = output_size - 1;
    }

    memcpy(output, fields->name, copy_len);
    output[copy_len] = '\0';
}

static int bluetooth_store_discovered_device(const ble_addr_t *address, int rssi, const char *name)
{
    char formatted_address[BLUETOOTH_ADDR_BYTES];
    size_t index;

    bluetooth_format_address(address, formatted_address, sizeof(formatted_address));
    for (index = 0; index < s_bluetooth_state.discovered_count; index++) {
        if (strcmp(s_bluetooth_state.discovered[index].address, formatted_address) == 0) {
            s_bluetooth_state.discovered[index].rssi = rssi;
            snprintf(s_bluetooth_state.discovered[index].name,
                     sizeof(s_bluetooth_state.discovered[index].name),
                     "%s",
                     (name != NULL && name[0] != '\0') ? name : "(unnamed)");
            return (int)index;
        }
    }

    if (s_bluetooth_state.discovered_count >= BLUETOOTH_DISCOVERY_LIMIT) {
        return -1;
    }

    snprintf(s_bluetooth_state.discovered[s_bluetooth_state.discovered_count].address,
             sizeof(s_bluetooth_state.discovered[s_bluetooth_state.discovered_count].address),
             "%s",
             formatted_address);
    snprintf(s_bluetooth_state.discovered[s_bluetooth_state.discovered_count].name,
             sizeof(s_bluetooth_state.discovered[s_bluetooth_state.discovered_count].name),
             "%s",
             (name != NULL && name[0] != '\0') ? name : "(unnamed)");
    s_bluetooth_state.discovered[s_bluetooth_state.discovered_count].rssi = rssi;
    s_bluetooth_state.discovered_count++;
    return (int)(s_bluetooth_state.discovered_count - 1);
}

static void bluetooth_on_reset(int reason)
{
    s_bluetooth_state.synced = false;
    s_bluetooth_state.last_error = ESP_FAIL;
    bluetooth_schedulef("bluetooth: host reset; reason=%d\n", reason);
}

static void bluetooth_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        s_bluetooth_state.last_error = ESP_FAIL;
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_bluetooth_state.own_addr_type);
    if (rc != 0) {
        s_bluetooth_state.last_error = ESP_FAIL;
        return;
    }

    s_bluetooth_state.synced = true;
    bluetooth_schedulef("bluetooth: BLE host synchronized with the C6 controller\n");
    bluetooth_notify_headerf(3500, "Bluetooth synced with C6");

    if (s_bluetooth_state.advertising_requested && !s_bluetooth_state.advertising_active) {
        if (bluetooth_start_advertising() == ESP_OK) {
            bluetooth_schedulef("bluetooth: advertising enabled\n");
        } else {
            s_bluetooth_state.last_error = ESP_FAIL;
            bluetooth_schedulef("bluetooth: failed to start pending advertising request after sync\n");
        }
    }

    if (s_bluetooth_state.scan_requested && !s_bluetooth_state.scan_active) {
        if (bluetooth_start_scan() == ESP_OK) {
            bluetooth_schedulef("bluetooth: passive scan started\n");
            bluetooth_notify_headerf(3500, "Bluetooth scan started");
        } else {
            s_bluetooth_state.last_error = ESP_FAIL;
            bluetooth_schedulef("bluetooth: failed to start pending scan request after sync\n");
        }
    }
}

static void bluetooth_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t bluetooth_start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    if (!s_bluetooth_state.synced) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)BLUETOOTH_DEVICE_NAME;
    fields.name_len = strlen(BLUETOOTH_DEVICE_NAME);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_bluetooth_state.own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           bluetooth_gap_event,
                           NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s_bluetooth_state.advertising_active = true;
    return ESP_OK;
}

static esp_err_t bluetooth_start_scan(void)
{
    struct ble_gap_disc_params disc_params;
    int rc;

    if (!s_bluetooth_state.synced) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.passive = 1;
    disc_params.filter_duplicates = 1;
    s_bluetooth_state.discovered_count = 0;

    rc = ble_gap_disc(s_bluetooth_state.own_addr_type,
                      BLE_HS_FOREVER,
                      &disc_params,
                      bluetooth_gap_event,
                      NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s_bluetooth_state.scan_active = true;
    return ESP_OK;
}

static int bluetooth_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        char name[BLUETOOTH_NAME_BYTES];
        int index;

        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
            bluetooth_extract_name(&fields, name, sizeof(name));
        } else {
            name[0] = '\0';
        }

        index = bluetooth_store_discovered_device(&event->disc.addr, event->disc.rssi, name);
        if (index >= 0) {
            bluetooth_schedulef("bluetooth.scan[%u]: addr=%s rssi=%d name=%s\n",
                                (unsigned int)index,
                                s_bluetooth_state.discovered[index].address,
                                s_bluetooth_state.discovered[index].rssi,
                                s_bluetooth_state.discovered[index].name);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_bluetooth_state.scan_active = false;
        s_bluetooth_state.scan_requested = false;
        bluetooth_schedulef("bluetooth: scan complete, %u device(s) reported\n",
                            (unsigned int)s_bluetooth_state.discovered_count);
        bluetooth_notify_headerf(4000,
                                 "Bluetooth scan complete: %u device(s)",
                                 (unsigned int)s_bluetooth_state.discovered_count);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_bluetooth_state.advertising_active = false;
        return 0;

    default:
        return 0;
    }
}
#endif

static esp_err_t bluetooth_refresh_fw_version(void)
{
    esp_err_t error;

    if (s_bluetooth_state.fw_version_valid) {
        return ESP_OK;
    }

    if (!s_bluetooth_state.hosted_ready) {
        error = esp_hosted_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        error = esp_hosted_connect_to_slave();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            return error;
        }

        s_bluetooth_state.hosted_ready = true;
    }

    error = esp_hosted_get_coprocessor_fwversion(&s_bluetooth_state.fw_version);
    if (error == ESP_OK) {
        s_bluetooth_state.fw_version_valid = true;
    }
    return error;
}

static esp_err_t bluetooth_ensure_ready(void)
{
#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE && CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI
    esp_err_t error;
    int rc;

    if (s_bluetooth_state.nimble_initialized) {
        s_bluetooth_state.last_error = ESP_OK;
        return ESP_OK;
    }

    error = bluetooth_refresh_fw_version();
    if (error != ESP_OK) {
        s_bluetooth_state.last_error = error;
        return error;
    }

    if (s_bluetooth_state.fw_version.major1 != ESP_HOSTED_VERSION_MAJOR_1 ||
        s_bluetooth_state.fw_version.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
#if P4_CONFIG_HOSTED_SKIP_VERSION_GATE
        ESP_LOGW("bluetooth", "Hosted version mismatch (gate skipped): host %u.%u.%u, C6 %u.%u.%u",
                 ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
                 s_bluetooth_state.fw_version.major1, s_bluetooth_state.fw_version.minor1,
                 s_bluetooth_state.fw_version.patch1);
#else
        s_bluetooth_state.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
#endif
    }

    if (!s_bluetooth_state.controller_enabled) {
        error = esp_hosted_bt_controller_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            s_bluetooth_state.last_error = error;
            return error;
        }

        error = esp_hosted_bt_controller_enable();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            s_bluetooth_state.last_error = error;
            return error;
        }
        s_bluetooth_state.controller_enabled = true;
    }

    if (!s_bluetooth_state.nimble_initialized) {
        rc = nimble_port_init();
        if (rc != 0) {
            s_bluetooth_state.last_error = ESP_FAIL;
            return ESP_FAIL;
        }

        ble_hs_cfg.reset_cb = bluetooth_on_reset;
        ble_hs_cfg.sync_cb = bluetooth_on_sync;
        ble_svc_gap_init();
        rc = ble_svc_gap_device_name_set(BLUETOOTH_DEVICE_NAME);
        if (rc != 0) {
            s_bluetooth_state.last_error = ESP_FAIL;
            return ESP_FAIL;
        }

        nimble_port_freertos_init(bluetooth_host_task);
        s_bluetooth_state.nimble_initialized = true;
    }

    s_bluetooth_state.last_error = ESP_OK;
    return ESP_OK;
#else
    s_bluetooth_state.last_error = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void bluetooth_report_not_available(esp_err_t error)
{
    if (error == ESP_ERR_INVALID_STATE && s_bluetooth_state.fw_version_valid) {
        bluetooth_appendf("bluetooth: Bluetooth not available on C6 - check firmware version (host %u.%u.x, C6 %u.%u.%u)\n",
                          ESP_HOSTED_VERSION_MAJOR_1,
                          ESP_HOSTED_VERSION_MINOR_1,
                          s_bluetooth_state.fw_version.major1,
                          s_bluetooth_state.fw_version.minor1,
                          s_bluetooth_state.fw_version.patch1);
        bluetooth_record_warningf("Hosted BLE version mismatch");
        return;
    }

    if (error == ESP_ERR_NOT_SUPPORTED) {
        bluetooth_appendf(SH_WARN "bluetooth: unavailable because hosted NimBLE is not enabled in sdkconfig" SH_RST "\n");
        bluetooth_record_warningf("Hosted NimBLE not enabled in sdkconfig");
        return;
    }

    bluetooth_appendf(SH_ERR "bluetooth: startup failed with %s (0x%x)" SH_RST "\n", esp_err_to_name(error), (unsigned int)error);
    bluetooth_record_errorf(error, "Hosted BLE startup failed");
}

void bluetooth_init(const networking_host_ops_t *ops)
{
    if (ops != NULL) {
        s_host_ops = *ops;
    }
}

void bluetooth_status(void)
{
    bluetooth_appendf(SH_LBL "bluetooth.hosted_ready:" SH_RST " %s" SH_RST "\n", s_bluetooth_state.hosted_ready ? SH_OK "yes" : SH_MUTE "no");
    bluetooth_appendf(SH_LBL "bluetooth.controller:" SH_RST " %s" SH_RST "\n", s_bluetooth_state.controller_enabled ? SH_OK "enabled" : SH_MUTE "disabled");
    bluetooth_appendf(SH_LBL "bluetooth.nimble:" SH_RST " %s" SH_RST "\n", s_bluetooth_state.nimble_initialized ? SH_OK "initialized" : SH_MUTE "off");
    bluetooth_appendf("bluetooth.synced: %s\n", s_bluetooth_state.synced ? "yes" : "no");
    bluetooth_appendf("bluetooth.scan: %s\n", s_bluetooth_state.scan_active ? "active" : "idle");
    bluetooth_appendf("bluetooth.advertise: %s\n", s_bluetooth_state.advertising_active ? "on" : "off");
    if (s_bluetooth_state.fw_version_valid) {
        bluetooth_appendf("bluetooth.c6_fw: %u.%u.%u\n",
                          s_bluetooth_state.fw_version.major1,
                          s_bluetooth_state.fw_version.minor1,
                          s_bluetooth_state.fw_version.patch1);
    }
    if (s_bluetooth_state.last_error != ESP_OK) {
        bluetooth_appendf("bluetooth.last_error: %s (0x%x)\n",
                          esp_err_to_name(s_bluetooth_state.last_error),
                          (unsigned int)s_bluetooth_state.last_error);
    }
}

void bluetooth_scan(void)
{
#if CONFIG_BT_NIMBLE_ENABLED
    esp_err_t error = bluetooth_ensure_ready();

    if (error != ESP_OK) {
        bluetooth_report_not_available(error);
        return;
    }

    if (s_bluetooth_state.scan_active) {
        bluetooth_appendf("bluetooth: scan already in progress\n");
        return;
    }

    s_bluetooth_state.scan_requested = true;
    if (!s_bluetooth_state.synced) {
        bluetooth_appendf("bluetooth: waiting for BLE host sync before scan\n");
        bluetooth_notify_headerf(3500, "Bluetooth waiting for sync");
        return;
    }

    error = bluetooth_start_scan();
    if (error != ESP_OK) {
        bluetooth_report_not_available(error);
        return;
    }

    bluetooth_appendf("bluetooth: passive scan started\n");
    bluetooth_record_infof("Passive BLE scan started");
    bluetooth_notify_headerf(3500, "Bluetooth scan started");
#else
    bluetooth_report_not_available(ESP_ERR_NOT_SUPPORTED);
#endif
}

void bluetooth_advertise(bool enable)
{
#if CONFIG_BT_NIMBLE_ENABLED
    if (!enable) {
        s_bluetooth_state.advertising_requested = false;
        if (s_bluetooth_state.advertising_active) {
            (void)ble_gap_adv_stop();
            s_bluetooth_state.advertising_active = false;
        }
        bluetooth_appendf("bluetooth: advertising disabled\n");
        return;
    }

    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return;
    }

    s_bluetooth_state.advertising_requested = true;
    if (!s_bluetooth_state.synced) {
        bluetooth_appendf("bluetooth: waiting for BLE host sync before advertising\n");
        return;
    }

    if (bluetooth_start_advertising() != ESP_OK) {
        bluetooth_report_not_available(ESP_FAIL);
        return;
    }

    bluetooth_appendf("bluetooth: advertising enabled\n");
    bluetooth_record_infof("BLE advertising enabled");
#else
    (void)enable;
    bluetooth_report_not_available(ESP_ERR_NOT_SUPPORTED);
#endif
}

bool bluetooth_is_enabled(void)
{
    return s_bluetooth_state.controller_enabled || s_bluetooth_state.nimble_initialized;
}

bool bluetooth_is_connected(void)
{
    return s_bluetooth_state.synced;
}

void bluetooth_handle_command(char *command)
{
    char *argv[4];
    int argc = bluetooth_split_args(command, argv, 4);

    if (argc <= 1 || bluetooth_text_equals_ignore_case(argv[1], "help")) {
        bluetooth_appendf("Bluetooth commands:\n");
        bluetooth_appendf("  bluetooth status            Show hosted BLE state on the ESP32-C6\n");
        bluetooth_appendf("  bluetooth scan              Run a passive BLE scan via hosted NimBLE\n");
        bluetooth_appendf("  bluetooth advertise on      Start non-connectable BLE advertising\n");
        bluetooth_appendf("  bluetooth advertise off     Stop BLE advertising\n");
        bluetooth_appendf("  bt enable                   Initialize hosted BLE without advertising\n");
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "status")) {
        bluetooth_status();
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "scan")) {
        bluetooth_scan();
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "enable")) {
        esp_err_t error = bluetooth_ensure_ready();
        if (error != ESP_OK) {
            bluetooth_report_not_available(error);
            return;
        }
        bluetooth_appendf("bluetooth: BLE host enabled on the ESP32-C6 co-processor\n");
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "advertise") && argc >= 3) {
        if (bluetooth_text_equals_ignore_case(argv[2], "on")) {
            bluetooth_advertise(true);
            return;
        }

        if (bluetooth_text_equals_ignore_case(argv[2], "off")) {
            bluetooth_advertise(false);
            return;
        }
    }

    bluetooth_appendf("Usage: bluetooth status | bluetooth scan | bluetooth advertise <on|off>\n");
}
