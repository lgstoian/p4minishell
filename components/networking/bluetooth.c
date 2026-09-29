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
#include "strutil.h"
#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_misc.h"
#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_HOST_FEAT_BT
#include "esp_hosted_bt_host_stack.h"
#endif

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
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

/* ---- BLE HID host state ---- */

/** BLE HID connection state. Tracked separately from the NimBLE sync flag
 *  because "synced" means the host stack is ready, while "device_connected"
 *  means a HID peripheral is actively attached and sending reports. */
typedef struct {
    bool connected;                 /**< A BLE HID device is connected. */
    uint16_t conn_handle;           /**< NimBLE connection handle. */
    uint16_t hid_service_handle;    /**< Handle of the HID service (0x1812). */
    uint16_t hid_service_end;       /**< End handle of the HID service. */
    uint16_t report_char_handle;    /**< Handle of the HID Report characteristic (0x2A4D). */
    uint16_t report_ccc_handle;     /**< Handle of the Report CCC descriptor. */
    uint16_t dsc_char_handle;       /**< Characteristic whose descriptors are being discovered. */
    uint16_t boot_kbd_inp_handle;   /**< Handle of the Boot Keyboard Input char (0x2A22), or 0. */
    uint16_t boot_mouse_inp_handle; /**< Handle of the Boot Mouse Input char (0x2A33), or 0. */
    bool hid_service_found;         /**< GATT discovery completed for HID service. */
    bool notifications_enabled;     /**< CCC notifications are active. */
    char address[BLUETOOTH_ADDR_BYTES]; /**< Connected device address. */
    char name[BLUETOOTH_NAME_BYTES];    /**< Connected device name. */
    int discovery_state;            /**< 0 idle, 1 discovering, 2 done. */
} ble_hid_state_t;

static ble_hid_state_t s_ble_hid;

/* Forward declarations for GATT client callbacks (defined after the GAP handler). */
static int bluetooth_disc_svc_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  const struct ble_gatt_svc *service,
                                  void *arg);
static int bluetooth_disc_chr_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  const struct ble_gatt_chr *chr,
                                  void *arg);
static int bluetooth_disc_dsc_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  uint16_t chr_val_handle,
                                  const struct ble_gatt_dsc *dsc,
                                  void *arg);
static void bluetooth_hid_cleanup(void);
static void bluetooth_process_hid_characteristics(uint16_t conn_handle);

/** HID Boot Keyboard report: 8 bytes, same format as USB HID boot protocol.
 *  Byte 0: modifier bitmask, byte 1: reserved, bytes 2-7: key codes. */
#define BLE_HID_BOOT_KBD_REPORT_LEN  8

/** Route a BLE HID boot keyboard report to the shell input path. */
static void bluetooth_hid_route_boot_keyboard(const uint8_t *report, uint16_t len);

#endif

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

/** Initiate GATT service discovery for the HID service on the connected device. */
static void bluetooth_start_hid_discovery(uint16_t conn_handle)
{
    /* 16-bit UUID for HID Service: 0x1812. Static so its address outlives the
     * asynchronous discovery call. */
    static const ble_uuid16_t hid_svc_uuid = BLE_UUID16_INIT(0x1812);
    int rc;

    s_ble_hid.discovery_state = 1;
    s_ble_hid.hid_service_handle = 0;
    s_ble_hid.hid_service_end = 0;
    s_ble_hid.report_char_handle = 0;
    s_ble_hid.report_ccc_handle = 0;
    s_ble_hid.boot_kbd_inp_handle = 0;
    s_ble_hid.boot_mouse_inp_handle = 0;
    s_ble_hid.hid_service_found = false;

    rc = ble_gattc_disc_svc_by_uuid(conn_handle, &hid_svc_uuid.u,
                                     bluetooth_disc_svc_cb, NULL);
    if (rc != 0) {
        s_ble_hid.discovery_state = 0;
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " HID service discovery start failed (rc=%d)\n", rc);
    }
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

