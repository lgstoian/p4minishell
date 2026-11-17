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
#include "display.h"
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
static esp_err_t bluetooth_start_advertising_ex(bool connectable);
static esp_err_t bluetooth_ensure_ready(void);
static void bluetooth_report_not_available(esp_err_t error);

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
    bool mouse_echo;                /**< Report BLE mouse motion to the transcript. */
    int32_t mouse_x;                /**< Accumulated X (echo only). */
    int32_t mouse_y;                /**< Accumulated Y (echo only). */
} ble_hid_state_t;

static ble_hid_state_t s_ble_hid;

/* ---- BLE chat (board-to-board) ---------------------------------------- *
 * A tiny GATT service, no Wi-Fi, broker or cable:
 *   0xFFF2  service
 *   0xFFF3  RX: peer writes a message here (server prints it)
 *   0xFFF4  TX: server notifies a message here (client subscribes)
 * One board runs the connectable server (`bluetooth chat on`), the other
 * joins by address (`bluetooth chat join <addr>`) and both can send.
 * Messages are capped to the negotiated ATT MTU minus the 3-byte header. */
#define BT_CHAT_SVC_UUID   0xFFF2
#define BT_CHAT_RX_UUID    0xFFF3
#define BT_CHAT_TX_UUID    0xFFF4
#define BT_CHAT_MSG_BYTES  160

/** What the pending/active BLE connection is for. The GAP event handler is
 *  shared, so this decides whether a connection starts HID discovery, chat
 *  client discovery, or is an inbound chat-server peer. */
typedef enum {
    BT_LINK_NONE = 0,
    BT_LINK_HID,
    BT_LINK_CHAT_CLIENT,
} bt_link_role_t;

typedef struct {
    bool server_enabled;    /**< Connectable chat server is running. */
    bool peer_connected;    /**< A chat peer is connected (either role). */
    bool client;            /**< We initiated (central) the chat link. */
    uint16_t conn_handle;
    uint16_t rx_handle;     /**< 0xFFF3 (client: discovered). */
    uint16_t tx_handle;     /**< 0xFFF4 (server: NimBLE sets on registration). */
    uint16_t ccc_handle;    /**< TX CCCD (client: discovered). */
    uint16_t svc_start;
    uint16_t svc_end;
    bool discovery_done;
    uint16_t mtu;           /**< Negotiated ATT MTU (23 until exchanged). */
    char peer[BLUETOOTH_ADDR_BYTES];
    char name[BLUETOOTH_NAME_BYTES];
    uint32_t rx_count;
    uint32_t tx_count;
} bt_chat_state_t;

static bt_chat_state_t s_chat;
static bt_link_role_t s_bt_link_role;
static uint16_t s_chat_server_tx_handle;  /* 0xFFF4 value handle, set at registration */
static const ble_uuid16_t s_chat_svc_uuid = BLE_UUID16_INIT(BT_CHAT_SVC_UUID);

