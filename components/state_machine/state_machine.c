/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file state_machine.c
 * @brief Lightweight finite state machine — static pool, no heap.
 */

#include "state_machine.h"
#include "p4heap.h"
#include <string.h>

/* Pool lives in PSRAM (internal fallback), allocated on first use, NOT in
 * .bss: internal SRAM is the scarce boot resource (see ai-context.md). The
 * `sm_pool` symbol stays a public pointer so the header contract holds. */
state_machine_t *sm_pool = NULL;

static bool sm_ensure_pool(void)
{
    if (sm_pool != NULL) {
        return true;
    }
    state_machine_t *pool = p4heap_calloc_any(P4_CONFIG_SM_MAX_INSTANCES, sizeof(*pool));
    if (pool == NULL) {
        return false;
    }
    /* calloc zeroes `current`; -1 marks an unused slot. */
    for (int i = 0; i < P4_CONFIG_SM_MAX_INSTANCES; i++) {
        pool[i].current = -1;
    }
    sm_pool = pool;
    return true;
}

/* ---- Helpers ---- */

static void sm_reset(state_machine_t *sm, void *ctx)
{
    sm->current = -1;
    sm->ctx = ctx;
    sm->in_transition = false;
    for (int s = 0; s < SM_MAX_STATES; s++) {
        sm->on_entry[s] = NULL;
        sm->on_exit[s] = NULL;
        for (int e = 0; e < SM_MAX_EVENTS; e++) {
            sm->transition_table[s][e] = (sm_state_t)(-1);
            sm->guards[s][e] = NULL;
        }
    }
}

/* ---- Public API ---- */

state_machine_t *sm_acquire(void *ctx)
{
    if (!sm_ensure_pool()) {
        return NULL;
    }
    for (int i = 0; i < P4_CONFIG_SM_MAX_INSTANCES; i++) {
        if (sm_pool[i].current < 0 && !sm_pool[i].in_transition) {
            sm_reset(&sm_pool[i], ctx);
            return &sm_pool[i];
        }
    }
    return NULL;
}

void sm_release(state_machine_t *sm)
{
    if (sm == NULL) return;
    sm->current = -1;
    sm->ctx = NULL;
    sm->in_transition = false;
}

void sm_set_transition(state_machine_t *sm, sm_state_t from, sm_event_t event, sm_state_t to)
{
    if (sm == NULL || from < 0 || from >= SM_MAX_STATES ||
        event < 0 || event >= SM_MAX_EVENTS || to < 0 || to >= SM_MAX_STATES) {
        return;
    }
    sm->transition_table[from][event] = to;
}

void sm_set_entry(state_machine_t *sm, sm_state_t state, sm_action_fn action)
{
    if (sm == NULL || state < 0 || state >= SM_MAX_STATES) return;
    sm->on_entry[state] = action;
}

void sm_set_exit(state_machine_t *sm, sm_state_t state, sm_action_fn action)
{
    if (sm == NULL || state < 0 || state >= SM_MAX_STATES) return;
    sm->on_exit[state] = action;
}

void sm_set_guard(state_machine_t *sm, sm_state_t from, sm_event_t event, sm_guard_fn guard)
{
    if (sm == NULL || from < 0 || from >= SM_MAX_STATES ||
        event < 0 || event >= SM_MAX_EVENTS) return;
    sm->guards[from][event] = guard;
}

void sm_set_initial(state_machine_t *sm, sm_state_t state)
{
    if (sm == NULL || state < 0 || state >= SM_MAX_STATES) return;
    sm->current = state;
    if (sm->on_entry[state]) {
        sm->on_entry[state](sm->ctx);
    }
}

bool sm_process(state_machine_t *sm, sm_event_t event)
{
    if (sm == NULL || sm->current < 0 || sm->current >= SM_MAX_STATES ||
        event < 0 || event >= SM_MAX_EVENTS || sm->in_transition) {
        return false;
    }

    sm_state_t next = sm->transition_table[sm->current][event];
    if (next < 0 || next >= SM_MAX_STATES) {
        return false;  /* No transition defined. */
    }

    /* Check guard. */
    if (sm->guards[sm->current][event]) {
        if (!sm->guards[sm->current][event](sm->ctx)) {
            return false;
        }
    }

    sm->in_transition = true;

    /* Exit current. */
    if (sm->on_exit[sm->current]) {
        sm->on_exit[sm->current](sm->ctx);
    }

    sm->current = next;

    /* Enter next. */
    if (sm->on_entry[next]) {
        sm->on_entry[next](sm->ctx);
    }

    sm->in_transition = false;
    return true;
}

sm_state_t sm_get_state(const state_machine_t *sm)
{
    if (sm == NULL) return (sm_state_t)(-1);
    return sm->current;
}

int sm_active_count(void)
{
    int count = 0;
    if (sm_pool == NULL) {
        return 0;
    }
    for (int i = 0; i < P4_CONFIG_SM_MAX_INSTANCES; i++) {
        if (sm_pool[i].current >= 0) count++;
    }
    return count;
}
