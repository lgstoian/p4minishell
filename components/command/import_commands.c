/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file import_commands.c
 * @brief `import` — portable interchange INTO the structured stores.
 *
 * The 95LX saved every app file in two forms (native + ASCII text). The
 * native side here is `db import` (hex-safe) and the alarm INI files; this
 * verb is the ASCII side back in:
 *
 *   import db <name> <csv|json|vcf> <file>
 *   import alarms <csv|json|ics> <file>
 *
 * Argument order mirrors `export`. The csv/json shapes match `export` output
 * exactly, so an export round-trips
 * (fresh ids are always handed out; the exported id column is ignored). vcf
 * reads vCard 3.0 contacts into `k=v` records (name/tel/email/org/note);
 * ics reads VEVENTs into alarms. Imported alarms arm with notify-only action
 * and keep their ENABLED bit (FIRED is cleared — a stale fire is history).
 *
 * Text-only interchange limits (same honesty as `export`): a CSV row must be
 * single-line, so payloads holding raw newlines round-trip via json or the
 * native `db export` form instead (such rows are skipped and counted, never
 * truncated). vCard `;` in values becomes `,` (the `k=v` convention forbids
 * `;`); binary vCard properties (PHOTO/LOGO/...) are skipped. An ics
 * `DTSTART...Z` (UTC) is read as device-local time and documented.
 *
 * Every read is a guarded SD session with a free-space-agnostic streaming
 * loop (only the JSON slurp is bounded by P4_CONFIG_DB_EXPORT_MAX_BYTES);
 * rows are capped by P4_CONFIG_DB_EXPORT_MAX_RECORDS. Batch-friendly:
 * ERRORLEVEL 0 imported, 1 empty/nothing imported, 2 usage/I-O.
 */

#include "command.h"
#include "storage.h"
#include "storage_commands.h"
#include "shell.h"
#include "db.h"
#include "alarm.h"
#include "pim.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef P4_CONFIG_DB_RECORD_MAX_BYTES
#define P4_CONFIG_DB_RECORD_MAX_BYTES 4096
#endif

#ifndef P4_CONFIG_DB_EXPORT_MAX_BYTES
#define P4_CONFIG_DB_EXPORT_MAX_BYTES (256 * 1024)
#endif

#ifndef P4_CONFIG_DB_EXPORT_MAX_RECORDS
#define P4_CONFIG_DB_EXPORT_MAX_RECORDS 256
#endif

#ifndef P4_CONFIG_DB_KEY_BYTES
#define P4_CONFIG_DB_KEY_BYTES 32
#endif

#ifndef P4_CONFIG_DB_CATEGORY_COUNT
#define P4_CONFIG_DB_CATEGORY_COUNT 16
#endif

#ifndef P4_CONFIG_ALARM_MAX_EVENTS
#define P4_CONFIG_ALARM_MAX_EVENTS 64
#endif

#ifndef P4_CONFIG_ALARM_TITLE_BYTES
#define P4_CONFIG_ALARM_TITLE_BYTES 48
#endif

#ifndef P4_CONFIG_ALARM_MSG_BYTES
#define P4_CONFIG_ALARM_MSG_BYTES 160
#endif

#ifndef P4_CONFIG_SD_PATH_BYTES
#define P4_CONFIG_SD_PATH_BYTES 320
#endif

#ifndef P4_CONFIG_TEXT_LINE_BYTES
#define P4_CONFIG_TEXT_LINE_BYTES 512
#endif

/** CSV row accumulator: a quoted 4 KB payload plus headroom. */
#define IMPORT_ROW_BYTES (P4_CONFIG_DB_RECORD_MAX_BYTES + 512)

/* (vCard/vCal line caps live in components/pim: PIM_LINE_BYTES,
 * PIM_VCF_NOTE_BYTES.) */

static void shell_command_import_usage(void)
{
    shell_print_usage("Usage: import db <name> <csv|json|vcf> <file>");
    shell_print_usage("Usage: import alarms <csv|json|ics> <file>");
}

/** Heap helper: PSRAM (bulk import buffers must stay out of the scarce
 *  internal DMA heap). Freed with heap_caps_free. */
static void *import_alloc(size_t size)
{
    return p4heap_alloc_psram(size);
}

