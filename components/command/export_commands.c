/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file export_commands.c
 * @brief `export` — portable interchange out of the structured stores.
 *
 * The 95LX saved every app file in two forms (native + ASCII text). The
 * native forms here are `db export` (hex-safe) and the alarm INI files;
 * this verb is the ASCII side: `export <db NAME | alarms> <csv|json|txt>
 * <file>` renders the store through the EXISTING db/alarm APIs (no parallel
 * readers) and writes atomically via storage_write_text_file (temp+rename,
 * free-space pre-check). Secret record payloads are included (a backup is
 * complete or it is useless); the destination file inherits no redaction,
 * which the confirmation text says out loud.
 *
 * Contact/calendar interchange: `export db <name> vcf <file>` writes vCard
 * 3.0 contacts from the `k=v` record fields (name/tel/email/org/note, with
 * FN falling back to the record key and NOTE to the whole payload), and
 * `export alarms ics <file>` writes VEVENTs (floating local DTSTART, RRULE
 * for daily/weekly recurrences). Both round-trip through the `import` verb.
 *
 * Text-only interchange: binary payload bytes (<0x20, >0x7E) escape as
 * \u00XX in JSON, print as '.' in TXT, and pass through raw in CSV
 * (CSV has no escape for control bytes — keep binary records in the
 * native `db export` form instead).
 *
 * Batch-friendly: ERRORLEVEL 0 exported, 1 empty/nothing-to-write,
 * 2 usage/I-O. The file itself is the machine-readable form, so no /b.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "alarm.h"
#include "batch.h"
#include "command.h"
#include "db.h"
#include "security_commands.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#ifndef P4_CONFIG_DB_FIND_MAX
#define P4_CONFIG_DB_FIND_MAX 64
#endif

#ifndef P4_CONFIG_DB_RECORD_MAX_BYTES
#define P4_CONFIG_DB_RECORD_MAX_BYTES 4096
#endif

#ifndef P4_CONFIG_DB_EXPORT_MAX_BYTES
#define P4_CONFIG_DB_EXPORT_MAX_BYTES (256 * 1024)
#endif

#ifndef P4_CONFIG_ALARM_MAX_EVENTS
#define P4_CONFIG_ALARM_MAX_EVENTS 64
#endif

static void shell_command_export_usage(void)
{
    shell_print_usage("Usage: export <db NAME | alarms> <csv|json|txt|vcf|ics> <file>");
    shell_print_usage("  (db takes csv|json|txt|vcf; alarms takes csv|json|txt|ics)");
}

/* ========================================================================
 * Growable document buffer (PSRAM-first, capped)
 * ======================================================================== */

typedef struct {
    char *data;
    size_t used;
    size_t cap;
    bool overflow;
} export_doc_t;

static void export_doc_init(export_doc_t *doc)
{
    doc->data = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (doc->data == NULL) {
        doc->data = malloc(4096);
    }
    doc->used = 0;
    doc->cap = (doc->data != NULL) ? 4096 : 0;
    doc->overflow = (doc->data == NULL);
    if (doc->data != NULL) {
        doc->data[0] = '\0';
    }
}

static void export_doc_free(export_doc_t *doc)
{
    if (doc->data != NULL) {
        heap_caps_free(doc->data);
        doc->data = NULL;
    }
    doc->used = 0;
    doc->cap = 0;
}

