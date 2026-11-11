/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_transcript.c
 * @brief Hardware-free transcript model: scrollback buffers, text assembly,
 *        output-redirection capture and the RAM clipboard.
 *
 * This is the half of the shell transcript that is pure C: it owns the plain
 * and ANSI scrollback buffers (PSRAM-first, lazily ensured), the append
 * machinery that keeps the two in step, the capture buffer that backs `>` /
 * `>>`, and the clipboard behind `clip` / `paste`. Nothing here includes
 * LVGL, the window manager, the display, USB-Serial-JTAG or the board BSP, so
 * the host unit-test lane (test/host) compiles and links it natively - the
 * same source the firmware runs.
 *
 * The view stays in shell.c and is reached only through the optional
 * `shell_transcript_sink_t` (see shell_transcript_internal.h): the LVGL label
 * repaint, the repaint batching/flush, the scroll-to-end, the internal-heap
 * guard's widget trim, the click-region row tracker and the USB-Serial-JTAG
 * mirror. shell.c registers the real implementation in `shell_init()`; with no
 * sink installed the model just fills its buffers, which is what the tests
 * exercise. There is no fake view anywhere.
 *
 * @module shell
 * @layer shell
 * @domain core
 * @provides transcript model: scrollback buffers, ANSI text assembly,
 *           output-redirection capture, RAM clipboard
 * @status stable
 * @test test/main/test_shell_parser.c, test/main/test_clipboard.c
 * @hard-rule ai-context.md#shell-core-rules
 */

#include "shell.h"
#include "shell_transcript_internal.h"

#include "ansi.h"
#include "p4heap.h"
#include "p4minishell_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Backward-compatibility aliases (the shell's config knob names). */
#define SHELL_TRANSCRIPT_BYTES          P4_CONFIG_TRANSCRIPT_BYTES
#define SHELL_CLIPBOARD_BYTES           P4_CONFIG_CLIPBOARD_BYTES

/* ========================================================================
 * MODEL STATE
 * ======================================================================== */

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

/* Scrollback-generation counter (bugs.md F26). The render path detects
 * head-moving truncation in O(1) from this value instead of comparing a
 * snapshot of the rendered prefix: every site that drops the oldest bytes
 * (reset, memory-pressure trim, buffer-full truncation) bumps it, and the
 * window manager rebuilds spans only when the epoch changes. */
static uint32_t s_transcript_epoch = 1;

/* Guards the plain/ANSI scrollback buffers (NOT the LVGL widgets: those stay
 * under the LVGL port lock). Lets worker appends proceed without serializing
 * behind LVGL render passes, which hold the port lock O(buffer) at a full
 * scrollback. Strict order everywhere: LVGL port lock outer, buffer lock
 * inner; buffer-only paths never take the port lock. Recursive. */
static SemaphoreHandle_t s_transcript_buf_lock = NULL;

/* Board-side hooks; NULL on the host lane and before shell_init(). */
static const shell_transcript_sink_t *s_sink = NULL;
static void *s_sink_ctx = NULL;

/* RAM clipboard backing the `clip` / `paste` commands. */
static char *s_clipboard = NULL;
static bool s_clipboard_is_file;

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

/* Used by the append paths above its definition below. */
static void shell_redirect_capture_add(const char *text, size_t len);

/* ========================================================================
 * BUFFER LIFECYCLE
 * ======================================================================== */

/**
 * Create the scrollback mutex and allocate the scrollback/clipboard buffers
 * on first use. Shell allocation policy lives in p4heap (PSRAM-first).
 *
 * On target `shell_init()` calls this before any producer exists; the lazy
 * path exists so a unit test that never boots the shell still gets real
 * buffers. Returns false when the memory is unavailable, in which case every
 * append is a no-op instead of dereferencing NULL.
 */
static bool shell_transcript_ensure(void)
{
    if (s_transcript_buf_lock == NULL) {
        s_transcript_buf_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_transcript == NULL) {
        s_transcript = p4heap_alloc_psram(SHELL_TRANSCRIPT_BYTES);
    }
    if (s_transcript_ansi == NULL) {
        s_transcript_ansi = p4heap_alloc_psram(SHELL_TRANSCRIPT_BYTES);
    }
    if (s_clipboard == NULL) {
        s_clipboard = p4heap_alloc_psram(SHELL_CLIPBOARD_BYTES);
    }
    if (s_transcript == NULL || s_transcript_ansi == NULL || s_clipboard == NULL) {
        return false;
    }
    return true;
}

