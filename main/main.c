/**
 * @file main.c
 * @brief P4MiniShell application entry point and LVGL event orchestration.
 *
 * This layer is intentionally thin. It owns only:
 *   - Boot sequencing (display, shell, command, modules, timers)
 *   - LVGL event callbacks for the transcript, input line, and history buttons
 *   - UI construction and rebuild after display rotation
 *   - Host bridge callbacks that components/c6ota and components/usb link
 *     against (ESP-IDF requires these to live in the app component)
 *
 * Everything else lives in the component modules:
 *   - components/shell/   transcript, history, debug log, UART console, input line
 *   - components/command/ every built-in command, shell state, batch engine
 *   - components/display/ display and touch hardware
 *   - components/windows/ LVGL screen layout
 *   - components/header/  fixed status bar
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "p4minishell.h"

#include "bluetooth.h"
#include "c6ota.h"
#include "clock.h"
#include "command.h"
#include "display.h"
#include "header.h"
#include "keyboard.h"
#include "networking.h"
#include "ansi_palette.h"
#include "shell.h"
#include "boot.h"
#include "usb.h"
#include "windows.h"

/* Backward-compatibility aliases */
#define SHELL_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_BOOT_MESSAGE              P4_CONFIG_BOOT_MESSAGE
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_HEADER_REFRESH_PERIOD_MS  P4_CONFIG_HEADER_REFRESH_PERIOD_MS

/* Periodic timer that drives the header status panel */
static lv_timer_t *s_header_status_timer;

static void shell_build_ui(void);

/* ========================================================================
 * HOST BRIDGE CALLBACKS
 * ========================================================================
 * components/c6ota and components/usb declare these as extern and rely on
 * the linker resolving them from the app (main) component. They are thin
 * adapters onto the shell core's transcript and debug-log API.
 */

void c6ota_host_transcript_append_text(const char *text)
{
    shell_transcript_append_text(text);
}

void c6ota_host_schedule_transcript_append_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void c6ota_host_record_error(esp_err_t error, const char *message)
{
    shell_record_errorf("c6ota", error, "%s", message != NULL ? message : "");
}

void c6ota_host_record_warning(const char *message)
{
    shell_record_warningf("c6ota", "%s", message != NULL ? message : "");
}

void c6ota_host_record_info(const char *message)
{
    shell_record_infof("c6ota", "%s", message != NULL ? message : "");
}

void c6ota_host_notify_header(const char *text, uint32_t timeout_ms)
{
    shell_header_notify(text, timeout_ms);
}

void usb_host_transcript_append_text(const char *text)
{
    /* Routed through the ANSI path so palette macros used by the USB module
     * are interpreted; text without specifiers is unaffected. */
    shell_transcript_append_ansi(text);
}

void usb_host_schedule_transcript_append_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void usb_host_record_error(esp_err_t error, const char *message)
{
    shell_record_errorf("usb", error, "%s", message != NULL ? message : "");
}

void usb_host_record_warning(const char *message)
{
    shell_record_warningf("usb", "%s", message != NULL ? message : "");
}

void usb_host_record_info(const char *message)
{
    shell_record_infof("usb", "%s", message != NULL ? message : "");
}

void usb_host_notify_header(const char *text, uint32_t timeout_ms)
{
    shell_header_notify(text, timeout_ms);
}

/* ========================================================================
 * MODULE CALLBACKS
 * ======================================================================== */

/**
 * C6 OTA progress sink.
 * percent == -1 marks a synchronous message that must land in the transcript
 * immediately; anything else is staged through the async transcript buffer
 * because it arrives from the OTA worker task.
 *
 * Colour is applied here rather than inside components/c6ota so the module
 * keeps emitting the exact contractual strings documented in API.md. The
 * message text is never altered — only wrapped in a palette colour chosen
 * from its content:
 *   - the confirmation warning and the YES prompt are shown as errors,
 *     because they precede a destructive, irreversible action
 *   - failures and cancellations are shown as warnings or errors
 *   - completion and live progress are shown in the OTA accent colour
 */
