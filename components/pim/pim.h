/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim.h
 * @brief Personal information interchange: vCard/iCalendar codecs.
 *
 * A LEAF below storage (REQUIRES only db, alarm, and p4heap): it never
 * prints, never parses commands, and never touches the network. The
 * `import`/`export` verbs in components/command are thin shells over this
 * API (usage, paths, SD sessions, summaries); this component owns the single
 * vCard 3.0 parser/serializer and the single iCalendar VEVENT parser/
 * serializer, plus the shared PSRAM render buffer, record commit, and
 * collection helpers they all use. There is exactly one implementation of
 * each — never a second copy in the command layer.
 *
 * Contacts are `db` records with `k=v` payloads (fields name/tel/email/org/
 * title/note); events are `alarm` store entries. Every bulk buffer is
 * PSRAM-first (p4heap); no large stack locals.
 *
 * Sync identity (serial CardDAV-lite, `pim get`/`pim put`): contacts carry
 * `uid=` (stable cross-device id) and `mtime=` (Unix) payload fields; alarm
 * events carry `uid`/`modified` (see alarm.h). `pim get` backfills missing
 * identities once (mint + persist); `pim put` upserts by uid with
 * newer-wins. `import`/`export` never touch identity fields.
 */

#ifndef P4MINISHELL_PIM_H
#define P4MINISHELL_PIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "db.h"
#include "alarm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* vCard/vCal unfolded-line cap (long NOTEs truncate, never split). */
#define PIM_LINE_BYTES 1024

/* vCard NOTE field cap inside the built `k=v` payload. */
#define PIM_VCF_NOTE_BYTES 512

/* Incoming vCard/iCalendar UID cap (foreign UIDs truncate, never split). */
#define PIM_VCF_UID_BYTES 64

/* ========================================================================
 * Growable render buffer (PSRAM-first, capped)
 * ======================================================================== */

typedef struct {
    char *data;
    size_t used;
    size_t cap;
    bool overflow;
} pim_doc_t;

/**
 * Start a document (4 KB PSRAM seed, doubles on growth). A NULL seed sets
 * overflow immediately; every writer below no-ops once overflow is set.
 */
void pim_doc_init(pim_doc_t *doc);

/** Release the document buffer. */
void pim_doc_free(pim_doc_t *doc);

/** Append @p len bytes (no NUL added). Sets overflow past the export cap. */
void pim_doc_write(pim_doc_t *doc, const char *text, size_t len);

/** Append a NUL-terminated string. */
void pim_doc_text(pim_doc_t *doc, const char *text);

/** Append vCard/iCalendar-escaped text (`\\`, `\n`, `\,`, `\;`). */
void pim_doc_ical_text(pim_doc_t *doc, const char *text);

/** Append one `PROP:value` line with escaped text. */
void pim_doc_ical_prop(pim_doc_t *doc, const char *prop, const char *text);

/**
 * Bounded string copy with explicit truncation (never -Wformat-truncation).
 * NULL source clears the destination; NULL/zero destination is a no-op.
 */
void pim_copy_trunc(char *dst, size_t dst_size, const char *src);

/* ========================================================================
 * Shared line plumbing (vCard/iCalendar content lines)
 * ======================================================================== */

/** Read the next non-blank physical line (keeps the newline). False at EOF. */
bool pim_next_line(FILE *file, char *line, size_t size);

/** One unfolded logical line per call, plus one trailing pending line. */
typedef void (*pim_line_emit_t)(const char *line, void *ctx);

/**
 * vCard/vCal logical-line pump: unfolds continuation lines (leading SP/HTAB)
 * and calls @p emit once per complete line plus once for a trailing pending
 * line at EOF. Buffers are PSRAM; an allocation failure ends the stream
 * silently (the caller's counts stay zero).
 */
void pim_pump_unfolded(FILE *file, pim_line_emit_t emit, void *ctx);

/**
 * Split a vCard/iCalendar content line ("NAME;params:value") into the
 * property name (group prefix stripped, params dropped) and the raw value.
 * Pure, unit-tested.
 */
bool pim_vcf_prop_split(const char *line, char *name_out, size_t name_size,
                        const char **value_out);

/* ========================================================================
 * Record counts, commits, and collection helpers
 * ======================================================================== */

/** Import/merge counters shared by every interchange path. */
typedef struct {
    int ok;
    int skipped;
} pim_count_t;

/**
 * Commit one parsed db row (category-validated, key truncated). @return true
 * when the record landed; otherwise skipped++ and false.
 */
bool pim_db_commit(const char *name, long cat, const char *key,
                   const char *payload, bool secret, pim_count_t *count);

/**
 * Commit one parsed alarm row (notify action, ENABLED forced, FIRED
 * cleared). @return true when the event landed; otherwise skipped++ and
 * false.
 */
bool pim_alarm_commit(const char *title, const char *msg, time_t when,
                      long recur, long flags,
                      const alarm_recur_params_t *params, pim_count_t *count);

