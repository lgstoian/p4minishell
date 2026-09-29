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
#include "pim.h"
#include "security_commands.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "p4heap.h"

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
 * Format append helpers (JSON/CSV/TXT write into the shared pim_doc_t
 * buffer owned by components/pim; vCard/iCalendar appends live there too)
 * ======================================================================== */

/** Append one JSON string literal (quotes + escapes, no surrounding label). */
static void export_doc_json_string(pim_doc_t *doc, const char *text, size_t len){
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    pim_doc_text(doc, "\"");
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            pim_doc_write(doc, pair, 2);
        } else if (c == '\n') {
            pim_doc_text(doc, "\\n");
        } else if (c == '\r') {
            pim_doc_text(doc, "\\r");
        } else if (c == '\t') {
            pim_doc_text(doc, "\\t");
        } else if (c < 0x20 || c > 0x7E) {
            char uni[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF]};
            pim_doc_write(doc, uni, sizeof(uni));
        } else {
            pim_doc_write(doc, (const char *)&text[i], 1);
        }
    }
    pim_doc_text(doc, "\"");
}

/** Append one CSV field via the single shared quoting implementation. */
static void export_doc_csv_field(pim_doc_t *doc, const char *text)
{
    size_t need = csv_format_field(text, NULL, 0);
    char *quoted = p4heap_alloc_psram(need);
    if (quoted == NULL) {
        doc->overflow = true;
        return;
    }
    csv_format_field(text, quoted, need);
    pim_doc_text(doc, quoted);
    heap_caps_free(quoted);
}

/** Append TXT-safe text (non-printables become '.'). */
static void export_doc_txt(pim_doc_t *doc, const char *text, size_t len){
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        char out = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t' ? (char)c : '.';
        pim_doc_write(doc, &out, 1);
    }
}

/* ========================================================================
 * db rendering (collection via components/pim; vcf via pim_db_render_vcf)
 * ======================================================================== */

static int export_db_csv(const char *name, pim_doc_t *doc, int *rows_out)
{
    pim_db_collect_t collect;
    int rc;
    int i;

    rc = pim_db_collect(name, &collect);
    if (rc != 0) {
        return rc;
    }
    pim_doc_text(doc, "id,cat,key,payload\n");
    for (i = 0; i < collect.count; i++) {
        char *payload = p4heap_alloc_psram(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char idbuf[16];
        char catbuf[8];
        if (payload == NULL) {
            pim_db_collect_free(&collect);
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
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, catbuf);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, info.key);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, payload);
        pim_doc_text(doc, "\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    pim_db_collect_free(&collect);
    return 0;
}

static int export_db_json(const char *name, pim_doc_t *doc, int *rows_out)
{
    pim_db_collect_t collect;
    int rc;
    int i;

    rc = pim_db_collect(name, &collect);
    if (rc != 0) {
        return rc;
    }
    pim_doc_text(doc, "[\n");
    for (i = 0; i < collect.count; i++) {
        char *payload = p4heap_alloc_psram(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char idbuf[16];
        char catbuf[8];
        if (payload == NULL) {
            pim_db_collect_free(&collect);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)info.id);
        snprintf(catbuf, sizeof(catbuf), "%u", (unsigned)info.category);
        pim_doc_text(doc, "  {\"id\": ");
        pim_doc_text(doc, idbuf);
        pim_doc_text(doc, ", \"cat\": ");
        pim_doc_text(doc, catbuf);
        pim_doc_text(doc, ", \"key\": ");
        export_doc_json_string(doc, info.key, strlen(info.key));
        pim_doc_text(doc, ", \"payload\": ");
        export_doc_json_string(doc, payload, len);
        pim_doc_text(doc, "}");
        pim_doc_text(doc, (i + 1 < collect.count) ? ",\n" : "\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    pim_doc_text(doc, "]\n");
    pim_db_collect_free(&collect);
    return 0;
}

static int export_db_txt(const char *name, pim_doc_t *doc, int *rows_out)
{
    pim_db_collect_t collect;
    int rc;
    int i;

    rc = pim_db_collect(name, &collect);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < collect.count; i++) {
        char *payload = p4heap_alloc_psram(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char head[96];
        if (payload == NULL) {
            pim_db_collect_free(&collect);
            return 2;
        }
        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        snprintf(head, sizeof(head), "#%lu cat=%u key=%s\n",
                 (unsigned long)info.id, (unsigned)info.category, info.key);
        pim_doc_text(doc, head);
        export_doc_txt(doc, payload, len);
        pim_doc_text(doc, "\n\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    pim_db_collect_free(&collect);
    return 0;
}

/* ========================================================================
 * alarm rendering (collection via components/pim; ics via pim_alarms_render_ics)
 * ======================================================================== */

/* (db-style collection lives in components/pim: pim_alarms_collect.) */

static int export_alarms_csv(pim_doc_t *doc, int *rows_out)
{
    pim_alarm_collect_t collect;
    int rc;
    int i;

    rc = pim_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    pim_doc_text(doc, "id,when,title,msg,recur,flags\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char idbuf[16];
        char whenbuf[32];
        char recurbuf[8];
        char flagbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)e->id);
        pim_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(recurbuf, sizeof(recurbuf), "%u", (unsigned)e->recur);
        snprintf(flagbuf, sizeof(flagbuf), "%u", (unsigned)e->flags);
        export_doc_csv_field(doc, idbuf);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, whenbuf);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, e->title);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, e->msg);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, recurbuf);
        pim_doc_text(doc, ",");
        export_doc_csv_field(doc, flagbuf);
        pim_doc_text(doc, "\n");
        (*rows_out)++;
    }
    pim_alarms_collect_free(&collect);
    return 0;
}