static int bluetooth_gatt_chat_rx(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
static void bluetooth_chat_start_discovery(uint16_t conn_handle);
static void bluetooth_chat_cleanup(void);

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

/** HID Boot Mouse report: buttons(1) + X(1) + Y(1), optional wheel(1).
 *  Same layout as the USB boot mouse report. */
#define BLE_HID_BOOT_MOUSE_REPORT_LEN  3

/** Route a BLE HID boot keyboard report to the shell input path. */
static void bluetooth_hid_route_boot_keyboard(const uint8_t *report, uint16_t len);

/** Route a BLE HID boot mouse report: wheel scrolls the transcript (always),
 *  motion/buttons echo when `bluetooth mouse on` is set. Layout mirrors
 *  usb_handle_mouse_input_locked (components/usb/usb.c); kept separate
 *  because the side-effect paths differ (ops table vs direct emit) and no
 *  shared HID component owns both transports. */
static void bluetooth_hid_route_boot_mouse(const uint8_t *report, uint16_t len);

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

/* ========================================================================
 * GATT SERVER (peripheral role)
 * ========================================================================
 * One GATT server: a standard Device Information service (manufacturer,
 * model, firmware revision) plus a small P4MiniShell status characteristic.
 * `bluetooth gatt on` registers it and switches advertising to connectable so
 * a central can reach it.
 */

static bool s_bluetooth_gatt_enabled;
static bool s_bluetooth_gatt_registered;

static int bluetooth_gatt_read_str(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *value = (const char *)arg;

    (void)conn_handle;
    (void)attr_handle;
    if (value != NULL) {
        os_mbuf_append(ctxt->om, value, strlen(value));
    }
    return 0;
}

static int bluetooth_gatt_read_status(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char buf[160];

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    snprintf(buf, sizeof(buf), "fw=%s wifi=%s",
             P4_CONFIG_VERSION_STRING, networking_wifi_state_string());
    os_mbuf_append(ctxt->om, buf, strlen(buf));
    return 0;
}

/** Notify-only characteristic access callback: reads return an empty value.
 *  NimBLE requires a non-NULL callback even when only NOTIFY is allowed. */
static int bluetooth_gatt_read_none(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

/** Print one received chat line as plain text. Remote content is untrusted:
 *  control bytes become '.', and the plain sink is used so a `@R` sequence in
 *  a message cannot inject ANSI palette commands. */
static void bluetooth_chat_print_rx(const char *text)
{
    char line[BT_CHAT_MSG_BYTES + 16];
    size_t out = 0;
    size_t i;

    out = (size_t)snprintf(line, sizeof(line), "[bt] ");
    if (text != NULL) {
        for (i = 0; text[i] != '\0' && out + 1 < sizeof(line) - 1; i++) {
            unsigned char c = (unsigned char)text[i];

            line[out++] = (c < 0x20 && c != '\t') ? '.' : (char)c;
        }
    }
    line[out++] = '\n';
    line[out] = '\0';
    if (s_host_ops.transcript_append_text != NULL) {
        s_host_ops.transcript_append_text(line);
    } else {
        bluetooth_appendf("%s", line);
    }
}

/** 0xFFF3 write handler: the peer sent a message. */
static int bluetooth_gatt_chat_rx(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char text[BT_CHAT_MSG_BYTES];
    uint16_t len;
    uint16_t copy;

    (void)attr_handle;
    (void)arg;
    if (ctxt == NULL || ctxt->om == NULL) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    len = OS_MBUF_PKTLEN(ctxt->om);
    copy = (uint16_t)((len < sizeof(text) - 1) ? len : sizeof(text) - 1);
    os_mbuf_copydata(ctxt->om, 0, copy, text);
    text[copy] = '\0';
    if (copy == 0) {
        return 0;
    }

    s_chat.rx_count++;
    s_chat.conn_handle = conn_handle;
    s_chat.peer_connected = true;
    bluetooth_chat_print_rx(text);
    bluetooth_notify_headerf(3000, "Bluetooth message");
    return 0;
}

static const struct ble_gatt_svc_def g_bluetooth_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),       /* Device Information */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), /* Manufacturer Name */
              .access_cb = bluetooth_gatt_read_str,
              .arg = (void *)"P4MiniShell",
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A24), /* Model Number */
              .access_cb = bluetooth_gatt_read_str,
              .arg = (void *)BLUETOOTH_DEVICE_NAME,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A26), /* Firmware Revision */
              .access_cb = bluetooth_gatt_read_str,
              .arg = (void *)P4_CONFIG_VERSION_STRING,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),       /* P4MiniShell status */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0xFFF1),
              .access_cb = bluetooth_gatt_read_status,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BT_CHAT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(BT_CHAT_RX_UUID),
              .access_cb = bluetooth_gatt_chat_rx,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(BT_CHAT_TX_UUID),
              .access_cb = bluetooth_gatt_read_none,
              .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_chat_server_tx_handle },
            { 0 },
        },
    },
    { 0 },
};

static bool bluetooth_gatt_register(void)
{
    int rc;

    if (s_bluetooth_gatt_registered) {
        return true;
    }
    rc = ble_gatts_count_cfg(g_bluetooth_gatt_svcs);
    if (rc != 0) {
        bluetooth_record_errorf(ESP_FAIL, "GATT resource count failed (rc=%d)", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(g_bluetooth_gatt_svcs);
    if (rc != 0) {
        bluetooth_record_errorf(ESP_FAIL, "GATT service registration failed (rc=%d)", rc);
        return false;
    }
    s_bluetooth_gatt_registered = true;
    return true;
}

void bluetooth_gatt(bool enable)
{
#if CONFIG_BT_NIMBLE_ENABLED
    if (!enable) {
        s_bluetooth_gatt_enabled = false;
        if (s_bluetooth_state.advertising_active) {
            (void)ble_gap_adv_stop();
            s_bluetooth_state.advertising_active = false;
        }
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " GATT server disabled\n");
        return;
    }

    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return;
    }
    s_bluetooth_gatt_enabled = true;
    if (s_bluetooth_state.synced) {
        if (!bluetooth_gatt_register()) {
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                              " GATT server registration failed\n");
            return;
        }
        s_bluetooth_state.advertising_requested = true;
        if (bluetooth_start_advertising_ex(true) == ESP_OK) {
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST
                              " GATT server enabled (connectable advertising)\n");
        } else {
            bluetooth_appendf(SH_WARN "bluetooth:" SH_RST
                              " GATT server registered; advertising failed\n");
        }
    } else {
        /* Remember the request: bluetooth_on_sync() starts advertising once
         * the hosted controller finishes synchronizing. Without this a
         * `chat on` issued immediately after boot would register the services
         * but never advertise, so a peer could never find the board. */
        s_bluetooth_state.advertising_requested = true;
        bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST
                          " waiting for BLE host sync before enabling the GATT server\n");
    }
    bluetooth_record_infof("BLE GATT server %s", enable ? "enabled" : "disabled");
#else
    (void)enable;
    bluetooth_report_not_available(ESP_ERR_NOT_SUPPORTED);
