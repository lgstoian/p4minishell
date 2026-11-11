/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file shell_print.c
 * @brief Hardware-free semantic print helpers (heading, field, ok/error/…).
 *
 * These are the `shell_print_*` entry points every command in the firmware
 * uses for user-visible output. Each one applies the shared `SH_*` palette
 * from ansi_palette.h, renders the line through the ANSI format builder and
 * hands the finished escape sequence to `shell_transcript_append_ansi()`.
 *
 * Nothing here touches LVGL, the window manager, the display, USB-Serial-JTAG
 * or the board BSP: the transcript model owns the buffers and (on the board)
 * mirrors the line through the sink shell.c registers. That is what lets the
 * host unit-test lane (test/host) compile this file natively - the same
 * source the firmware runs.
 *
 * @module shell
 * @layer shell
 * @domain core
 * @provides shell_print_heading/field/field_num/ok/error/warning/muted/usage
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule ai-context.md#shell-core-rules
 */

#include "shell.h"

#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"

#include <stdarg.h>
#include <stdio.h>

/**
 * Body budget for the semantic print helpers.
 *
 * Each helper renders the caller's text into this, then wraps it with a
 * colour prefix, a label, and a reset inside a full P4_CONFIG_ANSI_BUFFER_BYTES
 * line. Reserving 64 bytes of headroom guarantees the wrap always fits.
 */
#define SHELL_SEMANTIC_BODY_BYTES       (P4_CONFIG_ANSI_BUFFER_BYTES - 64)

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
