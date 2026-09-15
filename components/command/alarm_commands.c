/**
 * @file alarm_commands.c
 * @brief The `alarm` and `cal` commands (batch-friendly alarm/calendar).
 *
 * Thin argv-verb dispatchers over the `alarm` module (components/alarm). They
 * own printing and ERRORLEVEL (0 ok / 1 not found / none due / 2 usage|IO).
 * Options may appear anywhere on the line; `/b` selects bare, machine-readable
 * output for `for /f` / pipes. Titles and messages use the normal shell
 * quoting rules.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "batch.h"
#include "command.h"
#include "shell.h"
#include "ansi.h"
#include "ansi_palette.h"

#define ALARM_POS_MAX           8
#define SHELL_SD_PATH_BYTES     P4_CONFIG_SD_PATH_BYTES

/* ------------------------------------------------------------------------
 * Forward declarations (cal dump callbacks)
 * ---------------------------------------------------------------------- */

static bool alarm_list_dump_cb(const alarm_event_t *e, void *ctx);
static bool cal_day_dump_cb(const alarm_event_t *e, void *ctx);
static bool cal_next_cb(const alarm_event_t *e, void *ctx);
static bool cal_month_dump_cb(const alarm_event_t *e, void *ctx);

/** Context for date-range dump callbacks (cal). */
typedef struct {
    time_t lo;
    time_t hi;
    int n;
} alarm_range_ctx_t;

/** Context for list / find "next event" callbacks. */
typedef struct {
    bool bare;          /**< alarm list: bare output. */
} alarm_bare_ctx_t;

typedef struct {
    time_t target;      /**< cal next: the target `when`. */
    bool found;
} alarm_target_ctx_t;

/* ------------------------------------------------------------------------
 * Option scanning
 * ---------------------------------------------------------------------- */

typedef struct {
    bool bare;
    bool silent;
    bool beep;
    bool led;
    uint8_t recur;              /**< ALARM_RECUR_* / weekly mask. */
    int recur_day;              /**< Monthly/yearly day 1..31 (0 = from `when`). */
    int recur_month;            /**< Yearly month 1..12 (0 = from `when`). */
    int recur_nth;              /**< Monthly nth weekday 1..5, -1 = last. */
    const char *run_path;
    const char *msg;
    const char *from;
    const char *to;
} alarm_opts_t;

static void alarm_opts_init(alarm_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->recur = ALARM_RECUR_NONE;
}

/** Apply an option token; returns true when it was an option. */
static bool alarm_apply_option(const char *a, alarm_opts_t *o)
{
    const char *opt;
    const char *colon;
    size_t nlen;
    const char *val;

    if (a[0] != '/' || a[1] == '\0') {
        return false;
    }
    opt = a + 1;
    colon = strchr(opt, ':');
    if (colon != NULL) {
        nlen = (size_t)(colon - opt);
        val = colon + 1;
        if (nlen == 3 && strncasecmp(opt, "msg", 3) == 0) { o->msg = val; return true; }
        if (nlen == 4 && strncasecmp(opt, "from", 4) == 0) { o->from = val; return true; }
        if (nlen == 2 && strncasecmp(opt, "to", 2) == 0) { o->to = val; return true; }
        if (nlen == 3 && strncasecmp(opt, "run", 3) == 0) { o->run_path = val; return true; }
        if (nlen == 6 && strncasecmp(opt, "weekly", 6) == 0) {
            unsigned mask = (unsigned)strtoul(val, NULL, 0);
            o->recur = (uint8_t)(ALARM_RECUR_WEEKLY | (mask & 0x7F));
            return true;
        }
        if (nlen == 3 && strncasecmp(opt, "day", 3) == 0) {
            o->recur_day = atoi(val);
            return true;
        }
        if (nlen == 5 && strncasecmp(opt, "month", 5) == 0) {
            o->recur_month = atoi(val);
            return true;
        }
        if (nlen == 3 && strncasecmp(opt, "byw", 3) == 0) {
            if (strcasecmp(val, "last") == 0) {
                o->recur_nth = -1;
            } else {
                o->recur_nth = atoi(val);
            }
            return true;
        }
        return true;   /* unknown /opt:val ignored */
    }
    if (strcasecmp(opt, "b") == 0) { o->bare = true; return true; }
    if (strcasecmp(opt, "silent") == 0) { o->silent = true; return true; }
    if (strcasecmp(opt, "beep") == 0) { o->beep = true; return true; }
    if (strcasecmp(opt, "led") == 0) { o->led = true; return true; }
    if (strcasecmp(opt, "daily") == 0) { o->recur = ALARM_RECUR_DAILY; return true; }
    if (strcasecmp(opt, "monthly") == 0) { o->recur = ALARM_RECUR_MONTHLY; return true; }
    if (strcasecmp(opt, "yearly") == 0) { o->recur = ALARM_RECUR_YEARLY; return true; }
    return true;
}

