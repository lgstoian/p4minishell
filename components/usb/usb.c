#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

#include "usb.h"
#include "p4minishell_config.h"

/* Forward declarations for shell utility functions (resolved at link time) */
extern bool shell_text_equals_ignore_case(const char *left, const char *right);
extern int shell_split_args(char *text, char **argv, int max_args);
extern void shell_format_size(char *dst, size_t dst_size, size_t bytes);
extern const char *shell_entry_type(const struct stat *st);

/* ---- Backward-compatibility aliases ---- */
#define USB_HOST_LIB_TASK_STACK_BYTES   P4_CONFIG_USB_HOST_LIB_TASK_STACK
#define USB_EVENT_TASK_STACK_BYTES      P4_CONFIG_USB_EVENT_TASK_STACK
#define USB_DRIVER_TASK_STACK_BYTES     P4_CONFIG_USB_DRIVER_TASK_STACK
#define USB_EVENT_QUEUE_DEPTH           P4_CONFIG_USB_EVENT_QUEUE_DEPTH
#define USB_TEXT_BYTES                  P4_CONFIG_USB_TEXT_BYTES
#define USB_PATH_BYTES                  P4_CONFIG_USB_PATH_BYTES
#define USB_MSC_BASE_PATH               P4_CONFIG_USB_MSC_BASE_PATH
#define USB_LIST_LIMIT                  P4_CONFIG_USB_LIST_LIMIT
#define USB_HID_REPORT_MAX_BYTES        P4_CONFIG_USB_HID_REPORT_MAX_BYTES
#define USB_KEYBOARD_KEYS               P4_CONFIG_USB_KEYBOARD_KEYS

/* USB HID key codes (standard USB HID usage table — not project-configurable) */
#define USB_HID_KEY_A 0x04
#define USB_HID_KEY_Z 0x1D
#define USB_HID_KEY_1 0x1E
#define USB_HID_KEY_9 0x26
#define USB_HID_KEY_0 0x27
#define USB_HID_KEY_ENTER 0x28
#define USB_HID_KEY_ESC 0x29
#define USB_HID_KEY_BACKSPACE 0x2A
#define USB_HID_KEY_TAB 0x2B
#define USB_HID_KEY_SPACE 0x2C
#define USB_HID_KEY_MINUS 0x2D
#define USB_HID_KEY_EQUAL 0x2E
#define USB_HID_KEY_LEFT_BRACE 0x2F
#define USB_HID_KEY_RIGHT_BRACE 0x30
#define USB_HID_KEY_BACKSLASH 0x31
#define USB_HID_KEY_SEMICOLON 0x33
#define USB_HID_KEY_APOSTROPHE 0x34
#define USB_HID_KEY_GRAVE 0x35
#define USB_HID_KEY_COMMA 0x36
#define USB_HID_KEY_PERIOD 0x37
#define USB_HID_KEY_SLASH 0x38
#define USB_HID_KEY_CAPS_LOCK 0x39
#define USB_HID_KEY_F1 0x3A
#define USB_HID_KEY_F12 0x45
#define USB_HID_KEY_PRINT_SCREEN 0x46
#define USB_HID_KEY_SCROLL_LOCK 0x47
#define USB_HID_KEY_PAUSE 0x48
#define USB_HID_KEY_INSERT 0x49
#define USB_HID_KEY_HOME 0x4A
#define USB_HID_KEY_PAGE_UP 0x4B
#define USB_HID_KEY_DELETE 0x4C
#define USB_HID_KEY_END 0x4D
#define USB_HID_KEY_PAGE_DOWN 0x4E
#define USB_HID_KEY_RIGHT 0x4F
#define USB_HID_KEY_LEFT 0x50
#define USB_HID_KEY_DOWN 0x51
#define USB_HID_KEY_UP 0x52
#define USB_HID_KEY_NUM_LOCK 0x53
#define USB_HID_KEY_KP_SLASH 0x54
#define USB_HID_KEY_KP_ASTERISK 0x55
#define USB_HID_KEY_KP_MINUS 0x56
#define USB_HID_KEY_KP_PLUS 0x57
#define USB_HID_KEY_KP_ENTER 0x58
#define USB_HID_KEY_KP_1 0x59
#define USB_HID_KEY_KP_9 0x61
#define USB_HID_KEY_KP_0 0x62
#define USB_HID_KEY_KP_PERIOD 0x63
#define USB_HID_MOD_LEFT_CTRL  0x01
#define USB_HID_MOD_LEFT_SHIFT 0x02
#define USB_HID_MOD_LEFT_ALT   0x04
#define USB_HID_MOD_LEFT_GUI   0x08
#define USB_HID_MOD_RIGHT_CTRL  0x10
#define USB_HID_MOD_RIGHT_SHIFT 0x20
#define USB_HID_MOD_RIGHT_ALT   0x40
#define USB_HID_MOD_RIGHT_GUI   0x80

typedef enum {
    USB_MODULE_EVENT_MSC_CONNECTED = 0,
    USB_MODULE_EVENT_MSC_DISCONNECTED,
    USB_MODULE_EVENT_HID_CONNECTED,
    USB_MODULE_EVENT_HID_INPUT,
    USB_MODULE_EVENT_HID_DISCONNECTED,
    USB_MODULE_EVENT_HID_TRANSFER_ERROR,
} usb_module_event_kind_t;

