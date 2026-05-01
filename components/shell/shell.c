/**
 * @file shell.c
 * @brief Shell core implementation for P4MiniShell.
 *
 * Owns transcript buffers, command history, debug log, UART console bridge,
 * system info commands, and shell utility functions. Provides the public API
 * that command.c and main.c use for all shell output and state management.
 */

#include "shell.h"
#include "ansi.h"
#include "display.h"
#include "header.h"
#include "keyboard.h"
#include "windows.h"
#include "clock.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "networking.h"
#include "c6ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

/* Backward-compatibility aliases */
#define SHELL_TAG                       P4_CONFIG_SHELL_TAG
#define SHELL_BOARD_REQUESTED           P4_CONFIG_BOARD_REQUESTED
#define SHELL_BOARD_DETECTED            P4_CONFIG_BOARD_DETECTED
#define SHELL_BOOT_MESSAGE              P4_CONFIG_BOOT_MESSAGE
#define SHELL_PROMPT                    P4_CONFIG_SHELL_PROMPT
#define SHELL_TRANSCRIPT_BYTES          P4_CONFIG_TRANSCRIPT_BYTES
#define SHELL_ASYNC_TRANSCRIPT_BYTES    P4_CONFIG_ASYNC_TRANSCRIPT_BYTES
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_HISTORY_DEPTH     P4_CONFIG_COMMAND_HISTORY_DEPTH
#define SHELL_HEADER_REFRESH_PERIOD_MS  P4_CONFIG_HEADER_REFRESH_PERIOD_MS
#define SHELL_DEBUG_LOG_DEPTH           P4_CONFIG_DEBUG_LOG_DEPTH
#define SHELL_DEBUG_ENTRY_BYTES         P4_CONFIG_DEBUG_ENTRY_BYTES
#define SHELL_WIFI_SSID_BYTES           P4_CONFIG_WIFI_SSID_BYTES
#define SHELL_WIFI_PASSWORD_BYTES       P4_CONFIG_WIFI_PASSWORD_BYTES
#define SHELL_UART_CONSOLE_TASK_STACK_BYTES P4_CONFIG_UART_CONSOLE_TASK_STACK

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Transcript */
static char s_transcript[SHELL_TRANSCRIPT_BYTES];
static char s_async_transcript[SHELL_ASYNC_TRANSCRIPT_BYTES];
static size_t s_async_transcript_len;
static bool s_async_transcript_flush_queued;
static portMUX_TYPE s_async_transcript_lock = portMUX_INITIALIZER_UNLOCKED;

/* Command history */
static char s_command_history[SHELL_COMMAND_HISTORY_DEPTH][SHELL_COMMAND_BYTES];
static size_t s_command_history_count;
static int s_command_history_cursor = -1;
static char s_history_draft[SHELL_COMMAND_BYTES];

/* Debug log */
typedef struct {
    char entries[SHELL_DEBUG_LOG_DEPTH][SHELL_DEBUG_ENTRY_BYTES];
    size_t count;
    size_t next_index;
} shell_debug_log_t;

static shell_debug_log_t s_debug_log;
static size_t s_runtime_warning_count;

/* UART console */
static SemaphoreHandle_t s_uart_console_lock;

/* Boot timestamp */
static int64_t s_boot_timestamp_us;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void shell_async_transcript_flush_cb(void *user_data);
static void shell_uart_console_task(void *arg);

/* ========================================================================
 * TRANSCRIPT MANAGEMENT
 * ======================================================================== */

void shell_transcript_append_text(const char *text)
{
    lv_obj_t *transcript;
    size_t text_len;
    size_t current_len;
    size_t available;
    size_t to_copy;

    if (text == NULL) {
        return;
    }

    transcript = windows_get_transcript();
    if (transcript == NULL) {
        return;
    }

    text_len = strlen(text);
    if (text_len == 0) {
        return;
    }

    current_len = strlen(s_transcript);
    if (current_len >= SHELL_TRANSCRIPT_BYTES - 1) {
        return;
    }

    available = SHELL_TRANSCRIPT_BYTES - 1 - current_len;
    to_copy = text_len < available ? text_len : available;

    memcpy(s_transcript + current_len, text, to_copy);
    s_transcript[current_len + to_copy] = '\0';

    lv_textarea_set_text(transcript, s_transcript);
    lv_textarea_set_cursor_pos(transcript, LV_TEXTAREA_CURSOR_LAST);
}

