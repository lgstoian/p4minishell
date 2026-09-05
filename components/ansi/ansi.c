/**
 * @file ansi.c
 * @brief ANSI/VT escape sequence module implementation for P4MiniShell.
 *
 * Implements SGR (Select Graphic Rendition) escape sequence parsing,
 * color palette management, format string building, and text processing.
 * All colors are initialized from p4minishell_config.h macros and can be
 * modified at runtime via the public API.
 *
 * Thread safety: The color palette is read-only after init for normal
 * operation. Runtime palette modifications (ansi_set_palette_color) are
 * not thread-safe by design — call only during initialization.
 */

#include "ansi.h"
#include "p4minishell_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* TUI hooks (weak linkage to avoid circular dependency). */
__attribute__((weak)) bool windows_tui_mode_active(void) { return false; }
__attribute__((weak)) void tui_set_cursor(int row, int col) { (void)row; (void)col; }
__attribute__((weak)) void tui_get_cursor(int *row, int *col) { if (row) *row=1; if (col) *col=1; }
__attribute__((weak)) void tui_clear(void) {}
__attribute__((weak)) void tui_clear_line(int mode) { (void)mode; }
__attribute__((weak)) void tui_save_cursor(void) {}
__attribute__((weak)) void tui_restore_cursor(void) {}
__attribute__((weak)) void tui_set_cursor_visible(bool visible) { (void)visible; }
__attribute__((weak)) void tui_alt_enter(void) {}
__attribute__((weak)) void tui_alt_leave(void) {}

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/** ANSI 16-color palette. Indexed by ansi_color_index_t. */
static uint32_t s_palette[ANSI_COLOR_COUNT];

/** Default foreground and background colors. */
static uint32_t s_default_fg;
static uint32_t s_default_bg;

/** 256-color palette (indices 16-255). Generated on init. */
static uint32_t s_palette_256[256];

/* Forward declarations */
static void ansi_generate_256_palette(void);

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void ansi_init(void)
{
    /* Initialize the 16-color palette from config macros.
     * These are the standard terminal colors mapped to the shell's
     * dark green-on-black aesthetic, inspired by PowerShell on Windows. */

    s_palette[ANSI_COLOR_BLACK]         = P4_CONFIG_ANSI_BLACK;
    s_palette[ANSI_COLOR_RED]           = P4_CONFIG_ANSI_RED;
    s_palette[ANSI_COLOR_GREEN]         = P4_CONFIG_ANSI_GREEN;
    s_palette[ANSI_COLOR_YELLOW]        = P4_CONFIG_ANSI_YELLOW;
    s_palette[ANSI_COLOR_BLUE]          = P4_CONFIG_ANSI_BLUE;
    s_palette[ANSI_COLOR_MAGENTA]       = P4_CONFIG_ANSI_MAGENTA;
    s_palette[ANSI_COLOR_CYAN]          = P4_CONFIG_ANSI_CYAN;
    s_palette[ANSI_COLOR_WHITE]         = P4_CONFIG_ANSI_WHITE;
    s_palette[ANSI_COLOR_BRIGHT_BLACK]  = P4_CONFIG_ANSI_BRIGHT_BLACK;
    s_palette[ANSI_COLOR_BRIGHT_RED]    = P4_CONFIG_ANSI_BRIGHT_RED;
    s_palette[ANSI_COLOR_BRIGHT_GREEN]  = P4_CONFIG_ANSI_BRIGHT_GREEN;
    s_palette[ANSI_COLOR_BRIGHT_YELLOW] = P4_CONFIG_ANSI_BRIGHT_YELLOW;
    s_palette[ANSI_COLOR_BRIGHT_BLUE]   = P4_CONFIG_ANSI_BRIGHT_BLUE;
    s_palette[ANSI_COLOR_BRIGHT_MAGENTA]= P4_CONFIG_ANSI_BRIGHT_MAGENTA;
    s_palette[ANSI_COLOR_BRIGHT_CYAN]   = P4_CONFIG_ANSI_BRIGHT_CYAN;
    s_palette[ANSI_COLOR_BRIGHT_WHITE]  = P4_CONFIG_ANSI_BRIGHT_WHITE;

    s_default_fg = P4_CONFIG_ANSI_DEFAULT_FG;
    s_default_bg = P4_CONFIG_ANSI_DEFAULT_BG;

    /* Generate 256-color palette. */
    ansi_generate_256_palette();

    s_initialized = true;
}

bool ansi_is_initialized(void)
{
    return s_initialized;
}

/* ========================================================================
 * COLOR PALETTE
 * ======================================================================== */

uint32_t ansi_get_palette_color(ansi_color_index_t index)
{
    if (index < 0 || index >= ANSI_COLOR_COUNT) {
        return s_default_fg;
    }
    return s_palette[index];
}

uint32_t ansi_get_default_fg(void)
{
    return s_default_fg;
}

uint32_t ansi_get_default_bg(void)
{
    return s_default_bg;
}

void ansi_set_palette_color(ansi_color_index_t index, uint32_t color)
{
    if (index >= 0 && index < ANSI_COLOR_COUNT) {
        s_palette[index] = color;
    }
}

/* ========================================================================
 * SGR CODE TO COLOR INDEX MAPPING
 * ======================================================================== */

/**
 * Map an SGR foreground color code (30-37, 90-97) to a palette index.
 * Returns -1 if not a valid foreground color code.
 */
