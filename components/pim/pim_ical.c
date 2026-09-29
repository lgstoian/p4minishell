/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_ical.c
 * @brief iCalendar VEVENT interchange over the alarm store.
 *
 * Parse half moved out of components/command/import_commands.c, render half
 * moved out of components/command/export_commands.c: the single VEVENT
 * reader (floating local DTSTART, SUMMARY/DESCRIPTION, RRULE daily/weekly/
 * monthly/yearly; unknown frequencies degrade to one-shot; UTC DTSTART read
 * as device-local, documented) and the single VEVENT writer (UID minted as
 * "<id>@p4minishell").
 */

#include "pim.h"
#include "p4minishell_config.h"

#include <string.h>
#include <strings.h>

#ifndef P4_CONFIG_ALARM_MAX_EVENTS
#define P4_CONFIG_ALARM_MAX_EVENTS 64
#endif

#ifndef P4_CONFIG_ALARM_TITLE_BYTES
#define P4_CONFIG_ALARM_TITLE_BYTES 48
#endif

#ifndef P4_CONFIG_ALARM_MSG_BYTES
#define P4_CONFIG_ALARM_MSG_BYTES 160
#endif

bool pim_ics_datetime(const char *text, struct tm *out)
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
 * iCalendar event import
 * ======================================================================== */

typedef struct {
    char title[P4_CONFIG_ALARM_TITLE_BYTES];
    char msg[P4_CONFIG_ALARM_MSG_BYTES];
    time_t when;
    bool have_when;
    long recur;
    /** Incoming `UID` (truncated), or empty when absent. */
    char uid[PIM_VCF_UID_BYTES];
    /** Incoming `DTSTAMP` as Unix time, or 0 when absent/unparseable. */
    time_t modified;
    bool active;
} pim_vevent_t;

static void pim_vevent_reset(pim_vevent_t *e)
{
    memset(e, 0, sizeof(*e));
    e->active = true;
}

/** Unescape ics text (`\\` `\,` `\;` `\n`) in place. */
static void pim_ics_unescape(char *text)
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
static long pim_ics_rrule(const char *value, int wday, int ev_mon, int ev_mday,
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
    pim_vevent_t ev;
    char rrule[128];
    bool in_event;
    bool merge;
    pim_count_t *count;
} pim_ics_ctx_t;

/** Handle one complete (unfolded) iCalendar content line. */
static void pim_ics_line(const char *current, void *vctx)
{
    pim_ics_ctx_t *ctx = (pim_ics_ctx_t *)vctx;
    char prop[32];
    const char *value = NULL;
    struct tm tmv;

    if (ctx == NULL) {
        return;
    }
    if (!pim_vcf_prop_split(current, prop, sizeof(prop), &value)) {
        return;
    }
    if (strcasecmp(prop, "BEGIN") == 0) {
        if (strcasecmp(value, "VEVENT") == 0) {
            pim_vevent_reset(&ctx->ev);
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
                    long recur = pim_ics_rrule(ctx->rrule, wday, ev_mon, ev_mday,
                                               &params);
                    if (ctx->merge) {
                        (void)pim_alarms_upsert(ctx->ev.title, ctx->ev.msg,
                                                ctx->ev.when, recur, &params,
                                                ctx->ev.uid, ctx->ev.modified,
                                                ctx->count);
                    } else {
                        pim_alarm_commit(ctx->ev.title, ctx->ev.msg, ctx->ev.when,
                                         recur, ALARM_FLAG_ENABLED, &params,
                                         ctx->count);
                    }
                }
            }
            ctx->in_event = false;
        }
        return;
    }
    if (strcasecmp(prop, "DTSTART") == 0) {
        if (pim_ics_datetime(value, &tmv)) {
            ctx->ev.when = mktime(&tmv);
            ctx->ev.have_when = (ctx->ev.when != (time_t)-1);
        }
    } else if (strcasecmp(prop, "SUMMARY") == 0) {
        char decoded[PIM_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        pim_ics_unescape(decoded);
        pim_copy_trunc(ctx->ev.title, sizeof(ctx->ev.title), decoded);
    } else if (strcasecmp(prop, "DESCRIPTION") == 0) {
        char decoded[PIM_LINE_BYTES];
        snprintf(decoded, sizeof(decoded), "%s", value);
        pim_ics_unescape(decoded);
        pim_copy_trunc(ctx->ev.msg, sizeof(ctx->ev.msg), decoded);
    } else if (strcasecmp(prop, "RRULE") == 0) {
        pim_copy_trunc(ctx->rrule, sizeof(ctx->rrule), value);
    } else if (strcasecmp(prop, "UID") == 0) {
        /* Captured for the merge path; the add path ignores it. */
        if (ctx->ev.uid[0] == '\0') {
            pim_copy_trunc(ctx->ev.uid, sizeof(ctx->ev.uid), value);
        }
    } else if (strcasecmp(prop, "DTSTAMP") == 0) {
        if (ctx->ev.modified == 0 && pim_ics_datetime(value, &tmv)) {
            time_t stamp = mktime(&tmv);
            if (stamp != (time_t)-1) {
                ctx->ev.modified = stamp;
            }
        }
    }
    /* DTEND/DURATION/others are intentionally ignored. */
}

