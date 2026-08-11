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
 *
 * Setting the date/time adjusts the C-library clock; a later SNTP sync
 * overrides it, matching how DOS-era boxes behaved against an authoritative
 * source.
 */

#include "clock.h"
#include "p4minishell_config.h"
#include "esp_err.h"
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

void clock_command_timezone(int argc, char **argv)
{
    if (argc == 1) {
        clock_emit_field("Timezone:", "%s", time_get_timezone());
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
