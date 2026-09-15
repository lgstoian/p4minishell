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
#include "editor_view.h"
#include "networking.h"
#include "led.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "audio.h"
#include "shell.h"
#include "boot.h"
#include "storage.h"
#include "usb.h"
#include "windows.h"
#include "ui_test.h"

/* Backward-compatibility aliases */
#define SHELL_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_BOOT_MESSAGE              P4_CONFIG_BOOT_MESSAGE
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES

/* Periodic timer that drives the header status panel */
static lv_timer_t *s_header_status_timer;

/* Input-row Stop visibility polls the worker busy state (LVGL task). */
#define SHELL_STOP_POLL_MS 150
static lv_timer_t *s_stop_poll_timer;

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

/* Severity-aware variant: @p level matches header_notify_level_t
 * (0 = info, 1 = warn, 2 = error). */
void c6ota_host_notify_header_level(const char *text, uint32_t timeout_ms, int level)
{
    shell_header_notify_level(text, timeout_ms, (header_notify_level_t)level);
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

/* Runs the transcript scroll on the LVGL task. user_data carries the signed
 * pixel delta as an intptr_t. */
static void shell_mouse_scroll_cb(void *user_data)
{
    if (editor_view_is_open()) {
        editor_view_scroll_by((int32_t)(intptr_t)user_data);
        return;
    }
    windows_scroll_transcript_by((int32_t)(intptr_t)user_data);
}

void usb_host_scroll_transcript(int32_t pixels)
{
    /* The USB module task is not the LVGL task; queue the scroll onto the LVGL
     * task exactly like the USB keyboard injection path does. */
    lv_async_call(shell_mouse_scroll_cb, (void *)(intptr_t)pixels);
    /* A USB mouse scroll is user activity for the power idle clock. */
    shell_power_notify_activity();
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
     * text and the reset lands before that newline would matter. Use
     * ansi_format so the @-specifiers become real ESC sequences. */
    ansi_format(line, sizeof(line), "%s%s" SH_RST, colour, msg);

    if (percent == -1) {
        shell_transcript_append_ansi(line);
        return;
    }

    shell_schedule_transcript_appendf_ansi("%s", line);
}

/** Adapter matching usb_keyboard_input_cb_t for the shell CLI injection path. */
static void shell_usb_keyboard_cb(uint8_t key_code, uint8_t modifiers, usb_key_event_t event)
{
    shell_usb_keyboard_input(key_code, modifiers, (event == USB_KEY_EVENT_PRESS));
    /* USB typing is user activity: reset the power idle clock and wake the
     * display if the idle timer had switched it off. */
    shell_power_notify_activity();
}

/** LVGL timer callback driving the adaptive header status refresh. */
static void shell_header_status_timer_cb(lv_timer_t *timer)
{
    uint32_t next_ms;

    shell_header_status_refresh();
    shell_power_idle_tick();

    /* Reschedule from the current situation (idle-off wake, busy OTA/job,
     * Wi-Fi bring-up, startup, clock minute boundary, idle-off deadline). */
    next_ms = shell_header_refresh_interval_ms();
    lv_timer_set_period(timer, next_ms);
    lv_timer_reset(timer);
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

/* Input-row Tab button: complete the current input-line word by touch, using
 * the same provider as the USB Tab key. The event runs on the LVGL task, so
 * the completion call is direct. */
static void shell_tab_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }
    if (windows_editor_mode_active()) {
        return;
    }
    shell_input_line_tab_complete();
}

/* Input-row Stop button: the touch foreground break. Requests an abort when
 * a command runs (the batch/delay checkpoints unwind with `^C`); idle taps
 * are impossible because the button only shows while busy. LVGL task. */
static void shell_stop_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }
    if (windows_editor_mode_active()) {
        return;
    }
    if (shell_is_command_busy() && !shell_key_wait_is_active()) {
        shell_request_abort();
    }
}

/* Stop-button visibility follows the worker busy state (editor and other
 * input-row modes suppress it inside windows_set_stop_visible). */
static void shell_stop_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    windows_set_stop_visible(shell_is_command_busy());
}

/* Input-row transcript scroll buttons: page the transcript up and down on
 * touch. The event runs on the LVGL task, so the scroll calls are direct. */
