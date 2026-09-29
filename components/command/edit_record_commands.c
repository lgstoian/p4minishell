/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file edit_record_commands.c
 * @brief `edit db` / `edit alarm` — record editing through the text editor.
 *
 * Record payloads are staged byte-verbatim into a temp `.txt` file (the
 * filetype registry yields TEXT, so the editor renders PLAIN): CSV rows,
 * `k=v` contact payloads, JSON, and alarm INI text all appear as plain
 * text lines, and the byte-preserving document model returns them
 * byte-identical when untouched. The editor has no CSV grid mode; the
 * `csv` verbs keep operating on files separately.
 *
 * Save-back goes through the store APIs, never around them:
 *   - `edit db <name> <id>`: db_get (secret gate mirrors `export db` /
 *     `pim get db`) -> edit -> db_set echoing category/key/secret exactly.
 *     Payloads are opaque bytes, so CSV grids survive untouched; when the
 *     payload carries pim identity, `mtime=` is bumped (pim newer-wins
 *     protects the device-side edit), otherwise the buffer is written back
 *     byte-identical to what the editor returned.
 *   - `edit alarm <id>`: alarm_get -> alarm_event_render -> edit ->
 *     alarm_event_parse (same field semantics as the file loader) ->
 *     alarm_set. A stored `uid` is sync identity, not content, so it
 *     survives the edit even if the `uid=` line was touched; `modified`
 *     is bumped whenever the surviving event carries a uid.
 *
 * Byte-identical round-trips skip the store write entirely (no timestamp
 * churn). Any failure after the editor ran keeps the temp file and prints
 * its path, so edited text is never silently lost. Record payloads are
 * text; binary payloads are out of scope for a text editor.
 *
 * Batch-friendly: ERRORLEVEL 0 saved/unchanged, 1 not-found/locked/failed,
 * 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "command.h"
#include "db.h"
#include "editor.h"
#include "pim.h"
#include "security_commands.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"

static void shell_command_edit_db_usage(void)
{
    shell_print_usage("Usage: edit db <name> <id> [/focus] [/template <name>]");
}

static void shell_command_edit_alarm_usage(void)
{
    shell_print_usage("Usage: edit alarm <id> [/focus] [/template <name>]");
}

/** Parse a monotonic decimal record id (digits only, 1..UINT32_MAX). */
static bool edit_parse_id(const char *arg, uint32_t *out)
{
    const char *c;
    char *end = NULL;
    unsigned long v;

    if (arg == NULL || arg[0] == '\0' || out == NULL) {
        return false;
    }
    for (c = arg; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
    }
    v = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || v == 0 || v > (unsigned long)UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/**
 * Stage @p len bytes of @p text into a temp `.txt` file for the editor.
 * @return 0 with @p path_out filled, 1 with the failure already reported
 * (no temp file exists on failure paths).
 */
static int edit_stage_temp(const char *text, size_t len,
                           char *path_out, size_t path_size)
{
    shell_sd_session_t session;
    FILE *f = NULL;

    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("edit: SD card not present - insert and retry");
        return 1;
    }
    if (storage_temp_path(path_out, path_size, "txt") != ESP_OK) {
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot create temp file");
        return 1;
    }
    f = fopen(path_out, "wb");
    if (f == NULL) {
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot create %s", path_out);
        return 1;
    }
    if (len > 0 && fwrite(text, 1, len, f) != len) {
        fclose(f);
        remove(path_out);
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot stage %s", path_out);
        return 1;
    }
    fclose(f);
    shell_sd_end(&session, "edit");
    return 0;
}

/**
 * Read the edited temp file back (up to @p cap bytes). Oversize or I/O
 * failure keeps the temp file and reports its path; success leaves removal
 * to the caller.
 * @return 0 with @p len_out set, 1 with the failure already reported.
 */
