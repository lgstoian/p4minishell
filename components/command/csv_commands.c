/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file csv_commands.c
 * @brief `csv` grid verbs: rows/cols/cell over guarded SD plus =EXPR eval.
 *
 * Minimal spreadsheet substrate for batch apps. Parsing is the pure
 * `csv_split_line()` core in components/storage (unit-tested, no I/O);
 * this file owns the verbs, so it may use the `calc` evaluator and the
 * environment table (the command layer already depends on batch).
 *
 * File input follows the text-utility contract: an explicit file wins,
 * otherwise the active `< file` / pipe source is used
 * (`storage_resolve_input_source`). Every verb returns an ERRORLEVEL
 * (0 ok, 1 none/error, 2 usage) and honours `/b` bare output for pipes.
 */

#include "command.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "batch.h"
#include "calc.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef P4_CONFIG_CSV_MAX_COLS
#define P4_CONFIG_CSV_MAX_COLS 32
#endif

#ifndef P4_CONFIG_CSV_FIELD_BYTES
#define P4_CONFIG_CSV_FIELD_BYTES 256
#endif

#ifndef P4_CONFIG_CSV_ROWS_MAX
#define P4_CONFIG_CSV_ROWS_MAX 256
#endif

#ifndef P4_CONFIG_CSV_PASSES
#define P4_CONFIG_CSV_PASSES 8
#endif

static void shell_command_csv_usage(void)
{
    shell_print_usage("Usage: csv rows|cols|cell|get|eval <file> [row col] [/b] [/v:NAME]");
    shell_print_usage("Usage: csv set <file> <row> <col> <value...>");
}

/** Options accepted anywhere on the `csv` line. */
typedef struct {
    bool bare;
    char var[64];
    bool has_var;
} csv_opts_t;

/** Split subcommand/file/indices/options out of argv.
 *  @return 0 on success, 2 on a usage error (already reported). */
static int csv_parse_args(int argc, char **argv, const char **sub_out,
                          const char **file_out, long *row_out, long *col_out,
                          csv_opts_t *opts_out, int *numbers_seen_out)
{
    const char *sub = NULL;
    const char *file = NULL;
    long numbers[2] = {0, 0};
    int numbers_seen = 0;
    int i;

    memset(opts_out, 0, sizeof(*opts_out));
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        char *end = NULL;
        long value;

        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (arg[0] == '/') {
            if (strcasecmp(arg, "/b") == 0) {
                opts_out->bare = true;
            } else if (strncasecmp(arg, "/v:", 3) == 0 && strlen(arg) > 3 &&
                       strlen(arg) < sizeof(opts_out->var)) {
                snprintf(opts_out->var, sizeof(opts_out->var), "%s", arg + 3);
                opts_out->has_var = true;
            } else {
                shell_command_csv_usage();
                return 2;
            }
        } else if (sub == NULL) {
            sub = arg;
        } else if (file == NULL && (arg[0] < '0' || arg[0] > '9')) {
            file = arg;
        } else if (numbers_seen < 2) {
            value = strtol(arg, &end, 10);
            if (end == arg || *end != '\0' || value < 1 || value > 1000000) {
                shell_command_csv_usage();
                return 2;
            }
            numbers[numbers_seen++] = value;
        } else {
            shell_command_csv_usage();
            return 2;
        }
    }
    /* A digit-leading file name is ambiguous with a row index; prefer the
     * file when it exists on the card is overkill here, so document the
     * rule instead: the first non-option token after the subcommand is the
     * file unless it parses as a bare number and no file was seen yet and
     * a later token looks like a path. Keep it simple: numbers are indices,
     * the file is the first non-numeric token. */
    if (sub == NULL) {
        shell_command_csv_usage();
        return 2;
    }
    *sub_out = sub;
    *file_out = file;
    *row_out = numbers[0];
    *col_out = numbers[1];
    *numbers_seen_out = numbers_seen;
    return 0;
}

/** Heap helper: PSRAM first (bulk CSV buffers must stay out of the scarce
 *  internal DMA heap), plain malloc fallback. Freed with heap_caps_free. */
static void *csv_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(size);
    }
    return p;
}

/** Format one field with RFC-4180 quoting (`"a""b"`), for files and pipes.
 *  Pure, unit-tested, shared with the `export` command (single quoting
 *  implementation). With @p out NULL (or @p out_size 0) returns the bytes
 *  needed including the terminator. */