static void shell_c6ota_progress_callback(int percent, const char *msg)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    const char *colour;

    if (msg == NULL || msg[0] == '\0') {
        return;
    }

    if (strstr(msg, "WARNING:") != NULL || strstr(msg, "type YES") != NULL) {
        colour = SH_ERR_HI;
    } else if (strstr(msg, "failed") != NULL || strstr(msg, "cancelled") != NULL) {
        colour = SH_ERR;
    } else if (strstr(msg, "warning") != NULL) {
        colour = SH_WARN;
    } else if (strstr(msg, "successfully") != NULL) {
        colour = SH_OK_HI;
    } else {
        colour = SH_OTA;
    }

    /* The message already carries its own newline, so the colour wraps the
     * text and the reset lands before that newline would matter. */
    snprintf(line, sizeof(line), "%s%s" SH_RST, colour, msg);

    if (percent == -1) {
        shell_transcript_append_ansi(line);
        return;
    }

    shell_schedule_transcript_appendf("%s", line);
}

/** Adapter matching usb_keyboard_input_cb_t for the shell CLI injection path. */
static void shell_usb_keyboard_cb(uint8_t key_code, uint8_t modifiers, usb_key_event_t event)
{
    shell_usb_keyboard_input(key_code, modifiers, (event == USB_KEY_EVENT_PRESS));
}

/** LVGL timer callback driving the periodic header status refresh. */
static void shell_header_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    shell_header_status_refresh();
}

/* ========================================================================
 * LVGL EVENT CALLBACKS
 * ======================================================================== */

static void shell_history_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *prev_btn = windows_get_prev_button();
    lv_obj_t *next_btn = windows_get_next_button();

    if (prev_btn != NULL && lv_event_get_target(event) == prev_btn) {
        shell_recall_history(-1);
    } else if (next_btn != NULL && lv_event_get_target(event) == next_btn) {
        shell_recall_history(1);
    }
}

/* Tapping the transcript moves focus and the keyboard binding back to the
 * input line so typing always goes to the prompt. */
static void shell_transcript_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_obj_t *kb = windows_get_keyboard();
        lv_obj_t *il = windows_get_input_line();

        if (kb != NULL && il != NULL) {
            lv_keyboard_set_textarea(kb, il);
        }
        if (il != NULL) {
            lv_obj_add_state(il, LV_STATE_FOCUSED);
            lv_textarea_set_cursor_pos(il, LV_TEXTAREA_CURSOR_LAST);
        }
    }
}