static int ansi_sgr_to_fg_index(int code)
{
    switch (code) {
    case ANSI_FG_BLACK:        return ANSI_COLOR_BLACK;
    case ANSI_FG_RED:          return ANSI_COLOR_RED;
    case ANSI_FG_GREEN:        return ANSI_COLOR_GREEN;
    case ANSI_FG_YELLOW:       return ANSI_COLOR_YELLOW;
    case ANSI_FG_BLUE:         return ANSI_COLOR_BLUE;
    case ANSI_FG_MAGENTA:      return ANSI_COLOR_MAGENTA;
    case ANSI_FG_CYAN:         return ANSI_COLOR_CYAN;
    case ANSI_FG_WHITE:        return ANSI_COLOR_WHITE;
    case ANSI_FG_BRIGHT_BLACK: return ANSI_COLOR_BRIGHT_BLACK;
    case ANSI_FG_BRIGHT_RED:   return ANSI_COLOR_BRIGHT_RED;
    case ANSI_FG_BRIGHT_GREEN: return ANSI_COLOR_BRIGHT_GREEN;
    case ANSI_FG_BRIGHT_YELLOW:return ANSI_COLOR_BRIGHT_YELLOW;
    case ANSI_FG_BRIGHT_BLUE:  return ANSI_COLOR_BRIGHT_BLUE;
    case ANSI_FG_BRIGHT_MAGENTA:return ANSI_COLOR_BRIGHT_MAGENTA;
    case ANSI_FG_BRIGHT_CYAN:  return ANSI_COLOR_BRIGHT_CYAN;
    case ANSI_FG_BRIGHT_WHITE: return ANSI_COLOR_BRIGHT_WHITE;
    case ANSI_FG_DEFAULT:      return -2; /* special: default */
    default:                   return -1; /* not a color code */
    }
}

/**
 * Map an SGR background color code (40-47, 100-107) to a palette index.
 * Returns -1 if not a valid background color code.
 */
static int ansi_sgr_to_bg_index(int code)
{
    switch (code) {
    case ANSI_BG_BLACK:        return ANSI_COLOR_BLACK;
    case ANSI_BG_RED:          return ANSI_COLOR_RED;
    case ANSI_BG_GREEN:        return ANSI_COLOR_GREEN;
    case ANSI_BG_YELLOW:       return ANSI_COLOR_YELLOW;
    case ANSI_BG_BLUE:         return ANSI_COLOR_BLUE;
    case ANSI_BG_MAGENTA:      return ANSI_COLOR_MAGENTA;
    case ANSI_BG_CYAN:         return ANSI_COLOR_CYAN;
    case ANSI_BG_WHITE:        return ANSI_COLOR_WHITE;
    case ANSI_BG_BRIGHT_BLACK: return ANSI_COLOR_BRIGHT_BLACK;
    case ANSI_BG_BRIGHT_RED:   return ANSI_COLOR_BRIGHT_RED;
    case ANSI_BG_BRIGHT_GREEN: return ANSI_COLOR_BRIGHT_GREEN;
    case ANSI_BG_BRIGHT_YELLOW:return ANSI_COLOR_BRIGHT_YELLOW;
    case ANSI_BG_BRIGHT_BLUE:  return ANSI_COLOR_BRIGHT_BLUE;
    case ANSI_BG_BRIGHT_MAGENTA:return ANSI_COLOR_BRIGHT_MAGENTA;
    case ANSI_BG_BRIGHT_CYAN:  return ANSI_COLOR_BRIGHT_CYAN;
    case ANSI_BG_BRIGHT_WHITE: return ANSI_COLOR_BRIGHT_WHITE;
    case ANSI_BG_DEFAULT:      return -2; /* special: default */
    default:                   return -1; /* not a color code */
    }
}

/* ========================================================================
 * ANSI STATE MACHINE
 * ======================================================================== */

/**
 * Initialize an ANSI state to defaults.
 */
static void ansi_state_init(ansi_state_t *state)
{
    if (state == NULL) return;
    state->fg_index = -1;
    state->bg_index = -1;
    state->attrs = ANSI_ATTR_NONE;
    state->fg_color = s_default_fg;
    state->bg_color = s_default_bg;
    state->bold = false;
    state->underline = false;
}

/**
 * Apply an SGR code to the ANSI state.
 */
static void ansi_state_apply_sgr(ansi_state_t *state, int code)
{
    int idx;

    if (state == NULL) return;

    switch (code) {
    case ANSI_RESET:
        ansi_state_init(state);
        break;

    case ANSI_BOLD:
        state->attrs |= ANSI_ATTR_BOLD;
        state->bold = true;
        break;

    case ANSI_DIM:
        state->attrs |= ANSI_ATTR_DIM;
        break;

    case ANSI_ITALIC:
        state->attrs |= ANSI_ATTR_ITALIC;
        break;

    case ANSI_UNDERLINE:
        state->attrs |= ANSI_ATTR_UNDERLINE;
        state->underline = true;
        break;

    case ANSI_BLINK:
        state->attrs |= ANSI_ATTR_BLINK;
        break;

    case ANSI_REVERSE:
        state->attrs |= ANSI_ATTR_REVERSE;
        break;

    case ANSI_HIDDEN:
        state->attrs |= ANSI_ATTR_HIDDEN;
        break;

    case ANSI_STRIKE:
        state->attrs |= ANSI_ATTR_STRIKE;
        break;

    /* Bold/underline off (22 = normal intensity, 24 = underline off) */
    case 22:
        state->attrs &= ~(ANSI_ATTR_BOLD | ANSI_ATTR_DIM);
        state->bold = false;
        break;

    case 23:
        state->attrs &= ~ANSI_ATTR_ITALIC;
        break;

    case 24:
        state->attrs &= ~ANSI_ATTR_UNDERLINE;
        state->underline = false;
        break;

    case 25:
        state->attrs &= ~ANSI_ATTR_BLINK;
        break;

    case 27:
        state->attrs &= ~ANSI_ATTR_REVERSE;
        break;

    case 28:
        state->attrs &= ~ANSI_ATTR_HIDDEN;
        break;

    case 29:
        state->attrs &= ~ANSI_ATTR_STRIKE;
        break;

    default:
        /* Try foreground color */
        idx = ansi_sgr_to_fg_index(code);
        if (idx >= 0) {
            state->fg_index = idx;
            state->fg_color = s_palette[idx];
            state->attrs &= ~ANSI_ATTR_DIM; /* explicit color clears dim */
            return;
        }
        if (idx == -2) {
            /* ANSI_FG_DEFAULT */
            state->fg_index = -1;
            state->fg_color = s_default_fg;
            return;
        }

        /* Try background color */
        idx = ansi_sgr_to_bg_index(code);
        if (idx >= 0) {
            state->bg_index = idx;
            state->bg_color = s_palette[idx];
            return;
        }
        if (idx == -2) {
            /* ANSI_BG_DEFAULT */
            state->bg_index = -1;
            state->bg_color = s_default_bg;
            return;
        }
        break;
    }
}