int pim_ics_parse_stream(FILE *fp, pim_count_t *count)
{
    pim_ics_ctx_t ctx;

    if (fp == NULL || count == NULL) {
        return 2;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.count = count;
    ctx.merge = false;
    pim_pump_unfolded(fp, pim_ics_line, &ctx);
    return 0;
}

int pim_ics_merge_stream(FILE *fp, pim_count_t *count)
{
    pim_ics_ctx_t ctx;

    if (fp == NULL || count == NULL) {
        return 2;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.count = count;
    ctx.merge = true;
    pim_pump_unfolded(fp, pim_ics_line, &ctx);
    return 0;
}

/* ========================================================================
 * iCalendar event rendering
 * ======================================================================== */

/** Render a local timestamp as floating "YYYYMMDDTHHMMSS" (no Z). */
static void pim_ics_when(time_t when, char *out, size_t out_size)
{
    struct tm tmv;
    if (localtime_r(&when, &tmv) != NULL &&
        strftime(out, out_size, "%Y%m%dT%H%M%S", &tmv) != 0) {
        return;
    }
    snprintf(out, out_size, "%lld", (long long)when);
}

static int pim_alarms_render_ics_ex(pim_doc_t *doc, int *rows_out,
                                      bool identity)
{
    pim_alarm_collect_t collect;
    time_t now = time(NULL);
    char stamp[32];
    int rc;
    int i;

    if (doc == NULL || rows_out == NULL) {
        return 2;
    }
    rc = pim_alarms_collect(&collect);
    if (rc != 0) {
        return rc;
    }
    pim_ics_when(now, stamp, sizeof(stamp));
    pim_doc_text(doc, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n");
    pim_doc_text(doc, "PRODID:-//P4MiniShell//EN\r\n");
    for (i = 0; i < collect.count; i++) {
        const alarm_event_t *e = &collect.events[i];
        char uid[PIM_VCF_UID_BYTES];
        char start[32];
        const char *use_stamp = stamp;
        char estamp[32];
        if (identity && e->uid[0] != '\0') {
            /* Stored sync identity (backfilled by `pim get`). */
            pim_copy_trunc(uid, sizeof(uid), e->uid);
            if (e->modified > 0) {
                pim_ics_when(e->modified, estamp, sizeof(estamp));
                use_stamp = estamp;
            }
        } else {
            snprintf(uid, sizeof(uid), "%lu@p4minishell", (unsigned long)e->id);
        }
        pim_ics_when(e->when, start, sizeof(start));
        pim_doc_text(doc, "BEGIN:VEVENT\r\n");
        pim_doc_ical_prop(doc, "UID", uid);
        pim_doc_ical_prop(doc, "DTSTAMP", use_stamp);
        pim_doc_ical_prop(doc, "DTSTART", start);
        pim_doc_ical_prop(doc, "SUMMARY", e->title);
        pim_doc_ical_prop(doc, "DESCRIPTION", e->msg);
        if (e->recur == ALARM_RECUR_DAILY) {
            pim_doc_ical_prop(doc, "RRULE", "FREQ=DAILY");
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
                    pim_doc_text(doc, rule);
                    pim_doc_text(doc, "\r\n");
                } else {
                    pim_doc_ical_prop(doc, "RRULE", "FREQ=MONTHLY");
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
                    pim_doc_ical_prop(doc, "RRULE", rule);
                } else {
                    pim_doc_ical_prop(doc, "RRULE", "FREQ=MONTHLY");
                }
            }
        } else if (e->recur == ALARM_RECUR_YEARLY) {
            pim_doc_ical_prop(doc, "RRULE", "FREQ=YEARLY");
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
                pim_doc_ical_prop(doc, "RRULE", rule);
            }
        }
        pim_doc_text(doc, "END:VEVENT\r\n");
        (*rows_out)++;
    }
    pim_doc_text(doc, "END:VCALENDAR\r\n");
    pim_alarms_collect_free(&collect);
    return 0;
}

int pim_alarms_render_ics(pim_doc_t *doc, int *rows_out)
{
    /* identity=false: historical export shape, byte-identical. */
    return pim_alarms_render_ics_ex(doc, rows_out, false);
}

int pim_alarms_render_ics_uid(pim_doc_t *doc, int *rows_out)
{
    return pim_alarms_render_ics_ex(doc, rows_out, true);
}
