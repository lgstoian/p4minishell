/**
 * @file clock_commands.c
 * @brief Clock command surface for P4MiniShell (date / time / timezone / sntp).
 *
 * The command implementations live with the clock component so all time- and
 * SNTP-related behaviour is owned here. The shell is a LEAF dependency of the
 * shell (shell -> clock), so this module must not depend on the shell: it
 * renders through the clock_host_ops_t table registered by the command module
 * in command_init(). The wrappers map one-to-one onto the shell print helpers,
 * so the on-screen output is identical to any command living in command.c.
 *
 * Commands:
 *   - date [MM-DD-YYYY]      show the clock panel, or set the system date
 *   - time [HH:MM[:SS]]      show the clock panel, or set the system time
 *   - timezone [TZ]          show the timezone, or set a POSIX TZ string
 *   - sntp|ntpsync [sync]    show NTP sync status, or force a re-sync
 *   - timer|stopwatch ...    named stopwatch runs (start/stop/lap/status)
 *
 * Setting the date/time adjusts the C-library clock; a later SNTP sync
 * overrides it, matching how DOS-era boxes behaved against an authoritative
 * source.
 */

#include "clock.h"
#include "p4minishell_config.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* ========================================================================
 * HOST-RENDER OPS (registered by the command module)
 * ======================================================================== */

static clock_host_ops_t s_clock_ops;
static bool s_clock_ops_set = false;

void clock_register_host_ops(const clock_host_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_clock_ops, 0, sizeof(s_clock_ops));
        s_clock_ops_set = false;
        return;
    }
    s_clock_ops = *ops;
    s_clock_ops_set = true;
}

static void clock_emit_text(const char *text)
{
    if (s_clock_ops_set && s_clock_ops.emit_text != NULL) {
        s_clock_ops.emit_text(text);
    }
}

static void clock_emit_heading(const char *format, ...)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_heading != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_clock_ops.print_heading(line);
}

static void clock_emit_field(const char *label, const char *format, ...)
{
    char value[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_field != NULL)) {
        return;
    }
    value[0] = '\0';
    if (format != NULL) {
        va_start(args, format);
        vsnprintf(value, sizeof(value), format, args);
        va_end(args);
    }
    s_clock_ops.print_field(label, value);
}

static void clock_emit_ok(const char *format, ...)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_ok != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_clock_ops.print_ok(line);
}

static void clock_emit_error(const char *format, ...)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_error != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_clock_ops.print_error(line);
}

static void clock_emit_muted(const char *format, ...)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_muted != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_clock_ops.print_muted(line);
}