size_t csv_format_field(const char *text, char *out, size_t out_size)
{
    bool needs_quotes;
    size_t need = 1;
    const char *p;

    if (text == NULL) {
        text = "";
    }
    needs_quotes = (strchr(text, ',') != NULL) || (strchr(text, '"') != NULL) ||
                   (strchr(text, '\n') != NULL) || (strchr(text, '\r') != NULL);
    for (p = text; *p != '\0'; p++) {
        need++;
        if (*p == '"') {
            need++;
        }
    }
    if (needs_quotes) {
        need += 2;
    }
    if (out == NULL || out_size == 0) {
        return need;
    }
    {
        size_t used = 0;
        if (needs_quotes) {
            if (used + 1 >= out_size) {
                out[0] = '\0';
                return need;
            }
            out[used++] = '"';
        }
        for (p = text; *p != '\0'; p++) {
            if (*p == '"') {
                if (used + 2 >= out_size) {
                    break;
                }
                out[used++] = '"';
                out[used++] = '"';
            } else {
                if (used + 1 >= out_size) {
                    break;
                }
                out[used++] = *p;
            }
        }
        if (needs_quotes && used + 1 < out_size) {
            out[used++] = '"';
        }
        out[(used < out_size) ? used : out_size - 1] = '\0';
    }
    return need;
}

/** Open @p file_arg (or the redirected source) guarded on the SD card.
 *  @return FILE on success (caller closes + ends the session), NULL on
 *          failure after reporting. @p resolved_out holds the live path. */
static FILE *csv_open_source(const char *file_arg, char *resolved_out, size_t resolved_size,
                             shell_sd_session_t *session_out, const char *what)
{
    esp_err_t error;
    FILE *file;

    error = storage_resolve_input_source(file_arg, resolved_out, resolved_size);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_transcript_append_text("csv: no input file given and no input redirection active\n");
        shell_command_csv_usage();
        return NULL;
    }
    if (error != ESP_OK) {
        shell_print_error("csv: invalid path");
        return NULL;
    }
    if (shell_sd_begin(session_out) != ESP_OK) {
        shell_print_error("csv: SD card not present - insert and retry");
        return NULL;
    }
    file = fopen(resolved_out, "r");
    if (file == NULL) {
        shell_print_error("csv: cannot open %s (%s)", resolved_out, strerror(errno));
        shell_sd_end(session_out, what);
        return NULL;
    }
    return file;
}

/** Store @p value into the `/v:NAME` variable. */
static int csv_store_var(const csv_opts_t *opts, const char *value)
{
    if (!opts->has_var) {
        return 0;
    }
    if (shell_env_set(opts->var, value) != ESP_OK) {
        shell_print_error("csv: cannot store '%s' (bad name or table full)", opts->var);
        return 1;
    }
    return 0;
}

/** Read the next non-blank line. @return false at EOF. */
static bool csv_next_row(FILE *file, char *line, size_t size)
{
    while (fgets(line, (int)size, file) != NULL) {
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p != '\0') {
            return true;
        }
    }
    return false;
}

static int csv_cmd_rows(const char *file_arg, const csv_opts_t *opts)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *line;
    long rows = 0;
    int rc = 0;

    line = csv_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    if (line == NULL) {
        shell_print_error("csv: out of memory");
        return 1;
    }
    file = csv_open_source(file_arg, resolved, sizeof(resolved), &session, "csv");
    if (file == NULL) {
        heap_caps_free(line);
        return 2;
    }
    while (csv_next_row(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        rows++;
    }
    fclose(file);
    shell_sd_end(&session, "csv");
    if (!opts->bare) {
        shell_print_field_num("rows:", rows);
    } else {
        shell_transcript_appendf("%ld\n", rows);
    }
    if (rows == 0) {
        rc = 1;
    } else if (opts->has_var) {
        char value[32];
        snprintf(value, sizeof(value), "%ld", rows);
        rc = csv_store_var(opts, value);
    }
    heap_caps_free(line);
    return rc;
}

