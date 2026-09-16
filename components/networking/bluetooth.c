/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_misc.h"
#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_HOST_FEAT_BT
#include "esp_hosted_bt_host_stack.h"
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#endif

#include "bluetooth.h"
#include "ansi.h"
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
    /** Result cap for the current scan run (0 = config default). */
    int scan_limit;
    /** Advertising name chosen by `bluetooth advertise on [name]`. This is
     *  session-only: it never survives a reboot, matching the RAM-only shell
     *  environment contract. Empty means "use the configured default name". */
    char session_advertise_name[BLUETOOTH_NAME_BYTES];
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
    ansi_vformat(buffer, sizeof(buffer), format, args);
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

    const char *advertise_name = s_bluetooth_state.session_advertise_name[0] != '\0'
                                     ? s_bluetooth_state.session_advertise_name
                                     : BLUETOOTH_DEVICE_NAME;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)advertise_name;
    fields.name_len = strlen(advertise_name);
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

    /* Bounded duration (milliseconds) so the scan always ends and the shell
     * command never hangs waiting on BLE_HS_FOREVER. The duration is the
     * second argument of ble_gap_disc, not a field of disc_params. */
    rc = ble_gap_disc(s_bluetooth_state.own_addr_type,
                      (int32_t)P4_CONFIG_BT_SCAN_DURATION_MS,
                      &disc_params,
                      bluetooth_gap_event,
                      NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s_bluetooth_state.scan_active = true;
    return ESP_OK;
}

/**
 * Sort the discovered devices by RSSI (strongest first) and print the result
 * as an aligned name / address / RSSI report, capped by the scan limit.
 *
 * Called once when the bounded scan ends. Also registered for the pending
 * scan request in bluetooth_on_sync via BLE_GAP_EVENT_DISC_COMPLETE.
 */
static void bluetooth_report_scan_results(void)
{
    size_t count = s_bluetooth_state.discovered_count;
    size_t limit = count;
    size_t index;
    size_t inner;

    if (s_bluetooth_state.scan_limit > 0 && limit > (size_t)s_bluetooth_state.scan_limit) {
        limit = (size_t)s_bluetooth_state.scan_limit;
    }

    /* Selection sort by RSSI, strongest first. The device set is bounded by
     * BLUETOOTH_DISCOVERY_LIMIT (16), so the O(n^2) loop is trivial. */
    for (index = 0; index < count; index++) {
        for (inner = index + 1; inner < count; inner++) {
            if (s_bluetooth_state.discovered[inner].rssi > s_bluetooth_state.discovered[index].rssi) {
                bluetooth_device_t swap = s_bluetooth_state.discovered[index];
                s_bluetooth_state.discovered[index] = s_bluetooth_state.discovered[inner];
                s_bluetooth_state.discovered[inner] = swap;
            }
        }
    }

    bluetooth_appendf(SH_HEAD "  %-32s %-17s %s" SH_RST "\n", "NAME", "ADDRESS", "RSSI");
    for (index = 0; index < limit; index++) {
        bluetooth_appendf("  " SH_VAL "%-32s" SH_RST " " SH_VAL "%-17s" SH_RST " " SH_NUM "%d" SH_RST "\n",
                          s_bluetooth_state.discovered[index].name,
                          s_bluetooth_state.discovered[index].address,
                          s_bluetooth_state.discovered[index].rssi);
    }
    bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " scan complete, " SH_NUM "%u" SH_RST
                      " device(s) found (%u shown)\n",
                      (unsigned int)count, (unsigned int)limit);
    bluetooth_notify_headerf(4000,
                             "Bluetooth scan complete: %u device(s)",
                             (unsigned int)count);
}