static int edit_readback_temp(const char *tmp_path, char *buf, size_t cap,
                              size_t *len_out)
{
    shell_sd_session_t session;
    FILE *f = NULL;
    long size = 0;

    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("edit: SD card not present - insert and retry - text kept in %s",
                          tmp_path);
        return 1;
    }
    f = fopen(tmp_path, "rb");
    if (f == NULL) {
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot re-read %s - text kept in %s",
                          tmp_path, tmp_path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot re-read %s - text kept in %s",
                          tmp_path, tmp_path);
        return 1;
    }
    size = ftell(f);
    if (size < 0 || (size_t)size > cap) {
        fclose(f);
        shell_sd_end(&session, "edit");
        shell_print_error("edit: text exceeds %u bytes - not saved, text kept in %s",
                          (unsigned)cap, tmp_path);
        return 1;
    }
    rewind(f);
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        shell_sd_end(&session, "edit");
        shell_print_error("edit: cannot re-read %s - text kept in %s",
                          tmp_path, tmp_path);
        return 1;
    }
    fclose(f);
    shell_sd_end(&session, "edit");
    buf[size] = '\0';
    *len_out = (size_t)size;
    return 0;
}

/** Run the editor session; @return the session errorlevel (0 = clean exit). */
static int edit_run_editor(const char *tmp_path, bool focus,
                           const char *template_name)
{
    editor_session_opts_t opts;
    int errorlevel = 0;

    opts.focus = focus;
    opts.template_name = template_name;
    /* The editor runs on the command worker task; it blocks until quit. */
    if (editor_session_run_opts(tmp_path, &opts, &errorlevel) != ESP_OK &&
        errorlevel == 0) {
        errorlevel = 1;
    }
    return errorlevel;
}

