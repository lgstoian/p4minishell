/**
 * @file header_refresh.c
 * @brief Pure adaptive-cadence policy for the header status poll.
 */

#include "header_refresh.h"
#include "p4minishell_config.h"

/** Land just after the minute boundary so the clock never shows a stale minute. */
#define HEADER_CLOCK_EPSILON_MS 50

static uint32_t header_refresh_clamp(uint32_t interval)
{
    if (interval < P4_CONFIG_HEADER_REFRESH_MIN_MS) {
        return P4_CONFIG_HEADER_REFRESH_MIN_MS;
    }
    return interval;
}

uint32_t header_refresh_interval_ms(const header_refresh_state_t *state)
{
    uint32_t interval;

    if (state == NULL) {
        return P4_CONFIG_HEADER_REFRESH_IDLE_MS;
    }

    if (state->display_off) {
        return header_refresh_clamp(P4_CONFIG_HEADER_REFRESH_WAKE_MS);
    }
    if (state->busy) {
        return header_refresh_clamp(P4_CONFIG_HEADER_REFRESH_BUSY_MS);
    }
    if (state->wifi_connecting) {
        return header_refresh_clamp(P4_CONFIG_HEADER_REFRESH_CONNECTING_MS);
    }
    if (state->startup) {
        return header_refresh_clamp(P4_CONFIG_HEADER_REFRESH_STARTUP_MS);
    }

    interval = P4_CONFIG_HEADER_REFRESH_IDLE_MS;

    /* Exact HH:MM: wake at the next minute boundary when it is sooner. */
    if (state->clock_enabled && state->seconds_to_next_minute > 0) {
        uint32_t to_minute =
            (uint32_t)state->seconds_to_next_minute * 1000u + HEADER_CLOCK_EPSILON_MS;
        if (to_minute < interval) {
            interval = to_minute;
        }
    }

    /* Never sleep past the pending idle-display-off deadline. */
    if (state->ms_to_idle_off > 0 && (uint32_t)state->ms_to_idle_off < interval) {
        interval = (uint32_t)state->ms_to_idle_off;
    }

    return header_refresh_clamp(interval);
}