#endif
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

    if (s_bluetooth_gatt_enabled && !s_bluetooth_gatt_registered) {
        (void)bluetooth_gatt_register();
    }

    if (s_bluetooth_state.advertising_requested && !s_bluetooth_state.advertising_active) {
        if (bluetooth_start_advertising_ex(s_bluetooth_gatt_enabled) == ESP_OK) {
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

static esp_err_t bluetooth_start_advertising_ex(bool connectable)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    if (!s_bluetooth_state.synced) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NimBLE rejects a second ble_gap_adv_start() with EALREADY. Re-issuing an
     * advertise request must therefore restart rather than fail: this is what
     * lets the GATT server switch a running non-connectable advert to a
     * connectable one (and vice versa) without an error. */
    if (s_bluetooth_state.advertising_active) {
        (void)ble_gap_adv_stop();
        s_bluetooth_state.advertising_active = false;
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
    if (s_chat.server_enabled) {
        /* Advertise the chat service so a scanning peer can identify the
         * board before connecting (and so this is not mistaken for the
         * status-only GATT server). */
        fields.uuids16 = &s_chat_svc_uuid;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc == BLE_HS_EMSGSIZE && s_chat.server_enabled) {
        /* A long session name plus the UUID can exceed the 31-byte advert;
         * keep the name (it is what a human scans for) and drop the UUID. */
        fields.uuids16 = NULL;
        fields.num_uuids16 = 0;
        fields.uuids16_is_complete = 0;
        rc = ble_gap_adv_set_fields(&fields);
    }
    if (rc != 0) {
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
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
        if (event->connect.status != 0) {
            if (s_bt_link_role == BT_LINK_CHAT_CLIENT) {
                bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                                  " chat connection failed (status=%d)\n",
                                  event->connect.status);
                bluetooth_chat_cleanup();
            } else {
                bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                                  " connection failed (status=%d)\n",
                                  event->connect.status);
                bluetooth_record_errorf(ESP_FAIL, "BLE connection failed (status=%d)",
                                        event->connect.status);
                bluetooth_hid_cleanup();
            }
            s_bt_link_role = BT_LINK_NONE;
            return 0;
        }

        if (s_bt_link_role == BT_LINK_CHAT_CLIENT) {
            s_chat.peer_connected = true;
            s_chat.client = true;
            s_chat.conn_handle = event->connect.conn_handle;
            s_chat.discovery_done = false;
            s_chat.mtu = 23;
            s_chat.rx_handle = 0;
            s_chat.tx_handle = 0;
            s_chat.ccc_handle = 0;
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " chat connected to "
                              SH_VAL "%s" SH_RST " (handle %u)\n",
                              s_chat.peer, event->connect.conn_handle);
            bluetooth_notify_headerf(4000, "Bluetooth chat connected");
            bluetooth_chat_start_discovery(event->connect.conn_handle);
        } else if (s_bt_link_role == BT_LINK_HID) {
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
        } else if (s_chat.server_enabled) {
            /* Inbound connection: this board is the chat server. */
            s_chat.peer_connected = true;
            s_chat.client = false;
            s_chat.conn_handle = event->connect.conn_handle;
            s_chat.discovery_done = true;
            s_chat.mtu = 23;
            {
                struct ble_gap_conn_desc desc;

                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                    bluetooth_format_address(&desc.peer_id_addr,
                                             s_chat.peer, sizeof(s_chat.peer));
                } else {
                    snprintf(s_chat.peer, sizeof(s_chat.peer), "(unknown)");
                }
            }
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " chat peer connected: "
                              SH_VAL "%s" SH_RST "\n", s_chat.peer);
            bluetooth_notify_headerf(4000, "Bluetooth chat peer connected");
        } else {
            bluetooth_appendf(SH_WARN "bluetooth:" SH_RST
                              " rejected an unexpected BLE connection\n");
            (void)ble_gap_terminate(event->connect.conn_handle,
                                    BLE_ERR_REM_USER_CONN_TERM);
        }
        s_bt_link_role = BT_LINK_NONE;
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        /* A BLE connection was terminated. */
        if (s_ble_hid.connected &&
            event->disconnect.conn.conn_handle == s_ble_hid.conn_handle) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " BLE HID device disconnected (reason=%d)\n",
                              event->disconnect.reason);
            bluetooth_notify_headerf(4000, "BLE keyboard disconnected");
            bluetooth_record_infof("BLE HID disconnected (reason=%d)", event->disconnect.reason);
            bluetooth_hid_cleanup();
        }
        if (s_chat.peer_connected &&
            event->disconnect.conn.conn_handle == s_chat.conn_handle) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " chat peer disconnected (reason=%d)\n",
                              event->disconnect.reason);
            bluetooth_notify_headerf(4000, "Bluetooth chat disconnected");
            bluetooth_chat_cleanup();
            if (s_chat.server_enabled) {
                (void)bluetooth_start_advertising_ex(true);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* A HID report (or other notification) arrived from the connected
         * device. Boot handles route by characteristic; the generic Report
         * handle only counts when it is exactly one boot keyboard report
         * (report-protocol devices need report-map parsing, which we do
         * not do — those bytes are ignored rather than misread). */
        uint16_t attr_handle = event->notify_rx.attr_handle;

        if (s_chat.peer_connected &&
            event->notify_rx.conn_handle == s_chat.conn_handle &&
            attr_handle == s_chat.tx_handle &&
            event->notify_rx.om != NULL) {
            char text[BT_CHAT_MSG_BYTES];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);

            if (len > sizeof(text) - 1) {
                len = sizeof(text) - 1;
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, text);
            text[len] = '\0';
            s_chat.rx_count++;
            bluetooth_chat_print_rx(text);
            bluetooth_notify_headerf(3000, "Bluetooth message");
            return 0;
        }

        if (s_ble_hid.connected &&
            event->notify_rx.conn_handle == s_ble_hid.conn_handle &&
            event->notify_rx.om != NULL &&
            (attr_handle == s_ble_hid.boot_kbd_inp_handle ||
             attr_handle == s_ble_hid.boot_mouse_inp_handle ||
             attr_handle == s_ble_hid.report_char_handle)) {
            uint8_t data[64];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);

            if (len > sizeof(data)) {
                len = sizeof(data);
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, data);
            if (attr_handle == s_ble_hid.boot_mouse_inp_handle) {
                if (len >= BLE_HID_BOOT_MOUSE_REPORT_LEN) {
                    bluetooth_hid_route_boot_mouse(data, len);
                }
            } else if (attr_handle == s_ble_hid.boot_kbd_inp_handle) {
                if (len >= BLE_HID_BOOT_KBD_REPORT_LEN) {
                    bluetooth_hid_route_boot_keyboard(data, len);
                }
            } else if (len == BLE_HID_BOOT_KBD_REPORT_LEN) {
                bluetooth_hid_route_boot_keyboard(data, len);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        if (s_chat.peer_connected &&
            event->mtu.conn_handle == s_chat.conn_handle) {
            s_chat.mtu = event->mtu.value;
        }
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

static esp_err_t bluetooth_ensure_ready_once(void)
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
        bluetooth_appendf(SH_ERR "bluetooth: hosted firmware version read failed (%s)" SH_RST "\n",
                          esp_err_to_name(error));
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
        bluetooth_appendf(SH_ERR "bluetooth: hosted version mismatch (host %u.x, C6 %u.%u.%u)" SH_RST "\n",
                          P4_CONFIG_HOSTED_COMPAT_MAJOR,
                          s_bluetooth_state.fw_version.major1, s_bluetooth_state.fw_version.minor1,
                          s_bluetooth_state.fw_version.patch1);
        return ESP_ERR_INVALID_STATE;
#endif
    }

    if (!s_bluetooth_state.controller_enabled) {
        esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
        error = esp_hosted_bt_host_stack_setup(&cfg);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            s_bluetooth_state.last_error = error;
            bluetooth_appendf(SH_ERR "bluetooth: hosted controller bring-up failed (%s)" SH_RST "\n",
                              esp_err_to_name(error));
            return error;
        }
        if (error == ESP_ERR_INVALID_STATE) {
            ESP_LOGW("bluetooth", "hosted controller already up (0x%x)", (unsigned)error);
        }
        s_bluetooth_state.controller_enabled = true;
    }

    if (!s_bluetooth_state.nimble_initialized) {
        rc = nimble_port_init();
        if (rc != 0) {
            s_bluetooth_state.last_error = ESP_FAIL;
            bluetooth_appendf(SH_ERR "bluetooth: NimBLE host init failed (rc=%d) - is the hosted HCI transport bound?" SH_RST "\n",
                              rc);
            return ESP_FAIL;
        }

        ble_hs_cfg.reset_cb = bluetooth_on_reset;
        ble_hs_cfg.sync_cb = bluetooth_on_sync;
        ble_svc_gap_init();
        rc = ble_svc_gap_device_name_set(BLUETOOTH_DEVICE_NAME);
        if (rc != 0) {
            s_bluetooth_state.last_error = ESP_FAIL;
            bluetooth_appendf(SH_ERR "bluetooth: GAP device name set failed (rc=%d)" SH_RST "\n", rc);
            /* nimble is up but the flag is not set: tear it down so a retry
             * does not call nimble_port_init() on an initialized stack. */
            nimble_port_deinit();
            return ESP_FAIL;
        }

        /* Attribute definitions must be added BEFORE the host task starts:
         * ble_gatts_start() (called from ble_hs_start inside the task) builds
         * the ATT database from them. Adding services after sync only appends
         * definitions that are never registered, so the characteristics answer
         * no handles and a peer discovers nothing. Registering every service
         * here costs a few bytes and makes `gatt on`/`chat on` work whenever
         * they are issued. */
        rc = ble_gatts_count_cfg(g_bluetooth_gatt_svcs);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(g_bluetooth_gatt_svcs);
        }
        if (rc != 0) {
            bluetooth_record_errorf(ESP_FAIL, "GATT registration at init failed (rc=%d)", rc);
        } else {
            s_bluetooth_gatt_registered = true;
        }

        nimble_port_freertos_init(bluetooth_host_task);
        /* esp_nimble_enable() ignores the xTaskCreatePinnedToCore() result, so
         * a failed host-task creation looks exactly like success. Verify it
         * explicitly: without this task there is no host event loop and the
         * stack never syncs. */
        if (xTaskGetHandle("nimble_host") == NULL) {
            s_bluetooth_state.nimble_initialized = false;
            s_bluetooth_state.last_error = ESP_FAIL;
            bluetooth_appendf(SH_ERR "bluetooth: NimBLE host task was not created "
                                    "(raise CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE?)" SH_RST "\n");
            return ESP_FAIL;
        }
        s_bluetooth_state.nimble_initialized = true;
    }

    s_bluetooth_state.last_error = ESP_OK;
    return ESP_OK;