void shell_transcript_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;

    if (format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    shell_transcript_append_text(buffer);
}

void shell_transcript_append_ansi(const char *text)
{
    char plain[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (text == NULL) {
        return;
    }

    /* Strip ANSI escape sequences for the transcript textarea.
     * LVGL textareas don't support per-character coloring, so we
     * deliver plain text to the transcript. The UART console path
     * passes ANSI codes through natively for real terminals. */
    ansi_strip_to_plain(plain, sizeof(plain), text);
    shell_transcript_append_text(plain);

    /* Also write the raw ANSI text to the UART console for
     * terminals that support ANSI rendering. */
    shell_uart_console_write_text(text);
}

void shell_transcript_appendf_ansi(const char *format, ...)
{
    char buffer[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (format == NULL) {
        return;
    }

    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);

    shell_transcript_append_ansi(buffer);
}

void shell_schedule_transcript_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;
    size_t len;
    size_t available;

    if (format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    len = strlen(buffer);
    if (len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_async_transcript_lock);
    available = SHELL_ASYNC_TRANSCRIPT_BYTES - 1 - s_async_transcript_len;
    if (len > available) {
        len = available;
    }
    if (len > 0) {
        memcpy(s_async_transcript + s_async_transcript_len, buffer, len);
        s_async_transcript_len += len;
        s_async_transcript[s_async_transcript_len] = '\0';
    }

    if (!s_async_transcript_flush_queued && s_async_transcript_len > 0) {
        s_async_transcript_flush_queued = true;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        lv_async_call(shell_async_transcript_flush_cb, NULL);
    } else {
        portEXIT_CRITICAL(&s_async_transcript_lock);
    }
}

static void shell_async_transcript_flush_cb(void *user_data)
{
    (void)user_data;

    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len > 0) {
        shell_transcript_append_text(s_async_transcript);
        s_async_transcript_len = 0;
        s_async_transcript[0] = '\0';
    }
    s_async_transcript_flush_queued = false;
    portEXIT_CRITICAL(&s_async_transcript_lock);

    shell_history_transcript_scroll_to_end();
}

void shell_transcript_reset(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    s_transcript[0] = '\0';

    if (transcript != NULL) {
        lv_textarea_set_text(transcript, "");
    }
}

