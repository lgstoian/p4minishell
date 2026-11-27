/* Host mock of p4minishell_config.h - values copied from the shipped header. */
#ifndef POWER_GATE_CONFIG_H
#define POWER_GATE_CONFIG_H
#define P4_CONFIG_SHELL_TAG                         "power-gate"
#define P4_CONFIG_REBOOT_DELAY_MS                   100
#define P4_CONFIG_BATTERY_ATTEN                     0
#define P4_CONFIG_BATTERY_MIN_SLEEP_FREQ_MHZ        80
#define P4_CONFIG_BATTERY_ADC_SAMPLES               4
#define P4_CONFIG_BATTERY_ADC_EMA_ALPHA             0
#define P4_CONFIG_POWER_SLEEP_DEFAULT_SECS          60
#define P4_CONFIG_POWER_SLEEP_MAX_SECS              86400
#define P4_CONFIG_POWER_SLEEP_PRE_DELAY_MS          1
#define P4_CONFIG_POWER_LIGHT_SLEEP_SHUTDOWN_WIFI   1
#define P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS       86400
#ifndef P4_CONFIG_POWER_WAKE_GPIO
#define P4_CONFIG_POWER_WAKE_GPIO                   (-1)
#endif
#define P4_CONFIG_POWER_WAKE_LEVEL                  0
#define P4_CONFIG_POWER_WAKE_TOUCH                  1
#define P4_CONFIG_POWER_WAKE_KEYBOARD               1
#define P4_CONFIG_CLOCK_VALID_EPOCH                 1600000000
#define P4_CONFIG_HEADER_REFRESH_IDLE_MS            2000
#define P4_CONFIG_HEADER_REFRESH_WAKE_MS            150
#define P4_CONFIG_HEADER_REFRESH_BUSY_MS            1000
#define P4_CONFIG_HEADER_REFRESH_CONNECTING_MS      750
#define P4_CONFIG_HEADER_REFRESH_STARTUP_MS         500
#define P4_CONFIG_HEADER_REFRESH_MIN_MS             100
#define P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS          3000
#define P4_CONFIG_HEADER_BAT_LOW_PCT                20
#define P4_CONFIG_HEADER_BAT_CRIT_PCT               10
#define P4_CONFIG_HOSTNAME_DEFAULT                  "P4MINISHELL"
#define P4_CONFIG_ENV_NAME_BYTES                    32
#define P4_CONFIG_TIMEZONE_BYTES                    64
#define P4_CONFIG_NTP_SERVER                        "pool.ntp.org"
#endif