#if CONFIG_BT_NIMBLE_ENABLED
    case BLE_GAP_EVENT_CONNECT: {
        /* A BLE connection was established (we initiated it) or failed. */
        if (event->connect.status == 0) {
            /* Connection successful. */
            s_ble_hid.connected = true;
            s_ble_hid.conn_handle = event->connect.conn_handle;
            s_ble_hid.discovery_state = 0;

            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " connected to " SH_VAL "%s" SH_RST
                              " (handle %u)\n", s_ble_hid.address, event->connect.conn_handle);
            bluetooth_notify_headerf(4000, "BLE connected");

            /* Start HID service discovery. The ATT MTU is left at the default
             * (23 bytes): boot-keyboard reports are 8 bytes, so an explicit MTU
             * exchange adds no value and would start a second GATT procedure
             * while discovery is queued. */
            bluetooth_start_hid_discovery(event->connect.conn_handle);
        } else {
            /* Connection failed. */
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " connection failed (status=%d)\n",
                              event->connect.status);
            bluetooth_record_errorf(ESP_FAIL, "BLE connection failed (status=%d)", event->connect.status);
            bluetooth_hid_cleanup();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        /* A BLE connection was terminated. */
        if (event->disconnect.conn.conn_handle == s_ble_hid.conn_handle) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " BLE HID device disconnected (reason=%d)\n",
                              event->disconnect.reason);
            bluetooth_notify_headerf(4000, "BLE keyboard disconnected");
            bluetooth_record_infof("BLE HID disconnected (reason=%d)", event->disconnect.reason);
            bluetooth_hid_cleanup();
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* A HID report (or other notification) arrived from the connected
         * device. Route boot-protocol keyboard reports to the shell input. */
        uint16_t attr_handle = event->notify_rx.attr_handle;

        if (s_ble_hid.connected &&
            event->notify_rx.conn_handle == s_ble_hid.conn_handle &&
            event->notify_rx.om != NULL &&
            (attr_handle == s_ble_hid.report_char_handle ||
             attr_handle == s_ble_hid.boot_kbd_inp_handle)) {
            uint8_t data[64];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);

            if (len > sizeof(data)) {
                len = sizeof(data);
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, data);
            if (len >= BLE_HID_BOOT_KBD_REPORT_LEN) {
                bluetooth_hid_route_boot_keyboard(data, len);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " MTU negotiated: %d bytes\n", event->mtu.value);
        return 0;
    }
#endif

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
#if CONFIG_BT_NIMBLE_ENABLED
    if (s_ble_hid.connected) {
        bluetooth_appendf("  " SH_LBL "HID device:" SH_RST " " SH_VAL "%s" SH_RST " (" SH_VAL "%s" SH_RST ")\n",
                          s_ble_hid.name[0] != '\0' ? s_ble_hid.name : "(unnamed)",
                          s_ble_hid.address);
    }
#endif
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

#if CONFIG_BT_NIMBLE_ENABLED
bool bluetooth_is_device_connected(void)
{
    return s_ble_hid.connected;
}

/* ---- BLE HID GATT client ----
 * The GATT client discovers the HID service (0x1812) on a connected peripheral
 * and subscribes to HID report notifications. Incoming reports are routed to
 * the shell input path via the same keyboard callback USB HID uses.
 *
 * NimBLE GATT client uses individual typed callbacks for each operation:
 *   ble_gatt_disc_svc_fn  for service discovery
 *   ble_gatt_chr_fn       for characteristic discovery
 *   ble_gatt_dsc_fn       for descriptor discovery */

/** Previous HID report for delta detection (press/release). */
static uint8_t s_ble_hid_prev_keys[6];

/** Route a BLE HID boot keyboard report to the shell input path. The report
 *  format matches USB HID boot protocol: modifiers in byte 0, up to 6 key
 *  codes in bytes 2-7. Emits PRESS for newly-present keys and RELEASE for keys
 *  that were in the previous report but are gone from this one. */
