/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file os_commands.c
 * @module command
 * @layer command
 * @domain diagnostics
 * @provides `os` read-only facade over the existing subsystem accessors
 * @status experimental
 * @test test/main/test_os_facade.c, tools/host_os_gate.py
 * @hard-rule ai-context.md#hard-rules (one implementation, no second driver)
 * @brief Batch-facing diagnostic/control facade over the ONE set of owner APIs.
 *
 * Every `os <area>` line reads an accessor that already belongs to another
 * component: task count from FreeRTOS, start-pool state from batch, board
 * capabilities from board_caps, devices from usb/storage/networking, capacity
 * from storage, heap from p4heap/heap_caps, and trust from security/pkg. This
 * file owns no driver, no registry, no socket and no second dispatcher: it
 * formats what the owners already publish into one machine-readable `key=value`
 * surface. A subsystem that cannot answer reports `unavailable` explicitly and
 * sets ERRORLEVEL 1; it is never invented.
 *
 * The pure parse/format region below uses only the C standard library so the
 * host gate compiles the SAME shipped code with mock accessor snapshots. Its
 * declarations live once in command.h between the OS_PURE_DECL markers.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "command.h"
#include "shell.h"
#include "batch.h"
#include "board_config.h"
#include "board_caps.h"
#include "p4minishell_config.h"
#include "storage.h"
#include "networking.h"
#include "display.h"
#include "usb.h"
#include "security_commands.h"
#include "p4heap.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------------
 * Pure parse/format region. No ESP-IDF, LVGL, SD or FreeRTOS access here: the
 * host gate extracts this block verbatim and compiles it with mock data.
 * ---------------------------------------------------------------------- */
/* OS_PURE_IMPL_BEGIN */

static int os_ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static bool os_text_eq(const char *left, const char *right)
{
    int i = 0;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[i] != '\0' && right[i] != '\0') {
        if (os_ascii_lower((unsigned char)left[i]) !=
            os_ascii_lower((unsigned char)right[i])) {
            return false;
        }
        i++;
    }
    return left[i] == '\0' && right[i] == '\0';
}

const char *command_os_area_name(os_area_t area)
{
    switch (area) {
        case OS_AREA_PM:   return "pm";
        case OS_AREA_DRV:  return "drv";
        case OS_AREA_IO:   return "io";
        case OS_AREA_FS:   return "fs";
        case OS_AREA_NET:  return "net";
        case OS_AREA_MEM:  return "mem";
        case OS_AREA_DISK: return "disk";
        case OS_AREA_SEC:  return "sec";
        case OS_AREA_PWR:  return "pwr";
        case OS_AREA_VER:  return "ver";
        default:           return "invalid";
    }
}

const char *command_os_status_name(bool available)
{
    return available ? "ok" : "unavailable";
}

bool command_os_mode_allows(os_area_t area, os_mode_t mode)
{
    if (area < 0 || area >= OS_AREA_COUNT || mode == OS_MODE_INVALID) {
        return false;
    }
    if (mode == OS_MODE_STATUS) {
        return true;
    }
    if (area == OS_AREA_FS) {
        return mode == OS_MODE_FREE;
    }
    return (area == OS_AREA_PM || area == OS_AREA_DRV || area == OS_AREA_IO ||
            area == OS_AREA_DISK) && mode == OS_MODE_LIST;
}

int command_os_parse(int argc, char **argv, os_area_t *area_out,
                     os_mode_t *mode_out, bool *bare_out)
{
    os_area_t area = OS_AREA_INVALID;
    os_mode_t mode = OS_MODE_STATUS;
    bool seen_mode = false;
    bool bare = false;
    int i;

    if (area_out == NULL || mode_out == NULL || bare_out == NULL) {
        return 2;
    }
    *area_out = OS_AREA_INVALID;
    *mode_out = OS_MODE_STATUS;
    *bare_out = false;
    if (argv == NULL || argc < 2 || argv[1] == NULL) {
        return 2;
    }
    for (i = 0; i < (int)OS_AREA_COUNT; i++) {
        if (os_text_eq(argv[1], command_os_area_name((os_area_t)i))) {
            area = (os_area_t)i;
            break;
        }
    }
    if (area == OS_AREA_INVALID) {
        return 2;
    }
    for (i = 2; i < argc; i++) {
        const char *token = argv[i];

        if (token == NULL) {
            return 2;
        }
        if (os_text_eq(token, "/b")) {
            bare = true;
        } else if (os_text_eq(token, "status") || os_text_eq(token, "list") ||
                   os_text_eq(token, "free")) {
            if (seen_mode) {
                return 2;
            }
            seen_mode = true;
            if (os_text_eq(token, "list")) {
                mode = OS_MODE_LIST;
            } else if (os_text_eq(token, "free")) {
                mode = OS_MODE_FREE;
            } else {
                mode = OS_MODE_STATUS;
            }
        } else {
            return 2;
        }
    }
    *area_out = area;
    *mode_out = mode;
    *bare_out = bare;
    return 0;
}

