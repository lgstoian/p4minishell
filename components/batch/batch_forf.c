/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_forf.c
 * @brief Pure `for /f` parsing: the option set, the line splitter and the
 *        quoted-command detector.
 *
 * These three helpers are string surgery only - no SD card, no batch frame,
 * no transcript. They live in their own translation unit so the hardware-free
 * host lane (test/host) can drive them directly through test/main/test_calc.c,
 * the same source the board test app runs. The execution half (reading lines,
 * binding tokens, running the body) stays in batch.c.
 *
 * @module components/batch
 * @layer batch
 * @domain core
 * @provides for /f option parser, line splitter, command-set detector
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule pure string parsing - no storage, display or board BSP here
 */

#include "batch.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ========================================================================
 * `for /f` — FILE-LINE LOOPS
 * ======================================================================== */

void shell_forf_options_default(shell_forf_options_t *opts)
{
    if (opts == NULL) {
        return;
    }
    snprintf(opts->delims, sizeof(opts->delims), " \t");
    opts->token_list[0] = 1;
    opts->token_count = 1;
    opts->skip = 0;
    /* cmd.exe parity: `;`-led lines are comments unless `eol=` overrides
     * (an explicit empty `eol=` still disables the filter). */
    opts->eol = ';';
    opts->star = false;
    opts->usebackq = false;
}

/** Parse a bounded decimal integer (digits only). */
static bool shell_forf_parse_num(const char *text, size_t len, int *out)
{
    int value = 0;

    if (len == 0 || len > 6) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    *out = value;
    return true;
}

/** Parse a `tokens=` list: `N`, `N-M`, and `*` items separated by commas. */
static bool shell_forf_parse_tokens(const char *value, size_t vlen, shell_forf_options_t *opts)
{
    size_t pos = 0;

    if (vlen == 0) {
        return true;   /* empty `tokens=` falls back to the default below */
    }

    while (pos < vlen) {
        size_t start = pos;

        while (pos < vlen && value[pos] != ',') {
            pos++;
        }
        {
            size_t ilen = pos - start;

            if (ilen == 0) {
                return false;   /* empty item such as `tokens=1,,2` */
            }
            if (ilen == 1 && value[start] == '*') {
                opts->star = true;
            } else {
                int lo;
                int hi;
                const char *dash = memchr(value + start, '-', ilen);

                if (dash != NULL) {
                    size_t lo_len = (size_t)(dash - (value + start));

                    if (!shell_forf_parse_num(value + start, lo_len, &lo) ||
                        !shell_forf_parse_num(dash + 1, ilen - lo_len - 1, &hi)) {
                        return false;
                    }
                } else {
                    if (!shell_forf_parse_num(value + start, ilen, &lo)) {
                        return false;
                    }
                    hi = lo;
                }
                if (lo < 1 || hi < lo) {
                    return false;
                }
                for (int i = lo; i <= hi; i++) {
                    if (opts->token_count >= P4_CONFIG_FORF_TOKEN_MAX) {
                        return false;   /* too many requested tokens */
                    }
                    opts->token_list[opts->token_count++] = i;
                }
            }
        }
        if (pos < vlen) {
            pos++;   /* consume ',' */
        }
    }
    return true;
}