/** Collect options into @p o and positional tokens into @p pos. */
static int alarm_collect(int argc, char **argv, int start, alarm_opts_t *o,
                         char **pos, int pos_cap)
{
    int pcount = 0;
    int i;

    for (i = start; i < argc; i++) {
        if (!alarm_apply_option(argv[i], o) && pcount < pos_cap) {
            pos[pcount++] = argv[i];
        }
    }
    return pcount;
}

/** Join positional tokens [from..pcount) into a heap string. */
static char *alarm_join(char **pos, int from, int pcount)
{
    size_t cap = P4_CONFIG_ALARM_TITLE_BYTES;
    size_t used = 0;
    char *buf;
    int i;

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    buf[0] = '\0';
    for (i = from; i < pcount; i++) {
        size_t len = strlen(pos[i]);
        if (used > 0) {
            if (used + 1 >= cap) break;
            buf[used++] = ' ';
        }
        if (used + len >= cap) len = cap - used - 1;
        memcpy(buf + used, pos[i], len);
        used += len;
        buf[used] = '\0';
    }
    return buf;
}

/** Format a timestamp as "YYYY-MM-DD HH:MM" into @p buf. */
static void alarm_fmt_time(time_t when, char *buf, size_t size)
{
    struct tm tmv;

    if (localtime_r(&when, &tmv) == NULL || strftime(buf, size, "%Y-%m-%d %H:%M", &tmv) == 0) {
        snprintf(buf, size, "?");
    }
}

/** Format just the date "YYYY-MM-DD". */
static void alarm_fmt_date(time_t when, char *buf, size_t size)
{
    struct tm tmv;

    if (localtime_r(&when, &tmv) == NULL || strftime(buf, size, "%Y-%m-%d", &tmv) == 0) {
        snprintf(buf, size, "?");
    }
}

/** Build the action/flag summary string. */
static void alarm_fmt_state(const alarm_event_t *e, char *buf, size_t size)
{
    size_t used = 0;

    buf[0] = '\0';
    if (e->flags & ALARM_FLAG_ENABLED) {
        used += (size_t)snprintf(buf + used, size - used, "enabled ");
    }
    if (e->flags & ALARM_FLAG_FIRED) {
        used += (size_t)snprintf(buf + used, size - used, "fired ");
    }
    if (e->flags & ALARM_FLAG_SILENT) {
        used += (size_t)snprintf(buf + used, size - used, "silent ");
    }
    if (e->recur == ALARM_RECUR_DAILY) {
        used += (size_t)snprintf(buf + used, size - used, "daily ");
    } else if (e->recur == ALARM_RECUR_MONTHLY) {
        used += (size_t)snprintf(buf + used, size - used, "monthly ");
    } else if (e->recur == ALARM_RECUR_YEARLY) {
        used += (size_t)snprintf(buf + used, size - used, "yearly ");
    } else if ((e->recur & ALARM_RECUR_WEEKLY) != 0) {
        used += (size_t)snprintf(buf + used, size - used, "weekly ");
    }
    if (e->action & ALARM_ACTION_NOTIFY) used += (size_t)snprintf(buf + used, size - used, "notify ");
    if (e->action & ALARM_ACTION_BEEP)   used += (size_t)snprintf(buf + used, size - used, "beep ");
    if (e->action & ALARM_ACTION_LED)    used += (size_t)snprintf(buf + used, size - used, "led ");
    if (e->action & ALARM_ACTION_RUN)    used += (size_t)snprintf(buf + used, size - used, "run ");
    if (used == 0) {
        snprintf(buf, size, "none");
    }
}