size_t command_os_format_field(const char *key, const char *value,
                               bool available, char *out, size_t out_size)
{
    const char *src;
    size_t written = 0;

    if (key == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    src = (available && value != NULL) ? value : "unavailable";
    if (strlen(key) + 2 >= out_size) {
        (void)snprintf(out, out_size, "unavailable");
        return strlen(out);
    }
    (void)snprintf(out, out_size, "%s=", key);
    written = strlen(out);
    for (; *src != '\0' && written + 1 < out_size; src++) {
        char c = *src;

        if (c == '|' || c == '\r' || c == '\n') {
            c = (c == '|') ? '/' : ' ';
        }
        out[written++] = c;
    }
    /* A value that does not fit is never truncated into a partial lie. */
    if (*src != '\0') {
        (void)snprintf(out, out_size, "%s=unavailable", key);
        written = strlen(out);
    } else {
        out[written] = '\0';
    }
    return written;
}

size_t command_os_format_status(os_area_t area, bool available,
                                char *out, size_t out_size)
{
    int n;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    n = snprintf(out, out_size, "os.%s.status=%s",
                 command_os_area_name(area), command_os_status_name(available));
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return strlen(out);
}

/* OS_PURE_IMPL_END */

/* ------------------------------------------------------------------------
 * Owner-API adapters. Each field is filled from an existing accessor only.
 * ---------------------------------------------------------------------- */

#define OS_FIELD_VALUE_BYTES 96
#define OS_MAX_FIELDS        16
#define OS_LINE_BYTES        192

typedef struct {
    const char *key;
    char value[OS_FIELD_VALUE_BYTES];
    bool available;
} os_field_t;

static void os_field_set(os_field_t *field, const char *key, bool available,
                         const char *format, ...)
{
    va_list args;

    field->key = key;
    field->available = available;
    field->value[0] = '\0';
    if (available && format != NULL) {
        va_start(args, format);
        (void)vsnprintf(field->value, sizeof(field->value), format, args);
        va_end(args);
    }
}

static bool os_fields_print(const os_field_t *fields, int count)
{
    bool ok = true;
    char line[OS_LINE_BYTES];
    int i;

    for (i = 0; i < count; i++) {
        (void)command_os_format_field(fields[i].key, fields[i].value,
                                      fields[i].available, line, sizeof(line));
        shell_transcript_appendf("%s\n", line);
        if (!fields[i].available) {
            ok = false;
        }
    }
    return ok;
}

static void os_usage(void)
{
    shell_print_usage(
        "Usage: os pm [list|status] [/b]      FreeRTOS tasks + start pool slots\n"
        "       os drv [list|status] [/b]     board capabilities + GPIO table owner\n"
        "       os io [list|status] [/b]      USB / SD / serial / input devices\n"
        "       os fs [status|free] [/b]      mounted storage capacity\n"
        "       os net [status] [/b]          networking host ops + Wi-Fi state\n"
        "       os mem [status] [/b]          heap, DMA pool, himem per slot\n"
        "       os disk [list|status] [/b]    storage volumes + disk geometry\n"
        "       os sec [status] [/b]          device lock + PKG trust key\n"
        "       os pwr [status] [/b]          power / sleep / battery status (read-only)\n"
        "       os ver [/b]                   build / ABI snapshot");
}

/* pm: uxTaskGetNumberOfTasks() + batch start-pool accessors. Per-task rows and
 * freeze/priority stay with `ps`/`start`/`nice`; no second task walker. */
static bool os_pm(os_mode_t mode, os_field_t *fields, int *count)
{
    int used = 0;
    int i = 0;
    int slot;

    for (slot = 0; slot < P4_CONFIG_BG_TASKS; slot++) {
        if (batch_bg_slot_in_use(slot)) {
            used++;
        }
    }
    os_field_set(&fields[i++], "os.pm.tasks", true, "%u",
                 (unsigned)uxTaskGetNumberOfTasks());
    os_field_set(&fields[i++], "os.pm.bg_used", true, "%d", used);
    os_field_set(&fields[i++], "os.pm.bg_total", true, "%d", P4_CONFIG_BG_TASKS);
    if (mode == OS_MODE_LIST) {
        for (slot = 0; slot < P4_CONFIG_BG_TASKS && i < OS_MAX_FIELDS; slot++) {
            os_field_set(&fields[i++], "os.pm.slot", true, "%d=%s", slot,
                         batch_bg_slot_in_use(slot) ? "used" : "free");
        }
    }
    *count = i;
    return true;
}

/* drv: board_caps_* capability queries. The GPIO reserved table is owned by
 * periph_commands.c; this facade names that owner instead of copying it. */
static bool os_drv(os_field_t *fields, int *count)
{
    int i = 0;

    os_field_set(&fields[i++], "os.drv.board", true, "%s", BOARD_CFG_ID);
    os_field_set(&fields[i++], "os.drv.gpio_reserved", true,
                 "owner=periph_commands (read-only)");
    os_field_set(&fields[i++], "os.drv.battery_adc", true, "%s",
                 board_caps_has_battery_adc() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.ina226", true, "%s",
                 board_caps_has_ina226() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.camera", true, "%s",
                 board_caps_has_camera() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.imu", true, "%s",
                 board_caps_has_imu() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.tab5kbd", true, "%s",
                 board_caps_has_tab5kbd() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.ws2812", true, "%s",
                 board_caps_has_ws2812() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.rtc_bsp_i2c", true, "%s",
                 board_caps_rtc_use_bsp_i2c() ? "yes" : "no");
    os_field_set(&fields[i++], "os.drv.rgb_via_tab5kbd", true, "%s",
                 board_caps_rgb_via_tab5kbd() ? "yes" : "no");
    *count = i;
    return true;
}

/* io: USB MSC slots, CDC-ACM serial state, SD mount and the fitted keyboard.
 * Every value comes from its owner's accessor; no device is enumerated twice. */
static bool os_io(os_mode_t mode, os_field_t *fields, int *count)
{
    userial_status_t serial;
    bool serial_ok;
    int i = 0;
    int slot;

    os_field_set(&fields[i++], "os.io.sd", true, "%s",
                 storage_sd_is_mounted() ? "mounted" : "absent");
    serial_ok = userial_get_status(&serial);
    os_field_set(&fields[i++], "os.io.userial", serial_ok, "%s",
                 serial_ok ? (serial.open ? "open" : "closed") : NULL);
    if (serial_ok && mode == OS_MODE_LIST) {
        os_field_set(&fields[i++], "os.io.userial_link", true, "%s",
                     serial.link_lost ? "lost" : "up");
    }
    os_field_set(&fields[i++], "os.io.keyboard", true, "%s",
                 board_caps_has_tab5kbd() ? "tab5kbd" : "usb-host");
    for (slot = 0; slot < P4_CONFIG_USB_MSC_MAX_DEVICES && i < OS_MAX_FIELDS; slot++) {
        bool connected = usb_msc_slot_connected(slot);

        os_field_set(&fields[i++], "os.io.usb", true, "%d=%s%s", slot,
                     connected ? "connected" : "absent",
                     (connected && usb_msc_slot_mounted(slot)) ? ",mounted" : "");
    }
    *count = i;
    return true;
}

/* fs / disk: storage volume + capacity accessors. `free` narrows fs to space;
 * `list` adds a row per registered volume. */
static bool os_fs(os_mode_t mode, os_field_t *fields, int *count)
{
    storage_space_info_t info;
    bool mounted = storage_sd_is_mounted();
    bool space_ok = false;
    int i = 0;

    if (mounted) {
        memset(&info, 0, sizeof(info));
        space_ok = (storage_get_space_info(&info) == ESP_OK);
    }
    os_field_set(&fields[i++], "os.fs.sd", true, "%s",
                 mounted ? "mounted" : "absent");
    if (mode != OS_MODE_FREE) {
        os_field_set(&fields[i++], "os.fs.cluster", space_ok, "%u",
                     space_ok ? (unsigned)info.cluster_bytes : 0u);
    }
    os_field_set(&fields[i++], "os.fs.free_bytes", space_ok, "%llu",
                 space_ok ? (unsigned long long)info.free_bytes : 0ull);
    os_field_set(&fields[i++], "os.fs.total_bytes", space_ok, "%llu",
                 space_ok ? (unsigned long long)info.total_bytes : 0ull);
    *count = i;
    return true;
}

static bool os_disk(os_mode_t mode, os_field_t *fields, int *count)
{
    storage_disk_info_t info;
    bool sd_ok = false;
    int i = 0;
    int vol;

    memset(&info, 0, sizeof(info));
    if (storage_sd_is_mounted()) {
        sd_ok = (storage_disk_get_info(STORAGE_VOLUME_SD, &info) == ESP_OK);
    }
    os_field_set(&fields[i++], "os.disk.sd", sd_ok, "%s",
                 sd_ok ? "present" : NULL);
    if (sd_ok) {
        os_field_set(&fields[i++], "os.disk.sd.capacity", true, "%llu",
                     (unsigned long long)info.capacity_bytes);
        os_field_set(&fields[i++], "os.disk.sd.sector", true, "%u/%u",
                     (unsigned)info.sector_size, (unsigned)info.sector_count);
    }
    if (mode == OS_MODE_LIST) {
        for (vol = STORAGE_VOLUME_USB0; vol < STORAGE_VOLUME_COUNT && i < OS_MAX_FIELDS; vol++) {
            uint32_t sector_size = 0;
            uint32_t sector_count = 0;
            char label[32];
            bool present;

            label[0] = '\0';
            present = storage_usb_volume_geometry(vol - STORAGE_VOLUME_USB0,
                                                  &sector_size, &sector_count,
                                                  label, sizeof(label));
            os_field_set(&fields[i++], "os.disk.usb", true, "%d=%s%s",
                         vol - STORAGE_VOLUME_USB0,
                         present ? "present" : "absent",
                         present ? label : "");
        }
    }
    *count = i;
    return true;
}

/* net: the registered host ops table plus Wi-Fi state. No socket is opened. */
static bool os_net(os_field_t *fields, int *count)
{
    const networking_host_ops_t *ops = networking_get_host_ops();
    bool registered = (ops != NULL);
    int i = 0;

    os_field_set(&fields[i++], "os.net.host_ops", registered, "%s",
                 registered ? "registered" : NULL);
    os_field_set(&fields[i++], "os.net.wifi_state", registered, "%s",
                 registered ? networking_wifi_state_string() : NULL);
    os_field_set(&fields[i++], "os.net.connected", registered, "%s",
                 registered ? (networking_wifi_is_connected() ? "yes" : "no") : NULL);
    *count = i;
    return true;
}

/* mem: heap_caps + p4heap pools + himem per-slot bytes. */
static bool os_mem(os_field_t *fields, int *count)
{
    int i = 0;
    int slot;

    os_field_set(&fields[i++], "os.mem.internal_free", true, "%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    os_field_set(&fields[i++], "os.mem.internal_total", true, "%u",
                 (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT));
    os_field_set(&fields[i++], "os.mem.internal_min", true, "%u",
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    os_field_set(&fields[i++], "os.mem.psram_free", true, "%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    os_field_set(&fields[i++], "os.mem.dma_free", true, "%u",
                 (unsigned)p4heap_dma_free());
    os_field_set(&fields[i++], "os.mem.dma_largest", true, "%u",
                 (unsigned)p4heap_internal_largest());
    for (slot = 0; slot <= P4_CONFIG_BG_TASKS && i < OS_MAX_FIELDS; slot++) {
        os_field_set(&fields[i++], "os.mem.himem", true, "%d=%u", slot,
                     (unsigned)himem_slot_bytes(slot));
    }
    *count = i;
    return true;
}

/* sec: lock/private gate + the one p4sign trust key. Read-only. */
static bool os_sec(os_field_t *fields, int *count)
{
    uint8_t pub[PKG_SIGN_PUB_BYTES];
    char fingerprint[65];
    bool key_ok;
    int i = 0;

    memset(pub, 0, sizeof(pub));
    key_ok = (pkg_sign_pubkey_load(pub) == 0);
    fingerprint[0] = '\0';
    if (key_ok) {
        pkg_sign_fingerprint(pub, fingerprint);
    }
    os_field_set(&fields[i++], "os.sec.locked", true, "%s",
                 security_is_locked() ? "yes" : "no");
    os_field_set(&fields[i++], "os.sec.private", true, "%s",
                 security_can_reveal_private() ? "allowed" : "denied");
    os_field_set(&fields[i++], "os.sec.pkg_key", key_ok, "%s",
                 key_ok ? fingerprint : NULL);
    *count = i;
    return true;
}

/* pwr: read-only power/sleep/battery status over the existing accessors. It
 * reports CONFIGURED wake sources, never armed ones: arming happens only in
 * shell_command_sleep()/deepsleep() and is cleared after wake. */
static void os_gpio_word(int gpio, char *out, size_t size)
{
    if (gpio < 0) {
        (void)snprintf(out, size, "none");
    } else {
        (void)snprintf(out, size, "GPIO%d", gpio);
    }
}

static bool os_pwr(os_field_t *fields, int *count)
{
    int battery_mv = 0;
    int battery_pct = 0;
    int raw = 0;
    int gpio_mv = 0;
    bool battery_ok = (command_battery_read(&battery_mv, &battery_pct,
                                            &raw, &gpio_mv) == ESP_OK);
    display_power_state_t display_state = display_get_power_state();
    int touch_int = display_get_touch_int_gpio();
#ifdef BOARD_CFG_TAB5KBD_INT_GPIO
    int keyboard_int = BOARD_CFG_TAB5KBD_INT_GPIO;
#else
    int keyboard_int = GPIO_NUM_NC;
#endif
    int wake_gpio = P4_CONFIG_POWER_WAKE_GPIO;
    bool deep_ok = shell_power_deep_wake_gpio_eligible(wake_gpio);
    const char *display_word;
    char wake_word[16];
    char touch_word[16];
    char keyboard_word[16];
    char sources[96];
    size_t used;
    int i = 0;

    switch (display_state) {
        case DISPLAY_POWER_SLEEP: display_word = "sleep"; break;
        case DISPLAY_POWER_OFF:   display_word = "off";   break;
        case DISPLAY_POWER_ON:
        default:                  display_word = "on";    break;
    }
    os_gpio_word(wake_gpio, wake_word, sizeof(wake_word));
    os_gpio_word(touch_int, touch_word, sizeof(touch_word));
    os_gpio_word(keyboard_int, keyboard_word, sizeof(keyboard_word));
    used = (size_t)snprintf(sources, sizeof(sources), "timer");
    if (wake_gpio >= 0 && used < sizeof(sources)) {
        used += (size_t)snprintf(sources + used, sizeof(sources) - used,
                                 ",%s", wake_word);
    }
    if (touch_int >= 0 && used < sizeof(sources)) {
        used += (size_t)snprintf(sources + used, sizeof(sources) - used,
                                 ",touch %s", touch_word);
    }
    if (keyboard_int >= 0 && used < sizeof(sources)) {
        used += (size_t)snprintf(sources + used, sizeof(sources) - used,
                                 ",keyboard %s", keyboard_word);
    }
    if (wake_gpio >= 0 && !deep_ok && used < sizeof(sources)) {
        (void)snprintf(sources + used, sizeof(sources) - used,
                       " (deep: timer only)");
    }

#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    os_field_set(&fields[i++], "os.pwr.pm", true, "enabled");
#else
    os_field_set(&fields[i++], "os.pwr.pm", true, "disabled");
#endif
    os_field_set(&fields[i++], "os.pwr.light_sleep", true, "%s",
                 shell_power_light_sleep_requested() ? "requested" : "off");
    os_field_set(&fields[i++], "os.pwr.idle_timeout_s", true, "%d",
                 shell_power_get_idle_timeout());
    os_field_set(&fields[i++], "os.pwr.ms_to_idle_off", true, "%d",
                 shell_power_ms_until_idle_off());
    os_field_set(&fields[i++], "os.pwr.display", true, "%s", display_word);
    os_field_set(&fields[i++], "os.pwr.battery_mv", battery_ok, "%d",
                 battery_ok ? battery_mv : 0);
    os_field_set(&fields[i++], "os.pwr.battery_pct", battery_ok, "%d",
                 battery_ok ? battery_pct : 0);
    os_field_set(&fields[i++], "os.pwr.wifi", true, "%s",
                 networking_wifi_is_connected() ? "connected" : "down");
    os_field_set(&fields[i++], "os.pwr.wake_gpio", true, "%s", wake_word);
    os_field_set(&fields[i++], "os.pwr.wake_level", true, "%s",
                 P4_CONFIG_POWER_WAKE_LEVEL ? "high" : "low");
    os_field_set(&fields[i++], "os.pwr.touch_int_gpio", true, "%s", touch_word);
    os_field_set(&fields[i++], "os.pwr.keyboard_int_gpio", true, "%s",
                 keyboard_word);
    os_field_set(&fields[i++], "os.pwr.deep_gpio_eligible", true, "%s",
                 deep_ok ? "yes" : "no");
    os_field_set(&fields[i++], "os.pwr.wake_cause", true, "%s",
                 shell_power_wake_cause_string(esp_sleep_get_wakeup_cause()));
    os_field_set(&fields[i++], "os.pwr.wake_sources", true, "%s", sources);
    *count = i;
    return true;
}

/* ver: compile-time build identity + the native ABI tag. No accessor can be
 * missing here, so this area is always available. */
static bool os_ver(os_field_t *fields, int *count)
{
    int i = 0;

    os_field_set(&fields[i++], "os.ver.version", true, "%s", P4_CONFIG_VERSION_STRING);
    os_field_set(&fields[i++], "os.ver.build", true, "%s %s", __DATE__, __TIME__);
    os_field_set(&fields[i++], "os.ver.abi", true, "%s", P4_CONFIG_NATIVE_ABI);
    os_field_set(&fields[i++], "os.ver.board", true, "%s", BOARD_CFG_ID);
    *count = i;
    return true;
}

int shell_command_os(int argc, char **argv)
{
    os_area_t area;
    os_mode_t mode;
    bool bare;
    os_field_t fields[OS_MAX_FIELDS];
    int count = 0;
    bool ok;
    char status[64];

    if (command_os_parse(argc, argv, &area, &mode, &bare) != 0 ||
        !command_os_mode_allows(area, mode)) {
        os_usage();
        batch_set_errorlevel(2);
        return 2;
    }
    switch (area) {
        case OS_AREA_PM:   ok = os_pm(mode, fields, &count); break;
        case OS_AREA_DRV:  ok = os_drv(fields, &count); break;
        case OS_AREA_IO:   ok = os_io(mode, fields, &count); break;
        case OS_AREA_FS:   ok = os_fs(mode, fields, &count); break;
        case OS_AREA_NET:  ok = os_net(fields, &count); break;
        case OS_AREA_MEM:  ok = os_mem(fields, &count); break;
        case OS_AREA_DISK: ok = os_disk(mode, fields, &count); break;
        case OS_AREA_SEC:  ok = os_sec(fields, &count); break;
        case OS_AREA_PWR:  ok = os_pwr(fields, &count); break;
        case OS_AREA_VER:  ok = os_ver(fields, &count); break;
        default:           ok = false; break;
    }
    if (!ok || count <= 0) {
        ok = false;
    }
    if (!os_fields_print(fields, count)) {
        ok = false;
    }
    (void)command_os_format_status(area, ok, status, sizeof(status));
    shell_transcript_appendf("%s\n", status);
    if (!bare) {
        shell_print_ok("os %s: %s", command_os_area_name(area),
                       command_os_status_name(ok));
    }
    batch_set_errorlevel(ok ? 0 : 1);
    return ok ? 0 : 1;
}
