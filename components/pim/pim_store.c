/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_store.c
 * @brief Record commits and collection helpers over db/alarm.
 *
 * Moved out of components/command/import_commands.c (the commit and
 * when-cell parsers) and components/command/export_commands.c (the db/alarm
 * collection prologues): the single commit implementation behind the csv,
 * json, vcf, and ics interchange paths, and the single collection behind
 * every rendering path. Collect buffers are PSRAM.
 */

#include "pim.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_random.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef P4_CONFIG_DB_RECORD_MAX_BYTES
#define P4_CONFIG_DB_RECORD_MAX_BYTES 4096
#endif

#ifndef P4_CONFIG_DB_FIND_MAX
#define P4_CONFIG_DB_FIND_MAX 64
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

bool pim_db_commit(const char *name, long cat, const char *key,
                   const char *payload, bool secret, pim_count_t *count)
{
    char keybuf[P4_CONFIG_DB_KEY_BYTES];
    uint32_t id = 0;

    if (cat < 0 || cat >= P4_CONFIG_DB_CATEGORY_COUNT) {
        count->skipped++;
        return false;
    }
    pim_copy_trunc(keybuf, sizeof(keybuf), key);
    if (db_add(name, (uint8_t)cat, keybuf, secret,
               payload != NULL ? payload : "", payload != NULL ? strlen(payload) : 0,
               &id) != ESP_OK) {
        count->skipped++;
        return false;
    }
    count->ok++;
    return true;
}

static bool pim_alarm_commit_core(const char *title, const char *msg, time_t when,
                                            long recur, long flags,
                                            const alarm_recur_params_t *params,
                                            pim_count_t *count, uint32_t *out_id)
{
    char t[P4_CONFIG_ALARM_TITLE_BYTES];
    char m[P4_CONFIG_ALARM_MSG_BYTES];
    uint8_t action = ALARM_ACTION_NOTIFY;
    uint8_t fl;
    uint32_t id = 0;

    pim_copy_trunc(t, sizeof(t), title != NULL && title[0] != '\0' ? title : "Alarm");
    pim_copy_trunc(m, sizeof(m), msg);
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
    if (out_id != NULL) {
        *out_id = id;
    }
    count->ok++;
    return true;
}

bool pim_alarm_commit(const char *title, const char *msg, time_t when,
                      long recur, long flags,
                      const alarm_recur_params_t *params, pim_count_t *count)
{
    /* out_id NULL: historical import shape, behavior identical. */
    return pim_alarm_commit_core(title, msg, when, recur, flags, params,
                                 count, NULL);
}

bool pim_db_collect_cb(const db_record_info_t *info, void *ctx)
{
    pim_db_collect_t *c = (pim_db_collect_t *)ctx;
    if (c->count >= P4_CONFIG_DB_FIND_MAX) {
        return false;
    }
    c->infos[c->count++] = *info;
    return true;
}

int pim_db_collect(const char *name, pim_db_collect_t *collect)
{
    int out_count = 0;
    esp_err_t error;

    collect->infos = p4heap_alloc_psram((size_t)P4_CONFIG_DB_FIND_MAX * sizeof(db_record_info_t));
    if (collect->infos == NULL) {
        collect->count = 0;
        return 2;
    }
    collect->count = 0;
    error = db_find(name, 0xFF, NULL, NULL, false, true,
                    pim_db_collect_cb, collect, &out_count);
    if (error == ESP_ERR_NOT_FOUND) {
        heap_caps_free(collect->infos);
        collect->infos = NULL;
        return 1;
    }
    if (error != ESP_OK) {
        heap_caps_free(collect->infos);
        collect->infos = NULL;
        return 2;
    }
    return 0;
}

void pim_db_collect_free(pim_db_collect_t *collect)
{
    if (collect->infos != NULL) {
        heap_caps_free(collect->infos);
        collect->infos = NULL;
    }
    collect->count = 0;
}

bool pim_alarm_collect_cb(const alarm_event_t *event, void *ctx)
{
    pim_alarm_collect_t *c = (pim_alarm_collect_t *)ctx;
    if (c->count >= P4_CONFIG_ALARM_MAX_EVENTS) {
        return false;
    }
    c->events[c->count++] = *event;
    return true;
}