/** Append @p len bytes (no NUL added). Sets overflow past the export cap. */
static void export_doc_write(export_doc_t *doc, const char *text, size_t len)
{
    if (doc->overflow || doc->data == NULL) {
        doc->overflow = true;
        return;
    }
    if (doc->used + len + 1 > (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES) {
        doc->overflow = true;
        return;
    }
    if (doc->used + len + 1 > doc->cap) {
        size_t grown = doc->cap * 2;
        char *grown_data;
        while (grown < doc->used + len + 1) {
            grown *= 2;
        }
        if (grown > (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES) {
            grown = P4_CONFIG_DB_EXPORT_MAX_BYTES;
        }
        grown_data = heap_caps_malloc(grown, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown_data == NULL) {
            grown_data = malloc(grown);
        }
        if (grown_data == NULL) {
            doc->overflow = true;
            return;
        }
        memcpy(grown_data, doc->data, doc->used + 1);
        heap_caps_free(doc->data);
        doc->data = grown_data;
        doc->cap = grown;
    }
    memcpy(doc->data + doc->used, text, len);
    doc->used += len;
    doc->data[doc->used] = '\0';
}

static void export_doc_text(export_doc_t *doc, const char *text)
{
    export_doc_write(doc, text, strlen(text));
}

/** Append one JSON string literal (quotes + escapes, no surrounding label). */
static void export_doc_json_string(export_doc_t *doc, const char *text, size_t len){
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    export_doc_text(doc, "\"");
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            export_doc_write(doc, pair, 2);
        } else if (c == '\n') {
            export_doc_text(doc, "\\n");
        } else if (c == '\r') {
            export_doc_text(doc, "\\r");
        } else if (c == '\t') {
            export_doc_text(doc, "\\t");
        } else if (c < 0x20 || c > 0x7E) {
            char uni[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF]};
            export_doc_write(doc, uni, sizeof(uni));
        } else {
            export_doc_write(doc, (const char *)&text[i], 1);
        }
    }
    export_doc_text(doc, "\"");
}

/** Append one CSV field via the single shared quoting implementation. */
static void export_doc_csv_field(export_doc_t *doc, const char *text)
{
    size_t need = csv_format_field(text, NULL, 0);
    char *quoted = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (quoted == NULL) {
        quoted = malloc(need);
    }
    if (quoted == NULL) {
        doc->overflow = true;
        return;
    }
    csv_format_field(text, quoted, need);
    export_doc_text(doc, quoted);
    heap_caps_free(quoted);
}

/** Append TXT-safe text (non-printables become '.'). */
static void export_doc_txt(export_doc_t *doc, const char *text, size_t len){
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        char out = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t' ? (char)c : '.';
        export_doc_write(doc, &out, 1);
    }
}

/** Append vCard/iCalendar-escaped text (`\\`, `\n`, `\,`, `\;`). */
static void export_doc_ical_text(export_doc_t *doc, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
        case '\\':
            export_doc_text(doc, "\\\\");
            break;
        case '\n':
            export_doc_text(doc, "\\n");
            break;
        case '\r':
            break;
        case ',':
            export_doc_text(doc, "\\,");
            break;
        case ';':
            export_doc_text(doc, "\\;");
            break;
        default:
            export_doc_write(doc, p, 1);
            break;
        }
    }
}

/** Append one `PROP:value` line with escaped text. */
static void export_doc_ical_prop(export_doc_t *doc, const char *prop, const char *text)
{
    export_doc_text(doc, prop);
    export_doc_text(doc, ":");
    export_doc_ical_text(doc, text);
    export_doc_text(doc, "\r\n");
}

/* ========================================================================
 * db collection (reuses db_find + db_get, like db_cmd_find)
 * ======================================================================== */

typedef struct {
    db_record_info_t *infos;
    int count;
} export_collect_t;

static bool export_collect_cb(const db_record_info_t *info, void *ctx)
{
    export_collect_t *c = (export_collect_t *)ctx;
    if (c->count >= P4_CONFIG_DB_FIND_MAX) {
        return false;
    }
    c->infos[c->count++] = *info;
    return true;
}

/* ========================================================================
 * db rendering
 * ======================================================================== */

