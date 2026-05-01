#include <time.h>
#include "clock.h"
#include "p4minishell_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define CLOCK_TAG P4_CONFIG_SHELL_TAG

static bool s_initialized = false;
static bool s_synchronized = false;
static char s_tz_string[64] = "UTC";
static char s_time_buf[64];
static char s_time_utc_buf[64];
static int64_t s_boot_timestamp_us;

static void clock_sntp_cb(struct timeval *tv)
{
    (void)tv;
    s_synchronized = true;
    ESP_LOGI(CLOCK_TAG, "Time synchronized via NTP");
}

void time_init(void)
{
    if (s_initialized) return;
    s_boot_timestamp_us = esp_timer_get_time();

    /* SNTP init is deferred — esp_sntp_init() requires the lwIP TCP/IP
     * stack to be fully running. networking_init() starts Wi-Fi in a
     * background task, so lwIP may not be ready yet when time_init()
     * is called. The SNTP client will be started when Wi-Fi connects. */
    setenv("TZ", s_tz_string, 1);
    tzset();
    s_initialized = true;
    ESP_LOGI(CLOCK_TAG, "Clock module initialized (SNTP deferred)");
}

/** Start SNTP client — called when lwIP is confirmed ready. */
void time_start_sntp(void)
{
    if (s_synchronized) return;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(clock_sntp_cb);
    esp_sntp_init();
    ESP_LOGI(CLOCK_TAG, "SNTP client started");
}

bool time_is_initialized(void) { return s_initialized; }

struct tm time_get_local(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    return ti;
}

struct tm time_get_utc(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    gmtime_r(&now, &ti);
    return ti;
}

time_t time_get_unix(void) { time_t now; time(&now); return now; }

uint32_t time_get_uptime_sec(void)
{
    int64_t u = esp_timer_get_time() - s_boot_timestamp_us;
    return (uint32_t)(u / 1000000ULL);
}

const char *time_get_formatted(void)
{
    struct tm ti = time_get_local();
    snprintf(s_time_buf, sizeof(s_time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return s_time_buf;
}

const char *time_get_formatted_utc(void)
{
    struct tm ti = time_get_utc();
    snprintf(s_time_utc_buf, sizeof(s_time_utc_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return s_time_utc_buf;
}

bool time_is_synchronized(void) { return s_synchronized; }

void time_set_timezone(const char *tz)
{
    if (!tz) return;
    snprintf(s_tz_string, sizeof(s_tz_string), "%s", tz);
    setenv("TZ", s_tz_string, 1);
    tzset();
    ESP_LOGI(CLOCK_TAG, "Timezone: %s", s_tz_string);
}

const char *time_get_timezone(void) { return s_tz_string; }