#else
    s_bluetooth_state.last_error = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t bluetooth_ensure_ready(void)
{
    esp_err_t error = bluetooth_ensure_ready_once();

    /* The hosted controller can still be settling when the first attempt runs
     * (the C6 BT service is brought up over the same SDIO link as Wi-Fi), so a
     * single bounded retry turns a transient failure into a working stack. */
    if (error != ESP_OK && P4_CONFIG_BT_INIT_RETRIES > 0) {
        ESP_LOGW("bluetooth", "host init attempt 1 failed (%s); retrying",
                 esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_BT_INIT_RETRY_DELAY_MS));
        error = bluetooth_ensure_ready_once();
    }
    return error;
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

    if (bluetooth_start_advertising_ex(false) != ESP_OK) {
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

/** Route a BLE HID boot mouse report. Button byte 0, signed X/Y in bytes 1-2,
 *  optional signed wheel in byte 3 (positive = scroll up, matching USB). The
 *  wheel scrolls the transcript even with echo off; motion echo needs
 *  `bluetooth mouse on`. */
static void bluetooth_hid_route_boot_mouse(const uint8_t *report, uint16_t len)
{
    uint8_t buttons;
    int8_t dx;
    int8_t dy;
    int8_t wheel = 0;

    if (len < BLE_HID_BOOT_MOUSE_REPORT_LEN) {
        return;
    }
    buttons = report[0];
    dx = (int8_t)report[1];
    dy = (int8_t)report[2];
    if (len > BLE_HID_BOOT_MOUSE_REPORT_LEN) {
        wheel = (int8_t)report[3];
    }

    if (wheel != 0 && s_host_ops.bluetooth_mouse_scroll != NULL) {
        s_host_ops.bluetooth_mouse_scroll(-(int32_t)wheel * P4_CONFIG_TRANSCRIPT_SCROLL_STEP);
    }

    /* Shared cursor with USB (same display-owned pointer indev, same
     * opt-in switches): motion + left button feed it, self-gated there. */
    display_mouse_report((int)dx, (int)dy, buttons);

    if (!s_ble_hid.mouse_echo) {
        return;
    }
    s_ble_hid.mouse_x += dx;
    s_ble_hid.mouse_y += dy;
    bluetooth_appendf("bluetooth mouse: X=%06d Y=%06d |%c|%c|\n",
                      (int)s_ble_hid.mouse_x,
                      (int)s_ble_hid.mouse_y,
                      (buttons & 0x01) ? 'o' : ' ',
                      (buttons & 0x02) ? 'o' : ' ');
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

    /* Prefer HID Report Input (0x2A4D), fall back to Boot Keyboard Input (0x2A22),
     * then Boot Mouse Input (0x2A33). */
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

    if (s_ble_hid.boot_mouse_inp_handle != 0) {
        s_ble_hid.dsc_char_handle = s_ble_hid.boot_mouse_inp_handle;
        rc = ble_gattc_disc_all_dscs(conn_handle, s_ble_hid.boot_mouse_inp_handle,
                                     s_ble_hid.hid_service_end,
                                     bluetooth_disc_dsc_cb, NULL);
        if (rc == 0) {
            bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " discovering Boot Mouse CCC\n");
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
        /* Descriptor discovery complete. If no CCC was found, walk down the
         * fallback chain: Report -> Boot Keyboard -> Boot Mouse. */
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
            if ((s_ble_hid.dsc_char_handle == s_ble_hid.report_char_handle ||
                 s_ble_hid.dsc_char_handle == s_ble_hid.boot_kbd_inp_handle) &&
                s_ble_hid.boot_mouse_inp_handle != 0) {
                s_ble_hid.dsc_char_handle = s_ble_hid.boot_mouse_inp_handle;
                if (ble_gattc_disc_all_dscs(conn_handle, s_ble_hid.boot_mouse_inp_handle,
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
    s_bt_link_role = BT_LINK_HID;
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
        s_bt_link_role = BT_LINK_NONE;
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
    s_ble_hid.mouse_x = 0;
    s_ble_hid.mouse_y = 0;
    memset(s_ble_hid_prev_keys, 0, sizeof(s_ble_hid_prev_keys));
}

/* ---- BLE chat: central-side discovery, send, and role control ---------- */

static int bluetooth_chat_disc_dsc_cb(uint16_t conn_handle,
                                      const struct ble_gatt_error *error,
                                      uint16_t chr_val_handle,
                                      const struct ble_gatt_dsc *dsc,
                                      void *arg)
{
    (void)arg;
    (void)chr_val_handle;
    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
            s_chat.ccc_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (s_chat.ccc_handle == 0) {
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                          " chat TX characteristic has no CCC descriptor\n");
        return 0;
    }
    {
        uint8_t enable[2] = { 1, 0 };   /* notifications on */

        if (ble_gattc_write_flat(conn_handle, s_chat.ccc_handle,
                                 enable, sizeof(enable), NULL, NULL) != 0) {
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                              " could not subscribe to chat notifications\n");
            return 0;
        }
    }
    s_chat.discovery_done = true;
    bluetooth_appendf(SH_OK "bluetooth:" SH_RST
                      " chat ready (rx=%u tx=%u mtu=%u)\n",
                      (unsigned int)s_chat.rx_handle,
                      (unsigned int)s_chat.tx_handle,
                      (unsigned int)s_chat.mtu);
    bluetooth_notify_headerf(3500, "Bluetooth chat ready");
    /* The subscription write must finish before the MTU procedure; NimBLE
     * queues it, and the MTU event updates s_chat.mtu for long messages. */
    (void)ble_gattc_exchange_mtu(conn_handle, NULL, NULL);
    return 0;
}

static int bluetooth_chat_disc_chr_cb(uint16_t conn_handle,
                                      const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *chr,
                                      void *arg)
{
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);

        if (uuid == BT_CHAT_RX_UUID) {
            s_chat.rx_handle = chr->val_handle;
        } else if (uuid == BT_CHAT_TX_UUID) {
            s_chat.tx_handle = chr->val_handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (s_chat.rx_handle == 0 || s_chat.tx_handle == 0) {
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                          " chat service is missing its RX/TX characteristics\n");
        return 0;
    }
    (void)ble_gattc_disc_all_dscs(conn_handle, s_chat.tx_handle,
                                 s_chat.svc_end, bluetooth_chat_disc_dsc_cb, NULL);
    return 0;
}

static int bluetooth_chat_disc_svc_cb(uint16_t conn_handle,
                                      const struct ble_gatt_error *error,
                                      const struct ble_gatt_svc *service,
                                      void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_chat.svc_start = service->start_handle;
        s_chat.svc_end = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (s_chat.svc_start == 0) {
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                          " peer has no P4MiniShell chat service (0xFFF2);"
                          " run `bluetooth chat on` there first\n");
        return 0;
    }
    if (ble_gattc_disc_all_chrs(conn_handle, s_chat.svc_start, s_chat.svc_end,
                                bluetooth_chat_disc_chr_cb, NULL) != 0) {
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                          " could not start characteristic discovery\n");
    }
    return 0;
}