/* ========================================================================
 * ANSI TEXT PROCESSING
 * ======================================================================== */

void ansi_process_text(const char *text, ansi_segment_fn_t segment_fn, void *user_data)
{
    ansi_state_t state;
    const char *p;
    const char *segment_start;
    int sgr_codes[16];
    int sgr_count;

    if (text == NULL || segment_fn == NULL) {
        return;
    }

    ansi_state_init(&state);
    p = text;
    segment_start = p;

    while (*p != '\0') {
        /* Look for ESC [ sequence */
        if (*p == '\x1B' && *(p + 1) == '[') {
            /* Flush current segment if non-empty */
            if (p > segment_start) {
                /* Temporarily null-terminate the segment */
                char saved = *p;
                *(char *)p = '\0';
                segment_fn(segment_start, &state, user_data);
                *(char *)p = saved;
            }

            /* Parse SGR parameters: ESC [ ... m */
            p += 2; /* Skip ESC [ */
            sgr_count = 0;

            /* Parse semicolon-separated numeric parameters */
            while (*p != '\0' && sgr_count < 16) {
                /* Skip non-digit characters (except ';' and 'm') */
                if (*p == ';') {
                    p++;
                    continue;
                }
                if (*p == 'm') {
                    p++;
                    break;
                }
                if (isdigit((unsigned char)*p)) {
                    int val = 0;
                    while (isdigit((unsigned char)*p)) {
                        val = val * 10 + (*p - '0');
                        p++;
                    }
                    sgr_codes[sgr_count++] = val;
                } else {
                    /* Unknown character in CSI — skip the whole sequence */
                    while (*p != '\0' && *p != 'm') {
                        p++;
                    }
                    if (*p == 'm') p++;
                    sgr_count = 0;
                    break;
                }
            }

            /* Apply all parsed SGR codes */
            for (int i = 0; i < sgr_count; i++) {
                ansi_state_apply_sgr(&state, sgr_codes[i]);
            }

            segment_start = p;
        } else {
            p++;
        }
    }

    /* Flush final segment */
    if (p > segment_start) {
        segment_fn(segment_start, &state, user_data);
    }
}

int ansi_strip_to_plain(char *dst, size_t dst_size, const char *src)
{
    const char *p;
    size_t written = 0;

    if (dst == NULL || dst_size == 0 || src == NULL) {
        return 0;
    }

    p = src;
    while (*p != '\0' && written < dst_size - 1) {
        if (*p == '\x1B' && *(p + 1) == '[') {
            /* Skip the entire escape sequence */
            p += 2;
            while (*p != '\0' && *p != 'm') {
                p++;
            }
            if (*p == 'm') p++;
        } else {
            dst[written++] = *p++;
        }
    }

    dst[written] = '\0';
    return (int)written;
}

bool ansi_contains_escapes(const char *text)
{
    if (text == NULL) return false;
    return strchr(text, '\x1B') != NULL;
}

/* ========================================================================
 * ANSI FORMAT STRING BUILDER
 * ======================================================================== */

/**
 * Internal: write an SGR code sequence into dst.
 * Returns number of bytes written.
 */
static int ansi_write_sgr(char *dst, size_t dst_size, size_t *pos, int code)
{
    char buf[16];
    int len;

    len = snprintf(buf, sizeof(buf), "\x1B[%dm", code);
    if (len < 0) return 0;

    if (*pos + (size_t)len < dst_size) {
        memcpy(dst + *pos, buf, (size_t)len);
        *pos += (size_t)len;
        return len;
    }
    return 0;
}

/* ========================================================================
 * 256-COLOR PALETTE GENERATION
 * ======================================================================== */

static uint32_t s_palette_256[256];

/**
 * Generate the 256-color palette:
 *   0-15:    Standard 16 colors (from config)
 *   16-231:  6x6x6 color cube (216 colors)
 *   232-255: Grayscale ramp (24 steps)
 */