bool shell_forf_parse_options(const char *text, size_t len, shell_forf_options_t *opts)
{
    size_t pos = 0;

    if (text == NULL || opts == NULL) {
        return false;
    }
    shell_forf_options_default(opts);

    while (pos < len) {
        size_t start;
        size_t wlen;
        const char *eq;
        size_t klen;
        const char *value;
        size_t vlen;

        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
        if (pos >= len) {
            break;
        }

        start = pos;
        while (pos < len && text[pos] != ' ' && text[pos] != '\t') {
            pos++;
        }
        wlen = pos - start;

        /* `usebackq` is accepted for DOS parity; the quoted-command source is
         * not supported on this shell, so it is a no-op. */
        if (wlen == 8 && strncasecmp(text + start, "usebackq", 8) == 0) {
            opts->usebackq = true;
            continue;
        }

        eq = memchr(text + start, '=', wlen);
        if (eq == NULL) {
            return false;   /* bare word that is not an option */
        }
        klen = (size_t)(eq - (text + start));
        value = eq + 1;
        vlen = wlen - klen - 1;

        if (klen == 6 && strncasecmp(text + start, "delims", 6) == 0) {
            size_t n = vlen < sizeof(opts->delims) - 1 ? vlen : sizeof(opts->delims) - 1;

            memcpy(opts->delims, value, n);
            opts->delims[n] = '\0';
        } else if (klen == 6 && strncasecmp(text + start, "tokens", 6) == 0) {
            /* A `tokens=` option replaces the default token 1 rather than
             * appending to it. */
            opts->token_count = 0;
            opts->star = false;
            if (!shell_forf_parse_tokens(value, vlen, opts)) {
                return false;
            }
        } else if (klen == 4 && strncasecmp(text + start, "skip", 4) == 0) {
            /* `skip=` with no value means skip nothing (TCMD writes it when the
             * pane cursor is at the top). Like the empty `eol=` above, it is
             * accepted rather than reported as a malformed option. */
            if (vlen == 0) {
                opts->skip = 0;
            } else if (!shell_forf_parse_num(value, vlen, &opts->skip)) {
                return false;
            }
        } else if (klen == 3 && strncasecmp(text + start, "eol", 3) == 0) {
            opts->eol = (vlen > 0) ? *value : '\0';
        } else {
            return false;   /* unknown option */
        }
    }

    /* `tokens=` with no usable entry (or no `tokens=` at all) defaults to 1. */
    if (opts->token_count == 0 && !opts->star) {
        opts->token_list[0] = 1;
        opts->token_count = 1;
    }
    return true;
}

int shell_forf_split_line(const char *line, const char *delims,
                          shell_forf_tok_t *tokens, int max_tokens)
{
    const char *p = line;
    int count = 0;

    if (line == NULL || tokens == NULL || max_tokens <= 0) {
        return 0;
    }

    /* An empty delims set means "no delimiters": the whole line is one token,
     * matching DOS `delims=` with no characters. */
    if (delims == NULL || delims[0] == '\0') {
        if (line[0] != '\0') {
            tokens[0].start = line;
            tokens[0].len = strlen(line);
            return 1;
        }
        return 0;
    }

    while (*p != '\0') {
        while (*p != '\0' && strchr(delims, *p) != NULL) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        {
            const char *start = p;

            while (*p != '\0' && strchr(delims, *p) == NULL) {
                p++;
            }
            if (count < max_tokens) {
                tokens[count].start = start;
                tokens[count].len = (size_t)(p - start);
                count++;
            }
        }
    }
    return count;
}

/**
 * Detect the `for /f ... in ('command')` command form (pure, no SD or
 * pipeline access). A single-quoted set is always the command form; with
 * @p usebackq a backquoted set is too. Surrounding whitespace is ignored.
 * On success the inner command text (without the quotes) is written to
 * @p inner_out.
 */
bool shell_forf_is_command_set(const char *set_str, bool usebackq,
                               char *inner_out, size_t inner_size)
{
    const char *s;
    size_t slen;
    char open;
    char close;
    size_t inner_len;

    if (inner_out == NULL || inner_size == 0) {
        return false;
    }
    inner_out[0] = '\0';
    if (set_str == NULL) {
        return false;
    }

    s = set_str;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    slen = strlen(s);
    while (slen > 0 && isspace((unsigned char)s[slen - 1])) {
        slen--;
    }
    if (slen < 2) {
        return false;
    }

    open = s[0];
    close = s[slen - 1];
    if (open == '\'' && close == '\'') {
        /* Single-quoted command: always the command form. */
    } else if (usebackq && open == '`' && close == '`') {
        /* usebackq backquote command form. */
    } else {
        return false;
    }

    inner_len = slen - 2;
    if (inner_len >= inner_size) {
        inner_len = inner_size - 1;
    }
    memcpy(inner_out, s + 1, inner_len);
    inner_out[inner_len] = '\0';
    return true;
}