static void bluetooth_chat_start_discovery(uint16_t conn_handle)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(BT_CHAT_SVC_UUID);

    s_chat.svc_start = 0;
    s_chat.svc_end = 0;
    if (ble_gattc_disc_svc_by_uuid(conn_handle, &uuid.u,
                                   bluetooth_chat_disc_svc_cb, NULL) != 0) {
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                          " could not start chat service discovery\n");
    }
}

/** Reset the per-connection chat state. The server's TX value handle is a
 *  registration property and deliberately survives a disconnect. */
static void bluetooth_chat_cleanup(void)
{
    s_chat.peer_connected = false;
    s_chat.discovery_done = false;
    s_chat.conn_handle = 0;
    s_chat.rx_handle = 0;
    s_chat.tx_handle = 0;
    s_chat.ccc_handle = 0;
    s_chat.svc_start = 0;
    s_chat.svc_end = 0;
    s_chat.mtu = 23;
}

/** `bluetooth chat on [name]`: register the GATT server and advertise
 *  connectably. Idempotent. */
void bluetooth_chat_server(bool enable, const char *name)
{
    if (!enable) {
        if (s_chat.peer_connected) {
            (void)ble_gap_terminate(s_chat.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            bluetooth_chat_cleanup();
        }
        s_chat.server_enabled = false;
        if (s_bluetooth_state.advertising_active) {
            (void)ble_gap_adv_stop();
            s_bluetooth_state.advertising_active = false;
        }
        bluetooth_appendf(SH_MUTE "bluetooth:" SH_RST " chat server off\n");
        return;
    }

    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return;
    }
    if (name != NULL && name[0] != '\0') {
        snprintf(s_bluetooth_state.session_advertise_name,
                 sizeof(s_bluetooth_state.session_advertise_name), "%s", name);
    }
    s_chat.server_enabled = true;
    s_chat.mtu = 23;
    /* Registers the whole service table (Device Information, status, chat)
     * and switches advertising to connectable with the chat UUID. */
    bluetooth_gatt(true);
}