static void ansi_generate_256_palette(void)
{
    int i;

    /* Indices 0-15: standard 16 colors from config */
    s_palette_256[0]  = P4_CONFIG_ANSI_BLACK;
    s_palette_256[1]  = P4_CONFIG_ANSI_RED;
    s_palette_256[2]  = P4_CONFIG_ANSI_GREEN;
    s_palette_256[3]  = P4_CONFIG_ANSI_YELLOW;
    s_palette_256[4]  = P4_CONFIG_ANSI_BLUE;
    s_palette_256[5]  = P4_CONFIG_ANSI_MAGENTA;
    s_palette_256[6]  = P4_CONFIG_ANSI_CYAN;
    s_palette_256[7]  = P4_CONFIG_ANSI_WHITE;
    s_palette_256[8]  = P4_CONFIG_ANSI_BRIGHT_BLACK;
    s_palette_256[9]  = P4_CONFIG_ANSI_BRIGHT_RED;
    s_palette_256[10] = P4_CONFIG_ANSI_BRIGHT_GREEN;
    s_palette_256[11] = P4_CONFIG_ANSI_BRIGHT_YELLOW;
    s_palette_256[12] = P4_CONFIG_ANSI_BRIGHT_BLUE;
    s_palette_256[13] = P4_CONFIG_ANSI_BRIGHT_MAGENTA;
    s_palette_256[14] = P4_CONFIG_ANSI_BRIGHT_CYAN;
    s_palette_256[15] = P4_CONFIG_ANSI_BRIGHT_WHITE;

    /* Indices 16-231: 6x6x6 color cube */
    static const uint8_t cube_vals[6] = {0x00, 0x33, 0x66, 0x99, 0xCC, 0xFF};
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + (r * 36) + (g * 6) + b;
                s_palette_256[idx] = ((uint32_t)cube_vals[r] << 16) |
                                     ((uint32_t)cube_vals[g] << 8) |
                                     (uint32_t)cube_vals[b];
            }
        }
    }

    /* Indices 232-255: Grayscale ramp (24 steps) */
    for (i = 0; i < 24; i++) {
        uint8_t v = 0x08 + i * 10; /* 0x08, 0x12, 0x1C, ... 0xEE */
        s_palette_256[232 + i] = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    }
}

/* ========================================================================
 * 256-COLOR PALETTE RGB LOOKUP
 * ======================================================================== */

void ansi_256_color_rgb(int index, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t color;

    if (index < 0 || index >= 256) {
        index = 0;
    }
    color = s_palette_256[index];
    *r = (color >> 16) & 0xFF;
    *g = (color >> 8) & 0xFF;
    *b = color & 0xFF;
}

/* ========================================================================
 * EXTENDED SGR PARSING (256-color, truecolor)
 * ======================================================================== */

/**
 * Apply a parsed SGR sequence (potentially multi-parameter) to the ANSI state.
 * Handles 16-color, 256-color (38;5;n / 48;5;n), and truecolor (38;2;r;g;b / 48;2;r;g;b).
 */
static void ansi_apply_sgr_sequence(ansi_state_t *state, const int *codes, int count)
{
    int i = 0;

    while (i < count) {
        int code = codes[i++];

        switch (code) {
        case ANSI_RESET:
            ansi_state_init(state);
            break;

        case ANSI_BOLD:
            state->attrs |= ANSI_ATTR_BOLD;
            state->bold = true;
            break;

        case ANSI_DIM:
            state->attrs |= ANSI_ATTR_DIM;
            break;

        case ANSI_ITALIC:
            state->attrs |= ANSI_ATTR_ITALIC;
            break;

        case ANSI_UNDERLINE:
            state->attrs |= ANSI_ATTR_UNDERLINE;
            state->underline = true;
            break;

        case ANSI_BLINK:
            state->attrs |= ANSI_ATTR_BLINK;
            break;

        case ANSI_REVERSE:
            state->attrs |= ANSI_ATTR_REVERSE;
            break;

        case ANSI_HIDDEN:
            state->attrs |= ANSI_ATTR_HIDDEN;
            break;

        case ANSI_STRIKE:
            state->attrs |= ANSI_ATTR_STRIKE;
            break;

        case 22:
            state->attrs &= ~(ANSI_ATTR_BOLD | ANSI_ATTR_DIM);
            state->bold = false;
            break;

        case 23:
            state->attrs &= ~ANSI_ATTR_ITALIC;
            break;

        case 24:
            state->attrs &= ~ANSI_ATTR_UNDERLINE;
            state->underline = false;
            break;

        case 25:
            state->attrs &= ~ANSI_ATTR_BLINK;
            break;

        case 27:
            state->attrs &= ~ANSI_ATTR_REVERSE;
            break;

        case 28:
            state->attrs &= ~ANSI_ATTR_HIDDEN;
            break;

        case 29:
            state->attrs &= ~ANSI_ATTR_STRIKE;
            break;

        case ANSI_FG_256_PREFIX:  /* 38 - extended foreground */
        case ANSI_BG_256_PREFIX:  /* 48 - extended background */
            if (i >= count) break;
            int subcode = codes[i++];
            bool is_fg = (code == ANSI_FG_256_PREFIX);

            if (subcode == 5) {  /* 256-color mode: 38;5;n or 48;5;n */
                if (i >= count) break;
                int idx = codes[i++];
                if (idx >= 0 && idx < 256) {
                    if (is_fg) {
                        state->fg_mode = ANSI_COLOR_MODE_256;
                        state->fg_index = idx;
                        state->fg_color = s_palette_256[idx];
                    } else {
                        state->bg_mode = ANSI_COLOR_MODE_256;
                        state->bg_index = idx;
                        state->bg_color = s_palette_256[idx];
                    }
                }
            } else if (subcode == 2) {  /* Truecolor mode: 38;2;r;g;b or 48;2;r;g;b */
                if (i + 2 >= count) break;
                uint8_t r = (uint8_t)codes[i++];
                uint8_t g = (uint8_t)codes[i++];
                uint8_t b = (uint8_t)codes[i++];
                if (is_fg) {
                    state->fg_mode = ANSI_COLOR_MODE_TRUECOLOR;
                    state->fg_r = r;
                    state->fg_g = g;
                    state->fg_b = b;
                    state->fg_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                } else {
                    state->bg_mode = ANSI_COLOR_MODE_TRUECOLOR;
                    state->bg_r = r;
                    state->bg_g = g;
                    state->bg_b = b;
                    state->bg_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
            }
            break;

        default:
            /* Try standard foreground color */
            int idx = ansi_sgr_to_fg_index(code);
            if (idx >= 0) {
                state->fg_mode = ANSI_COLOR_MODE_16;
                state->fg_index = idx;
                state->fg_color = s_palette[idx];
                state->attrs &= ~ANSI_ATTR_DIM; /* explicit color clears dim */
                break;
            }
            if (idx == -2) {  /* ANSI_FG_DEFAULT (39) */
                state->fg_mode = ANSI_COLOR_MODE_16;
                state->fg_index = -1;
                state->fg_color = s_default_fg;
                break;
            }

            /* Try standard background color */
            idx = ansi_sgr_to_bg_index(code);
            if (idx >= 0) {
                state->bg_mode = ANSI_COLOR_MODE_16;
                state->bg_index = idx;
                state->bg_color = s_palette[idx];
                break;
            }
            if (idx == -2) {  /* ANSI_BG_DEFAULT (49) */
                state->bg_mode = ANSI_COLOR_MODE_16;
                state->bg_index = -1;
                state->bg_color = s_default_bg;
                break;
            }
            break;
        }
    }
}

/* ========================================================================
 * HELPER FUNCTIONS FOR ansi_vformat
 * ======================================================================== */

static bool ansi_check_prefix(const char **p, const char *prefix)
{
    size_t len = strlen(prefix);
    if (strncmp(*p, prefix, len) == 0) {
        *p += len;
        return true;
    }
    return false;
}

static int ansi_parse_int(const char **p)
{
    int val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (*(*p)++ - '0');
    }
    return val;
}

