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
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

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

/** vCard/vCal unfolded-line cap (long NOTEs are truncated, never split). */
#define IMPORT_LINE_BYTES 1024

/** vCard NOTE field cap inside the built `k=v` payload. */
#define IMPORT_VCF_NOTE_BYTES 512

static void shell_command_import_usage(void)
{
    shell_print_usage("Usage: import db <name> <csv|json|vcf> <file>");
    shell_print_usage("Usage: import alarms <csv|json|ics> <file>");
}

/** Heap helper: PSRAM first, plain malloc fallback. Freed with heap_caps_free. */
static void *import_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(size);
    }
    return p;
}

/** Bounded string copy (explicit truncation, no -Wformat-truncation). */
static void import_copy_trunc(char *dst, size_t dst_size, const char *src)
{
    size_t n;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
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
 * Pure interchange parsers (declared in command.h, unit-tested)
 * ======================================================================== */

bool import_vcf_prop_split(const char *line, char *name_out, size_t name_size,
                           const char **value_out)
{
    const char *colon;
    const char *head_end;
    const char *name;
    const char *semi;
    const char *dot;
    size_t namelen;

    if (line == NULL || name_out == NULL || name_size == 0 || value_out == NULL) {
        return false;
    }
    colon = strchr(line, ':');
    if (colon == NULL || colon == line) {
        return false;
    }
    head_end = colon;
    /* The property name ends at the first ';' (parameters follow). */
    semi = memchr(line, ';', (size_t)(colon - line));
    if (semi != NULL) {
        head_end = semi;
    }
    name = line;
    namelen = (size_t)(head_end - line);
    /* Strip a group prefix ("item1.TEL" -> "TEL"). */
    dot = memchr(line, '.', namelen);
    if (dot != NULL) {
        name = dot + 1;
        namelen = (size_t)(head_end - name);
    }
    if (namelen == 0 || namelen + 1 > name_size) {
        return false;
    }
    memcpy(name_out, name, namelen);
    name_out[namelen] = '\0';
    *value_out = colon + 1;
    return true;
}

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

bool import_ics_datetime(const char *text, struct tm *out)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int n = 0;
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap;
    int maxday;

    if (text == NULL || out == NULL) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    /* Date-only form (VALUE=DATE) defaults to midnight. */
    if (sscanf(text, "%4d%2d%2d%n", &y, &mo, &d, &n) != 3) {
        return false;
    }
    if (text[n] == 'T' || text[n] == 't') {
        int m2 = 0;
        if (sscanf(text + n + 1, "%2d%2d%2d%n", &h, &mi, &s, &m2) != 3) {
            return false;
        }
        n += 1 + m2;
    }
    /* A trailing 'Z' (UTC) is accepted but read as device-local (documented). */
    if (text[n] == 'Z' || text[n] == 'z') {
        n++;
    }
    if (text[n] != '\0') {
        return false;
    }
    if (mo < 1 || mo > 12 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
        return false;
    }
    leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
    maxday = dim[mo - 1] + ((mo == 2) ? leap : 0);
    if (d < 1 || d > maxday) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900;
    out->tm_mon = mo - 1;
    out->tm_mday = d;
    out->tm_hour = h;
    out->tm_min = mi;
    out->tm_sec = s;
    out->tm_isdst = -1;
    return true;
}

/* ========================================================================
 * Line plumbing (unfolding, quote balancing)
 * ======================================================================== */

