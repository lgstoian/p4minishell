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
#include "ansi_palette.h"
#include "display.h"
#include "header.h"
#include "keyboard.h"
#include "windows.h"
#include "clock.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <time.h>
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
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
#define SHELL_KEY_QUEUE_DEPTH           P4_CONFIG_KEY_QUEUE_DEPTH
#define SHELL_PROMPT_TEMPLATE_BYTES     P4_CONFIG_PROMPT_TEMPLATE_BYTES
#define SHELL_PROMPT_RENDER_BYTES       (P4_CONFIG_PROMPT_TEMPLATE_BYTES + P4_CONFIG_PS_PATH_MAX_DISPLAY + 64)

/**
 * Body budget for the semantic print helpers.
 *
 * Each helper renders the caller's text into this, then wraps it with a
 * colour prefix, a label, and a reset inside a full P4_CONFIG_ANSI_BUFFER_BYTES
 * line. Reserving 64 bytes of headroom guarantees the wrap always fits.
 */
#define SHELL_SEMANTIC_BODY_BYTES       (P4_CONFIG_ANSI_BUFFER_BYTES - 64)

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
static bool s_uart_console_running;

/* Serializes command submissions coming from the serial console */
static SemaphoreHandle_t s_shell_command_lock;

/* Interactive keypress wait: queue fed by every input source */
static QueueHandle_t s_key_queue;
static volatile bool s_key_wait_active;

/* Runtime prompt template set by the `prompt` command */
static char s_prompt_template[SHELL_PROMPT_TEMPLATE_BYTES] = P4_CONFIG_PROMPT_DEFAULT_TEMPLATE;

/* Boot timestamp */
static int64_t s_boot_timestamp_us;

/* Command module operations, registered by command_init() */
static shell_command_ops_t s_command_ops;

/* ========================================================================
 * COMMAND MODULE HOOKS
 * ======================================================================== */

void shell_register_command_ops(const shell_command_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_command_ops, 0, sizeof(s_command_ops));
        return;
    }

    s_command_ops = *ops;
}

/**
 * Current working directory as seen by the shell core.
 * Falls back to the SD mount point before command_init() registers its ops.
 */
static const char *shell_current_cwd(void)
{
    if (s_command_ops.get_cwd == NULL) {
        return BSP_SD_MOUNT_POINT;
    }

    return s_command_ops.get_cwd();
}

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void shell_async_transcript_flush_cb(void *user_data);
static void shell_uart_console_task(void *arg);
static void shell_prompt_expand(char *output, size_t output_size, bool allow_escape);

/* ========================================================================
 * TRANSCRIPT MANAGEMENT
 * ======================================================================== */

/**
 * Append text to the transcript buffer and repaint the LVGL textarea.
 *
 * @param text           Text to append (already plain, no ANSI sequences).
 * @param mirror_to_uart When true, the same text is echoed to the serial
 *                       console. ANSI callers pass false because they emit the
 *                       raw escape-sequence form to the terminal themselves.
 */
static void shell_transcript_append_internal(const char *text, bool mirror_to_uart)
{
    static const char truncation_marker[] = "\n[history truncated]\n";
    lv_obj_t *transcript;
    size_t current_len;
    size_t text_len;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    transcript = windows_get_transcript();

    /* Once the UI is up, the transcript is LVGL-backed state. Serialize every
     * appender (the LVGL task, UART console task, Wi-Fi background task, and
     * command worker) against the render cycle with the recursive LVGL port
     * lock, so the shared buffer and the textarea are never touched while the
     * LVGL task is mid-render. The mutex is recursive, so call paths that
     * already hold it (LVGL event callbacks, shell_uart_console_submit_command)
     * nest without deadlock. */
    if (transcript != NULL) {
        lvgl_port_lock(0);
    }

    current_len = strlen(s_transcript);
    text_len = strlen(text);

    /* A single append larger than the whole buffer keeps only its tail. */
    if (text_len >= sizeof(s_transcript)) {
        text += text_len - (sizeof(s_transcript) - 1);
        text_len = strlen(text);
        s_transcript[0] = '\0';
        current_len = 0;
    }

    /* Drop the oldest half of the transcript and mark the cut so the user
     * can tell scrollback was discarded rather than silently lost. */
    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        size_t keep = sizeof(s_transcript) / 2;

        if (current_len > keep) {
            memmove(s_transcript, s_transcript + current_len - keep, keep);
            current_len = keep;
            s_transcript[current_len] = '\0';
        }

        if (current_len + sizeof(truncation_marker) < sizeof(s_transcript)) {
            memcpy(s_transcript + current_len, truncation_marker, sizeof(truncation_marker) - 1);
            current_len += sizeof(truncation_marker) - 1;
            s_transcript[current_len] = '\0';
        }
    }

    if (current_len + text_len + 1 >= sizeof(s_transcript)) {
        text_len = sizeof(s_transcript) - current_len - 1;
    }

    memcpy(s_transcript + current_len, text, text_len);
    s_transcript[current_len + text_len] = '\0';

    /* Mirror plain transcript output to the serial console so idf.py monitor
     * stays a first-class shell endpoint. */
    if (mirror_to_uart) {
        shell_uart_console_write_text(text);
    }

    transcript = windows_get_transcript();
    if (transcript != NULL) {
        lv_textarea_set_text(transcript, s_transcript);
        lv_textarea_set_cursor_pos(transcript, LV_TEXTAREA_CURSOR_LAST);
        lvgl_port_unlock();
    }
}

void shell_transcript_append_text(const char *text)
{
    shell_transcript_append_internal(text, true);
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
     * passes ANSI codes through natively for real terminals, so the
     * plain append must not mirror to UART or the line would print twice. */
    ansi_strip_to_plain(plain, sizeof(plain), text);
    shell_transcript_append_internal(plain, false);

    /* Write the raw ANSI text to the UART console for
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

/* ========================================================================
 * SEMANTIC OUTPUT HELPERS
 * ========================================================================
 * These apply the shared palette so no command has to pick colours. Each
 * builds the coloured form and hands it to shell_transcript_append_ansi(),
 * which keeps the LVGL transcript and the UART console in step.
 */

/**
 * Shared body for the single-colour helpers.
 *
 * @param colour   Palette macro for the whole line.
 * @param format   printf-style format, ANSI specifiers already resolved by
 *                 the caller's palette macros.
 * @param newline  When true, a '\n' is appended after the reset sequence.
 */
static void shell_print_coloured(const char *colour, const char *format,
                                 bool newline, va_list args)
{
    /* The body is rendered into a smaller buffer than the composed line so
     * there is guaranteed room for the colour prefix, the reset, and the
     * optional trailing newline. */
    char body[SHELL_SEMANTIC_BODY_BYTES];
    char fmt[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (format == NULL) {
        return;
    }

    /* Render the caller's text with the real printf, so width and precision
     * flags work. Doing it first also means a '%s' value containing '@' can
     * never be mistaken for a colour specifier. */
    vsnprintf(body, sizeof(body), format, args);

    /* The SH_* macros expand to '@'-specifiers, which ansi_vformat (reached
     * through shell_transcript_appendf_ansi) converts to real SGR escapes.
     * The colour and reset must be literal in the format string for that
     * conversion to happen; the rendered body is passed as a %s argument so
     * any literal '%' in it stays data. */
    snprintf(fmt, sizeof(fmt), "%s%%s%s%s", colour, SH_RST, newline ? "\n" : "");
    shell_transcript_appendf_ansi(fmt, body);
}

void shell_print_heading(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_HEAD, format, true, args);
    va_end(args);
}

void shell_print_field(const char *label, const char *format, ...)
{
    char value[SHELL_SEMANTIC_BODY_BYTES];
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (label == NULL) {
        return;
    }

    value[0] = '\0';
    if (format != NULL) {
        va_start(args, format);
        vsnprintf(value, sizeof(value), format, args);
        va_end(args);
    }

    snprintf(line, sizeof(line), SH_LBL "%s" SH_RST " " SH_VAL "%s" SH_RST "\n", label, value);
    shell_transcript_append_ansi(line);
}

void shell_print_field_num(const char *label, long value)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (label == NULL) {
        return;
    }

    snprintf(line, sizeof(line), SH_LBL "%s" SH_RST " " SH_NUM "%ld" SH_RST "\n", label, value);
    shell_transcript_append_ansi(line);
}

void shell_print_ok(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_OK, format, true, args);
    va_end(args);
}

void shell_print_error(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_ERR, format, true, args);
    va_end(args);
}

void shell_print_warning(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_WARN, format, true, args);
    va_end(args);
}

void shell_print_muted(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_MUTE, format, true, args);
    va_end(args);
}