static void bluetooth_hid_route_boot_keyboard(const uint8_t *report, uint16_t len)
{
    uint8_t modifiers;
    uint8_t current_keys[6];
    int i, j;
    bool found;

    if (len < BLE_HID_BOOT_KBD_REPORT_LEN) {
        return;
    }

    modifiers = report[0];
    memset(current_keys, 0, sizeof(current_keys));
    for (i = 0; i < 6 && (size_t)(2 + i) < len; i++) {
        current_keys[i] = report[2 + i];
    }

    /* Emit RELEASE for keys that were pressed but are no longer in the report. */
    for (i = 0; i < 6; i++) {
        if (s_ble_hid_prev_keys[i] == 0) {
            continue;
        }
        found = false;
        for (j = 0; j < 6; j++) {
            if (current_keys[j] == s_ble_hid_prev_keys[i]) {
                found = true;
                break;
            }
        }
        if (!found && s_host_ops.bluetooth_keyboard_input != NULL) {
            s_host_ops.bluetooth_keyboard_input(s_ble_hid_prev_keys[i], 0, false);
        }
    }

    /* Emit PRESS for newly-pressed keys. */
    for (i = 0; i < 6; i++) {
        if (current_keys[i] == 0) {
            continue;
        }
        found = false;
        for (j = 0; j < 6; j++) {
            if (current_keys[j] == current_keys[i] && j < i) {
                found = true;
                break;
            }
        }
        if (!found && s_host_ops.bluetooth_keyboard_input != NULL) {
            s_host_ops.bluetooth_keyboard_input(current_keys[i], modifiers, true);
        }
    }

    memcpy(s_ble_hid_prev_keys, current_keys, sizeof(s_ble_hid_prev_keys));
}

/** Callback for service discovery by UUID. Called once per matching service. */
static int bluetooth_disc_svc_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  const struct ble_gatt_svc *service,
                                  void *arg)
{
    (void)arg;

    if (error != NULL && error->status != 0) {
        s_ble_hid.discovery_state = 0;
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " HID service discovery error (status=%d)\n",
                          error->status);
        return 0;
    }

    if (service == NULL) {
        /* Discovery complete (no more services). If HID service not found, report it. */
        if (!s_ble_hid.hid_service_found) {
            s_ble_hid.discovery_state = 0;
            bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " HID service (0x1812) not found on device\n");
        }
        return 0;
    }

    /* Found the HID service. */
    s_ble_hid.hid_service_handle = service->start_handle;
    s_ble_hid.hid_service_end = service->end_handle;
    s_ble_hid.hid_service_found = true;

    bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " HID service found at handles %u-%u\n",
                      service->start_handle, service->end_handle);

    /* Now discover characteristics within the HID service. */
    {
        int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                          service->end_handle,
                                          bluetooth_disc_chr_cb, NULL);
        if (rc != 0) {
            s_ble_hid.discovery_state = 0;
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " characteristic discovery failed (rc=%d)\n", rc);
        }
    }

    return 0;
}

/** Callback for characteristic discovery. Called once per characteristic. */
static int bluetooth_disc_chr_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  const struct ble_gatt_chr *chr,
                                  void *arg)
{
    (void)arg;

    if (error != NULL && error->status != 0) {
        return 0;
    }

    if (chr == NULL) {
        /* Discovery complete. Now subscribe to notifications. */
        bluetooth_process_hid_characteristics(conn_handle);
        return 0;
    }

    if (chr->uuid.u.type != BLE_UUID_TYPE_16) {
        return 0;
    }

    switch (ble_uuid_u16(&chr->uuid.u)) {
    case 0x2A4D: /* HID Report */
        s_ble_hid.report_char_handle = chr->val_handle;
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " found HID Report handle=%u\n", chr->val_handle);
        break;
    case 0x2A22: /* Boot Keyboard Input */
        s_ble_hid.boot_kbd_inp_handle = chr->val_handle;
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " found Boot Keyboard handle=%u\n", chr->val_handle);
        break;
    case 0x2A33: /* Boot Mouse Input */
        s_ble_hid.boot_mouse_inp_handle = chr->val_handle;
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " found Boot Mouse handle=%u\n", chr->val_handle);
        break;
    case 0x2A4E: { /* Protocol Mode - set boot protocol */
        uint8_t proto = 0x00;
        (void)ble_gattc_write_flat(conn_handle, chr->val_handle,
                                   &proto, sizeof(proto), NULL, NULL);
        break;
    }
    default:
        break;
    }

    return 0;
}