/** db collection for rendering (reuses db_find, like db_cmd_find). */
typedef struct {
    db_record_info_t *infos;
    int count;
} pim_db_collect_t;

/** db_find callback: keeps the first P4_CONFIG_DB_FIND_MAX records. */
bool pim_db_collect_cb(const db_record_info_t *info, void *ctx);

/**
 * Collect every record of @p name into @p collect (PSRAM).
 * @return 0 collected, 1 db missing, 2 I/O (caller frees nothing on error).
 */
int pim_db_collect(const char *name, pim_db_collect_t *collect);

/** Release a db collection. */
void pim_db_collect_free(pim_db_collect_t *collect);

/** alarm collection for rendering (reuses alarm_list). */
typedef struct {
    alarm_event_t *events;
    int count;
} pim_alarm_collect_t;

/** alarm_list callback: keeps the first P4_CONFIG_ALARM_MAX_EVENTS events. */
bool pim_alarm_collect_cb(const alarm_event_t *event, void *ctx);

/**
 * Collect every alarm into @p collect (PSRAM).
 * @return 0 collected, 2 I/O (caller frees nothing on error).
 */
int pim_alarms_collect(pim_alarm_collect_t *collect);

/** Release an alarm collection. */
void pim_alarms_collect_free(pim_alarm_collect_t *collect);

/** Render a timestamp as local "%Y-%m-%d %H:%M" (unix fallback). */
void pim_when_text(time_t when, char *out, size_t out_size);

/**
 * Parse an export `when` cell: unix digits, or "YYYY-MM-DD HH:MM[:SS]".
 * Pure (delegates date parsing to alarm_parse_datetime).
 */
bool pim_alarm_when(const char *text, time_t *out);

/* ========================================================================
 * vCard contact interchange (db records with k=v fields)
 * ======================================================================== */

/**
 * One parsed vCard contact with sync identity (superset of the import card:
 * same field caps, plus incoming `UID` and parsed `REV`). Passed by the
 * merge stream to the upsert path; `import` never fills uid/mtime.
 */
typedef struct {
    char name[64];
    char tel[128];
    char email[128];
    char org[64];
    char title[64];
    char note[PIM_VCF_NOTE_BYTES];
    /** Incoming `UID` value (truncated to fit), or empty when absent. */
    char uid[PIM_VCF_UID_BYTES];
    /** Incoming `REV` as Unix time, or 0 when absent/unparseable. */
    time_t mtime;
    bool active;
} pim_card_t;

/**
 * Parse vCard 3.0 contacts from an open stream into @p dbname
 * (fields name/tel/email/org/title/note; FN falls back to the record key,
 * NOTE to the whole payload; PHOTO/binary properties skipped; `;` in values
 * becomes `,` per the `k=v` convention; records capped by
 * P4_CONFIG_DB_EXPORT_MAX_RECORDS).
 *
 * @return 0 with @p count updated (ok/skipped). The caller owns the stream.
 */
int pim_vcf_parse_stream(FILE *fp, const char *dbname, pim_count_t *count);

/**
 * Assemble the `k=v` payload for a card. With @p uid NULL the payload is
 * exactly the historical import shape (no identity fields); the merge path
 * passes the stored/incoming uid plus mtime. Pure formatting helper shared
 * by the add and merge commits.
 */
void pim_vcf_build_payload(const pim_card_t *c, const char *uid,
                           time_t mtime, char *out, size_t size);

/**
 * Render every record of @p name as vCard 3.0 into @p doc.
 * @return 0 rendered (*rows_out set), 1 db missing, 2 I/O.
 */
int pim_db_render_vcf(const char *name, pim_doc_t *doc, int *rows_out);

/**
 * Sync variant of pim_db_render_vcf (backs `pim get db`): same cards plus
 * `UID` (stored `uid=`, every record backfilled first) and `REV` (from
 * `mtime=`). `export` keeps the plain variant, byte-identical.
 * @return 0 rendered (*rows_out set), 1 db missing, 2 I/O.
 */
int pim_db_render_vcf_uid(const char *name, pim_doc_t *doc, int *rows_out);

/* ========================================================================
 * iCalendar event interchange (alarm store)
 * ======================================================================== */

/**
 * Parse an iCalendar DATE-TIME ("YYYYMMDDTHHMMSS", date-only, trailing Z
 * accepted as device-local) into @p out with full range checks (no mktime,
 * so TZ-independent). Pure, unit-tested.
 */
bool pim_ics_datetime(const char *text, struct tm *out);

/**
 * Parse VEVENTs from an open stream into the alarm store (DTSTART floating
 * local, SUMMARY/DESCRIPTION, RRULE daily/weekly/monthly/yearly; unknown
 * frequencies degrade to one-shot; UID/DTSTAMP/DTEND/DURATION ignored;
 * events capped by P4_CONFIG_ALARM_MAX_EVENTS).
 *
 * @return 0 with @p count updated (ok/skipped). The caller owns the stream.
 */