static int export_db_csv(const char *name, export_doc_t *doc, int *rows_out)
{
    export_collect_t collect;
    int out_count = 0;
    esp_err_t error;
    int i;

    collect.infos = heap_caps_malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collect.infos == NULL) {
        collect.infos = malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t));
    }
    if (collect.infos == NULL) {
        return 2;
    }
    collect.count = 0;
    error = db_find(name, 0xFF, NULL, NULL, false, true,
                    export_collect_cb, &collect, &out_count);
    if (error == ESP_ERR_NOT_FOUND) {
        heap_caps_free(collect.infos);
        return 1;
    }
    if (error != ESP_OK) {
        heap_caps_free(collect.infos);
        return 2;
    }
    export_doc_text(doc, "id,cat,key,payload\n");
    for (i = 0; i < collect.count; i++) {
        char *payload = heap_caps_malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char idbuf[16];
        char catbuf[8];
        if (payload == NULL) {
            payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        }
        if (payload == NULL) {
            heap_caps_free(collect.infos);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        payload[len] = '\0';
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)info.id);
        snprintf(catbuf, sizeof(catbuf), "%u", (unsigned)info.category);
        export_doc_csv_field(doc, idbuf);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, catbuf);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, info.key);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, payload);
        export_doc_text(doc, "\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    heap_caps_free(collect.infos);
    return 0;
}

static int export_db_json(const char *name, export_doc_t *doc, int *rows_out)
{
    export_collect_t collect;
    int out_count = 0;
    esp_err_t error;
    int i;

    collect.infos = heap_caps_malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collect.infos == NULL) {
        collect.infos = malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t));
    }
    if (collect.infos == NULL) {
        return 2;
    }
    collect.count = 0;
    error = db_find(name, 0xFF, NULL, NULL, false, true,
                    export_collect_cb, &collect, &out_count);
    if (error == ESP_ERR_NOT_FOUND) {
        heap_caps_free(collect.infos);
        return 1;
    }
    if (error != ESP_OK) {
        heap_caps_free(collect.infos);
        return 2;
    }
    export_doc_text(doc, "[\n");
    for (i = 0; i < collect.count; i++) {
        char *payload = heap_caps_malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char idbuf[16];
        char catbuf[8];
        if (payload == NULL) {
            payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        }
        if (payload == NULL) {
            heap_caps_free(collect.infos);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)info.id);
        snprintf(catbuf, sizeof(catbuf), "%u", (unsigned)info.category);
        export_doc_text(doc, "  {\"id\": ");
        export_doc_text(doc, idbuf);
        export_doc_text(doc, ", \"cat\": ");
        export_doc_text(doc, catbuf);
        export_doc_text(doc, ", \"key\": ");
        export_doc_json_string(doc, info.key, strlen(info.key));
        export_doc_text(doc, ", \"payload\": ");
        export_doc_json_string(doc, payload, len);
        export_doc_text(doc, "}");
        export_doc_text(doc, (i + 1 < collect.count) ? ",\n" : "\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    export_doc_text(doc, "]\n");
    heap_caps_free(collect.infos);
    return 0;
}

