/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file state_machine.h
 * @brief Lightweight finite state machine.
 *
 * Pool storage is allocated once, lazily, from PSRAM (internal RAM as a
 * fallback) rather than BSS. State/event counts are bounded by config macros.
 * Each instance has entry/exit/transition actions and optional guards.
 */

#ifndef P4MINISHELL_STATE_MACHINE_H
#define P4MINISHELL_STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>
#include "p4minishell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum states/events for a single state machine instance. */
#define SM_MAX_STATES   P4_CONFIG_SM_MAX_STATES
#define SM_MAX_EVENTS   P4_CONFIG_SM_MAX_EVENTS

/** State handle (small integer). */
typedef int sm_state_t;

/** Event handle (small integer). */
typedef int sm_event_t;

/** Action function: called on entry/exit/transition. */
typedef void (*sm_action_fn)(void *ctx);

/** Guard function: return true to allow transition, false to block. */
typedef bool (*sm_guard_fn)(void *ctx);

/** State machine instance. */
typedef struct {
    sm_state_t current;                          /**< Current state. */
    sm_state_t transition_table[SM_MAX_STATES][SM_MAX_EVENTS]; /**< Next state, -1 = no transition. */
    sm_action_fn on_entry[SM_MAX_STATES];        /**< Called on entering a state. */
    sm_action_fn on_exit[SM_MAX_STATES];         /**< Called on leaving a state. */
    sm_guard_fn guards[SM_MAX_STATES][SM_MAX_EVENTS]; /**< Optional guards. */
    void *ctx;                                   /**< User context for callbacks. */
    bool in_transition;                          /**< Reentrancy guard. */
} state_machine_t;

/** Pool of state machine instances.
 *  Backed by PSRAM (internal RAM fallback) and allocated on first use, so it
 *  never occupies the scarce boot-time internal BSS. */
extern state_machine_t *sm_pool;

/** Initialize a state machine instance from the pool.
 *  @return instance pointer, or NULL if pool full. */
state_machine_t *sm_acquire(void *ctx);

/** Release a state machine instance back to the pool. */
void sm_release(state_machine_t *sm);

/** Configure a transition: from state, on event, to next_state. */
void sm_set_transition(state_machine_t *sm, sm_state_t from, sm_event_t event, sm_state_t to);

/** Set entry action for a state. */
void sm_set_entry(state_machine_t *sm, sm_state_t state, sm_action_fn action);

/** Set exit action for a state. */
void sm_set_exit(state_machine_t *sm, sm_state_t state, sm_action_fn action);

/** Set a guard for a transition. */
void sm_set_guard(state_machine_t *sm, sm_state_t from, sm_event_t event, sm_guard_fn guard);

/** Set the initial state. */
void sm_set_initial(state_machine_t *sm, sm_state_t state);

/** Process an event.
 *  @return true if transition occurred, false if blocked or no transition. */
bool sm_process(state_machine_t *sm, sm_event_t event);

/** Get the current state. */
sm_state_t sm_get_state(const state_machine_t *sm);

/** Number of active instances. */
int sm_active_count(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_STATE_MACHINE_H */
