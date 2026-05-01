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

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/** ANSI 16-color palette. Indexed by ansi_color_index_t. */
static uint32_t s_palette[ANSI_COLOR_COUNT];

/** Default foreground and background colors. */
static uint32_t s_default_fg;
static uint32_t s_default_bg;

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
                /* Capture the full format specifier */
                size_t fmt_len = (size_t)(p - fmt_start) + 1;
                if (fmt_len < sizeof(temp)) {
                    memcpy(temp, fmt_start, fmt_len);
                    temp[fmt_len] = '\0';
                    p++;

                    /* Determine the type for va_arg */
                    char spec = temp[fmt_len - 1];
                    int written;
                    if (spec == 's') {
                        char *s = va_arg(args, char *);
                        written = snprintf(temp, sizeof(temp), "%s", s ? s : "(null)");
                    } else if (spec == 'd' || spec == 'i') {
                        int d = va_arg(args, int);
                        written = snprintf(temp, sizeof(temp), "%d", d);
                    } else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
                        unsigned int u = va_arg(args, unsigned int);
                        if (spec == 'u') written = snprintf(temp, sizeof(temp), "%u", u);
                        else if (spec == 'x') written = snprintf(temp, sizeof(temp), "%x", u);
                        else if (spec == 'X') written = snprintf(temp, sizeof(temp), "%X", u);
                        else written = snprintf(temp, sizeof(temp), "%o", u);
                    } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
                               spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A') {
                        double f = va_arg(args, double);
                        written = snprintf(temp, sizeof(temp), "%f", f);
                    } else if (spec == 'c') {
                        int c = va_arg(args, int);
                        written = snprintf(temp, sizeof(temp), "%c", c);
                    } else if (spec == 'p') {
                        void *ptr = va_arg(args, void *);
                        written = snprintf(temp, sizeof(temp), "%p", ptr);
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
