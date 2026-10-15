/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_str.c
 * @brief `str` verb — string operation suite.
 *
 * Text handling today means juggling substring syntax and `set` reassignments.
 * A dedicated verb makes string operations first-class batch operations.
 */

#include "batch.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR_VALUE_BYTES  P4_CONFIG_ENV_VALUE_BYTES

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/** Read a variable value or literal (if not a defined var, use as literal). */
static const char *str_read_var(const char *name)
{
    const char *val = shell_env_get(name);
    return val != NULL ? val : name;
}

/* ========================================================================
 * VERB IMPLEMENTATION
 * ======================================================================== */

void shell_command_str(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage(
            "Usage: str /len VAR\n"
            "       str /sub VAR START LEN DEST\n"
            "       str /find VAR NEEDLE [IDX_VAR]\n"
            "       str /upper VAR DEST\n"
            "       str /lower VAR DEST\n"
            "       str /trim VAR DEST\n"
            "       str /rep VAR OLD NEW DEST\n"
            "       str /split VAR DELIM DEST\n"
            "       str /join VAR DELIM DEST\n"
            "       str /fmt DEST \"format\" args...");
        batch_set_errorlevel(2);
        return;
    }

    if (argv[1][0] != '/' || argv[1][1] == '\0') {
        shell_print_error("str: expected a /switch");
        batch_set_errorlevel(2);
        return;
    }

    char flag = (char)toupper((unsigned char)argv[1][1]);

    /* /len VAR — length of variable value */
    if (flag == 'L' && argc >= 3) {
        const char *val = str_read_var(argv[2]);
        char numbuf[12];
        snprintf(numbuf, sizeof(numbuf), "%d", (int)strlen(val));
        /* Store in %LEN% convention */
        shell_transcript_appendf("%s\n", numbuf);
        batch_set_errorlevel(0);
        return;
    }

    /* /sub VAR START LEN DEST — substring */
    if (flag == 'S' && argc >= 6) {
        const char *val = str_read_var(argv[2]);
        int start = (int)strtol(argv[3], NULL, 10);
        int len = (int)strtol(argv[4], NULL, 10);
        size_t vlen = strlen(val);

        if (start < 0) start = (int)vlen + start; /* negative = from end */
        if (start < 0) start = 0;
        if (start > (int)vlen) start = (int)vlen;
        if (len < 0) len = (int)vlen - start; /* negative = drop from end */
        if (start + len > (int)vlen) len = (int)vlen - start;
        if (len < 0) len = 0;

        char *dest = malloc(STR_VALUE_BYTES);
        if (dest == NULL) {
            shell_print_error("str /sub: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        snprintf(dest, STR_VALUE_BYTES, "%.*s", len, val + start);
        shell_env_set(argv[5], dest);
        free(dest);
        batch_set_errorlevel(0);
        return;
    }

    /* /find VAR NEEDLE [IDX_VAR] — find needle in VAR */
    if (flag == 'F' && argc >= 4) {
        const char *val = str_read_var(argv[2]);
        const char *needle = argv[3];
        const char *pos = strstr(val, needle);
        char idx_buf[12];
        int idx = (pos != NULL) ? (int)(pos - val) : -1;
        snprintf(idx_buf, sizeof(idx_buf), "%d", idx);
        shell_transcript_appendf("%s\n", idx_buf);
        if (argc >= 5) {
            shell_env_set(argv[4], idx_buf);
        }
        batch_set_errorlevel(idx >= 0 ? 0 : 1);
        return;
    }

    /* /upper VAR DEST — uppercase */
    if (flag == 'U' && argc >= 4) {
        const char *val = str_read_var(argv[2]);
        size_t vlen = strlen(val);
        char *dest = malloc(STR_VALUE_BYTES);
        if (dest == NULL) {
            shell_print_error("str /upper: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        for (size_t i = 0; i <= vlen && i < STR_VALUE_BYTES; i++) {
            dest[i] = (char)toupper((unsigned char)val[i]);
        }
        shell_env_set(argv[3], dest);
        free(dest);
        batch_set_errorlevel(0);
        return;
    }

    /* /lower VAR DEST — lowercase */
    if (flag == 'W' && argc >= 4) {
        const char *val = str_read_var(argv[2]);
        size_t vlen = strlen(val);
        char *dest = malloc(STR_VALUE_BYTES);
        if (dest == NULL) {
            shell_print_error("str /lower: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        for (size_t i = 0; i <= vlen && i < STR_VALUE_BYTES; i++) {
            dest[i] = (char)tolower((unsigned char)val[i]);
        }
        shell_env_set(argv[3], dest);
        free(dest);
        batch_set_errorlevel(0);
        return;
    }

    /* /trim VAR DEST — strip leading/trailing whitespace */
    if (flag == 'T' && argc >= 4) {
        const char *val = str_read_var(argv[2]);
        const char *start = val;
        while (*start != '\0' && isspace((unsigned char)*start)) {
            start++;
        }
        const char *end = val + strlen(val);
        while (end > start && isspace((unsigned char)*(end - 1))) {
            end--;
        }
        size_t len = (size_t)(end - start);
        char *dest = malloc(STR_VALUE_BYTES);
        if (dest == NULL) {
            shell_print_error("str /trim: out of memory");
            batch_set_errorlevel(1);
            return;
        }
        snprintf(dest, STR_VALUE_BYTES, "%.*s", (int)len, start);
        shell_env_set(argv[3], dest);
        free(dest);
        batch_set_errorlevel(0);
        return;
    }

    /* /rep VAR OLD NEW DEST — replace all occurrences */
    if (flag == 'R' && argc >= 6) {
        const char *val = str_read_var(argv[2]);
        const char *old = argv[3];
        const char *new_s = argv[4];
        size_t old_len = strlen(old);

        char *dest = malloc(STR_VALUE_BYTES);
        if (dest == NULL) {
            shell_print_error("str /rep: out of memory");
            batch_set_errorlevel(1);
            return;
        }

        if (old_len == 0) {
            snprintf(dest, STR_VALUE_BYTES, "%s", val);
        } else {
            size_t dlen = 0;
            const char *p = val;
            const char *match;
            while ((match = strstr(p, old)) != NULL && dlen < STR_VALUE_BYTES - 1) {
                size_t chunk = (size_t)(match - p);
                if (dlen + chunk >= STR_VALUE_BYTES - 1) {
                    chunk = STR_VALUE_BYTES - 1 - dlen;
                }
                memcpy(dest + dlen, p, chunk);
                dlen += chunk;
                size_t new_len = strlen(new_s);
                if (dlen + new_len >= STR_VALUE_BYTES - 1) {
                    new_len = STR_VALUE_BYTES - 1 - dlen;
                }
                memcpy(dest + dlen, new_s, new_len);
                dlen += new_len;
                p = match + old_len;
            }
            size_t remainder = strlen(p);
            if (dlen + remainder >= STR_VALUE_BYTES - 1) {
                remainder = STR_VALUE_BYTES - 1 - dlen;
            }
            memcpy(dest + dlen, p, remainder);
            dlen += remainder;
            dest[dlen] = '\0';
        }
        shell_env_set(argv[5], dest);
        free(dest);
        batch_set_errorlevel(0);
        return;
    }

    /* /split VAR DELIM DEST — split VAR on DELIM, emit DEST[0..n-1] */
    if (flag == 'P' && argc >= 5) {
        const char *val = str_read_var(argv[2]);
        const char *delim = argv[3];
        const char *dest_name = argv[4];
        size_t dlen = strlen(delim);

        /* Clear any existing dest array */
        char len_name[P4_CONFIG_ENV_NAME_BYTES];
        snprintf(len_name, sizeof(len_name), "%s.__len", dest_name);
        shell_env_set(len_name, NULL);

        int count = 0;
        const char *p = val;
        const char *match;
        while ((match = strstr(p, delim)) != NULL || *p != '\0') {
            size_t chunk;
            if (match != NULL) {
                chunk = (size_t)(match - p);
            } else {
                chunk = strlen(p);
            }
            char elem[P4_CONFIG_ENV_NAME_BYTES];
            char val_buf[STR_VALUE_BYTES];
            snprintf(elem, sizeof(elem), "%s[%d]", dest_name, count);
            snprintf(val_buf, sizeof(val_buf), "%.*s", (int)chunk, p);
            shell_env_set(elem, val_buf);
            count++;
            if (match == NULL) break;
            p = match + dlen;
        }
        /* Set length */
        char len_buf[12];
        snprintf(len_buf, sizeof(len_buf), "%d", count);
        shell_env_set(len_name, len_buf);
        batch_set_errorlevel(0);
        return;
    }

    /* /join VAR DELIM DEST — join VAR[0..n-1] with DELIM */
    if (flag == 'J' && argc >= 5) {
        const char *delim = argv[3];
        const char *src_name = argv[2];
        char out[STR_VALUE_BYTES];
        out[0] = '\0';

        char len_name[P4_CONFIG_ENV_NAME_BYTES];
        snprintf(len_name, sizeof(len_name), "%s.__len", src_name);
        const char *len_val = shell_env_get(len_name);
        int count = (len_val != NULL) ? (int)strtol(len_val, NULL, 10) : 0;

        for (int i = 0; i < count; i++) {
            char elem[P4_CONFIG_ENV_NAME_BYTES];
            snprintf(elem, sizeof(elem), "%s[%d]", src_name, i);
            const char *val = shell_env_get(elem);
            if (i > 0) strlcat(out, delim, sizeof(out));
            if (val != NULL) strlcat(out, val, sizeof(out));
        }
        shell_env_set(argv[4], out);
        batch_set_errorlevel(0);
        return;
    }

    /* /fmt DEST \"format\" args... — printf-style formatting with %1..%9 */
    if (flag == 'M' && argc >= 4) {
        const char *format = argv[2];
        char out[P4_CONFIG_STR_FMT_BYTES];
        size_t olen = 0;

        for (size_t i = 0; format[i] != '\0' && olen < sizeof(out) - 1; i++) {
            if (format[i] == '%' && format[i + 1] != '\0') {
                if (format[i + 1] == '%') {
                    out[olen++] = '%';
                    i++;
                } else if (format[i + 1] >= '1' && format[i + 1] <= '9') {
                    int arg_idx = format[i + 1] - '1' + 3; /* %1 -> argv[3] */
                    i++;
                    if (arg_idx < argc) {
                        size_t alen = strlen(argv[arg_idx]);
                        if (olen + alen >= sizeof(out) - 1) {
                            alen = sizeof(out) - 1 - olen;
                        }
                        memcpy(out + olen, argv[arg_idx], alen);
                        olen += alen;
                    }
                } else {
                    out[olen++] = format[i];
                }
            } else {
                out[olen++] = format[i];
            }
        }
        out[olen] = '\0';
        shell_env_set(argv[3], out);
        batch_set_errorlevel(0);
        return;
    }

    shell_print_error("str: unknown switch /%s", argv[1] + 1);
    batch_set_errorlevel(2);
}