static int csv_cmd_cols(const char *file_arg, const csv_opts_t *opts)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *line;
    char *scratch;
    csv_field_t *fields;
    long cols = 0;
    int rc = 0;

    line = csv_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    scratch = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES);
    fields = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * sizeof(csv_field_t));
    if (line == NULL || scratch == NULL || fields == NULL) {
        shell_print_error("csv: out of memory");
        heap_caps_free(line);
        heap_caps_free(scratch);
        heap_caps_free(fields);
        return 1;
    }
    file = csv_open_source(file_arg, resolved, sizeof(resolved), &session, "csv");
    if (file == NULL) {
        heap_caps_free(line);
        heap_caps_free(scratch);
        heap_caps_free(fields);
        return 2;
    }
    if (csv_next_row(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        cols = csv_split_line(line, scratch,
                              (size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES,
                              fields, P4_CONFIG_CSV_MAX_COLS);
    }
    fclose(file);
    shell_sd_end(&session, "csv");
    if (!opts->bare) {
        shell_print_field_num("cols:", cols);
    } else {
        shell_transcript_appendf("%ld\n", cols);
    }
    if (cols == 0) {
        rc = 1;
    } else if (opts->has_var) {
        char value[32];
        snprintf(value, sizeof(value), "%ld", cols);
        rc = csv_store_var(opts, value);
    }
    heap_caps_free(line);
    heap_caps_free(scratch);
    heap_caps_free(fields);
    return rc;
}

static int csv_cmd_cell(const char *file_arg, long row, long col, int numbers_seen,
                        const csv_opts_t *opts)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *line;
    char *scratch;
    csv_field_t *fields;
    long current = 0;
    int found = 0;
    int rc;

    if (numbers_seen < 2) {
        shell_command_csv_usage();
        return 2;
    }
    if (row > P4_CONFIG_CSV_ROWS_MAX || col > P4_CONFIG_CSV_MAX_COLS) {
        shell_print_error("csv: cell out of range (max %d rows x %d cols)",
                          P4_CONFIG_CSV_ROWS_MAX, P4_CONFIG_CSV_MAX_COLS);
        return 1;
    }
    line = csv_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    scratch = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES);
    fields = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * sizeof(csv_field_t));
    if (line == NULL || scratch == NULL || fields == NULL) {
        shell_print_error("csv: out of memory");
        heap_caps_free(line);
        heap_caps_free(scratch);
        heap_caps_free(fields);
        return 1;
    }
    file = csv_open_source(file_arg, resolved, sizeof(resolved), &session, "csv");
    if (file == NULL) {
        heap_caps_free(line);
        heap_caps_free(scratch);
        heap_caps_free(fields);
        return 2;
    }
    rc = 1;
    while (csv_next_row(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        int n;
        current++;
        if (current != row) {
            continue;
        }
        n = csv_split_line(line, scratch,
                           (size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES,
                           fields, P4_CONFIG_CSV_MAX_COLS);
        if (col > n) {
            shell_print_error("csv: row %ld has %d column(s)", row, n);
            break;
        }
        {
            char *value = csv_alloc(P4_CONFIG_CSV_FIELD_BYTES + 1);
            if (value == NULL) {
                shell_print_error("csv: out of memory");
                break;
            }
            snprintf(value, P4_CONFIG_CSV_FIELD_BYTES + 1, "%.*s",
                     (int)fields[col - 1].len, scratch + fields[col - 1].off);
            if (!opts->bare) {
                shell_print_field("cell:", "R%ldC%ld = %s", row, col, value);
            } else {
                shell_transcript_appendf("%s\n", value);
            }
            found = (csv_store_var(opts, value) == 0);
            heap_caps_free(value);
        }
        rc = found ? 0 : 1;
        break;
    }
    if (rc == 1 && current < row) {
        shell_print_error("csv: file has %ld row(s)", current);
    }
    fclose(file);
    shell_sd_end(&session, "csv");
    heap_caps_free(line);
    heap_caps_free(scratch);
    heap_caps_free(fields);
    return rc;
}

/* ========================================================================
 * SET (in-place cell mutation)
 * ======================================================================== */

/** True when every double-quote in @p text is closed (`""` counts as one). */
static bool csv_quotes_balanced(const char *text)
{
    bool open = false;

    for (; *text != '\0'; text++) {
        if (*text == '"') {
            if (open && *(text + 1) == '"') {
                text++;
            } else {
                open = !open;
            }
        }
    }
    return !open;
}

/**
 * `csv set <file> <row> <col> <value...>`: replace one cell, 1-based.
 * Untouched rows stream through byte-for-byte; only the edited row is
 * re-quoted (RFC-4180 via the shared formatter). Ragged rows extend with
 * empty fields, missing trailing rows append as empty lines. Multi-line
 * quoted fields are refused rather than corrupted. Atomic temp+rename
 * behind a free-space estimate.
 */
static int csv_cmd_set(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    FILE *tmpf = NULL;
    char *line = NULL;
    char *scratch = NULL;
    csv_field_t *fields = NULL;
    char *value = NULL;
    char *tmp_path = NULL;
    const char *file_arg = NULL;
    long row = 0;
    long col = 0;
    long current = 0;
    size_t value_len = 0;
    int rc = 1;
    int i;

    if (argc < 6) {
        shell_command_csv_usage();
        return 2;
    }
    file_arg = argv[2];
    {
        char *end = NULL;
        row = strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || row < 1 || row > P4_CONFIG_CSV_ROWS_MAX) {
            shell_print_error("csv: row must be 1..%d", P4_CONFIG_CSV_ROWS_MAX);
            return 2;
        }
        col = strtol(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || col < 1 || col > P4_CONFIG_CSV_MAX_COLS) {
            shell_print_error("csv: col must be 1..%d", P4_CONFIG_CSV_MAX_COLS);
            return 2;
        }
    }
    /* The value is the rest of the line rejoined (quoting survives: the
     * tokenizer strips quotes, the formatter below re-quotes as needed). */
    for (i = 5; i < argc; i++) {
        value_len += strlen(argv[i]) + 1;
    }
    value = csv_alloc(value_len + 1);
    if (value == NULL) {
        shell_print_error("csv: out of memory");
        return 1;
    }
    value[0] = '\0';
    for (i = 5; i < argc; i++) {
        if (i > 5) {
            strcat(value, " ");
        }
        strcat(value, argv[i]);
    }
    line = csv_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    scratch = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES);
    fields = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * sizeof(csv_field_t));
    tmp_path = csv_alloc(P4_CONFIG_SD_PATH_BYTES + 8);
    if (line == NULL || scratch == NULL || fields == NULL || tmp_path == NULL) {
        shell_print_error("csv: out of memory");
        goto done;
    }
    file = csv_open_source(file_arg, resolved, sizeof(resolved), &session, "csv");
    if (file == NULL) {
        rc = 2;
        goto done;
    }
    snprintf(tmp_path, P4_CONFIG_SD_PATH_BYTES + 8, "%s.tmp", resolved);
    {
        uint64_t needed = (uint64_t)strlen(value) + P4_CONFIG_TEXT_LINE_BYTES;
        uint64_t reclaim = (uint64_t)storage_get_file_size(resolved);
        if (!storage_check_free_space(needed, reclaim, "csv")) {
            goto close;
        }
    }
    tmpf = fopen(tmp_path, "w");
    if (tmpf == NULL) {
        shell_print_error("csv: cannot write %s", tmp_path);
        goto close;
    }
    while (csv_next_row(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        int n;
        size_t llen = strlen(line);
        current++;
        /* The shared line model caps rows at TEXT_LINE_BYTES; a fragment
         * that fills the buffer without a newline may be a longer row the
         * verbs cannot parse. Refuse rather than corrupt it on write-back. */
        if (llen + 1 >= P4_CONFIG_TEXT_LINE_BYTES && line[llen - 1] != '\n') {
            shell_print_error("csv: row %ld exceeds %d bytes", current,
                              P4_CONFIG_TEXT_LINE_BYTES - 1);
            goto fail;
        }
        if (!csv_quotes_balanced(line)) {
            shell_print_error("csv: row %ld holds a multi-line field (unsupported)", current);
            goto fail;
        }
        if (current != row) {
            /* Untouched rows pass through byte-for-byte (quoting style and
             * raggedness preserved). Blank lines are dropped: they are not
             * rows in the csv verbs' shared numbering. */
            fputs(line, tmpf);
            if (line[strlen(line) - 1] != '\n') {
                fputc('\n', tmpf);
            }
            continue;
        }
        n = csv_split_line(line, scratch,
                           (size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES,
                           fields, P4_CONFIG_CSV_MAX_COLS);
        if (n > P4_CONFIG_CSV_MAX_COLS) {
            shell_print_error("csv: row %ld is wider than %d columns", row,
                              P4_CONFIG_CSV_MAX_COLS);
            goto fail;
        }
        for (i = 0; i < col; i++) {
            const char *text = "";
            char field[P4_CONFIG_CSV_FIELD_BYTES + 1];
            size_t need;
            char *quoted;
            if (i == col - 1) {
                text = value;
            } else if (i < n) {
                /* Never truncate a preserved field: refuse instead. */
                if (fields[i].len > P4_CONFIG_CSV_FIELD_BYTES) {
                    shell_print_error("csv: row %ld col %d exceeds %d bytes",
                                      row, i + 1, P4_CONFIG_CSV_FIELD_BYTES);
                    goto fail;
                }
                snprintf(field, sizeof(field), "%.*s",
                         (int)fields[i].len, scratch + fields[i].off);
                text = field;
            }
            need = csv_format_field(text, NULL, 0);
            quoted = csv_alloc(need);
            if (quoted == NULL) {
                shell_print_error("csv: out of memory");
                goto fail;
            }
            csv_format_field(text, quoted, need);
            fputs(quoted, tmpf);
            heap_caps_free(quoted);
            if (i + 1 < col) {
                fputc(',', tmpf);
            }
        }
        fputc('\n', tmpf);
    }
    /* Past-EOF rows: blank lines do not count as rows (csv_next_row skips
     * them, matching `cell`/`eval` numbering), so pad with "," filler rows
     * that do count, then write the new row itself. */
    while (current + 1 < row) {
        fputc(',', tmpf);
        fputc('\n', tmpf);
        current++;
    }
    if (current + 1 == row) {
        /* The target row never streamed (empty file / short file). */
        for (i = 0; i < col; i++) {
            size_t need;
            char *quoted;
            need = csv_format_field(i == col - 1 ? value : "", NULL, 0);
            quoted = csv_alloc(need);
            if (quoted == NULL) {
                shell_print_error("csv: out of memory");
                goto fail;
            }
            csv_format_field(i == col - 1 ? value : "", quoted, need);
            fputs(quoted, tmpf);
            heap_caps_free(quoted);
            if (i + 1 < col) {
                fputc(',', tmpf);
            }
        }
        fputc('\n', tmpf);
    }
    {
        int flush_rc = fflush(tmpf);
        int close_rc = fclose(tmpf);
        tmpf = NULL;
        if (flush_rc != 0 || close_rc != 0) {
            shell_print_error("csv: cannot write %s", tmp_path);
            goto close;
        }
    }
    /* FATFS rename refuses to overwrite: remove-then-rename like the store. */
    remove(resolved);
    if (rename(tmp_path, resolved) != 0) {
        remove(tmp_path);
        shell_print_error("csv: cannot replace %s", resolved);
        goto close;
    }
    shell_print_ok("csv: R%ldC%ld set", row, col);
    rc = 0;
    goto close;
fail:
    if (tmpf != NULL) {
        fclose(tmpf);
        tmpf = NULL;
    }
    remove(tmp_path);
    rc = 1;
close:
    if (tmpf != NULL) {
        fclose(tmpf);
    }
    if (file != NULL) {
        fclose(file);
        shell_sd_end(&session, "csv");
    }
done:
    heap_caps_free(line);
    heap_caps_free(scratch);
    heap_caps_free(fields);
    heap_caps_free(value);
    heap_caps_free(tmp_path);
    return rc;
}

/* ========================================================================
 * EVAL (formula grid)
 * ======================================================================== */

/** Numeric value of a grid cell for reference substitution (VAL semantics:
 *  leading-numeric parses, anything else reads as 0). */
static double csv_cell_number(const char *text)
{
    char *end = NULL;
    double value;

    if (text == NULL) {
        return 0.0;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == '\0') {
        return 0.0;
    }
    value = strtod(text, &end);
    if (end == text || !isfinite(value)) {
        return 0.0;
    }
    return value;
}

/** Aggregate ids for range functions (SUM/AVG/MIN/MAX/COUNT over R:C rects). */
typedef enum {
    CSV_AGG_NONE = 0,
    CSV_AGG_SUM,
    CSV_AGG_AVG,
    CSV_AGG_MIN,
    CSV_AGG_MAX,
    CSV_AGG_COUNT
} csv_agg_t;

/** Match an aggregate name at @p p (case-insensitive, '(' required). */
static csv_agg_t csv_agg_name(const char *p, size_t *len_out)
{
    static const struct {
        const char *name;
        csv_agg_t id;
    } table[] = {
        {"SUM", CSV_AGG_SUM},
        {"AVG", CSV_AGG_AVG},
        {"MIN", CSV_AGG_MIN},
        {"MAX", CSV_AGG_MAX},
        {"COUNT", CSV_AGG_COUNT},
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t n = strlen(table[i].name);
        const char *q;
        if (strncasecmp(p, table[i].name, n) != 0) {
            continue;
        }
        /* "SUMMARY" must not match SUM: only '(' (after spaces) counts. */
        q = p + n;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q == '(') {
            *len_out = (size_t)(q - p) + 1;
            return table[i].id;
        }
        return CSV_AGG_NONE;
    }
    return CSV_AGG_NONE;
}