/** Guarded SD open for reading (mirrors the csv source pattern). */
static FILE *import_open_file(const char *file_arg, char *resolved_out, size_t resolved_size,
                              shell_sd_session_t *session_out)
{
    FILE *file;

    if (shell_fs_resolve_path(file_arg, resolved_out, resolved_size) != ESP_OK) {
        shell_print_error("import: invalid path");
        return NULL;
    }
    if (shell_sd_begin(session_out) != ESP_OK) {
        shell_print_error("import: SD card not present - insert and retry");
        return NULL;
    }
    file = fopen(resolved_out, "r");
    if (file == NULL) {
        shell_print_error("import: cannot open %s (%s)", resolved_out, strerror(errno));
        shell_sd_end(session_out, "import");
        return NULL;
    }
    return file;
}

static void import_close_file(FILE *file, shell_sd_session_t *session)
{
    if (file != NULL) {
        fclose(file);
    }
    shell_sd_end(session, "import");
}

/* ========================================================================
 * Pure JSON interchange parser (declared in command.h, unit-tested).
 * The vCard/iCalendar line parsers live in components/pim.
 * ======================================================================== */

size_t import_json_unescape(const char *src, size_t len, char *dst, size_t dst_size)
{
    size_t si = 0;
    size_t di = 0;

    if (dst == NULL || dst_size == 0) {
        return 0;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    while (si < len && di + 1 < dst_size) {
        char c = src[si];
        if (c != '\\' || si + 1 >= len) {
            dst[di++] = c;
            si++;
            continue;
        }
        si++; /* consume the backslash */
        switch (src[si]) {
        case '"': dst[di++] = '"'; si++; break;
        case '\\': dst[di++] = '\\'; si++; break;
        case '/': dst[di++] = '/'; si++; break;
        case 'b': dst[di++] = '\b'; si++; break;
        case 'f': dst[di++] = '\f'; si++; break;
        case 'n': dst[di++] = '\n'; si++; break;
        case 'r': dst[di++] = '\r'; si++; break;
        case 't': dst[di++] = '\t'; si++; break;
        case 'u': {
            unsigned cp = 0;
            int k;
            if (si + 4 >= len) {
                dst[di++] = 'u';
                si++;
                break;
            }
            for (k = 1; k <= 4; k++) {
                char h = src[si + k];
                cp <<= 4;
                if (h >= '0' && h <= '9') {
                    cp |= (unsigned)(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    cp |= (unsigned)(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    cp |= (unsigned)(h - 'A' + 10);
                } else {
                    cp |= 0xFFFFFFFFu;
                    break;
                }
            }
            if ((cp & 0xFFFFFFFFu) == 0xFFFFFFFFu) {
                dst[di++] = 'u'; /* tolerant: keep the letter, drop '\' */
                si++;
                break;
            }
            si += 5;
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = '?'; /* lone surrogate: no pair handling in flat text */
            }
            if (cp < 0x80) {
                dst[di++] = (char)cp;
            } else if (cp < 0x800 && di + 2 < dst_size) {
                dst[di++] = (char)(0xC0 | (cp >> 6));
                dst[di++] = (char)(0x80 | (cp & 0x3F));
            } else if (di + 3 < dst_size) {
                dst[di++] = (char)(0xE0 | (cp >> 12));
                dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[di++] = (char)(0x80 | (cp & 0x3F));
            } else {
                dst[di++] = '?';
            }
            break;
        }
        default:
            dst[di++] = src[si]; /* tolerant: keep the escaped char */
            si++;
            break;
        }
    }
    dst[di] = '\0';
    return di;
}

/* (iCalendar DATE-TIME parsing lives in components/pim: pim_ics_datetime.) */

/* ========================================================================
 * Line plumbing (non-blank physical lines, quote balancing)
 * ======================================================================== */

/** True when every double-quote in @p text is closed (`""` counts as one). */
static bool import_quotes_balanced(const char *text)
{
    bool open = false;

    for (; *text != '\0'; text++) {
        if (*text == '"') {
            if (open && *(text + 1) == '"') {
                text++; /* escaped pair inside a quoted field */
            } else {
                open = !open;
            }
        }
    }
    return !open;
}

/**
 * Read one logical CSV row, joining physical lines while a quoted field
 * dangles (export quotes fields holding commas/quotes). @return true with a
 * balanced row in @p row, false at EOF (@p capped reports a row that hit the
 * buffer cap and must be skipped, never truncated).
 */
static bool import_read_csv_row(FILE *file, char *row, size_t size, bool *capped)
{
    size_t used = 0;
    char *line;

    *capped = false;
    row[0] = '\0';
    line = import_alloc(P4_CONFIG_TEXT_LINE_BYTES);
    if (line == NULL) {
        return false;
    }
    while (pim_next_line(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
        size_t len = strlen(line);
        if (used + len + 1 > size) {
            /* Too big for one row: skip it whole (a multi-line payload
             * belongs in json/native instead). When mid-row, drain to the
             * next balanced boundary so the following row still parses:
             * quote parity adds mod 2, so the running balance flips per
             * odd-quoted line. An oversize line on an empty row needs no
             * drain — it is dropped and reading resumes after it. */
            *capped = true;
            if (!import_quotes_balanced(row)) {
                bool balanced = false;
                /* Drain following lines until quote parity closes (the row was
                 * already oversized/unbalanced; never stop on an unbalanced
                 * line). */
                while (!balanced &&
                       pim_next_line(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
                    balanced = import_quotes_balanced(line);
                }
            }
            heap_caps_free(line);
            return true;
        }
        memcpy(row + used, line, len + 1);
        used += len;
        if (import_quotes_balanced(row)) {
            heap_caps_free(line);
            return true;
        }
    }
    heap_caps_free(line);
    return used > 0;
}

/** Case-insensitive header-row match for the `export` shapes. */
static bool import_is_header4(const char *a, const char *b, const char *c, const char *d,
                              const char *ea, const char *eb, const char *ec, const char *ed)
{
    return strcasecmp(a, ea) == 0 && strcasecmp(b, eb) == 0 &&
           strcasecmp(c, ec) == 0 && strcasecmp(d, ed) == 0;
}

/* ========================================================================
 * db import (csv / json / vcf)
 * Commits and counts are pim_db_commit/pim_count_t in components/pim,
 * shared with the vcf pipeline; csv/json parsing stays here.
 * ======================================================================== */

static int import_db_csv(const char *resolved, const char *name, pim_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    char *row;
    char *scratch;
    csv_field_t fields[8];
    bool first = true;
    int n;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    row = import_alloc(IMPORT_ROW_BYTES);
    scratch = import_alloc(IMPORT_ROW_BYTES);
    if (row == NULL || scratch == NULL) {
        if (row != NULL) {
            heap_caps_free(row);
        }
        if (scratch != NULL) {
            heap_caps_free(scratch);
        }
        import_close_file(file, &session);
        shell_print_error("import: out of memory");
        return 2;
    }
    for (;;) {
        bool capped = false;
        const char *key;
        const char *payload;
        char *end = NULL;
        long cat;

        if (count->ok + count->skipped >= P4_CONFIG_DB_EXPORT_MAX_RECORDS) {
            break;
        }
        if (!import_read_csv_row(file, row, IMPORT_ROW_BYTES, &capped)) {
            break;
        }
        if (capped) {
            count->skipped++; /* multi-line payload: use json/native instead */
            continue;
        }
        n = csv_split_line(row, scratch, IMPORT_ROW_BYTES, fields, 8);
        if (n < 4) {
            count->skipped++;
            continue;
        }
        if (first && import_is_header4(scratch + fields[0].off, scratch + fields[1].off,
                                      scratch + fields[2].off, scratch + fields[3].off,
                                      "id", "cat", "key", "payload")) {
            first = false;
            continue;
        }
        first = false;
        cat = strtol(scratch + fields[1].off, &end, 10);
        if (end == scratch + fields[1].off || *end != '\0') {
            count->skipped++;
            continue;
        }
        key = scratch + fields[2].off;
        payload = scratch + fields[3].off;
        {
            bool secret = false;
            if (n >= 5) {
                const char *f = scratch + fields[4].off;
                secret = strcasecmp(f, "1") == 0 || strcasecmp(f, "secret") == 0 ||
                         strcasecmp(f, "true") == 0;
            }
            pim_db_commit(name, cat, key, payload, secret, count);
        }
    }
    heap_caps_free(row);
    heap_caps_free(scratch);
    import_close_file(file, &session);
    return 0;
}

/* --- Minimal JSON scanner for the exact `export` shapes --- */

typedef struct {
    const char *p;
    const char *end;
} import_json_t;

static void import_json_ws(import_json_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
                             *j->p == '\r' || *j->p == '\n')) {
        j->p++;
    }
}

static bool import_json_ch(import_json_t *j, char c)
{
    import_json_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

/** Parse a JSON string (opening quote current); unescapes into @p out. */
static bool import_json_string(import_json_t *j, char *out, size_t out_size)
{
    const char *start;
    size_t raw;

    import_json_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    start = j->p;
    while (j->p < j->end) {
        if (*j->p == '\\' && j->p + 1 < j->end) {
            j->p += 2;
            continue;
        }
        if (*j->p == '"') {
            break;
        }
        j->p++;
    }
    if (j->p >= j->end) {
        return false;
    }
    raw = (size_t)(j->p - start);
    j->p++; /* closing quote */
    import_json_unescape(start, raw, out, out_size);
    return true;
}

static bool import_json_long(import_json_t *j, long *out)
{
    char *end = NULL;

    import_json_ws(j);
    *out = strtol(j->p, &end, 10);
    if (end == j->p) {
        return false;
    }
    j->p = end;
    return true;
}

/** Skip one JSON value (string/number/literal/nested array or object). */
static bool import_json_skip(import_json_t *j)
{
    char tmp[32];

    import_json_ws(j);
    if (j->p >= j->end) {
        return false;
    }
    if (*j->p == '"') {
        return import_json_string(j, tmp, sizeof(tmp));
    }
    if (*j->p == '{' || *j->p == '[') {
        char open = *j->p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool instr = false;
        j->p++;
        depth = 1;
        while (j->p < j->end && depth > 0) {
            char c = *j->p++;
            if (instr) {
                if (c == '\\' && j->p < j->end) {
                    j->p++;
                } else if (c == '"') {
                    instr = false;
                }
            } else if (c == '"') {
                instr = true;
            } else if (c == open) {
                depth++;
            } else if (c == close) {
                depth--;
            }
        }
        return depth == 0;
    }
    /* number or literal: consume to the next structural char. */
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
           *j->p != ' ' && *j->p != '\t' && *j->p != '\r' && *j->p != '\n') {
        j->p++;
    }
    return true;
}

/** Slurp a whole interchange file, capped (JSON needs random access). */
static char *import_slurp(const char *resolved, size_t cap, size_t *len_out)
{
    shell_sd_session_t session;
    FILE *file;
    char *buf;
    size_t used = 0;
    size_t got;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return NULL;
        }
    }
    buf = import_alloc(cap + 1);
    if (buf == NULL) {
        import_close_file(file, &session);
        return NULL;
    }
    while (used < cap && (got = fread(buf + used, 1, cap - used, file)) > 0) {
        used += got;
    }
    if (!feof(file)) {
        heap_caps_free(buf); /* bigger than the interchange cap */
        import_close_file(file, &session);
        return NULL;
    }
    buf[used] = '\0';
    import_close_file(file, &session);
    if (len_out != NULL) {
        *len_out = used;
    }
    return buf;
}