static void clock_emit_usage(const char *format, ...)
{
    char line[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.print_usage != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_clock_ops.print_usage(line);
}

static void clock_record_error(const char *domain, esp_err_t error, const char *format, ...)
{
    char message[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.record_error != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    s_clock_ops.record_error(domain, error, message);
}

static void clock_record_warning(const char *domain, const char *format, ...)
{
    char message[P4_CONFIG_ANSI_BUFFER_BYTES];
    va_list args;

    if (!(s_clock_ops_set && s_clock_ops.record_warning != NULL)) {
        return;
    }
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    s_clock_ops.record_warning(domain, message);
}

static bool clock_cmd_equals(const char *left, const char *right)
{
    if (s_clock_ops_set && s_clock_ops.equals_ignore_case != NULL) {
        return s_clock_ops.equals_ignore_case(left, right);
    }
    return strcasecmp(left, right) == 0;
}

/* ========================================================================
 * CLOCK PANEL
 * ======================================================================== */

/** Print the full clock panel shown by `date` and `time` with no argument. */
static void clock_print_panel(void)
{
    char uptime[48];
    const char *sync = time_is_synchronized() ? "synced" : "not synced";

    time_get_uptime_formatted(uptime, sizeof(uptime));

    clock_emit_heading("Date / Time");
    clock_emit_field("Local:", "%s", time_get_formatted());
    clock_emit_field("UTC:", "%s", time_get_formatted_utc());
    clock_emit_field("Unix:", "%ld", (long)time_get_unix());
    clock_emit_field("Timezone:", "%s", time_get_timezone());
    clock_emit_field("Uptime:", "%s", uptime);
    if (time_is_synchronized()) {
        clock_emit_field("NTP sync:", "%s (%s)", sync, time_get_ntp_server());
    } else {
        clock_emit_field("NTP sync:", "%s", sync);
    }
}

/* ========================================================================
 * COMMANDS
 * ======================================================================== */

void clock_command_date(int argc, char **argv)
{
    struct tm now;
    struct timeval tv;
    time_t stamp;
    int month = 0;
    int day = 0;
    int year = 0;

    if (argc == 1) {
        clock_print_panel();
        return;
    }

    if (argc != 2) {
        clock_emit_usage("Usage: date [MM-DD-YYYY]");
        return;
    }

    if (sscanf(argv[1], "%d-%d-%d", &month, &day, &year) != 3 &&
        sscanf(argv[1], "%d/%d/%d", &month, &day, &year) != 3) {
        clock_emit_usage("Usage: date [MM-DD-YYYY]");
        clock_record_warning("date", "Unparsable date argument: %s", argv[1]);
        return;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1970 || year > 2099) {
        clock_emit_usage("Usage: date [MM-DD-YYYY]");
        clock_emit_text("date: value out of range (months 1-12, days 1-31, years 1970-2099)\n");
        clock_record_warning("date", "Out of range or wrong order (expected MM-DD-YYYY): %s", argv[1]);
        return;
    }

    now = time_get_local();
    now.tm_mon = month - 1;
    now.tm_mday = day;
    now.tm_year = year - 1900;
    now.tm_isdst = -1;

    stamp = mktime(&now);
    if (stamp == (time_t)-1) {
        clock_emit_error("date: could not apply that date");
        clock_record_warning("date", "mktime rejected %s", argv[1]);
        return;
    }

    tv.tv_sec = stamp;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        clock_emit_error("date: failed to update the system clock");
        clock_record_error("date", ESP_FAIL, "settimeofday failed");
        return;
    }

    clock_rtc_note_synced("manual");
    clock_rtc_ext_write(stamp);
    clock_emit_field("The current date is:", "%s", time_get_formatted());
}

void clock_command_time(int argc, char **argv)
{
    struct tm now;
    struct timeval tv;
    time_t stamp;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int parsed;

    if (argc == 1) {
        clock_print_panel();
        return;
    }

    if (argc != 2) {
        clock_emit_usage("Usage: time [HH:MM[:SS]]");
        return;
    }

    parsed = sscanf(argv[1], "%d:%d:%d", &hour, &minute, &second);
    if (parsed < 2) {
        clock_emit_usage("Usage: time [HH:MM[:SS]]");
        clock_record_warning("time", "Unparsable time argument: %s", argv[1]);
        return;
    }
    if (parsed == 2) {
        second = 0;
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        clock_emit_text("time: value out of range (hours 0-23, minutes and seconds 0-59)\n");
        return;
    }

    now = time_get_local();
    now.tm_hour = hour;
    now.tm_min = minute;
    now.tm_sec = second;
    now.tm_isdst = -1;

    stamp = mktime(&now);
    if (stamp == (time_t)-1) {
        clock_emit_error("time: could not apply that time");
        clock_record_warning("time", "mktime rejected %s", argv[1]);
        return;
    }

    tv.tv_sec = stamp;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        clock_emit_error("time: failed to update the system clock");
        clock_record_error("time", ESP_FAIL, "settimeofday failed");
        return;
    }

    clock_rtc_note_synced("manual");
    clock_rtc_ext_write(stamp);
    clock_emit_field("The current time is:", "%s", time_get_formatted());
}

void clock_command_sntp(int argc, char **argv)
{
    if (argc >= 2 && clock_cmd_equals(argv[1], "sync")) {
        clock_emit_ok("sntp: requesting re-synchronization against %s",
                      time_get_ntp_server());
        time_force_resync();
        return;
    }

    if (argc != 1) {
        clock_emit_usage("Usage: sntp|ntpsync [sync]");
        return;
    }

    clock_emit_heading("NTP Synchronization");
    clock_emit_field("Server:", "%s", time_get_ntp_server());
    clock_emit_field("Status:", "%s", time_is_synchronized() ? "synced" : "not synced");
    clock_emit_field("Local time:", "%s", time_get_formatted());
    clock_emit_muted("sntp sync forces a fresh NTP exchange");
}