static bool ansi_parse_hex_color(const char **p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (strlen(*p) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)(*p)[i])) return false;
    }
    uint32_t rgb = strtoul(*p, NULL, 16);
    *r = (rgb >> 16) & 0xFF;
    *g = (rgb >> 8) & 0xFF;
    *b = rgb & 0xFF;
    *p += 6;
    return true;
}

static int ansi_write_csi(char *dst, size_t dst_size, size_t *pos, const char *fmt, ...)
{
    char buf[32];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) return 0;
    if (*pos + (size_t)len < dst_size) {
        memcpy(dst + *pos, buf, (size_t)len);
        *pos += (size_t)len;
        return len;
    }
    return 0;
}

int ansi_vformat(char *dst, size_t dst_size, const char *format, va_list args)
{
    size_t pos = 0;
    const char *p;
    char temp[256];

    if (dst == NULL || dst_size == 0 || format == NULL) {
        return 0;
    }

    dst[0] = '\0';
    p = format;

    while (*p != '\0' && pos < dst_size - 1) {
        if (*p == '@') {
            p++;

            if (*p == '\0') goto done;

            /* Check for parameterized specifiers first (those with colons) */
            if (ansi_check_prefix(&p, "POS:")) {
                int row = ansi_parse_int(&p);
                if (*p == ';') p++;
                int col = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%d;%dH", row, col);
                continue;
            }
            if (ansi_check_prefix(&p, "UP:")) {
                int n = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%dA", n);
                continue;
            }
            if (ansi_check_prefix(&p, "DOWN:")) {
                int n = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%dB", n);
                continue;
            }
            if (ansi_check_prefix(&p, "LEFT:")) {
                int n = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%dD", n);
                continue;
            }
            if (ansi_check_prefix(&p, "RIGHT:")) {
                int n = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%dC", n);
                continue;
            }
            if (ansi_check_prefix(&p, "SAVE")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[s");
                continue;
            }
            if (ansi_check_prefix(&p, "RESTORE")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[u");
                continue;
            }
            if (ansi_check_prefix(&p, "CLEAR_LINE")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[2K");
                continue;
            }
            if (ansi_check_prefix(&p, "CLEAR_SCREEN")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[3J");
                continue;
            }
            if (ansi_check_prefix(&p, "CLEAR")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[2J");
                continue;
            }
            if (ansi_check_prefix(&p, "SCROLL:")) {
                int top = ansi_parse_int(&p);
                if (*p == ';') p++;
                int bot = ansi_parse_int(&p);
                ansi_write_csi(dst, dst_size, &pos, "\x1B[%d;%dr", top, bot);
                continue;
            }
            if (ansi_check_prefix(&p, "ALTON")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[?1049h");
                continue;
            }
            if (ansi_check_prefix(&p, "ALTOFF")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[?1049l");
                continue;
            }
            if (ansi_check_prefix(&p, "CURSON")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[?25h");
                continue;
            }
            if (ansi_check_prefix(&p, "CURSOFF")) {
                ansi_write_csi(dst, dst_size, &pos, "\x1B[?25l");
                continue;
            }
            if (ansi_check_prefix(&p, "256:")) {
                int idx = ansi_parse_int(&p);
                if (idx >= 0 && idx < 256) {
                    ansi_write_csi(dst, dst_size, &pos, "\x1B[38;5;%dm", idx);
                }
                continue;
            }
            if (ansi_check_prefix(&p, "BG256:")) {
                int idx = ansi_parse_int(&p);
                if (idx >= 0 && idx < 256) {
                    ansi_write_csi(dst, dst_size, &pos, "\x1B[48;5;%dm", idx);
                }
                continue;
            }
            if (ansi_check_prefix(&p, "RGB:")) {
                uint8_t r, g, b;
                if (ansi_parse_hex_color(&p, &r, &g, &b)) {
                    ansi_write_csi(dst, dst_size, &pos, "\x1B[38;2;%d;%d;%dm", r, g, b);
                }
                continue;
            }
            if (ansi_check_prefix(&p, "BGRGB:")) {
                uint8_t r, g, b;
                if (ansi_parse_hex_color(&p, &r, &g, &b)) {
                    ansi_write_csi(dst, dst_size, &pos, "\x1B[48;2;%d;%d;%dm", r, g, b);
                }
                continue;
            }

            /* Single-character specifiers */
            if (*p == '\0') goto done;

            switch (*p) {
            case '\0': goto done;

            /* Reset */
            case 'R':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_RESET);
                p++;
                break;

            /* Bold */
            case 'B':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_BOLD);
                p++;
                break;

            /* Dim */
            case 'D':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_DIM);
                p++;
                break;

            /* Italic */
            case 'I':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_ITALIC);
                p++;
                break;

            /* Underline */
            case 'U':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_UNDERLINE);
                p++;
                break;

            /* Foreground colors */
            case 'k':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BLACK);
                p++;
                break;
            case 'r':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_RED);
                p++;
                break;
            case 'g':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_GREEN);
                p++;
                break;
            case 'y':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_YELLOW);
                p++;
                break;
            case 'b':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BLUE);
                p++;
                break;
            case 'm':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_MAGENTA);
                p++;
                break;
            case 'c':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_CYAN);
                p++;
                break;
            case 'w':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_WHITE);
                p++;
                break;

            /* Bright foreground */
            case 'K':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_BLACK);
                p++;
                break;
            case 'E':
                /* @E = bright rEd */
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_RED);
                p++;
                break;
            case 'G':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_GREEN);
                p++;
                break;
            case 'Y':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_YELLOW);
                p++;
                break;
            case 'L':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_BLUE);
                p++;
                break;
            case 'M':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_MAGENTA);
                p++;
                break;
            case 'C':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_CYAN);
                p++;
                break;
            case 'W':
                ansi_write_sgr(dst, dst_size, &pos, ANSI_FG_BRIGHT_WHITE);
                p++;
                break;

            /* PowerShell semantic tokens — map to PS color scheme.
             * Use unique letters that don't conflict with existing color codes:
             * @P=PS prefix, @H=Path, @Q=Suffix, @X=Error, @V=Warning,
             * @O=Success, @N=Info, @T=String, @Z=Number, @F=Path value */
            case 'P':  /* @P = PS prefix ("PS ") */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_PREFIX);
                p++;
                break;
            case 'H':  /* @H = Path in prompt */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_PATH);
                p++;
                break;
            case 'Q':  /* @Q = Prompt suffix ("> ") */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_SUFFIX);
                p++;
                break;
            case 'X':  /* @X = Error prefix */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_ERROR);
                p++;
                break;
            case 'V':  /* @V = Warning prefix */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_WARNING);
                p++;
                break;
            case 'O':  /* @O = Success prefix */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_SUCCESS);
                p++;
                break;
            case 'N':  /* @N = Info prefix */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_INFO);
                p++;
                break;
            case 'T':  /* @T = String value */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_STRING);
                p++;
                break;
            case 'Z':  /* @Z = Numeric value */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_NUMBER);
                p++;
                break;
            case 'F':  /* @F = Path value */
                ansi_write_sgr(dst, dst_size, &pos, P4_CONFIG_PS_COLOR_PATH_VALUE);
                p++;
                break;

            /* @@ is literal @ */
            case '@':
                if (pos < dst_size - 1) {
                    dst[pos++] = '@';
                }
                p++;
                break;

            default:
                /* Unknown @ code — output literal @ and the char */
                if (pos < dst_size - 1) {
                    dst[pos++] = '@';
                }
                if (pos < dst_size - 1) {
                    dst[pos++] = *p;
                }
                p++;
                break;
            }
        } else if (*p == '%') {
            /* Standard printf format specifier */
            const char *fmt_start = p;
            p++;
            /* Parse the format specifier */
            while (*p != '\0' && strchr("diouxXfFeEgGaAcspn%", *p) == NULL) {
                p++;
            }
            if (*p == '%') {
                /* Literal %% */
                if (pos < dst_size - 1) {
                    dst[pos++] = '%';
                }
                p++;
            } else if (*p != '\0') {
                /* Capture the full format specifier, including any flags,
                 * width, precision, and length modifiers. */
                size_t fmt_len = (size_t)(p - fmt_start) + 1;
                if (fmt_len < sizeof(temp)) {
                    char spec_buf[32];
                    char spec;
                    int written;
                    bool is_long = false;
                    bool is_long_long = false;

                    memcpy(spec_buf, fmt_start, fmt_len);
                    spec_buf[fmt_len] = '\0';
                    p++;

                    spec = spec_buf[fmt_len - 1];

                    /* Detect length modifiers so the correct type is pulled
                     * from the va_list; `ll` must be checked before `l`. */
                    if (fmt_len >= 3 && spec_buf[fmt_len - 3] == 'l' && spec_buf[fmt_len - 2] == 'l') {
                        is_long_long = true;
                    } else if (fmt_len >= 2 && spec_buf[fmt_len - 2] == 'l') {
                        is_long = true;
                    }

                    /* The captured specifier is handed to snprintf verbatim,
                     * so flags such as %-4s, %10s, and %.1f behave exactly as
                     * they do in printf. Passing only the bare conversion
                     * would silently drop the caller's column alignment. */
                    if (spec == 's') {
                        char *s = va_arg(args, char *);
                        written = snprintf(temp, sizeof(temp), spec_buf, s ? s : "(null)");
                    } else if (spec == 'd' || spec == 'i') {
                        if (is_long_long) {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, long long));
                        } else if (is_long) {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, long));
                        } else {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, int));
                        }
                    } else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
                        if (is_long_long) {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, unsigned long long));
                        } else if (is_long) {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, unsigned long));
                        } else {
                            written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, unsigned int));
                        }
                    } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
                               spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A') {
                        written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, double));
                    } else if (spec == 'c') {
                        written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, int));
                    } else if (spec == 'p') {
                        written = snprintf(temp, sizeof(temp), spec_buf, va_arg(args, void *));
                    } else {
                        written = snprintf(temp, sizeof(temp), "%s", "(fmt)");
                    }

                    if (written > 0) {
                        size_t wlen = (size_t)written;
                        if (pos + wlen < dst_size) {
                            memcpy(dst + pos, temp, wlen);
                            pos += wlen;
                        } else if (pos < dst_size - 1) {
                            size_t rem = dst_size - 1 - pos;
                            memcpy(dst + pos, temp, rem);
                            pos += rem;
                        }
                    }
                }
            }
        } else {
            /* Regular character */
            dst[pos++] = *p++;
        }
    }