bool shell_transcript_model_init(void)
{
    if (!shell_transcript_ensure()) {
        return false;
    }
    s_transcript[0] = '\0';
    s_transcript_ansi[0] = '\0';
    s_transcript_len = 0;
    s_transcript_ansi_len = 0;
    return true;
}

void shell_transcript_set_sink(const shell_transcript_sink_t *sink, void *ctx)
{
    s_sink = sink;
    s_sink_ctx = ctx;
}

void shell_transcript_model_lock(void)
{
    (void)shell_transcript_ensure();
    if (s_transcript_buf_lock == NULL) {
        return;
    }
    xSemaphoreTakeRecursive(s_transcript_buf_lock, portMAX_DELAY);
}

void shell_transcript_model_unlock(void)
{
    if (s_transcript_buf_lock != NULL) {
        xSemaphoreGiveRecursive(s_transcript_buf_lock);
    }
}

void shell_transcript_model_clear(void)
{
    shell_transcript_model_lock();
    if (s_transcript != NULL && s_transcript_ansi != NULL) {
        s_transcript[0] = '\0';
        s_transcript_ansi[0] = '\0';
    }
    s_transcript_len = 0;
    s_transcript_ansi_len = 0;
    s_transcript_epoch++;
    shell_transcript_model_unlock();
}

uint32_t shell_transcript_model_epoch(void)
{
    return s_transcript_epoch;
}

size_t shell_transcript_model_trim_oldest(size_t keep)
{
    size_t plain_len;
    size_t ansi_len;
    size_t dropped_bytes = 0;
    size_t dropped_lines = 0;
    size_t i;

    if (s_transcript == NULL || s_transcript_ansi == NULL) {
        return 0;
    }

    plain_len = s_transcript_len;
    ansi_len = s_transcript_ansi_len;

    if (plain_len > keep) {
        dropped_bytes = plain_len - keep;
        /* Count the lines that disappear so click/link regions can be
         * rebased instead of firing a command from a different row. */
        for (i = 0; i < dropped_bytes; i++) {
            if (s_transcript[i] == '\n') {
                dropped_lines++;
            }
        }
        memmove(s_transcript, s_transcript + dropped_bytes, keep);
        plain_len = keep;
        s_transcript[plain_len] = '\0';
        s_transcript_epoch++;
    }
    if (ansi_len > keep) {
        memmove(s_transcript_ansi, s_transcript_ansi + ansi_len - keep, keep);
        ansi_len = keep;
        s_transcript_ansi[ansi_len] = '\0';
    }
    s_transcript_len = plain_len;
    s_transcript_ansi_len = ansi_len;

    return dropped_lines;
}

