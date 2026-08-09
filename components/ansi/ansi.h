/**
 * @file ansi.h
 * @brief ANSI/VT escape sequence module for P4MiniShell.
 *
 * Provides SGR (Select Graphic Rendition) escape sequence parsing and
 * formatting. Maps ANSI color codes to configurable LVGL color values.
 * Supports both transcript (LVGL textarea with color-tagged segments)
 * and UART console (raw ANSI pass-through) output paths.
 *
 * Features:
 *   - SGR code parsing: ESC[...m sequences
 *   - Foreground colors: 30-37 (standard), 90-97 (bright)
 *   - Background colors: 40-47 (standard), 100-107 (bright)
 *   - Text attributes: reset(0), bold(1), dim(2), italic(3), underline(4)
 *   - ANSI format string builder with va_list support
 *   - Color palette configurable via p4minishell_config.h
 *   - Thread-safe color lookup
 *
 * Architecture:
 *   This module OWNS the ANSI/SGR color palette and escape sequence
 *   processing. It does NOT own LVGL widgets or transcript buffers.
 *   The shell transcript module (shell.c) calls into this module to
 *   interpret ANSI codes in transcript text.
 *
 * Usage:
 *   1. ansi_init() — initialize the color palette from config
 *   2. ansi_format(dst, dst_size, "\\e[32m%s\\e[0m", "green text") — wrap text
 *   3. ansi_strip_apply(text, out_fn) — strip ANSI codes, call out_fn with segments
 *   4. Direct color access via ansi_get_fg_color(), ansi_get_bg_color()
 */

#ifndef P4MINISHELL_ANSI_H
#define P4MINISHELL_ANSI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * ANSI SGR CODE CONSTANTS
 * ======================================================================== */

/** ANSI escape sequence introducer. */
#define ANSI_ESC "\x1B"

/** ANSI CSI (Control Sequence Introducer) = ESC [ */
#define ANSI_CSI "\x1B["

/** SGR (Select Graphic Rendition) terminator. */
#define ANSI_SGR_END "m"

/** ANSI SGR reset code. */
#define ANSI_RESET      0

/** ANSI SGR attribute codes. */
#define ANSI_BOLD       1
#define ANSI_DIM        2
#define ANSI_ITALIC     3
#define ANSI_UNDERLINE  4
#define ANSI_BLINK      5
#define ANSI_REVERSE    7
#define ANSI_HIDDEN     8
#define ANSI_STRIKE     9

/** ANSI foreground color codes (standard). */
#define ANSI_FG_BLACK   30
#define ANSI_FG_RED     31
#define ANSI_FG_GREEN   32
#define ANSI_FG_YELLOW  33
#define ANSI_FG_BLUE    34
#define ANSI_FG_MAGENTA 35
#define ANSI_FG_CYAN    36
#define ANSI_FG_WHITE   37

/** ANSI foreground color codes (bright). */
#define ANSI_FG_BRIGHT_BLACK   90
#define ANSI_FG_BRIGHT_RED     91
#define ANSI_FG_BRIGHT_GREEN   92
#define ANSI_FG_BRIGHT_YELLOW  93
#define ANSI_FG_BRIGHT_BLUE    94
#define ANSI_FG_BRIGHT_MAGENTA 95
#define ANSI_FG_BRIGHT_CYAN    96
#define ANSI_FG_BRIGHT_WHITE   97

/** ANSI background color codes (standard). */
#define ANSI_BG_BLACK   40
#define ANSI_BG_RED     41
#define ANSI_BG_GREEN   42
#define ANSI_BG_YELLOW  43
#define ANSI_BG_BLUE    44
#define ANSI_BG_MAGENTA 45
#define ANSI_BG_CYAN    46
#define ANSI_BG_WHITE   47

/** ANSI background color codes (bright). */
#define ANSI_BG_BRIGHT_BLACK   100
#define ANSI_BG_BRIGHT_RED     101
#define ANSI_BG_BRIGHT_GREEN   102
#define ANSI_BG_BRIGHT_YELLOW  103
#define ANSI_BG_BRIGHT_BLUE    104
#define ANSI_BG_BRIGHT_MAGENTA 105
#define ANSI_BG_BRIGHT_CYAN    106
#define ANSI_BG_BRIGHT_WHITE   107

/** ANSI default foreground/background codes. */
#define ANSI_FG_DEFAULT 39
#define ANSI_BG_DEFAULT 49

/* ========================================================================
 * ANSI COLOR INDEX ENUMERATION
 * ======================================================================== */