/* ------------------------------------------------------------------------
 * Verbatim handlers (each returns ERRORLEVEL)
 * ---------------------------------------------------------------------- */

static bool alarm_list_dump_cb(const alarm_event_t *e, void *ctx);

static int alarm_cmd_add(char **pos, int pcount, alarm_opts_t *o)
{
    time_t when;
    char *title;
    uint32_t id = 0;
    esp_err_t error;

    if (pcount < 2) {
        shell_print_usage("Usage: alarm add <YYYY-MM-DD> <HH:MM> [title] [/msg:..] [/daily|/weekly:mask|/monthly|/yearly] [/day:D] [/month:M] [/byw:N|last] [/beep] [/led] [/run:file.bat] [/silent]");
        return 2;
    }
    if (!alarm_parse_datetime(pos[0], pos[1], &when)) {
        shell_print_error("alarm: invalid date/time (expected YYYY-MM-DD HH:MM)");
        return 2;
    }
    /* Monthly/yearly parameter checks (/day: /month: /byw: need a matching
     * monthly/yearly base; /byw: snaps `when` to the next matching date). */
    if ((o->recur_day != 0 || o->recur_month != 0 || o->recur_nth != 0) &&
        o->recur != ALARM_RECUR_MONTHLY && o->recur != ALARM_RECUR_YEARLY) {
        shell_print_error("alarm: /day: /month: /byw: need /monthly or /yearly");
        return 2;
    }
    if (o->recur == ALARM_RECUR_YEARLY && o->recur_nth != 0) {
        shell_print_error("alarm: /byw: is monthly-only");
        return 2;
    }
    if (o->recur_day < 0 || o->recur_day > 31 ||
        o->recur_month < 0 || o->recur_month > 12 ||
        o->recur_nth < -1 || o->recur_nth > 5) {
        shell_print_error("alarm: /day: 1..31, /month: 1..12, /byw: 1..5|last");
        return 2;
    }
    if (o->recur == ALARM_RECUR_MONTHLY && o->recur_nth != 0) {
        struct tm tmv;
        int wday;
        int guard = 0;
        if (localtime_r(&when, &tmv) == NULL) {
            shell_print_error("alarm: clock error");
            return 2;
        }
        wday = tmv.tm_wday;
        /* Snap forward to the next date matching nth+weekday (a full year
         * always contains one; 400 days caps the scan far above the
         * ~90-day worst case). The leaf then keeps that weekday across
         * advances. */
        while (guard++ < 400) {
            int mday = tmv.tm_mday;
            int n = (mday - 1) / 7 + 1;
            bool last = false;
            {
                struct tm probe = tmv;
                time_t pt;
                probe.tm_mday = mday + 7;
                probe.tm_isdst = -1;
                pt = mktime(&probe);
                if (pt != (time_t)-1) {
                    struct tm back;
                    if (localtime_r(&pt, &back) != NULL) {
                        last = (back.tm_mon != tmv.tm_mon);
                    }
                }
            }
            if (tmv.tm_wday == wday &&
                ((o->recur_nth > 0 && n == o->recur_nth) ||
                 (o->recur_nth < 0 && last))) {
                break;
            }
            tmv.tm_mday++;
            tmv.tm_isdst = -1;
            {
                time_t t = mktime(&tmv);
                if (t == (time_t)-1 || localtime_r(&t, &tmv) == NULL) {
                    shell_print_error("alarm: clock error");
                    return 2;
                }
                when = t;
            }
        }
    }

    title = alarm_join(pos, 2, pcount);
    if (title == NULL) {
        shell_print_error("alarm: out of memory");
        return 2;
    }
    if (title[0] == '\0') {
        snprintf(title, P4_CONFIG_ALARM_TITLE_BYTES, "Alarm");
    }

    {
        uint8_t action = ALARM_ACTION_NOTIFY;
        alarm_recur_params_t params;
        if (o->beep) action |= ALARM_ACTION_BEEP;
        if (o->led) action |= ALARM_ACTION_LED;
#if P4_CONFIG_ALARM_ENABLE_RUN_ACTION
        if (o->run_path != NULL) action |= ALARM_ACTION_RUN;
#endif
        params.day = o->recur_day;
        params.month = o->recur_month;
        params.nth = o->recur_nth;
        error = alarm_add_ex(title, o->msg, when, o->recur, action, o->run_path,
                             &params, &id);
    }
    free(title);
    if (error == ESP_ERR_NO_MEM) {
        shell_print_error("alarm: store is full (max %d events)", P4_CONFIG_ALARM_MAX_EVENTS);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("alarm: could not add event (%s)", esp_err_to_name(error));
        return 2;
    }
    if (o->bare) {
        shell_transcript_appendf("%lu\n", (unsigned long)id);
    } else {
        char when_s[24];
        alarm_fmt_time(when, when_s, sizeof(when_s));
        shell_print_ok("alarm: %lu scheduled for %s", (unsigned long)id, when_s);
    }
    return 0;
}

