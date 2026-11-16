/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_log.c
 * @brief Hardware-free debug log ring, warning counter and the
 *        shell_record_{error,warning,info}f() family.
 *
 * The ring is plain RAM and the record helpers only need the transcript model
 * and the ESP-IDF log macros, so this half of the shell lives outside the
 * LVGL-coupled shell.c. That lets the host unit-test lane and the batch
 * environment core (which records a warning when nested execution is
 * attempted before command_init()) link it without a display.
 *
 * The single view-side dependency - scheduling a transcript repaint - goes
 * through the transcript model's optional sink: shell_transcript_schedule_appendf()
 * hands the line to the view's async staging buffer on the target and appends
 * synchronously when no view exists. Ordering is unchanged: the ring push and
 * the ESP_LOG line happen before the transcript append, exactly as before.
 *
 * @module shell
 * @layer shell
 * @domain core
 * @provides debug log ring, warning counter, shell_record_*f()
 * @status stable
 * @test test/main/test_shell_debug_log.c
 * @hard-rule no LVGL, display, USB or board BSP in this file
 */

#include "shell.h"
#include "shell_transcript_internal.h"

#include "p4minishell_config.h"

#include "esp_err.h"
#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Backward-compatibility aliases (the shell's config knob names). */
#define SHELL_DEBUG_LOG_DEPTH   P4_CONFIG_DEBUG_LOG_DEPTH
#define SHELL_DEBUG_ENTRY_BYTES P4_CONFIG_DEBUG_ENTRY_BYTES

/* ========================================================================
 * RING STATE
 * ======================================================================== */

typedef struct {
    char entries[SHELL_DEBUG_LOG_DEPTH][SHELL_DEBUG_ENTRY_BYTES];
    size_t count;
    size_t next_index;
} shell_debug_log_t;

static shell_debug_log_t s_debug_log;
static size_t s_runtime_warning_count;

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
    shell_transcript_schedule_appendf("%s: %s: %s (0x%x)\n",
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

/** Oldest-entry slot: the ring overwrites from next_index, so a full ring
 *  starts there and a partial ring starts at 0. Shared by the transcript
 *  printer and the `debug save` exporter so both agree on ordering. */
static size_t shell_debug_oldest_slot(void)
{
    return s_debug_log.count >= SHELL_DEBUG_LOG_DEPTH
           ? s_debug_log.next_index
           : 0;
}

size_t shell_debug_get_count(void)
{
    return s_debug_log.count;
}

bool shell_debug_get_entry(size_t index, char *out, size_t out_size)
{
    size_t slot;

    if (out == NULL || out_size == 0 || index >= s_debug_log.count) {
        return false;
    }
    slot = (shell_debug_oldest_slot() + index) % SHELL_DEBUG_LOG_DEPTH;
    snprintf(out, out_size, "%s", s_debug_log.entries[slot]);
    return true;
}

void shell_debug_log_reset(void)
{
    memset(&s_debug_log, 0, sizeof(s_debug_log));
    s_runtime_warning_count = 0;
}