void shell_print_usage(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    shell_print_coloured(SH_USAGE, format, true, args);
    va_end(args);
}

/**
 * Stage text in the async buffer, dropping the oldest bytes when full.
 * Caller must hold s_async_transcript_lock.
 */
static void shell_async_transcript_append_pending(const char *text)
{
    size_t text_len;

    if (text == NULL) {
        return;
    }

    text_len = strlen(text);
    if (text_len == 0) {
        return;
    }

    /* A single message larger than the buffer keeps only its tail. */
    if (text_len >= sizeof(s_async_transcript)) {
        text += text_len - (sizeof(s_async_transcript) - 1);
        text_len = strlen(text);
        s_async_transcript_len = 0;
        s_async_transcript[0] = '\0';
    }

    /* Prefer dropping the oldest staged bytes over dropping the newest
     * message, so the most recent module output always reaches the user. */
    if (s_async_transcript_len + text_len + 1 >= sizeof(s_async_transcript)) {
        size_t drop = s_async_transcript_len + text_len + 1 - sizeof(s_async_transcript);

        if (drop >= s_async_transcript_len) {
            s_async_transcript_len = 0;
            s_async_transcript[0] = '\0';
        } else {
            memmove(s_async_transcript, s_async_transcript + drop, s_async_transcript_len - drop);
            s_async_transcript_len -= drop;
            s_async_transcript[s_async_transcript_len] = '\0';
        }
    }

    if (s_async_transcript_len + text_len + 1 >= sizeof(s_async_transcript)) {
        text_len = sizeof(s_async_transcript) - s_async_transcript_len - 1;
    }

    memcpy(s_async_transcript + s_async_transcript_len, text, text_len);
    s_async_transcript_len += text_len;
    s_async_transcript[s_async_transcript_len] = '\0';
}