/** Parse one `{"id":..,"cat":..,"key":"..","payload":".."}` object (id ignored). */
static bool import_db_json_object(import_json_t *j, char *key_out, size_t key_size,
                                  char *pay_out, size_t pay_size, long *cat_out)
{
    char name[32];
    bool seen_end = false;

    key_out[0] = '\0';
    pay_out[0] = '\0';
    *cat_out = 0;
    if (!import_json_ch(j, '{')) {
        return false;
    }
    for (;;) {
        import_json_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            seen_end = true;
            break;
        }
        if (!import_json_string(j, name, sizeof(name))) {
            return false;
        }
        if (!import_json_ch(j, ':')) {
            return false;
        }
        if (strcasecmp(name, "cat") == 0) {
            long v = 0;
            if (!import_json_long(j, &v)) {
                return false;
            }
            *cat_out = v;
        } else if (strcasecmp(name, "key") == 0) {
            if (!import_json_string(j, key_out, key_size)) {
                return false;
            }
        } else if (strcasecmp(name, "payload") == 0) {
            if (!import_json_string(j, pay_out, pay_size)) {
                return false;
            }
        } else {
            if (!import_json_skip(j)) {
                return false;
            }
        }
        import_json_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            seen_end = true;
            break;
        }
        return false;
    }
    return seen_end;
}