int pim_ics_parse_stream(FILE *fp, pim_count_t *count);

/**
 * Render every alarm as VEVENTs into @p doc (floating local DTSTART, RRULE
 * for daily/weekly/monthly/yearly, UID minted as "<id>@p4minishell").
 * @return 0 rendered (*rows_out set), 2 I/O.
 */
int pim_alarms_render_ics(pim_doc_t *doc, int *rows_out);

/**
 * Sync variant of pim_alarms_render_ics (backs `pim get alarms`): UID is the
 * stored event uid (every event backfilled first) and DTSTAMP the stored
 * `modified`. `export` keeps the plain variant, byte-identical.
 * @return 0 rendered (*rows_out set), 2 I/O.
 */
int pim_alarms_render_ics_uid(pim_doc_t *doc, int *rows_out);

/* ========================================================================
 * Sync identity and merge (newer-wins by uid)
 * ======================================================================== */

/**
 * Mint a 128-bit random hex sync identity (32 chars + NUL) via esp_random.
 * Pure (no SD); unit-tested for shape.
 */
void pim_mint_uid(char *out, size_t out_size);

/**
 * Newer-wins decision: true when @p incoming should replace @p existing.
 * A missing incoming timestamp (<= 0) never overwrites a timestamped
 * record; two untimestamped records replace (first sync convergence).
 * Pure, unit-tested.
 */
bool pim_merge_should_replace(time_t incoming, time_t existing);

/**
 * Find a db record by its sync `uid=` payload field (linear scan over the
 * collected infos + one payload read each). @return true with @p info_out
 * and @p mtime_out (stored `mtime=`, 0 when absent) on match.
 */
bool pim_db_find_uid(const char *name, const char *uid,
                     db_record_info_t *info_out, time_t *mtime_out);

/**
 * Replace a record's payload in place, preserving its category, key, and
 * secret flag. @return true with count->ok++, else count->skipped++.
 */
bool pim_db_replace(const char *name, const db_record_info_t *info,
                    const char *key, const char *payload, pim_count_t *count);

/**
 * Merge one parsed vCard into @p dbname by uid: create via the existing
 * add path when unknown (incoming uid stored, or a fresh mint when the
 * card carries none), `pim_db_replace` when the incoming REV is newer,
 * skip otherwise. @return true with @p count updated.
 */
bool pim_db_upsert(const char *name, const pim_card_t *card,
                   pim_count_t *count);

/**
 * Merge one parsed VEVENT into the alarm store by uid: create via the
 * existing add path (then store the incoming uid/modified with
 * `alarm_set`) when unknown; content-update via `alarm_set` (existing
 * flags preserved) when the incoming DTSTAMP is newer; skip otherwise.
 * @return true with @p count updated.
 */
bool pim_alarms_upsert(const char *title, const char *msg, time_t when,
                       long recur, const alarm_recur_params_t *params,
                       const char *uid, time_t modified, pim_count_t *count);

/**
 * Backfill sync identity for every record of @p name lacking `uid=`
 * (mint + persist with `mtime`=now). One-time per record; the `pim get`
 * side effect that makes the next render's UIDs stable.
 * @return 0 with @p backfilled_out set, 1 db missing, 2 I/O.
 */
int pim_db_backfill(const char *name, int *backfilled_out);

/**
 * Backfill sync identity for every alarm lacking `uid` (mint + `alarm_set`
 * with `modified`=now).
 * @return 0 with @p backfilled_out set, 2 I/O.
 */
int pim_alarms_backfill(int *backfilled_out);

/**
 * Bump the `mtime=` identity stamp of a pim contact payload to @p now, in
 * place. Only payloads that already carry pim identity (the same `uid=`
 * predicate the sync membership test uses) are touched: an existing
 * `mtime=` value span is replaced, otherwise `;mtime=<now>` is appended
 * when @p cap allows. CSV grids, JSON, and plain notes never carry `uid=`
 * and pass through byte-identical, so `edit db` can never corrupt them
 * here. Pure (no SD); unit-tested incl. the CSV pass-through.
 * @return true when the buffer changed.
 */
bool pim_payload_bump_mtime(char *buf, size_t cap, time_t now);

/**
 * Merge-mode vCard stream (backs `pim put db`): same parser as
 * pim_vcf_parse_stream, but every complete card goes through
 * pim_db_upsert instead of a fresh add. `import` keeps the add variant.
 * @return 0 with @p count updated (ok/skipped). The caller owns the stream.
 */
int pim_vcf_merge_stream(FILE *fp, const char *dbname, pim_count_t *count);

/**
 * Merge-mode VEVENT stream (backs `pim put alarms`): same parser as
 * pim_ics_parse_stream, but every complete event goes through
 * pim_alarms_upsert instead of a fresh add. `import` keeps the add variant.
 * @return 0 with @p count updated (ok/skipped). The caller owns the stream.
 */
int pim_ics_merge_stream(FILE *fp, pim_count_t *count);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_PIM_H */
