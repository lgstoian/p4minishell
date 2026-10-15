/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_dim.c
 * @brief `dim` verb — bounds-checked array dimensioning with C pointer-like
 *        semantics.
 *
 * Indexed variables (SPR[0]) already work, but they are unmanaged: a typo
 * silently creates a stray slot, and there is no length. `dim` gives arrays
 * an explicit size, hidden bookkeeping slots, bounds checks, and pointer-like
 * reference operations.
 *
 * Storage lives entirely in the ARRAY class slot slab: element N is stored as
 * NAME[N] (the bracket form the expansion engine already resolves). Bookkeeping
 * slots:
 *   NAME.__len      element count (1D) or total elements (2D)
 *   NAME.__rows     row count (2D only, 0 or absent = 1D)
 *   NAME.__cols     column count (2D only)
 *   NAME.__ref      pointer: array name this variable references
 *
 * A read past NAME.__len becomes a trapped error instead of an empty string.
 *
 * Pointer semantics (C-like):
 *   dim /ref PTR ARR        make PTR reference ARR (PTR.__ref = ARR)
 *   dim /get PTR IDX VAL    VAL = ARR[IDX]  (resolves PTR.__ref)
 *   dim /set PTR IDX VAL    ARR[IDX] = VAL  (resolves PTR.__ref)
 *   dim /get PTR R C VAL    VAL = ARR[R][C] (2D, resolves PTR.__ref)
 *   dim /set PTR R C VAL    ARR[R][C] = VAL (2D, resolves PTR.__ref)
 */

#include "batch.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM_NAME_BYTES        P4_CONFIG_ENV_NAME_BYTES
#define DIM_VALUE_BYTES       P4_CONFIG_ENV_VALUE_BYTES

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/** Build a bookkeeping slot name: "NAME.__tag". */
static void dim_book_name(const char *name, const char *tag, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s.__%s", name, tag);
}

/** Read the length bookkeeping slot; returns -1 when not dimensioned. */
static int dim_get_length(const char *name)
{
    char bk[DIM_NAME_BYTES];
    dim_book_name(name, "len", bk, sizeof(bk));
    const char *val = shell_env_get(bk);
    if (val == NULL) {
        return -1;
    }
    return (int)strtol(val, NULL, 10);
}

/** Set the length bookkeeping slot. */
static esp_err_t dim_set_length(const char *name, int length)
{
    char bk[DIM_NAME_BYTES], numbuf[12];
    dim_book_name(name, "len", bk, sizeof(bk));
    snprintf(numbuf, sizeof(numbuf), "%d", length);
    return shell_env_set(bk, numbuf);
}

/** Read rows (2D); 0 means 1D. */
static int dim_get_rows(const char *name)
{
    char bk[DIM_NAME_BYTES];
    dim_book_name(name, "rows", bk, sizeof(bk));
    const char *val = shell_env_get(bk);
    if (val == NULL) return 0;
    return (int)strtol(val, NULL, 10);
}

/** Read cols (2D); 0 means 1D. */
static int dim_get_cols(const char *name)
{
    char bk[DIM_NAME_BYTES];
    dim_book_name(name, "cols", bk, sizeof(bk));
    const char *val = shell_env_get(bk);
    if (val == NULL) return 0;
    return (int)strtol(val, NULL, 10);
}

/** Set rows/cols bookkeeping. */
static void dim_set_dims(const char *name, int rows, int cols)
{
    char bk[DIM_NAME_BYTES], numbuf[12];
    if (rows > 0 && cols > 0) {
        dim_book_name(name, "rows", bk, sizeof(bk));
        snprintf(numbuf, sizeof(numbuf), "%d", rows);
        shell_env_set(bk, numbuf);
        dim_book_name(name, "cols", bk, sizeof(bk));
        snprintf(numbuf, sizeof(numbuf), "%d", cols);
        shell_env_set(bk, numbuf);
    } else {
        dim_book_name(name, "rows", bk, sizeof(bk));
        shell_env_set(bk, NULL);
        dim_book_name(name, "cols", bk, sizeof(bk));
        shell_env_set(bk, NULL);
    }
}