void clock_command_rtc(int argc, char **argv)
{
    int64_t age;

    if (argc == 2 && clock_cmd_equals(argv[1], "anchor")) {
        if (!time_is_set() && !clock_rtc_is_stale()) {
            clock_emit_error("rtc: clock is unset (nothing to anchor)");
            return;
        }
        clock_rtc_anchor_now();
        clock_emit_ok("rtc: anchor written");
        return;
    }
    if (argc != 1) {
        clock_emit_usage("Usage: rtc [anchor]");
        return;
    }
    clock_emit_heading("RTC Backup");
    clock_emit_field("Source:", "%s", clock_rtc_source());
    clock_emit_field("State:", "%s",
                     clock_rtc_is_stale() ? "stale (last-known time)" :
                     time_is_set() ? "valid" : "unset");
    clock_emit_field("RTC counter:", "%llu us", (unsigned long long)clock_rtc_counter_us());
    age = clock_rtc_anchor_age_sec();
    if (age < 0) {
        clock_emit_field("Anchor:", "never written");
    } else {
        clock_emit_field("Anchor:", "%lld s ago", (long long)age);
    }
    {
        int ext = clock_rtc_ext_state();
        clock_emit_field("Ext chip:", "%s",
                         ext > 0 ? "present" : (ext == 0 ? "absent/unconfigured" : "not probed"));
    }
    clock_emit_field("SNTP:", "%s", time_is_synchronized() ? "synced" : "not synced");
    clock_emit_muted("rtc anchor forces an NVS anchor write now");
}

void clock_command_timezone(int argc, char **argv)
{
    if (argc == 1) {
        clock_emit_field("Timezone:", "%s", time_get_timezone_label());
        clock_emit_field("TZ string:", "%s", time_get_timezone());
        clock_emit_field("Local time:", "%s", time_get_formatted());
        clock_emit_muted("timezone <TZ> sets a POSIX timezone string");
        return;
    }

    if (argc != 2) {
        clock_emit_usage("Usage: timezone [TZ]");
        return;
    }

    time_set_timezone(argv[1]);
    clock_emit_ok("timezone: set to '%s'", time_get_timezone());
    clock_emit_field("Local time:", "%s", time_get_formatted());
}

/* ========================================================================
 * TIMER / STOPWATCH
 * ======================================================================== */

/** Options accepted anywhere on the `timer` line. */
typedef struct {
    bool bare;
    char var[P4_CONFIG_TIMER_NAME_BYTES + 64];
    bool has_var;
} clock_timer_opts_t;

/** Split `start|stop|lap|status`, `[name]`, `/b`, `/v:NAME` out of argv.
 *  @return 0 on success, 2 on a usage error (already reported). */
static int clock_timer_parse_opts(int argc, char **argv, const char **verb_out,
                                  const char **name_out, clock_timer_opts_t *opts_out)
{
    const char *verb = NULL;
    const char *name = NULL;
    int i;

    memset(opts_out, 0, sizeof(*opts_out));
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (arg[0] == '/') {
            if (clock_cmd_equals(arg, "/b")) {
                opts_out->bare = true;
            } else if (strncasecmp(arg, "/v:", 3) == 0 && strlen(arg) > 3 &&
                       strlen(arg) <= sizeof(opts_out->var) - 1) {
                snprintf(opts_out->var, sizeof(opts_out->var), "%s", arg + 3);
                opts_out->has_var = true;
            } else {
                clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
                return 2;
            }
        } else if (verb == NULL) {
            verb = arg;
        } else if (name == NULL) {
            name = arg;
        } else {
            clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
            return 2;
        }
    }
    if (verb == NULL) {
        clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
        return 2;
    }
    *verb_out = verb;
    *name_out = name;
    return 0;
}

/** Store the elapsed milliseconds into the `/v:NAME` variable.
 *  @return 0 on success, 1 when storage is unavailable or rejects the name. */
static int clock_timer_store_var(const clock_timer_opts_t *opts, int64_t elapsed_ms)
{
    char value[32];

    if (!opts->has_var) {
        return 0;
    }
    if (!(s_clock_ops_set && s_clock_ops.set_env != NULL)) {
        clock_emit_error("timer: variable storage is unavailable");
        return 1;
    }
    snprintf(value, sizeof(value), "%lld", (long long)elapsed_ms);
    if (s_clock_ops.set_env(opts->var, value) != 0) {
        clock_emit_error("timer: cannot store '%s' (bad name or table full)", opts->var);
        return 1;
    }
    return 0;
}

/** Render one run line; bare mode prints `name state ms laps` for pipes. */
static void clock_timer_print_run(const char *name, bool running, int64_t elapsed_ms,
                                  uint32_t laps, bool bare)
{
    if (bare) {
        char line[96];
        snprintf(line, sizeof(line), "%s %s %lld %lu", name, running ? "running" : "stopped",
                 (long long)elapsed_ms, (unsigned long)laps);
        clock_emit_text(line);
        clock_emit_text("\n");
    } else {
        clock_emit_field(name, "%s, %lld ms (%.2f s), %lu lap(s)", running ? "running" : "stopped",
                         (long long)elapsed_ms, (double)elapsed_ms / 1000.0,
                         (unsigned long)laps);
    }
}