/* ========================================================================
 * TEXT ASSEMBLY
 * ======================================================================== */

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
        s_transcript_epoch++;   /* the rendered prefix was thrown away */
    }

    /* Drop the oldest half and mark the cut. */
    if (plain_len + plain_text_len + 1 >= plain_size) {
        size_t keep = plain_size / 2;

        s_transcript_epoch++;

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

/* --- Optional board-side hooks ------------------------------------------- */

static void shell_transcript_sink_pressure(void)
{
    if (s_sink != NULL && s_sink->on_pressure != NULL) {
        s_sink->on_pressure(s_sink_ctx);
    }
}

static void shell_transcript_sink_appended(void)
{
    if (s_sink != NULL && s_sink->on_append != NULL) {
        s_sink->on_append(s_sink_ctx);
    }
}

static void shell_transcript_sink_rows(const char *text, size_t len)
{
    if (s_sink != NULL && s_sink->on_text != NULL) {
        s_sink->on_text(s_sink_ctx, text, len);
    }
}

static void shell_transcript_sink_mirror(const char *text, size_t len)
{
    if (s_sink != NULL && s_sink->on_mirror != NULL) {
        s_sink->on_mirror(s_sink_ctx, text, len);
    }
}

/**
 * Append text to the transcript buffer and notify the view.
 *
 * @param text           Text to append (already plain, no ANSI sequences).
 * @param mirror_to_uart When true, the same text is echoed to the serial
 *                       console. ANSI callers pass false because they emit the
 *                       raw escape-sequence form to the terminal themselves.
 */
static void shell_transcript_append_internal(const char *text, bool mirror_to_uart)
{
    static const char truncation_marker[] = "\n[history truncated]\n";
    size_t text_len;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (!shell_transcript_ensure()) {
        return;
    }

    /* An active output-redirection capture owns stdout: the command's text
     * goes to the capture (and thus the redirect file) ONLY, never the visible
     * transcript or the serial mirror. This is DOS `>` semantics; mirroring it
     * would both duplicate the output on screen and leak the intermediate
     * stages of `cmd1 | cmd2 > file`. */
    if (s_redirect_capturing) {
        shell_redirect_capture_add(text, strlen(text));
        return;
    }

    /* A single command with a large output can grow the span group fast
     * enough to exhaust the internal heap mid-command. Reclaim scrollback
     * when the internal heap drops below the threshold (rate-limited, keeps
     * the newest three quarters), so a tiny stdio allocation never aborts
     * the board while output is still being emitted. Cheap when memory is
     * fine (a single free-size query plus a timestamp check). The view side
     * owns the policy and its own locks (LVGL port outer, buffer inner). */
    shell_transcript_sink_pressure();

    /* Serialize buffer appends (worker, LVGL task via the async drain,
     * console task) against each other with the lightweight buffer mutex —
     * deliberately NOT the LVGL port lock, which the render task can hold
     * O(buffer) per frame at a full scrollback. Widget/label work happens
     * in the deferred label step below, which takes the port lock itself. */
    shell_transcript_model_lock();
    shell_transcript_append_to_buffer(s_transcript, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_len,
                                      s_transcript_ansi, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_ansi_len,
                                      text, text, truncation_marker);
    shell_transcript_model_unlock();

    text_len = strlen(text);

    /* Mirror plain transcript output to the serial console so idf.py monitor
     * stays a first-class shell endpoint. Outside both locks: UART has its
     * own lock, and pacing serial bytes on LVGL work is exactly the stall
     * this discipline removes. */
    if (mirror_to_uart) {
        shell_transcript_sink_mirror(text, text_len);
    }

    shell_transcript_sink_appended();

    /* Update anchor position tracking for click regions */
    shell_transcript_sink_rows(text, text_len);
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

    if (!shell_transcript_ensure()) {
        return;
    }

    /* The UART console passes the raw ANSI code through natively for real
     * terminals; the LVGL transcript renders it via recolor markup so colours
     * appear on screen too. s_transcript stays plain for history/redirection,
     * while s_transcript_ansi keeps the real escape sequences for the label. */
    ansi_strip_to_plain(plain, sizeof(plain), text);

    /* While an output-redirection capture is active, the (stripped) output
     * goes to the capture only — see shell_transcript_append_internal. */
    if (s_redirect_capturing) {
        shell_redirect_capture_add(plain, strlen(plain));
        return;
    }

    /* Buffer appends serialize on the lightweight buffer mutex (see
     * shell_transcript_append_internal); the label step takes the LVGL port
     * lock itself only when it actually repaints. */
    shell_transcript_model_lock();
    shell_transcript_append_to_buffer(s_transcript, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_len,
                                      s_transcript_ansi, SHELL_TRANSCRIPT_BYTES,
                                      &s_transcript_ansi_len,
                                      plain, text, "\n[history truncated]\n");
    shell_transcript_model_unlock();

    shell_transcript_sink_appended();

    /* Write the raw ANSI text to the UART console for
     * terminals that support ANSI rendering. */
    shell_transcript_sink_mirror(text, strlen(text));
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
 * READS
 * ======================================================================== */

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

    if (!shell_transcript_ensure()) {
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

    if (!shell_transcript_ensure()) {
        return NULL;
    }
    return s_transcript_ansi + offset;
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

bool shell_redirect_capture_active(void)
{
    return s_redirect_capturing;
}

/* ========================================================================
 * RAM CLIPBOARD (clip / paste)
 * ========================================================================
 * Co-located with the transcript because it is the same hardware-free output
 * state: a fixed RAM buffer plus the "is this a file path" flag. Its
 * allocation rides the same PSRAM-first init as the scrollback.
 */

void shell_clipboard_set(const char *text)
{
    if (!shell_transcript_ensure()) {
        return;
    }
    if (text == NULL) {
        s_clipboard[0] = '\0';
    } else {
        snprintf(s_clipboard, SHELL_CLIPBOARD_BYTES, "%s", text);
    }
    s_clipboard_is_file = false;
}

void shell_clipboard_set_file(const char *path)
{
    if (!shell_transcript_ensure()) {
        return;
    }
    if (path != NULL) {
        snprintf(s_clipboard, SHELL_CLIPBOARD_BYTES, "%s", path);
    }
    s_clipboard_is_file = true;
}

const char *shell_clipboard_get(void)
{
    if (!shell_transcript_ensure()) {
        return "";
    }
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
    return shell_clipboard_get()[0] != '\0';
}
