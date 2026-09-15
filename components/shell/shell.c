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
#include "header_refresh.h"
#include "keyboard.h"
#include "windows.h"
#include "clock.h"
#include "p4minishell_config.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <time.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_vfs_dev.h>
#include <fcntl.h>
#include <esp_lvgl_port.h>
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
#define SHELL_CLIPBOARD_BYTES           P4_CONFIG_CLIPBOARD_BYTES
#define SHELL_COMMAND_BYTES             P4_CONFIG_COMMAND_BYTES
#define SHELL_COMMAND_HISTORY_DEPTH     P4_CONFIG_COMMAND_HISTORY_DEPTH
#define SHELL_HISTORY_TOTAL_BYTES       P4_CONFIG_HISTORY_TOTAL_BYTES
#define SHELL_HEADER_REFRESH_PERIOD_MS  P4_CONFIG_HEADER_REFRESH_PERIOD_MS
#define SHELL_DEBUG_LOG_DEPTH           P4_CONFIG_DEBUG_LOG_DEPTH
#define SHELL_DEBUG_ENTRY_BYTES         P4_CONFIG_DEBUG_ENTRY_BYTES
#define SHELL_WIFI_SSID_BYTES           P4_CONFIG_WIFI_SSID_BYTES
#define SHELL_WIFI_PASSWORD_BYTES       P4_CONFIG_WIFI_PASSWORD_BYTES
#define SHELL_UART_CONSOLE_TASK_STACK_BYTES P4_CONFIG_UART_CONSOLE_TASK_STACK
#define SHELL_KEY_QUEUE_DEPTH           P4_CONFIG_KEY_QUEUE_DEPTH
/** Queue item: one key as a NUL-terminated UTF-8 sequence (4 bytes max). */
#define SHELL_KEY_SEQ_BYTES             5
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
 * BUILD IDENTITY
 * ======================================================================== */

/** One-line build identity: "P4MiniShell v0.31.0 | built <date> <time> | git <hash>". */
void shell_get_build_identity(char *buf, size_t size)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const char *git = (d != NULL) ? d->version : "n/a";
    const char *date = (d != NULL) ? d->date : "n/a";
    const char *time = (d != NULL) ? d->time : "n/a";

    if (buf == NULL || size == 0) {
        return;
    }
    snprintf(buf, size, "%s %s | built %s %s | git %s",
             P4_CONFIG_PRODUCT_NAME,
             P4_CONFIG_VERSION_STRING,
             date, time, git);
}

/** Full (multi-line) build identity for `version` / `about`. */
void shell_get_build_details(const esp_app_desc_t **desc_out)
{
    if (desc_out != NULL) {
        *desc_out = esp_app_get_description();
    }
}

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Transcript. Held in PSRAM (not internal SRAM) because the pair of 16 KB
 * buffers plus the clipboard dominate the small internal heap; keeping them
 * out of internal RAM preserves headroom for the WiFi/SDIO transport mempool
 * and the USB-Serial/JTAG ring buffers, which need DMA-capable memory. */
static char *s_transcript = NULL;
/* ANSI form of the transcript (real SGR escapes). Kept in parallel with the
 * plain s_transcript: the span-group transcript parses this into coloured
 * spans so on-screen colours match the UART console, while history/
 * redirection keep using the plain form. */
static char *s_transcript_ansi = NULL;

/* Cached lengths of the two transcript buffers. Recomputing them with
 * strlen() on every append scanned the whole 64 KB buffer, making appends
 * O(buffer) and long output loops quadratic. Every mutation site keeps these
 * in sync, and all reads use them instead of strlen(). */
static size_t s_transcript_len = 0;
static size_t s_transcript_ansi_len = 0;

/* Deferred label repaint batching (see shell_transcript_defer_begin()).
 * Repainting the LVGL span group costs O(buffer): each repaint re-parses the
 * whole ANSI scrollback and rebuilds every span, so per-line repaints make
 * big listings crawl (~1 KB/s at a full 64 KB buffer, measured). While the
 * depth is nonzero, appends mark the label dirty instead of repainting; the
 * serial mirror and the buffers stay live on every line. A time rule keeps
 * slow-printing commands (ping, progress) live on screen. Read/written from
 * the worker (segment begin/end) and the LVGL task (async flush); worst case
 * of a race is one redundant or skipped coalesced repaint, never corruption
 * (buffers are authoritative, the label is derived). */
/* Deferred label repaint batching (see shell_transcript_defer_begin()).
 * Repainting the LVGL span group costs O(buffer): each repaint re-parses the
 * whole ANSI scrollback and rebuilds every span, so per-line repaints make
 * big listings crawl (~1 KB/s at a full 64 KB buffer, measured). While the
 * depth is nonzero, appends mark the label dirty instead of repainting; the
 * serial mirror and the buffers stay live on every line. A time rule keeps
 * slow-printing commands (ping, progress) live on screen.
 *
 * Depth/dirty/bypass are PER WORKER TASK (main + `start` background pool):
 * two tasks interleaving segments must not consume each other's dirty flag,
 * or one task's output would never repaint. Unknown tasks (unit tests, init)
 * use slot 0. The flush timestamp stays global (one screen). */
typedef struct {
    int depth;
    bool dirty;
    /* Async drain bypass: background tasks (Wi-Fi, OTA progress) drain
     * through the same append entry points but must stay live even
     * mid-command, so the flush callback sets this around its appends and
     * the deferral stands down. Cross-task worst case is cosmetic (one
     * coalesced repaint). */
    bool bypass;
} shell_defer_slot_t;

static shell_defer_slot_t s_defer_slots[1 + P4_CONFIG_BG_TASKS];
/* Sized >= 1 even with the pool disabled (loop bounds still use the knob). */
static void *s_bg_tasks[(P4_CONFIG_BG_TASKS > 0) ? P4_CONFIG_BG_TASKS : 1];
static int64_t s_transcript_last_flush_us;

/* Set when a repaint was skipped because the transcript was hidden (gfx canvas
 * / TUI / app mode). The header poll repaints once when it becomes visible
 * again, so the skipped output is not lost. */
static bool s_transcript_repaint_pending;

static shell_defer_slot_t *shell_defer_current(void)
{
    void *me = (void *)xTaskGetCurrentTaskHandle();
    int i;

    if (me != NULL) {
        for (i = 0; i < P4_CONFIG_BG_TASKS; i++) {
            if (s_bg_tasks[i] == me) {
                return &s_defer_slots[1 + i];
            }
        }
    }
    return &s_defer_slots[0];
}

#define s_transcript_defer_depth  (shell_defer_current()->depth)
#define s_transcript_defer_dirty  (shell_defer_current()->dirty)
#define s_transcript_defer_bypass (shell_defer_current()->bypass)

void shell_register_bg_task(void *task)
{
    int i;

    if (task == NULL) {
        return;
    }
    for (i = 0; i < P4_CONFIG_BG_TASKS; i++) {
        if (s_bg_tasks[i] == NULL) {
            s_bg_tasks[i] = task;
            s_defer_slots[1 + i].depth = 0;
            s_defer_slots[1 + i].dirty = false;
            s_defer_slots[1 + i].bypass = false;
            return;
        }
    }
}

void shell_unregister_bg_task(void *task)
{
    int i;

    if (task == NULL) {
        return;
    }
    for (i = 0; i < P4_CONFIG_BG_TASKS; i++) {
        if (s_bg_tasks[i] == task) {
            s_bg_tasks[i] = NULL;
            s_defer_slots[1 + i].depth = 0;
            s_defer_slots[1 + i].dirty = false;
            s_defer_slots[1 + i].bypass = false;
            return;
        }
    }
}

bool shell_is_background_task(void)
{
    void *me = (void *)xTaskGetCurrentTaskHandle();
    int i;

    if (me == NULL) {
        return false;
    }
    for (i = 0; i < P4_CONFIG_BG_TASKS; i++) {
        if (s_bg_tasks[i] == me) {
            return true;
        }
    }
    return false;
}
/* Guards the plain/ANSI scrollback buffers (NOT the LVGL widgets: those stay
 * under the LVGL port lock). Lets worker appends proceed without serializing
 * behind LVGL render passes, which hold the port lock O(buffer) at a full
 * scrollback. Strict order everywhere: LVGL port lock outer, buffer lock
 * inner; buffer-only paths never take the port lock. Recursive. */
static SemaphoreHandle_t s_transcript_buf_lock = NULL;

static bool shell_transcript_buf_lock(void)
{
    if (s_transcript_buf_lock == NULL) {
        s_transcript_buf_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_transcript_buf_lock == NULL) {
        return false;
    }
    xSemaphoreTakeRecursive(s_transcript_buf_lock, portMAX_DELAY);
    return true;
}

static void shell_transcript_buf_unlock(void)
{
    if (s_transcript_buf_lock != NULL) {
        xSemaphoreGiveRecursive(s_transcript_buf_lock);
    }
}

/* RAM clipboard backing the `clip` / `paste` commands. */
static char *s_clipboard = NULL;
static bool s_clipboard_is_file;
static char s_async_transcript[SHELL_ASYNC_TRANSCRIPT_BYTES];
static size_t s_async_transcript_len;
static bool s_async_transcript_flush_queued;
static portMUX_TYPE s_async_transcript_lock = portMUX_INITIALIZER_UNLOCKED;

/* Output-redirection capture. When a command is being redirected, its output
 * is mirrored into this dedicated heap buffer (bounded by
 * P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES) so the redirected file holds the
 * command's FULL output even when the 16 KB transcript truncates. Owned by
 * the shell; the command module opens/closes the window around dispatch. */
static char *s_redirect_capture = NULL;
static size_t s_redirect_capture_len = 0;
static size_t s_redirect_capture_cap = 0;
static bool s_redirect_capturing = false;
static bool s_redirect_capture_truncated = false;

/* Used by shell_transcript_append_to_buffer() before its definition below. */
static void shell_redirect_capture_add(const char *text, size_t len);

/**
 * One saved redirection-capture level. When a redirected command runs another
 * redirected command (a pipeline whose stages spool to their own files inside
 * an outer `>` / `>>`), `shell_redirect_capture_begin()` pushes the current
 * capture onto this stack and starts a fresh one; the inner `_reset()` pops
 * the outer state back so the outer file still receives the command's full
 * output. Without the stack, the inner stage's reset freed the buffer the
 * outer capture depended on, so `cmd1 | cmd2 > out.txt` wrote an empty file.
 */
typedef struct {
    char *buffer;
    size_t len;
    size_t cap;
    bool truncated;
} shell_redirect_capture_level_t;

static shell_redirect_capture_level_t s_redirect_capture_stack[P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH];
static int s_capture_depth = 0;

/* Command history, heap-backed so very long (up to P4_CONFIG_COMMAND_BYTES)
 * commands do not reserve a fixed grid of RAM. Each entry is a strdup'd line;
 * the pointer table and total byte usage are bounded by
 * SHELL_COMMAND_HISTORY_DEPTH and SHELL_HISTORY_TOTAL_BYTES. */
static char **s_command_history;
static size_t s_command_history_count;
static size_t s_command_history_bytes;
static size_t s_command_history_generation;
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
static TaskHandle_t s_uart_console_task_handle;
/* Timestamp (us) of the first "disconnected" observation in the current streak;
 * 0 while the SOF monitor reports connected. Used to tolerate transient false
 * disconnects without blocking on-device when no host is attached. */
static int64_t s_mirror_disconnected_since_us;

/* USB-Serial/JTAG driver ring sizes. The default console ring is tiny
 * (256 bytes) and silently drops input whenever a host bursts faster than the
 * reader drains it; these larger rings make serial input (including the
 * `receive` binary transfer) lossless. Sizes are kept modest because the rings
 * live in internal RAM, which the WiFi/SDIO transport mempool also needs. */
#define SHELL_USJ_RX_BUFFER_BYTES 8192
#define SHELL_USJ_TX_BUFFER_BYTES 4096

/* Serializes command submissions coming from the serial console */
static SemaphoreHandle_t s_shell_command_lock;

/* Interactive keypress wait: queue fed by every input source */
static QueueHandle_t s_key_queue;
static volatile bool s_key_wait_active;
/* Batch-file execution flag, maintained by the batch engine (see shell.h). */
static bool s_batch_active;
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
 * Format a value wrapped in a colour spec into a caller buffer, converting
 * the `@`-specifiers to real SGR escapes. The result is safe to hand to
 * shell_transcript_appendf_ansi() as a %s argument: ansi_vformat() will not
 * touch the already-converted bytes, ansi_strip_to_plain() (used for the LVGL
 * transcript) strips the real escapes back to plain text, and the UART console
 * renders the escapes as colour. Without this helper, callers that pass a
 * coloured value as a %s argument would emit the literal "@G...@R" markers.
 *
 * @param buf       Caller-provided output buffer.
 * @param buf_size  Size of @p buf.
 * @param colour    Palette macro for the value (e.g. SH_OK).
 * @param value     Plain text to colour.
 * @return Pointer to @p buf.
 */