int clock_command_timer(int argc, char **argv)
{
    const char *verb;
    const char *name;
    clock_timer_opts_t opts;
    int64_t elapsed_ms = 0;
    int rc;

    if (clock_timer_parse_opts(argc, argv, &verb, &name, &opts) != 0) {
        return 2;
    }

    if (clock_cmd_equals(verb, "start")) {
        rc = clock_timer_start(name);
        if (rc == 2) {
            clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
            return 2;
        }
        if (rc != 0) {
            clock_emit_error("timer: no free stopwatch slots (max %d)", P4_CONFIG_TIMER_SLOTS);
            return 1;
        }
        if (!opts.bare) {
            clock_emit_ok("timer: '%s' started", (name != NULL && name[0] != '\0') ? name : "default");
        }
        return 0;
    }

    if (clock_cmd_equals(verb, "stop")) {
        rc = clock_timer_stop(name, &elapsed_ms);
        if (rc == 2) {
            clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
            return 2;
        }
        if (rc != 0) {
            clock_emit_error("timer: no running run named '%s'",
                             (name != NULL && name[0] != '\0') ? name : "default");
            return 1;
        }
        if (clock_timer_store_var(&opts, elapsed_ms) != 0) {
            return 1;
        }
        if (!opts.bare) {
            clock_emit_ok("timer: '%s' stopped at %lld ms",
                          (name != NULL && name[0] != '\0') ? name : "default",
                          (long long)elapsed_ms);
        } else {
            char line[32];
            snprintf(line, sizeof(line), "%lld\n", (long long)elapsed_ms);
            clock_emit_text(line);
        }
        return 0;
    }

    if (clock_cmd_equals(verb, "lap")) {
        rc = clock_timer_lap(name, &elapsed_ms);
        if (rc == 2) {
            clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
            return 2;
        }
        if (rc != 0) {
            clock_emit_error("timer: no running run named '%s'",
                             (name != NULL && name[0] != '\0') ? name : "default");
            return 1;
        }
        if (clock_timer_store_var(&opts, elapsed_ms) != 0) {
            return 1;
        }
        if (!opts.bare) {
            clock_emit_ok("timer: '%s' lap at %lld ms",
                          (name != NULL && name[0] != '\0') ? name : "default",
                          (long long)elapsed_ms);
        } else {
            char line[32];
            snprintf(line, sizeof(line), "%lld\n", (long long)elapsed_ms);
            clock_emit_text(line);
        }
        return 0;
    }

    if (clock_cmd_equals(verb, "status")) {
        if (name != NULL) {
            bool running = false;
            uint32_t laps = 0;
            rc = clock_timer_status_at(name, esp_timer_get_time(), &running, &elapsed_ms, &laps);
            if (rc == 2) {
                clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
                return 2;
            }
            if (rc != 0) {
                clock_emit_error("timer: no run named '%s'", name);
                return 1;
            }
            if (clock_timer_store_var(&opts, elapsed_ms) != 0) {
                return 1;
            }
            if (!opts.bare) {
                clock_emit_heading("Stopwatch");
            }
            clock_timer_print_run(name, running, elapsed_ms, laps, opts.bare);
            return 0;
        }
        if (clock_timer_slot_count() == 0) {
            if (!opts.bare) {
                clock_emit_muted("(no timers)");
            }
            return 1;
        }
        if (!opts.bare) {
            clock_emit_heading("Stopwatch");
        }
        {
            int index = 0;
            char slot_name[P4_CONFIG_TIMER_NAME_BYTES + 1];
            bool running = false;
            int64_t slot_ms = 0;
            while (clock_timer_get_slot(index, slot_name, sizeof(slot_name),
                                        &running, &slot_ms)) {
                uint32_t laps = 0;
                (void)clock_timer_status_at(slot_name, esp_timer_get_time(), NULL, NULL, &laps);
                clock_timer_print_run(slot_name, running, slot_ms, laps, opts.bare);
                index++;
            }
        }
        return 0;
    }

    clock_emit_usage("Usage: timer|stopwatch start|stop|lap|status [name] [/b] [/v:NAME]");
    return 2;
}