static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_obj_t *kb = windows_get_keyboard();
        lv_obj_t *il = windows_get_input_line();

        if (kb != NULL && il != NULL) {
            lv_keyboard_set_textarea(kb, il);
        }
        if (il != NULL) {
            lv_textarea_set_cursor_pos(il, LV_TEXTAREA_CURSOR_LAST);
        }

        /* Touch-to-show-keyboard: tapping the input line raises the OSK,
         * matching the Windows 11 touch keyboard behavior. Suppressed while
         * a USB keyboard is driving the input line. */
        if (!keyboard_is_visible() && !keyboard_is_external_input_enabled()) {
            keyboard_show();
        }
        return;
    }

    /* The LVGL keyboard's hide button (LV_SYMBOL_KEYBOARD) sends
     * LV_EVENT_CANCEL to the bound textarea. */
    if (code == LV_EVENT_CANCEL) {
        keyboard_hide();
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *il = windows_get_input_line();
        const char *text = il != NULL ? lv_textarea_get_text(il) : NULL;

        if (text == NULL) {
            return;
        }

        /* While a command is waiting on a keypress (pause, choice, more),
         * the on-screen keyboard feeds the key queue instead of the command
         * line. The typed character is consumed and the line is restored so
         * the answer never lingers at the prompt. */
        if (shell_key_wait_is_active()) {
            char typed[SHELL_COMMAND_BYTES];

            shell_extract_input_text(typed, sizeof(typed));
            if (typed[0] != '\0') {
                shell_key_wait_submit(typed[0]);
            }
            shell_input_line_reset();
            return;
        }

        /* Guard against backspace deleting into the prompt prefix. */
        shell_input_line_repair_prompt(text);
        return;
    }

    if (code == LV_EVENT_READY) {
        char command[SHELL_COMMAND_BYTES];
        char transcript_command[SHELL_COMMAND_BYTES];

        /* A pending keypress wait swallows the submission: Enter answers the
         * prompt rather than dispatching a command. */
        if (shell_key_wait_is_active()) {
            shell_key_wait_submit('\r');
            shell_input_line_reset();
            return;
        }

        /* LV_EVENT_READY is the confirmed submission path, including
         * YES/NO replies to the C6 OTA confirmation prompt. */
        shell_extract_input_text(command, sizeof(command));
        shell_format_command_for_transcript(command, transcript_command, sizeof(transcript_command));
        shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);

        if (shell_command_should_store_history(command)) {
            shell_store_command_history(command);
        }
        shell_reset_history_cursor();
        shell_input_line_reset();

        /* Dispatch on the worker task so heavy commands never run on the
         * LVGL event-callback stack. */
        shell_execute_command_async(command);
        shell_history_transcript_scroll_to_end();
        return;
    }

    if (code == LV_EVENT_KEY) {
        uint32_t key = *((const uint32_t *)lv_event_get_param(event));

        if (key == LV_KEY_UP) {
            shell_recall_history(-1);
        } else if (key == LV_KEY_DOWN) {
            shell_recall_history(1);
        }
    }
}

/* Logs on-screen keyboard mode changes and special button presses. */
static void shell_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *target = lv_event_get_current_target(event);
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(target);

        if (btn_id != LV_BUTTONMATRIX_BUTTON_NONE) {
            const char *txt = lv_buttonmatrix_get_button_text(target, btn_id);
            if (txt != NULL) {
                ESP_LOGI(SHELL_TAG, "Keyboard button: %s (id=%" PRIu32 ")", txt, btn_id);
            }
        }
    }
}

/* ========================================================================
 * UI CONSTRUCTION
 * ======================================================================== */

/**
 * Rebuild the whole LVGL surface. Registered with the display manager so it
 * runs on the LVGL task (with adequate stack) after a rotation change.
 */
static void shell_rebuild_ui_callback(void)
{
    shell_build_ui();
}