/** Skip spaces, reporting the new position. */
static const char *csv_skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/** Parse one R<row>C<col> corner (no range check; caller clips). */
static bool csv_parse_corner(const char **p, long *row_out, long *col_out)
{
    const char *q = csv_skip_spaces(*p);
    char *end = NULL;

    if ((*q != 'R' && *q != 'r') || !isdigit((unsigned char)*(q + 1))) {
        return false;
    }
    *row_out = strtol(q + 1, &end, 10);
    q = end;
    if ((*q != 'C' && *q != 'c') || !isdigit((unsigned char)*(q + 1))) {
        return false;
    }
    *col_out = strtol(q + 1, &end, 10);
    *p = end;
    return true;
}

/**
 * Try FN(R1C1:R2C2) at @p p for the range aggregates. Corners normalize
 * (either order); only in-grid cells contribute, like single refs. COUNT
 * tallies non-empty cells; the math functions fold VAL-semantics numbers.
 * @return true with *value set and *end past ')' on a full match.
 */
static bool csv_parse_range_agg(const char *p, char **cells, int rows, int cols,
                                double *value, const char **end)
{
    size_t skip = 0;
    csv_agg_t agg = csv_agg_name(p, &skip);
    long r1, c1, r2, c2;
    long rtop, rbot, cleft, cright;
    long r, c;
    long n = 0;
    double acc = 0.0;
    double extreme = 0.0;
    bool have = false;

    if (agg == CSV_AGG_NONE) {
        return false;
    }
    p += skip;
    if (!csv_parse_corner(&p, &r1, &c1)) {
        return false;
    }
    p = csv_skip_spaces(p);
    if (*p != ':') {
        return false;
    }
    p = csv_skip_spaces(p + 1);
    if (!csv_parse_corner(&p, &r2, &c2)) {
        return false;
    }
    p = csv_skip_spaces(p);
    if (*p != ')') {
        return false;
    }
    rtop = (r1 < r2) ? r1 : r2;
    rbot = (r1 > r2) ? r1 : r2;
    cleft = (c1 < c2) ? c1 : c2;
    cright = (c1 > c2) ? c1 : c2;
    for (r = rtop; r <= rbot; r++) {
        for (c = cleft; c <= cright; c++) {
            const char *text = (r >= 1 && r <= rows && c >= 1 && c <= cols &&
                                cells[(r - 1) * cols + (c - 1)] != NULL)
                                   ? cells[(r - 1) * cols + (c - 1)]
                                   : NULL;
            if (agg == CSV_AGG_COUNT) {
                if (text != NULL && text[0] != '\0') {
                    n++;
                }
                continue;
            }
            if (text == NULL) {
                continue;
            }
            {
                double v = csv_cell_number(text);
                if (!have) {
                    extreme = v;
                    have = true;
                }
                acc += v;
                n++;
                if (agg == CSV_AGG_MIN && v < extreme) {
                    extreme = v;
                }
                if (agg == CSV_AGG_MAX && v > extreme) {
                    extreme = v;
                }
            }
        }
    }
    switch (agg) {
    case CSV_AGG_SUM:
        *value = acc;
        break;
    case CSV_AGG_AVG:
        *value = (n > 0) ? acc / (double)n : 0.0;
        break;
    case CSV_AGG_MIN:
    case CSV_AGG_MAX:
        *value = have ? extreme : 0.0;
        break;
    case CSV_AGG_COUNT:
        *value = (double)n;
        break;
    default:
        return false;
    }
    *end = p + 1;
    return true;
}

