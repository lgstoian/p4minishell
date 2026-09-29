/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_vcard.c
 * @brief vCard 3.0 contact interchange over db `k=v` records.
 *
 * Parse half moved out of components/command/import_commands.c, render half
 * moved out of components/command/export_commands.c: the single vCard
 * reader (fields name/tel/email/org/title/note; FN falls back to the record
 * key, NOTE to the whole payload; `;` in values becomes `,` per the `k=v`
 * convention; PHOTO/binary properties skipped) and the single vCard writer.
 * Record payload reads are PSRAM; the per-card assembly scratch is small
 * bounded stack, as before.
 */

#include "pim.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef P4_CONFIG_DB_RECORD_MAX_BYTES
#define P4_CONFIG_DB_RECORD_MAX_BYTES 4096
#endif

#ifndef P4_CONFIG_DB_KEY_BYTES
#define P4_CONFIG_DB_KEY_BYTES 32
#endif

#ifndef P4_CONFIG_DB_EXPORT_MAX_RECORDS
#define P4_CONFIG_DB_EXPORT_MAX_RECORDS 256
#endif

/* ========================================================================
 * vCard contact import
 * ======================================================================== */

static void pim_vcard_reset(pim_card_t *c)
{
    memset(c, 0, sizeof(*c));
    c->active = true;
}

/** Parse a vCard REV value (iCalendar-style date-time) into Unix time. */
static time_t pim_vcf_rev_time(const char *rev)
{
    struct tm tmv;
    time_t out;

    if (rev == NULL || rev[0] == '\0') {
        return 0;
    }
    if (!pim_ics_datetime(rev, &tmv)) {
        return 0;
    }
    out = mktime(&tmv);
    return (out != (time_t)-1) ? out : 0;
}

/**
 * Assemble the `k=v` payload for a card. With @p uid NULL the payload is
 * exactly the historical import shape (no identity fields); the merge path
 * passes the stored/incoming uid plus mtime.
 */
void pim_vcf_build_payload(const pim_card_t *c, const char *uid,
                           time_t mtime, char *payload, size_t size)
{
    const char *key = c->name[0] != '\0' ? c->name : "contact";
    size_t used = 0;
    int n;

    payload[0] = '\0';
    n = snprintf(payload, size, "name=%s", key);
    if (n > 0) {
        used = (size_t)n;
    }
#define PIM_VCF_FIELD(f, v) do { \
        if ((v)[0] != '\0' && used + 1 < size) { \
            int m = snprintf(payload + used, size - used, ";" #f "=%s", (v)); \
            if (m > 0) { used += (size_t)m; } \
        } \
    } while (0)
    PIM_VCF_FIELD(tel, c->tel);
    PIM_VCF_FIELD(email, c->email);
    PIM_VCF_FIELD(org, c->org);
    PIM_VCF_FIELD(title, c->title);
    PIM_VCF_FIELD(note, c->note);
    if (uid != NULL && uid[0] != '\0') {
        PIM_VCF_FIELD(uid, uid);
        if (mtime > 0 && used + 1 < size) {
            int m = snprintf(payload + used, size - used, ";mtime=%lld",
                             (long long)mtime);
            if (m > 0) {
                used += (size_t)m;
            }
        }
    }
#undef PIM_VCF_FIELD
}

/** Unescape a vCard value in place (`\\` `\,` `\;` `\n`, then `;` -> `,`). */
static void pim_vcf_unescape(char *text)
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
static void pim_vcf_join(char *field, size_t size, const char *value)
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
static void pim_vcf_name_from_n(const char *value, char *out, size_t out_size)
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
        pim_copy_trunc(out, out_size, value);
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
        pim_copy_trunc(out + pos, out_size - pos, given);
        pos = strlen(out);
        if (family[0] != '\0' && pos + 1 < out_size) {
            out[pos++] = ' ';
            out[pos] = '\0';
        }
    }
    if (family[0] != '\0') {
        pim_copy_trunc(out + pos, out_size - pos, family);
    }
}