static void shell_scroll_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *up_btn = windows_get_scroll_up_button();
    lv_obj_t *down_btn = windows_get_scroll_down_button();

    if (up_btn != NULL && lv_event_get_target(event) == up_btn) {
        windows_scroll_transcript_by(-P4_CONFIG_TRANSCRIPT_SCROLL_STEP);
    } else if (down_btn != NULL && lv_event_get_target(event) == down_btn) {
        windows_scroll_transcript_by(P4_CONFIG_TRANSCRIPT_SCROLL_STEP);
    }
}

/* Tapping the transcript moves focus and the keyboard binding back to the
 * input line so typing always goes to the prompt. */
static void shell_transcript_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    /* While the modal editor owns the transcript surface, the shell-level
     * transcript behaviour (keyboard re-binding, tap-to-show) is disabled so
     * the editor's own touch handling is never overridden. */
    if (windows_editor_mode_active()) {
        return;
    }

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

        /* Tapping the transcript summons the on-screen keyboard when it is
         * hidden and no USB keyboard is driving the input line. This keeps
         * the OSK recoverable on a touch device after it was auto-hidden or
         * dismissed, without disturbing scrolling (which only happens while
         * the keyboard is already visible). */
        if (!keyboard_is_visible() && !keyboard_is_external_input_enabled()) {
            keyboard_show();
        }
    }
}

static void shell_input_line_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    /* The input line is hidden while the modal editor owns the OSK; nothing
     * shell-level should react to it. */
    if (windows_editor_mode_active()) {
        return;
    }

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
            /* Command-sized buffer is heap-allocated: this callback runs on
             * the LVGL task, whose stack is far smaller than 4096 bytes. */
            char *typed = malloc(SHELL_COMMAND_BYTES);

            if (typed != NULL) {
                shell_extract_input_text(typed, SHELL_COMMAND_BYTES);
                if (typed[0] != '\0') {
                    /* First codepoint as one queue item (symbols answer
                     * key waits unfragmented); raw first byte on invalid
                     * bytes (legacy behavior). */
                    uint32_t cp;
                    size_t seq_len;

                    if (shell_utf8_decode(typed, strlen(typed), &cp, &seq_len)) {
                        shell_key_wait_submit_utf8(typed, seq_len);
                    } else {
                        shell_key_wait_submit(typed[0]);
                    }
                }
                free(typed);
            }
            shell_input_line_reset();
            return;
        }

        /* Guard against backspace deleting into the prompt prefix. */
        shell_input_line_repair_prompt(text);
        shell_input_line_ghost_refresh();
        return;
    }

    if (code == LV_EVENT_READY) {
        /* Command-sized buffers are heap-allocated: this callback runs on the
         * LVGL task, whose stack is far smaller than two 4096-byte locals. */
        char *command = malloc(SHELL_COMMAND_BYTES);
        char *transcript_command = malloc(SHELL_COMMAND_BYTES);

        if (command == NULL || transcript_command == NULL) {
            free(command);
            free(transcript_command);
            return;
        }

        /* A pending keypress wait swallows the submission: Enter answers the
         * prompt rather than dispatching a command. */
        if (shell_key_wait_is_active()) {
            shell_key_wait_submit('\r');
            shell_input_line_reset();
            free(command);
            free(transcript_command);
            return;
        }

        /* LV_EVENT_READY is the confirmed submission path, including
         * YES/NO replies to the C6 OTA confirmation prompt. */
        shell_extract_input_text(command, SHELL_COMMAND_BYTES);
        shell_format_command_for_transcript(command, transcript_command, SHELL_COMMAND_BYTES);
        shell_transcript_appendf("%s%s\n", shell_prompt_render_plain(), transcript_command);

        if (shell_command_should_store_history(command)) {
            shell_store_command_history(command);
        }
        shell_reset_history_cursor();
        shell_input_line_reset();
        shell_input_line_ghost_refresh();

        /* Dispatch on the worker task so heavy commands never run on the
         * LVGL event-callback stack. */
        shell_execute_command_async(command);
        free(command);
        free(transcript_command);
        /* Jump to the output of the submitted command even if the user was
         * reading earlier history. */
        shell_force_transcript_scroll_to_end();
        return;
    }

    if (code == LV_EVENT_KEY) {
        uint32_t key = *((const uint32_t *)lv_event_get_param(event));

        if (key == LV_KEY_UP) {
            shell_recall_history(-1);
        } else if (key == LV_KEY_DOWN) {
            shell_recall_history(1);
        }
        shell_input_line_ghost_refresh();
    }
}