static const char *shell_colour_value(char *buf, size_t buf_size, const char *colour, const char *value)
{
    char spec[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (buf == NULL || buf_size == 0) {
        return buf;
    }

    snprintf(spec, sizeof(spec), "%s%s%s", colour, value != NULL ? value : "", SH_RST);
    ansi_format(buf, buf_size, spec);
    return buf;
}

/**
 * Append to the plain transcript buffer with tail-keeping / truncation, and
 * mirror the same tail-cut into the ANSI transcript buffer so the two stay
 * aligned for the label render.
 */
static void shell_transcript_append_to_buffer(char *plain, size_t plain_size,
                                               size_t *plain_len_io,
                                               char *ansi, size_t ansi_size,
                                               size_t *ansi_len_io,
                                               const char *plain_text,
                                               const char *ansi_text,
                                               const char *truncation_marker)
{
    size_t plain_len;
    size_t ansi_len;
    size_t plain_text_len;
    size_t ansi_text_len;

    if (plain_text == NULL || plain_text[0] == '\0' || ansi_text == NULL) {
        return;
    }

    /* Lengths are tracked by the caller (see s_transcript_len), so appends no
     * longer scan the whole buffer. Fall back to strlen for a NULL pointer. */
    plain_len = (plain_len_io != NULL) ? *plain_len_io : strlen(plain);
    ansi_len = (ansi_len_io != NULL) ? *ansi_len_io : strlen(ansi);
    plain_text_len = strlen(plain_text);
    ansi_text_len = strlen(ansi_text);

    /* A single append larger than the whole buffer keeps only its tail. */
    if (plain_text_len >= plain_size) {
        plain_text += plain_text_len - (plain_size - 1);
        plain_text_len = strlen(plain_text);
        ansi_text += ansi_text_len - (ansi_size - 1);
        ansi_text_len = strlen(ansi_text);
        plain[0] = '\0';
        plain_len = 0;
        ansi[0] = '\0';
        ansi_len = 0;
    }

    /* Drop the oldest half and mark the cut. */
    if (plain_len + plain_text_len + 1 >= plain_size) {
        size_t keep = plain_size / 2;

        if (plain_len > keep) {
            memmove(plain, plain + plain_len - keep, keep);
            plain_len = keep;
            plain[plain_len] = '\0';
        }
        if (ansi_len > keep) {
            memmove(ansi, ansi + ansi_len - keep, keep);
            ansi_len = keep;
            ansi[ansi_len] = '\0';
        }

        if (plain_len + strlen(truncation_marker) < plain_size) {
            size_t marker_len = strlen(truncation_marker);
            memcpy(plain + plain_len, truncation_marker, marker_len);
            plain_len += marker_len;
            plain[plain_len] = '\0';
            if (ansi_len + marker_len < ansi_size) {
                memcpy(ansi + ansi_len, truncation_marker, marker_len);
                ansi_len += marker_len;
                ansi[ansi_len] = '\0';
            }
        }
    }

    if (plain_len + plain_text_len + 1 >= plain_size) {
        plain_text_len = plain_size - plain_len - 1;
    }
    memcpy(plain + plain_len, plain_text, plain_text_len);
    plain[plain_len + plain_text_len] = '\0';

    /* Mirror the newly-appended plain text into an active output-redirection
     * capture, so a redirected file holds the command's full output even when
     * the transcript itself truncates. */
    shell_redirect_capture_add(plain_text, plain_text_len);

    if (ansi_len + ansi_text_len + 1 >= ansi_size) {
        ansi_text_len = ansi_size - ansi_len - 1;
    }
    memcpy(ansi + ansi_len, ansi_text, ansi_text_len);
    ansi[ansi_len + ansi_text_len] = '\0';

    /* Publish the new lengths so the next append needs no strlen scan. */
    if (plain_len_io != NULL) {
        *plain_len_io = plain_len + plain_text_len;
    }
    if (ansi_len_io != NULL) {
        *ansi_len_io = ansi_len + ansi_text_len;
    }
}

/**
 * Repaint the transcript label from the ANSI transcript buffer, converting
 * the real SGR escapes into LVGL recolor markup so colours render on screen.
 * Caller must hold the LVGL port lock.
 */
static void shell_transcript_update_label(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    if (transcript == NULL) {
        return;
    }

    /* While the transcript is hidden (a gfx canvas, TUI, or full-screen app
     * covers it), skip the O(transcript) staging copy + span rebuild: nothing
     * is visible, and doing it per batch line is what made batch/canvas apps
     * slow down as the transcript grew (each line copied the whole up-to-64 KB
     * buffer). Remember to repaint once when it becomes visible again. */
    if (windows_transcript_is_hidden()) {
        s_transcript_repaint_pending = true;
        return;
    }
    s_transcript_repaint_pending = false;

    /* The transcript is an LVGL label with recolor enabled. windows_set_
     * transcript_text() converts the ANSI buffer to recolor markup and defers
     * the actual widget update (set text, layout, scroll) to the LVGL task via
     * a coalesced lv_async_call, so no label work races the render cycle from
     * a non-LVGL task. Called under lvgl_port lock from
     * shell_transcript_append_internal. */
    windows_set_transcript_text_len(s_transcript_ansi, s_transcript_ansi_len);
}

/**
 * Repaint the transcript once it is visible again after a hidden-skip. Called
 * from the header poll (LVGL task) so an app that hides the transcript and
 * prints nothing on exit still restores it.
 */
static void shell_transcript_repaint_if_pending(void)
{
    if (!s_transcript_repaint_pending || windows_transcript_is_hidden()) {
        return;
    }
    s_transcript_defer_dirty = true;
    shell_transcript_flush_now();
}

void shell_transcript_defer_begin(void)
{
    s_transcript_defer_depth++;
}

void shell_transcript_flush_now(void)
{
    if (!s_transcript_defer_dirty) {
        return;
    }
    s_transcript_defer_dirty = false;
    s_transcript_last_flush_us = esp_timer_get_time();
    /* update_label() requires the port lock; none of the explicit flush
     * points (segment end, key waits, screenshots) holds it, and the mutex
     * is recursive so the time-rule path (already locked) nests safely.
     * Strict order PORT outer, buffer inner: the staging read below races
     * worker appends otherwise. */
    if (windows_get_transcript() != NULL) {
        lvgl_port_lock(0);
        shell_transcript_buf_lock();
        shell_transcript_update_label();
        shell_transcript_buf_unlock();
        lvgl_port_unlock();
    }
}

void shell_transcript_defer_end(void)
{
    if (s_transcript_defer_depth > 0) {
        s_transcript_defer_depth--;
    }
    if (s_transcript_defer_depth == 0) {
        shell_transcript_flush_now();
    }
}

/**
 * Label-update entry point for transcript appends. Outside a deferral window
 * this repaints immediately (historical behavior); inside one it marks the
 * label dirty and repaints at most every P4_CONFIG_TRANSCRIPT_FLUSH_MS, so a
 * burst of lines costs O(1) span rebuilds instead of O(lines) while staying
 * live for slow printers. Takes both locks itself (PORT outer, buffer inner:
 * the staging read races worker appends otherwise); append paths hold none.
 */
static void shell_transcript_maybe_update_label(void)
{
    bool immediate = (s_transcript_defer_depth == 0 || s_transcript_defer_bypass);

    if (!immediate) {
        s_transcript_defer_dirty = true;
        if (esp_timer_get_time() - s_transcript_last_flush_us <
            (int64_t)P4_CONFIG_TRANSCRIPT_FLUSH_MS * 1000) {
            return;
        }
        /* Time rule fired: repaint below; dirty stays set so the segment end
         * still reconciles anything appended after this repaint. */
    }
    if (windows_get_transcript() != NULL) {
        lvgl_port_lock(0);
        shell_transcript_buf_lock();
        shell_transcript_update_label();
        shell_transcript_buf_unlock();
        lvgl_port_unlock();
    }
    s_transcript_last_flush_us = esp_timer_get_time();
}

/**
 * Append text to the transcript buffer and repaint the LVGL label.
 *
 * @param text           Text to append (already plain, no ANSI sequences).
 * @param mirror_to_uart When true, the same text is echoed to the serial
 *                       console. ANSI callers pass false because they emit the
 *                       raw escape-sequence form to the terminal themselves.
 */
static void shell_transcript_append_internal(const char *text, bool mirror_to_uart)
{
    static const char truncation_marker[] = "\n[history truncated]\n";

    if (text == NULL || text[0] == '\0') {
        return;
    }

    /* A single command with a large output can grow the span group fast
     * enough to exhaust the internal heap mid-command. Reclaim scrollback
     * when the internal heap drops below the threshold (rate-limited, keeps
     * the newest three quarters), so a tiny stdio allocation never aborts
     * the board while output is still being emitted. Cheap when memory is
     * fine (a single free-size query plus a timestamp check). Takes its own
     * locks (LVGL port outer, transcript buffer inner). */
    shell_transcript_guard_internal();

    /* Serialize buffer appends (worker, LVGL task via the async drain,
     * console task) against each other with the lightweight buffer mutex —
     * deliberately NOT the LVGL port lock, which the render task can hold
     * O(buffer) per frame at a full scrollback. Widget/label work happens
     * below in the deferred label step, which takes the port lock itself. */
    shell_transcript_buf_lock();
    shell_transcript_append_to_buffer(s_transcript, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_len,
                                      s_transcript_ansi, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_ansi_len,
                                      text, text, truncation_marker);
    shell_transcript_buf_unlock();

    /* Mirror plain transcript output to the serial console so idf.py monitor
     * stays a first-class shell endpoint. Outside both locks: UART has its
     * own lock, and pacing serial bytes on LVGL work is exactly the stall
     * this discipline removes. */
    if (mirror_to_uart) {
        shell_uart_console_write_text(text);
    }

    shell_transcript_maybe_update_label();

    /* Update anchor position tracking for click regions */
    extern void shell_transcript_update_anchor_position(const char *text);
    shell_transcript_update_anchor_position(text);
}

void shell_transcript_append_text(const char *text)
{
    shell_transcript_append_internal(text, true);
}

void shell_transcript_appendf(const char *format, ...)
{
    /* Command-sized because callers (notably `echo`) format an entire
     * interactive command line, which can be up to P4_CONFIG_COMMAND_BYTES.
     * Heap-allocated: this runs on the command worker task, and a fixed
     * 512-byte stack buffer silently truncates long output. */
    char *buffer = malloc(P4_CONFIG_COMMAND_BYTES);
    va_list args;

    if (format == NULL) {
        free(buffer);
        return;
    }

    if (buffer == NULL) {
        return;
    }

    va_start(args, format);
    vsnprintf(buffer, P4_CONFIG_COMMAND_BYTES, format, args);
    va_end(args);

    shell_transcript_append_text(buffer);
    free(buffer);
}

void shell_transcript_append_ansi(const char *text)
{
    char plain[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (text == NULL) {
        return;
    }

    /* The UART console passes the raw ANSI code through natively for real
     * terminals; the LVGL transcript renders it via recolor markup so colours
     * appear on screen too. s_transcript stays plain for history/redirection,
     * while s_transcript_ansi keeps the real escape sequences for the label. */
    ansi_strip_to_plain(plain, sizeof(plain), text);

    /* Buffer appends serialize on the lightweight buffer mutex (see
     * shell_transcript_append_internal); the label step takes the LVGL port
     * lock itself only when it actually repaints. */
    shell_transcript_buf_lock();
    shell_transcript_append_to_buffer(s_transcript, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_len,
                                      s_transcript_ansi, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_ansi_len,
                                      plain, text, "\n[history truncated]\n");
    shell_transcript_buf_unlock();

    shell_transcript_maybe_update_label();

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
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    char fmt[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (format == NULL) {
        return;
    }

    /* Compose the full format string with the colour prefix and reset suffix
     * literal, then render the whole thing through ansi_vformat so every
     * @-specifier (both the palette wrapper and any the caller embedded, e.g.
     * SH_LBL/SH_VAL in shell_print_field) is converted to real SGR escapes.
     * The caller's args go straight through ansi_vformat, so any '%s' value
     * containing '@' is treated as data (injection guard). */
    snprintf(fmt, sizeof(fmt), "%s%s%s%s", colour, format, SH_RST, newline ? "\n" : "");
    ansi_vformat(line, sizeof(line), fmt, args);
    shell_transcript_append_ansi(line);
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

    /* The SH_* macros expand to @-specifiers; render the whole line through
     * ansi_format so every specifier (label colour, value colour, resets) is
     * converted to real SGR escapes. label and value are substituted as %s
     * arguments so any literal '%' or '@' in them stays data. */
    const char *fmt = SH_LBL "%s" SH_RST " " SH_VAL "%s" SH_RST "\n";
    ansi_format(line, sizeof(line), fmt, label, value);
    shell_transcript_append_ansi(line);
}

void shell_print_field_num(const char *label, long value)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];

    if (label == NULL) {
        return;
    }

    /* The SH_* macros expand to @-specifiers; render the whole line through
     * ansi_format so every specifier is converted to real SGR escapes. label
     * is substituted as a %s argument so any literal '%' or '@' in it stays
     * data. */
    const char *fmt = SH_LBL "%s" SH_RST " " SH_NUM "%ld" SH_RST "\n";
    ansi_format(line, sizeof(line), fmt, label, value);
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

    if (queue_flush) {
        /* The flush callback touches LVGL (the transcript label). Before the
         * UI exists - unit tests, or very early boot - there is no widget, so
         * lv_async_call would allocate from an uninitialized LVGL heap. Flush
         * synchronously instead: the transcript buffers are RAM-backed and the
         * append path is NULL-safe, so the output still lands. */
        if (windows_get_transcript() == NULL) {
            shell_async_transcript_flush_cb(NULL);
        } else if (lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
            portENTER_CRITICAL(&s_async_transcript_lock);
            s_async_transcript_flush_queued = false;
            portEXIT_CRITICAL(&s_async_transcript_lock);
            ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
        }
    }
}

void shell_schedule_transcript_appendf_ansi(const char *format, ...)
{
    char buffer[512];
    va_list args;
    bool queue_flush = false;

    if (format == NULL) {
        return;
    }

    va_start(args, format);
    ansi_vformat(buffer, sizeof(buffer), format, args);
    va_end(args);

    portENTER_CRITICAL(&s_async_transcript_lock);
    shell_async_transcript_append_pending(buffer);
    if (!s_async_transcript_flush_queued && s_async_transcript_len > 0) {
        s_async_transcript_flush_queued = true;
        queue_flush = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (queue_flush) {
        if (windows_get_transcript() == NULL) {
            shell_async_transcript_flush_cb(NULL);
        } else if (lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
            portENTER_CRITICAL(&s_async_transcript_lock);
            s_async_transcript_flush_queued = false;
            portEXIT_CRITICAL(&s_async_transcript_lock);
            ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
        }
    }
}

static void shell_async_transcript_flush_cb(void *user_data)
{
    char *pending = NULL;
    bool requeue = false;

    (void)user_data;

    /* Drain the staging buffer under the critical section, then append
     * outside it so LVGL work never runs with interrupts masked. The
     * scratch copy is heap-allocated: it can be up to
     * SHELL_ASYNC_TRANSCRIPT_BYTES, which is far too large for the main task
     * stack when this callback runs synchronously before the UI exists, and
     * wasteful on the LVGL task stack in the normal async path. */
    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len == 0) {
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        return;
    }

    pending = malloc(s_async_transcript_len + 1);
    if (pending == NULL) {
        /* Cannot drain now; leave the staged text intact and drop the flush
         * request so the next append schedules a fresh flush. */
        s_async_transcript_flush_queued = false;
        portEXIT_CRITICAL(&s_async_transcript_lock);
        return;
    }
    memcpy(pending, s_async_transcript, s_async_transcript_len + 1);
    s_async_transcript_len = 0;
    s_async_transcript[0] = '\0';
    s_async_transcript_flush_queued = false;
    portEXIT_CRITICAL(&s_async_transcript_lock);

    /* Background output stays live even while a worker command defers its
     * own repaints (OTA progress, Wi-Fi events). */
    s_transcript_defer_bypass = true;
    if (ansi_contains_escapes(pending)) {
        shell_transcript_append_ansi(pending);
    } else {
        shell_transcript_append_text(pending);
    }
    s_transcript_defer_bypass = false;
    free(pending);
    shell_history_transcript_scroll_to_end();

    /* Producers may have staged more text while we were appending. */
    portENTER_CRITICAL(&s_async_transcript_lock);
    if (s_async_transcript_len > 0 && !s_async_transcript_flush_queued) {
        s_async_transcript_flush_queued = true;
        requeue = true;
    }
    portEXIT_CRITICAL(&s_async_transcript_lock);

    if (requeue) {
        if (windows_get_transcript() == NULL) {
            /* Still no UI: drain synchronously. The scratch is now heap, so
             * the recursion is stack-light; it terminates as soon as the
             * staging buffer empties. */
            shell_async_transcript_flush_cb(NULL);
        } else if (lv_async_call(shell_async_transcript_flush_cb, NULL) != LV_RESULT_OK) {
            portENTER_CRITICAL(&s_async_transcript_lock);
            s_async_transcript_flush_queued = false;
            portEXIT_CRITICAL(&s_async_transcript_lock);
            ESP_LOGW(SHELL_TAG, "Failed to queue transcript flush");
        }
    }
}

void shell_transcript_reset(void)
{
    lv_obj_t *transcript = windows_get_transcript();

    /* Lock order PORT outer, buffer inner (strict everywhere). */
    if (transcript != NULL) {
        lvgl_port_lock(0);
    }
    shell_transcript_buf_lock();
    s_transcript[0] = '\0';
    s_transcript_ansi[0] = '\0';
    s_transcript_len = 0;
    s_transcript_ansi_len = 0;
    /* Content was replaced, not appended: no pending repaint can be valid. */
    s_transcript_defer_dirty = false;
    shell_transcript_buf_unlock();

    if (transcript != NULL) {
        windows_set_transcript_text("");
        lvgl_port_unlock();
    }
}

/**
 * Reclaim internal heap from the transcript scrollback when the DMA-capable
 * heap runs low.
 *
 * The on-screen span group holds its coloured history as LVGL span objects
 * whose per-span overhead lives in the internal heap. Over a long session
 * that accumulation can starve the heap until a tiny stdio allocation (a
 * newlib FILE lock mutex created by printf) fails and aborts the board. When
 * free internal RAM drops below P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES,
 * drop the oldest half of the text buffers and free the corresponding spans
 * synchronously, keeping the heap above the failure floor.
 *
 * Called at the start of command execution (before any printf of that
 * command) so the reclaimed memory is available before output is emitted.
 */
/* Consecutive-trim rate limit: at most one trim per window, so a burst of
 * appends cannot trim on every line (each trim forces a span rebuild, which
 * is the visible flicker). */
#define SHELL_TRIM_MIN_INTERVAL_US   (2000000LL)
/* Severe-pressure floor: below this, trim harder to guarantee progress. */
#define SHELL_TRIM_SEVERE_BYTES      (8192)

static size_t s_transcript_trim_count = 0;
static int64_t s_transcript_last_trim_us = 0;

size_t shell_transcript_trim_count(void)
{
    return s_transcript_trim_count;
}

void shell_transcript_guard_internal(void)
{
    static const char truncation_marker[] = "\n[history trimmed under memory pressure]\n";
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t now_us = esp_timer_get_time();
    size_t plain_len;
    size_t ansi_len;
    size_t keep;

    if (free_internal >= P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES) {
        return;
    }

    /* Rate-limit: one trim per window is enough; the freed spans cover the
     * burst that follows. Without this, every append in a long listing
     * re-trims, re-logs, and re-rebuilds spans (the flicker + log spam). */
    if (now_us - s_transcript_last_trim_us < SHELL_TRIM_MIN_INTERVAL_US) {
        return;
    }
    s_transcript_last_trim_us = now_us;

    /* Lock order is LVGL-port outer, buffer inner (strict everywhere: flush,
     * reset, and this guard). The widget tail below needs the port lock;
     * buffer surgery needs the buffer lock. Callers hold no locks. */
    if (!lvgl_port_lock(0)) {
        return;
    }
    shell_transcript_buf_lock();

    plain_len = s_transcript_len;
    ansi_len = s_transcript_ansi_len;

    /* Gentle trim: keep the newest three quarters so a single trim rarely
     * repeats; only under severe pressure fall back to keeping half. */
    if (free_internal < SHELL_TRIM_SEVERE_BYTES) {
        keep = P4_CONFIG_TRANSCRIPT_BYTES / 2;
    } else {
        keep = (P4_CONFIG_TRANSCRIPT_BYTES * 3) / 4;
    }
    if (keep < 512) keep = 512;
    if (plain_len > keep) {
        memmove(s_transcript, s_transcript + plain_len - keep, keep);
        plain_len = keep;
        s_transcript[plain_len] = '\0';
    }
    if (ansi_len > keep) {
        memmove(s_transcript_ansi, s_transcript_ansi + ansi_len - keep, keep);
        ansi_len = keep;
        s_transcript_ansi[ansi_len] = '\0';
    }

    if (plain_len + sizeof(truncation_marker) < P4_CONFIG_TRANSCRIPT_BYTES) {
        size_t marker_len = sizeof(truncation_marker) - 1;
        memcpy(s_transcript + plain_len, truncation_marker, marker_len);
        plain_len += marker_len;
        s_transcript[plain_len] = '\0';
        if (ansi_len + marker_len < P4_CONFIG_TRANSCRIPT_BYTES) {
            memcpy(s_transcript_ansi + ansi_len, truncation_marker, marker_len);
            ansi_len += marker_len;
            s_transcript_ansi[ansi_len] = '\0';
        }
    }
    s_transcript_len = plain_len;
    s_transcript_ansi_len = ansi_len;

    /* Sync the staging buffer to the newly-trimmed ANSI transcript BEFORE
     * trimming spans. Otherwise windows_transcript_trim() works on stale
     * content, and the subsequent shell_transcript_update_label() overwrites
     * it again, causing a full span rebuild (blue flash). */
    windows_set_transcript_text_len(s_transcript_ansi, s_transcript_ansi_len);
    windows_transcript_trim();
    shell_transcript_buf_unlock();
    lvgl_port_unlock();

    s_transcript_trim_count++;
    ESP_LOGW(SHELL_TAG, "Transcript trimmed under memory pressure "
             "(internal free %u B, trims %u)", (unsigned int)free_internal,
             (unsigned int)s_transcript_trim_count);
}

void shell_history_transcript_scroll_to_end(void)
{
    /* The transcript label is owned by the LVGL task; the window manager
     * scrolls it directly under lvgl_port_lock. */
    windows_scroll_transcript_to_end();
}

void shell_force_transcript_scroll_to_end(void)
{
    /* Command submission: jump to the newest output even if the user was
     * reading earlier history. Deferred to the LVGL task by the window
     * manager's coalesced repaint. */
    windows_force_scroll_transcript_to_end();
}

size_t shell_transcript_get_length(void)
{
    return s_transcript_len;
}

const char *shell_transcript_get_text_from(size_t offset)
{
    size_t length = s_transcript_len;

    if (offset > length) {
        return NULL;
    }

    return s_transcript + offset;
}

size_t shell_transcript_get_ansi_length(void)
{
    return s_transcript_ansi_len;
}

const char *shell_transcript_get_ansi_from(size_t offset)
{
    size_t length = s_transcript_ansi_len;

    if (offset > length) {
        return NULL;
    }
    return s_transcript_ansi + offset;
}

/* ========================================================================
 * APP MODE (screen save/restore + full-screen surface)
 * ========================================================================
 * A clean way for batch files (the `appmode` command) and native apps (the
 * applib `app_mode_enter`/`app_mode_exit`) to take over the shell: the current
 * transcript is saved, the shell input widgets can be hidden for a full-screen
 * app surface, and on exit the saved screen is restored. The implementation
 * lives here (below both batch and applib) so nothing is duplicated.
 */

static bool s_app_mode_full_screen = false;

/** Save the current transcript (ANSI form, colours preserved) into a heap
 *  buffer. Returns NULL when the transcript is empty or on allocation failure. */
char *shell_screen_save(void)
{
    size_t length;
    char *copy;

    /* Snapshot atomically: an async append mid-copy would tear the text. */
    shell_transcript_buf_lock();
    length = shell_transcript_get_ansi_length();
    if (length == 0) {
        shell_transcript_buf_unlock();
        return NULL;
    }
    copy = malloc(length + 1);
    if (copy == NULL) {
        shell_transcript_buf_unlock();
        return NULL;
    }
    memcpy(copy, shell_transcript_get_ansi_from(0), length + 1);
    shell_transcript_buf_unlock();
    return copy;
}

/** Clear the transcript and re-append the saved screen (no-op on NULL). */
void shell_screen_restore(const char *saved)
{
    shell_transcript_reset();
    if (saved != NULL && saved[0] != '\0') {
        shell_transcript_append_ansi(saved);
    }
}

/** Release a screen copy from `shell_screen_save`. */
void shell_screen_discard(char *saved)
{
    free(saved);
}

/**
 * Enter app mode. With @p full_screen the shell input widgets (input line and
 * the prev/next/scroll buttons) are hidden so the transcript becomes a clean
 * app surface; the on-screen keyboard can still be shown for app input. The
 * caller saves the screen first with `shell_screen_save`.
 */
void shell_app_mode_enter(bool full_screen)
{
    /* Background workers share the one screen: entering app mode here
     * would hide the main task's widgets, so refuse silently (the batch
     * `appmode` verb reports it; native applib apps just render inline). */
    if (shell_is_background_task()) {
        return;
    }
    if (s_app_mode_full_screen) {
        return;
    }
    if (full_screen) {
        /* Only touch LVGL when the window manager is up (transcript is
         * created during windows_init); before that the flag just tracks
         * that app mode was entered. */
        if (windows_get_transcript() != NULL) {
            lvgl_port_lock(0);
            windows_enter_app_mode();
            lvgl_port_unlock();
        }
    }
    s_app_mode_full_screen = true;
}

/** Leave app mode and restore the shell input widgets. */
void shell_app_mode_exit(void)
{
    if (!s_app_mode_full_screen) {
        return;
    }
    if (windows_get_transcript() != NULL) {
        lvgl_port_lock(0);
        windows_exit_app_mode();
        lvgl_port_unlock();
    }
    s_app_mode_full_screen = false;
}

/** Whether the shell is currently in (full-screen) app mode. */
bool shell_app_mode_active(void)
{
    return s_app_mode_full_screen;
}

/* ========================================================================
 * OUTPUT-REDIRECTION CAPTURE
 * ======================================================================== */

/** Append @p len bytes of @p text to the active redirection capture. */
static void shell_redirect_capture_add(const char *text, size_t len)
{
    size_t new_len;

    if (!s_redirect_capturing || text == NULL || len == 0) {
        return;
    }

    /* Bound the capture: drop anything past the cap and remember it so the
     * caller can warn. */
    if (s_redirect_capture_len + len > P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES) {
        len = P4_CONFIG_REDIRECT_CAPTURE_MAX_BYTES - s_redirect_capture_len;
        s_redirect_capture_truncated = true;
        if (len == 0) {
            return;
        }
    }

    new_len = s_redirect_capture_len + len;
    if (new_len + 1 > s_redirect_capture_cap) {
        size_t new_cap = s_redirect_capture_cap == 0 ? 1024 : s_redirect_capture_cap;
        char *nb;

        while (new_cap < new_len + 1) {
            new_cap *= 2;
        }
        nb = realloc(s_redirect_capture, new_cap);
        if (nb == NULL) {
            /* Out of memory: stop capturing so the command keeps working. */
            s_redirect_capturing = false;
            return;
        }
        s_redirect_capture = nb;
        s_redirect_capture_cap = new_cap;
    }

    memcpy(s_redirect_capture + s_redirect_capture_len, text, len);
    s_redirect_capture_len = new_len;
    s_redirect_capture[s_redirect_capture_len] = '\0';
}

/** Open the redirection-capture window (call before dispatching a redirected
 *  command). When a capture is already active (nested redirect), the current
 *  capture is pushed onto the stack so the outer file still gets the whole
 *  output after the inner captures finish. */
void shell_redirect_capture_begin(void)
{
    if (s_redirect_capturing) {
        if (s_capture_depth < P4_CONFIG_REDIRECT_CAPTURE_MAX_DEPTH) {
            s_redirect_capture_stack[s_capture_depth].buffer = s_redirect_capture;
            s_redirect_capture_stack[s_capture_depth].len = s_redirect_capture_len;
            s_redirect_capture_stack[s_capture_depth].cap = s_redirect_capture_cap;
            s_redirect_capture_stack[s_capture_depth].truncated = s_redirect_capture_truncated;
            s_capture_depth++;
        } else {
            /* Depth exhausted: drop the current capture so the command keeps
             * working; the outer file loses its content but nothing breaks. */
            free(s_redirect_capture);
        }
    } else {
        free(s_redirect_capture);
    }

    /* Start a fresh capture for this (possibly nested) command. The pushed
     * outer state is restored by the matching shell_redirect_capture_reset(). */
    s_redirect_capture = NULL;
    s_redirect_capture_len = 0;
    s_redirect_capture_cap = 0;
    s_redirect_capture_truncated = false;
    s_redirect_capturing = true;
}

/** Close the redirection-capture window. */
void shell_redirect_capture_end(void)
{
    s_redirect_capturing = false;
}

/** Return the captured output (NUL-terminated) and its length. The buffer
 *  stays valid until the next shell_redirect_capture_begin()/reset(). */
const char *shell_redirect_capture_get(size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = s_redirect_capture_len;
    }
    return s_redirect_capture != NULL ? s_redirect_capture : "";
}