static void pim_vcf_commit(const char *name, const pim_card_t *c, pim_count_t *count)
{
    char payload[P4_CONFIG_DB_KEY_BYTES + 128 + 128 + 64 + 64 + PIM_VCF_NOTE_BYTES + 192];
    const char *key = c->name[0] != '\0' ? c->name : "contact";

    /* uid NULL: historical import shape, byte-identical. */
    pim_vcf_build_payload(c, NULL, 0, payload, sizeof(payload));
    pim_db_commit(name, 0, key, payload, false, count);
}

typedef struct {
    pim_card_t card;
    bool in_card;
    bool merge;
    int contact_no;
    const char *dbname;
    pim_count_t *count;
} pim_vcf_ctx_t;

/** Handle one complete (unfolded) vCard content line. */
static void pim_vcf_line(const char *current, void *vctx)
{
    pim_vcf_ctx_t *ctx = (pim_vcf_ctx_t *)vctx;
    char prop[32];
    const char *value = NULL;

    if (ctx == NULL) {
        return;
    }
    if (!pim_vcf_prop_split(current, prop, sizeof(prop), &value)) {
        return;
    }
    if (strcasecmp(prop, "BEGIN") == 0) {
        if (strcasecmp(value, "VCARD") == 0) {
            pim_vcard_reset(&ctx->card);
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
                if (ctx->merge) {
                    (void)pim_db_upsert(ctx->dbname, &ctx->card, ctx->count);
                } else {
                    pim_vcf_commit(ctx->dbname, &ctx->card, ctx->count);
                }
            }
            ctx->in_card = false;
        }
        return;
    }
    {
        char decoded[PIM_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        pim_vcf_unescape(decoded);
        if (strcasecmp(prop, "FN") == 0) {
            pim_copy_trunc(ctx->card.name, sizeof(ctx->card.name), decoded);
        } else if (strcasecmp(prop, "N") == 0) {
            if (ctx->card.name[0] == '\0') {
                pim_vcf_name_from_n(decoded, ctx->card.name, sizeof(ctx->card.name));
            }
        } else if (strcasecmp(prop, "TEL") == 0) {
            pim_vcf_join(ctx->card.tel, sizeof(ctx->card.tel), decoded);
        } else if (strcasecmp(prop, "EMAIL") == 0) {
            pim_vcf_join(ctx->card.email, sizeof(ctx->card.email), decoded);
        } else if (strcasecmp(prop, "ORG") == 0) {
            if (ctx->card.org[0] == '\0') {
                pim_copy_trunc(ctx->card.org, sizeof(ctx->card.org), decoded);
            }
        } else if (strcasecmp(prop, "TITLE") == 0) {
            if (ctx->card.title[0] == '\0') {
                pim_copy_trunc(ctx->card.title, sizeof(ctx->card.title), decoded);
            }
        } else if (strcasecmp(prop, "NOTE") == 0) {
            if (ctx->card.note[0] == '\0') {
                pim_copy_trunc(ctx->card.note, sizeof(ctx->card.note), decoded);
            }
        } else if (strcasecmp(prop, "UID") == 0) {
            /* Captured for the merge path; the add path ignores it. */
            if (ctx->card.uid[0] == '\0') {
                pim_copy_trunc(ctx->card.uid, sizeof(ctx->card.uid), decoded);
            }
        } else if (strcasecmp(prop, "REV") == 0) {
            if (ctx->card.mtime == 0) {
                ctx->card.mtime = pim_vcf_rev_time(decoded);
            }
        }
        /* PHOTO/LOGO/KEY/SOUND/AGENT/PRODID/VERSION/... skipped. */
    }
}