/** `bluetooth chat join <addr>`: connect as the GATT client. */
esp_err_t bluetooth_chat_join(const char *addr_str)
{
    ble_addr_t addr;
    struct ble_gap_conn_params conn_params;
    unsigned int a[6];
    int rc;

    if (addr_str == NULL || addr_str[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_chat.peer_connected) {
        bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " chat already connected to "
                          SH_VAL "%s" SH_RST "\n", s_chat.peer);
        return ESP_ERR_INVALID_STATE;
    }
    if (sscanf(addr_str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        bluetooth_appendf(SH_USAGE "Usage: bluetooth chat join XX:XX:XX:XX:XX:XX"
                          SH_RST "\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (bluetooth_ensure_ready() != ESP_OK) {
        bluetooth_report_not_available(s_bluetooth_state.last_error);
        return ESP_ERR_INVALID_STATE;
    }

    addr.type = BLE_OWN_ADDR_PUBLIC;
    addr.val[0] = (uint8_t)a[5];
    addr.val[1] = (uint8_t)a[4];
    addr.val[2] = (uint8_t)a[3];
    addr.val[3] = (uint8_t)a[2];
    addr.val[4] = (uint8_t)a[1];
    addr.val[5] = (uint8_t)a[0];

    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl = P4_CONFIG_BLE_CONN_SCAN_ITVL;
    conn_params.scan_window = P4_CONFIG_BLE_CONN_SCAN_WINDOW;
    conn_params.itvl_min = P4_CONFIG_BLE_CONN_ITVL_MIN;
    conn_params.itvl_max = P4_CONFIG_BLE_CONN_ITVL_MAX;
    conn_params.latency = P4_CONFIG_BLE_CONN_LATENCY;
    conn_params.supervision_timeout = P4_CONFIG_BLE_CONN_SUPERVISION_TIMEOUT;

    if (s_bluetooth_state.scan_active) {
        (void)ble_gap_disc_cancel();
        s_bluetooth_state.scan_active = false;
    }
    if (s_bluetooth_state.advertising_active) {
        (void)ble_gap_adv_stop();
        s_bluetooth_state.advertising_active = false;
    }

    snprintf(s_chat.peer, sizeof(s_chat.peer), "%s", addr_str);
    s_chat.name[0] = '\0';
    s_chat.mtu = 23;
    for (size_t index = 0; index < s_bluetooth_state.discovered_count; index++) {
        if (strutil_text_equals_ignore_case(s_bluetooth_state.discovered[index].address,
                                            addr_str)) {
            snprintf(s_chat.name, sizeof(s_chat.name), "%s",
                     s_bluetooth_state.discovered[index].name);
            break;
        }
    }

    s_bt_link_role = BT_LINK_CHAT_CLIENT;
    s_chat.client = true;
    rc = ble_gap_connect(s_bluetooth_state.own_addr_type, &addr,
                         P4_CONFIG_BLE_CONNECT_TIMEOUT_MS, &conn_params,
                         bluetooth_gap_event, NULL);
    if (rc != 0) {
        s_bt_link_role = BT_LINK_NONE;
        s_chat.client = false;
        bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " chat connect failed (rc=%d)\n", rc);
        return ESP_FAIL;
    }
    bluetooth_appendf(SH_PROMPT "bluetooth:" SH_RST " chat joining " SH_VAL "%s" SH_RST "...\n",
                      addr_str);
    return ESP_OK;
}

/** `bluetooth chat send <text>`: write to the server's RX or notify the
 *  client's TX, depending on the role this board plays. */
esp_err_t bluetooth_chat_send(const char *text)
{
    char message[BT_CHAT_MSG_BYTES];
    size_t len;
    size_t max_len;

    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_chat.peer_connected) {
        bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " chat has no peer\n");
        return ESP_ERR_INVALID_STATE;
    }
    max_len = (s_chat.mtu > 3) ? (size_t)(s_chat.mtu - 3) : 20u;
    if (max_len > BT_CHAT_MSG_BYTES - 1) {
        max_len = BT_CHAT_MSG_BYTES - 1;
    }
    len = strlen(text);
    if (len > max_len) {
        len = max_len;
    }
    memcpy(message, text, len);
    message[len] = '\0';

    if (s_chat.client) {
        if (!s_chat.discovery_done || s_chat.rx_handle == 0) {
            bluetooth_appendf(SH_WARN "bluetooth:" SH_RST
                              " chat is still discovering the peer\n");
            return ESP_ERR_INVALID_STATE;
        }
        if (ble_gattc_write_flat(s_chat.conn_handle, s_chat.rx_handle,
                                 message, len, NULL, NULL) != 0) {
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " chat send failed\n");
            return ESP_FAIL;
        }
    } else {
        struct os_mbuf *om;

        if (s_chat_server_tx_handle == 0) {
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST
                              " chat server TX is not registered\n");
            return ESP_ERR_INVALID_STATE;
        }
        om = ble_hs_mbuf_from_flat(message, len);
        if (om == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (ble_gatts_notify_custom(s_chat.conn_handle, s_chat_server_tx_handle,
                                    om) != 0) {
            os_mbuf_free_chain(om);
            bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " chat notify failed\n");
            return ESP_FAIL;
        }
    }

    s_chat.tx_count++;
    /* Plain %s: ansi_vformat does not implement the `.*` precision, which
     * garbled the echo when it was `%.*s`. */
    bluetooth_appendf(SH_BT "[me]" SH_RST " %s\n", message);
    return ESP_OK;
}