/** Read the next non-blank physical line (keeps the newline). False at EOF. */
static bool import_next_line(FILE *file, char *line, size_t size)
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
    while (import_next_line(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
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
                while (!balanced &&
                       import_next_line(file, line, P4_CONFIG_TEXT_LINE_BYTES)) {
                    balanced = (import_quotes_balanced(line) == balanced);
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
 * ======================================================================== */

typedef struct {
    int ok;
    int skipped;
} import_count_t;

/** Commit one parsed db row. @return true when the record landed. */
static bool import_db_commit(const char *name, long cat, const char *key,
                             const char *payload, bool secret, import_count_t *count)
{
    char keybuf[P4_CONFIG_DB_KEY_BYTES];
    uint32_t id = 0;

    if (cat < 0 || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        count->skipped++;
        return false;
    }
    import_copy_trunc(keybuf, sizeof(keybuf), key);
    if (db_add(name, (uint8_t)cat, keybuf, secret,
               payload != NULL ? payload : "", payload != NULL ? strlen(payload) : 0,
               &id) != ESP_OK) {
        count->skipped++;
        return false;
    }
    count->ok++;
    return true;
}

static int import_db_csv(const char *resolved, const char *name, import_count_t *count)
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
            import_db_commit(name, cat, key, payload, secret, count);
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

static int import_db_json(const char *resolved, const char *name, import_count_t *count)
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
        import_db_commit(name, cat, key, payload, false, count);
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

/* --- vCard contact import --- */

typedef struct {
    char name[64];
    char tel[128];
    char email[128];
    char org[64];
    char title[64];
    char note[IMPORT_VCF_NOTE_BYTES];
    bool active;
} import_vcard_t;

static void import_vcard_reset(import_vcard_t *c)
{
    memset(c, 0, sizeof(*c));
    c->active = true;
}

/** Unescape a vCard value in place (`\\` `\,` `\;` `\n`, then `;` -> `,`). */
static void import_vcf_unescape(char *text)
{
    char *r = text;
    char *w = text;

    while (*r != '\0') {
        if (*r == '\\' && *(r + 1) != '\0') {
            r++;
            if (*r == 'n' || *r == 'N') {
                *w++ = ' ';
            } else {
                *w++ = *r;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    for (w = text; *w != '\0'; w++) {
        if (*w == ';') {
            *w = ','; /* the `k=v` convention forbids ';' in values */
        }
    }
}

/** Append @p value to a comma-joined multi-value field. */
static void import_vcf_join(char *field, size_t size, const char *value)
{
    size_t used = strlen(field);
    size_t vlen = strlen(value);

    if (vlen == 0) {
        return;
    }
    if (used > 0 && used + 2 < size) {
        field[used++] = ',';
        field[used++] = ' ';
    }
    if (used + vlen >= size) {
        vlen = size - used - 1;
    }
    memcpy(field + used, value, vlen);
    field[used + vlen] = '\0';
}

/** Build "Given Family" from an N property ("Family;Given;..."). */
static void import_vcf_name_from_n(const char *value, char *out, size_t out_size)
{
    const char *semi = strchr(value, ';');
    char family[64];
    char given[64];
    size_t flen;
    size_t glen;
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (semi == NULL) {
        import_copy_trunc(out, out_size, value);
        return;
    }
    flen = (size_t)(semi - value);
    if (flen > sizeof(family) - 1) {
        flen = sizeof(family) - 1;
    }
    memcpy(family, value, flen);
    family[flen] = '\0';
    {
        const char *g = semi + 1;
        const char *extra = strchr(g, ';');
        glen = (extra != NULL) ? (size_t)(extra - g) : strlen(g);
        if (glen > sizeof(given) - 1) {
            glen = sizeof(given) - 1;
        }
        memcpy(given, g, glen);
        given[glen] = '\0';
    }
    if (given[0] != '\0') {
        import_copy_trunc(out + pos, out_size - pos, given);
        pos = strlen(out);
        if (family[0] != '\0' && pos + 1 < out_size) {
            out[pos++] = ' ';
            out[pos] = '\0';
        }
    }
    if (family[0] != '\0') {
        import_copy_trunc(out + pos, out_size - pos, family);
    }
}

static void import_vcf_commit(const char *name, const import_vcard_t *c, import_count_t *count)
{
    char payload[P4_CONFIG_DB_KEY_BYTES + 128 + 128 + 64 + 64 + IMPORT_VCF_NOTE_BYTES + 64];
    const char *key = c->name[0] != '\0' ? c->name : "contact";
    size_t used = 0;
    int n;

    payload[0] = '\0';
    n = snprintf(payload, sizeof(payload), "name=%s", key);
    if (n > 0) {
        used = (size_t)n;
    }
#define IMPORT_VCF_FIELD(f, v) do { \
        if ((v)[0] != '\0' && used + 1 < sizeof(payload)) { \
            int m = snprintf(payload + used, sizeof(payload) - used, ";" #f "=%s", (v)); \
            if (m > 0) { used += (size_t)m; } \
        } \
    } while (0)
    IMPORT_VCF_FIELD(tel, c->tel);
    IMPORT_VCF_FIELD(email, c->email);
    IMPORT_VCF_FIELD(org, c->org);
    IMPORT_VCF_FIELD(title, c->title);
    IMPORT_VCF_FIELD(note, c->note);
#undef IMPORT_VCF_FIELD
    import_db_commit(name, 0, key, payload, false, count);
}

typedef struct {
    import_vcard_t card;
    bool in_card;
    int contact_no;
    const char *dbname;
    import_count_t *count;
} import_vcf_ctx_t;

/** Handle one complete (unfolded) vCard content line. */
static void import_vcf_line(const char *current, void *vctx)
{
    import_vcf_ctx_t *ctx = (import_vcf_ctx_t *)vctx;
    char prop[32];
    const char *value = NULL;

    if (ctx == NULL) {
        return;
    }
    if (!import_vcf_prop_split(current, prop, sizeof(prop), &value)) {
        return;
    }
    if (strcasecmp(prop, "BEGIN") == 0) {
        if (strcasecmp(value, "VCARD") == 0) {
            import_vcard_reset(&ctx->card);
            ctx->in_card = true;
        }
        return;
    }
    if (!ctx->in_card) {
        return;
    }
    if (strcasecmp(prop, "END") == 0) {
        if (strcasecmp(value, "VCARD") == 0 && ctx->card.active) {
            ctx->contact_no++;
            if (ctx->card.name[0] == '\0') {
                snprintf(ctx->card.name, sizeof(ctx->card.name),
                         "contact-%d", ctx->contact_no);
            }
            if (ctx->count->ok + ctx->count->skipped < P4_CONFIG_DB_EXPORT_MAX_RECORDS) {
                import_vcf_commit(ctx->dbname, &ctx->card, ctx->count);
            }
            ctx->in_card = false;
        }
        return;
    }
    {
        char decoded[IMPORT_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        import_vcf_unescape(decoded);
        if (strcasecmp(prop, "FN") == 0) {
            import_copy_trunc(ctx->card.name, sizeof(ctx->card.name), decoded);
        } else if (strcasecmp(prop, "N") == 0) {
            if (ctx->card.name[0] == '\0') {
                import_vcf_name_from_n(decoded, ctx->card.name, sizeof(ctx->card.name));
            }
        } else if (strcasecmp(prop, "TEL") == 0) {
            import_vcf_join(ctx->card.tel, sizeof(ctx->card.tel), decoded);
        } else if (strcasecmp(prop, "EMAIL") == 0) {
            import_vcf_join(ctx->card.email, sizeof(ctx->card.email), decoded);
        } else if (strcasecmp(prop, "ORG") == 0) {
            if (ctx->card.org[0] == '\0') {
                import_copy_trunc(ctx->card.org, sizeof(ctx->card.org), decoded);
            }
        } else if (strcasecmp(prop, "TITLE") == 0) {
            if (ctx->card.title[0] == '\0') {
                import_copy_trunc(ctx->card.title, sizeof(ctx->card.title), decoded);
            }
        } else if (strcasecmp(prop, "NOTE") == 0) {
            if (ctx->card.note[0] == '\0') {
                import_copy_trunc(ctx->card.note, sizeof(ctx->card.note), decoded);
            }
        }
        /* PHOTO/LOGO/KEY/SOUND/AGENT/PRODID/VERSION/REV/UID/... skipped. */
    }
}

/** Strip trailing CR/LF in place. */
static void import_strip_eol(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
}

/**
 * vCard/vCal logical-line pump: unfolds continuation lines (leading SP/HTAB)
 * and calls @p emit once per complete line plus once for a trailing pending
 * line at EOF.
 */
typedef void (*import_line_emit_t)(const char *line, void *ctx);

static void import_pump_unfolded(FILE *file, import_line_emit_t emit, void *ctx)
{
    char *line = import_alloc(IMPORT_LINE_BYTES);
    char *unfolded = import_alloc(IMPORT_LINE_BYTES);

    if (line == NULL || unfolded == NULL) {
        if (line != NULL) {
            heap_caps_free(line);
        }
        if (unfolded != NULL) {
            heap_caps_free(unfolded);
        }
        return;
    }
    unfolded[0] = '\0';
    while (import_next_line(file, line, IMPORT_LINE_BYTES)) {
        size_t ulen;
        import_strip_eol(line);
        if ((line[0] == ' ' || line[0] == '\t') && unfolded[0] != '\0') {
            ulen = strlen(unfolded);
            if (ulen + strlen(line) < IMPORT_LINE_BYTES) {
                memmove(unfolded + ulen, line + 1, strlen(line));
            }
            continue;
        }
        if (unfolded[0] != '\0') {
            emit(unfolded, ctx);
        }
        snprintf(unfolded, IMPORT_LINE_BYTES, "%s", line);
    }
    if (unfolded[0] != '\0') {
        emit(unfolded, ctx);
    }
    heap_caps_free(line);
    heap_caps_free(unfolded);
}

static int import_db_vcf(const char *resolved, const char *name, import_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    import_vcf_ctx_t ctx;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.dbname = name;
    ctx.count = count;
    import_pump_unfolded(file, import_vcf_line, &ctx);
    import_close_file(file, &session);
    return 0;
}

/* ========================================================================
 * alarms import (csv / json / ics)
 * ======================================================================== */

/** Parse an export `when` cell: unix digits, or "YYYY-MM-DD HH:MM[:SS]". */
static bool import_alarm_when(const char *text, time_t *out)
{
    const char *p = text;
    bool digits;
    char date[16];
    const char *sp;

    if (text == NULL || out == NULL) {
        return false;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    digits = *p != '\0';
    for (const char *q = p; *q != '\0'; q++) {
        if (*q < '0' || *q > '9') {
            digits = false;
            break;
        }
    }
    if (digits) {
        *out = (time_t)strtoll(p, NULL, 10);
        return *out > 0;
    }
    sp = strchr(p, ' ');
    if (sp == NULL) {
        return false;
    }
    if ((size_t)(sp - p) >= sizeof(date)) {
        return false;
    }
    memcpy(date, p, (size_t)(sp - p));
    date[sp - p] = '\0';
    return alarm_parse_datetime(date, sp + 1, out);
}

/** Commit one parsed alarm row. @return true when the event landed. */
static bool import_alarm_commit(const char *title, const char *msg, time_t when,
                                long recur, long flags,
                                const alarm_recur_params_t *params, import_count_t *count)
{
    char t[P4_CONFIG_ALARM_TITLE_BYTES];
    char m[P4_CONFIG_ALARM_MSG_BYTES];
    uint8_t action = ALARM_ACTION_NOTIFY;
    uint8_t fl;
    uint32_t id = 0;

    import_copy_trunc(t, sizeof(t), title != NULL && title[0] != '\0' ? title : "Alarm");
    import_copy_trunc(m, sizeof(m), msg);
    fl = (uint8_t)(flags & 0xFF);
    fl |= ALARM_FLAG_ENABLED;
    fl &= (uint8_t)~ALARM_FLAG_FIRED;
    if ((fl & ALARM_FLAG_SILENT) == 0) {
        action |= ALARM_ACTION_BEEP;
    }
    if (recur < 0 || recur > 255) {
        count->skipped++;
        return false;
    }
    if (alarm_add_ex(t, m, when, (uint8_t)recur, action, NULL, params, &id) != ESP_OK) {
        count->skipped++;
        return false;
    }
    count->ok++;
    return true;
}

static int import_alarms_csv(const char *resolved, import_count_t *count)
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
        if (!import_alarm_when(scratch + fields[1].off, &when)) {
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
        import_alarm_commit(scratch + fields[2].off, scratch + fields[3].off,
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
            if (!have_when && !import_alarm_when(strbuf, when_out)) {
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

static int import_alarms_json(const char *resolved, import_count_t *count)
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
        import_alarm_commit(title, msg, when, recur, flags, NULL, count);
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

/* --- iCalendar event import --- */

typedef struct {
    char title[P4_CONFIG_ALARM_TITLE_BYTES];
    char msg[P4_CONFIG_ALARM_MSG_BYTES];
    time_t when;
    bool have_when;
    long recur;
    bool active;
} import_vevent_t;

static void import_vevent_reset(import_vevent_t *e)
{
    memset(e, 0, sizeof(*e));
    e->active = true;
}

/** Unescape ics text (`\\` `\,` `\;` `\n`) in place. */
static void import_ics_unescape(char *text)
{
    char *r = text;
    char *w = text;

    while (*r != '\0') {
        if (*r == '\\' && *(r + 1) != '\0') {
            r++;
            if (*r == 'n' || *r == 'N') {
                *w++ = ' ';
            } else {
                *w++ = *r;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/**
 * Map an RRULE value to the alarm recur byte, filling monthly/yearly
 * params (DTSTART month/day supplied for defaults). Unknown frequencies
 * degrade to one-shot; the `wday` (DTSTART weekday) anchors BYDAY-less
 * weekly rules.
 */
static long import_ics_rrule(const char *value, int wday, int ev_mon, int ev_mday,
                             alarm_recur_params_t *params)
{
    bool weekly = false;
    bool daily = false;
    bool monthly = false;
    bool yearly = false;
    long mask = 0;
    long monthday = 0;
    long setpos = 0;
    const char *p = value;

    if (params != NULL) {
        memset(params, 0, sizeof(*params));
    }
    while (*p != '\0') {
        if (strncasecmp(p, "FREQ=DAILY", 10) == 0) {
            daily = true;
        } else if (strncasecmp(p, "FREQ=WEEKLY", 12) == 0) {
            weekly = true;
        } else if (strncasecmp(p, "FREQ=MONTHLY", 13) == 0) {
            monthly = true;
        } else if (strncasecmp(p, "FREQ=YEARLY", 12) == 0) {
            yearly = true;
        } else if (strncasecmp(p, "BYDAY=", 6) == 0) {
            const char *q = p + 6;
            while (*q != '\0' && *q != ';') {
                if (strncasecmp(q, "MO", 2) == 0) {
                    mask |= 1 << 1;
                } else if (strncasecmp(q, "TU", 2) == 0) {
                    mask |= 1 << 2;
                } else if (strncasecmp(q, "WE", 2) == 0) {
                    mask |= 1 << 3;
                } else if (strncasecmp(q, "TH", 2) == 0) {
                    mask |= 1 << 4;
                } else if (strncasecmp(q, "FR", 2) == 0) {
                    mask |= 1 << 5;
                } else if (strncasecmp(q, "SA", 2) == 0) {
                    mask |= 1 << 6;
                } else if (strncasecmp(q, "SU", 2) == 0) {
                    mask |= 1 << 0;
                }
                while (*q != '\0' && *q != ',' && *q != ';') {
                    q++;
                }
                if (*q == ',') {
                    q++;
                }
            }
        } else if (strncasecmp(p, "BYMONTHDAY=", 11) == 0) {
            monthday = strtol(p + 11, NULL, 10);
        } else if (strncasecmp(p, "BYSETPOS=", 9) == 0) {
            setpos = strtol(p + 9, NULL, 10);
        }
        while (*p != '\0' && *p != ';') {
            p++;
        }
        if (*p == ';') {
            p++;
        }
    }
    if (daily) {
        return ALARM_RECUR_DAILY;
    }
    if (weekly) {
        if (mask == 0 && wday >= 0 && wday <= 6) {
            mask = 1 << wday;
        }
        if (mask == 0) {
            return ALARM_RECUR_NONE;
        }
        return (long)(ALARM_RECUR_WEEKLY | (mask & 0x7F));
    }
    if (monthly) {
        if (params != NULL) {
            if (setpos >= -1 && setpos <= 5 && setpos != 0) {
                params->nth = (int)setpos;
            } else if (monthday >= 1 && monthday <= 31) {
                params->day = (int)monthday;
            } else if (ev_mday >= 1 && ev_mday <= 31) {
                params->day = ev_mday;
            }
        }
        return ALARM_RECUR_MONTHLY;
    }
    if (yearly) {
        if (params != NULL) {
            params->month = (ev_mon >= 1 && ev_mon <= 12) ? ev_mon : 0;
            params->day = (ev_mday >= 1 && ev_mday <= 31) ? ev_mday : 0;
        }
        return ALARM_RECUR_YEARLY;
    }
    return ALARM_RECUR_NONE;
}

typedef struct {
    import_vevent_t ev;
    char rrule[128];
    bool in_event;
    import_count_t *count;
} import_ics_ctx_t;

/** Handle one complete (unfolded) iCalendar content line. */
static void import_ics_line(const char *current, void *vctx)
{
    import_ics_ctx_t *ctx = (import_ics_ctx_t *)vctx;
    char prop[32];
    const char *value = NULL;
    struct tm tmv;

    if (ctx == NULL) {
        return;
    }
    if (!import_vcf_prop_split(current, prop, sizeof(prop), &value)) {
        return;
    }
    if (strcasecmp(prop, "BEGIN") == 0) {
        if (strcasecmp(value, "VEVENT") == 0) {
            import_vevent_reset(&ctx->ev);
            ctx->rrule[0] = '\0';
            ctx->in_event = true;
        }
        return;
    }
    if (!ctx->in_event) {
        return;
    }
    if (strcasecmp(prop, "END") == 0) {
        if (strcasecmp(value, "VEVENT") == 0) {
            if (ctx->ev.active && ctx->ev.have_when) {
                int wday = -1;
                int ev_mon = 0;
                int ev_mday = 0;
                struct tm lt;
                alarm_recur_params_t params;
                if (localtime_r(&ctx->ev.when, &lt) != NULL) {
                    wday = lt.tm_wday;
                    ev_mon = lt.tm_mon + 1;
                    ev_mday = lt.tm_mday;
                }
                memset(&params, 0, sizeof(params));
                if (ctx->count->ok + ctx->count->skipped < P4_CONFIG_ALARM_MAX_EVENTS) {
                    import_alarm_commit(ctx->ev.title, ctx->ev.msg, ctx->ev.when,
                                        import_ics_rrule(ctx->rrule, wday, ev_mon, ev_mday,
                                                         &params),
                                        ALARM_FLAG_ENABLED, &params, ctx->count);
                }
            }
            ctx->in_event = false;
        }
        return;
    }
    if (strcasecmp(prop, "DTSTART") == 0) {
        if (import_ics_datetime(value, &tmv)) {
            ctx->ev.when = mktime(&tmv);
            ctx->ev.have_when = (ctx->ev.when != (time_t)-1);
        }
    } else if (strcasecmp(prop, "SUMMARY") == 0) {
        char decoded[IMPORT_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        import_ics_unescape(decoded);
        import_copy_trunc(ctx->ev.title, sizeof(ctx->ev.title), decoded);
    } else if (strcasecmp(prop, "DESCRIPTION") == 0) {
        char decoded[IMPORT_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        import_ics_unescape(decoded);
        import_copy_trunc(ctx->ev.msg, sizeof(ctx->ev.msg), decoded);
    } else if (strcasecmp(prop, "RRULE") == 0) {
        import_copy_trunc(ctx->rrule, sizeof(ctx->rrule), value);
    }
    /* UID/DTSTAMP/DTEND/DURATION/others are intentionally ignored. */
}

static int import_alarms_ics(const char *resolved, import_count_t *count)
{
    shell_sd_session_t session;
    FILE *file;
    import_ics_ctx_t ctx;

    {
        char live[P4_CONFIG_SD_PATH_BYTES];
        file = import_open_file(resolved, live, sizeof(live), &session);
        if (file == NULL) {
            return 2;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.count = count;
    import_pump_unfolded(file, import_ics_line, &ctx);
    import_close_file(file, &session);
    return 0;
}

/* ========================================================================
 * Dispatcher
 * ======================================================================== */

int shell_command_import(int argc, char **argv)
{
    const char *pos[5];
    int pcount = 0;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    import_count_t count = {0, 0};
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