static int export_db_txt(const char *name, export_doc_t *doc, int *rows_out)
{
    export_collect_t collect;
    int out_count = 0;
    esp_err_t error;
    int i;

    collect.infos = heap_caps_malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collect.infos == NULL) {
        collect.infos = malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t));
    }
    if (collect.infos == NULL) {
        return 2;
    }
    collect.count = 0;
    error = db_find(name, 0xFF, NULL, NULL, false, true,
                    export_collect_cb, &collect, &out_count);
    if (error == ESP_ERR_NOT_FOUND) {
        heap_caps_free(collect.infos);
        return 1;
    }
    if (error != ESP_OK) {
        heap_caps_free(collect.infos);
        return 2;
    }
    for (i = 0; i < collect.count; i++) {
        char *payload = heap_caps_malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char head[96];
        if (payload == NULL) {
            payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        }
        if (payload == NULL) {
            heap_caps_free(collect.infos);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        snprintf(head, sizeof(head), "#%lu cat=%u key=%s\n",
                 (unsigned long)info.id, (unsigned)info.category, info.key);
        export_doc_text(doc, head);
        export_doc_txt(doc, payload, len);
        export_doc_text(doc, "\n\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    heap_caps_free(collect.infos);
    return 0;
}

/* ========================================================================
 * vCard contact rendering (db records with k=v fields)
 * ======================================================================== */

/** Bounded copy into a fixed field (explicit truncation, no -Wformat-truncation).
 *  Shared with `import_commands.c` (declared in command.h). */
void command_copy_trunc(char *dst, size_t dst_size, const char *src)
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

static int export_db_vcf(const char *name, export_doc_t *doc, int *rows_out)
{
    export_collect_t collect;
    int out_count = 0;
    esp_err_t error;
    int i;

    collect.infos = heap_caps_malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collect.infos == NULL) {
        collect.infos = malloc((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t));
    }
    if (collect.infos == NULL) {
        return 2;
    }
    collect.count = 0;
    error = db_find(name, 0xFF, NULL, NULL, false, true,
                    export_collect_cb, &collect, &out_count);
    if (error == ESP_ERR_NOT_FOUND) {
        heap_caps_free(collect.infos);
        return 1;
    }
    if (error != ESP_OK) {
        heap_caps_free(collect.infos);
        return 2;
    }
    for (i = 0; i < collect.count; i++) {
        char *payload = heap_caps_malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char fn[64];
        char tel[128];
        char email[128];
        char org[64];
        char title[64];
        char note[512];
        if (payload == NULL) {
            payload = malloc(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        }
        if (payload == NULL) {
            heap_caps_free(collect.infos);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        payload[len] = '\0';
        fn[0] = tel[0] = email[0] = org[0] = title[0] = note[0] = '\0';
        db_field_get(payload, "name", fn, sizeof(fn));
        if (fn[0] == '\0') {
            command_copy_trunc(fn, sizeof(fn), info.key);
        }
        if (!db_field_get(payload, "tel", tel, sizeof(tel))) {
            db_field_get(payload, "phone", tel, sizeof(tel));
        }
        if (!db_field_get(payload, "email", email, sizeof(email))) {
            db_field_get(payload, "mail", email, sizeof(email));
        }
        db_field_get(payload, "org", org, sizeof(org));
        db_field_get(payload, "title", title, sizeof(title));
        if (!db_field_get(payload, "note", note, sizeof(note))) {
            command_copy_trunc(note, sizeof(note), payload);
        }
        export_doc_text(doc, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
        export_doc_ical_prop(doc, "N", fn);
        export_doc_ical_prop(doc, "FN", fn);
        if (tel[0] != '\0') {
            export_doc_ical_prop(doc, "TEL", tel);
        }
        if (email[0] != '\0') {
            export_doc_ical_prop(doc, "EMAIL", email);
        }
        if (org[0] != '\0') {
            export_doc_ical_prop(doc, "ORG", org);
        }
        if (title[0] != '\0') {
            export_doc_ical_prop(doc, "TITLE", title);
        }
        if (note[0] != '\0') {
            export_doc_ical_prop(doc, "NOTE", note);
        }
        export_doc_text(doc, "END:VCARD\r\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    heap_caps_free(collect.infos);
    return 0;
}

/* ========================================================================
 * alarm rendering (reuses alarm_list)
 * ======================================================================== */

typedef struct {
    alarm_event_t *events;
    int count;
} export_alarm_collect_t;

static bool export_alarm_cb(const alarm_event_t *event, void *ctx)
{
    export_alarm_collect_t *c = (export_alarm_collect_t *)ctx;
    if (c->count >= P4_CONFIG_ALARM_MAX_EVENTS) {
        return false;
    }
    c->events[c->count++] = *event;
    return true;
}

static void export_when_text(time_t when, char *out, size_t out_size)
{
    struct tm tmv;
    if (localtime_r(&when, &tmv) != NULL &&
        strftime(out, out_size, "%Y-%m-%d %H:%M", &tmv) != 0) {
        return;
    }
    snprintf(out, out_size, "%lld", (long long)when);
}

static int export_alarms_collect(export_alarm_collect_t *collect)
{
    esp_err_t error;

    collect->events = heap_caps_malloc((size_t)P4_CONFIG_ALARM_MAX_EVENTS * sizeof(alarm_event_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collect->events == NULL) {
        collect->events = malloc((size_t)P4_CONFIG_ALARM_MAX_EVENTS * sizeof(alarm_event_t));
    }
    if (collect->events == NULL) {
        return 2;
    }
    collect->count = 0;
    error = alarm_list(export_alarm_cb, collect);
    if (error != ESP_OK) {
        heap_caps_free(collect->events);
        collect->events = NULL;
        return 2;
    }
    return 0;
}

static int export_alarms_csv(export_doc_t *doc, int *rows_out)
{
    export_alarm_collect_t collect;
    int rc;
    int i;

    rc = export_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    export_doc_text(doc, "id,when,title,msg,recur,flags\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char idbuf[16];
        char whenbuf[32];
        char recurbuf[8];
        char flagbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)e->id);
        export_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(recurbuf, sizeof(recurbuf), "%u", (unsigned)e->recur);
        snprintf(flagbuf, sizeof(flagbuf), "%u", (unsigned)e->flags);
        export_doc_csv_field(doc, idbuf);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, whenbuf);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, e->title);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, e->msg);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, recurbuf);
        export_doc_text(doc, ",");
        export_doc_csv_field(doc, flagbuf);
        export_doc_text(doc, "\n");
        (*rows_out)++;
    }
    heap_caps_free(collect.events);
    return 0;
}

static int export_alarms_json(export_doc_t *doc, int *rows_out)
{
    export_alarm_collect_t collect;
    int rc;
    int i;

    rc = export_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    export_doc_text(doc, "[\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char idbuf[16];
        char whenbuf[32];
        char unbuf[24];
        char recurbuf[8];
        char flagbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)e->id);
        export_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(unbuf, sizeof(unbuf), "%lld", (long long)e->when);
        snprintf(recurbuf, sizeof(recurbuf), "%u", (unsigned)e->recur);
        snprintf(flagbuf, sizeof(flagbuf), "%u", (unsigned)e->flags);
        export_doc_text(doc, "  {\"id\": ");
        export_doc_text(doc, idbuf);
        export_doc_text(doc, ", \"when\": ");
        export_doc_json_string(doc, whenbuf, strlen(whenbuf));
        export_doc_text(doc, ", \"unix\": ");
        export_doc_text(doc, unbuf);
        export_doc_text(doc, ", \"title\": ");
        export_doc_json_string(doc, e->title, strlen(e->title));
        export_doc_text(doc, ", \"msg\": ");
        export_doc_json_string(doc, e->msg, strlen(e->msg));
        export_doc_text(doc, ", \"recur\": ");
        export_doc_text(doc, recurbuf);
        export_doc_text(doc, ", \"flags\": ");
        export_doc_text(doc, flagbuf);
        export_doc_text(doc, "}");
        export_doc_text(doc, (i + 1 < collect.count) ? ",\n" : "\n");
        (*rows_out)++;
    }
    export_doc_text(doc, "]\n");
    heap_caps_free(collect.events);
    return 0;
}