done:
    dst[pos] = '\0';
    return (int)pos;
}

int ansi_format(char *dst, size_t dst_size, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = ansi_vformat(dst, dst_size, format, args);
    va_end(args);

    return result;
}

/* ========================================================================
 * QUICK COLOR FORMATTERS
 * ======================================================================== */

int ansi_fg_text(char *dst, size_t dst_size, int fg_code, const char *text)
{
    return snprintf(dst, dst_size, "\x1B[%dm%s\x1B[0m", fg_code, text ? text : "");
}

int ansi_fg_bg_text(char *dst, size_t dst_size, int fg_code, int bg_code, const char *text)
{
    return snprintf(dst, dst_size, "\x1B[%d;%dm%s\x1B[0m", fg_code, bg_code, text ? text : "");
}

int ansi_attr_text(char *dst, size_t dst_size, int attr_code, const char *text)
{
    return snprintf(dst, dst_size, "\x1B[%dm%s\x1B[0m", attr_code, text ? text : "");
}

/* ========================================================================
 * LVGL RECOLOR MARKUP
 * ========================================================================
 * LVGL labels with lv_label_set_recolor(true) render per-span colors using
 * `#rrggbb text #` markup. This converts a string containing real SGR escape
 * sequences (as produced by ansi_vformat / the @-palette pipeline) into that
 * markup, so the LVGL transcript can show the same colours as the UART
 * console.
 */

