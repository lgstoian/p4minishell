/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file shell_transcript_internal.h
 * @brief Private seam between the transcript MODEL (shell_transcript.c) and
 *        its VIEW/glue (shell.c).
 *
 * The model is hardware-free: it owns the plain/ANSI scrollback buffers, the
 * text assembly, the output-redirection capture and the RAM clipboard, and it
 * never includes LVGL, the window manager, the display, USB-Serial-JTAG or the
 * board BSP. That is what lets the host unit-test lane link it natively.
 *
 * Everything the model still needs from the board lives behind this optional
 * sink. `shell.c` registers the real implementation in `shell_init()`; on the
 * host nothing registers it, the pointer stays NULL, and the model simply does
 * not call it. This is dependency inversion, not a stub: there is no fake
 * implementation anywhere, and the model has no fallback that pretends a
 * widget exists.
 *
 * Callbacks fire at exactly the points the old inline code ran, so the target
 * keeps the same text, the same ordering and the same flush points:
 *
 *   append (plain):  on_pressure -> buffers -> on_mirror -> on_append -> on_text
 *   append (ANSI):   on_pressure -> buffers -> on_append -> on_mirror
 *
 * The ANSI path has never tracked click/anchor rows (only the plain path
 * calls on_text); that asymmetry is preserved on purpose.
 *
 * @module shell
 * @layer shell
 * @domain core
 * @provides transcript model sink seam + model-private accessors
 * @status stable
 * @test test/main/test_shell_parser.c, test/main/test_clipboard.c
 * @hard-rule ai-context.md#shell-core-rules
 */

/**
 * Board-side hooks the transcript model calls when they are installed.
 *
 * Every member is optional; the model checks for NULL before each call. A
 * NULL table (the host lane, or early boot before `shell_init()`) means
 * "text only, no view" - it is a valid, fully supported configuration.
 */
typedef struct {
    /**
     * Called once before an append touches the buffers (never while an
     * output-redirection capture owns stdout). The shell uses it for the
     * internal-heap guard that reclaims scrollback under memory pressure.
     */
    void (*on_pressure)(void *ctx);

    /**
     * Called after the text landed in the buffers and before the serial
     * mirror: stage/schedule an LVGL label repaint.
     */
    void (*on_append)(void *ctx);

    /**
     * Called with the plain text that just landed, for the click-region /
     * anchor row tracker. Plain appends only (see the file comment).
     * @p text is NUL-terminated; @p len is `strlen(text)`.
     */
    void (*on_text)(void *ctx, const char *text, size_t len);

    /**
     * Called with the bytes destined for the USB-Serial-JTAG console: the
     * plain form for a plain append, the raw escape form for an ANSI append.
     * @p text is NUL-terminated; @p len is `strlen(text)`.
     */
    void (*on_mirror)(void *ctx, const char *text, size_t len);

    /**
     * Called by `shell_transcript_schedule_appendf()` with a fully formatted
     * line. The view stages it in its async buffer and schedules one repaint,
     * preserving the producer/consumer split that keeps LVGL work on the UI
     * task. When no sink is installed (host lane, early boot) the caller
     * appends the text synchronously instead.
     * @p text is NUL-terminated.
     */
    void (*on_schedule)(void *ctx, const char *text);
} shell_transcript_sink_t;

/**
 * Install (or clear, with NULL) the board-side sink.
 *
 * Not thread-safe by design: it is called once from `shell_init()` before any
 * producer task exists. Passing NULL is the host/test configuration.
 */
void shell_transcript_set_sink(const shell_transcript_sink_t *sink, void *ctx);

/**
 * Format one line and hand it to the view's deferred append path.
 *
 * On the target this reaches the sink's `on_schedule` callback, which stages
 * the text and queues the same LVGL async flush as before. With no sink
 * installed (host unit-test lane, or callers before `shell_init()`) the text
 * is appended synchronously, which is exactly what the old view-side helper
 * did when no transcript widget existed.
 *
 * This is the hardware-free entry point `shell_record_errorf()` uses, so the
 * debug log ring no longer lives in the LVGL-coupled shell.c.
 */
void shell_transcript_schedule_appendf(const char *format, ...);

/**
 * Allocate the scrollback and clipboard buffers (PSRAM-first) and clear them.
 *
 * Called by `shell_init()`, which reports the failure and stops boot exactly
 * as it did when the allocation lived there. Idempotent.
 *
 * @return false when a buffer could not be allocated.
 */
bool shell_transcript_model_init(void);

/**
 * Take/release the recursive scrollback mutex (guards the plain and ANSI
 * buffers, never the LVGL widgets). The view side needs it around the staging
 * copy it makes from the buffers, so the lock travels with the model.
 * Both are safe before `shell_transcript_model_init()`.
 */
void shell_transcript_model_lock(void);
void shell_transcript_model_unlock(void);

/** Empty both buffers and bump the scrollback epoch. Takes the model lock. */
void shell_transcript_model_clear(void);

/** Current scrollback epoch (bumped whenever the rendered prefix is cut). */
uint32_t shell_transcript_model_epoch(void);

/**
 * Read-only access to the ANSI form of the scrollback: the label's source
 * text (real SGR escapes preserved) and its length. Declared here because they
 * are shell.c-private views of the model, not part of the public shell.h API.
 * `_from()` returns NULL when @p offset is past the end.
 */
size_t shell_transcript_get_ansi_length(void);
const char *shell_transcript_get_ansi_from(size_t offset);

/**
 * Drop the oldest bytes so at most @p keep_bytes remain in each buffer,
 * returning how many transcript rows disappeared (for click-region rebasing).
 *
 * Pure buffer surgery: the caller owns the policy (when to trim, what to keep)
 * and must already hold the model lock, because it also restages the trimmed
 * text into the window manager while still under it.
 */
size_t shell_transcript_model_trim_oldest(size_t keep_bytes);

#ifdef __cplusplus
}
#endif