/** Index into the ANSI color palette (16 standard terminal colors). */
typedef enum {
    ANSI_COLOR_BLACK = 0,
    ANSI_COLOR_RED,
    ANSI_COLOR_GREEN,
    ANSI_COLOR_YELLOW,
    ANSI_COLOR_BLUE,
    ANSI_COLOR_MAGENTA,
    ANSI_COLOR_CYAN,
    ANSI_COLOR_WHITE,
    ANSI_COLOR_BRIGHT_BLACK,
    ANSI_COLOR_BRIGHT_RED,
    ANSI_COLOR_BRIGHT_GREEN,
    ANSI_COLOR_BRIGHT_YELLOW,
    ANSI_COLOR_BRIGHT_BLUE,
    ANSI_COLOR_BRIGHT_MAGENTA,
    ANSI_COLOR_BRIGHT_CYAN,
    ANSI_COLOR_BRIGHT_WHITE,
    ANSI_COLOR_COUNT
} ansi_color_index_t;

/** ANSI text style attributes (bitmask). */
typedef enum {
    ANSI_ATTR_NONE      = 0,
    ANSI_ATTR_BOLD      = (1 << 0),
    ANSI_ATTR_DIM       = (1 << 1),
    ANSI_ATTR_ITALIC    = (1 << 2),
    ANSI_ATTR_UNDERLINE = (1 << 3),
    ANSI_ATTR_BLINK     = (1 << 4),
    ANSI_ATTR_REVERSE   = (1 << 5),
    ANSI_ATTR_HIDDEN    = (1 << 6),
    ANSI_ATTR_STRIKE    = (1 << 7),
} ansi_attr_t;

/** Current ANSI rendering state. */
typedef struct {
    int fg_index;           /**< Current foreground color index (-1 = default). */
    int bg_index;           /**< Current background color index (-1 = default). */
    ansi_attr_t attrs;      /**< Current active attributes bitmask. */
    uint32_t fg_color;      /**< Resolved foreground LVGL color (hex). */
    uint32_t bg_color;      /**< Resolved background LVGL color (hex). */
    bool bold;              /**< Bold attribute active. */
    bool underline;         /**< Underline attribute active. */
} ansi_state_t;

/** Callback for ANSI-stripped text segments. */
typedef void (*ansi_segment_fn_t)(const char *text, const ansi_state_t *state, void *user_data);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * Initialize the ANSI module with the color palette from p4minishell_config.h.
 * Must be called once during boot, before any ANSI formatting is used.
 */
void ansi_init(void);

/**
 * Check if the ANSI module is initialized.
 * @return true if ansi_init() completed successfully.
 */
bool ansi_is_initialized(void);

/* ========================================================================
 * COLOR PALETTE
 * ======================================================================== */

/**
 * Get the LVGL hex color for a given ANSI color index.
 * @param index  Color index from ansi_color_index_t.
 * @return 24-bit RGB color value (0xRRGGBB).
 */
uint32_t ansi_get_palette_color(ansi_color_index_t index);

/**
 * Get the current default foreground color.
 * @return 24-bit RGB color value.
 */
uint32_t ansi_get_default_fg(void);

/**
 * Get the current default background color.
 * @return 24-bit RGB color value.
 */
uint32_t ansi_get_default_bg(void);

/**
 * Set a palette entry at runtime.
 * @param index  Color index to modify.
 * @param color  24-bit RGB color value.
 */
void ansi_set_palette_color(ansi_color_index_t index, uint32_t color);

/* ========================================================================
 * ANSI FORMAT STRING BUILDER
 * ======================================================================== */

/**
 * Format a string with ANSI SGR escape sequences.
 * Works like snprintf but accepts ANSI color/attribute codes as format specifiers.
 *
 * Special format specifiers (prefixed with @):
 *   @R  — reset all attributes
 *   @B  — bold on
 *   @D  — dim on
 *   @I  — italic on
 *   @U  — underline on
 *   @k  — foreground black (standard)
 *   @r  — foreground red (standard)
 *   @g  — foreground green (standard)
 *   @y  — foreground yellow (standard)
 *   @b  — foreground blue (standard)
 *   @m  — foreground magenta (standard)
 *   @c  — foreground cyan (standard)
 *   @w  — foreground white (standard)
 *   @K  — foreground bright black (grey)
 *   @E  — foreground bright red
 *   @G  — foreground bright green
 *   @Y  — foreground bright yellow
 *   @L  — foreground bright blue
 *   @M  — foreground bright magenta
 *   @C  — foreground bright cyan
 *   @W  — foreground bright white
 *   @@  — literal '@'
 *
 * Note: there are no individual "off" specifiers for bold, dim, italic, or
 * underline. Use @R to reset all attributes, or @B/@D/@I/@U to toggle them.
 * Standard printf format specifiers (%s, %d, etc.) are passed through.
 *
 * @param dst       Output buffer.
 * @param dst_size  Size of output buffer.
 * @param format    Format string with ANSI specifiers.
 * @param ...       Variable arguments for printf specifiers.
 * @return Number of bytes written (excluding null terminator).
 */