/* Route an on-screen keyboard button into the shell's command input line.
 * Runs on the LVGL task from the keyboard event callback. */
static void shell_osk_into_input(const char *txt)
{
    lv_obj_t *input_line = windows_get_input_line();

    if (input_line == NULL || txt == NULL) {
        return;
    }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(input_line);
    } else if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0 || strcmp(txt, "Enter") == 0) {
        lv_obj_send_event(input_line, LV_EVENT_READY, NULL);
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        lv_obj_send_event(input_line, LV_EVENT_READY, NULL);
    } else if (strcmp(txt, LV_SYMBOL_LEFT) == 0) {
        lv_textarea_cursor_left(input_line);
    } else if (strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
        lv_textarea_cursor_right(input_line);
    } else if (strcmp(txt, "+/-") == 0) {
        /* Numeric-mode sign toggle. */
        const char *ta = lv_textarea_get_text(input_line);
        if (ta != NULL && ta[0] == '-') {
            lv_textarea_set_cursor_pos(input_line, 0);
            lv_textarea_delete_char(input_line);
            lv_textarea_add_char(input_line, '+');
        } else {
            lv_textarea_set_cursor_pos(input_line, 0);
            lv_textarea_add_char(input_line, '-');
        }
    } else if (strcmp(txt, LV_SYMBOL_CLOSE) == 0 ||
               strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        keyboard_hide();
    } else {
        /* Any single-codepoint label is typed as-is (UTF-8 symbols included:
         * decode the first codepoint and require it to span the whole label
         * so multi-character action labels like "Tab" never leak a letter).
         * The input line renders them via the chained UI font. */
        uint32_t cp;
        size_t seq_len;

        if (shell_utf8_decode(txt, strlen(txt), &cp, &seq_len) &&
            txt[seq_len] == '\0') {
            lv_textarea_add_char(input_line, cp);
        }
    }
}

/* Logs on-screen keyboard mode changes and special button presses, and routes
 * every button to the shell input line or the modal editor. This callback is
 * the keyboard's only LV_EVENT_VALUE_CHANGED handler (keyboard_register_event_callback
 * replaces LVGL's default handler), so mode switching and typing are owned
 * here. */
