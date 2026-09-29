// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file applib_msg.h
 * @brief Messaging group of the applib (events without owning sockets).
 *
 * Registered by `command_init()` so native apps publish/subscribe through
 * the netsvc service instead of including `networking.h`. Every hook is
 * NULL-checked, so the helpers degrade gracefully before registration.
 */
#ifndef P4MINISHELL_APPLIB_MSG_H
#define P4MINISHELL_APPLIB_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Registered by `command_init()`; every hook is NULL-checked before use. */
typedef struct {
    /** Publish (journals when offline). 0 ok, nonzero failed. */
    int (*msg_publish)(const char *topic, const uint8_t *payload, size_t len);
    /** Subscribe a filter (resent on reconnect). 0 ok, nonzero failed. */
    int (*msg_subscribe)(const char *filter);
    /** True while the broker session is up. */
    bool (*msg_connected)(void);
} applib_msg_ops_t;

/** Register (or clear, with NULL) the messaging ops used by the helpers. */
void applib_register_msg_ops(const applib_msg_ops_t *ops);

/** Publish a message (queued offline). False before registration/failure. */
bool app_msg_publish(const char *topic, const char *text);

/** Subscribe a topic filter. False before registration/failure. */
bool app_msg_subscribe(const char *filter);

/** True while the broker session is up (false before registration). */
bool app_msg_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_APPLIB_MSG_H */