static int export_alarms_json(pim_doc_t *doc, int *rows_out)
{
    pim_alarm_collect_t collect;
    int rc;
    int i;

    rc = pim_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    pim_doc_text(doc, "[\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char idbuf[16];
        char whenbuf[32];
        char unbuf[24];
        char recurbuf[8];
        char flagbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)e->id);
        pim_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(unbuf, sizeof(unbuf), "%lld", (long long)e->when);
        snprintf(recurbuf, sizeof(recurbuf), "%u", (unsigned)e->recur);
        snprintf(flagbuf, sizeof(flagbuf), "%u", (unsigned)e->flags);
        pim_doc_text(doc, "  {\"id\": ");
        pim_doc_text(doc, idbuf);
        pim_doc_text(doc, ", \"when\": ");
        export_doc_json_string(doc, whenbuf, strlen(whenbuf));
        pim_doc_text(doc, ", \"unix\": ");
        pim_doc_text(doc, unbuf);
        pim_doc_text(doc, ", \"title\": ");
        export_doc_json_string(doc, e->title, strlen(e->title));
        pim_doc_text(doc, ", \"msg\": ");
        export_doc_json_string(doc, e->msg, strlen(e->msg));
        pim_doc_text(doc, ", \"recur\": ");
        pim_doc_text(doc, recurbuf);
        pim_doc_text(doc, ", \"flags\": ");
        pim_doc_text(doc, flagbuf);
        pim_doc_text(doc, "}");
        pim_doc_text(doc, (i + 1 < collect.count) ? ",\n" : "\n");
        (*rows_out)++;
    }
    pim_doc_text(doc, "]\n");
    pim_alarms_collect_free(&collect);
    return 0;
}

static int export_alarms_txt(pim_doc_t *doc, int *rows_out)
{
    pim_alarm_collect_t collect;
    int rc;
    int i;

    rc = pim_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char head[128];
        char whenbuf[32];
        pim_when_text(e->when, whenbuf, sizeof(whenbuf));
        snprintf(head, sizeof(head), "#%lu %s\n%s\n", (unsigned long)e->id, whenbuf, e->title);
        pim_doc_text(doc, head);
        export_doc_txt(doc, e->msg, strlen(e->msg));
        pim_doc_text(doc, "\n\n");
        (*rows_out)++;
    }
    pim_alarms_collect_free(&collect);
    return 0;
}

/* (iCalendar rendering lives in components/pim: pim_alarms_render_ics.) */

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
    pim_doc_t doc;
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
    pim_doc_init(&doc);
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
            rc = pim_db_render_vcf(name, &doc, &rows);
        } else {
            rc = export_db_txt(name, &doc, &rows);
        }
    } else {
        if (strcasecmp(format, "csv") == 0) {
            rc = export_alarms_csv(&doc, &rows);
        } else if (strcasecmp(format, "json") == 0) {
            rc = export_alarms_json(&doc, &rows);
        } else if (strcasecmp(format, "ics") == 0) {
            rc = pim_alarms_render_ics(&doc, &rows);
        } else {
            rc = export_alarms_txt(&doc, &rows);
        }
    }
    if (rc != 0) {
        if (rc == 1) {
            shell_print_error("export: nothing to export");
        }
        pim_doc_free(&doc);
        return rc;
    }
    if (doc.overflow) {
        pim_doc_free(&doc);
        shell_print_error("export: output exceeds %d bytes", P4_CONFIG_DB_EXPORT_MAX_BYTES);
        return 1;
    }
    if (rows == 0) {
        pim_doc_free(&doc);
        shell_print_muted("export: store is empty");
        return 1;
    }
    if (storage_write_text_file(resolved, doc.data) != ESP_OK) {
        pim_doc_free(&doc);
        shell_print_error("export: cannot write %s", resolved);
        return 2;
    }
    pim_doc_free(&doc);
    shell_print_ok("export: %d row(s) -> %s (includes secret payloads)", rows, resolved);
    return 0;
}
