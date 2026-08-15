/**
 * @file applib_net.h
 * @brief Wi-Fi state group of the applib (state without owning the stack).
 *
 * Registered by `command_init()` so this component reads Wi-Fi state through
 * the table instead of including `networking.h`. Every hook is NULL-checked,
 * so the helpers degrade gracefully before registration.
 */

#ifndef P4MINISHELL_APPLIB_NET_H
#define P4MINISHELL_APPLIB_NET_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Registered by `command_init()`; every hook is NULL-checked before use. */
typedef struct {
    /** True when Wi-Fi is associated with an access point. */
    bool (*wifi_is_connected)(void);
    /** Read RSSI in dBm. Returns true on success, false when not associated. */
    bool (*wifi_get_rssi)(int *rssi_out);
    /** Human-readable Wi-Fi runtime state ("started", "connected", ...). */
    const char *(*wifi_state_string)(void);
} applib_net_ops_t;

/** Register (or clear, with NULL) the networking ops used by the helpers. */
void applib_register_net_ops(const applib_net_ops_t *ops);

/** True when Wi-Fi is associated (false before the ops table is registered). */
bool app_wifi_is_connected(void);

/**
 * Current RSSI in dBm. Returns 0 when not associated, the query failed, or
 * the ops table is not registered.
 */
int app_wifi_get_rssi(void);

/** Wi-Fi runtime state string ("n/a" before registration / when unknown). */
const char *app_wifi_state_string(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_NET_H */