typedef struct {
    usb_module_event_kind_t kind;
    union {
        struct {
            uint8_t address;
        } msc_connected;
        struct {
            msc_host_device_handle_t handle;
        } msc_disconnected;
        struct {
            hid_host_device_handle_t handle;
        } hid_connected;
        struct {
            hid_host_device_handle_t handle;
            uint8_t proto;
            uint8_t data[USB_HID_REPORT_MAX_BYTES];
            size_t length;
        } hid_input;
        struct {
            hid_host_device_handle_t handle;
            uint8_t proto;
        } hid_interface;
    } data;
} usb_module_event_t;

typedef struct {
    bool attached;
    bool echo_enabled;
    hid_host_device_handle_t handle;
    hid_host_dev_params_t params;
} usb_hid_slot_t;

typedef struct {
    bool connected;
    bool mounted;
    msc_host_device_handle_t device;
    msc_host_vfs_handle_t vfs_handle;
    msc_host_device_info_t info;
} usb_msc_state_t;

extern void usb_host_transcript_append_text(const char *text);
extern void usb_host_schedule_transcript_append_text(const char *text);
extern void usb_host_record_error(esp_err_t error, const char *message);
extern void usb_host_record_warning(const char *message);
extern void usb_host_record_info(const char *message);
extern void usb_host_notify_header(const char *text, uint32_t timeout_ms);

static SemaphoreHandle_t s_usb_lock;
static QueueHandle_t s_usb_event_queue;
static bool s_usb_initialized;
static bool s_usb_host_installed;
static bool s_usb_msc_driver_installed;
static bool s_usb_hid_driver_installed;
static usb_msc_state_t s_usb_msc;
static usb_hid_slot_t s_usb_keyboard;
static usb_hid_slot_t s_usb_mouse;
static hid_keyboard_input_report_boot_t s_prev_keyboard_report;
static int s_mouse_x;
static int s_mouse_y;
static usb_keyboard_input_cb_t s_keyboard_input_cb;

static void usb_record_errorf(esp_err_t error, const char *format, ...)
{
    char buffer[USB_TEXT_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_record_error(error, buffer);
}

static void usb_record_warningf(const char *format, ...)
{
    char buffer[USB_TEXT_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_record_warning(buffer);
}

static void usb_record_infof(const char *format, ...)
{
    char buffer[USB_TEXT_BYTES];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_record_info(buffer);
}

static void usb_emit_syncf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_transcript_append_text(buffer);
}

static void usb_emit_asyncf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_schedule_transcript_append_text(buffer);
}

static void usb_notify_headerf(uint32_t timeout_ms, const char *format, ...)
{
    char buffer[160];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    usb_host_notify_header(buffer, timeout_ms);
}

/* text_equals_ignore_case and split_args are in shell.h via shell_text_equals_ignore_case and shell_split_args */

static void usb_wide_to_ascii(const wchar_t *input, char *output, size_t output_size)
{
    size_t index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    while (input[index] != 0 && index + 1 < output_size) {
        wchar_t ch = input[index];
        output[index] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        index++;
    }

    output[index] = '\0';
}

/* format_size and entry_type are in shell.h via shell_format_size and shell_entry_type */

static bool usb_keyboard_key_present(const uint8_t *keys, uint8_t key)
{
    size_t index;

    for (index = 0; index < USB_KEYBOARD_KEYS; index++) {
        if (keys[index] == key) {
            return true;
        }
    }
    return false;
}

static const char *usb_key_name(uint8_t key_code)
{
    switch (key_code) {
    case USB_HID_KEY_ENTER:      return "ENTER";
    case USB_HID_KEY_ESC:        return "ESC";
    case USB_HID_KEY_BACKSPACE:  return "BACKSPACE";
    case USB_HID_KEY_TAB:        return "TAB";
    case USB_HID_KEY_SPACE:      return "SPACE";
    case USB_HID_KEY_LEFT:       return "LEFT";
    case USB_HID_KEY_RIGHT:      return "RIGHT";
    case USB_HID_KEY_UP:         return "UP";
    case USB_HID_KEY_DOWN:       return "DOWN";
    case USB_HID_KEY_CAPS_LOCK:  return "CAPS";
    case USB_HID_KEY_INSERT:     return "INS";
    case USB_HID_KEY_HOME:       return "HOME";
    case USB_HID_KEY_PAGE_UP:    return "PGUP";
    case USB_HID_KEY_DELETE:     return "DEL";
    case USB_HID_KEY_END:        return "END";
    case USB_HID_KEY_PAGE_DOWN:  return "PGDN";
    case USB_HID_KEY_NUM_LOCK:   return "NUMLK";
    case USB_HID_KEY_PRINT_SCREEN: return "PRTSC";
    case USB_HID_KEY_SCROLL_LOCK:  return "SCRLK";
    case USB_HID_KEY_PAUSE:        return "PAUSE";
    default: return NULL;
    }
}