int pim_alarms_collect(pim_alarm_collect_t *collect)
{
    esp_err_t error;

    collect->events = p4heap_alloc_psram((size_t)P4_CONFIG_ALARM_MAX_EVENTS * sizeof(alarm_event_t));
    if (collect->events == NULL) {
        collect->count = 0;
        return 2;
    }
    collect->count = 0;
    error = alarm_list(pim_alarm_collect_cb, collect);
    if (error != ESP_OK) {
        heap_caps_free(collect->events);
        collect->events = NULL;
        return 2;
    }
    return 0;
}

void pim_alarms_collect_free(pim_alarm_collect_t *collect)
{
    if (collect->events != NULL) {
        heap_caps_free(collect->events);
        collect->events = NULL;
    }
    collect->count = 0;
}

void pim_when_text(time_t when, char *out, size_t out_size)
{
    struct tm tmv;
    if (localtime_r(&when, &tmv) != NULL &&
        strftime(out, out_size, "%Y-%m-%d %H:%M", &tmv) != 0) {
        return;
    }
    snprintf(out, out_size, "%lld", (long long)when);
}

bool pim_alarm_when(const char *text, time_t *out)
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

/* ========================================================================
 * Sync identity and merge (newer-wins by uid)
 *
 * Contacts carry `uid=` (stable cross-device id) and `mtime=` (Unix)
 * payload fields; alarm events carry `uid`/`modified` (see alarm.h).
 * `import`/`export` never read or write these fields; only the merge
 * streams and the `pim get` backfill below touch them.
 * ======================================================================== */

void pim_mint_uid(char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t w = 0;
    size_t i;
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    /* 32 hex chars (128-bit); a short buffer truncates the tail. */
    for (i = 0; i < 32 && pos + 1 < out_size; i++) {
        if ((i % 8) == 0) {
            w = esp_random();
        } else {
            w >>= 4;
        }
        out[pos++] = hex[w & 0x0Fu];
    }
    out[pos] = '\0';
}

bool pim_merge_should_replace(time_t incoming, time_t existing)
{
    if (incoming <= 0) {
        /* A missing incoming timestamp never overwrites a timestamped
         * record; two untimestamped records replace (first-sync). */
        return existing <= 0;
    }
    if (existing <= 0) {
        return true;
    }
    return incoming >= existing;
}