/** Release the capture buffer (call after the redirected file is written).
 *  If a nested capture is being closed, the saved outer capture is restored
 *  and recording resumes for it. */
void shell_redirect_capture_reset(void)
{
    if (s_capture_depth > 0) {
        /* Pop the saved outer capture back into place and resume recording
         * for it; the inner capture's buffer has already been written out. */
        s_capture_depth--;
        free(s_redirect_capture);
        s_redirect_capture = s_redirect_capture_stack[s_capture_depth].buffer;
        s_redirect_capture_len = s_redirect_capture_stack[s_capture_depth].len;
        s_redirect_capture_cap = s_redirect_capture_stack[s_capture_depth].cap;
        s_redirect_capture_truncated = s_redirect_capture_stack[s_capture_depth].truncated;
        s_redirect_capture_stack[s_capture_depth].buffer = NULL;
        s_redirect_capturing = true;
    } else {
        s_redirect_capturing = false;
        free(s_redirect_capture);
        s_redirect_capture = NULL;
        s_redirect_capture_len = 0;
        s_redirect_capture_cap = 0;
        s_redirect_capture_truncated = false;
    }
}

/** Report whether the capture hit its size cap (output dropped). */
bool shell_redirect_capture_was_truncated(void)
{
    return s_redirect_capture_truncated;
}

/* ========================================================================
 * COMMAND HISTORY
 * ======================================================================== */

/**
 * Detect `wifi connect <ssid> <password>` and `crypt ... /p:<password>`,
 * which must never reach the transcript or the recall buffer in clear text.
 */
static bool shell_command_is_sensitive(const char *command)
{
    char *buffer = malloc(SHELL_COMMAND_BYTES);
    char *argv[8];
    int argc;
    int i;
    bool sensitive = false;

    if (buffer == NULL) {
        return false;
    }

    snprintf(buffer, SHELL_COMMAND_BYTES, "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 8);
    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        sensitive = true;
    }
    if (argc >= 2 && strcmp(argv[0], "crypt") == 0) {
        for (i = 1; i < argc; i++) {
            if (strncasecmp(argv[i], "/p:", 3) == 0) {
                sensitive = true;
                break;
            }
        }
    }
    free(buffer);
    return sensitive;
}

/** Overwrite the value of a `/p:<password>` token with asterisks (in place,
 *  length-preserving so history byte accounting is unaffected). */
static void shell_mask_inline_password(char *command)
{
    char *at = command;

    while (*at != '\0') {
        if ((at[0] == '/' || at[0] == '-') &&
            (at[1] == 'p' || at[1] == 'P') && at[2] == ':') {
            char *value = at + 3;
            while (*value != '\0' && *value != ' ' && *value != '\t') {
                *value++ = '*';
            }
            at = value;
        } else {
            at++;
        }
    }
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
    char *buffer = malloc(SHELL_COMMAND_BYTES);
    char *argv[4];
    int argc;

    if (output == NULL || output_size == 0) {
        free(buffer);
        return;
    }
    if (buffer == NULL) {
        snprintf(output, output_size, "%s", command != NULL ? command : "");
        return;
    }

    snprintf(buffer, SHELL_COMMAND_BYTES, "%s", command != NULL ? command : "");
    argc = shell_split_args(buffer, argv, 4);

    if (argc >= 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) {
        snprintf(output, output_size, "wifi connect %s ********", argv[2]);
        free(buffer);
        return;
    }

    if (argc >= 2 && strcmp(argv[0], "crypt") == 0) {
        snprintf(output, output_size, "%s", command != NULL ? command : "");
        shell_mask_inline_password(output);
        free(buffer);
        return;
    }

    snprintf(output, output_size, "%s", command != NULL ? command : "");
    free(buffer);
}

/** Drop the oldest history entry and slide the rest down (pointer array). */
static void shell_history_drop_oldest(void)
{
    size_t index;

    if (s_command_history_count == 0) {
        return;
    }
    s_command_history_bytes -= strlen(s_command_history[0]) + 1;
    free(s_command_history[0]);
    for (index = 1; index < s_command_history_count; index++) {
        s_command_history[index - 1] = s_command_history[index];
    }
    s_command_history_count--;
}

void shell_store_command_history(const char *command)
{
    char *masked = NULL;
    const char *entry;
    size_t entry_bytes;

    if (command == NULL || command[0] == '\0') {
        return;
    }

    /* Defence in depth: callers are expected to filter sensitive commands
     * via shell_command_should_store_history(), but mask here as well so no
     * code path can ever persist a Wi-Fi password in the recall buffer. */
    if (shell_command_is_sensitive(command)) {
        masked = malloc(SHELL_COMMAND_BYTES);
        if (masked == NULL) {
            return;
        }
        shell_format_command_for_transcript(command, masked, SHELL_COMMAND_BYTES);
        entry = masked;
    } else {
        entry = command;
    }

    /* Skip consecutive duplicates so repeated Enter presses do not flood the
     * recall buffer. */
    if (s_command_history_count > 0 &&
        strcmp(s_command_history[s_command_history_count - 1], entry) == 0) {
        free(masked);
        return;
    }

    /* Make sure the pointer table exists. */
    if (s_command_history == NULL) {
        s_command_history = calloc(SHELL_COMMAND_HISTORY_DEPTH, sizeof(char *));
        if (s_command_history == NULL) {
            free(masked);
            return;
        }
    }

    /* Make room for the new entry: depth cap first, then the byte cap. */
    while (s_command_history_count >= SHELL_COMMAND_HISTORY_DEPTH) {
        shell_history_drop_oldest();
    }
    entry_bytes = strlen(entry) + 1;
    while (s_command_history_count > 0 &&
           s_command_history_bytes + entry_bytes > SHELL_HISTORY_TOTAL_BYTES) {
        shell_history_drop_oldest();
    }

    s_command_history[s_command_history_count] = strdup(entry);
    if (s_command_history[s_command_history_count] != NULL) {
        s_command_history_bytes += entry_bytes;
        s_command_history_count++;
        s_command_history_generation++;
    }

    free(masked);
}