static bool usb_key_to_ascii(uint8_t key_code, uint8_t modifiers, char *character_out)
{
    const bool shifted = (modifiers & (USB_HID_MOD_LEFT_SHIFT | USB_HID_MOD_RIGHT_SHIFT)) != 0;

    if (character_out == NULL) {
        return false;
    }

    if (key_code >= USB_HID_KEY_A && key_code <= USB_HID_KEY_Z) {
        *character_out = (char)((shifted ? 'A' : 'a') + (key_code - USB_HID_KEY_A));
        return true;
    }

    if (key_code >= USB_HID_KEY_1 && key_code <= USB_HID_KEY_9) {
        if (shifted) {
            static const char shifted_nums[] = "!@#$%^&*(";
            *character_out = shifted_nums[key_code - USB_HID_KEY_1];
        } else {
            *character_out = (char)('1' + (key_code - USB_HID_KEY_1));
        }
        return true;
    }

    if (key_code == USB_HID_KEY_0) {
        *character_out = shifted ? ')' : '0';
        return true;
    }

    if (key_code == USB_HID_KEY_SPACE) {
        *character_out = ' ';
        return true;
    }

    if (key_code == USB_HID_KEY_MINUS) {
        *character_out = shifted ? '_' : '-';
        return true;
    }

    if (key_code == USB_HID_KEY_EQUAL) {
        *character_out = shifted ? '+' : '=';
        return true;
    }

    if (key_code == USB_HID_KEY_LEFT_BRACE) {
        *character_out = shifted ? '{' : '[';
        return true;
    }

    if (key_code == USB_HID_KEY_RIGHT_BRACE) {
        *character_out = shifted ? '}' : ']';
        return true;
    }

    if (key_code == USB_HID_KEY_BACKSLASH) {
        *character_out = shifted ? '|' : '\\';
        return true;
    }

    if (key_code == USB_HID_KEY_SEMICOLON) {
        *character_out = shifted ? ':' : ';';
        return true;
    }

    if (key_code == USB_HID_KEY_APOSTROPHE) {
        *character_out = shifted ? '"' : '\'';
        return true;
    }

    if (key_code == USB_HID_KEY_GRAVE) {
        *character_out = shifted ? '~' : '`';
        return true;
    }

    if (key_code == USB_HID_KEY_COMMA) {
        *character_out = shifted ? '<' : ',';
        return true;
    }

    if (key_code == USB_HID_KEY_PERIOD) {
        *character_out = shifted ? '>' : '.';
        return true;
    }

    if (key_code == USB_HID_KEY_SLASH) {
        *character_out = shifted ? '?' : '/';
        return true;
    }

    /* Keypad number keys */
    if (key_code >= USB_HID_KEY_KP_1 && key_code <= USB_HID_KEY_KP_9) {
        *character_out = (char)('1' + (key_code - USB_HID_KEY_KP_1));
        return true;
    }

    if (key_code == USB_HID_KEY_KP_0) {
        *character_out = '0';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_PERIOD) {
        *character_out = '.';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_SLASH) {
        *character_out = '/';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_ASTERISK) {
        *character_out = '*';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_MINUS) {
        *character_out = '-';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_PLUS) {
        *character_out = '+';
        return true;
    }

    if (key_code == USB_HID_KEY_KP_ENTER) {
        *character_out = '\n';
        return true;
    }

    if (key_code == USB_HID_KEY_ENTER) {
        *character_out = '\n';
        return true;
    }

    if (key_code == USB_HID_KEY_TAB) {
        *character_out = '\t';
        return true;
    }

    if (key_code == USB_HID_KEY_BACKSPACE) {
        *character_out = '\b';
        return true;
    }

    if (key_code == USB_HID_KEY_ESC) {
        *character_out = 0x1B;  /* ESC */
        return true;
    }

    return false;
}

/* ---- Public API: full key mapping (exported for shell use) ---- */

const char *usb_key_name_full(uint8_t key_code)
{
    /* Check F1-F12 range */
    if (key_code >= USB_HID_KEY_F1 && key_code <= USB_HID_KEY_F12) {
        static char fname[4];
        snprintf(fname, sizeof(fname), "F%d", (int)(key_code - USB_HID_KEY_F1 + 1));
        return fname;
    }
    return usb_key_name(key_code);
}

bool usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers, char *out)
{
    return usb_key_to_ascii(key_code, modifiers, out);
}

static usb_hid_slot_t *usb_slot_for_proto(uint8_t proto)
{
    if (proto == HID_PROTOCOL_KEYBOARD) {
        return &s_usb_keyboard;
    }
    if (proto == HID_PROTOCOL_MOUSE) {
        return &s_usb_mouse;
    }
    return NULL;
}

static void usb_print_usage(void)
{
    usb_emit_syncf("Usage:\n");
    usb_emit_syncf("  usb status\n");
    usb_emit_syncf("  usb ls [path]\n");
    usb_emit_syncf("  usb keyboard <on|off>\n");
    usb_emit_syncf("  usb mouse <on|off>\n");
    usb_emit_syncf("Paths: usb:/file.txt, /usb0/file.txt, or relative-to-/usb0\n");
}