static int export_alarms_txt(export_doc_t *doc, int *rows_out)
{
    export_alarm_collect_t collect;
    int rc;
    int i;

    rc = export_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char head[128];
        char whenbuf[32];
        export_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(head, sizeof(head), "#%lu %s\n%s\n", (unsigned long)e->id, whenbuf, e->title);
        export_doc_text(doc, head);
        export_doc_txt(doc, e->msg, strlen(e->msg));
        export_doc_text(doc, "\n\n");
        (*rows_out)++;
    }
    heap_caps_free(collect.events);
    return 0;
}

/* ========================================================================
 * iCalendar event rendering
 * ======================================================================== */

/** Render a local timestamp as floating "YYYYMMDDTHHMMSS" (no Z). */
static void export_ics_when(time_t when, char *out, size_t out_size)
{
    struct tm tmv;
    if (localtime_r(&when, &tmv) != NULL &&
        strftime(out, out_size, "%Y%m%dT%H%M%S", &tmv) != 0) {
        return;
    }
    snprintf(out, out_size, "%lld", (long long)when);
}

static int export_alarms_ics(export_doc_t *doc, int *rows_out)
{
    export_alarm_collect_t collect;
    time_t now = time(NULL);
    char stamp[32];
    int rc;
    int i;

    rc = export_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    export_ics_when(now, stamp, sizeof(stamp));
    export_doc_text(doc, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n");
    export_doc_text(doc, "PRODID:-//P4MiniShell//EN\r\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char uid[32];
        char start[32];
        snprintf(uid, sizeof(uid), "%lu@p4minishell", (unsigned long)e->id);
        export_ics_when(e->when, start, sizeof(start));
        export_doc_text(doc, "BEGIN:VEVENT\r\n");
        export_doc_ical_prop(doc, "UID", uid);
        export_doc_ical_prop(doc, "DTSTAMP", stamp);
        export_doc_ical_prop(doc, "DTSTART", start);
        export_doc_ical_prop(doc, "SUMMARY", e->title);
        export_doc_ical_prop(doc, "DESCRIPTION", e->msg);
        if (e->recur == ALARM_RECUR_DAILY) {
            export_doc_ical_prop(doc, "RRULE", "FREQ=DAILY");
        } else if (e->recur == ALARM_RECUR_MONTHLY) {
            if (e->recur_nth != 0) {
                static const char *const days[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
                struct tm tmv;
                char rule[48];
                int wday = -1;
                int nth = e->recur_nth;
                if (localtime_r(&e->when, &tmv) != NULL) {
                    wday = tmv.tm_wday;
                }
                if (wday >= 0 && wday <= 6 && nth >= -1 && nth <= 5 && nth != 0) {
                    snprintf(rule, sizeof(rule), "RRULE:FREQ=MONTHLY;BYDAY=%s;BYSETPOS=%d",
                             days[wday], nth);
                    export_doc_text(doc, rule);
                    export_doc_text(doc, "\r\n");
                } else {
                    export_doc_ical_prop(doc, "RRULE", "FREQ=MONTHLY");
                }
            } else {
                char rule[40];
                int day = (e->recur_day >= 1 && e->recur_day <= 31) ? e->recur_day : 0;
                if (day == 0) {
                    struct tm tmv;
                    if (localtime_r(&e->when, &tmv) != NULL) {
                        day = tmv.tm_mday;
                    }
                }
                if (day >= 1) {
                    snprintf(rule, sizeof(rule), "FREQ=MONTHLY;BYMONTHDAY=%d", day);
                    export_doc_ical_prop(doc, "RRULE", rule);
                } else {
                    export_doc_ical_prop(doc, "RRULE", "FREQ=MONTHLY");
                }
            }
        } else if (e->recur == ALARM_RECUR_YEARLY) {
            export_doc_ical_prop(doc, "RRULE", "FREQ=YEARLY");
        } else if ((e->recur & ALARM_RECUR_WEEKLY) != 0) {
            static const char *const days[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
            char rule[64] = "FREQ=WEEKLY;BYDAY=";
            bool first = true;
            int d;
            for (d = 1; d <= 6; d++) {
                if (alarm_recur_weekday_matches(e->recur, d)) {
                    if (!first) {
                        snprintf(rule + strlen(rule), sizeof(rule) - strlen(rule), ",");
                    }
                    snprintf(rule + strlen(rule), sizeof(rule) - strlen(rule), "%s", days[d]);
                    first = false;
                }
            }
            if (alarm_recur_weekday_matches(e->recur, 0)) {
                if (!first) {
                    snprintf(rule + strlen(rule), sizeof(rule) - strlen(rule), ",");
                }
                snprintf(rule + strlen(rule), sizeof(rule) - strlen(rule), "SU");
            }
            if (!first) {
                export_doc_ical_prop(doc, "RRULE", rule);
            }
        }
        export_doc_text(doc, "END:VEVENT\r\n");
        (*rows_out)++;
    }
    export_doc_text(doc, "END:VCALENDAR\r\n");
    heap_caps_free(collect.events);
    return 0;
}

/* ========================================================================
 * Dispatcher
 * ======================================================================== */

int shell_command_export(int argc, char **argv)
{
    const char *what = NULL;
    const char *name = NULL;
    const char *format = NULL;
    const char *file = NULL;
    const char *pos[4];
    int pcount = 0;
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    export_doc_t doc;
    int rows = 0;
    int rc = 1;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (pcount < 4) {
            pos[pcount++] = arg;
        } else {
            shell_command_export_usage();
            return 2;
        }
    }
    if (pcount < 3) {
        shell_command_export_usage();
        return 2;
    }
    what = pos[0];
    if (strcasecmp(what, "db") == 0) {
        if (pcount != 4) {
            shell_command_export_usage();
            return 2;
        }
        /* A db export includes secret payloads (a backup is complete or
         * useless). Refuse while the device is locked so a locked session
         * cannot exfiltrate private records. Default (no passcode) is
         * unchanged. */
        if (!security_can_reveal_private()) {
            shell_print_error("export: device locked - unlock before exporting secret records");
            return 1;
        }
        name = pos[1];
        format = pos[2];
        file = pos[3];
    } else if (strcasecmp(what, "alarms") == 0 || strcasecmp(what, "alarm") == 0 ||
               strcasecmp(what, "cal") == 0) {
        if (pcount != 3) {
            shell_command_export_usage();
            return 2;
        }
        format = pos[1];
        file = pos[2];
    } else {
        shell_command_export_usage();
        return 2;
    }
    if (strcasecmp(format, "csv") != 0 && strcasecmp(format, "json") != 0 &&
        strcasecmp(format, "txt") != 0 && strcasecmp(format, "vcf") != 0 &&
        strcasecmp(format, "ics") != 0) {
        shell_command_export_usage();
        return 2;
    }
    if (strcasecmp(what, "db") == 0 && strcasecmp(format, "ics") == 0) {
        shell_command_export_usage();
        return 2;
    }
    if (strcasecmp(what, "db") != 0 && strcasecmp(format, "vcf") == 0) {
        shell_command_export_usage();
        return 2;
    }
    if (shell_fs_resolve_path(file, resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("export: invalid path");
        return 2;
    }
    export_doc_init(&doc);
    if (doc.overflow) {
        shell_print_error("export: out of memory");
        return 2;
    }
    if (strcasecmp(what, "db") == 0) {
        if (strcasecmp(format, "csv") == 0) {
            rc = export_db_csv(name, &doc, &rows);
        } else if (strcasecmp(format, "json") == 0) {
            rc = export_db_json(name, &doc, &rows);
        } else if (strcasecmp(format, "vcf") == 0) {
            rc = export_db_vcf(name, &doc, &rows);
        } else {
            rc = export_db_txt(name, &doc, &rows);
        }
    } else {
        if (strcasecmp(format, "csv") == 0) {
            rc = export_alarms_csv(&doc, &rows);
        } else if (strcasecmp(format, "json") == 0) {
            rc = export_alarms_json(&doc, &rows);
        } else if (strcasecmp(format, "ics") == 0) {
            rc = export_alarms_ics(&doc, &rows);
        } else {
            rc = export_alarms_txt(&doc, &rows);
        }
    }
    if (rc != 0) {
        if (rc == 1) {
            shell_print_error("export: nothing to export");
        }
        export_doc_free(&doc);
        return rc;
    }
    if (doc.overflow) {
        export_doc_free(&doc);
        shell_print_error("export: output exceeds %d bytes", P4_CONFIG_DB_EXPORT_MAX_BYTES);
        return 1;
    }
    if (rows == 0) {
        export_doc_free(&doc);
        shell_print_muted("export: store is empty");
        return 1;
    }
    if (storage_write_text_file(resolved, doc.data) != ESP_OK) {
        export_doc_free(&doc);
        shell_print_error("export: cannot write %s", resolved);
        return 2;
    }
    export_doc_free(&doc);
    shell_print_ok("export: %d row(s) -> %s (includes secret payloads)", rows, resolved);
    return 0;
}