/** Resolve a pointer variable to its target array name. Returns NULL if not
 *  a pointer. */
static const char *dim_resolve_ref(const char *name)
{
    char bk[DIM_NAME_BYTES];
    dim_book_name(name, "ref", bk, sizeof(bk));
    return shell_env_get(bk);
}

/** Clear a NAME[i] slot (passing NULL value clears it). */
static void dim_clear_element(const char *name, int idx)
{
    char key[DIM_NAME_BYTES];
    snprintf(key, sizeof(key), "%s[%d]", name, idx);
    shell_env_set(key, NULL);
}

/** Read a NAME[i] slot value (NULL when absent). */
static const char *dim_get_element(const char *name, int idx)
{
    char key[DIM_NAME_BYTES];
    snprintf(key, sizeof(key), "%s[%d]", name, idx);
    return shell_env_get(key);
}

/** Write a NAME[i] slot. */
static esp_err_t dim_set_element(const char *name, int idx, const char *value)
{
    char key[DIM_NAME_BYTES];
    snprintf(key, sizeof(key), "%s[%d]", name, idx);
    return shell_env_set(key, value);
}

/** Clear all bookkeeping slots for an array. */
static void dim_clear_bookkeeping(const char *name)
{
    char bk[DIM_NAME_BYTES];
    dim_book_name(name, "len", bk, sizeof(bk));
    shell_env_set(bk, NULL);
    dim_book_name(name, "rows", bk, sizeof(bk));
    shell_env_set(bk, NULL);
    dim_book_name(name, "cols", bk, sizeof(bk));
    shell_env_set(bk, NULL);
    dim_book_name(name, "ref", bk, sizeof(bk));
    shell_env_set(bk, NULL);
}

/** Get the target array name: if name is a pointer, resolve it; otherwise
 *  return name itself. */
static const char *dim_target_name(const char *name)
{
    const char *ref = dim_resolve_ref(name);
    return (ref != NULL && ref[0] != '\0') ? ref : name;
}

/* ========================================================================
 * VERB IMPLEMENTATION
 * ======================================================================== */