static esp_err_t usb_resolve_path(const char *input, char *output, size_t output_size)
{
    int written;
    const char *source = input;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source == NULL || source[0] == '\0') {
        source = USB_MSC_BASE_PATH;
    }

    if (strncmp(source, "usb:/", 5) == 0) {
        written = snprintf(output, output_size, "%s%s", USB_MSC_BASE_PATH, source + 4);
    } else if (strcmp(source, USB_MSC_BASE_PATH) == 0 ||
               strncmp(source, USB_MSC_BASE_PATH "/", strlen(USB_MSC_BASE_PATH) + 1) == 0) {
        written = snprintf(output, output_size, "%s", source);
    } else if (source[0] == '/') {
        written = snprintf(output, output_size, "%s", source);
    } else {
        written = snprintf(output, output_size, "%s/%s", USB_MSC_BASE_PATH, source);
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

static void usb_clear_hid_slot(usb_hid_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static void usb_msc_unmount_locked(void)
{
    if (s_usb_msc.mounted && s_usb_msc.vfs_handle != NULL) {
        esp_err_t error = msc_host_vfs_unregister(s_usb_msc.vfs_handle);
        if (error != ESP_OK) {
            usb_record_warningf("USB MSC unmount warning: %s", esp_err_to_name(error));
        }
        s_usb_msc.vfs_handle = NULL;
        s_usb_msc.mounted = false;
    }
}

static void usb_queue_event(const usb_module_event_t *event)
{
    if (event == NULL || s_usb_event_queue == NULL) {
        return;
    }

    if (xQueueSend(s_usb_event_queue, event, 0) != pdTRUE) {
        usb_host_record_warning("USB event queue full");
    }
}

static void usb_hid_interface_callback(hid_host_device_handle_t hid_device_handle,
                                       const hid_host_interface_event_t event,
                                       void *arg)
{
    usb_module_event_t queued = { 0 };
    hid_host_dev_params_t params;

    (void)arg;
    if (hid_host_device_get_params(hid_device_handle, &params) != ESP_OK) {
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        size_t length = 0;

        queued.kind = USB_MODULE_EVENT_HID_INPUT;
        queued.data.hid_input.handle = hid_device_handle;
        queued.data.hid_input.proto = params.proto;
        if (hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                      queued.data.hid_input.data,
                                                      sizeof(queued.data.hid_input.data),
                                                      &length) != ESP_OK) {
            return;
        }
        queued.data.hid_input.length = length;
        usb_queue_event(&queued);
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED || event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        queued.kind = event == HID_HOST_INTERFACE_EVENT_DISCONNECTED ?
            USB_MODULE_EVENT_HID_DISCONNECTED : USB_MODULE_EVENT_HID_TRANSFER_ERROR;
        queued.data.hid_interface.handle = hid_device_handle;
        queued.data.hid_interface.proto = params.proto;
        usb_queue_event(&queued);
    }
}

static void usb_msc_event_callback(const msc_host_event_t *event, void *arg)
{
    usb_module_event_t queued = { 0 };

    (void)arg;
    if (event == NULL) {
        return;
    }

    if (event->event == MSC_DEVICE_CONNECTED) {
        queued.kind = USB_MODULE_EVENT_MSC_CONNECTED;
        queued.data.msc_connected.address = event->device.address;
        usb_queue_event(&queued);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        queued.kind = USB_MODULE_EVENT_MSC_DISCONNECTED;
        queued.data.msc_disconnected.handle = event->device.handle;
        usb_queue_event(&queued);
    }
}

static void usb_hid_driver_callback(hid_host_device_handle_t hid_device_handle,
                                    const hid_host_driver_event_t event,
                                    void *arg)
{
    usb_module_event_t queued = { 0 };

    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    queued.kind = USB_MODULE_EVENT_HID_CONNECTED;
    queued.data.hid_connected.handle = hid_device_handle;
    usb_queue_event(&queued);
}

static void usb_handle_keyboard_input_locked(const usb_module_event_t *event)
{
    hid_keyboard_input_report_boot_t report = { 0 };
    size_t index;

    if (event->data.hid_input.length < sizeof(report)) {
        return;
    }

    memcpy(&report, event->data.hid_input.data, sizeof(report));

    /* Route new key presses to the registered input callback (shell CLI injection).
     * Also detect key releases (keys in prev report but not current). */
    for (index = 0; index < USB_KEYBOARD_KEYS; index++) {
        uint8_t key_code = report.key[index];

        if (key_code == 0) {
            continue;
        }

        /* New key press (not in previous report) */
        if (!usb_keyboard_key_present(s_prev_keyboard_report.key, key_code)) {
            if (s_keyboard_input_cb != NULL) {
                s_keyboard_input_cb(key_code, report.modifier.val, USB_KEY_EVENT_PRESS);
            }
        }
    }

    /* Detect key releases (in previous report but not current) */
    for (index = 0; index < USB_KEYBOARD_KEYS; index++) {
        uint8_t key_code = s_prev_keyboard_report.key[index];

        if (key_code == 0) {
            continue;
        }

        if (!usb_keyboard_key_present(report.key, key_code)) {
            if (s_keyboard_input_cb != NULL) {
                s_keyboard_input_cb(key_code, s_prev_keyboard_report.modifier.val, USB_KEY_EVENT_RELEASE);
            }
        }
    }

    /* Echo mode: print key names to transcript for debug */
    if (s_usb_keyboard.echo_enabled) {
        for (index = 0; index < USB_KEYBOARD_KEYS; index++) {
            uint8_t key_code = report.key[index];
            char character = '\0';
            const char *name;

            if (key_code == 0 || usb_keyboard_key_present(s_prev_keyboard_report.key, key_code)) {
                continue;
            }

            name = usb_key_name(key_code);
            if (usb_key_to_ascii(key_code, report.modifier.val, &character)) {
                usb_emit_asyncf("usb keyboard: %c\n", character);
            } else if (name != NULL) {
                usb_emit_asyncf("usb keyboard: %s\n", name);
            } else {
                usb_emit_asyncf("usb keyboard: key 0x%02X\n", key_code);
            }
        }
    }

    memcpy(&s_prev_keyboard_report, &report, sizeof(report));
}

static void usb_handle_mouse_input_locked(const usb_module_event_t *event)
{
    hid_mouse_input_report_boot_t report = { 0 };

    if (event->data.hid_input.length < sizeof(report) || !s_usb_mouse.echo_enabled) {
        return;
    }

    memcpy(&report, event->data.hid_input.data, sizeof(report));
    s_mouse_x += report.x_displacement;
    s_mouse_y += report.y_displacement;
    usb_emit_asyncf("usb mouse: X=%06d Y=%06d |%c|%c|\n",
                    s_mouse_x,
                    s_mouse_y,
                    report.buttons.button1 ? 'o' : ' ',
                    report.buttons.button2 ? 'o' : ' ');
}

static void usb_module_task(void *arg)
{
    usb_module_event_t event;

    (void)arg;
    while (xQueueReceive(s_usb_event_queue, &event, portMAX_DELAY) == pdTRUE) {
        switch (event.kind) {
        case USB_MODULE_EVENT_MSC_CONNECTED: {
            char manufacturer[64];
            char product[64];
            char serial[64];
            esp_err_t error;

            if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
                break;
            }

            if (s_usb_msc.device != NULL) {
                usb_msc_unmount_locked();
                (void)msc_host_uninstall_device(s_usb_msc.device);
                memset(&s_usb_msc, 0, sizeof(s_usb_msc));
            }

            error = msc_host_install_device(event.data.msc_connected.address, &s_usb_msc.device);
            if (error == ESP_OK) {
                s_usb_msc.connected = true;
                if (msc_host_get_device_info(s_usb_msc.device, &s_usb_msc.info) == ESP_OK) {
                    usb_wide_to_ascii(s_usb_msc.info.iManufacturer, manufacturer, sizeof(manufacturer));
                    usb_wide_to_ascii(s_usb_msc.info.iProduct, product, sizeof(product));
                    usb_wide_to_ascii(s_usb_msc.info.iSerialNumber, serial, sizeof(serial));
                    usb_emit_asyncf("usb: MSC device connected VID=0x%04X PID=0x%04X %s %s%s%s\n",
                                    s_usb_msc.info.idVendor,
                                    s_usb_msc.info.idProduct,
                                    manufacturer[0] != '\0' ? manufacturer : "<unknown>",
                                    product[0] != '\0' ? product : "<unnamed>",
                                    serial[0] != '\0' ? " sn=" : "",
                                    serial);
                    usb_notify_headerf(4500, "USB MSC connected");
                } else {
                    usb_emit_asyncf("usb: MSC device connected at /usb0\n");
                    usb_notify_headerf(4500, "USB MSC connected");
                }
            } else {
                usb_record_errorf(error, "USB MSC install_device failed for address %u", (unsigned int)event.data.msc_connected.address);
            }

            xSemaphoreGive(s_usb_lock);
            break;
        }
        case USB_MODULE_EVENT_MSC_DISCONNECTED:
            if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
                break;
            }
            if (s_usb_msc.device == event.data.msc_disconnected.handle) {
                usb_msc_unmount_locked();
                (void)msc_host_uninstall_device(s_usb_msc.device);
                memset(&s_usb_msc, 0, sizeof(s_usb_msc));
                usb_emit_asyncf("usb: MSC device disconnected\n");
                usb_notify_headerf(4500, "USB MSC disconnected");
            }
            xSemaphoreGive(s_usb_lock);
            break;
        case USB_MODULE_EVENT_HID_CONNECTED: {
            hid_host_device_config_t config = {
                .callback = usb_hid_interface_callback,
                .callback_arg = NULL,
            };
            hid_host_dev_params_t params;
            usb_hid_slot_t *slot;
            esp_err_t error;

            memset(&params, 0, sizeof(params));
            error = hid_host_device_get_params(event.data.hid_connected.handle, &params);
            if (error != ESP_OK) {
                usb_record_errorf(error, "USB HID get_params failed");
                break;
            }

            slot = usb_slot_for_proto(params.proto);
            if (slot == NULL) {
                usb_record_infof("USB HID interface connected with unsupported protocol %u", (unsigned int)params.proto);
                break;
            }

            error = hid_host_device_open(event.data.hid_connected.handle, &config);
            if (error != ESP_OK) {
                usb_record_errorf(error, "USB HID open failed for protocol %u", (unsigned int)params.proto);
                break;
            }

            if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                error = hid_class_request_set_protocol(event.data.hid_connected.handle, HID_REPORT_PROTOCOL_BOOT);
                if (error != ESP_OK) {
                    usb_record_warningf("USB HID boot protocol request failed for protocol %u: %s",
                                        (unsigned int)params.proto,
                                        esp_err_to_name(error));
                }
            }

            error = hid_host_device_start(event.data.hid_connected.handle);
            if (error != ESP_OK) {
                usb_record_errorf(error, "USB HID start failed for protocol %u", (unsigned int)params.proto);
                (void)hid_host_device_close(event.data.hid_connected.handle);
                break;
            }

            if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) == pdTRUE) {
                slot->attached = true;
                slot->handle = event.data.hid_connected.handle;
                slot->params = params;
                if (params.proto == HID_PROTOCOL_KEYBOARD) {
                    memset(&s_prev_keyboard_report, 0, sizeof(s_prev_keyboard_report));
                } else {
                    s_mouse_x = 0;
                    s_mouse_y = 0;
                }
                usb_emit_asyncf("usb: HID %s connected\n", params.proto == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse");
                usb_notify_headerf(3500,
                                   params.proto == HID_PROTOCOL_KEYBOARD ? "USB keyboard connected" : "USB mouse connected");
                xSemaphoreGive(s_usb_lock);
            }
            break;
        }
        case USB_MODULE_EVENT_HID_INPUT:
            if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
                break;
            }
            if (event.data.hid_input.proto == HID_PROTOCOL_KEYBOARD) {
                usb_handle_keyboard_input_locked(&event);
            } else if (event.data.hid_input.proto == HID_PROTOCOL_MOUSE) {
                usb_handle_mouse_input_locked(&event);
            }
            xSemaphoreGive(s_usb_lock);
            break;
        case USB_MODULE_EVENT_HID_DISCONNECTED:
            if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
                break;
            }
            if (s_usb_keyboard.handle == event.data.hid_interface.handle) {
                (void)hid_host_device_stop(s_usb_keyboard.handle);
                (void)hid_host_device_close(s_usb_keyboard.handle);
                usb_clear_hid_slot(&s_usb_keyboard);
                memset(&s_prev_keyboard_report, 0, sizeof(s_prev_keyboard_report));
                usb_emit_asyncf("usb: HID keyboard disconnected\n");
                usb_notify_headerf(3500, "USB keyboard disconnected");
            }
            if (s_usb_mouse.handle == event.data.hid_interface.handle) {
                (void)hid_host_device_stop(s_usb_mouse.handle);
                (void)hid_host_device_close(s_usb_mouse.handle);
                usb_clear_hid_slot(&s_usb_mouse);
                s_mouse_x = 0;
                s_mouse_y = 0;
                usb_emit_asyncf("usb: HID mouse disconnected\n");
                usb_notify_headerf(3500, "USB mouse disconnected");
            }
            xSemaphoreGive(s_usb_lock);
            break;
        case USB_MODULE_EVENT_HID_TRANSFER_ERROR:
            usb_record_warningf("USB HID transfer error on %s",
                                event.data.hid_interface.proto == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse");
            break;
        default:
            break;
        }
    }

    vTaskDelete(NULL);
}