int ansi_format(char *dst, size_t dst_size, const char *format, ...);

/**
 * Variadic version of ansi_format.
 */
int ansi_vformat(char *dst, size_t dst_size, const char *format, va_list args);

/* ========================================================================
 * ANSI SEQUENCE BUILDERS (convenience macros)
 * ======================================================================== */

/** Build a reset sequence string. */
#define ANSI_SGR_RESET          ANSI_CSI "0" ANSI_SGR_END

/** Build a bold sequence string. */
#define ANSI_SGR_BOLD           ANSI_CSI "1" ANSI_SGR_END

/** Build a dim sequence string. */
#define ANSI_SGR_DIM            ANSI_CSI "2" ANSI_SGR_END

/** Build an italic sequence string. */
#define ANSI_SGR_ITALIC         ANSI_CSI "3" ANSI_SGR_END

/** Build an underline sequence string. */
#define ANSI_SGR_UNDERLINE      ANSI_CSI "4" ANSI_SGR_END

/** Build a foreground color sequence string. */
#define ANSI_SGR_FG(code)       ANSI_CSI #code ANSI_SGR_END

/** Build a background color sequence string. */
#define ANSI_SGR_BG(code)       ANSI_CSI #code ANSI_SGR_END

/** Build a combined SGR sequence with multiple codes. */
#define ANSI_SGR_FG_BG(fg, bg)  ANSI_CSI #fg ";" #bg ANSI_SGR_END

/* ========================================================================
 * ANSI TEXT PROCESSING
 * ======================================================================== */

/**
 * Strip ANSI escape sequences from text and call segment_fn for each
 * plain-text segment with its associated rendering state.
 *
 * This is the main processing function for transcript output:
 * it parses SGR sequences, tracks state changes, and emits plain-text
 * segments with their current style information.
 *
 * @param text        Input text potentially containing ANSI escape sequences.
 * @param segment_fn  Callback invoked for each plain-text segment.
 * @param user_data   Opaque pointer passed to segment_fn.
 */
void ansi_process_text(const char *text, ansi_segment_fn_t segment_fn, void *user_data);

/**
 * Convert a string containing real SGR escape sequences (as produced by
 * ansi_vformat / the @-palette pipeline) into LVGL recolor markup
 * (`#rrggbb text #`), suitable for an LVGL label with recolor enabled.
 *
 * @param src       Input text with ANSI SGR escapes.
 * @param dst       Output buffer for the recolor-markup string.
 * @param dst_size  Size of the output buffer.
 * @return          Number of bytes written to dst.
 */
int ansi_to_lvgl_recolor(const char *src, char *dst, size_t dst_size);

/**
 * Strip ANSI escape sequences entirely, returning only plain text.
 * Useful for UART console output where the terminal handles ANSI natively
 * (pass-through mode) or for plain-text logging.
 *
 * @param dst       Output buffer for plain text.
 * @param dst_size  Size of output buffer.
 * @param src       Input text potentially containing ANSI escape sequences.
 * @return Number of bytes written (excluding null terminator).
 */
int ansi_strip_to_plain(char *dst, size_t dst_size, const char *src);

/**
 * Check if a string contains any ANSI escape sequences.
 * @param text  Text to check.
 * @return true if the text contains ANSI escape sequences.
 */
bool ansi_contains_escapes(const char *text);

/* ========================================================================
 * QUICK COLOR FORMATTERS (inline helpers)
 * ======================================================================== */

/**
 * Wrap text in ANSI SGR codes for a specific foreground color.
 * Writes to dst buffer.
 */
int ansi_fg_text(char *dst, size_t dst_size, int fg_code, const char *text);

/**
 * Wrap text in ANSI SGR codes for foreground + background colors.
 * Writes to dst buffer.
 */
int ansi_fg_bg_text(char *dst, size_t dst_size, int fg_code, int bg_code, const char *text);

/**
 * Wrap text in ANSI SGR codes for a specific attribute.
 * Writes to dst buffer.
 */
int ansi_attr_text(char *dst, size_t dst_size, int attr_code, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_ANSI_H */