/** After characteristic discovery, subscribe to the best HID input. */
static void bluetooth_process_hid_characteristics(uint16_t conn_handle)
{
    int rc;

    /* Prefer HID Report Input (0x2A4D), fall back to Boot Keyboard Input (0x2A22). */
    if (s_ble_hid.report_char_handle != 0) {
        /* Discover the CCC descriptor for the Report characteristic. */
        s_ble_hid.dsc_char_handle = s_ble_hid.report_char_handle;
        rc = ble_gattc_disc_all_dscs(conn_handle, s_ble_hid.report_char_handle,
                                      s_ble_hid.hid_service_end,
                                      bluetooth_disc_dsc_cb, NULL);
        if (rc == 0) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " discovering HID Report CCC\n");
            return;
        }
    }

    if (s_ble_hid.boot_kbd_inp_handle != 0) {
        s_ble_hid.dsc_char_handle = s_ble_hid.boot_kbd_inp_handle;
        rc = ble_gattc_disc_all_dscs(conn_handle, s_ble_hid.boot_kbd_inp_handle,
                                      s_ble_hid.hid_service_end,
                                      bluetooth_disc_dsc_cb, NULL);
        if (rc == 0) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " discovering Boot Keyboard CCC\n");
            return;
        }
    }

    s_ble_hid.discovery_state = 0;
    bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " no usable HID input found\n");
}

/** Callback for descriptor discovery. Called once per descriptor. */
static int bluetooth_disc_dsc_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  uint16_t chr_val_handle,
                                  const struct ble_gatt_dsc *dsc,
                                  void *arg)
{
    (void)arg;
    (void)chr_val_handle;

    if (error != NULL && error->status != 0) {
        return 0;
    }

    if (dsc == NULL) {
        /* Descriptor discovery complete. If no CCC was found for the Report
         * characteristic, fall back to the Boot Keyboard characteristic. */
        if (!s_ble_hid.notifications_enabled) {
            if (s_ble_hid.dsc_char_handle == s_ble_hid.report_char_handle &&
                s_ble_hid.boot_kbd_inp_handle != 0) {
                s_ble_hid.dsc_char_handle = s_ble_hid.boot_kbd_inp_handle;
                if (ble_gattc_disc_all_dscs(conn_handle, s_ble_hid.boot_kbd_inp_handle,
                                            s_ble_hid.hid_service_end,
                                            bluetooth_disc_dsc_cb, NULL) == 0) {
                    return 0;
                }
            }
            s_ble_hid.discovery_state = 0;
            bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " no HID notification descriptor found\n");
        }
        return 0;
    }

    /* Look for the Client Characteristic Configuration descriptor (0x2902). */
    if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        uint8_t val[2] = { 0x01, 0x00 }; /* Enable notifications */
        int rc = ble_gattc_write_flat(conn_handle, dsc->handle, val, sizeof(val), NULL, NULL);

        if (rc == 0) {
            s_ble_hid.report_ccc_handle = dsc->handle;
            s_ble_hid.notifications_enabled = true;
            s_ble_hid.discovery_state = 0;
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " BLE HID notifications enabled\n");
            bluetooth_notify_headerf(4000, "BLE keyboard connected");
            bluetooth_record_infof("BLE HID connected: %s", s_ble_hid.name);
        } else {
            s_ble_hid.discovery_state = 0;
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " CCC write failed (rc=%d)\n", rc);
        }
    }

    return 0;
}