static int import_db_json(const char *resolved, const char *name, pim_count_t *count)
{
    size_t len = 0;
    char *buf = import_slurp(resolved, (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES, &len);
    import_json_t j;
    char *key;
    char *payload;

    if (buf == NULL) {
        shell_print_error("import: cannot read %s", resolved);
        return 2;
    }
    key = import_alloc(P4_CONFIG_DB_KEY_BYTES);
    payload = import_alloc((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (key == NULL || payload == NULL) {
        if (key != NULL) {
            heap_caps_free(key);
        }
        if (payload != NULL) {
            heap_caps_free(payload);
        }
        heap_caps_free(buf);
        shell_print_error("import: out of memory");
        return 2;
    }
    j.p = buf;
    j.end = buf + len;
    if (!import_json_ch(&j, '[')) {
        heap_caps_free(key);
        heap_caps_free(payload);
        heap_caps_free(buf);
        shell_print_error("import: malformed JSON (expected '[')");
        return 2;
    }
    import_json_ws(&j);
    if (j.p < j.end && *j.p == ']') {
        heap_caps_free(key); /* empty array: nothing to do */
        heap_caps_free(payload);
        heap_caps_free(buf);
        return 0;
    }
    for (;;) {
        long cat = 0;
        if (count->ok + count->skipped >= P4_CONFIG_DB_EXPORT_MAX_RECORDS) {
            break;
        }
        if (!import_db_json_object(&j, key, P4_CONFIG_DB_KEY_BYTES,
                                   payload, (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1, &cat)) {
            heap_caps_free(key);
            heap_caps_free(payload);
            heap_caps_free(buf);
            shell_print_error("import: malformed JSON object");
            return 2;
        }
        pim_db_commit(name, cat, key, payload, false, count);
        if (!import_json_ch(&j, ',')) {
            break;
        }
    }
    if (!import_json_ch(&j, ']')) {
        heap_caps_free(key);
        heap_caps_free(payload);
        heap_caps_free(buf);
        shell_print_error("import: malformed JSON (expected ']')");
        return 2;
    }
    heap_caps_free(key);
    heap_caps_free(payload);
    heap_caps_free(buf);
    return 0;
}

/* --- vCard contact import (pim_vcf_parse_stream in components/pim) --- */

/**
 * vcf file entry: guarded SD open stays here (prints + sessions are the
 * command layer's job); parsing and committing are pim_vcf_parse_stream.
 */
static int import_db_vcf(const char *resolved, const char *name, pim_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    int rc;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    rc = pim_vcf_parse_stream(file, name, count);
    import_close_file(file, &session);
    return rc;
}

/* ========================================================================
 * alarms import (csv / json / ics)
 * Commits, when-cells, and counts are pim_alarm_commit/pim_alarm_when/
 * pim_count_t in components/pim, shared with the ics pipeline; csv/json
 * parsing stays here.
 * ======================================================================== */

static int import_alarms_csv(const char *resolved, pim_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    char *row;
    char *scratch;
    csv_field_t fields[8];
    bool first = true;
    int n;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    row = import_alloc(IMPORT_ROW_BYTES);
    scratch = import_alloc(IMPORT_ROW_BYTES);
    if (row == NULL || scratch == NULL) {
        if (row != NULL) {
            heap_caps_free(row);
        }
        if (scratch != NULL) {
            heap_caps_free(scratch);
        }
        import_close_file(file, &session);
        shell_print_error("import: out of memory");
        return 2;
    }
    for (;;) {
        bool capped = false;
        time_t when = 0;
        char *end = NULL;
        long recur = 0;
        long flags = 0;

        if (count->ok + count->skipped >= P4_CONFIG_ALARM_MAX_EVENTS) {
            break;
        }
        if (!import_read_csv_row(file, row, IMPORT_ROW_BYTES, &capped)) {
            break;
        }
        if (capped) {
            count->skipped++;
            continue;
        }
        n = csv_split_line(row, scratch, IMPORT_ROW_BYTES, fields, 8);
        if (n < 6) {
            count->skipped++;
            continue;
        }
        if (first && import_is_header4(scratch + fields[0].off, scratch + fields[1].off,
                                      scratch + fields[2].off, scratch + fields[3].off,
                                      "id", "when", "title", "msg")) {
            first = false;
            continue;
        }
        first = false;
        if (!pim_alarm_when(scratch + fields[1].off, &when)) {
            count->skipped++;
            continue;
        }
        recur = strtol(scratch + fields[4].off, &end, 10);
        if (end == scratch + fields[4].off || *end != '\0') {
            count->skipped++;
            continue;
        }
        flags = strtol(scratch + fields[5].off, &end, 10);
        if (end == scratch + fields[5].off || *end != '\0') {
            count->skipped++;
            continue;
        }
        pim_alarm_commit(scratch + fields[2].off, scratch + fields[3].off,
                            when, recur, flags, NULL, count);
    }
    heap_caps_free(row);
    heap_caps_free(scratch);
    import_close_file(file, &session);
    return 0;
}

/** Parse one export-shape alarm object; `unix` wins over `when`. */
static bool import_alarm_json_object(import_json_t *j, char *title_out, size_t title_size,
                                     char *msg_out, size_t msg_size, time_t *when_out,
                                     long *recur_out, long *flags_out)
{
    char name[32];
    char strbuf[256];
    bool seen_end = false;
    bool have_when = false;

    title_out[0] = '\0';
    msg_out[0] = '\0';
    *when_out = 0;
    *recur_out = 0;
    *flags_out = 0;
    if (!import_json_ch(j, '{')) {
        return false;
    }
    for (;;) {
        import_json_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            seen_end = true;
            break;
        }
        if (!import_json_string(j, name, sizeof(name))) {
            return false;
        }
        if (!import_json_ch(j, ':')) {
            return false;
        }
        if (strcasecmp(name, "unix") == 0) {
            long v = 0;
            if (!import_json_long(j, &v) || v <= 0) {
                return false;
            }
            *when_out = (time_t)v;
            have_when = true;
        } else if (strcasecmp(name, "when") == 0) {
            if (!import_json_string(j, strbuf, sizeof(strbuf))) {
                return false;
            }
            if (!have_when && !pim_alarm_when(strbuf, when_out)) {
                return false;
            }
            have_when = have_when || (*when_out > 0);
            if (*when_out <= 0) {
                return false;
            }
        } else if (strcasecmp(name, "title") == 0) {
            if (!import_json_string(j, title_out, title_size)) {
                return false;
            }
        } else if (strcasecmp(name, "msg") == 0) {
            if (!import_json_string(j, msg_out, msg_size)) {
                return false;
            }
        } else if (strcasecmp(name, "recur") == 0) {
            if (!import_json_long(j, recur_out)) {
                return false;
            }
        } else if (strcasecmp(name, "flags") == 0) {
            if (!import_json_long(j, flags_out)) {
                return false;
            }
        } else {
            if (!import_json_skip(j)) {
                return false;
            }
        }
        import_json_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            seen_end = true;
            break;
        }
        return false;
    }
    return seen_end && have_when;
}

static int import_alarms_json(const char *resolved, pim_count_t *count)
{
    size_t len = 0;
    char *buf = import_slurp(resolved, (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES, &len);
    import_json_t j;
    char *title;
    char *msg;

    if (buf == NULL) {
        shell_print_error("import: cannot read %s", resolved);
        return 2;
    }
    title = import_alloc(P4_CONFIG_ALARM_TITLE_BYTES);
    msg = import_alloc(P4_CONFIG_ALARM_MSG_BYTES);
    if (title == NULL || msg == NULL) {
        if (title != NULL) {
            heap_caps_free(title);
        }
        if (msg != NULL) {
            heap_caps_free(msg);
        }
        heap_caps_free(buf);
        shell_print_error("import: out of memory");
        return 2;
    }
    j.p = buf;
    j.end = buf + len;
    if (!import_json_ch(&j, '[')) {
        heap_caps_free(title);
        heap_caps_free(msg);
        heap_caps_free(buf);
        shell_print_error("import: malformed JSON (expected '[')");
        return 2;
    }
    import_json_ws(&j);
    if (j.p < j.end && *j.p == ']') {
        heap_caps_free(title);
        heap_caps_free(msg);
        heap_caps_free(buf);
        return 0;
    }
    for (;;) {
        time_t when = 0;
        long recur = 0;
        long flags = 0;
        if (count->ok + count->skipped >= P4_CONFIG_ALARM_MAX_EVENTS) {
            break;
        }
        if (!import_alarm_json_object(&j, title, P4_CONFIG_ALARM_TITLE_BYTES,
                                      msg, P4_CONFIG_ALARM_MSG_BYTES,
                                      &when, &recur, &flags)) {
            heap_caps_free(title);
            heap_caps_free(msg);
            heap_caps_free(buf);
            shell_print_error("import: malformed JSON object");
            return 2;
        }
        pim_alarm_commit(title, msg, when, recur, flags, NULL, count);
        if (!import_json_ch(&j, ',')) {
            break;
        }
    }
    if (!import_json_ch(&j, ']')) {
        heap_caps_free(title);
        heap_caps_free(msg);
        heap_caps_free(buf);
        shell_print_error("import: malformed JSON (expected ']')");
        return 2;
    }
    heap_caps_free(title);
    heap_caps_free(msg);
    heap_caps_free(buf);
    return 0;
}

/* --- iCalendar event import (pim_ics_parse_stream in components/pim) --- */

/* (VEVENT assembly lives in components/pim: pim_ics_parse_stream.) */

static int import_alarms_ics(const char *resolved, pim_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    int rc;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    rc = pim_ics_parse_stream(file, count);
    import_close_file(file, &session);
    return rc;
}

/* ========================================================================
 * Dispatcher
 * ======================================================================== */

int shell_command_import(int argc, char **argv)
{
    const char *pos[5];
    int pcount = 0;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    pim_count_t count = {0, 0};
    int rc = 2;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (pcount < 5) {
            pos[pcount++] = arg;
        } else {
            shell_command_import_usage();
            return 2;
        }
    }
    if (pcount < 1) {
        shell_command_import_usage();
        return 2;
    }
    if (strcasecmp(pos[0], "db") == 0) {
        const char *name;
        const char *file;
        const char *format;
        db_info_t info;

        if (pcount != 4) {
            shell_command_import_usage();
            return 2;
        }
        name = pos[1];
        format = pos[2];
        file = pos[3];
        if (!db_name_valid(name)) {
            shell_print_error("import: invalid database name %s", name);
            return 2;
        }
        if (db_info(name, &info) != ESP_OK) {
            shell_print_error("import: db %s not found (create it first)", name);
            return 1;
        }
        if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("import: invalid path");
            return 2;
        }
        if (strcasecmp(format, "csv") == 0) {
            rc = import_db_csv(resolved, name, &count);
        } else if (strcasecmp(format, "json") == 0) {
            rc = import_db_json(resolved, name, &count);
        } else if (strcasecmp(format, "vcf") == 0) {
            rc = import_db_vcf(resolved, name, &count);
        } else {
            shell_command_import_usage();
            return 2;
        }
        if (rc != 0) {
            return rc;
        }
        if (count.ok == 0) {
            shell_print_muted("import: nothing imported into %s (%d skipped)", name, count.skipped);
            return 1;
        }
        shell_print_ok("import: %d record(s) -> %s (%d skipped)", count.ok, name, count.skipped);
        return 0;
    }
    if (strcasecmp(pos[0], "alarms") == 0 || strcasecmp(pos[0], "alarm") == 0 ||
        strcasecmp(pos[0], "cal") == 0) {
        const char *file;
        const char *format;

        if (pcount != 3) {
            shell_command_import_usage();
            return 2;
        }
        file = pos[2];
        format = pos[1];
        if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("import: invalid path");
            return 2;
        }
        if (strcasecmp(format, "csv") == 0) {
            rc = import_alarms_csv(resolved, &count);
        } else if (strcasecmp(format, "json") == 0) {
            rc = import_alarms_json(resolved, &count);
        } else if (strcasecmp(format, "ics") == 0) {
            rc = import_alarms_ics(resolved, &count);
        } else {
            shell_command_import_usage();
            return 2;
        }
        if (rc != 0) {
            return rc;
        }
        if (count.ok == 0) {
            shell_print_muted("import: nothing imported (%d skipped)", count.skipped);
            return 1;
        }
        shell_print_ok("import: %d event(s) (%d skipped)", count.ok, count.skipped);
        return 0;
    }
    shell_command_import_usage();
    return 2;
}