void shell_command_dim(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage(
            "Usage: dim NAME SIZE [DEFAULT]\n"
            "       dim NAME ROWS COLS [DEFAULT]\n"
            "       dim /len NAME\n"
            "       dim /info NAME\n"
            "       dim /push NAME VALUE\n"
            "       dim /pop NAME [TARGET]\n"
            "       dim /shift NAME [TARGET]\n"
            "       dim /unshift NAME VALUE\n"
            "       dim /ins NAME INDEX VALUE\n"
            "       dim /rem NAME INDEX\n"
            "       dim /slice NAME START COUNT DEST\n"
            "       dim /copy SRC DST\n"
            "       dim /fill NAME VALUE [START COUNT]\n"
            "       dim /reverse NAME\n"
            "       dim /sort NAME [/n] [/d]\n"
            "       dim /find NAME VALUE [IDX_VAR]\n"
            "       dim /sum NAME DEST\n"
            "       dim /avg NAME DEST\n"
            "       dim /min NAME DEST\n"
            "       dim /max NAME DEST\n"
            "       dim /cmp ARR1 ARR2\n"
            "       dim /ref PTR ARR\n"
            "       dim /get PTR IDX [SUB] DEST\n"
            "       dim /set PTR IDX [SUB] VALUE\n"
            "       dim /del NAME");
        batch_set_errorlevel(2);
        return;
    }

    /* Variables used by both switch and plain forms */
    const char *name, *dest, *src, *val;
    int len, idx, start, count, rows, cols;
    esp_err_t err;

    /* Switch forms */
    if (argv[1][0] == '/' && argv[1][1] != '\0') {
        char flag = (char)toupper((unsigned char)argv[1][1]);

        /* /len — report current length */
        if (flag == 'L' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_warning("%s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            shell_transcript_appendf("%d\n", len);
            batch_set_errorlevel(0);
            return;
        }

        /* /info — report array metadata */
        if (flag == 'I' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_warning("%s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            rows = dim_get_rows(name);
            cols = dim_get_cols(name);
            if (rows > 0 && cols > 0) {
                shell_transcript_appendf("%s: %dx%d (%d elements)\n", name, rows, cols, len);
            } else {
                shell_transcript_appendf("%s: [%d]\n", name, len);
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /push NAME VALUE — append at first free slot (auto-grows) */
        if (flag == 'P' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) len = 0;
            if (len >= P4_CONFIG_DIM_MAX_SIZE) {
                shell_print_error("dim /push: %s is full (max %d)", name, P4_CONFIG_DIM_MAX_SIZE);
                batch_set_errorlevel(1);
                return;
            }
            dim_set_element(name, len, argv[3]);
            dim_set_length(name, len + 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /pop NAME [TARGET] — remove last, optionally store in TARGET */
        if (flag == 'U' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /pop: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            if (argc >= 4) {
                val = dim_get_element(name, len - 1);
                shell_env_set(argv[3], val != NULL ? val : "");
            }
            dim_clear_element(name, len - 1);
            dim_set_length(name, len - 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /shift NAME [TARGET] — remove first element, shift left */
        if (flag == 'H' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /shift: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            if (argc >= 4) {
                val = dim_get_element(name, 0);
                shell_env_set(argv[3], val != NULL ? val : "");
            }
            for (int i = 0; i < len - 1; i++) {
                val = dim_get_element(name, i + 1);
                dim_set_element(name, i, val != NULL ? val : "");
            }
            dim_clear_element(name, len - 1);
            dim_set_length(name, len - 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /unshift NAME VALUE — insert at beginning, shift right */
        if (flag == 'N' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) len = 0;
            if (len >= P4_CONFIG_DIM_MAX_SIZE) {
                shell_print_error("dim /unshift: %s is full", name);
                batch_set_errorlevel(1);
                return;
            }
            for (int i = len; i > 0; i--) {
                val = dim_get_element(name, i - 1);
                dim_set_element(name, i, val != NULL ? val : "");
            }
            dim_set_element(name, 0, argv[3]);
            dim_set_length(name, len + 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /ins NAME INDEX VALUE — insert at index, shift right */
        if (flag == 'E' && argc >= 5) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_error("dim /ins: %s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            idx = (int)strtol(argv[3], NULL, 10);
            if (idx < 0 || idx > len) {
                shell_print_error("dim /ins: index %d out of range [0..%d]", idx, len);
                batch_set_errorlevel(1);
                return;
            }
            for (int i = len; i > idx; i--) {
                val = dim_get_element(name, i - 1);
                dim_set_element(name, i, val != NULL ? val : "");
            }
            dim_set_element(name, idx, argv[4]);
            dim_set_length(name, len + 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /rem NAME INDEX — remove at index, shift left */
        if (flag == 'R' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /rem: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            idx = (int)strtol(argv[3], NULL, 10);
            if (idx < 0 || idx >= len) {
                shell_print_error("dim /rem: index %d out of range [0..%d]", idx, len - 1);
                batch_set_errorlevel(1);
                return;
            }
            for (int i = idx; i < len - 1; i++) {
                val = dim_get_element(name, i + 1);
                dim_set_element(name, i, val != NULL ? val : "");
            }
            dim_clear_element(name, len - 1);
            dim_set_length(name, len - 1);
            batch_set_errorlevel(0);
            return;
        }

        /* /slice NAME START COUNT DEST — copy elements into DEST array */
        if (flag == 'S' && argc >= 6) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_error("dim /slice: %s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            start = (int)strtol(argv[3], NULL, 10);
            count = (int)strtol(argv[4], NULL, 10);
            if (start < 0 || count < 0 || start + count > len) {
                shell_print_error("dim /slice: range [%d..%d) exceeds length %d", start, start + count, len);
                batch_set_errorlevel(1);
                return;
            }
            dest = argv[5];
            dim_clear_bookkeeping(dest);
            for (int i = 0; i < count; i++) {
                val = dim_get_element(name, start + i);
                dim_set_element(dest, i, val != NULL ? val : "");
            }
            dim_set_length(dest, count);
            batch_set_errorlevel(0);
            return;
        }

        /* /copy SRC DST — duplicate an array */
        if (flag == 'C' && argc >= 4) {
            src = argv[2];
            len = dim_get_length(src);
            if (len < 0) {
                shell_print_error("dim /copy: %s is not dimensioned", src);
                batch_set_errorlevel(1);
                return;
            }
            dest = argv[3];
            dim_clear_bookkeeping(dest);
            rows = dim_get_rows(src);
            cols = dim_get_cols(src);
            for (int i = 0; i < len; i++) {
                val = dim_get_element(src, i);
                dim_set_element(dest, i, val != NULL ? val : "");
            }
            dim_set_length(dest, len);
            if (rows > 0 && cols > 0) {
                dim_set_dims(dest, rows, cols);
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /fill NAME VALUE [START COUNT] — set elements to value */
        if (flag == 'F' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_error("dim /fill: %s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            start = 0;
            count = len;
            if (argc >= 5) {
                start = (int)strtol(argv[3], NULL, 10);
                count = (int)strtol(argv[4], NULL, 10);
                val = argv[5];
            } else {
                val = argv[3];
            }
            if (start < 0 || count < 0 || start + count > len) {
                shell_print_error("dim /fill: range [%d..%d) exceeds length %d", start, start + count, len);
                batch_set_errorlevel(1);
                return;
            }
            for (int i = start; i < start + count; i++) {
                dim_set_element(name, i, val);
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /reverse NAME — reverse element order in place */
        if (flag == 'V' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 1) {
                batch_set_errorlevel(0);
                return;
            }
            for (int i = 0; i < len / 2; i++) {
                const char *tmp_i = dim_get_element(name, i);
                const char *tmp_j = dim_get_element(name, len - 1 - i);
                dim_set_element(name, i, tmp_j != NULL ? tmp_j : "");
                dim_set_element(name, len - 1 - i, tmp_i != NULL ? tmp_i : "");
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /sort NAME [/n] [/d] — sort in place */
        if (flag == 'O' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_error("dim /sort: %s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            bool numeric = false;
            bool descending = false;
            for (int i = 3; i < argc; i++) {
                if (argv[i][0] == '/') {
                    char sf = (char)toupper((unsigned char)argv[i][1]);
                    if (sf == 'N') numeric = true;
                    else if (sf == 'D') descending = true;
                }
            }
            int sorted = shell_sort_array(name, numeric, descending);
            if (sorted < 0) {
                shell_print_error("dim /sort: sort failed");
                batch_set_errorlevel(1);
                return;
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /find NAME VALUE [IDX_VAR] — find value, return index or -1 */
        if (flag == 'A' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                if (argc >= 5) shell_env_set(argv[4], "-1");
                batch_set_errorlevel(1);
                return;
            }
            for (int i = 0; i < len; i++) {
                val = dim_get_element(name, i);
                if (val != NULL && strcmp(val, argv[3]) == 0) {
                    if (argc >= 5) {
                        char idx_buf[12];
                        snprintf(idx_buf, sizeof(idx_buf), "%d", i);
                        shell_env_set(argv[4], idx_buf);
                    }
                    batch_set_errorlevel(0);
                    return;
                }
            }
            if (argc >= 5) shell_env_set(argv[4], "-1");
            batch_set_errorlevel(1);
            return;
        }

        /* /sum NAME DEST — sum of numeric elements */
        if (flag == 'M' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len < 0) {
                shell_print_error("dim /sum: %s is not dimensioned", name);
                batch_set_errorlevel(1);
                return;
            }
            long total = 0;
            for (int i = 0; i < len; i++) {
                val = dim_get_element(name, i);
                if (val != NULL) total += strtol(val, NULL, 10);
            }
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%ld", total);
            shell_env_set(argv[3], numbuf);
            batch_set_errorlevel(0);
            return;
        }

        /* /avg NAME DEST — average of numeric elements */
        if (flag == 'G' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /avg: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            long total = 0;
            for (int i = 0; i < len; i++) {
                val = dim_get_element(name, i);
                if (val != NULL) total += strtol(val, NULL, 10);
            }
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%ld", total / len);
            shell_env_set(argv[3], numbuf);
            batch_set_errorlevel(0);
            return;
        }

        /* /min NAME DEST — minimum numeric element */
        if (flag == 'X' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /min: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            long best = 0;
            bool first = true;
            for (int i = 0; i < len; i++) {
                val = dim_get_element(name, i);
                if (val != NULL) {
                    long v = strtol(val, NULL, 10);
                    if (first || v < best) { best = v; first = false; }
                }
            }
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%ld", best);
            shell_env_set(argv[3], numbuf);
            batch_set_errorlevel(0);
            return;
        }

        /* /max NAME DEST — maximum numeric element */
        if (flag == 'W' && argc >= 4) {
            name = argv[2];
            len = dim_get_length(name);
            if (len <= 0) {
                shell_print_error("dim /max: %s is empty", name);
                batch_set_errorlevel(1);
                return;
            }
            long best = 0;
            bool first = true;
            for (int i = 0; i < len; i++) {
                val = dim_get_element(name, i);
                if (val != NULL) {
                    long v = strtol(val, NULL, 10);
                    if (first || v > best) { best = v; first = false; }
                }
            }
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%ld", best);
            shell_env_set(argv[3], numbuf);
            batch_set_errorlevel(0);
            return;
        }

        /* /cmp ARR1 ARR2 — compare arrays element by element */
        if (flag == 'K' && argc >= 4) {
            const char *a1 = argv[2];
            const char *a2 = argv[3];
            int l1 = dim_get_length(a1);
            int l2 = dim_get_length(a2);
            if (l1 != l2) {
                batch_set_errorlevel(1);
                return;
            }
            for (int i = 0; i < l1; i++) {
                const char *v1 = dim_get_element(a1, i);
                const char *v2 = dim_get_element(a2, i);
                if (strcmp(v1 != NULL ? v1 : "", v2 != NULL ? v2 : "") != 0) {
                    batch_set_errorlevel(1);
                    return;
                }
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /ref PTR ARR — make PTR a reference to ARR */
        if (flag == 'E' && argc >= 4) {
            const char *ptr = argv[2];
            const char *arr = argv[3];
            int arr_len = dim_get_length(arr);
            if (arr_len < 0) {
                shell_print_error("dim /ref: %s is not dimensioned", arr);
                batch_set_errorlevel(1);
                return;
            }
            char bk[DIM_NAME_BYTES];
            dim_book_name(ptr, "ref", bk, sizeof(bk));
            shell_env_set(bk, arr);
            batch_set_errorlevel(0);
            return;
        }

        /* /get PTR IDX [SUB] DEST — read through pointer (1D or 2D) */
        if (flag == 'T' && argc >= 5) {
            const char *ptr = argv[2];
            const char *target = dim_target_name(ptr);
            len = dim_get_length(target);
            if (len < 0) {
                shell_print_error("dim /get: %s does not reference an array", ptr);
                batch_set_errorlevel(1);
                return;
            }
            rows = dim_get_rows(target);
            cols = dim_get_cols(target);
            idx = (int)strtol(argv[3], NULL, 10);
            int sub = -1;
            if (argc >= 6) {
                sub = (int)strtol(argv[4], NULL, 10);
                dest = argv[5];
            } else {
                dest = argv[4];
            }
            int linear;
            if (rows > 0 && cols > 0 && sub >= 0) {
                /* 2D access: PTR ROW COL DEST */
                if (idx >= rows || sub >= cols) {
                    shell_print_error("dim /get: [%d][%d] out of range [%d][%d]", idx, sub, rows, cols);
                    batch_set_errorlevel(1);
                    return;
                }
                linear = idx * cols + sub;
            } else {
                if (idx < 0 || idx >= len) {
                    shell_print_error("dim /get: index %d out of range [0..%d]", idx, len - 1);
                    batch_set_errorlevel(1);
                    return;
                }
                linear = idx;
            }
            val = dim_get_element(target, linear);
            shell_env_set(dest, val != NULL ? val : "");
            batch_set_errorlevel(0);
            return;
        }

        /* /set PTR IDX [SUB] VALUE — write through pointer (1D or 2D) */
        if (flag == 'Z' && argc >= 5) {
            const char *ptr = argv[2];
            const char *target = dim_target_name(ptr);
            len = dim_get_length(target);
            if (len < 0) {
                shell_print_error("dim /set: %s does not reference an array", ptr);
                batch_set_errorlevel(1);
                return;
            }
            rows = dim_get_rows(target);
            cols = dim_get_cols(target);
            idx = (int)strtol(argv[3], NULL, 10);
            int sub = -1;
            if (argc >= 6) {
                sub = (int)strtol(argv[4], NULL, 10);
                val = argv[5];
            } else {
                val = argv[4];
            }
            int linear;
            if (rows > 0 && cols > 0 && sub >= 0) {
                if (idx >= rows || sub >= cols) {
                    shell_print_error("dim /set: [%d][%d] out of range [%d][%d]", idx, sub, rows, cols);
                    batch_set_errorlevel(1);
                    return;
                }
                linear = idx * cols + sub;
            } else {
                if (idx < 0 || idx >= len) {
                    shell_print_error("dim /set: index %d out of range [0..%d]", idx, len - 1);
                    batch_set_errorlevel(1);
                    return;
                }
                linear = idx;
            }
            err = dim_set_element(target, linear, val);
            if (err != ESP_OK) {
                shell_print_error("dim /set: %s", err == ESP_ERR_NO_MEM ? "arrays budget exhausted" : "store failed");
                batch_set_errorlevel(1);
                return;
            }
            batch_set_errorlevel(0);
            return;
        }

        /* /del NAME — free all slots */
        if (flag == 'D' && argc >= 3) {
            name = argv[2];
            len = dim_get_length(name);
            if (len >= 0) {
                for (int i = 0; i < len; i++) {
                    dim_clear_element(name, i);
                }
            }
            dim_clear_bookkeeping(name);
            batch_set_errorlevel(0);
            return;
        }

        shell_print_error("dim: unknown switch /%s", argv[1] + 1);
        batch_set_errorlevel(2);
        return;
    }

    /* Plain form: dim NAME SIZE [DEFAULT] or dim NAME ROWS COLS [DEFAULT] */
    if (argc < 3) {
        shell_print_error("dim: needs a size");
        batch_set_errorlevel(2);
        return;
    }

    name = argv[1];
    rows = 0;
    cols = 0;

    if (argc >= 5) {
        /* dim NAME ROWS COLS DEFAULT — 2D array */
        rows = (int)strtol(argv[2], NULL, 10);
        cols = (int)strtol(argv[3], NULL, 10);
        dest = argv[4];
        if (rows <= 0 || cols <= 0) {
            shell_print_error("dim: dimensions must be positive");
            batch_set_errorlevel(1);
            return;
        }
        if (rows * cols > P4_CONFIG_DIM_MAX_SIZE) {
            shell_print_error("dim: %dx%d = %d exceeds max %d", rows, cols, rows * cols, P4_CONFIG_DIM_MAX_SIZE);
            batch_set_errorlevel(1);
            return;
        }
        len = rows * cols;
        for (int i = 0; i < len; i++) {
            dim_set_element(name, i, dest);
        }
        dim_set_length(name, len);
        dim_set_dims(name, rows, cols);
        shell_print_ok("dim %s[%dx%d] = \"%s\"", name, rows, cols, dest);
    } else {
        /* dim NAME SIZE [DEFAULT] — 1D array */
        len = (int)strtol(argv[2], NULL, 10);
        dest = (argc >= 4) ? argv[3] : "";
        if (len < 0 || len > P4_CONFIG_DIM_MAX_SIZE) {
            shell_print_error("dim: size %d out of range [0..%d]", len, P4_CONFIG_DIM_MAX_SIZE);
            batch_set_errorlevel(1);
            return;
        }
        for (int i = 0; i < len; i++) {
            dim_set_element(name, i, dest);
        }
        dim_set_length(name, len);
        dim_set_dims(name, 0, 0);
        shell_print_ok("dim %s[%d] = \"%s\"", name, len, dest);
    }
    batch_set_errorlevel(0);
}