static int alarm_cmd_list(char **pos, int pcount, alarm_opts_t *o)
{
    alarm_bare_ctx_t ctx;
    esp_err_t error;

    (void)pos;
    (void)pcount;
    ctx.bare = o->bare;
    error = alarm_list(alarm_list_dump_cb, &ctx);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("alarm: SD card unavailable");
        return 2;
    }
    if (error != ESP_OK) {
        shell_print_error("alarm: could not list events (%s)", esp_err_to_name(error));
        return 2;
    }
    return 0;
}

static bool alarm_list_dump_cb(const alarm_event_t *e, void *ctx)
{
    alarm_bare_ctx_t *c = (alarm_bare_ctx_t *)ctx;
    char when_s[24];
    char state[64];

    alarm_fmt_time(e->when, when_s, sizeof(when_s));
    if (c->bare) {
        shell_transcript_appendf("%lu|%s|%s|%u|%u|%u\n",
                                 (unsigned long)e->id, when_s, e->title,
                                 (unsigned)e->flags, (unsigned)e->recur,
                                 (unsigned)e->action);
    } else {
        alarm_fmt_state(e, state, sizeof(state));
        shell_transcript_appendf_ansi("  " SH_NUM "%06lu" SH_RST " " SH_VAL "%s" SH_RST
                                      " " SH_TEXT "%s" SH_RST " (" SH_MUTE "%s" SH_RST ")\n",
                                      (unsigned long)e->id, when_s, e->title, state);
    }
    return true;
}