bool pim_db_find_uid(const char *name, const char *uid,
                     db_record_info_t *info_out, time_t *mtime_out)
{
    pim_db_collect_t collect;
    char *payload = NULL;
    int i;

    if (name == NULL || uid == NULL || uid[0] == '\0') {
        return false;
    }
    if (pim_db_collect(name, &collect) != 0) {
        return false;
    }
    payload = p4heap_alloc_psram((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (payload == NULL) {
        pim_db_collect_free(&collect);
        return false;
    }
    for (i = 0; i < collect.count; i++) {
        char found[PIM_VCF_UID_BYTES];
        size_t len = (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES;
        db_record_info_t info;

        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            continue;
        }
        payload[len] = '\0';
        if (db_field_get(payload, "uid", found, sizeof(found)) &&
            strcmp(found, uid) == 0) {
            if (info_out != NULL) {
                *info_out = info;
            }
            if (mtime_out != NULL) {
                char mbuf[24];
                long long mt = 0;
                if (db_field_get(payload, "mtime", mbuf, sizeof(mbuf))) {
                    mt = atoll(mbuf);
                }
                *mtime_out = (mt > 0) ? (time_t)mt : 0;
            }
            heap_caps_free(payload);
            pim_db_collect_free(&collect);
            return true;
        }
    }
    heap_caps_free(payload);
    pim_db_collect_free(&collect);
    return false;
}

bool pim_db_replace(const char *name, const db_record_info_t *info,
                    const char *key, const char *payload, pim_count_t *count)
{
    bool secret;
    char keybuf[P4_CONFIG_DB_KEY_BYTES];

    if (name == NULL || info == NULL || payload == NULL) {
        if (count != NULL) {
            count->skipped++;
        }
        return false;
    }
    secret = (info->flags & DB_FLAG_SECRET) != 0;
    pim_copy_trunc(keybuf, sizeof(keybuf),
                   key != NULL && key[0] != '\0' ? key : info->key);
    if (db_set(name, info->id, info->category, keybuf, secret,
               payload, strlen(payload)) != ESP_OK) {
        if (count != NULL) {
            count->skipped++;
        }
        return false;
    }
    if (count != NULL) {
        count->ok++;
    }
    return true;
}

bool pim_db_upsert(const char *name, const pim_card_t *card, pim_count_t *count)
{
    char uid[PIM_VCF_UID_BYTES];
    char *payload = NULL;
    const char *key;
    db_record_info_t info;
    time_t stored = 0;
    time_t mtime;
    bool found;
    bool ok;

    if (name == NULL || card == NULL || count == NULL) {
        return false;
    }
    if (card->uid[0] != '\0') {
        pim_copy_trunc(uid, sizeof(uid), card->uid);
    } else {
        pim_mint_uid(uid, sizeof(uid));
    }
    mtime = (card->mtime > 0) ? card->mtime : time(NULL);
    found = pim_db_find_uid(name, uid, &info, &stored);
    if (found && !pim_merge_should_replace(card->mtime, stored)) {
        count->skipped++;
        return false;
    }
    payload = p4heap_alloc_psram((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (payload == NULL) {
        count->skipped++;
        return false;
    }
    key = card->name[0] != '\0' ? card->name : "contact";
    pim_vcf_build_payload(card, uid, mtime, payload,
                          (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (found) {
        ok = pim_db_replace(name, &info, key, payload, count);
    } else {
        ok = pim_db_commit(name, 0, key, payload, false, count);
    }
    heap_caps_free(payload);
    return ok;
}

typedef struct {
    const char *uid;
    bool found;
    uint32_t id;
    time_t modified;
} pim_alarm_find_ctx_t;

static bool pim_alarm_find_cb(const alarm_event_t *event, void *ctx)
{
    pim_alarm_find_ctx_t *c = (pim_alarm_find_ctx_t *)ctx;

    if (c == NULL || event->uid[0] == '\0') {
        return true;
    }
    if (strcmp(event->uid, c->uid) == 0) {
        c->found = true;
        c->id = event->id;
        c->modified = event->modified;
        return false;
    }
    return true;
}

static bool pim_alarm_find_uid(const char *uid, uint32_t *id_out,
                               time_t *modified_out)
{
    pim_alarm_find_ctx_t ctx;

    if (uid == NULL || uid[0] == '\0') {
        return false;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.uid = uid;
    if (alarm_list(pim_alarm_find_cb, &ctx) != ESP_OK) {
        return false;
    }
    if (!ctx.found) {
        return false;
    }
    if (id_out != NULL) {
        *id_out = ctx.id;
    }
    if (modified_out != NULL) {
        *modified_out = ctx.modified;
    }
    return true;
}

bool pim_alarms_upsert(const char *title, const char *msg, time_t when,
                       long recur, const alarm_recur_params_t *params,
                       const char *uid, time_t modified, pim_count_t *count)
{
    char use_uid[P4_CONFIG_ALARM_UID_BYTES];
    uint32_t id = 0;
    time_t stored = 0;
    time_t mtime;
    bool found;

    if (count == NULL) {
        return false;
    }
    if (recur < 0 || recur > 255) {
        count->skipped++;
        return false;
    }
    if (uid != NULL && uid[0] != '\0') {
        pim_copy_trunc(use_uid, sizeof(use_uid), uid);
    } else {
        pim_mint_uid(use_uid, sizeof(use_uid));
    }
    mtime = (modified > 0) ? modified : time(NULL);
    found = pim_alarm_find_uid(use_uid, &id, &stored);
    if (found && !pim_merge_should_replace(modified, stored)) {
        count->skipped++;
        return false;
    }
    if (!found) {
        /* Create through the existing add path, then stamp identity. */
        if (!pim_alarm_commit_core(title, msg, when, recur, ALARM_FLAG_ENABLED,
                                   params, count, &id)) {
            return false;
        }
        {
            alarm_event_t ev;
            if (alarm_get(id, &ev) == ESP_OK) {
                pim_copy_trunc(ev.uid, sizeof(ev.uid), use_uid);
                ev.modified = mtime;
                /* The record landed even if the stamp fails (backfillable). */
                (void)alarm_set(id, &ev);
            }
        }
        return true;
    }
    {
        alarm_event_t ev;
        if (alarm_get(id, &ev) != ESP_OK) {
            count->skipped++;
            return false;
        }
        /* Content from incoming; flags/action/run stay device-local. */
        if (title != NULL && title[0] != '\0') {
            pim_copy_trunc(ev.title, sizeof(ev.title), title);
        }
        if (msg != NULL && msg[0] != '\0') {
            pim_copy_trunc(ev.msg, sizeof(ev.msg), msg);
        }
        ev.when = when;
        ev.recur = (uint8_t)recur;
        if (params != NULL) {
            ev.recur_day = (params->day >= 1 && params->day <= 31)
                           ? (uint8_t)params->day : 0;
            ev.recur_month = (params->month >= 1 && params->month <= 12)
                             ? (uint8_t)params->month : 0;
            ev.recur_nth = (params->nth >= -1 && params->nth <= 5 && params->nth != 0)
                           ? (int8_t)params->nth : 0;
        }
        pim_copy_trunc(ev.uid, sizeof(ev.uid), use_uid);
        ev.modified = mtime;
        if (alarm_set(id, &ev) != ESP_OK) {
            count->skipped++;
            return false;
        }
    }
    count->ok++;
    return true;
}

int pim_db_backfill(const char *name, int *backfilled_out)
{
    pim_db_collect_t collect;
    char *payload = NULL;
    time_t now = time(NULL);
    int rc;
    int i;

    if (backfilled_out != NULL) {
        *backfilled_out = 0;
    }
    rc = pim_db_collect(name, &collect);
    if (rc != 0) {
        return rc;
    }
    payload = p4heap_alloc_psram((size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1);
    if (payload == NULL) {
        pim_db_collect_free(&collect);
        return 2;
    }
    for (i = 0; i < collect.count; i++) {
        char found[PIM_VCF_UID_BYTES];
        char minted[PIM_VCF_UID_BYTES];
        size_t len = (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES;
        size_t cap = (size_t)P4_CONFIG_DB_RECORD_MAX_BYTES + 1;
        size_t used;
        db_record_info_t info;
        bool secret;

        if (db_get(name, collect.infos[i].id, payload, &len, true, &info) != ESP_OK) {
            continue;
        }
        payload[len] = '\0';
        if (db_field_get(payload, "uid", found, sizeof(found)) && found[0] != '\0') {
            continue;
        }
        /* Append identity; skip records whose payload is already full. */
        pim_mint_uid(minted, sizeof(minted));
        used = strlen(payload);
        if (used + 64 >= cap) {
            continue;
        }
        snprintf(payload + used, cap - used, ";uid=%s;mtime=%lld",
                 minted, (long long)now);
        secret = (info.flags & DB_FLAG_SECRET) != 0;
        if (db_set(name, info.id, info.category, info.key, secret,
                   payload, strlen(payload)) == ESP_OK &&
            backfilled_out != NULL) {
            (*backfilled_out)++;
        }
    }
    heap_caps_free(payload);
    pim_db_collect_free(&collect);
    return 0;
}

typedef struct {
    uint32_t *ids;
    int count;
    int cap;
} pim_backfill_ids_t;

static bool pim_backfill_collect_cb(const alarm_event_t *event, void *ctx)
{
    pim_backfill_ids_t *c = (pim_backfill_ids_t *)ctx;

    if (c == NULL || c->ids == NULL) {
        return false;
    }
    if (event->uid[0] != '\0') {
        return true;
    }
    if (c->count >= c->cap) {
        return false;
    }
    c->ids[c->count++] = event->id;
    return true;
}

int pim_alarms_backfill(int *backfilled_out)
{
    pim_backfill_ids_t ids;
    time_t now = time(NULL);
    int i;

    if (backfilled_out != NULL) {
        *backfilled_out = 0;
    }
    ids.ids = p4heap_alloc_psram((size_t)P4_CONFIG_ALARM_MAX_EVENTS * sizeof(uint32_t));
    if (ids.ids == NULL) {
        return 2;
    }
    ids.count = 0;
    ids.cap = P4_CONFIG_ALARM_MAX_EVENTS;
    if (alarm_list(pim_backfill_collect_cb, &ids) != ESP_OK) {
        heap_caps_free(ids.ids);
        return 2;
    }
    for (i = 0; i < ids.count; i++) {
        alarm_event_t ev;
        if (alarm_get(ids.ids[i], &ev) != ESP_OK) {
            continue;
        }
        if (ev.uid[0] != '\0') {
            continue;
        }
        pim_mint_uid(ev.uid, sizeof(ev.uid));
        ev.modified = now;
        if (alarm_set(ids.ids[i], &ev) == ESP_OK && backfilled_out != NULL) {
            (*backfilled_out)++;
        }
    }
    heap_caps_free(ids.ids);
    return 0;
}

bool pim_payload_bump_mtime(char *buf, size_t cap, time_t now)
{
    char uid[PIM_VCF_UID_BYTES];
    char stamp[32];
    size_t oldlen;
    size_t stamplen;
    const char *p;

    if (buf == NULL || cap == 0 || now <= 0) {
        return false;
    }
    /* Identity gate: the same `uid=` predicate the sync membership test
     * (pim_db_find_uid / pim_db_backfill) uses. CSV grids, JSON, and plain
     * notes never carry it, so they pass through byte-identical. */
    if (!db_field_get(buf, "uid", uid, sizeof(uid)) || uid[0] == '\0') {
        return false;
    }
    snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
    stamplen = strlen(stamp);
    oldlen = strlen(buf);
    /* Replace an existing `mtime=` value span in place. Segment grammar
     * mirrors db_field_get (`;`-delimited, case-insensitive name,
     * spaces/tabs trimmed) so the field the sync reader sees is the field
     * updated here. Width may change, hence memmove. */
    p = buf;
    while (*p != '\0') {
        const char *seg = p;
        const char *semi = strchr(seg, ';');
        const char *eq;
        const char *name;
        size_t name_len;

        if (semi == NULL) {
            semi = seg + strlen(seg);
        }
        eq = memchr(seg, '=', (size_t)(semi - seg));
        if (eq != NULL) {
            const char *v = eq + 1;
            const char *vend = semi;
            size_t old_vallen;
            size_t tail_len;

            name = seg;
            while (name < eq && (*name == ' ' || *name == '\t')) {
                name++;
            }
            name_len = (size_t)(eq - name);
            while (name_len > 0 &&
                   (name[name_len - 1] == ' ' || name[name_len - 1] == '\t')) {
                name_len--;
            }
            if (name_len == 5 && strncasecmp(name, "mtime", 5) == 0) {
                while (v < vend && (*v == ' ' || *v == '\t')) {
                    v++;
                }
                while (vend > v &&
                       (vend[-1] == ' ' || vend[-1] == '\t')) {
                    vend--;
                }
                old_vallen = (size_t)(vend - v);
                if (oldlen - old_vallen + stamplen + 1 > cap) {
                    return false;
                }
                tail_len = oldlen - (size_t)(vend - buf) + 1; /* incl. NUL */
                memmove((char *)v + stamplen, vend, tail_len);
                memcpy((char *)v, stamp, stamplen);
                return true;
            }
        }
        if (*semi == '\0') {
            break;
        }
        p = semi + 1;
    }
    /* No stamp yet: append one (room-checked). */
    if (oldlen + 7 + stamplen + 1 > cap) {
        return false;
    }
    buf[oldlen] = ';';
    memcpy(buf + oldlen + 1, "mtime=", 6);
    memcpy(buf + oldlen + 7, stamp, stamplen);
    buf[oldlen + 7 + stamplen] = '\0';
    return true;
}