static void shell_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *target = lv_event_get_current_target(event);
    uint32_t btn_id = lv_buttonmatrix_get_selected_button(target);

    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }

    /* Deduplicate OSK button events: a re-fire of the same button within the
     * debounce window is the same LVGL event reaching a second handler.
     * Dropping it here makes double input impossible even if a stray handler
     * is ever (re)registered. */
    if (!keyboard_osk_accept(btn_id)) {
        return;
    }

    const char *txt = lv_buttonmatrix_get_button_text(target, btn_id);
    if (txt == NULL) {
        return;
    }

    ESP_LOGI(SHELL_TAG, "Keyboard button: %s (id=%" PRIu32 ")", txt, btn_id);

    /* Mode-switch buttons are handled here (the LVGL default handler was
     * replaced by this callback): abc / ABC / 1# cycle the text maps, and
     * Nav enters the editor navigation page (only while the editor is open,
     * so the label never reaches the shell input line). */
    if (strcmp(txt, "abc") == 0) {
        keyboard_set_mode(KEYBOARD_MODE_TEXT_LOWER);
        return;
    }
    if (strcmp(txt, "ABC") == 0) {
        keyboard_set_mode(KEYBOARD_MODE_TEXT_UPPER);
        return;
    }
    if (strcmp(txt, "1#") == 0) {
        keyboard_set_mode(KEYBOARD_MODE_SYMBOLS);
        return;
    }
    if (strcmp(txt, "Nav") == 0) {
        if (editor_view_is_open()) {
            keyboard_set_mode(KEYBOARD_MODE_NAV);
        }
        return;
    }
    if (strcmp(txt, "Nav1") == 0) {
        if (editor_view_is_open()) {
            keyboard_set_mode(KEYBOARD_MODE_NAV);
        }
        return;
    }
    if (strcmp(txt, "Nav2") == 0 || strcmp(txt, "Edit") == 0) {
        if (editor_view_is_open()) {
            keyboard_set_mode(KEYBOARD_MODE_NAV2);
        }
        return;
    }

    /* While the modal editor is open every OSK button drives the editor. */
    if (editor_view_is_open()) {
        editor_view_handle_osk(txt);
        return;
    }

    shell_osk_into_input(txt);
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

    /* A rotation change tears the whole LVGL surface down. If the modal
     * editor is open its widgets would be destroyed underneath it, so close
     * the view first; editor_view_close() wakes the worker session so it
     * releases the document cleanly. */
    if (editor_view_is_open()) {
        editor_view_close();
    }

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

    lv_obj_t *scroll_up_btn = windows_get_scroll_up_button();
    if (scroll_up_btn != NULL) {
        lv_obj_add_event_cb(scroll_up_btn, shell_scroll_button_event_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *scroll_down_btn = windows_get_scroll_down_button();
    if (scroll_down_btn != NULL) {
        lv_obj_add_event_cb(scroll_down_btn, shell_scroll_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *tab_btn = windows_get_tab_button();
    if (tab_btn != NULL) {
        lv_obj_add_event_cb(tab_btn, shell_tab_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *stop_btn = windows_get_stop_button();
    if (stop_btn != NULL) {
        lv_obj_add_event_cb(stop_btn, shell_stop_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Keyboard callback receives LV_EVENT_VALUE_CHANGED for mode and button
     * presses, LV_EVENT_READY for OK, and LV_EVENT_CANCEL for hide. */
    keyboard_register_event_callback(shell_keyboard_event_cb, NULL);

    /* Synthetic touch indev for `ui` automation/tests (idempotent). */
    ui_test_init();

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

    /* Register the native apps (the applib ABI sample) as shell commands. */
    native_apps_register();

    /* When the SD card first mounts (startup, or a card inserted later and
     * mounted by the first SD command), generate the default boot files and
     * print a short "SD card ready" welcome. */
    storage_register_sd_first_mount_callback(boot_on_sd_first_mount);

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

    /* RGB status LED (WS2812 on GPIO26). Initialised before boot scripting so
     * a CONFIG.SYS `RGB=` directive can drive it during startup. */
    led_init();

    /* Pre-warm the speaker codec path while DMA-capable heap is still free.
     * The codec/I2S init needs contiguous DMA blocks, which lose the race
     * once Wi-Fi, USB and SD all initialize at boot (AUTOEXEC's `volume`
     * would otherwise fail once and spam the boot log; a later manual
     * `volume` always worked). Failures here are harmless: speaker init
     * stays lazy and retries on next audio use. The I2S driver also logs
     * two benign "dma frame num adjusted" notices on every successful init;
     * keep that tag at error level so the boot log stays clean while real
     * failures (and our own command-layer reports) remain visible. */
    esp_log_level_set("i2s_common", ESP_LOG_ERROR);
    (void)audio_init();

    /* Serialize the shared SDMMC host bring-up (N1). The SD card (slot 0) and
     * the ESP-Hosted C6 transport (slot 1) both initialize the SDMMC host at
     * boot from different tasks; sdmmc_host_init() is not thread-safe, so a
     * concurrent double-call corrupts the host and fails both slots. Pre-initing
     * the host here makes every later call hit the driver's idempotent-skip
     * branch and eliminates the race. Must run before networking_init() (which
     * spawns the wifi_bg task) and before any SD access. */
    storage_sdmmc_host_preinit();

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

    /* The SD is mounted and no boot session is held here, so this is the
     * reliable point to apply the saved UI choices (theme / header mode /
     * fonts). The first-mount callback can fire before the VFS is usable. */
    (void)font_restore_saved();

    /* Boot confirmation light: a short green flash once the shell is ready. */
    led_notify(LED_EVENT_BOOT_OK);

    /* Start the periodic header status refresh. main only feeds passive
     * header updates; the header module owns all rendering. */
    bsp_display_lock(0);
    shell_header_status_refresh();
    if (s_header_status_timer == NULL) {
        s_header_status_timer = lv_timer_create(shell_header_status_timer_cb,
                                                shell_header_refresh_interval_ms(),
                                                NULL);
    }
    /* Stop-button visibility poll (created once; the button itself is
     * rebuilt with the input row on rotation). */
    if (s_stop_poll_timer == NULL) {
        s_stop_poll_timer = lv_timer_create(shell_stop_poll_timer_cb,
                                            SHELL_STOP_POLL_MS, NULL);
    }
    bsp_display_unlock();
}