int shell_command_edit_db(const char *dbname, const char *id_arg, bool focus,
                          const char *template_name)
{
    db_record_info_t info;
    uint32_t id = 0;
    char *payload = NULL;
    char *edited = NULL;
    char tmp_path[P4_CONFIG_SD_PATH_BYTES];
    size_t len = 0;
    size_t edited_len = 0;
    bool secret = false;
    bool bumped = false;
    int ed = 0;
    esp_err_t error;

    if (dbname == NULL || dbname[0] == '\0' || !edit_parse_id(id_arg, &id)) {
        shell_command_edit_db_usage();
        return 2;
    }
    /* Metadata probe: NULL buffer reads info only (a secret payload
     * reports size 0 until revealed). */
    error = db_get(dbname, id, NULL, &len, false, &info);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("edit: record %lu not found in %s",
                          (unsigned long)id, dbname);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("edit: cannot read %s (%s)",
                          dbname, esp_err_to_name(error));
        return 2;
    }
    secret = (info.flags & DB_FLAG_SECRET) != 0;
    if (secret && !security_can_reveal_private()) {
        shell_print_error("edit: device locked - unlock before editing secret records");
        return 1;
    }
    /* Record-sized buffer: heap-allocate (this runs on the batch path). */
    payload = malloc((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (payload == NULL) {
        shell_print_error("edit: out of memory");
        return 2;
    }
    len = (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES;
    error = db_get(dbname, id, payload, &len, true, NULL);
    if (error != ESP_OK) {
        free(payload);
        shell_print_error("edit: cannot read record %lu in %s (%s)",
                          (unsigned long)id, dbname, esp_err_to_name(error));
        return 1;
    }
    payload[len] = '\0';
    if (edit_stage_temp(payload, len, tmp_path, sizeof(tmp_path)) != 0) {
        free(payload);
        return 1;
    }
    ed = edit_run_editor(tmp_path, focus, template_name);
    if (ed != 0) {
        free(payload);
        shell_print_error("edit: editor exited with errorlevel %d - text kept in %s",
                          ed, tmp_path);
        return ed;
    }
    edited = malloc((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (edited == NULL) {
        free(payload);
        remove(tmp_path);
        shell_print_error("edit: out of memory");
        return 2;
    }
    if (edit_readback_temp(tmp_path, edited,
                           (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES,
                           &edited_len) != 0) {
        free(payload);
        free(edited);
        return 1;
    }
    if (edited_len == len && memcmp(edited, payload, len) == 0) {
        /* Byte-identical: no store write, no timestamp churn. */
        free(payload);
        free(edited);
        remove(tmp_path);
        shell_print_muted("edit: unchanged - record %lu in %s",
                          (unsigned long)id, dbname);
        return 0;
    }
    /* Pim identity: bump `mtime=` so newer-wins protects the device-side
     * edit. CSV grids and plain notes never carry `uid=` and pass through
     * byte-identical (pim_payload_bump_mtime contract). */
    bumped = pim_payload_bump_mtime(edited,
                                    (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1,
                                    time(NULL));
    edited_len = strlen(edited);
    error = db_set(dbname, id, info.category, info.key, secret,
                   edited, edited_len);
    free(payload);
    free(edited);
    if (error != ESP_OK) {
        shell_print_error("edit: cannot save record %lu in %s (%s) - text kept in %s",
                          (unsigned long)id, dbname,
                          esp_err_to_name(error), tmp_path);
        return 1;
    }
    remove(tmp_path);
    shell_print_ok("edit: saved record %lu in %s (%u bytes%s)",
                   (unsigned long)id, dbname, (unsigned)edited_len,
                   bumped ? ", sync stamp updated" : "");
    return 0;
}

int shell_command_edit_alarm(const char *id_arg, bool focus,
                             const char *template_name)
{
    alarm_event_t ev;
    alarm_event_t updated;
    uint32_t id = 0;
    char *rendered = NULL;
    char *edited = NULL;
    char tmp_path[P4_CONFIG_SD_PATH_BYTES];
    size_t rlen = 0;
    size_t edited_len = 0;
    int ed = 0;
    esp_err_t error;

    if (!edit_parse_id(id_arg, &id)) {
        shell_command_edit_alarm_usage();
        return 2;
    }
    error = alarm_get(id, &ev);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("edit: alarm %lu not found", (unsigned long)id);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("edit: cannot read alarms (%s)",
                          esp_err_to_name(error));
        return 2;
    }
    rendered = malloc(ALARM_EVENT_TEXT_BYTES);
    if (rendered == NULL) {
        shell_print_error("edit: out of memory");
        return 2;
    }
    if (alarm_event_render(&ev, rendered, ALARM_EVENT_TEXT_BYTES) != ESP_OK) {
        free(rendered);
        shell_print_error("edit: cannot render alarm %lu", (unsigned long)id);
        return 2;
    }
    rlen = strlen(rendered);
    if (edit_stage_temp(rendered, rlen, tmp_path, sizeof(tmp_path)) != 0) {
        free(rendered);
        return 1;
    }
    ed = edit_run_editor(tmp_path, focus, template_name);
    if (ed != 0) {
        free(rendered);
        shell_print_error("edit: editor exited with errorlevel %d - text kept in %s",
                          ed, tmp_path);
        return ed;
    }
    edited = malloc((size_t)P4_CONFIG_INI_MAX_BYTES);
    if (edited == NULL) {
        free(rendered);
        remove(tmp_path);
        shell_print_error("edit: out of memory");
        return 2;
    }
    if (edit_readback_temp(tmp_path, edited,
                           (size_t)P4_CONFIG_INI_MAX_BYTES - 1,
                           &edited_len) != 0) {
        free(rendered);
        free(edited);
        return 1;
    }
    if (edited_len == rlen && memcmp(edited, rendered, rlen) == 0) {
        /* Byte-identical: no store write, no timestamp churn. */
        free(rendered);
        free(edited);
        remove(tmp_path);
        shell_print_muted("edit: unchanged - alarm %lu", (unsigned long)id);
        return 0;
    }
    if (alarm_event_parse(edited, id, &updated) != ESP_OK) {
        free(rendered);
        free(edited);
        shell_print_error("edit: cannot parse event (check the 'when=' line) - text kept in %s",
                          tmp_path);
        return 1;
    }
    /* Sync identity is a primary key, not content: a stored uid survives
     * the edit even when the `uid=` line was touched; a first-time uid
     * from the text is kept. */
    if (ev.uid[0] != '\0') {
        memcpy(updated.uid, ev.uid, sizeof(updated.uid));
    }
    if (updated.uid[0] != '\0') {
        updated.modified = time(NULL);
    }
    free(rendered);
    free(edited);
    error = alarm_set(id, &updated);
    if (error != ESP_OK) {
        shell_print_error("edit: cannot save alarm %lu (%s) - text kept in %s",
                          (unsigned long)id, esp_err_to_name(error),
                          tmp_path);
        return 1;
    }
    remove(tmp_path);
    shell_print_ok("edit: saved alarm %lu%s", (unsigned long)id,
                   updated.uid[0] != '\0' ? " (sync stamp updated)" : "");
    return 0;
}