int pim_vcf_parse_stream(FILE *fp, const char *dbname, pim_count_t *count)
{
    pim_vcf_ctx_t ctx;

    if (fp == NULL || dbname == NULL || count == NULL) {
        return 2;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.dbname = dbname;
    ctx.count = count;
    ctx.merge = false;
    pim_pump_unfolded(fp, pim_vcf_line, &ctx);
    return 0;
}

int pim_vcf_merge_stream(FILE *fp, const char *dbname, pim_count_t *count)
{
    pim_vcf_ctx_t ctx;

    if (fp == NULL || dbname == NULL || count == NULL) {
        return 2;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.dbname = dbname;
    ctx.count = count;
    ctx.merge = true;
    pim_pump_unfolded(fp, pim_vcf_line, &ctx);
    return 0;
}

/* ========================================================================
 * vCard contact rendering (db records with k=v fields)
 * ======================================================================== */

/** Render a Unix time as vCard REV "YYYYMMDDTHHMMSS" (device-local). */
static void pim_rev_text(time_t when, char *out, size_t out_size)
{
    struct tm tmv;
    if (localtime_r(&when, &tmv) != NULL &&
        strftime(out, out_size, "%Y%m%dT%H%M%S", &tmv) != 0) {
        return;
    }
    out[0] = '\0';
}

static int pim_db_render_vcf_ex(const char *name, pim_doc_t *doc,
                                int *rows_out, bool identity)
{
    pim_db_collect_t collect;
    int rc;
    int i;

    if (name == NULL || doc == NULL || rows_out == NULL) {
        return 2;
    }
    rc = pim_db_collect(name, &collect);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < collect.count; i++) {
        char *payload = p4heap_alloc_psram(P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
        size_t len = P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;
        char fn[64];
        char tel[128];
        char email[128];
        char org[64];
        char title[64];
        char note[512];
        if (payload == NULL) {
            pim_db_collect_free(&collect);
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
            pim_copy_trunc(fn, sizeof(fn), info.key);
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
            pim_copy_trunc(note, sizeof(note), payload);
        }
        pim_doc_text(doc, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
        pim_doc_ical_prop(doc, "N", fn);
        pim_doc_ical_prop(doc, "FN", fn);
        if (identity) {
            /* Stored uid (backfilled by `pim get`); derived fallback only
             * for a record added between backfill and render. */
            char uid[PIM_VCF_UID_BYTES];
            char mbuf[24];
            char rev[32];
            if (!db_field_get(payload, "uid", uid, sizeof(uid)) || uid[0] == '\0') {
                snprintf(uid, sizeof(uid), "%lu@p4minishell",
                         (unsigned long)info.id);
            }
            pim_doc_ical_prop(doc, "UID", uid);
            if (db_field_get(payload, "mtime", mbuf, sizeof(mbuf))) {
                long long mt = atoll(mbuf);
                if (mt > 0) {
                    pim_rev_text((time_t)mt, rev, sizeof(rev));
                    if (rev[0] != '\0') {
                        pim_doc_ical_prop(doc, "REV", rev);
                    }
                }
            }
        }
        if (tel[0] != '\0') {
            pim_doc_ical_prop(doc, "TEL", tel);
        }
        if (email[0] != '\0') {
            pim_doc_ical_prop(doc, "EMAIL", email);
        }
        if (org[0] != '\0') {
            pim_doc_ical_prop(doc, "ORG", org);
        }
        if (title[0] != '\0') {
            pim_doc_ical_prop(doc, "TITLE", title);
        }
        if (note[0] != '\0') {
            pim_doc_ical_prop(doc, "NOTE", note);
        }
        pim_doc_text(doc, "END:VCARD\r\n");
        heap_caps_free(payload);
        (*rows_out)++;
    }
    pim_db_collect_free(&collect);
    return 0;
}

int pim_db_render_vcf(const char *name, pim_doc_t *doc, int *rows_out)
{
    /* identity=false: historical export shape, byte-identical. */
    return pim_db_render_vcf_ex(name, doc, rows_out, false);
}

int pim_db_render_vcf_uid(const char *name, pim_doc_t *doc, int *rows_out)
{
    return pim_db_render_vcf_ex(name, doc, rows_out, true);
}