static int alarm_cmd_del(char **pos, int pcount, alarm_opts_t *o)
{
    (void)o;
    if (pcount < 1) {
        shell_print_usage("Usage: alarm del <id|all>");
        return 2;
    }
    if (strcasecmp(pos[0], "all") == 0) {
        esp_err_t error = alarm_del_all();

        if (error != ESP_OK) {
            shell_print_error("alarm: could not delete all events (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("alarm: deleted all events");
        return 0;
    }
    {
        uint32_t id = (uint32_t)strtoul(pos[0], NULL, 10);
        esp_err_t error = alarm_del(id, false);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("alarm: event %lu not found", (unsigned long)id);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("alarm: could not delete event (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("alarm: %lu soft-deleted (alarm purge to remove)", (unsigned long)id);
        return 0;
    }
}

static int alarm_cmd_snooze(char **pos, int pcount, alarm_opts_t *o)
{
    uint32_t id;
    char *end = NULL;
    long minutes = 10;
    esp_err_t error;
    char when_s[24];

    (void)o;
    if (pcount < 1 || pcount > 2) {
        shell_print_usage("Usage: alarm snooze <id> [minutes]");
        return 2;
    }
    id = (uint32_t)strtoul(pos[0], &end, 10);
    if (end == pos[0] || *end != '\0') {
        shell_print_usage("Usage: alarm snooze <id> [minutes]");
        return 2;
    }
    if (pcount == 2) {
        minutes = strtol(pos[1], &end, 10);
        if (end == pos[1] || *end != '\0' || minutes < 1 || minutes > 24 * 60) {
            shell_print_error("alarm: minutes must be 1..1440");
            return 2;
        }
    }
    error = alarm_snooze(id, (int)minutes);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("alarm: event %lu not found", (unsigned long)id);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("alarm: could not snooze event (%s)", esp_err_to_name(error));
        return 2;
    }
    {
        alarm_event_t e;
        if (alarm_get(id, &e) == ESP_OK) {
            alarm_fmt_time(e.when, when_s, sizeof(when_s));
            shell_print_ok("alarm: %lu snoozed to %s", (unsigned long)id, when_s);
        } else {
            shell_print_ok("alarm: %lu snoozed", (unsigned long)id);
        }
    }
    return 0;
}

static int alarm_cmd_enable(char **pos, int pcount, alarm_opts_t *o, bool enable){
    (void)o;
    if (pcount < 1) {
        shell_print_usage(enable ? "Usage: alarm enable <id>" : "Usage: alarm disable <id>");
        return 2;
    }
    {
        uint32_t id = (uint32_t)strtoul(pos[0], NULL, 10);
        esp_err_t error = alarm_enable(id, enable);
        if (error == ESP_ERR_NOT_FOUND) {
            shell_print_error("alarm: event %lu not found", (unsigned long)id);
            return 1;
        }
        if (error != ESP_OK) {
            shell_print_error("alarm: could not update event (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("alarm: %lu %s", (unsigned long)id, enable ? "enabled" : "disabled");
        return 0;
    }
}

static int alarm_cmd_status(char **pos, int pcount, alarm_opts_t *o)
{
    alarm_status_t st;
    char next_s[24];

    (void)pos;
    (void)pcount;
    if (alarm_status(&st) != ESP_OK) {
        shell_print_error("alarm: could not read status");
        return 2;
    }
    if (o->bare) {
        shell_transcript_appendf("%lu|%lu|%s|%d|%d\n",
                                 (unsigned long)st.count, (unsigned long)st.enabled,
                                 st.next_due ? (alarm_fmt_time(st.next_due, next_s, sizeof(next_s)), next_s) : "none",
                                 st.checker_running ? 1 : 0, st.catchup_done ? 1 : 0);
    } else {
        shell_print_field_num("alarm.count", (long)st.count);
        shell_print_field_num("alarm.enabled", (long)st.enabled);
        if (st.next_due != 0) {
            alarm_fmt_time(st.next_due, next_s, sizeof(next_s));
            shell_print_field("alarm.next", "%s", next_s);
        } else {
            shell_print_field("alarm.next", "none");
        }
        shell_print_field("alarm.checker", "%s", st.checker_running ? "running" : "stopped");
        shell_print_field("alarm.catchup", "%s", st.catchup_done ? "done" : "pending");
    }
    return 0;
}

static int alarm_cmd_purge(char **pos, int pcount, alarm_opts_t *o)
{
    (void)pos;
    (void)pcount;
    (void)o;
    {
        esp_err_t error = alarm_purge();
        if (error != ESP_OK) {
            shell_print_error("alarm: could not purge (%s)", esp_err_to_name(error));
            return 2;
        }
        shell_print_ok("alarm: purged fired/disabled events");
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * cal (thin calendar alias over the same store)
 * ---------------------------------------------------------------------- */

static int cal_cmd_today(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    time_t day_start, day_end;
    alarm_range_ctx_t ctx;

    localtime_r(&now, &tmv);
    tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
    day_start = mktime(&tmv);
    day_end = day_start + 86400;

    shell_transcript_append_text("Events today:\n");
    ctx.lo = day_start;
    ctx.hi = day_end;
    ctx.n = 0;
    alarm_list(cal_day_dump_cb, &ctx);
    shell_print_field_num("cal.today", ctx.n);
    return 0;
}

static bool cal_day_dump_cb(const alarm_event_t *e, void *ctx)
{
    alarm_range_ctx_t *c = (alarm_range_ctx_t *)ctx;
    char time_s[12];

    if (e->when < c->lo || e->when >= c->hi) {
        return true;
    }
    alarm_fmt_time(e->when, time_s, sizeof(time_s));
    shell_transcript_appendf_ansi("  " SH_NUM "%s" SH_RST " " SH_TEXT "%s" SH_RST "\n",
                                  time_s + 11, e->title);
    c->n++;
    return true;
}

static int cal_cmd_next(void)
{
    alarm_status_t st;
    char when_s[24];

    if (alarm_status(&st) != ESP_OK || st.next_due == 0) {
        shell_print_muted("cal: no events scheduled");
        return 1;
    }
    alarm_fmt_time(st.next_due, when_s, sizeof(when_s));
    /* Find and print the event whose `when` is the next due time. */
    {
        alarm_target_ctx_t ctx = { st.next_due, false };
        alarm_list(cal_next_cb, &ctx);
        if (!ctx.found) {
            shell_print_field("cal.next", "%s", when_s);
        }
    }
    return 0;
}

static bool cal_next_cb(const alarm_event_t *e, void *ctx)
{
    alarm_target_ctx_t *c = (alarm_target_ctx_t *)ctx;
    char when_s[24];

    if (e->when != c->target || c->found) {
        return true;
    }
    c->found = true;
    alarm_fmt_time(e->when, when_s, sizeof(when_s));
    shell_print_field("cal.next", "%s  %s", when_s, e->title);
    return true;
}

static int cal_cmd_week(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    time_t midnight;
    int d;

    localtime_r(&now, &tmv);
    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    midnight = mktime(&tmv);
    for (d = 0; d < 7; d++) {
        alarm_range_ctx_t ctx;
        char date_s[16];
        char wday_s[8];
        time_t day = midnight + (time_t)d * 86400;
        if (localtime_r(&day, &tmv) == NULL) {
            continue;
        }
        alarm_fmt_date(day, date_s, sizeof(date_s));
        if (strftime(wday_s, sizeof(wday_s), "%a", &tmv) == 0) {
            snprintf(wday_s, sizeof(wday_s), "?");
        }
        shell_transcript_appendf("%s %s:\n", wday_s, date_s);
        ctx.lo = day;
        ctx.hi = day + 86400;
        ctx.n = 0;
        alarm_list(cal_day_dump_cb, &ctx);
        if (ctx.n == 0) {
            shell_transcript_append_text("  -\n");
        }
    }
    return 0;
}

/** Days in a month for the grid (proleptic Gregorian). */
static int cal_month_days(int year, int month)
{
    static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days;

    if (month < 1 || month > 12) {
        return 0;
    }
    days = table[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        days = 29;
    }
    return days;
}

/** Collect event-day marks for [lo,hi) into a 31-bit mask. */
typedef struct {
    time_t lo;
    time_t hi;
    uint32_t marks;
    int n;
} cal_grid_ctx_t;

static bool cal_grid_mark_cb(const alarm_event_t *e, void *ctx)
{
    cal_grid_ctx_t *c = (cal_grid_ctx_t *)ctx;
    struct tm tmv;

    if (e->when < c->lo || e->when >= c->hi) {
        return true;
    }
    if (localtime_r(&e->when, &tmv) != NULL && tmv.tm_mday >= 1 && tmv.tm_mday <= 31) {
        c->marks |= (uint32_t)(1u << (tmv.tm_mday - 1));
    }
    c->n++;
    return true;
}

/** Print a classic month grid; event days carry a `*` marker. */
static void cal_print_grid(int year, int mon)
{
    static const char *const names[12] = {"January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December"};
    struct tm tmv;
    time_t start, end;
    cal_grid_ctx_t ctx;
    int first_wday;
    int days;
    int d;

    if (mon < 1 || mon > 12) {
        return;
    }
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = 1;
    tmv.tm_isdst = -1;
    start = mktime(&tmv);
    tmv.tm_mon = mon;
    end = mktime(&tmv);
    if (localtime_r(&start, &tmv) == NULL) {
        return;
    }
    first_wday = tmv.tm_wday;
    days = cal_month_days(year, mon);
    ctx.lo = start;
    ctx.hi = end;
    ctx.marks = 0;
    ctx.n = 0;
    alarm_list(cal_grid_mark_cb, &ctx);

    shell_transcript_appendf("   %s %d\n", names[mon - 1], year);
    shell_transcript_append_text("Su Mo Tu We Th Fr Sa\n");
    for (d = 0; d < first_wday; d++) {
        shell_transcript_append_text("   ");
    }
    for (d = 1; d <= days; d++) {
        bool mark = (ctx.marks & (uint32_t)(1u << (d - 1))) != 0;
        shell_transcript_appendf("%2d%s", d, mark ? "*" : " ");
        if ((first_wday + d) % 7 == 0 || d == days) {
            shell_transcript_append_text("\n");
        } else {
            shell_transcript_append_text(" ");
        }
    }
}

static int cal_cmd_month(const char *ym)
{
    int year = 0, mon = 0;
    struct tm tmv;
    time_t start, end;
    alarm_range_ctx_t ctx;

    if (sscanf(ym, "%d-%d", &year, &mon) != 2 || mon < 1 || mon > 12) {
        shell_print_usage("Usage: cal [today|week|next|YYYY-MM]");
        return 2;
    }
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = 1;
    tmv.tm_isdst = -1;
    start = mktime(&tmv);
    tmv.tm_mon = mon;   /* first day of the next month */
    end = mktime(&tmv);

    cal_print_grid(year, mon);
    shell_transcript_appendf("Events in %04d-%02d:\n", year, mon);
    ctx.lo = start;
    ctx.hi = end;
    ctx.n = 0;
    alarm_list(cal_month_dump_cb, &ctx);
    shell_print_field_num("cal.events", ctx.n);
    return 0;
}

static bool cal_month_dump_cb(const alarm_event_t *e, void *ctx)
{
    alarm_range_ctx_t *c = (alarm_range_ctx_t *)ctx;
    char date_s[16];
    char time_s[12];

    if (e->when < c->lo || e->when >= c->hi) {
        return true;
    }
    alarm_fmt_date(e->when, date_s, sizeof(date_s));
    alarm_fmt_time(e->when, time_s, sizeof(time_s));
    shell_transcript_appendf_ansi("  " SH_NUM "%s" SH_RST " " SH_VAL "%s" SH_RST
                                  " " SH_TEXT "%s" SH_RST "\n",
                                  date_s, time_s + 11, e->title);
    c->n++;
    return true;
}

/* ------------------------------------------------------------------------
 * Dispatchers
 * ---------------------------------------------------------------------- */

/** Start the alarm store + checker on first use (see command_init: eager
 * start fragments the DMA heap before USB host init and breaks USB HCD
 * bring-up, verified on hardware). Idempotent; failure degrades to
 * direct store access with no background firing. */
static void shell_alarm_ensure_init(void)
{
    if (!alarm_is_initialized()) {
        if (alarm_init() != ESP_OK) {
            shell_print_warning("alarm: background checker unavailable");
        }
    }
}

void shell_command_alarm(int argc, char **argv)
{
    const char *verb;
    alarm_opts_t opts;
    char *pos[ALARM_POS_MAX];
    int pcount;
    int result;

    shell_alarm_ensure_init();
    if (argc < 2) {
        shell_print_usage("Usage: alarm <add|list|del|enable|disable|snooze|status|purge> [...]");
        batch_set_errorlevel(2);
        return;
    }
    verb = argv[1];
    alarm_opts_init(&opts);
    pcount = alarm_collect(argc, argv, 2, &opts, pos, ALARM_POS_MAX);

    if (shell_text_equals_ignore_case(verb, "add")) {
        result = alarm_cmd_add(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "list")) {
        result = alarm_cmd_list(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "del") ||
               shell_text_equals_ignore_case(verb, "delete")) {
        result = alarm_cmd_del(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "enable")) {
        result = alarm_cmd_enable(pos, pcount, &opts, true);
    } else if (shell_text_equals_ignore_case(verb, "snooze")) {
        result = alarm_cmd_snooze(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "disable")) {
        result = alarm_cmd_enable(pos, pcount, &opts, false);
    } else if (shell_text_equals_ignore_case(verb, "status")) {
        result = alarm_cmd_status(pos, pcount, &opts);
    } else if (shell_text_equals_ignore_case(verb, "purge")) {
        result = alarm_cmd_purge(pos, pcount, &opts);
    } else {
        shell_print_error("alarm: unknown verb %s", verb);
        shell_print_usage("Usage: alarm <add|list|del|enable|disable|snooze|status|purge> [...]");
        result = 2;
    }
    batch_set_errorlevel(result);
}

void shell_command_cal(int argc, char **argv)
{
    int result = 0;

    shell_alarm_ensure_init();
    if (argc < 2) {
        result = cal_cmd_today();
    } else if (shell_text_equals_ignore_case(argv[1], "today")) {
        result = cal_cmd_today();
    } else if (shell_text_equals_ignore_case(argv[1], "week")) {
        result = cal_cmd_week();
    } else if (shell_text_equals_ignore_case(argv[1], "next")) {
        result = cal_cmd_next();
    } else {
        result = cal_cmd_month(argv[1]);
    }
    batch_set_errorlevel(result);
}