void shell_history_transcript_scroll_to_end(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    if (transcript == NULL) {
        return;
    }

    lv_obj_update_layout(transcript);
    lv_obj_scroll_to_y(transcript, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ========================================================================
 * COMMAND HISTORY
 * ======================================================================== */

void shell_store_command_history(const char *command)
{
    size_t len;

    if (command == NULL) {
        return;
    }

    len = strlen(command);
    if (len == 0 || len >= SHELL_COMMAND_BYTES) {
        return;
    }

    /* Mask wifi connect passwords */
    if (strncmp(command, "wifi connect ", 13) == 0) {
        const char *ssid_start = command + 13;
        const char *space = strchr(ssid_start, ' ');
        if (space != NULL) {
            size_t ssid_len = (size_t)(space - ssid_start);
            if (ssid_len < SHELL_COMMAND_BYTES - 20) {
                snprintf(s_command_history[s_command_history_count % SHELL_COMMAND_HISTORY_DEPTH],
                         SHELL_COMMAND_BYTES, "wifi connect %.*s ****", (int)ssid_len, ssid_start);
            }
        } else {
            snprintf(s_command_history[s_command_history_count % SHELL_COMMAND_HISTORY_DEPTH],
                     SHELL_COMMAND_BYTES, "%s", command);
        }
    } else {
        snprintf(s_command_history[s_command_history_count % SHELL_COMMAND_HISTORY_DEPTH],
                 SHELL_COMMAND_BYTES, "%s", command);
    }

    s_command_history_count++;
    s_command_history_cursor = -1;
    s_history_draft[0] = '\0';
}

void shell_recall_history(int direction)
{
    lv_obj_t *input_line = windows_get_input_line();
    size_t index;
    const char *entry;

    if (input_line == NULL) {
        return;
    }

    if (s_command_history_count == 0) {
        return;
    }

    if (direction < 0) {
        /* Recall older */
        if (s_command_history_cursor < 0) {
            /* Save current draft */
            const char *text = lv_textarea_get_text(input_line);
            if (text != NULL) {
                snprintf(s_history_draft, sizeof(s_history_draft), "%s", text);
            }
            s_command_history_cursor = (int)(s_command_history_count - 1);
        } else if (s_command_history_cursor > 0) {
            s_command_history_cursor--;
        }
    } else {
        /* Recall newer */
        if (s_command_history_cursor >= 0) {
            s_command_history_cursor++;
            if ((size_t)s_command_history_cursor >= s_command_history_count) {
                s_command_history_cursor = -1;
            }
        }
    }

    if (s_command_history_cursor >= 0) {
        index = (size_t)s_command_history_cursor % SHELL_COMMAND_HISTORY_DEPTH;
        entry = s_command_history[index];
    } else {
        entry = s_history_draft;
    }

    if (entry != NULL && entry[0] != '\0') {
        lv_textarea_set_text(input_line, entry);
        lv_textarea_set_cursor_pos(input_line, LV_TEXTAREA_CURSOR_LAST);
    }
}

const char *shell_get_history_draft(void)
{
    return s_history_draft;
}

/* ========================================================================
 * DEBUG LOG
 * ======================================================================== */

void shell_debug_log_push(const char *tag, const char *message)
{
    size_t index;

    if (tag == NULL || message == NULL) {
        return;
    }

    index = s_debug_log.next_index;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(s_debug_log.entries[index], SHELL_DEBUG_ENTRY_BYTES,
             "[%s] %s", tag, message);
#pragma GCC diagnostic pop

    s_debug_log.next_index = (s_debug_log.next_index + 1) % SHELL_DEBUG_LOG_DEPTH;
    if (s_debug_log.count < SHELL_DEBUG_LOG_DEPTH) {
        s_debug_log.count++;
    }
}

void shell_record_errorf(const char *tag, int error, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    char full[SHELL_DEBUG_ENTRY_BYTES + 32];
    va_list args;

    if (tag == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    snprintf(full, sizeof(full), "ERROR(%d): %.*s", error,
             (int)(sizeof(full) - 20), message);
    shell_debug_log_push(tag, full);
    ESP_LOGE(tag, "%s (0x%x)", message, error);
}

void shell_record_warningf(const char *tag, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    char full[SHELL_DEBUG_ENTRY_BYTES + 32];
    va_list args;

    if (tag == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    snprintf(full, sizeof(full), "WARN: %.*s",
             (int)(sizeof(full) - 8), message);
    shell_debug_log_push(tag, full);
    s_runtime_warning_count++;
    ESP_LOGW(tag, "%s", message);
}

void shell_record_infof(const char *tag, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    if (tag == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    ESP_LOGI(tag, "%s", message);
}

size_t shell_get_warning_count(void)
{
    return s_runtime_warning_count;
}

void shell_command_debug(void)
{
    size_t i;
    size_t start;

    shell_transcript_appendf_ansi("@Ydebug.log:@R %u entries, @r%u warnings@R\n",
                             (unsigned int)s_debug_log.count,
                             (unsigned int)s_runtime_warning_count);

    if (s_debug_log.count == 0) {
        shell_transcript_appendf_ansi("@kdebug.log: (empty)@R\n");
        return;
    }

    start = s_debug_log.count >= SHELL_DEBUG_LOG_DEPTH
            ? s_debug_log.next_index
            : 0;

    for (i = 0; i < s_debug_log.count; i++) {
        size_t index = (start + i) % SHELL_DEBUG_LOG_DEPTH;
        const char *entry = s_debug_log.entries[index];
        /* Color errors red, warnings yellow */
        if (strstr(entry, "ERROR") != NULL) {
            shell_transcript_appendf_ansi("  @k[%u]@R @r%s@R\n", (unsigned int)i, entry);
        } else if (strstr(entry, "WARN") != NULL) {
            shell_transcript_appendf_ansi("  @k[%u]@R @y%s@R\n", (unsigned int)i, entry);
        } else {
            shell_transcript_appendf_ansi("  @k[%u]@R %s\n", (unsigned int)i, entry);
        }
    }
}

/* ========================================================================
 * UART CONSOLE BRIDGE
 * ======================================================================== */

static void shell_uart_console_task(void *arg)
{
    char line[SHELL_COMMAND_BYTES];
    int pos = 0;
    int ch;

    (void)arg;

    /* Initialize UART for stdin.
     * The esp_vfs_dev_uart_* API is deprecated in ESP-IDF v5.5.3 in favor of
     * uart_vfs_dev_* replacements which are not yet available. Suppress the
     * deprecation warnings since we must use the only available API. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);
#pragma GCC diagnostic pop

    /* Print initial prompt */
    shell_uart_console_write_text("\nUART console ready. Type help for commands.\n");
    shell_uart_console_print_prompt();

    while (1) {
        ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            line[pos] = '\0';
            shell_uart_console_write_text("\n");

            if (pos > 0) {
                shell_uart_console_submit_command(line);
            }

            pos = 0;
            shell_uart_console_print_prompt();
        } else if (ch == '\b' || ch == 0x7f) {
            if (pos > 0) {
                pos--;
                shell_uart_console_write_text("\b \b");
            }
        } else if (pos < (int)(SHELL_COMMAND_BYTES - 1) && ch >= 32) {
            line[pos++] = (char)ch;
            putchar(ch);
        }
    }
}

void shell_uart_console_start(void)
{
    xTaskCreate(shell_uart_console_task,
                "shell_uart",
                SHELL_UART_CONSOLE_TASK_STACK_BYTES,
                NULL,
                tskIDLE_PRIORITY + 1,
                NULL);
}

void shell_uart_console_write_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    if (s_uart_console_lock != NULL) {
        xSemaphoreTake(s_uart_console_lock, portMAX_DELAY);
    }

    printf("%s", text);

    if (s_uart_console_lock != NULL) {
        xSemaphoreGive(s_uart_console_lock);
    }
}

void shell_uart_console_print_prompt(void)
{
    /* PowerShell-style colored prompt: "PS " (bright white) + path (bright yellow) + "> " (bright white)
     * On the UART console this renders with ANSI SGR codes for a native PowerShell look.
     * The LVGL input line uses a plain-text prompt set by shell_input_line_set_text(). */
    char prompt_buf[P4_CONFIG_ANSI_BUFFER_BYTES];
    extern const char *shell_get_cwd_for_prompt(void);
    const char *cwd = shell_get_cwd_for_prompt();

    /* Build colored prompt: \e[97mPS \e[93m<path>\e[97m> \e[0m */
    int len = snprintf(prompt_buf, sizeof(prompt_buf),
                       "\x1B[%dm" P4_CONFIG_PS_PREFIX "\x1B[%dm%s\x1B[%dm" P4_CONFIG_PS_SUFFIX "\x1B[0m",
                       P4_CONFIG_PS_COLOR_PREFIX,
                       P4_CONFIG_PS_COLOR_PATH, cwd ? cwd : P4_CONFIG_PS_PATH_SEPARATOR,
                       P4_CONFIG_PS_COLOR_SUFFIX);
    if (len > 0 && (size_t)len < sizeof(prompt_buf)) {
        shell_uart_console_write_text(prompt_buf);
    } else {
        shell_uart_console_write_text(P4_CONFIG_SHELL_PROMPT);
    }
}

void shell_uart_console_submit_command(const char *command)
{
    /* Forward declaration — implemented in command.c */
    extern void shell_execute_command(char *command);

    char cmd_copy[SHELL_COMMAND_BYTES];

    if (command == NULL || strlen(command) == 0) {
        return;
    }

    snprintf(cmd_copy, sizeof(cmd_copy), "%s", command);
    shell_execute_command(cmd_copy);
}

/* ========================================================================
 * POWERSHELL-STYLE PROMPT PATH HELPER
 * ========================================================================
 * Returns the current working directory formatted for the prompt.
 * Truncates long paths with "..." prefix to keep the prompt compact.
 * The path is stored in a static buffer (not thread-safe — single UART task).
 */

const char *shell_get_cwd_for_prompt(void)
{
    static char path_buf[P4_CONFIG_PS_PATH_MAX_DISPLAY + 8];
    extern const char *shell_get_cwd(void);  /* defined in main.c */
    const char *cwd = shell_get_cwd();

    if (cwd == NULL || cwd[0] == '\0') {
        return P4_CONFIG_PS_PATH_SEPARATOR;
    }

    size_t len = strlen(cwd);
    if (len <= P4_CONFIG_PS_PATH_MAX_DISPLAY) {
        return cwd;
    }

    /* Truncate: show "..." + last portion */
    const char *last_sep = strrchr(cwd, '\\');
    if (last_sep == NULL) last_sep = strrchr(cwd, '/');
    if (last_sep == NULL) {
        /* No separator found — truncate from beginning */
        size_t keep = P4_CONFIG_PS_PATH_MAX_DISPLAY - 3;
        if (keep > len) keep = len;
        snprintf(path_buf, sizeof(path_buf), "...%s", cwd + len - keep);
        return path_buf;
    }

    size_t suffix_len = strlen(last_sep);
    if (suffix_len + 3 > P4_CONFIG_PS_PATH_MAX_DISPLAY) {
        /* Even the suffix alone is too long */
        size_t keep = P4_CONFIG_PS_PATH_MAX_DISPLAY - 3;
        if (keep > suffix_len) keep = suffix_len;
        snprintf(path_buf, sizeof(path_buf), "...%s", last_sep + suffix_len - keep);
    } else {
        snprintf(path_buf, sizeof(path_buf), "...%s", last_sep);
    }
    return path_buf;
}

/* ========================================================================
 * USB KEYBOARD INPUT BRIDGE
 * ======================================================================== */

/**
 * Inject a USB keyboard event into the shell CLI input line.
 * This function is called from the USB module task context via the
 * registered input callback. It dispatches an LVGL async call to
 * safely manipulate the input line textarea from the LVGL task.
 */

/* Per-task context for async USB keyboard injection */
typedef struct {
    uint8_t key_code;
    uint8_t modifiers;
} usb_key_inject_ctx_t;

static void shell_usb_keyboard_inject_cb(void *user_data)
{
    usb_key_inject_ctx_t *ctx = (usb_key_inject_ctx_t *)user_data;
    lv_obj_t *input_line;

    if (ctx == NULL) {
        return;
    }

    input_line = windows_get_input_line();
    if (input_line == NULL) {
        free(ctx);
        return;
    }

    /* Forward declaration for usb_key_to_ascii_full */
    extern bool usb_key_to_ascii_full(uint8_t key_code, uint8_t modifiers, char *out);

    {
        char ch = '\0';

        if (usb_key_to_ascii_full(ctx->key_code, ctx->modifiers, &ch)) {
            if (ch == '\n' || ch == '\r') {
                /* Enter key: submit the command */
                lv_obj_send_event(input_line, LV_EVENT_READY, NULL);
            } else if (ch == '\b') {
                /* Backspace: delete last character */
                lv_textarea_delete_char(input_line);
            } else if (ch == 0x1B) {
                /* ESC: clear the input line */
                lv_textarea_set_text(input_line, "");
            } else if (ch == '\t') {
                /* Tab: insert spaces */
                lv_textarea_add_text(input_line, "    ");
            } else if (ch >= 0x20 && ch <= 0x7E) {
                /* Printable ASCII character */
                lv_textarea_add_char(input_line, (uint8_t)ch);
            }
        } else {
            /* Non-printable key: handle navigation and editing */
            switch (ctx->key_code) {
            case 0x4F: /* Right arrow: move cursor right */
                lv_textarea_cursor_right(input_line);
                break;
            case 0x50: /* Left arrow: move cursor left */
                lv_textarea_cursor_left(input_line);
                break;
            case 0x51: /* Down arrow: recall older history */
                extern void shell_recall_history(int direction);
                shell_recall_history(-1);
                break;
            case 0x52: /* Up arrow: recall newer history */
                extern void shell_recall_history(int direction);
                shell_recall_history(1);
                break;
            case 0x4C: /* Delete: remove char at cursor */
                lv_textarea_delete_char(input_line);
                break;
            case 0x4A: /* Home: move to beginning */
                {
                    const char *text = lv_textarea_get_text(input_line);
                    if (text != NULL) {
                        size_t len = strlen(text);
                        for (size_t i = 0; i < len; i++) {
                            lv_textarea_cursor_left(input_line);
                        }
                    }
                }
                break;
            case 0x4D: /* End: move to end */
                {
                    const char *text = lv_textarea_get_text(input_line);
                    if (text != NULL) {
                        size_t len = strlen(text);
                        for (size_t i = 0; i < len; i++) {
                            lv_textarea_cursor_right(input_line);
                        }
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    free(ctx);
}

void shell_usb_keyboard_input(uint8_t key_code, uint8_t modifiers, bool pressed)
{
    /* Only process key press events (not releases) for CLI injection */
    if (!pressed) {
        return;
    }

    /* Allocate context for async dispatch */
    usb_key_inject_ctx_t *ctx = calloc(1, sizeof(usb_key_inject_ctx_t));
    if (ctx == NULL) {
        return;
    }

    ctx->key_code = key_code;
    ctx->modifiers = modifiers;

    /* Dispatch to LVGL task via async call */
    if (lv_async_call(shell_usb_keyboard_inject_cb, ctx) != LV_RESULT_OK) {
        free(ctx);
    }
}

/* ========================================================================
 * SYSTEM INFO COMMANDS
 * ======================================================================== */

void shell_command_help(void)
{
    shell_transcript_appendf_ansi("@Y@BP4MiniShell Commands:@R\n");
    shell_transcript_appendf_ansi("  @Ghelp@R | sysinfo | clear/cls | reboot | version/ver | about | debug | mem\n");
    shell_transcript_appendf_ansi("  @Gbrightness@R <0-100> | @Grotate@R <0|90|180|270> | @Gbattery@R | @Gvolume@R <0-100>\n");
    shell_transcript_appendf_ansi("  @Ggpio@R list | status | read <pin> | set <pin> <0|1>\n");
    shell_transcript_appendf_ansi("  @Gcd@R [path] | @Gdir@R [path] | @Gcopy@R <src> <dst> | @Gmove@R <src> <dst>\n");
    shell_transcript_appendf_ansi("  @Gdel@R <path> | @Gren@R <src> <dst> | @Gmd@R <path> | @Grd@R <path>\n");
    shell_transcript_appendf_ansi("  @Gtype@R <path> | @Gwrite@R <path> <text> | @Gappend@R <path> <text> | @Gtouch@R <path>\n");
    shell_transcript_appendf_ansi("  @Gset@R [NAME=VALUE] | @Gpath@R [dirs] | @Gecho@R <text> | @Gecho@R on|off | @Gcall@R <file.bat>\n");
    shell_transcript_appendf_ansi("  @Gsd@R info | ls [path] | stat <path> | cat <path> [bytes]\n");
    shell_transcript_appendf_ansi("  @Gwifi@R status | scan | diag | connect [ssid pass] | disconnect\n");
    shell_transcript_appendf_ansi("  @Gbluetooth@R status | scan | advertise on|off\n");
    shell_transcript_appendf_ansi("  @Gusb@R status | ls [path] | keyboard <on|off> | mouse <on|off>\n");
    shell_transcript_appendf_ansi("  @Gc6ota@R <sd:/path|http[s]://url|default>\n");
    shell_transcript_appendf_ansi("  @Gdisplay@R info | resolution | refresh | power <on|sleep|off>\n");
    shell_transcript_appendf_ansi("  @Gkeyboard@R show | hide | toggle | status\n");
    shell_transcript_appendf_ansi("  @Gwindows@R info\n");
    shell_transcript_appendf_ansi("@kHistory recall:@R Prev/Next buttons above the keyboard\n");
    shell_transcript_appendf_ansi("@kOutput redirection:@R > file (overwrite) | >> file (append)\n");
}

void shell_command_sysinfo(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    uint32_t uptime_sec = time_get_uptime_sec();
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t days = uptime_sec / 86400;
    uint32_t hours = (uptime_sec % 86400) / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    uint32_t secs = uptime_sec % 60;
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? "@r" : "@g";

    shell_transcript_appendf_ansi("@Y@BP4MiniShell System Information:@R\n");
    shell_transcript_appendf_ansi("  @Cboard.requested_name:@R %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf_ansi("  @Cboard.detected_name:@R %s\n", SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  @Cversion:@R %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  @Ctime:@R %s %s%s\n",
                             time_get_formatted(),
                             time_get_timezone(),
                             time_is_synchronized() ? " @g(synced)@R" : " @y(unsynced)@R");

    /* Display info from the display manager */
    {
        display_info_t disp_info = display_get_info();
        shell_transcript_appendf_ansi("  @Cdisplay:@R %" PRId32 " x %" PRId32 " (native %" PRId32 " x %" PRId32
                                 "), %s, reset GPIO %d, backlight GPIO %d\n",
                                 disp_info.resolution.current_width,
                                 disp_info.resolution.current_height,
                                 disp_info.resolution.native_width,
                                 disp_info.resolution.native_height,
                                 disp_info.panel_driver,
                                 BOARD_CFG_LCD_RST_GPIO,
                                 BOARD_CFG_LCD_BACKLIGHT_GPIO);
        shell_transcript_appendf_ansi("  @Cdisplay.state:@R brightness=%d%% rotation=%s power=%s refresh=%" PRIu32 "Hz\n",
                                 disp_info.brightness_percent,
                                 display_rotation_to_string(disp_info.rotation),
                                 disp_info.power_state == DISPLAY_POWER_ON ? "@gon@R" :
                                 disp_info.power_state == DISPLAY_POWER_SLEEP ? "@ysleep@R" : "@roff@R",
                                 disp_info.refresh.current_hz);
    }

    shell_transcript_appendf_ansi("  @Cidf:@R %s\n", esp_get_idf_version());
    shell_transcript_appendf_ansi("  @Cfreertos:@R tasks=%" PRIu32 " uptime=%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             task_count, days, hours, mins, secs);
    shell_transcript_appendf_ansi("  @Cheap:@R free=%u bytes, internal_free=%u bytes, total=%u bytes (%s%u%%%%@R)\n",
                             (unsigned int)free_heap, (unsigned int)free_internal,
                             (unsigned int)total_heap, heap_color, heap_pct);

    networking_append_sysinfo_summary();
}

void shell_command_version(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    uint32_t uptime_sec = time_get_uptime_sec();
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const char *idf_ver = esp_get_idf_version();
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? "@r" : "@g";

    shell_transcript_appendf_ansi("@G%s@R\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf_ansi("  @Cversion:@R %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  @Cidf:@R %s\n", idf_ver != NULL ? idf_ver : "@runknown@R");
    shell_transcript_appendf_ansi("  @Cboard:@R %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  @Cchip:@R %s rev %d, %d cores\n",
                             CONFIG_IDF_TARGET, 1, 2);
    shell_transcript_appendf_ansi("  @Ctime:@R %s %s%s\n",
                             time_get_formatted(),
                             time_get_timezone(),
                             time_is_synchronized() ? " @g(synced)@R" : " @y(unsynced)@R");
    shell_transcript_appendf_ansi("  @Cuptime:@R %" PRIu32 "s\n", uptime_sec);
    shell_transcript_appendf_ansi("  @Cheap:@R %u/%u bytes free (%s%u%%%%@R)\n",
                             (unsigned int)free_heap, (unsigned int)total_heap,
                             heap_color, heap_pct);
    shell_transcript_appendf_ansi("  @Ctasks:@R %" PRIu32 "\n", task_count);
}

void shell_command_about(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    int64_t uptime_us = esp_timer_get_time() - s_boot_timestamp_us;
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000ULL);
    const char *idf_ver = esp_get_idf_version();

    shell_transcript_appendf_ansi("@G@BP4MiniShell@R — Embedded DOS-style command shell\n");
    shell_transcript_appendf_ansi("  @Cabout.version:@R %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  @Cabout.board:@R %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  @Cabout.idf:@R %s\n", idf_ver != NULL ? idf_ver : "@runknown@R");
    shell_transcript_appendf_ansi("  @Cabout.display:@R JD9165 1024x600 MIPI-DSI, GT911 touch\n");
    shell_transcript_appendf_ansi("  @Cabout.ui:@R locked transcript with touch keyboard, history buttons, and command prompt\n");
    shell_transcript_appendf_ansi("  @Cabout.uptime:@R %" PRIu32 "s\n", uptime_sec);
    shell_transcript_appendf_ansi("  @Cabout.tasks:@R %" PRIu32 "\n", task_count);
}

void shell_command_mem(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t task_count = uxTaskGetNumberOfTasks();
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? "@r" : "@g";

    shell_transcript_appendf_ansi("  @Cmem.heap:@R free=%u bytes, total=%u bytes (%s%u%%%%@R)\n",
                             (unsigned int)free_heap, (unsigned int)total_heap,
                             heap_color, heap_pct);
    shell_transcript_appendf_ansi("  @Cmem.heap_min:@R %u bytes\n", (unsigned int)min_free);
    shell_transcript_appendf_ansi("  @Cmem.internal:@R %u bytes free\n", (unsigned int)free_internal);
    shell_transcript_appendf_ansi("  @Cmem.tasks:@R %" PRIu32 "\n", task_count);
}

/* ========================================================================
 * HEADER STATUS
 * ======================================================================== */

void shell_header_status_refresh(void)
{
    /* Update header with current system metrics */
    header_update_mem(heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      heap_caps_get_total_size(MALLOC_CAP_8BIT));
    header_update_cpu(0, uxTaskGetNumberOfTasks());

    int64_t uptime_us = esp_timer_get_time() - s_boot_timestamp_us;
    header_update_uptime((uint32_t)(uptime_us / 1000000ULL));

    header_update_status();
}

int64_t shell_get_boot_timestamp_us(void)
{
    return s_boot_timestamp_us;
}

const char *shell_get_time_string(void)
{
    return time_get_formatted();
}

bool shell_time_is_synced(void)
{
    return time_is_synchronized();
}

/* ========================================================================
 * SHELL UTILITIES
 * ======================================================================== */

bool shell_text_equals_ignore_case(const char *left, const char *right)
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

char *shell_trim(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        end--;
    }

    *(end + 1) = '\0';
    return text;
}

int shell_split_args(char *text, char **argv, int max_args)
{
    int argc = 0;
    char *p = text;

    if (text == NULL || argv == NULL || max_args <= 0) {
        return 0;
    }

    while (*p != '\0' && argc < max_args) {
        /* Skip leading whitespace */
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        if (*p == '"') {
            p++;
            argv[argc] = p;
            while (*p != '\0' && *p != '"') {
                p++;
            }
            if (*p == '"') {
                *p = '\0';
                p++;
            }
        } else {
            argv[argc] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
        }

        argc++;
    }

    return argc;
}

bool shell_parse_percentage_arg(const char *text, int *percentage_out)
{
    char *endptr;
    long value;

    if (text == NULL || percentage_out == NULL) {
        return false;
    }

    value = strtol(text, &endptr, 10);
    if (*endptr != '\0' || value < 0 || value > 100) {
        return false;
    }

    *percentage_out = (int)value;
    return true;
}

bool shell_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out)
{
    char *endptr;
    unsigned long value;

    if (text == NULL || value_out == NULL) {
        return false;
    }

    value = strtoul(text, &endptr, 10);
    if (*endptr != '\0' || value < min_value || value > max_value) {
        return false;
    }

    *value_out = (size_t)value;
    return true;
}

void shell_join_args(char **argv, int start_index, int argc, char *output, size_t output_size)
{
    int i;
    size_t pos = 0;

    if (argv == NULL || output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';

    for (i = start_index; i < argc && pos < output_size - 1; i++) {
        if (i > start_index) {
            output[pos++] = ' ';
            output[pos] = '\0';
        }
        pos += snprintf(output + pos, output_size - pos, "%s", argv[i]);
    }
}

void shell_format_size(char *dst, size_t dst_size, size_t bytes)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)bytes;
    size_t unit_index = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    while (value >= 1024.0 && unit_index + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(dst, dst_size, "%zu %s", bytes, units[unit_index]);
    } else {
        snprintf(dst, dst_size, "%.1f %s", value, units[unit_index]);
    }
}

const char *shell_entry_type(const struct stat *st)
{
    if (st == NULL) {
        return "UNKNOWN";
    }
    if (S_ISDIR(st->st_mode)) {
        return "dir";
    }
    if (S_ISREG(st->st_mode)) {
        return "file";
    }
    return "other";
}

void shell_header_notify(const char *text, uint32_t timeout_ms)
{
    header_set_notification(text, timeout_ms);
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void shell_init(void)
{
    if (s_initialized) {
        return;
    }

    /* Initialize ANSI/VT color palette */
    ansi_init();

    /* Capture boot timestamp */
    s_boot_timestamp_us = esp_timer_get_time();

    /* Initialize UART console lock */
    if (s_uart_console_lock == NULL) {
        s_uart_console_lock = xSemaphoreCreateMutex();
    }

    /* Clear state */
    s_transcript[0] = '\0';
    s_async_transcript[0] = '\0';
    s_async_transcript_len = 0;
    s_async_transcript_flush_queued = false;
    s_command_history_count = 0;
    s_command_history_cursor = -1;
    s_history_draft[0] = '\0';
    memset(&s_debug_log, 0, sizeof(s_debug_log));
    s_runtime_warning_count = 0;

    s_initialized = true;

    ESP_LOGI(SHELL_TAG, "Shell module initialized");
}

void shell_set_default_state(void)
{
    /* Set default working directory and PATH */
}

bool shell_is_initialized(void)
{
    return s_initialized;
}