typedef struct {
    char *dst;
    size_t dst_size;
    size_t pos;
    uint32_t last_color;
    bool last_color_set;
} ansi_recolor_ctx_t;

static void ansi_recolor_segment(const char *text, const ansi_state_t *state, void *user_data)
{
    ansi_recolor_ctx_t *ctx = (ansi_recolor_ctx_t *)user_data;
    size_t len;
    size_t needed;

    if (text == NULL || state == NULL || ctx == NULL) {
        return;
    }

    len = strlen(text);
    if (len == 0) {
        return;
    }

    /* Estimate the bytes needed for this segment including a colour-change
     * marker (open "#rrggbb " + close " #") plus the text. */
    needed = len + 1;
    if (!ctx->last_color_set || ctx->last_color != state->fg_color) {
        needed += 10 + 2;
    }

    /* Keep the newest output: a terminal transcript must never lose its tail.
     * When the staged markup cannot hold the whole scrollback, discard what is
     * already staged and restart from this segment, so the newest lines are the
     * ones that stay visible. A single segment larger than the whole buffer
     * cannot be rendered and is dropped. */
    if (ctx->pos + needed > ctx->dst_size) {
        ctx->pos = 0;
        ctx->last_color_set = false;
        ctx->dst[0] = '\0';
        if (needed > ctx->dst_size) {
            return;
        }
    }

    /* Emit a recolor open/close pair only when the foreground differs from
     * the previous segment's, so runs of the same colour stay compact. */
    if (!ctx->last_color_set || ctx->last_color != state->fg_color) {
        if (ctx->last_color_set) {
            ctx->dst[ctx->pos++] = ' ';
            ctx->dst[ctx->pos++] = '#';
        }
        int written = snprintf(ctx->dst + ctx->pos, ctx->dst_size - ctx->pos,
                               "#%06X ", (unsigned int)(state->fg_color & 0xFFFFFFu));
        if (written > 0) {
            ctx->pos += (size_t)written;
        }
        ctx->last_color = state->fg_color;
        ctx->last_color_set = true;
    }

    if (ctx->pos + len + 1 > ctx->dst_size) {
        return;
    }
    memcpy(ctx->dst + ctx->pos, text, len);
    ctx->pos += len;
}

int ansi_to_lvgl_recolor(const char *src, char *dst, size_t dst_size)
{
    ansi_recolor_ctx_t ctx;

    if (src == NULL || dst == NULL || dst_size == 0) {
        return 0;
    }

    ctx.dst = dst;
    ctx.dst_size = dst_size;
    ctx.pos = 0;
    ctx.last_color = 0;
    ctx.last_color_set = false;

    dst[0] = '\0';

    ansi_process_text(src, ansi_recolor_segment, &ctx);

    /* Close any open colour span. */
    if (ctx.last_color_set && ctx.pos + 2 < ctx.dst_size) {
        ctx.dst[ctx.pos++] = ' ';
        ctx.dst[ctx.pos++] = '#';
    }

    if (ctx.pos < ctx.dst_size) {
        ctx.dst[ctx.pos] = '\0';
    } else {
        ctx.dst[ctx.dst_size - 1] = '\0';
    }
    return (int)ctx.pos;
}