/** Connect to a BLE device by MAC address. */
esp_err_t bluetooth_connect(const char *addr_str)
{
    ble_addr_t addr;
    struct ble_gap_conn_params conn_params;
    int rc;
    unsigned int a[6];

    if (s_ble_hid.connected) {
        bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " already connected to " SH_VAL "%s" SH_RST "\n",
                          s_ble_hid.address);
        return ESP_ERR_INVALID_STATE;
    }

    if (addr_str == NULL || addr_str[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse XX:XX:XX:XX:XX:XX address. */
    if (sscanf(addr_str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        bluetooth_appendf(SH_USAGE "Usage: bluetooth connect XX:XX:XX:XX:XX:XX" SH_RST "\n");
        return ESP_ERR_INVALID_ARG;
    }

    addr.type = BLE_OWN_ADDR_PUBLIC;
    addr.val[0] = (uint8_t)a[5];
    addr.val[1] = (uint8_t)a[4];
    addr.val[2] = (uint8_t)a[3];
    addr.val[3] = (uint8_t)a[2];
    addr.val[4] = (uint8_t)a[1];
    addr.val[5] = (uint8_t)a[0];

    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl = P4_CONFIG_BLE_CONN_SCAN_ITVL;
    conn_params.scan_window = P4_CONFIG_BLE_CONN_SCAN_WINDOW;
    conn_params.itvl_min = P4_CONFIG_BLE_CONN_ITVL_MIN;
    conn_params.itvl_max = P4_CONFIG_BLE_CONN_ITVL_MAX;
    conn_params.latency = P4_CONFIG_BLE_CONN_LATENCY;
    conn_params.supervision_timeout = P4_CONFIG_BLE_CONN_SUPERVISION_TIMEOUT;
    conn_params.min_ce_len = 0;
    conn_params.max_ce_len = 0;

    /* Stop any active scan before connecting. */
    if (s_bluetooth_state.scan_active) {
        (void)ble_gap_disc_cancel();
        s_bluetooth_state.scan_active = false;
    }

    /* Stop advertising before connecting. */
    if (s_bluetooth_state.advertising_active) {
        (void)ble_gap_adv_stop();
        s_bluetooth_state.advertising_active = false;
    }

    /* Remember the target address/name before the async connection callback
     * can fire, and reuse the name from a prior scan when available. */
    snprintf(s_ble_hid.address, sizeof(s_ble_hid.address), "%s", addr_str);
    s_ble_hid.name[0] = '\0';
    {
        size_t index;

        for (index = 0; index < s_bluetooth_state.discovered_count; index++) {
            if (strutil_text_equals_ignore_case(s_bluetooth_state.discovered[index].address,
                                                addr_str)) {
                snprintf(s_ble_hid.name, sizeof(s_ble_hid.name), "%s",
                         s_bluetooth_state.discovered[index].name);
                break;
            }
        }
    }

    rc = ble_gap_connect(s_bluetooth_state.own_addr_type, &addr,
                          P4_CONFIG_BLE_CONNECT_TIMEOUT_MS,
                          &conn_params,
                          bluetooth_gap_event, NULL);
    if (rc != 0) {
        s_ble_hid.address[0] = '\0';
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " connect failed (rc=%d)\n", rc);
        return ESP_FAIL;
    }

    bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST " connecting to " SH_VAL "%s" SH_RST "...\n", addr_str);
    bluetooth_record_infof("BLE connecting to %s", addr_str);
    return ESP_OK;
}

/** Disconnect from the current BLE HID device. */
void bluetooth_disconnect(void)
{
    if (!s_ble_hid.connected) {
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " no BLE device connected\n");
        return;
    }

    (void)ble_gap_terminate(s_ble_hid.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " disconnected from " SH_VAL "%s" SH_RST "\n",
                      s_ble_hid.address);
    bluetooth_record_infof("BLE disconnected from %s", s_ble_hid.address);
}