/** Substitute `R<row>C<col>` references (case-insensitive) with the current
 *  cell numbers. Out-of-range references read as 0. Always NUL-terminates. */
void csv_substitute_refs(const char *expr, char **cells, int rows, int cols,
                         char *out, size_t out_size)
{
    size_t used = 0;
    const char *p = expr;

    if (expr == NULL || cells == NULL || out == NULL || out_size == 0) {
        if (out != NULL && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    while (*p != '\0' && used + 1 < out_size) {
        /* Range aggregates first: SUM/AVG/MIN/MAX/COUNT(R1C1:R2C2). A bare
         * R-ref or any other word falls through to the copy below. */
        if (isalpha((unsigned char)*p)) {
            double agg_value = 0.0;
            const char *agg_end = NULL;
            if (csv_parse_range_agg(p, cells, rows, cols, &agg_value, &agg_end)) {
                char number[48];
                size_t len;
                size_t room;
                calc_format_number(agg_value, number, sizeof(number));
                len = strlen(number);
                room = out_size - 1 - used;
                if (len > room) {
                    len = room;
                }
                memcpy(out + used, number, len);
                used += len;
                out[used] = '\0';
                p = agg_end;
                continue;
            }

            /* An aggregate name followed by '(' that is not a valid R:C range
             * is a near-miss (SUM(R1C1), SUM(1), ...): copy FN(...) verbatim
             * so calc rejects it rather than us partially substituting the
             * refs inside it. */
            {
                size_t skip = 0;

                if (csv_agg_name(p, &skip) != CSV_AGG_NONE) {
                    const char *q = p + skip;   /* just past '(' */
                    int depth = 1;

                    while (*q != '\0' && depth > 0) {
                        if (*q == '(') {
                            depth++;
                        } else if (*q == ')') {
                            depth--;
                        }
                        q++;
                    }
                    while (p < q && used + 1 < out_size) {
                        out[used++] = *p++;
                    }
                    out[used] = '\0';
                    continue;
                }
            }
        }
        if ((*p == 'R' || *p == 'r') && isdigit((unsigned char)*(p + 1))) {
            char *end = NULL;
            long row = strtol(p + 1, &end, 10);
            if ((*end == 'C' || *end == 'c') && isdigit((unsigned char)*(end + 1))) {
                char *end2 = NULL;
                long col = strtol(end + 1, &end2, 10);

                /* A corner that begins a range (":" + corner) passes through
                 * verbatim; substituting the endpoints alone would corrupt the
                 * user's range into a malformed value for calc to reject. */
                {
                    const char *q = end2;

                    while (*q == ' ' || *q == '\t') {
                        q++;
                    }
                    if (*q == ':') {
                        const char *r2 = q + 1;
                        long rr, cc;

                        if (csv_parse_corner(&r2, &rr, &cc)) {
                            while (p < r2 && used + 1 < out_size) {
                                out[used++] = *p++;
                            }
                            out[used] = '\0';
                            continue;
                        }
                    }
                }

                char number[48];
                double value = 0.0;
                if (row >= 1 && row <= rows && col >= 1 && col <= cols &&
                    cells[(row - 1) * cols + (col - 1)] != NULL) {
                    value = csv_cell_number(cells[(row - 1) * cols + (col - 1)]);
                }
                calc_format_number(value, number, sizeof(number));
                {
                    size_t len = strlen(number);
                    size_t room = out_size - 1 - used;
                    if (len > room) {
                        len = room;
                    }
                    memcpy(out + used, number, len);
                    used += len;
                    out[used] = '\0';
                }
                p = end2;
                continue;
            }
        }
        out[used++] = *p++;
        out[used] = '\0';
    }
}

/** Print one field with CSV quoting when it needs it. */
static void csv_emit_quoted(const char *text)
{
    char *quoted;
    size_t need = csv_format_field(text, NULL, 0);
    if (need == 0) {
        return;
    }
    quoted = csv_alloc(need);
    if (quoted == NULL) {
        shell_transcript_appendf("%s", text != NULL ? text : "");
        return;
    }
    csv_format_field(text, quoted, need);
    shell_transcript_appendf("%s", quoted);
    heap_caps_free(quoted);
}

static void csv_grid_free(char **texts, bool *done, int rows, int cols)
{
    int i;
    if (texts != NULL) {
        for (i = 0; i < rows * cols; i++) {
            heap_caps_free(texts[i]);
        }
        heap_caps_free(texts);
    }
    heap_caps_free(done);
}

static int csv_cmd_eval(const char *file_arg, const csv_opts_t *opts)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    char *line = NULL;
    char *scratch = NULL;
    csv_field_t *fields = NULL;
    char **texts = NULL;
    bool *done = NULL;
    int rows = 0;
    int cols = 0;
    int rc = 1;
    int r, c, pass;

    if (opts->has_var) {
        /* The 24-slot environment table cannot hold a sheet; cell stores
         * single values instead (csv cell ... /v:NAME). */
        shell_command_csv_usage();
        return 2;
    }
    line = csv_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    scratch = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES);
    fields = csv_alloc((size_t)P4_CONFIG_CSV_MAX_COLS * sizeof(csv_field_t));
    texts = csv_alloc((size_t)P4_CONFIG_CSV_ROWS_MAX * P4_CONFIG_CSV_MAX_COLS * sizeof(char *));
    done = csv_alloc((size_t)P4_CONFIG_CSV_ROWS_MAX * P4_CONFIG_CSV_MAX_COLS * sizeof(bool));
    if (line == NULL || scratch == NULL || fields == NULL || texts == NULL || done == NULL) {
        shell_print_error("csv: out of memory");
        goto done;
    }
    memset(texts, 0, (size_t)P4_CONFIG_CSV_ROWS_MAX * P4_CONFIG_CSV_MAX_COLS * sizeof(char *));
    memset(done, 0, (size_t)P4_CONFIG_CSV_ROWS_MAX * P4_CONFIG_CSV_MAX_COLS * sizeof(bool));
    file = csv_open_source(file_arg, resolved, sizeof(resolved), &session, "csv");
    if (file == NULL) {
        rc = 2;
        goto done;
    }
    /* Load the grid (first row sets the width; ragged rows pad with ""). */
    while (rows < P4_CONFIG_CSV_ROWS_MAX &&
           csv_next_row(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        int n = csv_split_line(line, scratch,
                               (size_t)P4_CONFIG_CSV_MAX_COLS * P4_CONFIG_CSV_FIELD_BYTES,
                               fields, P4_CONFIG_CSV_MAX_COLS);
        if (n == 0) {
            continue;
        }
        if (rows == 0) {
            cols = n;
            if (cols > P4_CONFIG_CSV_MAX_COLS) {
                cols = P4_CONFIG_CSV_MAX_COLS;
            }
        }
        for (c = 0; c < cols; c++) {
            char *text;
            if (c < n && fields[c].len > 0) {
                text = csv_alloc(fields[c].len + 1);
                if (text == NULL) {
                    goto close;
                }
                memcpy(text, scratch + fields[c].off, fields[c].len);
                text[fields[c].len] = '\0';
            } else {
                text = csv_alloc(1);
                if (text == NULL) {
                    goto close;
                }
                text[0] = '\0';
            }
            texts[rows * cols + c] = text;
            done[rows * cols + c] = (text[0] != '=');
        }
        rows++;
    }
close:
    fclose(file);
    shell_sd_end(&session, "csv");
    file = NULL;
    if (rows == 0 || cols == 0) {
        shell_print_error("csv: no data rows in %s", resolved);
        goto done;
    }
    /* Resolve =EXPR cells iteratively (later cells may reference earlier
     * results and vice versa, up to the pass bound). */
    for (pass = 0; pass < P4_CONFIG_CSV_PASSES; pass++) {
        bool changed = false;
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                char *cell = texts[r * cols + c];
                if (done[r * cols + c] || cell == NULL || cell[0] != '=') {
                    continue;
                }
                {
                    char expr[P4_CONFIG_TEXT_LINE_BYTES];
                    char substituted[P4_CONFIG_TEXT_LINE_BYTES];
                    calc_value_t result;
                    const char *error = NULL;
                    snprintf(expr, sizeof(expr), "%s", cell + 1);
                    csv_substitute_refs(expr, texts, rows, cols, substituted, sizeof(substituted));
                    if (!calc_evaluate(substituted, &result, &error)) {
                        continue;
                    }
                    {
                        char *text;
                        if (result.is_string) {
                            text = csv_alloc(strlen(result.str) + 1);
                            if (text == NULL) {
                                goto done;
                            }
                            snprintf(text, strlen(result.str) + 1, "%s", result.str);
                        } else {
                            char number[64];
                            calc_format_number(result.num, number, sizeof(number));
                            text = csv_alloc(strlen(number) + 1);
                            if (text == NULL) {
                                goto done;
                            }
                            snprintf(text, strlen(number) + 1, "%s", number);
                        }
                        heap_caps_free(cell);
                        texts[r * cols + c] = text;
                        done[r * cols + c] = true;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    /* Report cells that never resolved (bad expression or reference cycle). */
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            if (!done[r * cols + c]) {
                shell_print_warning("csv: R%dC%d did not resolve (%s)", r + 1, c + 1,
                                    texts[r * cols + c] != NULL ? texts[r * cols + c] : "?");
            }
        }
    }
    if (!opts->bare) {
        /* Aligned table: measure each column first (capped for sanity). */
        int *widths = csv_alloc((size_t)cols * sizeof(int));
        if (widths == NULL) {
            goto done;
        }
        for (c = 0; c < cols; c++) {
            widths[c] = 1;
            for (r = 0; r < rows; r++) {
                const char *text = texts[r * cols + c];
                int len = (text != NULL) ? (int)strlen(text) : 0;
                if (len > 40) {
                    len = 40;
                }
                if (len > widths[c]) {
                    widths[c] = len;
                }
            }
        }
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                const char *text = texts[r * cols + c];
                shell_transcript_appendf("%-*s%s", widths[c], text != NULL ? text : "",
                                         (c + 1 < cols) ? " | " : "\n");
            }
        }
        heap_caps_free(widths);
    } else {
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                const char *text = texts[r * cols + c];
                csv_emit_quoted(text != NULL ? text : "");
                if (c + 1 < cols) {
                    shell_transcript_append_text(",");
                }
            }
            shell_transcript_append_text("\n");
        }
    }
    rc = 0;