static int bluetooth_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        char name[BLUETOOTH_NAME_BYTES];

        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
            bluetooth_extract_name(&fields, name, sizeof(name));
        } else {
            name[0] = '\0';
        }

        /* Collect every device first; the sorted report is printed once the
         * bounded scan ends (BLE_GAP_EVENT_DISC_COMPLETE). */
        (void)bluetooth_store_discovered_device(&event->disc.addr, event->disc.rssi, name);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_bluetooth_state.scan_active = false;
        s_bluetooth_state.scan_requested = false;
        bluetooth_report_scan_results();
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
#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_HOST_FEAT_BT
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

    /* Major-only gate (see networking.c): esp_hosted 3.x froze its public
     * compat version macros at 2.12.6, so compare the C6-reported major
     * against P4_CONFIG_HOSTED_COMPAT_MAJOR. */
    if (s_bluetooth_state.fw_version.major1 != P4_CONFIG_HOSTED_COMPAT_MAJOR) {
#if P4_CONFIG_HOSTED_SKIP_VERSION_GATE
        ESP_LOGW("bluetooth", "Hosted version mismatch (gate skipped): host %u.x, C6 %u.%u.%u",
                 P4_CONFIG_HOSTED_COMPAT_MAJOR,
                 s_bluetooth_state.fw_version.major1, s_bluetooth_state.fw_version.minor1,
                 s_bluetooth_state.fw_version.patch1);
#else
        s_bluetooth_state.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
#endif
    }

    if (!s_bluetooth_state.controller_enabled) {
        esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
        error = esp_hosted_bt_host_stack_setup(&cfg);
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
            /* nimble is up but the flag is not set: tear it down so a retry
             * does not call nimble_port_init() on an initialized stack. */
            nimble_port_deinit();
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
        bluetooth_appendf("bluetooth: Bluetooth not available on C6 - check firmware version (host %u.x, C6 %u.%u.%u)\n",
                          P4_CONFIG_HOSTED_COMPAT_MAJOR,
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

/** Append a "label: coloured value" line. The colour is placed into the
 *  format string (not a %s argument) so ansi_vformat converts it to a real
 *  SGR escape instead of leaving a literal @-marker on the transcript. */
static void bluetooth_append_coloured(const char *label, const char *colour, const char *value)
{
    char fmt[128];

    snprintf(fmt, sizeof(fmt), "  " SH_LBL "%%s:" SH_RST " %s%%s%s\n", colour, SH_RST);
    bluetooth_appendf(fmt, label, value);
}

void bluetooth_status(void)
{
    bluetooth_appendf(SH_HEAD "Bluetooth Status" SH_RST "\n");
    bluetooth_append_coloured("hosted ready",
                              s_bluetooth_state.hosted_ready ? SH_OK : SH_MUTE,
                              s_bluetooth_state.hosted_ready ? "yes" : "no");
    bluetooth_append_coloured("controller",
                              s_bluetooth_state.controller_enabled ? SH_OK : SH_MUTE,
                              s_bluetooth_state.controller_enabled ? "enabled" : "disabled");
    bluetooth_append_coloured("NimBLE host",
                              s_bluetooth_state.nimble_initialized ? SH_OK : SH_MUTE,
                              s_bluetooth_state.nimble_initialized ? "initialized" : "off");
    bluetooth_append_coloured("synced",
                              s_bluetooth_state.synced ? SH_OK : SH_MUTE,
                              s_bluetooth_state.synced ? "yes" : "no");
    bluetooth_append_coloured("scan",
                              s_bluetooth_state.scan_active ? SH_WARN : SH_MUTE,
                              s_bluetooth_state.scan_active ? "active" : "idle");
    if (s_bluetooth_state.advertising_active) {
        bluetooth_appendf("  " SH_LBL "advertising:" SH_RST " " SH_BT "on" SH_RST
                          " (name " SH_VAL "%s" SH_RST ")\n",
                          s_bluetooth_state.session_advertise_name[0] != '\0'
                              ? s_bluetooth_state.session_advertise_name
                              : BLUETOOTH_DEVICE_NAME);
    } else {
        bluetooth_appendf("  " SH_LBL "advertising:" SH_RST " " SH_MUTE "off" SH_RST "\n");
    }
    if (s_bluetooth_state.fw_version_valid) {
        bluetooth_appendf("  " SH_LBL "C6 firmware:" SH_RST " " SH_NUM "%u.%u.%u" SH_RST "\n",
                          s_bluetooth_state.fw_version.major1,
                          s_bluetooth_state.fw_version.minor1,
                          s_bluetooth_state.fw_version.patch1);
    }
    if (s_bluetooth_state.last_error != ESP_OK) {
        bluetooth_appendf("  " SH_LBL "last error:" SH_RST " " SH_ERR "%s" SH_RST " (0x%x)\n",
                          esp_err_to_name(s_bluetooth_state.last_error),
                          (unsigned int)s_bluetooth_state.last_error);
    }
}

void bluetooth_scan(int limit)
{
#if CONFIG_BT_NIMBLE_ENABLED
    esp_err_t error = bluetooth_ensure_ready();

    if (error != ESP_OK) {
        bluetooth_report_not_available(error);
        return;
    }

    if (s_bluetooth_state.scan_active) {
        bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " a scan is already in progress\n");
        return;
    }

    s_bluetooth_state.scan_limit = (limit > 0) ? limit : P4_CONFIG_BT_SCAN_LIMIT;
    if (s_bluetooth_state.scan_limit > BLUETOOTH_DISCOVERY_LIMIT) {
        s_bluetooth_state.scan_limit = BLUETOOTH_DISCOVERY_LIMIT;
    }

    s_bluetooth_state.scan_requested = true;
    if (!s_bluetooth_state.synced) {
        bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST " waiting for BLE host sync before scan\n");
        bluetooth_notify_headerf(3500, "Bluetooth waiting for sync");
        return;
    }

    error = bluetooth_start_scan();
    if (error != ESP_OK) {
        bluetooth_report_not_available(error);
        return;
    }

    bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST " passive scan started for " SH_NUM "%dms" SH_RST " (max " SH_NUM "%d" SH_RST " devices)\n",
                      (int)P4_CONFIG_BT_SCAN_DURATION_MS, s_bluetooth_state.scan_limit);
    bluetooth_record_infof("Passive BLE scan started");
    bluetooth_notify_headerf(3500, "Bluetooth scan started");