void shell_recall_history(int direction)
{
    char *current_input = malloc(SHELL_COMMAND_BYTES);

    if (s_command_history_count == 0 || current_input == NULL) {
        free(current_input);
        return;
    }

    shell_extract_input_text(current_input, SHELL_COMMAND_BYTES);

    if (s_command_history_cursor < 0) {
        /* Entering recall: remember what the user was typing. */
        snprintf(s_history_draft, sizeof(s_history_draft), "%s", current_input);
        if (direction < 0) {
            s_command_history_cursor = (int)s_command_history_count - 1;
        } else {
            free(current_input);
            return;
        }
    } else {
        s_command_history_cursor += direction;
        if (s_command_history_cursor < 0 ||
            s_command_history_cursor >= (int)s_command_history_count) {
            /* Walked off either end: restore the saved draft. */
            s_command_history_cursor = -1;
            shell_input_line_set_text(s_history_draft);
            free(current_input);
            return;
        }
    }

    shell_input_line_set_text(s_command_history[s_command_history_cursor]);
    free(current_input);
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

size_t shell_history_get_count(void)
{
    return s_command_history_count;
}

const char *shell_history_get(size_t index)
{
    if (index >= s_command_history_count || s_command_history == NULL) {
        return NULL;
    }
    return s_command_history[index];
}

size_t shell_history_generation(void)
{
    return s_command_history_generation;
}

void shell_history_clear(void)
{
    size_t index;

    for (index = 0; index < s_command_history_count; index++) {
        free(s_command_history[index]);
    }
    free(s_command_history);
    s_command_history = NULL;
    s_command_history_count = 0;
    s_command_history_bytes = 0;
    s_command_history_cursor = -1;
    s_history_draft[0] = '\0';
}

bool shell_history_save_lines(FILE *fp)
{
    size_t index;

    if (fp == NULL) {
        return false;
    }
    for (index = 0; index < shell_history_get_count(); index++) {
        const char *line = shell_history_get(index);

        if (line != NULL && fprintf(fp, "%s\n", line) < 0) {
            return false;
        }
    }
    return true;
}

size_t shell_history_load_lines(FILE *fp)
{
    // Command-sized and heap-allocated: `history /load` can run from a
    // nested batch line, so a stack buffer would eat into the worker stack.
    char *line = malloc(SHELL_COMMAND_BYTES);
    size_t loaded = 0;

    if (fp == NULL || line == NULL) {
        free(line);
        return 0;
    }
    while (fgets(line, SHELL_COMMAND_BYTES, fp) != NULL) {
        shell_trim(line);
        if (line[0] == '\0') {
            continue;
        }
        shell_store_command_history(line);
        loaded++;
    }
    free(line);
    return loaded;
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
    char *buffer = malloc(SHELL_PROMPT_RENDER_BYTES + SHELL_COMMAND_BYTES);
    const char *safe_command = command_text != NULL ? command_text : "";
    lv_obj_t *input_line = windows_get_input_line();

    if (input_line == NULL) {
        free(buffer);
        return;
    }
    if (buffer == NULL) {
        return;
    }

    /* The input line is LVGL-backed state; serialize with the render cycle
     * the same way the transcript appends do. The mutex is recursive, so the
     * LVGL event path (which already holds it) nests without deadlock. */
    lvgl_port_lock(0);

    /* Snapshot the prefix actually painted so extraction stays exact. */
    shell_input_line_render_prompt(s_input_line_prompt, sizeof(s_input_line_prompt));

    snprintf(buffer, SHELL_PROMPT_RENDER_BYTES + SHELL_COMMAND_BYTES,
             "%s%s", s_input_line_prompt, safe_command);
    lv_textarea_set_text(input_line, buffer);
    lv_textarea_set_cursor_pos(input_line, LV_TEXTAREA_CURSOR_LAST);
    lvgl_port_unlock();
    free(buffer);
}

void shell_input_line_reset(void)
{
    shell_input_line_set_text("");
}

void shell_extract_input_text(char *output, size_t output_size)
{
    lv_obj_t *input_line;
    const char *text;

    if (output == NULL || output_size == 0) {
        return;
    }

    /* The port lock asserts when LVGL is not up (unit tests, early boot),
     * so only take it when the widget exists — a NULL widget also implies
     * no port, matching the transcript/input-line convention. The mutex is
     * recursive, so the LVGL event path nests without deadlock, and the
     * textarea pointer stays valid for the whole locked region. */
    input_line = windows_get_input_line();
    if (input_line == NULL) {
        output[0] = '\0';
        return;
    }
    lvgl_port_lock(0);
    text = lv_textarea_get_text(input_line);

    if (text == NULL) {
        output[0] = '\0';
        lvgl_port_unlock();
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
    lvgl_port_unlock();

    shell_trim(output);
}

void shell_input_line_repair_prompt(const char *text)
{
    char *repaired = malloc(SHELL_COMMAND_BYTES);
    const char *user_text;
    size_t prompt_len;
    size_t text_len;
    size_t common = 0;

    if (repaired == NULL) {
        return;
    }
    if (text == NULL) {
        free(repaired);
        return;
    }

    /* Nothing to repair while the prompt prefix is intact. */
    if (strncmp(text, s_input_line_prompt, strlen(s_input_line_prompt)) == 0) {
        free(repaired);
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

    snprintf(repaired, SHELL_COMMAND_BYTES, "%s", user_text);
    shell_trim(repaired);
    shell_input_line_set_text(repaired);
    free(repaired);
}

void shell_input_line_paste(const char *text)
{
    lv_obj_t *input_line = windows_get_input_line();

    if (input_line == NULL || text == NULL) {
        return;
    }

    /* The input line is LVGL-backed state; serialize with the render cycle
     * the same way the transcript appends and input-line setters do. The
     * mutex is recursive, so the LVGL event path nests without deadlock.
     * add_text inserts at the cursor (DOS-style paste). */
    lvgl_port_lock(0);
    lv_textarea_add_text(input_line, text);
    lvgl_port_unlock();
}

/* ========================================================================
 * RAM CLIPBOARD (clip / paste)
 * ======================================================================== */

void shell_clipboard_set(const char *text)
{
    if (text == NULL) {
        s_clipboard[0] = '\0';
    } else {
        snprintf(s_clipboard, SHELL_CLIPBOARD_BYTES, "%s", text);
    }
    s_clipboard_is_file = false;
}

void shell_clipboard_set_file(const char *path)
{
    if (path != NULL) {
        snprintf(s_clipboard, SHELL_CLIPBOARD_BYTES, "%s", path);
    }
    s_clipboard_is_file = true;
}

const char *shell_clipboard_get(void)
{
    return s_clipboard;
}

bool shell_clipboard_is_file(void)
{
    return s_clipboard_is_file;
}

bool shell_clipboard_copy_transcript(int n_lines)
{
    const char *text;
    size_t length;
    size_t pos;
    int lines = 0;

    if (n_lines < 1) {
        n_lines = 1;
    }

    length = shell_transcript_get_length();
    text = shell_transcript_get_text_from(0);
    if (text == NULL) {
        return false;
    }

    /* Walk backwards counting newlines to find the start of the last
     * n_lines; cap n_lines by the actual line count. A single trailing
     * newline terminates the last line, it does not start a new (empty)
     * one, so skip it before counting. */
    pos = length;
    if (pos > 0 && text[pos - 1] == '\n') {
        pos--;
    }
    while (pos > 0 && lines < n_lines) {
        pos--;
        if (text[pos] == '\n') {
            lines++;
        }
    }
    if (pos > 0 && text[pos] == '\n') {
        pos++;   /* start just after the newline of the previous line */
    }

    shell_clipboard_set(text + pos);
    return s_clipboard[0] != '\0';
}

/* ========================================================================
 * TRANSCRIPT CLICK REGIONS (for anchor command)
 * ======================================================================== */

#define SHELL_CLICK_REGION_MAX 32

typedef struct {
    char text[64];
    char command[P4_CONFIG_COMMAND_BYTES];
    bool continue_line;
    int x, y;        /* Approximate position in transcript (line, col) */
    int len;         /* Text length */
    bool active;
} shell_click_region_t;

static shell_click_region_t s_click_regions[SHELL_CLICK_REGION_MAX];
static int s_click_region_count = 0;

/* Track approximate cursor position for anchor placement */
static int s_anchor_line = 0;
static int s_anchor_col = 0;
static bool s_anchor_continue_line = false;

void shell_transcript_add_anchor_region(const char *text, const char *command, bool continue_line)
{
    if (text == NULL || command == NULL || s_click_region_count >= SHELL_CLICK_REGION_MAX) {
        return;
    }

    shell_click_region_t *r = &s_click_regions[s_click_region_count++];
    strncpy(r->text, text, sizeof(r->text) - 1);
    r->text[sizeof(r->text) - 1] = '\0';
    strncpy(r->command, command, sizeof(r->command) - 1);
    r->command[sizeof(r->command) - 1] = '\0';
    r->continue_line = continue_line;
    r->x = s_anchor_col;
    r->y = s_anchor_line;
    r->len = (int)strlen(text);
    r->active = true;

    if (continue_line) {
        s_anchor_col += (int)strlen(text);
    } else {
        s_anchor_line++;
        s_anchor_col = 0;
    }
}

void shell_transcript_clear_click_regions(void)
{
    s_click_region_count = 0;
    s_anchor_line = 0;
    s_anchor_col = 0;
    s_anchor_continue_line = false;
}

bool shell_transcript_hit_test(int x, int y, const char **action_out)
{
    if (x < 0 || y < 0 || action_out == NULL) {
        return false;
    }

    for (int i = 0; i < s_click_region_count; i++) {
        shell_click_region_t *r = &s_click_regions[i];
        if (!r->active) continue;

        /* Simple hit-test: check if click falls within the anchor text region */
        if (y == r->y && x >= r->x && x < r->x + r->len) {
            *action_out = r->command;
            return true;
        }
    }
    return false;
}

/* Update anchor position tracking - called when transcript text is appended */
void shell_transcript_update_anchor_position(const char *text)
{
    if (text == NULL) return;

    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            s_anchor_line++;
            s_anchor_col = 0;
        } else {
            s_anchor_col++;
        }
        p++;
    }
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
    shell_transcript_appendf_ansi(SH_LBL "transcript trims:" SH_RST " " SH_NUM "%u" SH_RST " (internal guard)\n",
                             (unsigned int)s_transcript_trim_count);

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

bool shell_utf8_decode(const char *s, size_t avail, uint32_t *cp_out, size_t *len_out)
{
    unsigned char lead;
    size_t len;
    uint32_t cp;
    size_t i;

    if (s == NULL || avail == 0 || s[0] == '\0') {
        return false;
    }
    lead = (unsigned char)s[0];
    if (lead < 0x80) {
        len = 1;
        cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    } else {
        return false;
    }
    if (len > avail) {
        return false;
    }
    for (i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)s[i];
        if ((cont & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    /* Reject overlongs and out-of-range values. */
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return false;
    }
    if (cp_out != NULL) {
        *cp_out = cp;
    }
    if (len_out != NULL) {
        *len_out = len;
    }
    return true;
}

bool shell_key_wait_submit(char key)
{
    char seq[SHELL_KEY_SEQ_BYTES];

    if (!s_key_wait_active || s_key_queue == NULL) {
        return false;
    }

    seq[0] = key;
    seq[1] = '\0';

    /* Never block an input-source task on a full queue; a wait only ever
     * consumes one key, so dropping the overflow is the correct behavior. */
    if (xQueueSend(s_key_queue, &seq, 0) != pdTRUE) {
        return true;
    }

    return true;
}

bool shell_key_wait_submit_utf8(const char *bytes, size_t len)
{
    char seq[SHELL_KEY_SEQ_BYTES];
    uint32_t cp;
    size_t seq_len;

    if (!s_key_wait_active || s_key_queue == NULL) {
        return false;
    }
    if (!shell_utf8_decode(bytes, len, &cp, &seq_len) || seq_len >= SHELL_KEY_SEQ_BYTES) {
        return false;
    }
    memcpy(seq, bytes, seq_len);
    seq[seq_len] = '\0';

    if (xQueueSend(s_key_queue, &seq, 0) != pdTRUE) {
        return true;
    }

    return true;
}

bool shell_wait_for_key_utf8(uint32_t timeout_ms, char *buf, size_t buf_size)
{
    char seq[SHELL_KEY_SEQ_BYTES];

    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }

    /* Background workers never steal console keys: they take the headless
     * path immediately (pause/choice/more fall back to defaults/delays). */
    if (shell_is_background_task()) {
        (void)timeout_ms;
        return false;
    }

    if (s_key_queue == NULL || !s_key_wait_active) {
        return false;
    }

    /* A key wait always presents a prompt first (pause/choice/more/pagers);
     * repaint any deferred output so the user never answers a stale screen. */
    shell_transcript_flush_now();

    if (xQueueReceive(s_key_queue, &seq, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    seq[SHELL_KEY_SEQ_BYTES - 1] = '\0';

    if (buf != NULL && buf_size > 0) {
        size_t copy = strlen(seq);
        if (copy > buf_size - 1) {
            copy = buf_size - 1;
        }
        memcpy(buf, seq, copy);
        buf[copy] = '\0';
    }

    return true;
}

bool shell_wait_for_key(uint32_t timeout_ms, char *key_out)
{
    char seq[SHELL_KEY_SEQ_BYTES];

    if (key_out != NULL) {
        *key_out = '\0';
    }

    /* Background workers never steal console keys (see utf8 variant). */
    if (shell_is_background_task()) {
        (void)timeout_ms;
        return false;
    }

    if (s_key_queue == NULL || !s_key_wait_active) {
        return false;
    }

    /* A key wait always presents a prompt first (pause/choice/more/pagers);
     * repaint any deferred output so the user never answers a stale screen. */
    shell_transcript_flush_now();

    if (xQueueReceive(s_key_queue, &seq, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    seq[SHELL_KEY_SEQ_BYTES - 1] = '\0';

    /* ASCII fast path: multibyte keys yield their lead byte, which never
     * equals ASCII, so y/n/ESC compares stay correct. */
    if (key_out != NULL) {
        *key_out = seq[0];
    }

    return true;
}

/**
 * Shared line reader used by `shell_read_line` (echoed) and
 * `shell_read_line_hidden` (password mode, no echo). Collects a line through
 * the key queue with Backspace editing and ESC cancellation.
 *
 * @param echo  When true, each typed key (and the erase) is echoed so the
 *              user sees what they enter; when false nothing is echoed (the
 *              caller still gets the line, e.g. a password).
 */
static bool shell_read_line_mode(char *output, size_t output_size,
                                 uint32_t timeout_ms, bool echo)
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
        /* One key as a UTF-8 sequence (ASCII keys are 1 byte + NUL, so the
         * legacy single-char logic below is unchanged for them). */
        char seq[SHELL_KEY_SEQ_BYTES];
        char key;

        if (!shell_wait_for_key_utf8(timeout_ms, seq, sizeof(seq))) {
            break;
        }
        key = seq[0];

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
                /* Erase one full codepoint: walk back over continuation
                 * bytes so a multibyte character never tears. */
                do {
                    output[--length] = '\0';
                } while (length > 0 && ((unsigned char)output[length - 1] & 0xC0) == 0x80);
                /* Echo the erase so the transcript matches what is stored. */
                if (echo) {
                    shell_transcript_append_text("\b");
                }
            }
            continue;
        }

        /* Ignore control characters that are not editing keys. */
        if ((unsigned char)key < 0x20) {
            continue;
        }

        if (length + strlen(seq) + 1 >= output_size) {
            continue;
        }

        memcpy(output + length, seq, strlen(seq));
        length += strlen(seq);
        output[length] = '\0';

        /* Echo as typed so the user can see what they are entering, unless
         * the caller requested hidden (password) input. */
        if (echo) {
            shell_transcript_append_text(seq);
        }
    }

    shell_key_wait_end();
    shell_transcript_append_text("\n");

    return completed;
}

bool shell_read_line(char *output, size_t output_size, uint32_t timeout_ms)
{
    return shell_read_line_mode(output, output_size, timeout_ms, true);
}

bool shell_read_line_hidden(char *output, size_t output_size, uint32_t timeout_ms)
{
    return shell_read_line_mode(output, output_size, timeout_ms, false);
}

bool shell_key_input_available(void)
{
    /* The UART console is the always-on key source once started. A USB
     * keyboard is the other one; ask the USB module directly so an
     * on-screen-only session still falls back to the timed path. */
    return s_uart_console_running ||
           (s_command_ops.usb_is_keyboard_attached != NULL && s_command_ops.usb_is_keyboard_attached());
}

void shell_set_batch_active(bool active)
{
    s_batch_active = active;
}

bool shell_is_batch_active(void)
{
    return s_batch_active;
}

/* Foreground break + worker-busy state (see shell.h). Written from the
 * LVGL task (USB Ctrl+C, Stop button), read on the command worker; plain
 * bools like the background kill flags. */
static volatile bool s_foreground_abort;
static volatile bool s_command_busy;
static volatile bool s_foreground_break_seen;

void shell_request_abort(void)
{
    s_foreground_abort = true;
}

bool shell_abort_requested(void)
{
    return s_foreground_abort;
}

void shell_clear_abort(void)
{
    s_foreground_abort = false;
}

void shell_mark_foreground_break(void)
{
    s_foreground_break_seen = true;
}

bool shell_foreground_break_pending(void)
{
    return s_foreground_break_seen;
}

void shell_clear_foreground_break(void)
{
    s_foreground_break_seen = false;
}

void shell_set_command_busy(bool busy)
{
    s_command_busy = busy;
}

bool shell_is_command_busy(void)
{
    return s_command_busy;
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
    /* The line buffer is command-sized (up to P4_CONFIG_COMMAND_BYTES) and
     * would dominate the 12 KB console task stack, so it lives on the heap
     * for the life of the task. */
    char *line = malloc(SHELL_COMMAND_BYTES);
    size_t length = 0;
    bool prompt_visible = false;
    /* Timestamp of the last completed line. The USB-Serial-JTAG VFS maps CR to
     * LF, so a host CRLF arrives as two LFs: the first ends the line, the
     * second is an artifact. A leading LF that arrives within this grace
     * window after a completed line is dropped so it can never satisfy the
     * next key wait (set /p, pause, choice, crypt /ask) as an empty key. */
    int64_t line_done_us = 0;
    const int64_t line_artifact_grace_us = 100000;   /* 100 ms */

    (void)arg;

    if (line == NULL) {
        shell_record_errorf("uart", ESP_ERR_NO_MEM, "Failed to allocate the UART console line buffer");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    shell_uart_console_write_text("\nUART console ready. Type help for commands.\n");

    while (true) {
        char *newline;
        size_t got;

        if (!prompt_visible && !s_key_wait_active) {
            shell_uart_console_print_prompt();
            prompt_visible = true;
        }

        /* Read into the space after any partial line already buffered. The
         * line buffer is heap-allocated, so the size must be the allocation
         * size (not sizeof(line), which would be the pointer size). */
        if (fgets(line + length, SHELL_COMMAND_BYTES - length, stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            clearerr(stdin);
            continue;
        }

        got = strlen(line + length);
        length += got;

        /* Drop the artifact LF of a CRLF whose first LF just completed a line
         * (the CR->LF mapping makes the host's CRLF two LFs). Only LFs in the
         * brief window right after a completed line are dropped, so a genuine
         * blank line typed by the user is never swallowed. */
        if (got > 0 && (esp_timer_get_time() - line_done_us) < line_artifact_grace_us) {
            while (got > 0 && line[length - got] == '\n') {
                memmove(line + length - got, line + length - got + 1, got);
                length -= 1;
                got -= 1;
            }
        }

        /* A pending keypress wait swallows input before any command lookup,
         * so answering `pause` or `choice` never dispatches a command. The
         * whole freshly-read chunk is forwarded into the key queue (not just
         * its first character), so a confirmation word such as "YES" or a
         * `set /p` value can be typed on one serial line. The queue is reset
         * by shell_key_wait_begin()/shell_key_wait_end(), so stray characters
         * from a one-key wait (pause/choice/more) never leak into the next
         * prompt. This must not wait for a line terminator - a key wait is
         * answered as soon as a key is readable. */
        if (s_key_wait_active) {
            size_t key_index;
            size_t chunk_start = length - got;
            size_t forwarded = 0;

            /* Forward whole UTF-8 codepoints (not bytes): a multibyte key
             * is one queue item, so key waits never see fragments. ASCII
             * keeps the single-char fast path. */
            bool truncated = false;
            for (key_index = chunk_start;
                 key_index < length && forwarded < (size_t)P4_CONFIG_KEY_QUEUE_DEPTH;
                 ) {
                uint32_t cp;
                size_t seq_len;

                if (line[key_index] == '\n' || line[key_index] == '\0') {
                    shell_key_wait_submit('\r');
                    key_index++;
                    forwarded++;
                    continue;
                }
                if (!shell_utf8_decode(line + key_index, length - key_index,
                                       &cp, &seq_len)) {
                    /* Truncated tail (split USB-Serial-JTAG chunk): keep the
                     * partial bytes at the front so the next read completes
                     * the codepoint instead of dropping it. */
                    truncated = true;
                    break;
                }
                (void)cp;
                if (seq_len == 1) {
                    shell_key_wait_submit(line[key_index]);
                } else {
                    shell_key_wait_submit_utf8(line + key_index, seq_len);
                }
                key_index += seq_len;
                forwarded++;
            }
            if (truncated) {
                size_t leftover = length - key_index;
                memmove(line, line + key_index, leftover);
                length = leftover;
            } else {
                /* Fully consumed (or dropped on the depth cap, as before). */
                length = 0;
                line_done_us = esp_timer_get_time();
            }
            prompt_visible = false;
            continue;
        }

        /* The USB-Serial-JTAG driver delivers a logical line across several
         * reads (its RX FIFO is 64 bytes), so a chunk without a newline is
         * not a complete command. Buffer it and keep reading until the line
         * terminates, flushing over-long lines so the console never wedges. */
        newline = memchr(line, '\n', length);
        if (newline == NULL) {
            if (length > 0 && line[length - 1] == '\r') {
                /* CR-terminated line (no LF); treat as complete. */
                newline = line + length;
            } else if (length >= SHELL_COMMAND_BYTES - 1) {
                /* No terminator and the buffer is full: flush what we have. */
                newline = line + length;
            } else {
                continue;
            }
        }

        /* A complete line is ready: strip the trailing newline marker(s). */
        {
            size_t line_len = (size_t)(newline - line);

            while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
                line[--line_len] = '\0';
            }

            if (line_len > 0) {
                const char *trimmed = shell_trim(line);

                /* While a native modal surface is open, serial lines are
                 * routed to that surface instead of being dispatched as shell
                 * commands (the worker is blocked in the modal session). */
                bool modal_active = s_command_ops.modal_is_active != NULL &&
                                    s_command_ops.modal_is_active();

                if (modal_active) {
                    /* Console-local commands that must run on THIS task while
                     * the worker is blocked (the streaming `screenshot`, so a
                     * modal can be captured). The handler claims only its own
                     * exact tokens and returns false otherwise, so the modal
                     * still receives its input. */
                    if (s_command_ops.modal_console_command != NULL &&
                        s_command_ops.modal_console_command(trimmed)) {
                        length = 0;
                        line_done_us = esp_timer_get_time();
                        prompt_visible = false;
                        continue;
                    }
                    /* The modal OWNS the input while it is open and the worker
                     * is blocked inside it, so a line the modal does not claim
                     * MUST NOT be queued: the worker cannot run it until the
                     * modal closes, and a burst then overflows the command
                     * queue and drops input (including the modal's own quit
                     * line). Route it to the modal and drop it there. */
                    if (s_command_ops.modal_handle_serial_line != NULL) {
                        (void)s_command_ops.modal_handle_serial_line(trimmed);
                    }
                    length = 0;
                    line_done_us = esp_timer_get_time();
                    prompt_visible = false;
                    continue;
                }

                shell_uart_console_submit_command(trimmed);
            }        }

        length = 0;
        line_done_us = esp_timer_get_time();
        prompt_visible = false;
    }
}

void shell_uart_console_start(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* On USB-Serial/JTAG boards the console VFS normally runs in polled mode
     * against the 64-byte hardware FIFO, which drops bytes whenever a host
     * bursts faster than the reader polls. Install the interrupt-driven
     * driver with a large RX ring (and put the VFS in driver mode) so serial
     * input - especially the `receive` binary transfer - is buffered reliably
     * instead of silently losing bytes. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

        usj_cfg.rx_buffer_size = SHELL_USJ_RX_BUFFER_BYTES;
        usj_cfg.tx_buffer_size = SHELL_USJ_TX_BUFFER_BYTES;
        if (usb_serial_jtag_driver_install(&usj_cfg) == ESP_OK) {
            /* The non-deprecated spelling lives in a private IDF header; the
             * public alias is deprecated but still the supported call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            esp_vfs_usb_serial_jtag_use_driver();
#pragma GCC diagnostic pop
        }
    }
    fcntl(fileno(stdin), F_SETFL, 0);
    fcntl(fileno(stdout), F_SETFL, 0);
#endif

    if (s_uart_console_lock == NULL) {
        s_uart_console_lock = xSemaphoreCreateMutex();
    }

    if (s_shell_command_lock == NULL) {
        s_shell_command_lock = xSemaphoreCreateMutex();
    }

    if (xTaskCreateWithCaps(shell_uart_console_task,
                    "shell_uart",
                    SHELL_UART_CONSOLE_TASK_STACK_BYTES,
                    NULL,
                    tskIDLE_PRIORITY + 1,
                    &s_uart_console_task_handle,
                    MALLOC_CAP_SPIRAM) != pdPASS) {
        shell_record_errorf("uart", ESP_FAIL, "Failed to start UART console task");
        return;
    }

    /* The console reader is now an interactive key source, so pause, choice,
     * and more can block on a real keystroke instead of a timed delay. */
    s_uart_console_running = true;
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
/**
 * Mirror text to the USB-Serial/JTAG TX ring through the driver API.
 *
 * The IDF VFS write path (used by printf) drops the whole line when its
 * SOF-based connection monitor reports "disconnected", and that monitor can
 * false-report a disconnect for a few ms under host/load pressure. Writing via
 * usb_serial_jtag_write_bytes() bypasses that check. The VFS would expand LF to
 * CRLF (CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF); do it here so the wire format
 * is unchanged. Each segment waits at most the configured bound, so a stalled
 * ring delays but never wedges the writer.
 */
static void shell_uart_mirror_write(const char *text, size_t len)
{
    TickType_t ticks = pdMS_TO_TICKS(P4_CONFIG_UART_MIRROR_WRITE_TIMEOUT_MS);

    while (len > 0) {
        const char *nl = memchr(text, '\n', len);
        size_t seg = (nl != NULL) ? (size_t)(nl - text) : len;

        if (seg > 0) {
            (void)usb_serial_jtag_write_bytes(text, seg, ticks);
        }
        if (nl == NULL) {
            break;
        }
        (void)usb_serial_jtag_write_bytes("\r\n", 2, ticks);
        len -= seg + 1;
        text = nl + 1;
    }
}
#endif /* CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG */

void shell_uart_console_write_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* With no host attached there is no reader and the TX ring would block.
     * The SOF monitor can also false-report a disconnect for a few ms under
     * load, so only give up after the disconnect has persisted past the grace
     * window; within it every line is still written (the O3 output loss was a
     * transient flip dropping a single line). */
    if (usb_serial_jtag_is_connected()) {
        s_mirror_disconnected_since_us = 0;
    } else {
        int64_t now = esp_timer_get_time();
        if (s_mirror_disconnected_since_us == 0) {
            s_mirror_disconnected_since_us = now;
        } else if (now - s_mirror_disconnected_since_us >
                   (int64_t)P4_CONFIG_UART_MIRROR_DISCONNECT_GRACE_MS * 1000) {
            return;
        }
    }
#endif

    if (s_uart_console_lock != NULL) {
        xSemaphoreTake(s_uart_console_lock, portMAX_DELAY);
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_is_driver_installed()) {
        shell_uart_mirror_write(text, strlen(text));
    } else {
        printf("%s", text);
    }
#else
    printf("%s", text);
#endif

    if (s_uart_console_lock != NULL) {
        xSemaphoreGive(s_uart_console_lock);
    }
}

void shell_uart_console_rx_begin(void)
{
    /* Suspend the console reader so it cannot consume stdin while this caller
     * reads raw binary bytes. The console task is normally blocked in fgets or
     * about to read; freezing it keeps the raw stream for the caller.
     *
     * When the console task ITSELF is the caller (the streaming `screenshot`
     * runs there so a modal can be captured while the worker is blocked), it
     * must not suspend itself: it is already busy and cannot read stdin until
     * the stream finishes. A self-suspend would never be resumed. */
    if (s_uart_console_task_handle != NULL &&
        s_uart_console_task_handle != xTaskGetCurrentTaskHandle()) {
        vTaskSuspend(s_uart_console_task_handle);
    }
}

void shell_uart_console_rx_end(void)
{
    if (s_uart_console_task_handle != NULL &&
        s_uart_console_task_handle != xTaskGetCurrentTaskHandle()) {
        vTaskResume(s_uart_console_task_handle);
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
    /* Both copies are command-sized and would dominate the UART console
     * task's 12 KB stack, so they are heap-allocated here. */
    char *command_copy = malloc(SHELL_COMMAND_BYTES);
    char *transcript_command = malloc(SHELL_COMMAND_BYTES);

    if (command == NULL || command[0] == '\0' || command_copy == NULL || transcript_command == NULL) {
        free(command_copy);
        free(transcript_command);
        return;
    }

    /* A serial command is user activity: reset the power idle clock and wake
     * the display if the idle timer had switched it off. */
    if (s_command_ops.pm_notify_activity != NULL) {
        s_command_ops.pm_notify_activity();
    }

    snprintf(command_copy, SHELL_COMMAND_BYTES, "%s", command);
    shell_format_command_for_transcript(command_copy, transcript_command, SHELL_COMMAND_BYTES);

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
        free(command_copy);
        free(transcript_command);
        return;
    }

    shell_transcript_appendf("%s%s\n", SHELL_PROMPT, transcript_command);
    if (shell_command_should_store_history(command_copy)) {
        shell_store_command_history(command_copy);
    }
    shell_reset_history_cursor();

    /* Jump to the output of the submitted command even when the user was
     * reading earlier history. */
    shell_force_transcript_scroll_to_end();

    /* Release the LVGL lock BEFORE dispatching to the worker. A command such
     * as `edit` opens the modal editor with an lv_async_call, which the LVGL
     * task services by taking the same lock; if we still held it here, the
     * LVGL task would block on it while this (console) task waits for the
     * worker, deadlocking the LVGL task into a watchdog reboot. */
    lvgl_port_unlock();

    /* Run the command on the worker task when possible. */
    if (s_command_ops.execute_command_async != NULL) {
        s_command_ops.execute_command_async(command_copy);
    } else if (s_command_ops.execute_command != NULL) {
        s_command_ops.execute_command(command_copy);
    }

    if (s_shell_command_lock != NULL) {
        xSemaphoreGive(s_shell_command_lock);
    }

    free(command_copy);
    free(transcript_command);
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

/* Tab-completion cycle state: remembers the whole input line being completed
 * and how many times Tab was pressed, so repeated Tab cycles the matches. */
static char s_tab_last_word[SHELL_COMMAND_BYTES];
static int s_tab_match_index;

/* USB HID Ctrl modifier bits (HID standard; see components/usb/usb.h
 * USB_KEY_MOD_*. shell cannot include usb.h (usb already requires shell),
 * so the two stable bit values are mirrored here). */
#define SHELL_USB_MOD_LEFT_CTRL   0x01
#define SHELL_USB_MOD_RIGHT_CTRL  0x10

/**
 * Tab completion: complete the last word of the input line through the command
 * module's `complete_line` provider (help-table command names, aliases,
 * subcommands/flags, installed apps, and SD paths). A unique match fills it
 * in; repeated Tab cycles the matches. Runs on the LVGL task, so the
 * command-sized buffers are heap-allocated.
 */
void shell_input_line_tab_complete(void)
{
    char *input = malloc(SHELL_COMMAND_BYTES);
    char *completion = malloc(384);
    char *rebuilt = malloc(SHELL_COMMAND_BYTES);
    const char *word;
    size_t prefix_len = 0;
    int total;

    if (input == NULL || completion == NULL || rebuilt == NULL) {
        free(input);
        free(completion);
        free(rebuilt);
        return;
    }

    if (s_command_ops.complete_line == NULL) {
        free(input);
        free(completion);
        free(rebuilt);
        return;
    }

    shell_extract_input_text(input, SHELL_COMMAND_BYTES);
    if (input[0] == '\0') {
        free(input);
        free(completion);
        free(rebuilt);
        return;
    }

    /* The word is the last whitespace-delimited token; the prefix is
     * everything before it (including the trailing space). */
    {
        const char *last_space = strrchr(input, ' ');

        word = last_space != NULL ? last_space + 1 : input;
        prefix_len = (size_t)(word - input);
    }

    /* Reset the cycle when the line changes. */
    if (strcmp(input, s_tab_last_word) != 0) {
        snprintf(s_tab_last_word, sizeof(s_tab_last_word), "%s", input);
        s_tab_match_index = 0;
    }

    total = s_command_ops.complete_line(input, s_tab_match_index, completion, 384);
    if (total <= 0) {
        free(input);
        free(completion);
        free(rebuilt);
        shell_input_line_ghost_refresh();
        return;
    }

    /* Fill prefix + completion. */
    if (prefix_len > 0) {
        snprintf(rebuilt, SHELL_COMMAND_BYTES, "%.*s%s", (int)prefix_len, input, completion);
    } else {
        snprintf(rebuilt, SHELL_COMMAND_BYTES, "%s", completion);
    }
    shell_input_line_set_text(rebuilt);

    s_tab_match_index = (s_tab_match_index + 1) % total;

    free(input);
    free(completion);
    free(rebuilt);
    shell_input_line_ghost_refresh();
}

/* ========================================================================
 * INLINE GHOST COMPLETION
 * ======================================================================== */

static void shell_ghost_hide(void)
{
    lv_obj_t *ghost = windows_get_input_ghost();

    if (ghost != NULL) {
        lv_obj_add_flag(ghost, LV_OBJ_FLAG_HIDDEN);
    }
}

void shell_input_line_ghost_refresh(void)
{
    lv_obj_t *il = windows_get_input_line();
    lv_obj_t *ghost = windows_get_input_ghost();
    char *input = NULL;
    char *best = NULL;
    const char *text;
    const char *last_space;
    const char *word;
    size_t word_len;
    int total;

#if !P4_CONFIG_COMPLETION_GHOST
    (void)il;
    (void)input;
    (void)best;
    (void)text;
    (void)last_space;
    (void)word;
    (void)word_len;
    (void)total;
    shell_ghost_hide();
    return;
#endif

    if (il == NULL || ghost == NULL || s_command_ops.ghost_line == NULL) {
        shell_ghost_hide();
        return;
    }
    if (shell_history_search_active()) {
        shell_ghost_hide();
        return;
    }
    if (s_command_ops.modal_is_active != NULL && s_command_ops.modal_is_active()) {
        shell_ghost_hide();
        return;
    }

    text = lv_textarea_get_text(il);
    if (text == NULL || text[0] == '\0') {
        shell_ghost_hide();
        return;
    }
    /* Ghost only makes sense with the caret at the end of the line. */
    if (lv_textarea_get_cursor_pos(il) != (uint32_t)strlen(text)) {
        shell_ghost_hide();
        return;
    }

    input = malloc(SHELL_COMMAND_BYTES);
    best = malloc(384);
    if (input == NULL || best == NULL) {
        free(input);
        free(best);
        shell_ghost_hide();
        return;
    }

    shell_extract_input_text(input, SHELL_COMMAND_BYTES);
    if (input[0] == '\0') {
        free(input);
        free(best);
        shell_ghost_hide();
        return;
    }

    total = s_command_ops.ghost_line(input, best, 384);
    if (total <= 0) {
        free(input);
        free(best);
        shell_ghost_hide();
        return;
    }

    last_space = strrchr(input, ' ');
    word = last_space != NULL ? last_space + 1 : input;
    word_len = strlen(word);
    if (strncasecmp(best, word, word_len) != 0 || best[word_len] == '\0') {
        free(input);
        free(best);
        shell_ghost_hide();
        return;
    }

    /* Place the muted suffix right after the rendered text. Hide it when the
     * text already fills (and therefore scrolls) the input line. */
    {
        lv_point_t size = {0, 0};
        const lv_font_t *font = lv_obj_get_style_text_font(il, LV_PART_MAIN);

        lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > lv_obj_get_content_width(il)) {
            free(input);
            free(best);
            shell_ghost_hide();
            return;
        }
        lv_label_set_text(ghost, best + word_len);
        lv_obj_set_pos(ghost, size.x, 0);
    }
    lv_obj_remove_flag(ghost, LV_OBJ_FLAG_HIDDEN);

    free(input);
    free(best);
}

/* ========================================================================
 * REVERSE-HISTORY SEARCH (Ctrl+R)
 * ======================================================================== */

typedef struct {
    bool active;
    char query[96];
    size_t query_len;
    int match;                        /* 0 = newest matching entry */
    char draft[SHELL_COMMAND_BYTES];  /* line before the search began */
} shell_history_search_t;

static shell_history_search_t s_search;

bool shell_history_search_active(void)
{
    return s_search.active;
}

/** Case-insensitive substring test (newlib lacks a portable strcasestr). */
bool shell_history_search_matches(const char *entry, const char *query)
{
    size_t nl;

    if (entry == NULL || query == NULL) {
        return false;
    }
    nl = strlen(query);
    if (nl == 0) {
        return true;
    }
    for (; *entry != '\0'; entry++) {
        if (strncasecmp(entry, query, nl) == 0) {
            return true;
        }
    }
    return false;
}

static void shell_history_search_hide_label(void)
{
    lv_obj_t *lbl = windows_get_search_label();

    if (lbl != NULL) {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void shell_history_search_show_label(void)
{
    lv_obj_t *lbl = windows_get_search_label();
    lv_obj_t *row = windows_get_input_row();

    if (lbl == NULL) {
        return;
    }
    lv_label_set_text_fmt(lbl, "(reverse-i-search) '%s':", s_search.query);
    if (row != NULL) {
        lv_obj_align_to(lbl, row, LV_ALIGN_OUT_TOP_LEFT, 0, 2);
    }
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

/** Show the s_search.match-th newest history entry containing the query. */
static void shell_history_search_apply(void)
{
    size_t count = shell_history_get_count();
    const char *chosen = NULL;
    int seen = 0;
    int i;

    if (count > 0) {
        for (i = (int)count - 1; i >= 0; i--) {
            const char *entry = shell_history_get((size_t)i);

            if (entry == NULL) {
                continue;
            }
            if (s_search.query_len == 0 || shell_history_search_matches(entry, s_search.query)) {
                if (seen == s_search.match) {
                    chosen = entry;
                    break;
                }
                seen++;
            }
        }
    }
    if (chosen != NULL) {
        shell_input_line_set_text(chosen);
    }
    shell_history_search_show_label();
}

static void shell_history_search_begin(void)
{
    char *cur;

    if (s_search.active) {
        s_search.match++;
        shell_history_search_apply();
        return;
    }
    cur = malloc(SHELL_COMMAND_BYTES);
    if (cur != NULL) {
        shell_extract_input_text(cur, SHELL_COMMAND_BYTES);
        snprintf(s_search.draft, sizeof(s_search.draft), "%s", cur);
        free(cur);
    } else {
        s_search.draft[0] = '\0';
    }
    s_search.query[0] = '\0';
    s_search.query_len = 0;
    s_search.match = 0;
    s_search.active = true;
    shell_ghost_hide();
    shell_history_search_apply();
}

static void shell_history_search_cancel(void)
{
    if (!s_search.active) {
        return;
    }
    s_search.active = false;
    shell_input_line_set_text(s_search.draft);
    shell_history_search_hide_label();
    shell_input_line_ghost_refresh();
}

static void shell_history_search_accept(void)
{
    if (!s_search.active) {
        return;
    }
    s_search.active = false;
    shell_history_search_hide_label();
    shell_reset_history_cursor();
    shell_input_line_ghost_refresh();
}

static void shell_history_search_cycle(int dir)
{
    if (!s_search.active) {
        return;
    }
    if (dir < 0) {
        s_search.match++;
    } else if (s_search.match > 0) {
        s_search.match--;
    }
    shell_history_search_apply();
}

/** Consume a USB key while reverse search is active; start it on Ctrl+R. */
static bool shell_history_search_handle_key(uint8_t key_code, uint8_t modifiers, char ch)
{
    bool ctrl = (modifiers & (SHELL_USB_MOD_LEFT_CTRL | SHELL_USB_MOD_RIGHT_CTRL)) != 0;

    if (ctrl && key_code == 0x15) { /* Ctrl+R */
        shell_history_search_begin();
        return true;
    }
    if (!s_search.active) {
        return false;
    }
    if (ch == '\n' || ch == '\r') {
        shell_history_search_accept();
    } else if (ch == '\b') {
        if (s_search.query_len > 0) {
            s_search.query[--s_search.query_len] = '\0';
        }
        shell_history_search_apply();
    } else if (ch == 0x1B) {
        shell_history_search_cancel();
    } else if (ch >= 0x20 && ch <= 0x7E) {
        if (s_search.query_len + 1 < sizeof(s_search.query)) {
            s_search.query[s_search.query_len++] = ch;
            s_search.query[s_search.query_len] = '\0';
        }
        s_search.match = 0;
        shell_history_search_apply();
    } else if (key_code == 0x52) { /* Up: older */
        shell_history_search_cycle(-1);
    } else if (key_code == 0x51) { /* Down: newer */
        shell_history_search_cycle(1);
    }
    return true;
}

/* USB HID Ctrl modifier bits are defined earlier (with the reverse-search
 * handler); kept once to avoid a duplicate macro warning. */

/** Move the input-line cursor one word in @p dir (-1 left, +1 right),
 * emacs-style: skip spaces, then skip a word run. Walks codepoints (not
 * bytes) so multibyte text never tears. Positions are letter indices. */
static void shell_input_cursor_word(lv_obj_t *input_line, int dir)
{
    const char *text;
    /* Offset table is heap-allocated: this runs on the LVGL task, whose
     * stack is far smaller than a command-sized local. */
    uint32_t *offs = NULL;
    uint32_t letters = 0;
    uint32_t pos;
    uint32_t target;

    if (input_line == NULL || (dir != -1 && dir != +1)) {
        return;
    }
    text = lv_textarea_get_text(input_line);
    if (text == NULL) {
        return;
    }
    pos = lv_textarea_get_cursor_pos(input_line);

    /* Pass 1: map every letter index to its byte offset. */
    {
        uint32_t count = 0;
        uint32_t b = 0;
        uint32_t cp;
        size_t len;
        while (text[b] != '\0') {
            if (!shell_utf8_decode(text + b, strlen(text + b), &cp, &len)) {
                len = 1;
            }
            count++;
            b += (uint32_t)len;
        }
        offs = malloc((count + 1) * sizeof(uint32_t));
        if (offs == NULL) {
            return;
        }
        letters = count;
        b = 0;
        for (count = 0; count < letters; count++) {
            uint32_t cp;
            size_t len;
            offs[count] = b;
            if (!shell_utf8_decode(text + b, strlen(text + b), &cp, &len)) {
                len = 1;
            }
            b += (uint32_t)len;
        }
        offs[letters] = b;
    }
    if (pos > letters) {
        pos = letters;
    }
    target = pos;

    /* A word character is anything but ASCII space/tab. */
    if (dir < 0) {
        while (target > 0) {
            uint32_t cp = (unsigned char)text[offs[target - 1]];
            if (cp != ' ' && cp != '\t') {
                break;
            }
            target--;
        }
        while (target > 0) {
            uint32_t cp = (unsigned char)text[offs[target - 1]];
            if (cp == ' ' || cp == '\t') {
                break;
            }
            target--;
        }
    } else {
        while (target < letters) {
            uint32_t cp = (unsigned char)text[offs[target]];
            if (cp != ' ' && cp != '\t') {
                break;
            }
            target++;
        }
        while (target < letters) {
            uint32_t cp = (unsigned char)text[offs[target]];
            if (cp == ' ' || cp == '\t') {
                break;
            }
            target++;
        }
    }
    free(offs);
    lv_textarea_set_cursor_pos(input_line, (int32_t)target);
}

static void shell_usb_keyboard_inject_cb(void *user_data)
{
    usb_key_inject_ctx_t *ctx = (usb_key_inject_ctx_t *)user_data;
    lv_obj_t *input_line;
    if (ctx == NULL) {
        return;
    }

    /* While a native modal surface is open, every USB key goes to it. */
    if (s_command_ops.modal_is_active != NULL && s_command_ops.modal_is_active()) {
        /* Global chord hotkeys fire in non-editor modals (dialog/list are
         * consumed on close, so the line runs next); the editor keeps its
         * full Ctrl vocabulary and never yields a chord. */
        if (!windows_editor_mode_active() &&
            s_command_ops.bind_lookup_chord != NULL &&
            s_command_ops.execute_command_async != NULL) {
            char *bound = malloc(SHELL_COMMAND_BYTES);
            if (bound != NULL) {
                if (s_command_ops.bind_lookup_chord(ctx->key_code, ctx->modifiers,
                                                    bound, SHELL_COMMAND_BYTES)) {
                    s_command_ops.execute_command_async(bound);
                    free(bound);
                    free(ctx);
                    return;
                }
                free(bound);
            }
        }
        char ch = '\0';
        if (s_command_ops.usb_key_to_ascii != NULL &&
            s_command_ops.usb_key_to_ascii(ctx->key_code, ctx->modifiers, &ch)) {
            /* fall through with the ascii char */
        } else {
            ch = '\0';
        }
        if (s_command_ops.modal_handle_usb_key != NULL) {
            s_command_ops.modal_handle_usb_key(ctx->key_code, ctx->modifiers, ch);
        }
        free(ctx);
        return;
    }

    input_line = windows_get_input_line();
    if (input_line == NULL) {
        free(ctx);
        return;
    }

    /* Ctrl+C foreground break (HID 0x06 = 'c'): never typed, never routed
     * to modals (handled above). With no key-wait active it aborts a
     * running command, or clears the idle input line DOS-style. Active
     * waits (term/pause/choice/menu) own their keys through the sync path
     * and the guard below, so a remote `term` session still receives ^C;
     * an active reverse-search keeps its keys too. */
    {
        bool ctrl = (ctx->modifiers & (SHELL_USB_MOD_LEFT_CTRL | SHELL_USB_MOD_RIGHT_CTRL)) != 0;
        if (ctrl && ctx->key_code == 0x06 && !shell_key_wait_is_active() &&
            !shell_history_search_active()) {
            if (shell_is_command_busy()) {
                shell_request_abort();
            } else {
                lv_textarea_set_text(input_line, "");
            }
            free(ctx);
            return;
        }
    }

    /* Reverse-history search (Ctrl+R) owns the input line while active. */
    {
        char search_ch = '\0';

        if (s_command_ops.usb_key_to_ascii != NULL) {
            (void)s_command_ops.usb_key_to_ascii(ctx->key_code, ctx->modifiers, &search_ch);
        }
        if (shell_history_search_handle_key(ctx->key_code, ctx->modifiers, search_ch)) {
            free(ctx);
            return;
        }
    }

    /* Ctrl+letter chord hotkeys (`bind ^X`). The ASCII mapper below ignores
     * Ctrl, so chords would otherwise arrive as typed letters; they fire
     * here instead, before printing. ^C is reserved for break (handled
     * above) and never matches. Queued behind a running command like any
     * submission. */
    if (s_command_ops.bind_lookup_chord != NULL &&
        s_command_ops.execute_command_async != NULL) {
        char *bound = malloc(SHELL_COMMAND_BYTES);
        if (bound != NULL) {
            if (s_command_ops.bind_lookup_chord(ctx->key_code, ctx->modifiers,
                                                bound, SHELL_COMMAND_BYTES)) {
                s_command_ops.execute_command_async(bound);
                free(bound);
                free(ctx);
                return;
            }
            free(bound);
        }
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
                /* Tab: complete the current word (commands, aliases, paths). */
                shell_input_line_tab_complete();
            } else if (ch >= 0x20 && ch <= 0x7E) {
                /* Printable ASCII character */
                lv_textarea_add_char(input_line, (uint8_t)ch);
            }
        } else {
            /* Non-printable key: handle navigation and editing */
            switch (ctx->key_code) {
            case 0x4F: /* Right arrow: move cursor right (Ctrl: word jump) */
                if ((ctx->modifiers & (SHELL_USB_MOD_LEFT_CTRL | SHELL_USB_MOD_RIGHT_CTRL)) != 0) {
                    shell_input_cursor_word(input_line, +1);
                } else {
                    lv_textarea_cursor_right(input_line);
                }
                break;
            case 0x50: /* Left arrow: move cursor left (Ctrl: word jump) */
                if ((ctx->modifiers & (SHELL_USB_MOD_LEFT_CTRL | SHELL_USB_MOD_RIGHT_CTRL)) != 0) {
                    shell_input_cursor_word(input_line, -1);
                } else {
                    lv_textarea_cursor_left(input_line);
                }
                break;
            case 0x51: /* Down arrow: recall newer history */
                shell_recall_history(1);
                break;
            case 0x52: /* Up arrow: recall older history */
                shell_recall_history(-1);
                break;
            case 0x4C: /* Delete: remove char at cursor (forward) */
                lv_textarea_delete_char_forward(input_line);
                break;
            case 0x4B: /* PageUp: scroll the transcript up one page */
                {
                    lv_obj_t *transcript = windows_get_transcript();
                    int32_t page = transcript != NULL ? lv_obj_get_height(transcript) : 0;
                    windows_scroll_transcript_by(-page);
                }
                break;
            case 0x4E: /* PageDown: scroll the transcript down one page */
                {
                    lv_obj_t *transcript = windows_get_transcript();
                    int32_t page = transcript != NULL ? lv_obj_get_height(transcript) : 0;
                    windows_scroll_transcript_by(page);
                }
                break;
            case 0x4A: /* Home: move to beginning */
                lv_textarea_set_cursor_pos(input_line, 0);
                break;
            case 0x4D: /* End: move to end */
                lv_textarea_set_cursor_pos(input_line, LV_TEXTAREA_CURSOR_LAST);
                break;
            default:
                /* Bound function keys (F1..F12 via `bind`) fire their line
                 * onto the command worker. Modals and key waits never reach
                 * here (they are claimed earlier), so a bind cannot hijack
                 * a prompt. The line buffer is heap-sized: the LVGL task
                 * stack is no place for a command-sized local. */
                if (s_command_ops.bind_lookup_fkey != NULL &&
                    s_command_ops.execute_command_async != NULL) {
                    char *bound = malloc(P4_CONFIG_COMMAND_BYTES);
                    if (bound != NULL) {
                        if (s_command_ops.bind_lookup_fkey(ctx->key_code, bound,
                                                           P4_CONFIG_COMMAND_BYTES)) {
                            s_command_ops.execute_command_async(bound);
                            free(bound);
                            free(ctx);
                            return;
                        }
                        free(bound);
                    }
                }
                break;
            }
        }
    }

    shell_input_line_ghost_refresh();
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

/* One-line entry in the offline command reference (`help /all`, `help <cmd>`). */
typedef struct {
    const char *name;      /* primary command name (aliases listed in summary) */
    const char *summary;   /* one-line usage + description */
} shell_help_entry_t;

static const shell_help_entry_t s_shell_help_entries[] = {
    { "help",     "help [cmd | /all] - this summary; help /all lists every command; help <cmd> shows one" },
    { "about",    "about - project identity, build date/time, Git hash, license, third-party summary" },
    { "version",  "version | ver - banner, version, build date/time, Git hash, IDF, board, heap, uptime" },
    { "sysinfo",  "sysinfo - board, display, storage, heap, FreeRTOS tasks, uptime, Wi-Fi, OTA state" },
    { "mem",      "mem - free heap, total, minimum, internal, task count, PSRAM state" },
    { "debug",    "debug - last error/warning entries, Wi-Fi state, heap, warning count" },
    { "clear",    "clear | cls - clear transcript history and redraw the prompt" },
    { "reboot",   "reboot - restart the board" },
    { "launch",   "launch | launch <name> [args] | launch /list - discover and run script apps .bat/.cmd (PATH + sd:/APPS)" },
    { "apps",     "apps - list the registered native apps (applib ABI table)" },
    { "ps",       "ps | tasks | top [/b] [/O:key] - FreeRTOS task list (name, state, prio, core, stack, CPU%)" },
    { "tasks",    "tasks - alias of ps (FreeRTOS task list)" },
    { "top",      "top [/b] [/O:key] - task list sorted by CPU with a live summary header" },
    { "brightness", "brightness <0-100> - set display backlight" },
    { "rotate",   "rotate <0|90|180|270> - rotate the display (touch coordinates follow)" },
    { "battery",  "battery - read battery voltage/percent from the ADC divider" },
    { "power",    "power [status] | power idle [seconds|off] - display power state and idle display-off timeout" },
    { "sleep",    "sleep - enter light sleep (timer/GPIO wake); display-only idle is power/display off" },
    { "deepsleep", "deepsleep - enter ESP deep sleep (wake on configured source)" },
    { "pwm",      "pwm <pin> <freq> <duty%> - LEDC PWM tone on a pin" },
    { "freq",     "freq <pin> <hz> - square wave generator on a pin" },
    { "tone",     "tone <freq> [duration_ms] - play a tone through the speaker" },
    { "wavplay",  "wavplay <file.wav> - play a WAV file from SD" },
    { "audio",    "audio status | audio stop - background playback state" },
    { "adc",      "adc <pin> - one-shot ADC read on a pin" },
    { "i2c",      "i2c scan | peek <addr> <reg> | poke <addr> <reg> <val> - I2C bus tools" },
    { "spi",      "spi status - SPI configuration (transactions unsupported with hosted SDIO)" },
    { "rgb",      "rgb status | rgb <#RRGGBB|r g b|effect> | rgb auto <on|off> - WS2812 status LED" },
    { "gpio",     "gpio list | status | read <pin> | set <pin> <0|1> - digital IO" },
    { "volume",   "volume <0-100> - set speaker volume" },
    { "display",  "display info | resolution | refresh | power <on|sleep|off>" },
    { "keyboard", "keyboard show | hide | toggle | status - on-screen keyboard" },
    { "windows",  "windows info - window layout state" },
    { "ui",       "ui tap|longpress|swipe|press|move|release|key|target|targets|hit|state - synthetic touch automation" },
    { "dialog",   "dialog [/t:secs] \"title\" \"message\" [button1] [button2] - message box (ERRORLEVEL 0/1/255)" },
    { "list",     "list [/t:secs] [/v:NAME] \"title\" item... - scrollable selector (ERRORLEVEL = 0-based index)" },
    { "ask",      "ask [/t:secs] [/v:NAME] [/p] \"prompt\" [default] - text input to ASK_RESULT (ERRORLEVEL 0/1)" },
    { "browse",   "browse [/t:secs] [/v:NAME] [path] - fullscreen file picker (ERRORLEVEL 0/1)" },
    { "view",     "view [/t:secs] [--raw] <file> - text viewer pager (.md renders, .bmp/.dib open the image viewer) (ERRORLEVEL 0/1)" },
    { "open",     "open [/t:secs] [--raw] <file> - open by type: scripts in editor, md rendered, images viewed, rest as text (never executes)" },
    { "json",     "json validate|pretty <file> - check structure or print 2-space indented JSON (ERRORLEVEL 0/1)" },
    { "csv",      "csv rows|cols|cell|get|set|eval <file> [row col] [/b] [/v:NAME] - CSV grid (R1C1 refs, ranges, =EXPR via calc) (ERRORLEVEL 0/1/2)" },
    { "export",   "export <db NAME|alarms> <csv|json|txt|vcf|ics> <file> - portable store interchange (ERRORLEVEL 0/1/2)" },
    { "import",   "import db <name> <csv|json|vcf> <file> | import alarms <csv|json|ics> <file> - store interchange in, fresh ids (ERRORLEVEL 0/1/2)" },
    { "archive",  "archive create|extract|list|verify ... - USTAR backups with CRC manifest (ERRORLEVEL 0/1/2)" },
    { "backup",   "backup <file> [paths...] - archive DBS + alarms by default (ERRORLEVEL 0/1/2)" },
    { "crypt",    "crypt lock|unlock <src> <dst> [/p:pass|/ask] - password file encryption (ERRORLEVEL 0/1/2)" },
    { "hexview",  "hexview [/t:secs] <file> - 16-byte hex dump pager (ERRORLEVEL 0/1)" },
    { "image",    "image info <file.bmp> | image show [/t:secs] <file.bmp> - BMP metadata / fit-to-screen viewer (ERRORLEVEL 0/1/2)" },
    { "draw",     "draw <box|line|fill|text|bar|table|list|image|clear|window|save|restore|cursor|hold|alt-screen|close|refresh|fullscreen> - TUI drawing (foreground only)" },
    { "anchor",   "anchor <label> <command> [continue_line] - named transcript anchor region" },
    { "tui",      "tui status|clear|fullscreen|refresh - TUI control" },
    { "color",    "color [fg] [bg] - DOS COLOR parity (hex digits)" },
    { "locate",   "locate <row> <col> - DOS LOCATE parity (1-based, 80x25)" },
    { "config",   "config [KEY=VALUE | save | reset [key] | factory] - persistent settings (CONFIG.SYS)" },
    { "prompt",   "prompt [template] - set the command prompt template" },
    { "cd",       "cd | chdir [path] - show or change the working directory" },
    { "dir",      "dir [path] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order] - list directory" },
    { "copy",     "copy <src> <dst> - copy a file (dir target keeps basename)" },
    { "cursor",   "cursor [block|bar] [blink <ms 0..2000|off>] - input-line cursor style (session-only)" },
    { "move",     "move <src> <dst> - move a file (cwd-relative destination)" },
    { "del",      "del [/s] [/p|/f|/permanent] <path> - delete file(s); aliases erase" },
    { "ren",      "ren | rename <src> <dst> - rename a file" },
    { "md",       "md | mkdir <path> - create a directory" },
    { "rd",       "rd | rmdir <path> - remove a directory" },
    { "type",     "type <path> - print a file to the transcript" },
    { "write",    "write <path> <text> - write (overwrite) a file" },
    { "append",   "append <path> <text> - append text to a file" },
    { "touch",    "touch <path> - create/update a file timestamp" },
    { "attrib",   "attrib [+-RHSA] <path> - show/set file attributes" },
    { "label",    "label [name] - show/set the volume label" },
    { "xcopy",    "xcopy <src> <dst> [/S] [/E] [/I] [/Y] - recursive copy" },
    { "chkdsk",   "chkdsk | scandisk [path] [/F] - read-only filesystem check" },
    { "format",   "format [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q] - format the SD volume (interactive)" },
    { "find",     "find <text> [file] [/I] [/N] [/C] [/V] - text search, or recursive file discovery (/NAME: et al)" },
    { "findstr",  "findstr [switches] <search> [file...] - literal or regex-lite text search (/R /C /I /N /V /X /E /B /L /S /M /F /G)" },
    { "more",     "more [file] - paginated output (keypress- or timer-bounded)" },
    { "tree",     "tree [path] [/F] [/A] - directory tree" },
    { "fc",       "fc <f1> <f2> - compare two files" },
    { "font",     "font info | font coverage | font list | font set <terminal|ui> <name> [/save] | font size <terminal|ui> <px> [/save] - roles, coverage, live TTF switching + sizes" },
    { "theme",    "theme list | show [name] | set <name> [/save] - switch the UI color theme (persists in SHELL.INI)" },
    { "comp",     "comp <f1> <f2> - compare two files byte-by-byte" },
    { "sort",     "sort [file] [/R] [/I] [/U] - sort lines (pipes: sort < f | sort)" },
    { "clip",     "clip [text | copy [N] | file <path> | read <file> | paste <dest>] - clipboard" },
    { "history",  "history [ /save [file] | /load [file] | /search <text> | /clear ] - command recall buffer" },
    { "edit",     "edit <file> - modal text editor (byte-preserving document model)" },
    { "trash",    "trash - show trash | trash <path> - move to trash | trash empty - empty it" },
    { "undelete", "undelete <path> - restore a file from trash" },
    { "receive",  "receive <path> <size> [/crc] - host-to-device binary transfer (USB serial)" },
    { "send",     "send <path> [offset] [count] | send /diag - device-to-host binary transfer" },
    { "screenshot", "screenshot | scr | capture [file.bmp] - capture the screen to a BMP" },
    { "sd",       "sd info | ls [path] | stat <path> | cat <path> [bytes] | mount | eject" },
    { "sdeject",  "sdeject - safe SD unmount before card removal" },
    { "disk",     "disk list | detail | clean | create partition primary [size=N] | delete partition N | format" },
    { "set",      "set [NAME=VALUE] | set /a NAME=<expr> | set /p NAME=<prompt> - environment variables" },
    { "calc",     "calc [NAME=] <expr> | calc /deg | /rad | /angle | /hex | /fin | /date - float calculator (BASIC/financial/date funcs, PI, RAN#, &H hex)" },
    { "path",     "path [dirs] - show/set the executable search path" },
    { "echo",     "echo <text> | echo on|off - print text or toggle command echo" },
    { "call",     "call <file.bat> [args] - run a batch file from another" },
    { "if",       "if [not] errorlevel|exist|\"a\"==\"b\" <cmd> - conditional execution" },
    { "goto",     "goto :label - jump to a label in a batch file" },
    { "shift",    "shift - shift batch arguments" },
    { "pause",    "pause [message] - wait for a key (30 s timeout)" },
    { "choice",   "choice [/C:keys] [/N] [/T:c,secs] [/S] [text] - interactive selection" },
    { "delay",    "delay <ms> - pure deterministic wait (melodies/demos), clamped to P4_CONFIG_DELAY_MAX_MS" },
    { "gfx",      "gfx init|close|status|clear|pixel|line|rect|circle|hline|vline|triangle|ellipse|polygon|fill|text|show|image|load|blit|free|slots|save - RGB565 canvas + toolkit (max 320x240); image <file.bmp> blits a scaled BMP; load ingests 24/32-bit BMP sprites (max 64x64)" },
    { "plot",     "plot tui|window|auto|axes|func|polar|para|data|bar|table|line|point|clear|status - world-coordinate graphs + charts on the gfx canvas or TUI (uses calc)" },
    { "crc32",    "crc32 <path> - print a file's CRC-32 checksum (ERRORLEVEL 0/1)" },
    { "asset",    "asset check|list <app> - verify/list an app's APPS/<APP>.ASSETS manifest (ERRORLEVEL 0/1)" },
    { "pkg",      "pkg list|info <app>|verify <app>|check|install <app>|remove <app> - SD app packages (APPS/<APP>.APPINFO + .ASSETS, bundles under PKGS/)" },
    { "header",   "header [status] | mode [auto|full|compact] [/save] | show|hide - responsive status-bar layout" },
    { "start",    "start <command> [args] - run a command or batch file as a background job (see taskkill)" },
    { "taskkill", "taskkill <job> - cooperatively stop a background job (name like bg0, or slot number)" },
    { "for",      "for %v in (set) do <cmd> | for /f \"delims= tokens=\\n\" %%v in (file) do <cmd> - loops" },
    { "setlocal", "setlocal - begin a local environment scope" },
    { "endlocal", "endlocal - end a local environment scope" },
    { "exit",     "exit [/b] [code] - leave a batch file or the shell" },
    { "proc",     "proc [/args | /name | /depth | /errorlevel | /echo | /stdin] - introspect the batch process stack" },
    { "ini",      "ini <list|get|set|del|load|save> <file> [key] [value] - persistent state in KEY=VALUE files on the SD card" },
    { "appconfig", "appconfig <app> [path|list|get|set|del] [key] [value] - per-app settings (sd:/APPS/<APP>.INI)" },
    { "temp",     "temp [new [ext] | clean] - SD-backed temporary files" },
    { "ansi",     "ansi <sgr-codes> [text...] - emit ANSI-styled text (reverse, bold, color) into the transcript" },
    { "menu",     "menu <item> [item...] - numbered menu; ERRORLEVEL = chosen index (0 = cancel)" },
    { "markdown", "markdown <file> | markdown -e <text> | markdown on | off - render Markdown (echo/type auto-render; /raw bypasses)" },
    { "notify",   "notify [/t:secs] <text> | notify - - header notification (clear with -)" },
    { "appmode",  "appmode on [/full] [/clear] | appmode off | appmode status - enter/exit app mode (save/restore screen)" },
    { "rem",      "rem <text> - batch comment" },
    { "alias",    "alias [name[=value]] | alias /save [/load] [file] - DOSKEY-style macros" },
    { "unalias",  "unalias <name> - remove a macro alias" },
    { "bind",     "bind [F1..F12|^A..^Z <line>] | bind unbind <key> | bind /save [/load] [file] | bind /clear - key bindings (^C reserved)" },
    { "macro",    "macro record [file] | macro stop | macro play <file> | macro status - capture/replay command macros" },
    { "date",     "date [MM-DD-YYYY] - show/set the date" },
    { "db",       "db <create|list|info|drop|open|close|categories|add|get|set|del|purge|count|find|export|import> [...] - Palm-OS-style SD record store" },
    { "alarm",    "alarm <add|list|del|enable|disable|snooze|status|purge> [...] - SD-persisted alarms (daily/weekly/monthly/yearly, header notify / beep / LED / optional run)" },
    { "cal",      "cal [today|week|next|YYYY-MM] - calendar grid + events over the alarm store" },
    { "gfind",    "gfind <text> [/b] [/i] [/db:name] [/noalarms] [/nodb] - search db records + alarms (Palm-style global find)" },
    { "time",     "time [HH:MM[:SS]] - show/set the time" },
    { "timer",    "timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME] - named stopwatch runs" },
    { "timezone", "timezone - show/set the timezone" },
    { "sntp",     "sntp | ntpsync [server] - sync the clock via SNTP" },
    { "rtc",      "rtc [anchor] - RTC backup status (source, anchor age, ext chip)" },
    { "wifi",     "wifi status | scan [/b] | diag | connect [ssid pass] | disconnect" },
    { "bluetooth", "bluetooth | bt status | scan [limit] | advertise <on [name]|off>" },
    { "usb",      "usb status | ls [path] | keyboard <on|off> | mouse <on|off> | userial <status|open|close|send|recv|term>" },
    { "httpd",    "httpd status | start | stop - HTTP file server on port 80" },
    { "netstat",  "netstat - active TCP/UDP connections and listeners" },
    { "ipconfig", "ipconfig [/all] - IP configuration" },
    { "ping",     "ping <host-or-ip> [count] - ICMP echo" },
    { "dns",      "dns | nslookup <hostname> - resolve a hostname" },
    { "httpget",  "httpget | wget <url> [localfile] - download over HTTP(S)" },
    { "tcpterm",  "tcpterm <host> <port> [/t:secs] [text...] - one-shot TCP request/response (ERRORLEVEL 0/1/2)" },
    { "c6ota",    "c6ota <sd:/path | http[s]://url | default> - OTA the ESP32-C6 firmware" },
    { "camera",   "camera init | snap <filename> - camera stack (stub: not wired)" },
};

#define SHELL_HELP_ENTRY_COUNT \
    ((int)(sizeof(s_shell_help_entries) / sizeof(s_shell_help_entries[0])))

/** Print one help entry with the given label prefix. */
static void shell_help_print_entry(const shell_help_entry_t *e)
{
    if (e == NULL) {
        return;
    }
    shell_transcript_appendf_ansi("  " SH_EXE "%-12s" SH_RST " %s\n", e->name, e->summary);
}

size_t shell_help_entry_count(void)
{
    return (size_t)SHELL_HELP_ENTRY_COUNT;
}

bool shell_help_entry_get(size_t index, const char **name, const char **usage)
{
    if (index >= (size_t)SHELL_HELP_ENTRY_COUNT) {
        return false;
    }
    if (name != NULL) {
        *name = s_shell_help_entries[index].name;
    }
    if (usage != NULL) {
        *usage = s_shell_help_entries[index].summary;
    }
    return true;
}

void shell_command_help(int argc, char **argv)
{
    /* help <cmd> - print one entry */
    if (argc >= 2 && argv[1] != NULL &&
        argv[1][0] != '\0' && argv[1][0] != '/') {
        int i;
        for (i = 0; i < SHELL_HELP_ENTRY_COUNT; i++) {
            if (shell_text_equals_ignore_case(argv[1], s_shell_help_entries[i].name)) {
                shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell: %s" SH_RST "\n",
                                              s_shell_help_entries[i].name);
                shell_help_print_entry(&s_shell_help_entries[i]);
                shell_transcript_appendf_ansi(SH_MUTE "Run " SH_EXE "help /all" SH_RST
                                              SH_MUTE " to list every command.\n" SH_RST);
                return;
            }
        }
        shell_transcript_appendf_ansi(SH_ERR "help: unknown command '%s'\n" SH_RST,
                                      argv[1]);
        shell_transcript_appendf_ansi(SH_MUTE "Try " SH_EXE "help /all" SH_RST
                                      SH_MUTE " for the full command list.\n" SH_RST);
        return;
    }

    /* help /all - full offline command reference */
    if (argc >= 2 && argv[1] != NULL &&
        (shell_text_equals_ignore_case(argv[1], "/all") ||
         shell_text_equals_ignore_case(argv[1], "/?"))) {
        int i;
        shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell Command Reference (" SH_RST
                                      SH_NUM "%d" SH_RST SH_SUBHEAD " commands)" SH_RST "\n",
                                      SHELL_HELP_ENTRY_COUNT);
        for (i = 0; i < SHELL_HELP_ENTRY_COUNT; i++) {
            shell_help_print_entry(&s_shell_help_entries[i]);
        }
        shell_transcript_appendf_ansi(SH_MUTE "Offline reference: every command above is runnable. "
                                      "Run " SH_EXE "help <command>" SH_RST SH_MUTE
                                      " for a single line.\n" SH_RST);
        return;
    }

    /* help - quick summary (existing curated list) */
    shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell Commands:" SH_RST "\n");
    if (s_command_ops.sd_is_mounted != NULL && !s_command_ops.sd_is_mounted()) {
        shell_transcript_appendf_ansi(SH_MUTE "No SD card detected - insert a microSD card to use files, "
                                      "config, and scripts.\n" SH_RST);
    }
    shell_transcript_appendf_ansi("  " SH_EXE "help" SH_RST " | sysinfo | clear/cls | reboot | version/ver | about | debug | mem\n");
    shell_transcript_appendf_ansi("  " SH_EXE "ps" SH_RST " | " SH_EXE "tasks" SH_RST " | " SH_EXE "top" SH_RST " [/b] - FreeRTOS task list (name, state, priority, core, stack, CPU%)\n");
    shell_transcript_appendf_ansi("  " SH_EXE "brightness" SH_RST " <0-100> | " SH_EXE "rotate" SH_RST " <0|90|180|270> | " SH_EXE "battery" SH_RST " | " SH_EXE "volume" SH_RST " <0-100>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "config" SH_RST " [KEY=VALUE|save|reset [key]|factory] - persistent settings (CONFIG.SYS)\n");
    shell_transcript_appendf_ansi("  " SH_EXE "gpio" SH_RST " list | status | read <pin> | set <pin> <0|1>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "cd" SH_RST " [path] | " SH_EXE "copy" SH_RST " <src> <dst> | " SH_EXE "move" SH_RST " <src> <dst>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "dir" SH_RST " [path] [/W] [/P] [/S] [/B] [/L] [/A:attrs] [/O:order]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "del" SH_RST " <path> | " SH_EXE "ren" SH_RST " <src> <dst> | " SH_EXE "md" SH_RST " <path> | " SH_EXE "rd" SH_RST " <path>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "type" SH_RST " <path> | " SH_EXE "write" SH_RST " <path> <text> | " SH_EXE "append" SH_RST " <path> <text> | " SH_EXE "touch" SH_RST " <path>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "set" SH_RST " [NAME=VALUE] | " SH_EXE "set /a" SH_RST " NAME=<expr> | " SH_EXE "set /p" SH_RST " NAME=<prompt>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "calc" SH_RST " [NAME=] <expr> | " SH_EXE "calc" SH_RST " /deg | /rad | /angle | /hex - float calculator (ABS SIN COS TAN PI RAN# LEN HEX$ ...)\n");
    shell_transcript_appendf_ansi("  " SH_EXE "alias" SH_RST " [name[=value]] | " SH_EXE "alias /save" SH_RST " [/load] [file] | " SH_EXE "unalias" SH_RST " <name>  (DOSKEY-style macros)\n");
    shell_transcript_appendf_ansi("  " SH_EXE "path" SH_RST " [dirs] | " SH_EXE "echo" SH_RST " <text> | " SH_EXE "echo" SH_RST " on|off | " SH_EXE "call" SH_RST " <file.bat>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "attrib" SH_RST " [+-RHSA] <path> | " SH_EXE "label" SH_RST " [name] | " SH_EXE "xcopy" SH_RST " <src> <dst> [/S]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "chkdsk" SH_RST " [path] [/F] | " SH_EXE "format" SH_RST " [/FS:FAT|FAT32] [/A:size] [/V:label] [/Q]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "if" SH_RST " [not] errorlevel|exist|\"a\"==\"b\" cmd | " SH_EXE "goto" SH_RST " <label> | " SH_EXE "shift" SH_RST " | " SH_EXE "exit" SH_RST " [/b] [code]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "pause" SH_RST " | " SH_EXE "choice" SH_RST " [/C:keys] [/N] [/T:c,secs] [/S] [text] | " SH_EXE "setlocal" SH_RST " | " SH_EXE "endlocal" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_EXE "launch" SH_RST " | " SH_EXE "launch" SH_RST " <name> [args] | " SH_EXE "launch" SH_RST " /list | " SH_EXE "apps" SH_RST " | " SH_EXE "delay" SH_RST " <ms>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "prompt" SH_RST " [template] | " SH_EXE "date" SH_RST " [MM-DD-YYYY] | " SH_EXE "time" SH_RST " [HH:MM[:SS]]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "find" SH_RST " <text> [file] [/I] [/N] [/C] [/V] | " SH_EXE "more" SH_RST " [file] | " SH_EXE "fc" SH_RST " <f1> <f2>\n");
    shell_transcript_appendf_ansi("  " SH_EXE "find" SH_RST " [path] [/NAME:pat] [/SIZE:spec] [/NEWER:date] [/OLDER:date] [/DIRS] [/B] - recursive file discovery\n");
    shell_transcript_appendf_ansi("  " SH_EXE "tree" SH_RST " [path] [/F] [/A] | " SH_EXE "sort" SH_RST " [file] [/R] [/I] [/U]\n");
    shell_transcript_appendf_ansi("  " SH_EXE "sd" SH_RST " info | ls [path] | stat <path> | cat <path> [bytes] | " SH_EXE "sdeject" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_EXE "disk" SH_RST " list | detail | clean | create partition primary [size=N] | delete partition N | format\n");
    shell_transcript_appendf_ansi("  " SH_EXE "wifi" SH_RST " status | scan [/b] | diag | connect [ssid pass] | disconnect\n");
    shell_transcript_appendf_ansi("  " SH_EXE "ping" SH_RST " <host-or-ip> [count] | " SH_EXE "dns" SH_RST " <hostname> (nslookup)\n");
    shell_transcript_appendf_ansi("  " SH_EXE "httpget" SH_RST " <url> [localfile]  (alias " SH_EXE "wget" SH_RST ")\n");
    shell_transcript_appendf_ansi("  " SH_EXE "bluetooth" SH_RST " status | scan [limit] | advertise <on [name]|off>\n");
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
    shell_transcript_appendf_ansi(SH_SUBHEAD "Getting started:" SH_RST " " SH_EXE "dir" SH_RST " | " SH_EXE "cd" SH_RST " <path> | "
                                  SH_EXE "write" SH_RST " <file> <text> | " SH_EXE "type" SH_RST " <file> | "
                                  SH_EXE "edit" SH_RST " <file> | " SH_EXE "config" SH_RST " | "
                                  SH_EXE "wifi connect" SH_RST " <ssid> <pass>\n");
    shell_transcript_appendf_ansi(SH_MUTE "Full offline reference: " SH_EXE "help /all" SH_RST
                                  SH_MUTE " | one command: " SH_EXE "help <command>" SH_RST "\n");
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

    shell_transcript_appendf_ansi(SH_SUBHEAD SH_BOLD "P4MiniShell System Information:" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "board.requested_name:" SH_RST " %s\n", SHELL_BOARD_REQUESTED);
    shell_transcript_appendf_ansi("  " SH_LBL "board.detected_name:" SH_RST " %s\n", SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  " SH_LBL "version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    {
        const esp_app_desc_t *d = esp_app_get_description();
        const char *date = (d != NULL) ? d->date : "n/a";
        const char *time = (d != NULL) ? d->time : "n/a";
        const char *git  = (d != NULL) ? d->version : "n/a";
        shell_transcript_appendf_ansi("  " SH_LBL "build:" SH_RST " %s %s\n", date, time);
        shell_transcript_appendf_ansi("  " SH_LBL "git:" SH_RST " %s\n", git);
    }
    if (time_is_synchronized()) {
        shell_transcript_appendf_ansi("  " SH_LBL "time:" SH_RST " %s %s " SH_OK "(synced)" SH_RST "\n",
                                 time_get_formatted(),
                                 time_get_timezone());
    } else {
        shell_transcript_appendf_ansi("  " SH_LBL "time:" SH_RST " %s %s " SH_WARN "(unsynced)" SH_RST "\n",
                                 time_get_formatted(),
                                 time_get_timezone());
    }

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
        char power_str[24];
        shell_colour_value(power_str, sizeof(power_str),
                           disp_info.power_state == DISPLAY_POWER_ON ? SH_OK :
                           disp_info.power_state == DISPLAY_POWER_SLEEP ? SH_WARN : SH_ERR,
                           disp_info.power_state == DISPLAY_POWER_ON ? "on" :
                           disp_info.power_state == DISPLAY_POWER_SLEEP ? "sleep" : "off");
        shell_transcript_appendf_ansi("  " SH_LBL "display.state:" SH_RST " brightness=%d%% rotation=%s power=%s refresh=%" PRIu32 "Hz\n",
                                 disp_info.brightness_percent,
                                 display_rotation_to_string(disp_info.rotation),
                                 power_str,
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
    shell_transcript_appendf_ansi("  " SH_LBL "c6.hosted_transport:" SH_RST " sdio reset_gpio=%d\n",
                                 P4_CONFIG_C6_HOST_RESET_GPIO);
    if (s_command_ops.c6ota_is_busy != NULL && s_command_ops.c6ota_is_busy()) {
        shell_transcript_appendf_ansi("  " SH_LBL "c6.hosted_transport.busy:" SH_RST " " SH_WARN "yes" SH_RST "\n");
    } else {
        shell_transcript_appendf_ansi("  " SH_LBL "c6.hosted_transport.busy:" SH_RST " " SH_OK "no" SH_RST "\n");
    }
        shell_transcript_appendf_ansi("  " SH_LBL "idf:" SH_RST " %s\n", esp_get_idf_version());
        shell_transcript_appendf_ansi("  " SH_LBL "freertos:" SH_RST " tasks=%" PRIu32 " uptime=%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                                 task_count, days, hours, mins, secs);
        shell_transcript_appendf_ansi("  " SH_LBL "heap:" SH_RST " free=%u bytes, internal_free=%u bytes, total=%u bytes (",
                                 (unsigned int)free_heap, (unsigned int)free_internal,
                                 (unsigned int)total_heap);
        if (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) {
            shell_transcript_appendf_ansi(SH_ERR "%u%%%%" SH_RST ")\n", heap_pct);
        } else {
            shell_transcript_appendf_ansi(SH_OK "%u%%%%" SH_RST ")\n", heap_pct);
        }
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

    shell_transcript_appendf_ansi(SH_HEAD "%s" SH_RST "\n", SHELL_BOOT_MESSAGE);
    shell_transcript_appendf_ansi("  " SH_LBL "version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    {
        const esp_app_desc_t *d = esp_app_get_description();
        const char *date = (d != NULL) ? d->date : "n/a";
        const char *time = (d != NULL) ? d->time : "n/a";
        const char *git  = (d != NULL) ? d->version : "n/a";
        shell_transcript_appendf_ansi("  " SH_LBL "build:" SH_RST " %s %s\n", date, time);
        shell_transcript_appendf_ansi("  " SH_LBL "git:" SH_RST " %s\n", git);
    }
    if (idf_ver != NULL) {
        shell_transcript_appendf_ansi("  " SH_LBL "idf:" SH_RST " %s\n", idf_ver);
    } else {
        shell_transcript_appendf_ansi("  " SH_LBL "idf:" SH_RST " " SH_ERR "unknown" SH_RST "\n");
    }
    shell_transcript_appendf_ansi("  " SH_LBL "board:" SH_RST " %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    shell_transcript_appendf_ansi("  " SH_LBL "chip:" SH_RST " %s rev %d, %d cores\n",
                             CONFIG_IDF_TARGET, 1, 2);
    char synced_str[24];
    shell_colour_value(synced_str, sizeof(synced_str),
                       time_is_synchronized() ? SH_OK : SH_WARN,
                       time_is_synchronized() ? "(synced)" : "(unsynced)");
    shell_transcript_appendf_ansi("  " SH_LBL "time:" SH_RST " %s %s " SH_RST "%s\n",
                             time_get_formatted(),
                             time_get_timezone(),
                             synced_str);
    shell_transcript_appendf_ansi("  " SH_LBL "uptime:" SH_RST " %" PRIu32 "s\n", uptime_sec);
    shell_transcript_appendf_ansi("  " SH_LBL "heap:" SH_RST " %u/%u bytes free (",
                             (unsigned int)free_heap, (unsigned int)total_heap);
    if (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) {
        shell_transcript_appendf_ansi(SH_ERR "%u%%%%" SH_RST ")\n", heap_pct);
    } else {
        shell_transcript_appendf_ansi(SH_OK "%u%%%%" SH_RST ")\n", heap_pct);
    }
    shell_transcript_appendf_ansi("  " SH_LBL "tasks:" SH_RST " %" PRIu32 "\n", task_count);
}

void shell_command_about(void)
{
    uint32_t task_count = uxTaskGetNumberOfTasks();
    int64_t uptime_us = esp_timer_get_time() - s_boot_timestamp_us;
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000ULL);
    const char *idf_ver = esp_get_idf_version();

    shell_transcript_appendf_ansi(SH_HEAD SH_BOLD "%s" SH_RST " - Embedded DOS-style command shell\n", P4_CONFIG_PRODUCT_NAME);
    shell_transcript_appendf_ansi("  " SH_LBL "about.version:" SH_RST " %d.%d.%d\n",
                             P4_CONFIG_VERSION_MAJOR,
                             P4_CONFIG_VERSION_MINOR,
                             P4_CONFIG_VERSION_PATCH);
    {
        const esp_app_desc_t *d = esp_app_get_description();
        const char *date = (d != NULL) ? d->date : "n/a";
        const char *time = (d != NULL) ? d->time : "n/a";
        const char *git  = (d != NULL) ? d->version : "n/a";
        shell_transcript_appendf_ansi("  " SH_LBL "about.build:" SH_RST " %s %s\n", date, time);
        shell_transcript_appendf_ansi("  " SH_LBL "about.git:" SH_RST " %s\n", git);
    }
    shell_transcript_appendf_ansi("  " SH_LBL "about.board:" SH_RST " %s (%s)\n", SHELL_BOARD_REQUESTED, SHELL_BOARD_DETECTED);
    if (idf_ver != NULL) {
        shell_transcript_appendf_ansi("  " SH_LBL "about.idf:" SH_RST " %s\n", idf_ver);
    } else {
        shell_transcript_appendf_ansi("  " SH_LBL "about.idf:" SH_RST " " SH_ERR "unknown" SH_RST "\n");
    }
    shell_transcript_appendf_ansi("  " SH_LBL "about.display:" SH_RST " JD9165 1024x600 MIPI-DSI, GT911 touch\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.ui:" SH_RST " locked transcript with touch keyboard, history buttons, and command prompt\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.header:" SH_RST " real-time status bar (WiFi, BT, USB, SD, MEM, CPU, BAT) from FreeRTOS\n");
    shell_transcript_appendf_ansi("  " SH_LBL "about.uptime:" SH_RST " %" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n",
                             uptime_sec / 86400,
                             (uptime_sec % 86400) / 3600,
                             (uptime_sec % 3600) / 60,
                             uptime_sec % 60);
    shell_transcript_appendf_ansi("  " SH_LBL "about.tasks:" SH_RST " %" PRIu32 "\n", task_count);

    /* License: proprietary notice + third-party summary (always surfaced). */
    shell_transcript_appendf_ansi(SH_SUBHEAD "License" SH_RST "\n");
    shell_transcript_appendf_ansi("  " SH_LBL "license:" SH_RST " %s\n", P4_CONFIG_COPYRIGHT_NOTICE);
    shell_transcript_appendf_ansi("  " SH_MUTE "This firmware is proprietary software. Redistribution or "
                                  "modification without written permission is not permitted.\n" SH_RST);
    shell_transcript_appendf_ansi(SH_SUBHEAD "Third-party components" SH_RST "\n");
    shell_transcript_appendf_ansi("  ESP-IDF / esp_app_format (Espressif) - Apache-2.0\n");
    shell_transcript_appendf_ansi("  LVGL (LVGL Kft) - MIT\n");
    shell_transcript_appendf_ansi("  esp_lvgl_port, esp_hosted, esp_wifi_remote, led_strip, board BSP (Espressif) - Apache-2.0\n");
    shell_transcript_appendf_ansi("  FreeRTOS kernel - MIT | lwIP - BSD-3-Clause | FatFs - BSD-1-Clause\n");
    shell_transcript_appendf_ansi("  protobuf-c - BSD-2-Clause | usb host stack (Espressif) - Apache-2.0\n");
    shell_transcript_appendf_ansi("  " SH_MUTE "Full license texts ship in the project's managed_components/ "
                                  "directories.\n" SH_RST);
}

void shell_command_mem(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t task_count = uxTaskGetNumberOfTasks();
    unsigned int heap_pct = (unsigned int)(total_heap > 0 ? (free_heap * 100 / total_heap) : 0);

    shell_transcript_appendf_ansi("  " SH_LBL "mem.heap:" SH_RST " free=%u bytes, total=%u bytes (",
                             (unsigned int)free_heap, (unsigned int)total_heap);
    if (heap_pct < P4_CONFIG_HEADER_MEM_LOW_PCT) {
        shell_transcript_appendf_ansi(SH_ERR "%u%%%%" SH_RST ")\n", heap_pct);
    } else {
        shell_transcript_appendf_ansi(SH_OK "%u%%%%" SH_RST ")\n", heap_pct);
    }
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
 * TASK INTROSPECTION (ps / tasks / top)
 * ========================================================================
 * Read-only FreeRTOS task listing surfaced by `ps`, `tasks`, and `top`. It
 * reads a uxTaskGetSystemState() snapshot (heap-allocated, capped by
 * P4_CONFIG_TASK_SNAPSHOT_MAX) and prints each task's name, state, priority,
 * core, stack high-water mark (minimum free stack since creation) and, for
 * `top`, the CPU share since the previous sample. No task is modified, so the
 * feature is inherently read-only and safe from the command worker task.
 */

/** One entry of the previous-runtime bookkeeping used to compute per-task CPU
 *  share between two `ps`/`top` samples. Keyed by the task's unique
 *  xTaskNumber so a deleted-and-recreated task is treated as a fresh sample. */
typedef struct {
    UBaseType_t task_number;   /**< xTaskNumber at the last sample. */
    uint32_t runtime;          /**< ulRunTimeCounter at the last sample. */
} shell_task_runtime_sample_t;

static shell_task_runtime_sample_t s_task_runtime_samples[P4_CONFIG_TASK_SNAPSHOT_MAX];
static int s_task_runtime_sample_count;
static uint32_t s_task_runtime_total;
static bool s_task_runtime_sample_valid;

/** Map a FreeRTOS eTaskState to a short three-letter label for the table. */
static const char *shell_task_state_label(eTaskState state)
{
    switch (state) {
    case eRunning:    return "RUN";
    case eReady:      return "RDY";
    case eBlocked:    return "BLK";
    case eSuspended:  return "SUS";
    case eDeleted:    return "DEL";
    default:          return "?";
    }
}

/**
 * Compute the CPU share (%) of one task since the previous `ps`/`top` sample
 * by diffing its run-time counter against the stored value (both are monotonic
 * unsigned counters, so wrapping deltas are still correct). Returns 0 when
 * there is no previous sample, the total runtime did not advance, or the task
 * is new.
 */
static int shell_task_cpu_percent(UBaseType_t task_number, uint32_t runtime,
                                  uint32_t total_runtime)
{
    int index;

    if (!s_task_runtime_sample_valid || total_runtime <= s_task_runtime_total) {
        return 0;
    }
    for (index = 0; index < s_task_runtime_sample_count; index++) {
        if (s_task_runtime_samples[index].task_number == task_number) {
            uint32_t task_delta = runtime - s_task_runtime_samples[index].runtime;
            uint32_t total_delta = total_runtime - s_task_runtime_total;
            if (total_delta == 0) {
                return 0;
            }
            return (int)((task_delta * 100) / total_delta);
        }
    }
    return 0;
}

/**
 * Compare two task rows for `/O:` sorting. Numeric keys compare numerically,
 * state compares the three-letter label; the name is the deterministic
 * tie-breaker for every key. Exposed (non-static) for the unit tests.
 */
int shell_task_row_compare(const shell_task_row_t *a, const shell_task_row_t *b,
                           shell_task_sort_key_t key, bool reverse)
{
    int result = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }

    switch (key) {
    case SHELL_TASK_SORT_CPU:
        if (a->cpu_percent < b->cpu_percent) result = -1;
        else if (a->cpu_percent > b->cpu_percent) result = 1;
        break;
    case SHELL_TASK_SORT_STACK:
        if (a->highwater_bytes < b->highwater_bytes) result = -1;
        else if (a->highwater_bytes > b->highwater_bytes) result = 1;
        break;
    case SHELL_TASK_SORT_PRIORITY:
        if (a->priority < b->priority) result = -1;
        else if (a->priority > b->priority) result = 1;
        break;
    case SHELL_TASK_SORT_STATE:
        result = strcmp(a->state, b->state);
        break;
    case SHELL_TASK_SORT_NAME:
    default:
        result = 0;
        break;
    }

    /* Name is the tie-breaker so the order is deterministic. */
    if (result == 0) {
        result = strcmp(a->name, b->name);
    }

    return reverse ? -result : result;
}

/* qsort() takes no user pointer; the active sort settings live here on the
 * single command worker task, exactly like the `dir /O:` comparator. */
static shell_task_sort_key_t s_task_sort_key;
static bool s_task_sort_reverse;

static int shell_task_row_qsort_cmp(const void *left, const void *right)
{
    return shell_task_row_compare((const shell_task_row_t *)left,
                                  (const shell_task_row_t *)right,
                                  s_task_sort_key, s_task_sort_reverse);
}

/**
 * Parse a `/O:` value the same way `dir` does: each sort letter overwrites the
 * active key (last wins), a `-` prefix selects descending order. Returns false
 * on an unknown letter.
 */
static bool shell_task_parse_sort(const char *value, shell_task_sort_key_t *key,
                                  bool *reverse)
{
    while (*value != '\0') {
        if (*value == '-') {
            *reverse = true;
            value++;
            continue;
        }
        switch (toupper((unsigned char)*value)) {
        case 'N': *key = SHELL_TASK_SORT_NAME;     break;
        case 'C': *key = SHELL_TASK_SORT_CPU;      break;
        case 'S': *key = SHELL_TASK_SORT_STACK;    break;
        case 'P': *key = SHELL_TASK_SORT_PRIORITY; break;
        case 'T': *key = SHELL_TASK_SORT_STATE;    break;
        default:
            return false;
        }
        value++;
    }
    return true;
}

/**
 * `ps` / `tasks` / `top` — list FreeRTOS tasks and their CPU usage.
 *
 * Usage: ps|tasks|top [/b] [/O:key]
 *   - no argument: colour-coded table (name, state, priority, core, stack
 *     high-water bytes, CPU% since the previous sample). `top` also prints a
 *     summary line with the task count, free heap, and uptime.
 *   - `/b`: uncoloured machine-parsable rows ("name state prio core headb cpu")
 *     suitable for redirection / pipes.
 *   - `/O:key`: sort by N (name), C (CPU), S (stack), P (priority), or T
 *     (state); a `-` prefix reverses. `top` defaults to CPU descending, while
 *     `ps`/`tasks` keep the FreeRTOS order unless `/O:` is given.
 *
 * Returns an ERRORLEVEL: 0 ok, 1 snapshot failure, 2 usage.
 */
int shell_command_ps(int argc, char **argv)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    uint32_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_status_array;
    shell_task_row_t *rows;
    uint32_t total_runtime = 0;
    uint32_t obtained;
    bool bare = false;
    bool is_top = false;
    bool have_sort = false;
    shell_task_sort_key_t sort_key = SHELL_TASK_SORT_NAME;
    bool sort_reverse = false;
    uint32_t i;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (argv[i][0] == '/' && (argv[i][1] == 'b' || argv[i][1] == 'B') && argv[i][2] == '\0') {
            bare = true;
            continue;
        }
        if (argv[i][0] == '/' && (argv[i][1] == 'o' || argv[i][1] == 'O')) {
            const char *value = argv[i] + 2;

            if (*value == ':' || *value == '=') {
                value++;
            }
            if (*value == '\0') {
                sort_key = SHELL_TASK_SORT_NAME;
            } else if (!shell_task_parse_sort(value, &sort_key, &sort_reverse)) {
                shell_print_error("ps: unknown /O key in %s", argv[i]);
                shell_print_usage("Usage: ps|tasks|top [/b] [/O:N|C|S|P|T]  (- prefix reverses)");
                return 2;
            }
            have_sort = true;
            continue;
        }
        shell_print_error("ps: unknown option %s", argv[i]);
        shell_print_usage("Usage: ps|tasks|top [/b] [/O:N|C|S|P|T]  (- prefix reverses)");
        return 2;
    }
    if (argc > 0) {
        is_top = shell_text_equals_ignore_case(argv[0], "top");
    }

    /* `top` behaves like real `top`: CPU-descending unless overridden. */
    if (!have_sort && is_top) {
        sort_key = SHELL_TASK_SORT_CPU;
        sort_reverse = true;
    }

    if (task_count == 0) {
        task_count = 1;
    }
    if (task_count > P4_CONFIG_TASK_SNAPSHOT_MAX) {
        task_count = P4_CONFIG_TASK_SNAPSHOT_MAX;
    }

    /* The TaskStatus_t array can exceed the 8 KB worker stack (one entry is
     * ~40 bytes), so it is always heap-allocated. */
    task_status_array = heap_caps_calloc(task_count, sizeof(TaskStatus_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_status_array == NULL) {
        shell_print_error("ps: out of memory for the task snapshot");
        return 1;
    }

    obtained = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);
    if (obtained == 0) {
        heap_caps_free(task_status_array);
        shell_print_error("ps: uxTaskGetSystemState returned no tasks");
        return 1;
    }

    /* Normalize the snapshot into lightweight rows so /O: can sort them with
     * a simple qsort (the CPU% is computed once per task here). */
    rows = calloc(obtained, sizeof(*rows));
    if (rows == NULL) {
        heap_caps_free(task_status_array);
        shell_print_error("ps: out of memory for the task rows");
        return 1;
    }

    for (i = 0; i < obtained; i++) {
        const TaskStatus_t *task = &task_status_array[i];
        int cpu = shell_task_cpu_percent(task->xTaskNumber,
                                         (uint32_t)task->ulRunTimeCounter,
                                         total_runtime);
        uint32_t highwater_bytes = task->usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
        int core = -1;

#if configTASKLIST_INCLUDE_COREID
        core = (int)task->xCoreID;
#endif
        /* Unpinned tasks report tskNO_AFFINITY (0x7FFFFFFF); show that as "-"
         * in the table and -1 in the machine-readable form. */
        if (core == (int)tskNO_AFFINITY) {
            core = -1;
        }

        snprintf(rows[i].name, sizeof(rows[i].name), "%s", task->pcTaskName);
        snprintf(rows[i].state, sizeof(rows[i].state), "%s",
                 shell_task_state_label(task->eCurrentState));
        rows[i].priority = (unsigned int)task->uxCurrentPriority;
        rows[i].core = core;
        rows[i].highwater_bytes = highwater_bytes;
        rows[i].cpu_percent = cpu;
    }

    if (have_sort || is_top) {
        s_task_sort_key = sort_key;
        s_task_sort_reverse = sort_reverse;
        qsort(rows, (size_t)obtained, sizeof(rows[0]), shell_task_row_qsort_cmp);
    }

    if (is_top && !bare) {
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);
        shell_transcript_appendf_ansi(SH_HEAD "Tasks (%u)" SH_RST " | "
                                      SH_LBL "heap free=" SH_RST SH_NUM "%u" SH_RST " | "
                                      SH_LBL "uptime=" SH_RST SH_NUM "%us" SH_RST "\n",
                                      (unsigned int)obtained,
                                      (unsigned int)free_heap,
                                      (unsigned int)uptime_sec);
    }

    if (!bare) {
        shell_transcript_appendf_ansi("  " SH_HEAD "%-16s" SH_RST " " SH_HEAD "%-3s" SH_RST " "
                                      SH_HEAD "Prio" SH_RST " " SH_HEAD "Core" SH_RST " "
                                      SH_HEAD "HeadB" SH_RST " " SH_HEAD "CPU%%" SH_RST "\n",
                                      "Name", "Sta");
    }

    for (i = 0; i < obtained; i++) {
        const shell_task_row_t *row = &rows[i];
        char core_label[16];

        snprintf(core_label, sizeof(core_label), "%d", row->core);

        if (bare) {
            shell_transcript_appendf("%s %s %u %s %u %d\n",
                                     row->name,
                                     row->state,
                                     row->priority,
                                     core_label,
                                     (unsigned int)row->highwater_bytes,
                                     row->cpu_percent);
        } else {
            shell_transcript_appendf_ansi("  " SH_VAL "%-16s" SH_RST " " SH_CMD "%-3s" SH_RST " "
                                          SH_NUM "%4u" SH_RST " " SH_VAL "%4s" SH_RST " "
                                          SH_NUM "%6u" SH_RST " " SH_NUM "%3d%%" SH_RST "\n",
                                          row->name,
                                          row->state,
                                          row->priority,
                                          core_label,
                                          (unsigned int)row->highwater_bytes,
                                          row->cpu_percent);
        }
    }

    /* Store the runtime bookkeeping for the next sample. */
    {
        int store = (int)obtained;
        if (store > P4_CONFIG_TASK_SNAPSHOT_MAX) {
            store = P4_CONFIG_TASK_SNAPSHOT_MAX;
        }
        for (i = 0; i < (uint32_t)store; i++) {
            s_task_runtime_samples[i].task_number = task_status_array[i].xTaskNumber;
            s_task_runtime_samples[i].runtime = (uint32_t)task_status_array[i].ulRunTimeCounter;
        }
        s_task_runtime_sample_count = store;
        s_task_runtime_total = total_runtime;
        s_task_runtime_sample_valid = true;
    }

    free(rows);
    heap_caps_free(task_status_array);
    return 0;
#else
    (void)argc;
    (void)argv;
    shell_print_muted("ps: FreeRTOS trace facility is disabled (CONFIG_FREERTOS_USE_TRACE_FACILITY)");
    return 0;
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
static int shell_sample_cpu_percent(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* Lightweight load estimate: the run-time stats clock is esp_timer (us), so
     * CPU% = 100 * (1 - idle_delta / wall_delta) using the idle counter of the
     * calling core. This deliberately AVOIDS uxTaskGetSystemState(): that full
     * task-list walk suspends scheduling long enough to delay the MIPI-DSI DMA
     * refill ISR and flashes the panel blue (the "BSOD" underrun). The `ps` /
     * `top` commands still use the full snapshot on demand. */
    static uint32_t s_last_idle_us;
    static int64_t s_last_total_us;
    uint32_t idle_us = (uint32_t)ulTaskGetIdleRunTimeCounter();
    int64_t total_us = esp_timer_get_time();
    int cpu_percent = 0;

    if (s_last_total_us != 0 && total_us > s_last_total_us) {
        uint32_t total_delta = (uint32_t)(total_us - s_last_total_us);
        uint32_t idle_delta = (idle_us >= s_last_idle_us)
                              ? (idle_us - s_last_idle_us) : 0;
        if (total_delta > 0) {
            cpu_percent = 100 - (int)((int64_t)idle_delta * 100 / total_delta);
            if (cpu_percent < 0) cpu_percent = 0;
            if (cpu_percent > 100) cpu_percent = 100;
        }
    }

    s_last_idle_us = idle_us;
    s_last_total_us = total_us;
    return cpu_percent;
#else
    /* Without runtime stats, approximate load from heap pressure. */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    int heap_pct = (total_heap > 0) ? (int)((free_heap * 100) / total_heap) : 100;
    int cpu_percent = 100 - heap_pct;

    if (cpu_percent < 0) cpu_percent = 0;
    if (cpu_percent > 100) cpu_percent = 100;
    return cpu_percent;
#endif
}

/* Telemetry cache: sampled by a dedicated low-priority task
 * (shell_telemetry_task), NOT on the LVGL task. The expensive work in this
 * block is uxTaskGetSystemState() (a full task-list walk) and the battery ADC
 * read; running them on the LVGL task every telemetry period stalled the
 * MIPI-DSI framebuffer fetch and flashed the panel blue (the "BSOD" DSI
 * underrun). The render task now reads only these cached values. */
static uint32_t s_tlm_free_heap;
static uint32_t s_tlm_total_heap;
static uint32_t s_tlm_task_count;
static int s_tlm_cpu_percent;
static int s_tlm_battery_percent;
static bool s_tlm_battery_ok;
static TaskHandle_t s_tlm_task;

/** Sample the expensive telemetry once. Runs on the telemetry task. */
static void shell_telemetry_sample(void)
{
    /* Skip during a C6 OTA: PSRAM is unavailable while the flash is written, so
     * the snapshot would only add contention, and the header is not updated. */
    if (s_command_ops.c6ota_is_busy != NULL && s_command_ops.c6ota_is_busy()) {
        return;
    }

    s_tlm_free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_tlm_total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    s_tlm_task_count = uxTaskGetNumberOfTasks();
    s_tlm_cpu_percent = shell_sample_cpu_percent();

    /* Battery: record failure too so the panel can show "BAT N/C" instead of a
     * stale percentage. */
    s_tlm_battery_ok = false;
    if (s_command_ops.battery_read != NULL &&
        s_command_ops.battery_read(NULL, &s_tlm_battery_percent, NULL, NULL) == ESP_OK) {
        s_tlm_battery_ok = true;
    }
}

static void shell_telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        shell_telemetry_sample();
        vTaskDelay(pdMS_TO_TICKS(P4_CONFIG_HEADER_TELEMETRY_PERIOD_MS));
    }
}

/** Start the telemetry sampler. Idempotent; called after command_init() so the
 *  shell ops table (heap/battery/c6ota accessors) is registered. The task stack
 *  is internal because it must survive a C6 OTA (PSRAM unavailable) and it never
 *  touches host flash. */
void shell_start_telemetry(void)
{
    if (s_tlm_task != NULL) {
        return;
    }
    /* Pinned to core 0: ulTaskGetIdleRunTimeCounter() reports the calling
     * core's idle time, so pinning keeps the CPU% delta consistent (a migrating
     * task would mix per-core counters). */
    if (xTaskCreatePinnedToCore(shell_telemetry_task, "sheltlm",
                                P4_CONFIG_TELEMETRY_TASK_STACK, NULL,
                                tskIDLE_PRIORITY + 1, &s_tlm_task, 0) != pdPASS) {
        s_tlm_task = NULL;
        ESP_LOGW(SHELL_TAG, "telemetry task not started; header stats stay static");
    }
}

void shell_header_status_refresh(void)
{
    bool wifi_connected = s_command_ops.wifi_is_connected != NULL && s_command_ops.wifi_is_connected();
    int wifi_rssi = P4_CONFIG_HEADER_RSSI_UNKNOWN;
    bool sd_mounted = s_command_ops.sd_is_mounted != NULL && s_command_ops.sd_is_mounted();
    uint32_t uptime_sec = (uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000);
    char clock_text[16];

    /* Network clock bootstrap: once associated, detect the local timezone and
     * start SNTP. The hook is cheap and idempotent (it only kicks off a task);
     * the blocking HTTP work never runs here on the LVGL task. Must happen
     * before the display-off early-return so time is set even while dark. */
    if (wifi_connected && s_command_ops.time_auto_sync != NULL) {
        (void)s_command_ops.time_auto_sync();
    }

    /* Idle display-off: do no LVGL work while the backlight is off; the power
     * tick and the fast wake poll still run from main. */
    if (display_get_power_state() != DISPLAY_POWER_ON) {
        return;
    }

    /* Signal strength comes through the networking module's accessor, so no
     * esp_wifi_* call escapes components/networking/. */
    if (s_command_ops.wifi_get_rssi != NULL) {
        (void)s_command_ops.wifi_get_rssi(&wifi_rssi);
    }

    /* Restore the transcript if a hidden-skip (gfx canvas / TUI / app mode)
     * left it stale and it is visible again. */
    shell_transcript_repaint_if_pending();

    /* Idle center content: the local clock ("--:--" while unsynchronized). */
    (void)time_format_hm(clock_text, sizeof(clock_text));

    /* Batch every header field into one async render. Individual
     * header_update_*() calls would each schedule their own render and
     * produce visible flicker on each refresh period. */
    header_batch_t batch = {
        .wifi_connected = wifi_connected,
        .wifi_rssi = wifi_rssi,
        .battery_percent = s_tlm_battery_percent,
        .battery_adc_ready = s_tlm_battery_ok,
        .bt_enabled = s_command_ops.bluetooth_is_enabled != NULL && s_command_ops.bluetooth_is_enabled(),
        .bt_connected = s_command_ops.bluetooth_is_connected != NULL && s_command_ops.bluetooth_is_connected(),
        .usb_connected = s_command_ops.usb_is_connected != NULL && s_command_ops.usb_is_connected(),
        .sd_state = sd_mounted ? HEADER_SD_MOUNTED : HEADER_SD_NONE,
        .free_heap = s_tlm_free_heap,
        .total_heap = s_tlm_total_heap,
        .cpu_percent = s_tlm_cpu_percent,
        .task_count = s_tlm_task_count,
        .uptime_seconds = uptime_sec,
        .clock_text = clock_text,
        .c6ota_busy = s_command_ops.c6ota_is_busy != NULL && s_command_ops.c6ota_is_busy(),
        .bg_jobs_running = s_command_ops.bg_jobs_running != NULL && s_command_ops.bg_jobs_running(),
    };
    header_update_batch(&batch);

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

uint32_t shell_header_refresh_interval_ms(void)
{
    header_refresh_state_t state = { 0 };

    state.display_off = (display_get_power_state() != DISPLAY_POWER_ON);
    state.busy = (s_command_ops.c6ota_is_busy != NULL && s_command_ops.c6ota_is_busy()) ||
                 (s_command_ops.bg_jobs_running != NULL && s_command_ops.bg_jobs_running());

    if (!state.busy && !state.display_off) {
        bool connected = s_command_ops.wifi_is_connected != NULL &&
                         s_command_ops.wifi_is_connected();
        const char *wifi_state = (s_command_ops.wifi_state_string != NULL)
                                     ? s_command_ops.wifi_state_string()
                                     : NULL;
        /* Only the stack bring-up window ("starting"); an associated link is
         * excluded by !connected, and terminal states fall back to idle. */
        state.wifi_connecting = !connected && wifi_state != NULL &&
                                strcmp(wifi_state, "starting") == 0;
    }

    state.startup = ((uint32_t)((esp_timer_get_time() - s_boot_timestamp_us) / 1000000) <
                     P4_CONFIG_HEADER_STARTUP_GRACE_S);

    state.clock_enabled = time_is_set();
    if (state.clock_enabled) {
        struct tm ti = time_get_local();
        state.seconds_to_next_minute = 60 - ti.tm_sec; /* 1..60 */
    }

    state.ms_to_idle_off = (s_command_ops.pm_ms_until_idle_off != NULL)
                               ? s_command_ops.pm_ms_until_idle_off()
                               : 0;

    return header_refresh_interval_ms(&state);
}

int64_t shell_get_boot_timestamp_us(void)
{
    return s_boot_timestamp_us;
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

/**
 * Strip caret escapes (`^c` -> `c`) from a string in place WITHOUT removing
 * quote delimiters. Used by `echo`, which prints the raw remainder of the
 * command line and must keep `"..."` visible (matching DOS) while still
 * consuming a caret that escapes the following character (`echo a^&b` -> `a&b`).
 *
 * A caret inside single quotes is literal (the same rule the operator scanner
 * uses); a trailing caret with nothing after it is kept.
 */
char *shell_unescape_carets_in_place(char *text)
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
        if (*read == P4_CONFIG_ESCAPE_CHAR && state != SHELL_QUOTE_SINGLE && read[1] != '\0') {
            *write++ = read[1];
            read += 2;
            continue;
        }
        if (*read == '"' && state != SHELL_QUOTE_SINGLE) {
            state = (state == SHELL_QUOTE_DOUBLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_DOUBLE;
        } else if (*read == '\'' && state != SHELL_QUOTE_DOUBLE) {
            state = (state == SHELL_QUOTE_SINGLE) ? SHELL_QUOTE_NONE : SHELL_QUOTE_SINGLE;
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

    /* NULL-terminate the vector when there is room, matching the standard C
     * argv contract (native apps registered through the applib ABI rely on
     * it; built-ins only ever use argc). */
    if (argc < max_args) {
        argv[argc] = NULL;
    }

    return argc;
}

/**
 * Count the arguments in a command line without mutating it, using the same
 * quote/escape rules as shell_split_args(). Used to detect when a command has
 * more arguments than the dispatcher's argv capacity, so truncation is never
 * silent.
 */
int shell_count_args(const char *text)
{
    int count = 0;
    const char *p = text;

    if (text == NULL) {
        return 0;
    }

    while (*p != '\0') {
        shell_quote_state_t state = SHELL_QUOTE_NONE;

        /* Skip leading whitespace between arguments. */
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        count++;

        /* Advance over the token with the same quote/escape rules. */
        while (*p != '\0') {
            bool escaped = false;
            const char *next = shell_quote_advance(p, &state, &escaped);

            if (!escaped && state == SHELL_QUOTE_NONE && next == p + 1 &&
                isspace((unsigned char)*p)) {
                break;
            }
            p = next;
        }
        if (*p != '\0') {
            p++;
        }
    }

    return count;
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

    /* `if` and `for` consume the rest of the line as their command body
     * (cmd.exe semantics: `if cond a & b` runs a and b only when cond is
     * true). Splitting here would turn a `&` inside an if/for body into an
     * unconditional chain link that runs even when the condition is false,
     * so a guarded `... & goto label` would jump regardless of the test. */
    {
        char first_word[8];
        size_t word_len = 0;
        const char *word = text;

        while (*word == ' ' || *word == '\t') {
            word++;
        }
        while (word_len + 1 < sizeof(first_word) && *word != '\0' &&
               *word != ' ' && *word != '\t' && *word != '(') {
            first_word[word_len++] = (char)tolower((unsigned char)*word);
            word++;
        }
        first_word[word_len] = '\0';
        if (strcmp(first_word, "if") == 0 || strcmp(first_word, "for") == 0) {
            segments[0].command = shell_trim(text);
            segments[0].op = SHELL_CHAIN_FIRST;
            return 1;
        }
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

    {
        char *final_command = shell_trim(segment_start);
        /* Do not emit a trailing empty segment (e.g. after a dangling
         * separator, or for an all-whitespace line). */
        if (final_command[0] != '\0') {
            segments[count].command = final_command;
            segments[count].op = pending_op;
            count++;
        }
    }

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
    if (endptr == text || *endptr != '\0' || value < 0 || value > 100) {
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
    if (endptr == text || *endptr != '\0' || value < min_value || value > max_value) {
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

void shell_header_notify_level(const char *text, uint32_t timeout_ms,
                               header_notify_level_t level)
{
    header_notify(level, text, timeout_ms);
}

void shell_header_notify(const char *text, uint32_t timeout_ms)
{
    shell_header_notify_level(text, timeout_ms, HEADER_NOTIFY_INFO);
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

    /* Transcript buffer mutex (see s_transcript_buf_lock): guards the plain
     * and ANSI scrollback buffers against concurrent appends from the worker
     * and the LVGL task. Recursive: some paths touch buffers under both this
     * and the LVGL port lock (always port-outer/buffer-inner). */
    if (s_transcript_buf_lock == NULL) {
        s_transcript_buf_lock = xSemaphoreCreateRecursiveMutex();
    }

    /* Allocate the large transcript and clipboard buffers from PSRAM so the
     * tight internal heap stays available for DMA-capable users (WiFi/SDIO
     * transport mempool, USB-Serial/JTAG rings). */
    if (s_transcript == NULL) {
        s_transcript = heap_caps_malloc(SHELL_TRANSCRIPT_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_transcript == NULL) {
            s_transcript = heap_caps_malloc(SHELL_TRANSCRIPT_BYTES, MALLOC_CAP_8BIT);
        }
    }
    if (s_transcript_ansi == NULL) {
        s_transcript_ansi = heap_caps_malloc(SHELL_TRANSCRIPT_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_transcript_ansi == NULL) {
            s_transcript_ansi = heap_caps_malloc(SHELL_TRANSCRIPT_BYTES, MALLOC_CAP_8BIT);
        }
    }
    if (s_clipboard == NULL) {
        s_clipboard = heap_caps_malloc(SHELL_CLIPBOARD_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_clipboard == NULL) {
            s_clipboard = heap_caps_malloc(SHELL_CLIPBOARD_BYTES, MALLOC_CAP_8BIT);
        }
    }
    if (s_transcript == NULL || s_transcript_ansi == NULL || s_clipboard == NULL) {
        shell_record_errorf("shell", ESP_ERR_NO_MEM,
                            "Failed to allocate transcript/clipboard buffers");
        // Continuing would dereference NULL below; boot cannot proceed
        // without the transcript buffers.
        return;
    }

    /* Interactive keypress queue used by pause, choice, and more. Created
     * before any input source starts so no keystroke can be lost. */
    if (s_key_queue == NULL) {
        s_key_queue = xQueueCreate(SHELL_KEY_QUEUE_DEPTH, SHELL_KEY_SEQ_BYTES);
        if (s_key_queue == NULL) {
            shell_record_errorf("shell", ESP_ERR_NO_MEM, "Failed to create keypress queue");
        }
    }
    s_key_wait_active = false;

    /* Restore the default DOS prompt template */
    shell_prompt_reset();

    /* Clear state */
    s_transcript[0] = '\0';
    s_transcript_ansi[0] = '\0';
    s_transcript_len = 0;
    s_transcript_ansi_len = 0;
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
