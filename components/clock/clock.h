#ifndef P4MINISHELL_CLOCK_H
#define P4MINISHELL_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
void time_init(void);
void time_start_sntp(void);
bool time_is_initialized(void);
struct tm time_get_local(void);
struct tm time_get_utc(void);
time_t time_get_unix(void);
uint32_t time_get_uptime_sec(void);
const char *time_get_formatted(void);
const char *time_get_formatted_utc(void);
bool time_is_synchronized(void);
void time_set_timezone(const char *tz_string);
const char *time_get_timezone(void);
#ifdef __cplusplus
}
#endif
#endif