#else
    (void)limit;
    bluetooth_report_not_available(ESP_ERR_NOT_SUPPORTED);
#endif
}

void bluetooth_advertise(bool enable, const char *name)
{
#if CONFIG_BT_NIMBLE_ENABLED
    if (name != NULL && name[0] != '\0') {
        snprintf(s_bluetooth_state.session_advertise_name,
                 sizeof(s_bluetooth_state.session_advertise_name), "%s", name);
    }

    if (!enable) {
        s_bluetooth_state.advertising_requested = false;
        if (s_bluetooth_state.advertising_active) {
            (void)ble_gap_adv_stop();
            s_bluetooth_state.advertising_active = false;
        }
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " advertising disabled\n");
        return;
    }

    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return;
    }

    s_bluetooth_state.advertising_requested = true;
    if (!s_bluetooth_state.synced) {
        bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST " waiting for BLE host sync before advertising\n");
        return;
    }

    if (bluetooth_start_advertising() != ESP_OK) {
        bluetooth_report_not_available(ESP_FAIL);
        return;
    }

    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " advertising enabled as " SH_VAL "%s" SH_RST "\n",
                      s_bluetooth_state.session_advertise_name[0] != '\0'
                          ? s_bluetooth_state.session_advertise_name
                          : BLUETOOTH_DEVICE_NAME);
    bluetooth_record_infof("BLE advertising enabled");
#else
    (void)enable;
    (void)name;
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
    char *argv[6];
    int argc = bluetooth_split_args(command, argv, 6);

    if (argc <= 1 || bluetooth_text_equals_ignore_case(argv[1], "help")) {
        bluetooth_appendf(SH_SUBHEAD SH_BOLD "Bluetooth Commands:" SH_RST "\n");
        bluetooth_appendf("  " SH_CMD "bluetooth status" SH_RST "            Show hosted BLE state on the ESP32-C6\n");
        bluetooth_appendf("  " SH_CMD "bluetooth scan" SH_RST " [limit]     Bounded passive BLE scan, sorted by RSSI\n");
        bluetooth_appendf("  " SH_CMD "bluetooth advertise on" SH_RST " [name]  Start non-connectable advertising (session name)\n");
        bluetooth_appendf("  " SH_CMD "bluetooth advertise off" SH_RST "     Stop BLE advertising\n");
        bluetooth_appendf("  " SH_CMD "bt ..." SH_RST "                      Alias for the bluetooth family\n");
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "status")) {
        bluetooth_status();
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "scan")) {
        int limit = 0;

        if (argc >= 3) {
            char *end = NULL;
            long parsed = strtol(argv[2], &end, 10);

            if (end == NULL || *end != '\0' || parsed < 1 || parsed > BLUETOOTH_DISCOVERY_LIMIT) {
                bluetooth_appendf(SH_USAGE "Usage: bluetooth scan [1..%d]" SH_RST "\n", (int)BLUETOOTH_DISCOVERY_LIMIT);
                return;
            }
            limit = (int)parsed;
        }
        bluetooth_scan(limit);
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "enable")) {
        esp_err_t error = bluetooth_ensure_ready();
        if (error != ESP_OK) {
            bluetooth_report_not_available(error);
            return;
        }
        bluetooth_appendf(SH_OK "bluetooth:" SH_RST " BLE host enabled on the ESP32-C6 co-processor\n");
        return;
    }

    if (bluetooth_text_equals_ignore_case(argv[1], "advertise") && argc >= 3) {
        if (bluetooth_text_equals_ignore_case(argv[2], "on")) {
            /* The optional name is session-only and never persisted. */
            bluetooth_advertise(true, argc >= 4 ? argv[3] : NULL);
            return;
        }

        if (bluetooth_text_equals_ignore_case(argv[2], "off")) {
            bluetooth_advertise(false, NULL);
            return;
        }
    }

    bluetooth_appendf(SH_USAGE "Usage: bluetooth status | bluetooth scan [limit] | bluetooth advertise <on [name]|off>" SH_RST "\n");
}