done:
    if (file != NULL) {
        fclose(file);
        shell_sd_end(&session, "csv");
    }
    csv_grid_free(texts, done, (rows > 0) ? rows : P4_CONFIG_CSV_ROWS_MAX,
                  (cols > 0) ? cols : P4_CONFIG_CSV_MAX_COLS);
    heap_caps_free(line);
    heap_caps_free(scratch);
    heap_caps_free(fields);
    return rc;
}

int shell_command_csv(int argc, char **argv)
{
    const char *sub = NULL;
    const char *file = NULL;
    long row = 0;
    long col = 0;
    int numbers_seen = 0;
    csv_opts_t opts;
    int parse_rc;

    parse_rc = csv_parse_args(argc, argv, &sub, &file, &row, &col, &opts, &numbers_seen);
    if (parse_rc != 0) {
        /* `set` carries free-form value words that the shared parser
         * rejects; it validates its own tail instead. */
        if (argc >= 2 && strcasecmp(argv[1], "set") == 0) {
            return csv_cmd_set(argc, argv);
        }
        return parse_rc;
    }
    if (strcasecmp(sub, "rows") == 0) {
        return csv_cmd_rows(file, &opts);
    }
    if (strcasecmp(sub, "cols") == 0) {
        return csv_cmd_cols(file, &opts);
    }
    if (strcasecmp(sub, "cell") == 0 || strcasecmp(sub, "get") == 0) {
        return csv_cmd_cell(file, row, col, numbers_seen, &opts);
    }
    if (strcasecmp(sub, "set") == 0) {
        /* `set` takes free-form value words; csv_parse_args would reject
         * them as excess numbers, so it parses its own tail. */
        return csv_cmd_set(argc, argv);
    }
    if (strcasecmp(sub, "eval") == 0) {
        return csv_cmd_eval(file, &opts);
    }
    shell_command_csv_usage();
    return 2;
}