static void usb_host_lib_task(void *arg)
{
    uint32_t event_flags;

    (void)arg;
    while (s_usb_host_installed) {
        if (usb_host_lib_handle_events(portMAX_DELAY, &event_flags) != ESP_OK) {
            continue;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_record_infof("USB host library reports no clients");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            usb_record_infof("USB host library reports all devices freed");
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t usb_install_host_stack(void)
{
#if SOC_USB_OTG_SUPPORTED
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = NULL,
    };
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = USB_DRIVER_TASK_STACK_BYTES,
        .core_id = 0,
        .callback = usb_msc_event_callback,
        .callback_arg = NULL,
    };
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = USB_DRIVER_TASK_STACK_BYTES,
        .core_id = 0,
        .callback = usb_hid_driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t error;

    error = usb_host_install(&host_config);
    if (error != ESP_OK) {
        return error;
    }
    s_usb_host_installed = true;

    error = msc_host_install(&msc_config);
    if (error != ESP_OK) {
        return error;
    }
    s_usb_msc_driver_installed = true;

    error = hid_host_install(&hid_config);
    if (error != ESP_OK) {
        return error;
    }
    s_usb_hid_driver_installed = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// USB module owns the full host stack bring-up and keeps main/main.c limited to orchestration plus shell dispatch.
// Existing boot, SD, Wi-Fi, Bluetooth, and c6ota behavior must remain unchanged; this module only adds new USB command-family behavior.
void usb_init(void)
{
    esp_err_t error;

    if (s_usb_initialized) {
        return;
    }

    s_usb_lock = xSemaphoreCreateMutex();
    s_usb_event_queue = xQueueCreate(USB_EVENT_QUEUE_DEPTH, sizeof(usb_module_event_t));
    if (s_usb_lock == NULL || s_usb_event_queue == NULL) {
        usb_record_errorf(ESP_ERR_NO_MEM, "USB module initialization failed: queue or mutex allocation");
        return;
    }

    error = usb_install_host_stack();
    if (error != ESP_OK) {
        usb_record_errorf(error, "USB host stack install failed");
        return;
    }

    if (xTaskCreatePinnedToCore(usb_host_lib_task,
                                "usb_host_lib",
                                USB_HOST_LIB_TASK_STACK_BYTES,
                                NULL,
                                4,
                                NULL,
                                0) != pdPASS) {
        usb_record_errorf(ESP_ERR_NO_MEM, "USB host library task creation failed");
        return;
    }

    if (xTaskCreatePinnedToCore(usb_module_task,
                                "usb_module",
                                USB_EVENT_TASK_STACK_BYTES,
                                NULL,
                                4,
                                NULL,
                                0) != pdPASS) {
        usb_record_errorf(ESP_ERR_NO_MEM, "USB module task creation failed");
        return;
    }

    s_usb_initialized = true;
    usb_record_infof("USB host module initialized");
}

void usb_status(void)
{
    char capacity[32];
    char manufacturer[64];
    char product[64];
    char serial[64];

    if (!s_usb_initialized) {
        usb_emit_syncf("usb: host stack unavailable\n");
        return;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    usb_emit_syncf("usb.host: %s\n", s_usb_host_installed ? "ready" : "not_ready");
    usb_emit_syncf("usb.msc: connected=%s mounted=%s path=%s\n",
                   s_usb_msc.connected ? "yes" : "no",
                   s_usb_msc.mounted ? "yes" : "no",
                   USB_MSC_BASE_PATH);
    if (s_usb_msc.connected) {
        usb_wide_to_ascii(s_usb_msc.info.iManufacturer, manufacturer, sizeof(manufacturer));
        usb_wide_to_ascii(s_usb_msc.info.iProduct, product, sizeof(product));
        usb_wide_to_ascii(s_usb_msc.info.iSerialNumber, serial, sizeof(serial));
        shell_format_size(capacity, sizeof(capacity),
                          (size_t)((uint64_t)s_usb_msc.info.sector_size * (uint64_t)s_usb_msc.info.sector_count));
        usb_emit_syncf("usb.msc.vendor: 0x%04X\n", s_usb_msc.info.idVendor);
        usb_emit_syncf("usb.msc.product: 0x%04X\n", s_usb_msc.info.idProduct);
        usb_emit_syncf("usb.msc.name: %s %s\n",
                       manufacturer[0] != '\0' ? manufacturer : "<unknown>",
                       product[0] != '\0' ? product : "<unnamed>");
        if (serial[0] != '\0') {
            usb_emit_syncf("usb.msc.serial: %s\n", serial);
        }
        usb_emit_syncf("usb.msc.capacity: %s\n", capacity);
        usb_emit_syncf("usb.msc.sector_size: %u\n", (unsigned int)s_usb_msc.info.sector_size);
        usb_emit_syncf("usb.msc.sector_count: %u\n", (unsigned int)s_usb_msc.info.sector_count);
    }
    usb_emit_syncf("usb.hid.keyboard: attached=%s echo=%s\n",
                   s_usb_keyboard.attached ? "yes" : "no",
                   s_usb_keyboard.echo_enabled ? "on" : "off");
    usb_emit_syncf("usb.hid.mouse: attached=%s echo=%s\n",
                   s_usb_mouse.attached ? "yes" : "no",
                   s_usb_mouse.echo_enabled ? "on" : "off");

    xSemaphoreGive(s_usb_lock);
}

void usb_msc_mount(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,
    };
    esp_err_t error;

    if (!s_usb_initialized) {
        usb_emit_syncf("usb: host stack unavailable\n");
        return;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    if (!s_usb_msc.connected || s_usb_msc.device == NULL) {
        xSemaphoreGive(s_usb_lock);
        usb_emit_syncf("usb: MSC drive not connected - insert and retry\n");
        return;
    }

    if (s_usb_msc.mounted) {
        xSemaphoreGive(s_usb_lock);
        usb_emit_syncf("usb: MSC already mounted at %s\n", USB_MSC_BASE_PATH);
        return;
    }

    error = msc_host_vfs_register(s_usb_msc.device, USB_MSC_BASE_PATH, &mount_config, &s_usb_msc.vfs_handle);
    if (error == ESP_OK) {
        s_usb_msc.mounted = true;
        xSemaphoreGive(s_usb_lock);
        usb_emit_syncf("usb: mounted %s\n", USB_MSC_BASE_PATH);
        usb_notify_headerf(3500, "USB storage mounted");
        return;
    }

    xSemaphoreGive(s_usb_lock);
    usb_emit_syncf("usb: mount failed (%s)\n", esp_err_to_name(error));
    usb_record_errorf(error, "USB MSC mount failed");
}

void usb_msc_ls(const char *path)
{
    char resolved_path[USB_PATH_BYTES];
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    esp_err_t error;
    size_t count = 0;

    if (!s_usb_initialized) {
        usb_emit_syncf("usb: host stack unavailable\n");
        return;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) == pdTRUE) {
        bool needs_mount = s_usb_msc.connected && !s_usb_msc.mounted;
        xSemaphoreGive(s_usb_lock);
        if (needs_mount) {
            usb_msc_mount();
        }
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }
    if (!s_usb_msc.mounted) {
        xSemaphoreGive(s_usb_lock);
        usb_emit_syncf("usb: MSC drive not mounted\n");
        return;
    }
    xSemaphoreGive(s_usb_lock);

    error = usb_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        usb_emit_syncf("usb: invalid path\n");
        return;
    }

    if (stat(resolved_path, &st) != 0) {
        usb_emit_syncf("usb: path not found %s\n", resolved_path);
        return;
    }

    if (S_ISREG(st.st_mode)) {
        char size_text[32];

        shell_format_size(size_text, sizeof(size_text), (size_t)st.st_size);
        usb_emit_syncf("usb: FILE %s (%s)\n", resolved_path, size_text);
        return;
    }

    dir = opendir(resolved_path);
    if (dir == NULL) {
        usb_emit_syncf("usb: failed to open %s (%s)\n", resolved_path, strerror(errno));
        return;
    }

    usb_emit_syncf("usb: listing %s\n", resolved_path);
    while ((entry = readdir(dir)) != NULL) {
        char child_path[USB_PATH_BYTES];
        struct stat child_stat;
        char size_text[32];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (++count > USB_LIST_LIMIT) {
            usb_emit_syncf("usb: listing truncated after %u entries\n", (unsigned int)USB_LIST_LIMIT);
            break;
        }

        if (snprintf(child_path, sizeof(child_path), "%s/%s", resolved_path, entry->d_name) >= (int)sizeof(child_path)) {
            usb_emit_syncf("usb: path too long for %s\n", entry->d_name);
            continue;
        }

        if (stat(child_path, &child_stat) != 0) {
            usb_emit_syncf("usb: OTHER %s\n", entry->d_name);
            continue;
        }

        if (S_ISDIR(child_stat.st_mode)) {
            usb_emit_syncf("usb: %s  %s (-)\n", shell_entry_type(&child_stat), entry->d_name);
        } else {
            shell_format_size(size_text, sizeof(size_text), (size_t)child_stat.st_size);
            usb_emit_syncf("usb: %s %s (%s)\n", shell_entry_type(&child_stat), entry->d_name, size_text);
        }
    }

    closedir(dir);
}

void usb_hid_keyboard_enable(void)
{
    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    s_usb_keyboard.echo_enabled = true;
    xSemaphoreGive(s_usb_lock);
    usb_emit_syncf("usb: keyboard echo on\n");
}

void usb_hid_keyboard_disable(void)
{
    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    s_usb_keyboard.echo_enabled = false;
    xSemaphoreGive(s_usb_lock);
    usb_emit_syncf("usb: keyboard echo off\n");
}

void usb_hid_mouse_enable(void)
{
    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    s_usb_mouse.echo_enabled = true;
    xSemaphoreGive(s_usb_lock);
    usb_emit_syncf("usb: mouse echo on\n");
}

void usb_hid_mouse_disable(void)
{
    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        usb_emit_syncf("usb: state unavailable\n");
        return;
    }

    s_usb_mouse.echo_enabled = false;
    xSemaphoreGive(s_usb_lock);
    usb_emit_syncf("usb: mouse echo off\n");
}