static void shell_build_ui(void)
{
    esp_err_t err;

    /* Tear the window manager down first so every widget from the previous
     * layout is released cleanly. Required for display rotation support. */
    windows_deinit();

    /* Rebuild header, transcript, input row, and keyboard with
     * resolution-aware scaling for the current rotation. */
    err = windows_init();
    if (err != ESP_OK) {
        ESP_LOGE(SHELL_TAG, "Window manager initialization failed");
        shell_record_errorf("init", ESP_FAIL, "Window manager init failed");
        return;
    }

    lv_obj_t *transcript = windows_get_transcript();
    lv_obj_t *input_line = windows_get_input_line();
    lv_obj_t *prev_btn = windows_get_prev_button();
    lv_obj_t *next_btn = windows_get_next_button();

    if (transcript != NULL) {
        lv_obj_add_event_cb(transcript, shell_transcript_event_cb, LV_EVENT_ALL, NULL);
    }
    if (input_line != NULL) {
        lv_obj_add_event_cb(input_line, shell_input_line_event_cb, LV_EVENT_ALL, NULL);
    }
    if (prev_btn != NULL) {
        lv_obj_add_event_cb(prev_btn, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    }
    if (next_btn != NULL) {
        lv_obj_add_event_cb(next_btn, shell_history_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Keyboard callback receives LV_EVENT_VALUE_CHANGED for mode and button
     * presses, LV_EVENT_READY for OK, and LV_EVENT_CANCEL for hide. */
    keyboard_register_event_callback(shell_keyboard_event_cb, NULL);

    shell_transcript_reset();
    shell_transcript_appendf_ansi("@G%s@R\n", SHELL_BOOT_MESSAGE);
    shell_history_transcript_scroll_to_end();
    shell_input_line_reset();
}

/* ========================================================================
 * APPLICATION ENTRY POINT
 * ======================================================================== */

void app_main(void)
{
    esp_err_t disp_error;

    /* Initialize the display manager first. It wraps
     * bsp_display_start_with_config() and owns all display state
     * (rotation, brightness, power, touch handle). */
    disp_error = display_init();
    if (disp_error != ESP_OK) {
        ESP_LOGE(SHELL_TAG, "Display initialization failed");
        return;
    }

    /* Shell core owns the transcript, history, debug log, and boot
     * timestamp; the command module owns shell state and registers its
     * operations table with the shell core. */
    shell_init();
    command_init();

    /* Let the display manager trigger a full UI rebuild after rotation. */
    display_register_ui_rebuild_callback(shell_rebuild_ui_callback);

    shell_uart_console_start();

    /* Display and backlight are already up from display_init(). Build the
     * LVGL surface so the header bar and transcript share one screen. */
    bsp_display_lock(0);
    shell_build_ui();
    bsp_display_unlock();

    if (display_get_touch_handle() == NULL) {
        shell_record_warningf("init",
                              "GT911 touch handle lookup failed after BSP startup; rotate will stay display-only");
    }

    /* Record a healthy boot in the debug history rather than as a warning. */
    shell_record_infof("shell", "Shell UI initialized; boot banner is shown on the display transcript");

    shell_transcript_append_text("UART monitor accepts the same shell commands as the on-screen prompt.\n");
    shell_transcript_append_text("Wi-Fi starts in the background on boot; wifi diag is also available for an extra status + scan report.\n");
    shell_transcript_append_text("Bluetooth commands are routed through the ESP32-C6 hosted NimBLE module.\n");
    shell_transcript_append_text("USB host commands are available as usb status, usb ls, usb keyboard, and usb mouse.\n");
    shell_transcript_append_text("USB keyboard auto-detection: plug in a USB keyboard to type commands; on-screen keyboard hides automatically.\n");
    shell_transcript_append_text("Enter runs commands from the prompt line; history stays locked above.\n");

    c6ota_init();
    c6ota_register_progress_callback(shell_c6ota_progress_callback);

    networking_init(&(networking_host_ops_t){
        .transcript_append_text = shell_transcript_append_text,
        .schedule_transcript_append_text = shell_networking_schedule_text,
        .transcript_append_ansi = shell_transcript_append_ansi,
        .record_error = shell_networking_record_error,
        .record_warning = shell_networking_record_warning,
        .record_info = shell_networking_record_info,
        .notify_header = shell_header_notify,
    });

    usb_init();

    /* Route USB keyboard keystrokes into the shell input line. */
    usb_register_keyboard_input_callback(shell_usb_keyboard_cb);

    /* Timezone setup only. The SNTP client starts when Wi-Fi connects,
     * because it requires the lwIP TCP/IP thread to be running. */
    time_init();

    /* DOS-style boot scripting: ensure CONFIG.SYS / AUTOEXEC.BAT exist
     * (generating defaults once), apply CONFIG.SYS directives, then run
     * AUTOEXEC.BAT through the batch pipeline. Safe with no SD card. */
    boot_run_startup();

    /* Start the periodic header status refresh. main only feeds passive
     * header updates; the header module owns all rendering. */
    bsp_display_lock(0);
    shell_header_status_refresh();
    if (s_header_status_timer == NULL) {
        s_header_status_timer = lv_timer_create(shell_header_status_timer_cb,
                                                SHELL_HEADER_REFRESH_PERIOD_MS,
                                                NULL);
    }
    bsp_display_unlock();
}