/** Clean up BLE HID state on disconnection. */
static void bluetooth_hid_cleanup(void)
{
    s_ble_hid.connected = false;
    s_ble_hid.conn_handle = 0;
    s_ble_hid.hid_service_handle = 0;
    s_ble_hid.hid_service_end = 0;
    s_ble_hid.report_char_handle = 0;
    s_ble_hid.report_ccc_handle = 0;
    s_ble_hid.dsc_char_handle = 0;
    s_ble_hid.boot_kbd_inp_handle = 0;
    s_ble_hid.boot_mouse_inp_handle = 0;
    s_ble_hid.hid_service_found = false;
    s_ble_hid.notifications_enabled = false;
    s_ble_hid.discovery_state = 0;
    memset(s_ble_hid_prev_keys, 0, sizeof(s_ble_hid_prev_keys));
}
#else  /* !CONFIG_BT_NIMBLE_ENABLED */
/* NimBLE is disabled: the BLE HID host compiles to no-ops so the shell command
 * surface stays link-complete (the verbs report unavailable). */
bool bluetooth_is_device_connected(void)
{
    return false;
}

esp_err_t bluetooth_connect(const char *addr_str)
{
    (void)addr_str;
    return ESP_ERR_NOT_SUPPORTED;
}

void bluetooth_disconnect(void)
{
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

void bluetooth_handle_command(char *command)
{
    char *argv[6];
    int argc = strutil_split_args(command, argv, 6);

    if (argc <= 1 || strutil_text_equals_ignore_case(argv[1], "help")) {
        bluetooth_appendf(SH_SUBHEAD SH_BOLD "Bluetooth Commands:" SH_RST "\n");
        bluetooth_appendf("  " SH_CMD "bluetooth status" SH_RST "            Show hosted BLE state on the ESP32-C6\n");
        bluetooth_appendf("  " SH_CMD "bluetooth scan" SH_RST " [limit]     Bounded passive BLE scan, sorted by RSSI\n");
        bluetooth_appendf("  " SH_CMD "bluetooth advertise on" SH_RST " [name]  Start non-connectable advertising (session name)\n");
        bluetooth_appendf("  " SH_CMD "bluetooth advertise off" SH_RST "     Stop BLE advertising\n");
        bluetooth_appendf("  " SH_CMD "bluetooth connect" SH_RST " XX:XX:XX:XX:XX:XX  Connect to a BLE HID device\n");
        bluetooth_appendf("  " SH_CMD "bluetooth disconnect" SH_RST "       Disconnect from the current BLE device\n");
        bluetooth_appendf("  " SH_CMD "bt ..." SH_RST "                      Alias for the bluetooth family\n");
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "status")) {
        bluetooth_status();
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "scan")) {
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

    if (strutil_text_equals_ignore_case(argv[1], "enable")) {
        esp_err_t error = bluetooth_ensure_ready();
        if (error != ESP_OK) {
            bluetooth_report_not_available(error);
            return;
        }
        bluetooth_appendf(SH_OK "bluetooth:" SH_RST " BLE host enabled on the ESP32-C6 co-processor\n");
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "advertise") && argc >= 3) {
        if (strutil_text_equals_ignore_case(argv[2], "on")) {
            /* The optional name is session-only and never persisted. */
            bluetooth_advertise(true, argc >= 4 ? argv[3] : NULL);
            return;
        }

        if (strutil_text_equals_ignore_case(argv[2], "off")) {
            bluetooth_advertise(false, NULL);
            return;
        }
    }

#if CONFIG_BT_NIMBLE_ENABLED
    if (strutil_text_equals_ignore_case(argv[1], "connect") && argc >= 3) {
        esp_err_t error = bluetooth_connect(argv[2]);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " connect failed (%s)\n", esp_err_to_name(error));
        }
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "disconnect")) {
        bluetooth_disconnect();
        return;
    }
#endif

    bluetooth_appendf(SH_USAGE "Usage: bluetooth status | scan [limit] | advertise <on [name]|off> | connect <addr> | disconnect" SH_RST "\n");
}