bool usb_is_connected(void)
{
    bool connected = false;

    if (!s_usb_initialized) {
        return false;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    connected = s_usb_msc.connected || s_usb_keyboard.attached || s_usb_mouse.attached;
    xSemaphoreGive(s_usb_lock);
    return connected;
}

bool usb_is_mounted(void)
{
    bool mounted = false;

    if (!s_usb_initialized) {
        return false;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    mounted = s_usb_msc.mounted;
    xSemaphoreGive(s_usb_lock);
    return mounted;
}

bool usb_is_keyboard_attached(void)
{
    bool attached = false;

    if (!s_usb_initialized) {
        return false;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    attached = s_usb_keyboard.attached;
    xSemaphoreGive(s_usb_lock);
    return attached;
}

bool usb_is_mouse_attached(void)
{
    bool attached = false;

    if (!s_usb_initialized) {
        return false;
    }

    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    attached = s_usb_mouse.attached;
    xSemaphoreGive(s_usb_lock);
    return attached;
}

void usb_register_keyboard_input_callback(usb_keyboard_input_cb_t cb)
{
    if (xSemaphoreTake(s_usb_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_keyboard_input_cb = cb;
    xSemaphoreGive(s_usb_lock);
}

void usb_handle_command(char *command)
{
    char buffer[USB_PATH_BYTES];
    char *argv[6];
    int argc;

    if (command == NULL) {
        usb_print_usage();
        return;
    }

    snprintf(buffer, sizeof(buffer), "%s", command);
    argc = shell_split_args(buffer, argv, 6);
    if (argc <= 1) {
        usb_status();
        usb_print_usage();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        usb_status();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "ls")) {
        usb_msc_ls(argc >= 3 ? argv[2] : NULL);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "keyboard")) {
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "on")) {
            usb_hid_keyboard_enable();
            return;
        }
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "off")) {
            usb_hid_keyboard_disable();
            return;
        }
        usb_print_usage();
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "mouse")) {
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "on")) {
            usb_hid_mouse_enable();
            return;
        }
        if (argc >= 3 && shell_text_equals_ignore_case(argv[2], "off")) {
            usb_hid_mouse_disable();
            return;
        }
        usb_print_usage();
        return;
    }

    usb_print_usage();
}