/* ========================================================================
 * EXTENDED ANSI PROCESSING (256-color, truecolor, cursor/screen control)
 * ======================================================================== */

/**
 * Process ANSI text with extended color support (256-color, truecolor).
 * Also handles cursor/screen control sequences (CUP, ED, EL, DECSTBM, etc.).
 * @param text        Input text potentially containing ANSI escape sequences.
 * @param segment_fn  Extended callback for each plain-text segment.
 * @param user_data   Opaque pointer passed to segment_fn.
 */
void ansi_process_text_ex(const char *text, ansi_segment_ex_fn_t segment_fn, void *user_data)
{
    ansi_state_t state;
    const char *p;
    const char *segment_start;
    int sgr_codes[32];
    int sgr_count;

    if (text == NULL || segment_fn == NULL) {
        return;
    }

    ansi_state_init(&state);
    p = text;
    segment_start = p;

    while (*p != '\0') {
        /* Look for ESC [ sequence */
        if (*p == '\x1B' && *(p + 1) == '[') {
            /* Flush current segment if non-empty */
            if (p > segment_start) {
                /* Temporarily null-terminate the segment */
                char saved = *p;
                *(char *)p = '\0';
                segment_fn(segment_start, &state, user_data);
                *(char *)p = saved;
            }

            /* Parse CSI sequence: ESC [ ... <final_char> */
            p += 2; /* Skip ESC [ */
            sgr_count = 0;

            /* Parse semicolon-separated numeric parameters */
            while (*p != '\0' && sgr_count < 32) {
                /* Skip non-digit characters (except ';' and final chars) */
                if (*p == ';') {
                    p++;
                    continue;
                }

                /* Check for CSI final characters */
                if (*p == '?' ) {
                    /* Private mode prefix (e.g. ?25, ?1049) — consume and keep parsing numbers */
                    p++;
                }
                if (*p == 'm' || *p == 'H' || *p == 'J' || *p == 'K' ||
                    *p == 'r' || *p == 's' || *p == 'u' || *p == 'h' || *p == 'l' ||
                    *p == 'A' || *p == 'B' || *p == 'C' || *p == 'D') {
                    char final_char = *p++;
                    if (final_char == 'm') {
                        ansi_apply_sgr_sequence(&state, sgr_codes, sgr_count);
                    } else if (windows_tui_mode_active()) {
                        if (final_char == 'H') {
                            int row = (sgr_count >= 1 && sgr_codes[0] > 0) ? sgr_codes[0] : 1;
                            int col = (sgr_count >= 2 && sgr_codes[1] > 0) ? sgr_codes[1] : 1;
                            tui_set_cursor(row, col);
                        } else if (final_char == 'J') {
                            int mode = (sgr_count >= 1) ? sgr_codes[0] : 0;
                            if (mode == 2 || mode == 3) tui_clear();
                        } else if (final_char == 'K') {
                            int mode = (sgr_count >= 1) ? sgr_codes[0] : 0;
                            if (mode == 2) tui_clear_line(2);
                            else if (mode == 1) tui_clear_line(1);
                            else tui_clear_line(0);
                        } else if (final_char == 's') {
                            tui_save_cursor();
                        } else if (final_char == 'u') {
                            tui_restore_cursor();
                        } else if (final_char == 'h') {
                            int code = (sgr_count >= 1) ? sgr_codes[0] : 0;
                            if (code == 25) tui_set_cursor_visible(true);
                            else if (code == 1049) tui_alt_enter();
                        } else if (final_char == 'l') {
                            int code = (sgr_count >= 1) ? sgr_codes[0] : 0;
                            if (code == 25) tui_set_cursor_visible(false);
                            else if (code == 1049) tui_alt_leave();
                        } else if (final_char == 'A' || final_char == 'B' || final_char == 'C' || final_char == 'D') {
                            int n = (sgr_count >= 1 && sgr_codes[0] > 0) ? sgr_codes[0] : 1;
                            int r, c;
                            tui_get_cursor(&r, &c);
                            if (final_char == 'A') r -= n;
                            else if (final_char == 'B') r += n;
                            else if (final_char == 'C') c += n;
                            else if (final_char == 'D') c -= n;
                            tui_set_cursor(r, c);
                        }
                    }
                    break;
                }

                if (isdigit((unsigned char)*p)) {
                    int val = 0;
                    while (isdigit((unsigned char)*p)) {
                        val = val * 10 + (*p - '0');
                        p++;
                    }
                    sgr_codes[sgr_count++] = val;
                } else {
                    /* Unknown character in CSI — skip the whole sequence */
                    while (*p != '\0' && *p != 'm' && *p != 'H' && *p != 'J' &&
                           *p != 'K' && *p != 'r' && *p != 's' && *p != 'u' &&
                           *p != 'h' && *p != 'l' && *p != 'A' && *p != 'B' &&
                           *p != 'C' && *p != 'D') {
                        p++;
                    }
                    if (*p != '\0') p++;  /* Skip final char */
                    sgr_count = 0;
                    break;
                }
            }

            segment_start = p;
        } else {
            p++;
        }
    }

    /* Flush final segment */
    if (p > segment_start) {
        segment_fn(segment_start, &state, user_data);
    }
}