void bluetooth_chat_status(void)
{
    bluetooth_appendf(SH_SUBHEAD SH_BOLD "Bluetooth chat:" SH_RST "\n");
    bluetooth_appendf("  server=%s peer_connected=%s role=%s\n",
                      s_chat.server_enabled ? "on" : "off",
                      s_chat.peer_connected ? "yes" : "no",
                      s_chat.peer_connected
                          ? (s_chat.client ? "client" : "server")
                          : "none");
    if (s_chat.peer_connected) {
        bluetooth_appendf("  peer=%s%s%s mtu=%u rx=%u tx=%u ready=%s\n",
                          s_chat.peer,
                          s_chat.name[0] != '\0' ? " " : "",
                          s_chat.name[0] != '\0' ? s_chat.name : "",
                          (unsigned int)s_chat.mtu,
                          (unsigned int)s_chat.rx_handle,
                          (unsigned int)(s_chat.client ? s_chat.tx_handle
                                                       : s_chat_server_tx_handle),
                          s_chat.discovery_done ? "yes" : "no");
    }
    bluetooth_appendf("  rx=%lu tx=%lu\n",
                      (unsigned long)s_chat.rx_count,
                      (unsigned long)s_chat.tx_count);
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

void bluetooth_chat_server(bool enable, const char *name)
{
    (void)enable;
    (void)name;
    bluetooth_report_not_available(ESP_ERR_NOT_SUPPORTED);
}

esp_err_t bluetooth_chat_join(const char *addr_str)
{
    (void)addr_str;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_chat_send(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

void bluetooth_chat_status(void)
{
    bluetooth_appendf(SH_WARN "bluetooth:" SH_RST " BLE chat unavailable "
                      "(NimBLE is not enabled)\n");
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
        bluetooth_appendf("  " SH_CMD "bluetooth mouse on|off" SH_RST "    Echo BLE mouse motion (wheel always scrolls)\n");
        bluetooth_appendf("  " SH_CMD "bluetooth mouse cursor|click <on|off>" SH_RST "    Shared pointer cursor/click (same switches as usb; default off)\n");
        bluetooth_appendf("  " SH_CMD "bluetooth chat on" SH_RST " [name]      Start a connectable GATT chat server\n");
        bluetooth_appendf("  " SH_CMD "bluetooth chat join" SH_RST " <addr>     Join another board's chat server\n");
        bluetooth_appendf("  " SH_CMD "bluetooth chat send" SH_RST " <text>     Send a message to the peer\n");
        bluetooth_appendf("  " SH_CMD "bluetooth chat off|status" SH_RST "       Stop the server / show the link\n");
        bluetooth_appendf("  " SH_CMD "bt ..." SH_RST "                      Alias for the bluetooth family\n");
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "status")) {
        bluetooth_status();
        return;
    }

    if (strutil_text_equals_ignore_case(argv[1], "chat")) {
        if (argc < 3 || strutil_text_equals_ignore_case(argv[2], "status")) {
            bluetooth_chat_status();
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "on")) {
            bluetooth_chat_server(true, argc >= 4 ? argv[3] : NULL);
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "off")) {
            bluetooth_chat_server(false, NULL);
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "join") && argc >= 4) {
            esp_err_t error = bluetooth_chat_join(argv[3]);

            if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
                bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " chat join failed (%s)\n",
                                  esp_err_to_name(error));
            }
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "send") && argc >= 4) {
            char text[BT_CHAT_MSG_BYTES];
            size_t used = 0;

            text[0] = '\0';
            for (int i = 3; i < argc; i++) {
                int written = snprintf(text + used, sizeof(text) - used, "%s%s",
                                       i > 3 ? " " : "", argv[i]);

                if (written < 0 || (size_t)written >= sizeof(text) - used) {
                    break;
                }
                used += (size_t)written;
            }
            (void)bluetooth_chat_send(text);
            return;
        }
        bluetooth_appendf(SH_USAGE "Usage: bluetooth chat <on [name]|off|join <addr>|"
                          "send <text>|status>" SH_RST "\n");
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

    if (strutil_text_equals_ignore_case(argv[1], "gatt") && argc >= 3) {
        if (strutil_text_equals_ignore_case(argv[2], "on")) {
            bluetooth_gatt(true);
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "off")) {
            bluetooth_gatt(false);
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

    if (strutil_text_equals_ignore_case(argv[1], "mouse") && argc >= 3) {
        if (strutil_text_equals_ignore_case(argv[2], "on")) {
            s_ble_hid.mouse_echo = true;
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse motion echo on\n");
            return;
        }
        if (strutil_text_equals_ignore_case(argv[2], "off")) {
            s_ble_hid.mouse_echo = false;
            bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse motion echo off\n");
            return;
        }
        /* Cursor/click tracking shares one display-owned pointer indev
         * with USB (`usb mouse cursor|click` toggles the same switches);
         * both default off, wheel scroll always works. */
        if (argc >= 4 && strutil_text_equals_ignore_case(argv[2], "cursor")) {
            if (strutil_text_equals_ignore_case(argv[3], "on")) {
                if (display_mouse_cursor_enable() == ESP_OK) {
                    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse cursor on\n");
                } else {
                    bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " mouse cursor unavailable (display not ready)\n");
                }
                return;
            }
            if (strutil_text_equals_ignore_case(argv[3], "off")) {
                if (display_mouse_cursor_disable() == ESP_OK) {
                    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse cursor off\n");
                } else {
                    bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " mouse cursor unavailable (display not ready)\n");
                }
                return;
            }
        }
        if (argc >= 4 && strutil_text_equals_ignore_case(argv[2], "click")) {
            if (strutil_text_equals_ignore_case(argv[3], "on")) {
                if (display_mouse_click_enable() == ESP_OK) {
                    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse click on\n");
                } else {
                    bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " mouse click unavailable (display not ready)\n");
                }
                return;
            }
            if (strutil_text_equals_ignore_case(argv[3], "off")) {
                if (display_mouse_click_disable() == ESP_OK) {
                    bluetooth_appendf(SH_OK "bluetooth:" SH_RST " mouse click off\n");
                } else {
                    bluetooth_appendf(SH_ERR "bluetooth:" SH_RST " mouse click unavailable (display not ready)\n");
                }
                return;
            }
        }
    }
#endif

    bluetooth_appendf(SH_USAGE "Usage: bluetooth status | scan [limit] | advertise <on [name]|off> | gatt <on|off> | chat <on [name]|off|join <addr>|send <text>|status> | connect <addr> | disconnect | mouse <on|off|cursor <on|off>|click <on|off>>" SH_RST "\n");
}