void shell_schedule_transcript_appendf(const char *format, ...)
{
    char buffer[512];
    va_list args;
    bool queue_flush = false;

    if (format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    portENTER_CRITICAL(&s_async_transcript_lock);
    shell_async_transcript_append_pending(buffer);
    if (!s_async_transcript_flush_queued && s_async_transcript_len > 0) {
        s_async_transcript_flush_queued = true;
        queue_flush = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (queue_flush && lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
        portENTER_CRITICAL(&s_async_transcript_lock);
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
    }
}

static void shell_async_transcript_flush_cb(void *user_data)
{
    char pending[SHELL_ASYNC_TRANSCRIPT_BYTES];
    bool requeue = false;

    (void)user_data;

    /* Drain the staging buffer under the critical section, then append
     * outside it so LVGL work never runs with interrupts masked. */
    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len == 0) {
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        return;
    }

    memcpy(pending, s_async_transcript, s_async_transcript_len + 1);
    s_async_transcript_len = 0;
    s_async_transcript[0] = '\0';
    s_async_transcript_flush_queued = false;
    portEXIT_CRITICAL(&s_async_transcript_lock);

    shell_transcript_append_text(pending);
    shell_history_transcript_scroll_to_end();

    /* Producers may have staged more text while we were appending. */
    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len > 0 && !s_async_transcript_flush_queued) {
        s_async_transcript_flush_queued = true;
        requeue = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (requeue && lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
        portENTER_CRITICAL(&s_async_transcript_lock);
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
    }
}

void shell_transcript_reset(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    s_transcript[0] = '\0';

    if (transcript != NULL) {
        lvgl_port_lock(0);
        lv_textarea_set_text(transcript, "");
        lvgl_port_unlock();
    }
}

void shell_history_transcript_scroll_to_end(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    if (transcript == NULL) {
        return;
    }

    lvgl_port_lock(0);
    lv_obj_update_layout(transcript);
    lv_obj_scroll_to_y(transcript, LV_COORD_MAX, LV_ANIM_OFF);
    lvgl_port_unlock();
}

size_t shell_transcript_get_length(void)
{
    return strlen(s_transcript);
}

const char *shell_transcript_get_text_from(size_t offset)
{
    size_t length = strlen(s_transcript);

    if (offset > length) {
        return NULL;
    }

    return s_transcript + offset;
}

/* ========================================================================
 * COMMAND HISTORY
 * ======================================================================== */

/**
 * Detect `wifi connect <ssid> <password>`, which must never reach the
 * transcript or the recall buffer in clear text.
 */
static bool shell_command_is_sensitive(const char *command)
{
    char buffer[SHELL_COMMAND_BYTES];
    char *argv[4];
    int argc;

    snprintf(buffer, sizeof(buffer), "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    return argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0;
}

bool shell_command_should_store_history(const char *command)
{
    /* OTA confirmation replies (YES/NO) are not shell commands. */
    if (s_command_ops.c6ota_is_pending != NULL && s_command_ops.c6ota_is_pending()) {
        return false;
    }

    return !shell_command_is_sensitive(command);
}

void shell_format_command_for_transcript(const char *command, char *output, size_t output_size)
{
    char buffer[SHELL_COMMAND_BYTES];
    char *argv[4];
    int argc;

    if (output == NULL || output_size == 0) {
        return;
    }

    snprintf(buffer, sizeof(buffer), "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        snprintf(output, output_size, "wifi connect %s ********", argv[2]);
        return;
    }

    snprintf(output, output_size, "%s", command != NULL ? command : "");
}

void shell_store_command_history(const char *command)
{
    char masked[SHELL_COMMAND_BYTES];
    const char *entry;
    size_t index;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    /* Defence in depth: callers are expected to filter sensitive commands
     * via shell_command_should_store_history(), but mask here as well so no
     * code path can ever persist a Wi-Fi password in the recall buffer. */
    if (shell_command_is_sensitive(command)) {
        shell_format_command_for_transcript(command, masked, sizeof(masked));
        entry = masked;
    } else {
        entry = command;
    }

    /* Skip consecutive duplicates so repeated Enter presses do not flood
     * the 10-entry recall buffer. */
    if (s_command_history_count > 0 &&
        strcmp(s_command_history[s_command_history_count - 1], entry) == 0) {
        return;
    }

    /* Buffer full: drop the oldest entry and slide the rest down. memmove is
     * required here because source and destination are slots of the same
     * array and therefore overlap. */
    if (s_command_history_count == SHELL_COMMAND_HISTORY_DEPTH) {
        for (index = 1; index < SHELL_COMMAND_HISTORY_DEPTH; index++) {
            memmove(s_command_history[index - 1], s_command_history[index], SHELL_COMMAND_BYTES);
        }
        s_command_history_count--;
    }

    snprintf(s_command_history[s_command_history_count], SHELL_COMMAND_BYTES, "%s", entry);
    s_command_history_count++;
}

void shell_recall_history(int direction)
{
    char current_input[SHELL_COMMAND_BYTES];

    if (s_command_history_count == 0) {
        return;
    }

    shell_extract_input_text(current_input, sizeof(current_input));

    if (s_command_history_cursor < 0) {
        /* Entering recall: remember what the user was typing. */
        snprintf(s_history_draft, sizeof(s_history_draft), "%s", current_input);
        if (direction < 0) {
            s_command_history_cursor = (int)s_command_history_count - 1;
        } else {
            return;
        }
    } else {
        s_command_history_cursor += direction;
        if (s_command_history_cursor < 0 ||
            s_command_history_cursor >= (int)s_command_history_count) {
            /* Walked off either end: restore the saved draft. */
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            return;
        }
    }

    shell_input_line_set_text(s_command_history[s_command_history_cursor]);
}

const char *shell_get_history_draft(void)
{
    return s_history_draft;
}

void shell_reset_history_cursor(void)
{
    s_command_history_cursor = -1;
    s_history_draft[0] = '\0';
}

/* ========================================================================
 * INPUT LINE
 * ========================================================================
 * The LVGL input line always renders the shell prompt as a literal prefix.
 * These helpers are the single place that knows about that contract, so the
 * prompt can never be corrupted by editing or history recall.
 *
 * Because the `prompt` command can change the template at runtime, the prefix
 * currently painted on the widget is snapshotted here whenever the text is
 * set. Extraction and repair compare against that snapshot rather than
 * re-rendering, so a template or path change between two events can never
 * make the shell mis-parse what the user typed.
 */

static char s_input_line_prompt[SHELL_PROMPT_RENDER_BYTES] = SHELL_PROMPT;

/**
 * Render the active prompt into a single-line form fit for an LVGL textarea.
 * Control characters produced by `$_` or `$e` collapse to spaces because the
 * widget is one line and cannot render them.
 */
static void shell_input_line_render_prompt(char *output, size_t output_size)
{
    const char *rendered = shell_prompt_render_plain();
    size_t index;

    if (output == NULL || output_size == 0) {
        return;
    }

    snprintf(output, output_size, "%s", rendered != NULL ? rendered : SHELL_PROMPT);

    for (index = 0; output[index] != '\0'; index++) {
        if ((unsigned char)output[index] < 0x20) {
            output[index] = ' ';
        }
    }

    if (output[0] == '\0') {
        snprintf(output, output_size, "%s", SHELL_PROMPT);
    }
}

void shell_input_line_set_text(const char *command_text)
{
    char buffer[SHELL_PROMPT_RENDER_BYTES + SHELL_COMMAND_BYTES];
    const char *safe_command = command_text != NULL ? command_text : "";
    lv_obj_t *input_line = windows_get_input_line();

    if (input_line == NULL) {
        return;
    }

    /* The input line is LVGL-backed state; serialize with the render cycle
     * the same way the transcript appends do. The mutex is recursive, so the
     * LVGL event path (which already holds it) nests without deadlock. */
    lvgl_port_lock(0);

    /* Snapshot the prefix actually painted so extraction stays exact. */
    shell_input_line_render_prompt(s_input_line_prompt, sizeof(s_input_line_prompt));

    snprintf(buffer, sizeof(buffer), "%s%s", s_input_line_prompt, safe_command);
    lv_textarea_set_text(input_line, buffer);
    lv_textarea_set_cursor_pos(input_line, LV_TEXTAREA_CURSOR_LAST);
    lvgl_port_unlock();
}

void shell_input_line_reset(void)
{
    shell_input_line_set_text("");
}

void shell_extract_input_text(char *output, size_t output_size)
{
    lv_obj_t *input_line = windows_get_input_line();
    const char *text = input_line != NULL ? lv_textarea_get_text(input_line) : NULL;

    if (output == NULL || output_size == 0) {
        return;
    }

    if (text == NULL) {
        output[0] = '\0';
        return;
    }

    /* Cache prompt lengths so each is computed once instead of twice (once for
     * strncmp, once for the pointer offset). */
    size_t prompt_len = strlen(s_input_line_prompt);
    size_t fallback_len = strlen(SHELL_PROMPT);

    if (strncmp(text, s_input_line_prompt, prompt_len) == 0) {
        snprintf(output, output_size, "%s", text + prompt_len);
    } else if (strncmp(text, SHELL_PROMPT, fallback_len) == 0) {
        /* Fall back to the compile-time prompt so a template change that
         * lands between the set and the submit still parses correctly. */
        snprintf(output, output_size, "%s", text + fallback_len);
    } else {
        snprintf(output, output_size, "%s", text);
    }

    shell_trim(output);
}

void shell_input_line_repair_prompt(const char *text)
{
    char repaired[SHELL_COMMAND_BYTES];
    const char *user_text;
    size_t prompt_len;
    size_t text_len;
    size_t common = 0;

    if (text == NULL) {
        return;
    }

    /* Nothing to repair while the prompt prefix is intact. */
    if (strncmp(text, s_input_line_prompt, strlen(s_input_line_prompt)) == 0) {
        return;
    }

    prompt_len = strlen(s_input_line_prompt);
    text_len = strlen(text);
    while (common < prompt_len && common < text_len && s_input_line_prompt[common] == text[common]) {
        common++;
    }

    if (common > 0 && common < prompt_len) {
        /* Prompt partially deleted: skip whatever prompt characters remain. */
        user_text = text + common;
        while (*user_text == ' ') {
            user_text++;
        }
    } else {
        /* Prompt fully deleted (or never matched): treat all of it as input. */
        user_text = text;
    }

    snprintf(repaired, sizeof(repaired), "%s", user_text);
    shell_trim(repaired);
    shell_input_line_set_text(repaired);
}

/* ========================================================================
 * DEBUG LOG
 * ======================================================================== */

void shell_debug_log_push(const char *tag, const char *message)
{
    size_t index;

    index = s_debug_log.next_index;
    snprintf(s_debug_log.entries[index], SHELL_DEBUG_ENTRY_BYTES,
             "%s: %s", tag != NULL ? tag : "log", message != NULL ? message : "");

    s_debug_log.next_index = (s_debug_log.next_index + 1) % SHELL_DEBUG_LOG_DEPTH;
    if (s_debug_log.count < SHELL_DEBUG_LOG_DEPTH) {
        s_debug_log.count++;
    }
}

void shell_record_errorf(const char *tag, int error, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    if (tag == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    shell_debug_log_push(tag, message);
    ESP_LOGE(tag, "%s (0x%x)", message, (unsigned int)error);

    /* Errors are also surfaced in the transcript so a user working purely
     * from the on-screen shell sees the failure without running `debug`. */
    shell_schedule_transcript_appendf("%s: %s: %s (0x%x)\n",
                                      tag,
                                      message,
                                      esp_err_to_name((esp_err_t)error),
                                      (unsigned int)error);
}

void shell_record_warningf(const char *tag, const char *format, ...)
{
    char message[SHELL_DEBUG_ENTRY_BYTES];
    va_list args;

    if (tag == NULL || format == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    s_runtime_warning_count++;
    shell_debug_log_push(tag, message);
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

    shell_debug_log_push(tag, message);
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

    shell_transcript_appendf_ansi(SH_LBL "debug.log:" SH_RST " %u entries, " SH_WARN "%u warnings" SH_RST "\n",
                             (unsigned int)s_debug_log.count,
                             (unsigned int)s_runtime_warning_count);

    if (s_debug_log.count == 0) {
        shell_transcript_appendf_ansi(SH_MUTE "debug.log: (empty)" SH_RST "\n");
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
            shell_transcript_appendf_ansi("  " SH_MUTE "[%u]" SH_RST " " SH_ERR "%s" SH_RST "\n", (unsigned int)i, entry);
        } else if (strstr(entry, "WARN") != NULL) {
            shell_transcript_appendf_ansi("  " SH_MUTE "[%u]" SH_RST " " SH_WARN "%s" SH_RST "\n", (unsigned int)i, entry);
        } else {
            shell_transcript_appendf_ansi("  " SH_MUTE "[%u]" SH_RST " %s\n", (unsigned int)i, entry);
        }
    }
}

/* ========================================================================
 * INTERACTIVE KEYPRESS WAIT
 * ========================================================================
 * `pause`, `choice`, and `more` block on a real keystroke instead of a timed
 * delay. Every input source (UART console reader, USB HID bridge, LVGL
 * on-screen keyboard) funnels keys here through shell_key_wait_submit().
 * While a wait is active the sources stop treating input as a command line,
 * so a keypress answering a prompt is never dispatched as a shell command.
 */

void shell_key_wait_begin(void)
{
    if (s_key_queue == NULL) {
        return;
    }

    /* Discard anything typed before the prompt appeared so a stale keystroke
     * cannot satisfy this wait immediately. */
    xQueueReset(s_key_queue);
    s_key_wait_active = true;
}

void shell_key_wait_end(void)
{
    s_key_wait_active = false;

    if (s_key_queue != NULL) {
        xQueueReset(s_key_queue);
    }
}

bool shell_key_wait_is_active(void)
{
    return s_key_wait_active;
}

bool shell_key_wait_submit(char key)
{
    if (!s_key_wait_active || s_key_queue == NULL) {
        return false;
    }

    /* Never block an input-source task on a full queue; a wait only ever
     * consumes one key, so dropping the overflow is the correct behavior. */
    if (xQueueSend(s_key_queue, &key, 0) != pdTRUE) {
        return true;
    }

    return true;
}

bool shell_wait_for_key(uint32_t timeout_ms, char *key_out)
{
    char key = '\0';

    if (key_out != NULL) {
        *key_out = '\0';
    }

    if (s_key_queue == NULL || !s_key_wait_active) {
        return false;
    }

    if (xQueueReceive(s_key_queue, &key, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    if (key_out != NULL) {
        *key_out = key;
    }

    return true;
}

bool shell_read_line(char *output, size_t output_size, uint32_t timeout_ms)
{
    size_t length = 0;
    bool completed = false;

    if (output == NULL || output_size == 0) {
        return false;
    }

    output[0] = '\0';

    /* Without a key source the caller must not block; report failure so it
     * can fall back rather than stalling a batch file. */
    if (!shell_key_input_available()) {
        return false;
    }

    shell_key_wait_begin();

    while (true) {
        char key = '\0';

        if (!shell_wait_for_key(timeout_ms, &key)) {
            break;
        }

        if (key == '\r' || key == '\n') {
            completed = true;
            break;
        }

        /* ESC abandons the line, matching the input-line behavior. */
        if (key == 0x1B) {
            output[0] = '\0';
            length = 0;
            break;
        }

        if (key == '\b' || key == 0x7F) {
            if (length > 0) {
                output[--length] = '\0';
                /* Echo the erase so the transcript matches what is stored. */
                shell_transcript_append_text("\b");
            }
            continue;
        }

        /* Ignore control characters that are not editing keys. */
        if ((unsigned char)key < 0x20) {
            continue;
        }

        if (length + 1 >= output_size) {
            continue;
        }

        output[length++] = key;
        output[length] = '\0';

        /* Echo as typed so the user can see what they are entering. */
        {
            char echo[2] = {key, '\0'};

            shell_transcript_append_text(echo);
        }
    }

    shell_key_wait_end();
    shell_transcript_append_text("\n");

    return completed;
}

bool shell_key_input_available(void)
{
    /* The UART console is the always-on key source once started. A USB
     * keyboard is the other one; ask the USB module directly so an
     * on-screen-only session still falls back to the timed path. */
    return s_uart_console_running ||
           (s_command_ops.usb_is_keyboard_attached != NULL && s_command_ops.usb_is_keyboard_attached());
}

/* ========================================================================
 * RUNTIME PROMPT TEMPLATE
 * ========================================================================
 * The `prompt` command stores a DOS-style template here. Rendering expands
 * the `$` metacharacters against live state (path, date, time, version) and
 * is shared by the UART console and the LVGL input line, so both surfaces
 * always agree on what the prompt looks like.
 */

/**
 * Expand the prompt template into @p output.
 *
 * @param output       Destination buffer.
 * @param output_size  Size of @p output in bytes.
 * @param allow_escape When false, `$e` (ESC) is dropped instead of emitted.
 *                     The LVGL textarea cannot render escape sequences.
 */
static void shell_prompt_expand(char *output, size_t output_size, bool allow_escape)
{
    size_t out = 0;
    const char *cursor = s_prompt_template;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';

    while (*cursor != '\0' && out + 1 < output_size) {
        const char *insert = NULL;
        char single[2] = {0, 0};
        char path_buf[P4_CONFIG_PS_PATH_MAX_DISPLAY + 8];

        if (*cursor != '$') {
            output[out++] = *cursor++;
            continue;
        }

        cursor++;
        if (*cursor == '\0') {
            /* Trailing '$' is literal, matching COMMAND.COM. */
            output[out++] = '$';
            break;
        }

        switch (tolower((unsigned char)*cursor)) {
        case 'p':
            shell_get_cwd_for_prompt(path_buf, sizeof(path_buf));
            insert = path_buf;
            break;
        case 'g':
            single[0] = '>';
            insert = single;
            break;
        case 'l':
            single[0] = '<';
            insert = single;
            break;
        case 'b':
            single[0] = '|';
            insert = single;
            break;
        case 'q':
            single[0] = '=';
            insert = single;
            break;
        case 'a':
            single[0] = '&';
            insert = single;
            break;
        case 'c':
            single[0] = '(';
            insert = single;
            break;
        case 'f':
            single[0] = ')';
            insert = single;
            break;
        case 's':
            single[0] = ' ';
            insert = single;
            break;
        case '_':
            single[0] = '\n';
            insert = single;
            break;
        case '$':
            single[0] = '$';
            insert = single;
            break;
        case 'n':
            /* Drive letter. This board has a single SD volume. */
            insert = P4_CONFIG_SD_DRIVE_LETTER;
            break;
        case 'v':
            insert = P4_CONFIG_VERSION_STRING;
            break;
        case 'd':
        case 't': {
            const char *stamp = time_get_formatted();
            /* "YYYY-MM-DD HH:MM:SS" — take the date or the time half. */
            if (stamp != NULL && strlen(stamp) >= 19) {
                static char part[12];
                if (tolower((unsigned char)*cursor) == 'd') {
                    memcpy(part, stamp, 10);
                    part[10] = '\0';
                } else {
                    memcpy(part, stamp + 11, 8);
                    part[8] = '\0';
                }
                insert = part;
            }
            break;
        }
        case 'e':
            if (allow_escape) {
                single[0] = 0x1B;
                insert = single;
            }
            break;
        case 'h':
            /* Destructive backspace: remove the previously rendered char. */
            if (out > 0) {
                out--;
            }
            break;
        default:
            /* Unknown metacharacter renders literally, including the '$'. */
            if (out + 1 < output_size) {
                output[out++] = '$';
            }
            single[0] = *cursor;
            insert = single;
            break;
        }

        cursor++;

        if (insert == NULL) {
            continue;
        }

        while (*insert != '\0' && out + 1 < output_size) {
            output[out++] = *insert++;
        }
    }

    output[out] = '\0';
}

void shell_prompt_set_template(const char *template_text)
{
    if (template_text == NULL || template_text[0] == '\0') {
        shell_prompt_reset();
        return;
    }

    snprintf(s_prompt_template, sizeof(s_prompt_template), "%s", template_text);
}

const char *shell_prompt_get_template(void)
{
    return s_prompt_template;
}

void shell_prompt_reset(void)
{
    snprintf(s_prompt_template, sizeof(s_prompt_template), "%s", P4_CONFIG_PROMPT_DEFAULT_TEMPLATE);
}

const char *shell_prompt_render_plain(void)
{
    static char rendered[SHELL_PROMPT_RENDER_BYTES];

    shell_prompt_expand(rendered, sizeof(rendered), false);
    if (rendered[0] == '\0') {
        snprintf(rendered, sizeof(rendered), "%s", SHELL_PROMPT);
    }

    return rendered;
}

/* ========================================================================
 * UART CONSOLE BRIDGE
 * ======================================================================== */

/**
 * Serial console reader. Uses line-buffered fgets() so the shell works over
 * USB-Serial-JTAG (where CONFIG_ESP_CONSOLE_UART_NUM is -1 and there is no
 * UART driver to install) as well as a real UART. The prompt is only redrawn
 * after a submission or an empty line, which avoids prompt spam while idle.
 *
 * stdin is set to unbuffered mode (_IONBF) in shell_uart_console_start(), so
 * fgets() reads character-by-character from the underlying USB CDC or UART
 * driver. On error (e.g. USB disconnect), fgets() returns NULL; the loop
 * clears the error with clearerr(stdin) and retries after a short delay.
 * This is the standard ESP-IDF pattern for console input on USB-Serial-JTAG.
 *
 * While an interactive keypress wait is active the reader stops printing the
 * prompt and forwards the first character of each line into the key queue
 * instead of dispatching it as a command.
 */
static void shell_uart_console_task(void *arg)
{
    char line[SHELL_COMMAND_BYTES];
    bool prompt_visible = false;

    (void)arg;

    shell_uart_console_write_text("\nUART console ready. Type help for commands.\n");

    while (true) {
        char *trimmed;
        size_t length;

        if (!prompt_visible && !s_key_wait_active) {
            shell_uart_console_print_prompt();
            prompt_visible = true;
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            clearerr(stdin);
            continue;
        }

        /* A pending keypress wait swallows the line before any command
         * lookup, so answering `pause` or `choice` never dispatches a
         * command. A bare Enter reports as '\r'. */
        if (s_key_wait_active) {
            char key = line[0];

            if (key == '\n' || key == '\0') {
                key = '\r';
            }
            shell_key_wait_submit(key);
            prompt_visible = false;
            continue;
        }

        trimmed = shell_trim(line);
        length = strlen(trimmed);
        while (length > 0 && (trimmed[length - 1] == '\n' || trimmed[length - 1] == '\r')) {
            trimmed[--length] = '\0';
        }

        if (trimmed[0] == '\0') {
            prompt_visible = false;
            continue;
        }

        shell_uart_console_submit_command(trimmed);
        prompt_visible = false;
    }
}

void shell_uart_console_start(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (s_uart_console_lock == NULL) {
        s_uart_console_lock = xSemaphoreCreateMutex();
    }

    if (s_shell_command_lock == NULL) {
        s_shell_command_lock = xSemaphoreCreateMutex();
    }

    if (xTaskCreate(shell_uart_console_task,
                    "shell_uart",
                    SHELL_UART_CONSOLE_TASK_STACK_BYTES,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        shell_record_errorf("uart", ESP_FAIL, "Failed to start UART console task");
        return;
    }

    /* The console reader is now an interactive key source, so pause, choice,
     * and more can block on a real keystroke instead of a timed delay. */
    s_uart_console_running = true;
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
    char prompt_buf[P4_CONFIG_ANSI_BUFFER_BYTES];
    int len;

    /* This function is the one place that emits raw SGR rather than palette
     * macros. It writes straight to the serial console instead of the
     * transcript, so there is no ansi_format() pass to expand `@` codes, and
     * the colour numbers are parameterised from the P4_CONFIG_PS_COLOR_*
     * values rather than fixed. Everything that reaches the transcript uses
     * the palette in components/ansi/ansi_palette.h.
     *
     * A user-supplied `prompt` template takes over the whole line. It is
     * expanded with escape support (so `$e` works on a real terminal) and
     * wrapped in the prompt color. */
    if (strcmp(s_prompt_template, P4_CONFIG_PROMPT_DEFAULT_TEMPLATE) != 0) {
        char rendered[SHELL_PROMPT_RENDER_BYTES];

        shell_prompt_expand(rendered, sizeof(rendered), true);
        len = snprintf(prompt_buf, sizeof(prompt_buf),
                       "\x1B[%dm%s\x1B[0m", P4_CONFIG_PS_COLOR_PROMPT, rendered);
        if (len > 0 && (size_t)len < sizeof(prompt_buf)) {
            shell_uart_console_write_text(prompt_buf);
            return;
        }
        shell_uart_console_write_text(rendered);
        return;
    }

    /* Default PowerShell-style colored prompt: "PS " (bright white) + path
     * (bright yellow) + "> " (bright white). On the UART console this renders
     * with ANSI SGR codes for a native PowerShell look. The LVGL input line
     * uses the plain-text form from shell_prompt_render_plain(). */
    {
        char cwd_buf[P4_CONFIG_PS_PATH_MAX_DISPLAY + 8];
        shell_get_cwd_for_prompt(cwd_buf, sizeof(cwd_buf));

        /* Build colored prompt: \e[97mPS \e[93m<path>\e[97m> \e[0m */
        len = snprintf(prompt_buf, sizeof(prompt_buf),
                       "\x1B[%dm" P4_CONFIG_PS_PREFIX "\x1B[%dm%s\x1B[%dm" P4_CONFIG_PS_SUFFIX "\x1B[0m",
                       P4_CONFIG_PS_COLOR_PREFIX,
                       P4_CONFIG_PS_COLOR_PATH, cwd_buf,
                       P4_CONFIG_PS_COLOR_SUFFIX);
        if (len > 0 && (size_t)len < sizeof(prompt_buf)) {
            shell_uart_console_write_text(prompt_buf);
        } else {
            shell_uart_console_write_text(P4_CONFIG_SHELL_PROMPT);
        }
    }
}

void shell_uart_console_submit_command(const char *command)
{
    char command_copy[SHELL_COMMAND_BYTES];
    char transcript_command[SHELL_COMMAND_BYTES];

    if (command == NULL || command[0] == '\0') {
        return;
    }

    snprintf(command_copy, sizeof(command_copy), "%s", command);
    shell_format_command_for_transcript(command_copy, transcript_command, sizeof(transcript_command));

    /* Serialize UART submissions so two console lines can never interleave
     * their transcript writes. */
    if (s_shell_command_lock != NULL) {
        (void)xSemaphoreTake(s_shell_command_lock, portMAX_DELAY);
    }

    /* The transcript and history are LVGL-backed state, so take the LVGL
     * lock before touching them from this non-LVGL task. */
    if (!lvgl_port_lock(0)) {
        shell_schedule_transcript_appendf("shell: failed to lock LVGL for UART command %s\n", transcript_command);
        shell_record_errorf("uart", ESP_FAIL, "Failed to lock LVGL for UART command %s", transcript_command);
        if (s_shell_command_lock != NULL) {
            xSemaphoreGive(s_shell_command_lock);
        }
        return;
    }

    shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);
    if (shell_command_should_store_history(command_copy)) {
        shell_store_command_history(command_copy);
    }
    shell_reset_history_cursor();
    if (s_command_ops.execute_command != NULL) {
        s_command_ops.execute_command(command_copy);
    }
    shell_history_transcript_scroll_to_end();
    lvgl_port_unlock();

    if (s_shell_command_lock != NULL) {
        xSemaphoreGive(s_shell_command_lock);
    }
}

/* ========================================================================
 * POWERSHELL-STYLE PROMPT PATH HELPER
 * ========================================================================
 * Returns the current working directory formatted for the prompt.
 * Truncates long paths with "..." prefix to keep the prompt compact.
 *
 * Thread-safe: writes into the caller-provided @p buf so no shared state
 * exists between the UART console task and the LVGL input-line task, both of
 * which render the prompt.
 */

void shell_get_cwd_for_prompt(char *buf, size_t buf_size)
{
    const char *cwd = shell_current_cwd();

    if (buf == NULL || buf_size == 0) {
        return;
    }

    if (cwd == NULL || cwd[0] == '\0') {
        snprintf(buf, buf_size, "%s", P4_CONFIG_PS_PATH_SEPARATOR);
        return;
    }

    size_t len = strlen(cwd);
    if (len <= P4_CONFIG_PS_PATH_MAX_DISPLAY) {
        snprintf(buf, buf_size, "%s", cwd);
        return;
    }

    /* Truncate: show "..." + last portion */
    const char *last_sep = strrchr(cwd, '\\');
    if (last_sep == NULL) last_sep = strrchr(cwd, '/');
    if (last_sep == NULL) {
        /* No separator found — truncate from beginning */
        size_t keep = P4_CONFIG_PS_PATH_MAX_DISPLAY - 3;
        if (keep > len) keep = len;
        snprintf(buf, buf_size, "...%s", cwd + len - keep);
        return;
    }

    size_t suffix_len = strlen(last_sep);
    if (suffix_len + 3 > P4_CONFIG_PS_PATH_MAX_DISPLAY) {
        /* Even the suffix alone is too long */
        size_t keep = P4_CONFIG_PS_PATH_MAX_DISPLAY - 3;
        if (keep > suffix_len) keep = suffix_len;
        snprintf(buf, buf_size, "...%s", last_sep + suffix_len - keep);
    } else {
        snprintf(buf, buf_size, "...%s", last_sep);
    }
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

    {
        char ch = '\0';

        if (s_command_ops.usb_key_to_ascii != NULL &&
            s_command_ops.usb_key_to_ascii(ctx->key_code, ctx->modifiers, &ch)) {
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
                shell_recall_history(-1);
                break;
            case 0x52: /* Up arrow: recall newer history */
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

    /* An active keypress wait consumes the key directly and never reaches
     * the input line, so answering `pause` or `choice` cannot leave stray
     * characters at the prompt. Handled synchronously here rather than in
     * the LVGL async callback because the wait runs on the command worker
     * task and must not depend on an LVGL round trip. */
    if (s_key_wait_active) {
        char ch = '\0';

        if (s_command_ops.usb_key_to_ascii != NULL &&
            s_command_ops.usb_key_to_ascii(key_code, modifiers, &ch)) {
            if (ch == '\n') {
                ch = '\r';
            }
        } else {
            /* Non-printable keys still satisfy an "any key" wait. */
            ch = '\r';
        }

        shell_key_wait_submit(ch);
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
    shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell Commands:" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_EXE "help" SH_RST " | sysinfo | clear/cls | reboot | version/ver | about | debug | mem\n");
    shell_transcript_appendf_ansi("  " SH_EXE "brightness" SH_RST " <0-100> | " SH_EXE "rotate" SH_RST " <0|90|180|270> | " SH_EXE "battery" SH_RST " | " SH_EXE "volume" SH_RST " <0-100>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "gpio" SH_RST " list | status | read <pin> | set <pin> <0|1>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "cd" SH_RST " [path] | " SH_EXE "copy" SH_RST " <src> <dst> | " SH_EXE "move" SH_RST " <src> <dst>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "dir" SH_RST " [path] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "del" SH_RST " <path> | " SH_EXE "ren" SH_RST " <src> <dst> | " SH_EXE "md" SH_RST " <path> | " SH_EXE "rd" SH_RST " <path>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "type" SH_RST " <path> | " SH_EXE "write" SH_RST " <path> <text> | " SH_EXE "append" SH_RST " <path> <text> | " SH_EXE "touch" SH_RST " <path>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "set" SH_RST " [NAME=VALUE] | " SH_EXE "set /a" SH_RST " NAME=<expr> | " SH_EXE "set /p" SH_RST " NAME=<prompt>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "path" SH_RST " [dirs] | " SH_EXE "echo" SH_RST " <text> | " SH_EXE "echo" SH_RST " on|off | " SH_EXE "call" SH_RST " <file.bat>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "attrib" SH_RST " [+-RHSA] <path> | " SH_EXE "label" SH_RST " [name] | " SH_EXE "xcopy" SH_RST " <src> <dst> [/S]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "chkdsk" SH_RST " [path] [/F] | " SH_EXE "format" SH_RST " [/FS:type] [/V:label]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "if" SH_RST " [not] errorlevel|exist|\"a\"==\"b\" cmd | " SH_EXE "goto" SH_RST " <label> | " SH_EXE "shift" SH_RST " | " SH_EXE "exit" SH_RST " [/b] [code]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "pause" SH_RST " | " SH_EXE "choice" SH_RST " [/C:keys] [/N] [/T:c,secs] [/S] [text] | " SH_EXE "setlocal" SH_RST " | " SH_EXE "endlocal" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_EXE "prompt" SH_RST " [template] | " SH_EXE "date" SH_RST " [MM-DD-YYYY] | " SH_EXE "time" SH_RST " [HH:MM[:SS]]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "find" SH_RST " <text> [file] [/I] [/N] [/C] [/V] | " SH_EXE "more" SH_RST " [file] | " SH_EXE "fc" SH_RST " <f1> <f2>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "tree" SH_RST " [path] [/F] [/A] | " SH_EXE "sort" SH_RST " [file] [/R] [/I] [/U]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "sd" SH_RST " info | ls [path] | stat <path> | cat <path> [bytes] | " SH_EXE "sdeject" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_EXE "wifi" SH_RST " status | scan | diag | connect [ssid pass] | disconnect\n");
    shell_transcript_appendf_ansi("  " SH_EXE "bluetooth" SH_RST " status | scan | advertise on|off\n");
    shell_transcript_appendf_ansi("  " SH_EXE "usb" SH_RST " status | ls [path] | keyboard <on|off> | mouse <on|off>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "c6ota" SH_RST " <sd:/path|http[s]://url|default>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "display" SH_RST " info | resolution | refresh | power <on|sleep|off>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "keyboard" SH_RST " show | hide | toggle | status\n");
    shell_transcript_appendf_ansi("  " SH_EXE "windows" SH_RST " info\n");
    shell_transcript_appendf_ansi(SH_MUTE "History recall:" SH_RST " Prev/Next buttons above the keyboard\n");
    shell_transcript_appendf_ansi(SH_MUTE "Redirection:" SH_RST " > file (overwrite) | >> file (append) | < file (input)\n");
    shell_transcript_appendf_ansi(SH_MUTE "Pipes:" SH_RST " cmd1 | cmd2 | cmd3 (up to %d stages)\n", P4_CONFIG_PIPE_STAGE_MAX);
    shell_transcript_appendf_ansi(SH_MUTE "Chaining:" SH_RST " a & b (both) | a && b (if a works) | a || b (if a fails)\n");
    shell_transcript_appendf_ansi(SH_MUTE "Quoting:" SH_RST " \"text\" groups, 'text' is literal, ^c escapes one character\n");
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
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? SH_ERR : SH_OK;

    shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell System Information:" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "board.requested_name:" SH_RST " %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf_ansi("  " SH_LBL "board.detected_name:" SH_RST " %s\n", SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  " SH_LBL "version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  " SH_LBL "time:" SH_RST " %s %s%s\n",
                             time_get_formatted(),
                             time_get_timezone(),
                             time_is_synchronized() ? " " SH_OK "(synced)" SH_RST : " " SH_WARN "(unsynced)" SH_RST);

    /* Display info from the display manager */
    {
        display_info_t disp_info = display_get_info();
        shell_transcript_appendf_ansi("  " SH_LBL "display:" SH_RST " %" PRId32 " x %" PRId32 " (native %" PRId32 " x %" PRId32
                                 "), %s, reset GPIO %d, backlight GPIO %d\n",
                                 disp_info.resolution.current_width,
                                 disp_info.resolution.current_height,
                                 disp_info.resolution.native_width,
                                 disp_info.resolution.native_height,
                                 disp_info.panel_driver,
                                 BOARD_CFG_LCD_RST_GPIO,
                                 BOARD_CFG_LCD_BACKLIGHT_GPIO);
        shell_transcript_appendf_ansi("  " SH_LBL "display.state:" SH_RST " brightness=%d%% rotation=%s power=%s refresh=%" PRIu32 "Hz\n",
                                 disp_info.brightness_percent,
                                 display_rotation_to_string(disp_info.rotation),
                                 disp_info.power_state == DISPLAY_POWER_ON ? SH_OK "on" SH_RST :
                                 disp_info.power_state == DISPLAY_POWER_SLEEP ? SH_WARN "sleep" SH_RST : SH_ERR "off" SH_RST,
                                 disp_info.refresh.current_hz);
        shell_transcript_appendf_ansi("  " SH_LBL "display.timing:" SH_RST " pclk=%" PRIu32 "MHz, lanes=%d, bitrate=%" PRIu32
                                 "Mbps, hsync=%" PRIu32 " hbp=%" PRIu32 " hfp=%" PRIu32
                                 " vsync=%" PRIu32 " vbp=%" PRIu32 " vfp=%" PRIu32 "\n",
                                 disp_info.refresh.pixel_clock_mhz,
                                 disp_info.mipi_lane_num,
                                 disp_info.refresh.dsi_lane_bitrate_mbps,
                                 disp_info.hsync, disp_info.hbp, disp_info.hfp,
                                 disp_info.vsync, disp_info.vbp, disp_info.vfp);
        shell_transcript_appendf_ansi("  " SH_LBL "display.buffer:" SH_RST " draw=%" PRIu32 ", double=%d, dma=%d, spiram=%d, sw_rotate=%d\n",
                                 disp_info.draw_buffer_size,
                                 disp_info.double_buffer ? 1 : 0,
                                 disp_info.buffer_dma ? 1 : 0,
                                 disp_info.buffer_spiram ? 1 : 0,
                                 disp_info.sw_rotate ? 1 : 0);
        shell_transcript_appendf_ansi("  " SH_LBL "touch:" SH_RST " %s on I2C%d, SDA GPIO %d, SCL GPIO %d, %dHz, pullup=%d\n",
                                 disp_info.touch_driver,
                                 BOARD_CFG_I2C_PORT,
                                 BOARD_CFG_I2C_SDA_GPIO,
                                 BOARD_CFG_I2C_SCL_GPIO,
                                 BOARD_CFG_I2C_CLK_SPEED_HZ,
                                 BOARD_CFG_I2C_ENABLE_INTERNAL_PULLUP);
    }

    shell_transcript_appendf_ansi("  " SH_LBL "audio:" SH_RST " I2S%d BCLK=%d WS=%d DOUT=%d MCLK=%d amp=%d volume=%d%%%%\n",
                             BOARD_CFG_I2S_PORT,
                             BSP_I2S_SCLK,
                             BSP_I2S_LCLK,
                             BSP_I2S_DOUT,
                             BSP_I2S_MCLK,
                             BSP_POWER_AMP_IO,
                             s_command_ops.get_volume_percent != NULL
                                 ? s_command_ops.get_volume_percent() : 0);
    shell_transcript_appendf_ansi("  " SH_LBL "battery:" SH_RST " adc_gpio=%d divider=%d:%d range=%dmV..%dmV\n",
                             BOARD_CFG_BATTERY_ADC_GPIO,
                             BOARD_CFG_BATTERY_DIVIDER_NUMERATOR,
                             BOARD_CFG_BATTERY_DIVIDER_DENOMINATOR,
                             BOARD_CFG_BATTERY_EMPTY_MV,
                             BOARD_CFG_BATTERY_FULL_MV);
    shell_transcript_appendf_ansi("  " SH_LBL "hardware.rgb:" SH_RST " gpio=%d ws2812=%d\n",
                             BOARD_CFG_RGB_LED_GPIO,
                             BOARD_CFG_RGB_LED_IS_WS2812);
    shell_transcript_appendf_ansi("  " SH_LBL "hardware.camera:" SH_RST " supported=%d\n", BOARD_CFG_CAMERA_SUPPORTED);
    shell_transcript_appendf_ansi("  " SH_LBL "storage:" SH_RST " spiffs=%s, sd=%s\n", BSP_SPIFFS_MOUNT_POINT, BSP_SD_MOUNT_POINT);
    shell_transcript_appendf_ansi("  " SH_LBL "c6.hosted_transport:" SH_RST " sdio reset_gpio=%d busy=%s\n",
                             P4_CONFIG_C6_HOST_RESET_GPIO,
                             s_command_ops.c6ota_is_busy != NULL && s_command_ops.c6ota_is_busy()
                                 ? SH_WARN "yes" SH_RST : SH_OK "no" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "idf:" SH_RST " %s\n", esp_get_idf_version());
    shell_transcript_appendf_ansi("  " SH_LBL "freertos:" SH_RST " tasks=%" PRIu32 " uptime=%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             task_count, days, hours, mins, secs);
    shell_transcript_appendf_ansi("  " SH_LBL "heap:" SH_RST " free=%u bytes, internal_free=%u bytes, total=%u bytes (%s%u%%%%" SH_RST ")\n",
                             (unsigned int)free_heap, (unsigned int)free_internal,
                             (unsigned int)total_heap, heap_color, heap_pct);
#if CONFIG_SPIRAM
    shell_transcript_appendf_ansi("  " SH_LBL "psram:" SH_RST " " SH_OK "enabled" SH_RST ", total=%u bytes, free=%u bytes\n",
                             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    shell_transcript_appendf_ansi("  " SH_LBL "psram:" SH_RST " " SH_MUTE "disabled" SH_RST "\n");
#endif

    if (s_command_ops.append_sysinfo_summary != NULL) {
        s_command_ops.append_sysinfo_summary();
    }
}

void shell_command_version(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    uint32_t uptime_sec = time_get_uptime_sec();
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const char *idf_ver = esp_get_idf_version();
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? SH_ERR : SH_OK;

    shell_transcript_appendf_ansi(SH_HEAD "%s" SH_RST "\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf_ansi("  " SH_LBL "version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  " SH_LBL "idf:" SH_RST " %s\n", idf_ver != NULL ? idf_ver : SH_ERR "unknown" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "board:" SH_RST " %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  " SH_LBL "chip:" SH_RST " %s rev %d, %d cores\n",
                             CONFIG_IDF_TARGET, 1, 2);
    shell_transcript_appendf_ansi("  " SH_LBL "time:" SH_RST " %s %s%s\n",
                             time_get_formatted(),
                             time_get_timezone(),
                             time_is_synchronized() ? " " SH_OK "(synced)" SH_RST : " " SH_WARN "(unsynced)" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "uptime:" SH_RST " %" PRIu32 "s\n", uptime_sec);
    shell_transcript_appendf_ansi("  " SH_LBL "heap:" SH_RST " %u/%u bytes free (%s%u%%%%" SH_RST ")\n",
                             (unsigned int)free_heap, (unsigned int)total_heap,
                             heap_color, heap_pct);
    shell_transcript_appendf_ansi("  " SH_LBL "tasks:" SH_RST " %" PRIu32 "\n", task_count);
}

void shell_command_about(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    int64_t uptime_us = esp_timer_get_time() - s_boot_timestamp_us;
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000ULL);
    const char *idf_ver = esp_get_idf_version();

    shell_transcript_appendf_ansi(SH_HEAD SH_BOLD "P4MiniShell" SH_RST " — Embedded DOS-style command shell\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    shell_transcript_appendf_ansi("  " SH_LBL "about.board:" SH_RST " %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  " SH_LBL "about.idf:" SH_RST " %s\n", idf_ver != NULL ? idf_ver : SH_ERR "unknown" SH_RST);
    shell_transcript_appendf_ansi("  " SH_LBL "about.display:" SH_RST " JD9165 1024x600 MIPI-DSI, GT911 touch\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.ui:" SH_RST " locked transcript with touch keyboard, history buttons, and command prompt\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.header:" SH_RST " real-time status bar (WiFi, BT, USB, SD, MEM, CPU, BAT) from FreeRTOS\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.uptime:" SH_RST " %" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             uptime_sec / 86400,
                             (uptime_sec % 86400) / 3600,
                             (uptime_sec % 3600) / 60,
                             uptime_sec % 60);
    shell_transcript_appendf_ansi("  " SH_LBL "about.tasks:" SH_RST " %" PRIu32 "\n", task_count);
}

void shell_command_mem(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t task_count = uxTaskGetNumberOfTasks();
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);
    const char *heap_color = (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) ? SH_ERR : SH_OK;

    shell_transcript_appendf_ansi("  " SH_LBL "mem.heap:" SH_RST " free=%u bytes, total=%u bytes (%s%u%%%%" SH_RST ")\n",
                             (unsigned int)free_heap, (unsigned int)total_heap,
                             heap_color, heap_pct);
    shell_transcript_appendf_ansi("  " SH_LBL "mem.heap_min:" SH_RST " %u bytes\n", (unsigned int)min_free);
    shell_transcript_appendf_ansi("  " SH_LBL "mem.internal:" SH_RST " %u bytes free\n", (unsigned int)free_internal);
    shell_transcript_appendf_ansi("  " SH_LBL "mem.tasks:" SH_RST " %" PRIu32 "\n", task_count);
#if CONFIG_SPIRAM
    shell_transcript_appendf_ansi("  " SH_LBL "mem.psram.free:" SH_RST " %u bytes\n",
                             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    shell_transcript_appendf_ansi("  " SH_LBL "mem.psram.total:" SH_RST " %u bytes\n",
                             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
    shell_transcript_appendf_ansi("  " SH_LBL "mem.psram:" SH_RST " " SH_MUTE "disabled" SH_RST "\n");
#endif
}

/* ========================================================================
 * HEADER STATUS
 * ======================================================================== */

/**
 * Compute instantaneous CPU load from the FreeRTOS idle-task runtime counter.
 * Returns 0 on the first call (no previous sample to diff against) and when
 * runtime stats are unavailable.
 */
static int shell_sample_cpu_percent(uint32_t task_count)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static uint32_t s_last_idle_count = 0;
    static uint32_t s_last_total_runtime = 0;
    TaskStatus_t *task_status_array;
    uint32_t total_runtime = 0;
    uint32_t idle_runtime = 0;
    uint32_t obtained;
    int cpu_percent = 0;

    task_status_array = calloc(task_count, sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        return 0;
    }

    obtained = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
    for (uint32_t i = 0; i < obtained; i++) {
        if (strncmp(task_status_array[i].pcTaskName, "IDLE", 4) == 0) {
            idle_runtime = task_status_array[i].ulRunTimeCounter;
            break;
        }
    }
    free(task_status_array);

    if (s_last_total_runtime > 0 && total_runtime > s_last_total_runtime) {
        uint32_t total_delta = total_runtime - s_last_total_runtime;
        uint32_t idle_delta = (idle_runtime >= s_last_idle_count)
                              ? (idle_runtime - s_last_idle_count) : 0;
        if (total_delta > 0) {
            cpu_percent = 100 - (int)((idle_delta * 100) / total_delta);
            if (cpu_percent < 0) cpu_percent = 0;
            if (cpu_percent > 100) cpu_percent = 100;
        }
    }

    s_last_idle_count = idle_runtime;
    s_last_total_runtime = total_runtime;
    return cpu_percent;
#else
    /* Without runtime stats, approximate load from heap pressure. */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    int heap_pct = (total_heap > 0) ? (int)((free_heap * 100) / total_heap) : 100;
    int cpu_percent = 100 - heap_pct;

    (void)task_count;
    if (cpu_percent < 0) cpu_percent = 0;
    if (cpu_percent > 100) cpu_percent = 100;
    return cpu_percent;
#endif
}

void shell_header_status_refresh(void)
{
    int battery_percent = 0;
    bool battery_ok = false;
    bool wifi_connected = s_command_ops.wifi_is_connected != NULL && s_command_ops.wifi_is_connected();
    int wifi_rssi = P4_CONFIG_HEADER_RSSI_UNKNOWN;
    bool sd_mounted = s_command_ops.sd_is_mounted != NULL && s_command_ops.sd_is_mounted();
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t task_count = uxTaskGetNumberOfTasks();
    int cpu_percent = shell_sample_cpu_percent(task_count);
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);

    /* Signal strength comes through the networking module's accessor, so no
     * esp_wifi_* call escapes components/networking/. */
    if (s_command_ops.wifi_get_rssi != NULL) {
        (void)s_command_ops.wifi_get_rssi(&wifi_rssi);
    }

    /* Battery: always refresh the header even when the ADC read fails so the
     * panel can show "BAT N/C" instead of a stale percentage. */
    if (s_command_ops.battery_read != NULL &&
        s_command_ops.battery_read(NULL, &battery_percent, NULL, NULL) == ESP_OK) {
        battery_ok = true;
    }

    /* Batch every header field into one async render. Individual
     * header_update_*() calls would each schedule their own render and
     * produce visible flicker on each refresh period. */
    header_update_batch(
        wifi_connected, wifi_rssi,
        battery_percent, battery_ok,
        s_command_ops.bluetooth_is_enabled != NULL && s_command_ops.bluetooth_is_enabled(),
        s_command_ops.bluetooth_is_connected != NULL && s_command_ops.bluetooth_is_connected(),
        s_command_ops.usb_is_connected != NULL && s_command_ops.usb_is_connected(),
        sd_mounted ? HEADER_SD_MOUNTED : HEADER_SD_NONE,
        free_heap, total_heap,
        cpu_percent, task_count,
        uptime_sec
    );

    /* USB keyboard auto-detect: hide the on-screen keyboard while a USB
     * keyboard is attached and restore it on removal. Edge-triggered so the
     * keyboard is not forced on every refresh tick. */
    {
        static bool s_last_usb_kb_attached = false;
        bool usb_kb_attached = s_command_ops.usb_is_keyboard_attached != NULL &&
                               s_command_ops.usb_is_keyboard_attached();

        if (usb_kb_attached != s_last_usb_kb_attached) {
            s_last_usb_kb_attached = usb_kb_attached;
            keyboard_set_external_input(usb_kb_attached);
            shell_header_notify(usb_kb_attached ? "USB keyboard detected" : "USB keyboard removed",
                                P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
        }
    }

    /* SD insert/remove notifications, suppressed for the first sample so
     * boot does not emit a spurious "SD card unmounted" message. */
    {
        static bool s_sd_state_known = false;
        static bool s_sd_last_mounted = false;

        if (!s_sd_state_known) {
            s_sd_state_known = true;
            s_sd_last_mounted = sd_mounted;
        } else if (sd_mounted != s_sd_last_mounted) {
            shell_header_notify(sd_mounted ? "SD card mounted" : "SD card unmounted",
                                P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
            s_sd_last_mounted = sd_mounted;
        }
    }

    /* header_update_batch() schedules the async render itself. Never call
     * header_force_render() here: a synchronous full redraw on every refresh
     * period causes visible screen flashes. */
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

/* ========================================================================
 * QUOTING AND ESCAPING
 * ========================================================================
 * One scanner backs every surface that must distinguish syntax from data:
 * the argument tokenizer, redirection parsing, pipe splitting, and command
 * chaining. Keeping it here means those five places can never disagree.
 */

/**
 * Advance one position through @p text, maintaining quote state.
 *
 * @param cursor      Current position.
 * @param state       Quote state, updated in place.
 * @param escaped_out Set to true when this position is an escaped literal
 *                    (the caret was consumed and this is the payload). May be
 *                    NULL. Escapes are inert inside single quotes, matching
 *                    the "fully literal" contract.
 * @return Pointer to the next position to examine.
 */
static const char *shell_quote_advance(const char *cursor,
                                       shell_quote_state_t *state,
                                       bool *escaped_out)
{
    if (escaped_out != NULL) {
        *escaped_out = false;
    }

    if (cursor == NULL || *cursor == '\0') {
        return cursor;
    }

    /* A caret escapes the following character everywhere except inside
     * single quotes, where the whole run is taken literally. */
    if (*cursor == P4_CONFIG_ESCAPE_CHAR && *state != SHELL_QUOTE_SINGLE && cursor[1] != '\0') {
        if (escaped_out != NULL) {
            *escaped_out = true;
        }
        return cursor + 2;
    }

    if (*cursor == '"' && *state != SHELL_QUOTE_SINGLE) {
        *state = (*state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
        return cursor + 1;
    }

    if (*cursor == '\'' && *state != SHELL_QUOTE_DOUBLE) {
        *state = (*state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
        return cursor + 1;
    }

    return cursor + 1;
}

char *shell_find_unquoted_any(const char *text, const char *targets)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    const char *cursor = text;

    if (text == NULL || targets == NULL) {
        return NULL;
    }

    while (*cursor != '\0') {
        const char *candidate = cursor;
        bool escaped = false;
        const char *next = shell_quote_advance(cursor, &state, &escaped);

        /* A candidate only counts when it is the character itself, outside
         * quotes, and not the payload of a caret escape. */
        if (!escaped && state == SHELL_QUOTE_NONE && next == candidate + 1 &&
            strchr(targets, *candidate) != NULL && *candidate != '\0') {
            /* Quote delimiters advance by one too, but they were consumed as
             * state changes above and are never in the target set in
             * practice. Guard anyway so a caller cannot search for a quote. */
            if (*candidate != '"' && *candidate != '\'') {
                return (char *)candidate;
            }
        }

        cursor = next;
    }

    return NULL;
}

char *shell_find_unquoted_char(const char *text, char target)
{
    char targets[2];

    targets[0] = target;
    targets[1] = '\0';

    return shell_find_unquoted_any(text, targets);
}

bool shell_has_unquoted_char(const char *text, char target)
{
    return shell_find_unquoted_char(text, target) != NULL;
}

char *shell_unescape_in_place(char *text)
{
    shell_quote_state_t state = SHELL_QUOTE_NONE;
    char *read;
    char *write;

    if (text == NULL) {
        return NULL;
    }

    read = text;
    write = text;

    while (*read != '\0') {
        /* Caret escape: drop the caret, keep the payload verbatim. */
        if (*read == P4_CONFIG_ESCAPE_CHAR && state != SHELL_QUOTE_SINGLE && read[1] != '\0') {
            *write++ = read[1];
            read += 2;
            continue;
        }

        /* Quote delimiters group text but are not part of the value. */
        if (*read == '"' && state != SHELL_QUOTE_SINGLE) {
            state = (state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
            read++;
            continue;
        }

        if (*read == '\'' && state != SHELL_QUOTE_DOUBLE) {
            state = (state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
            read++;
            continue;
        }

        *write++ = *read++;
    }

    *write = '\0';
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
        shell_quote_state_t state = SHELL_QUOTE_NONE;
        char *token_start;

        /* Skip leading whitespace between arguments. */
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        /* An argument runs until unquoted whitespace. Quote runs and caret
         * escapes are carried along and stripped once the extent is known,
         * so `"a b"`, `'a b'`, and `a^ b` all yield one argument. */
        token_start = p;
        while (*p != '\0') {
            bool escaped = false;
            const char *next = shell_quote_advance(p, &state, &escaped);

            if (!escaped && state == SHELL_QUOTE_NONE && next == p + 1 &&
                isspace((unsigned char)*p)) {
                break;
            }

            p = (char *)next;
        }

        if (*p != '\0') {
            *p = '\0';
            p++;
        }

        /* Remove the quoting and escape markup now that the token is
         * delimited, so handlers receive the literal value. */
        argv[argc] = shell_unescape_in_place(token_start);
        argc++;
    }

    return argc;
}

/* ========================================================================
 * COMMAND CHAINING
 * ======================================================================== */

int shell_split_chain(char *text,
                      shell_chain_segment_t *segments,
                      int max_segments,
                      bool *truncated_out)
{
    int count = 0;
    char *segment_start;
    char *cursor;
    shell_chain_op_t pending_op = SHELL_CHAIN_FIRST;

    if (truncated_out != NULL) {
        *truncated_out = false;
    }

    if (text == NULL || segments == NULL || max_segments <= 0) {
        return 0;
    }

    segment_start = text;
    cursor = text;

    while (true) {
        char *op_pos = shell_find_unquoted_any(cursor, "&|");
        shell_chain_op_t next_op;
        size_t op_len;

        if (op_pos == NULL) {
            break;
        }

        /* A single '|' is the pipe operator, not a chain separator; leave it
         * for the pipeline splitter. '||' is chaining. */
        if (op_pos[0] == '|') {
            if (op_pos[1] != '|') {
                cursor = op_pos + 1;
                continue;
            }
            next_op = SHELL_CHAIN_ON_FAILURE;
            op_len = 2;
        } else {
            if (op_pos[1] == '&') {
                next_op = SHELL_CHAIN_ON_SUCCESS;
                op_len = 2;
            } else {
                next_op = SHELL_CHAIN_ALWAYS;
                op_len = 1;
            }
        }

        if (count >= max_segments) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            return count;
        }

        /* Close the current segment at the operator and record it. */
        {
            char *tail = op_pos + op_len;

            *op_pos = '\0';
            segments[count].command = shell_trim(segment_start);
            segments[count].op = pending_op;
            count++;

            pending_op = next_op;
            segment_start = tail;
            cursor = tail;
        }
    }

    if (count >= max_segments) {
        if (truncated_out != NULL) {
            *truncated_out = true;
        }
        return count;
    }

    segments[count].command = shell_trim(segment_start);
    segments[count].op = pending_op;
    count++;

    return count;
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

    /* Interactive keypress queue used by pause, choice, and more. Created
     * before any input source starts so no keystroke can be lost. */
    if (s_key_queue == NULL) {
        s_key_queue = xQueueCreate(SHELL_KEY_QUEUE_DEPTH, sizeof(char));
        if (s_key_queue == NULL) {
            shell_record_errorf("shell", ESP_ERR_NO_MEM, "Failed to create keypress queue");
        }
    }
    s_key_wait_active = false;

    /* Restore the default DOS prompt template */
    shell_prompt_reset();

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

bool shell_is_initialized(void)
{
    return s_initialized;
}
