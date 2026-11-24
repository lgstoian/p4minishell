/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file modal_args.c
 * @brief Pure option parsers shared by the modal surfaces and the batch verbs.
 *
 * `/t:secs` and `/v:NAME` are parsed identically by every surface and by
 * `lock`/`wait` (batch sync), so the parsers live in their own hardware-free
 * translation unit: no LVGL, no display, no modal runtime, no storage. That is
 * what lets the host unit-test lane exercise them (and the batch verbs that
 * call them) natively, exactly as the firmware does.
 *
 * @module modal
 * @layer leaf
 * @domain ui
 * @provides modal option parsers (/t: seconds, /v: variable) + pure focus/progress/session selectors
 * @status stable
 * @test test/main/test_modal.c, test/main/test_svc.c, tools/host_modal_gate.py
 * @hard-rule pure string/state logic only - no LVGL, display, FreeRTOS or storage here
 */

#include "modal_surf.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Behaviour moved verbatim out of modal_surf.c: the historical spelling is a
 * `/t:` prefix (case-insensitive) with at least one trailing character, and
 * `secs` parses via atoi() so zero, negative and non-numeric values all yield
 * timeout 0 (wait forever). */
bool modal_parse_timeout_arg(const char *arg, uint32_t *timeout_ms_out)
{
    int secs;

    if (arg == NULL || timeout_ms_out == NULL) {
        return false;
    }
    if (strncasecmp(arg, "/t:", 3) != 0 || strlen(arg) <= 3) {
        return false;
    }
    secs = atoi(arg + 3);
    *timeout_ms_out = (secs > 0) ? (uint32_t)secs * 1000u : 0u;
    return true;
}

bool modal_parse_var_arg(const char *arg, const char **name_out)
{
    if (arg == NULL || name_out == NULL) {
        return false;
    }
    if (strncasecmp(arg, "/v:", 3) != 0 || strlen(arg) <= 3) {
        return false;
    }
    *name_out = arg + 3;
    return true;
}

/* -------------------------------------------------------------------------
 * PURE SELECTORS / STATE
 * -------------------------------------------------------------------------
 * Keeping these in the same hardware-free translation unit as the /t: and /v:
 * parsers is deliberate: the firmware surfaces and the host modal gate call
 * the identical implementation, so a host transition result is real evidence
 * about the code that ships, not a re-implementation.
 * ---------------------------------------------------------------------- */

int modal_focus_clamp(int index, int count)
{
    if (count <= 0) {
        return 0;
    }
    if (index < 0) {
        return 0;
    }
    if (index >= count) {
        return count - 1;
    }
    return index;
}

bool modal_focus_move(uint8_t key_code, int count, int current,
                      int page_step, int *out)
{
    int next;
    int page;

    if (out == NULL || count <= 0) {
        return false;
    }
    next = modal_focus_clamp(current, count);
    page = page_step > 0 ? page_step : 5;

    switch (key_code) {
    case MODAL_KEY_UP:      next -= 1; break;
    case MODAL_KEY_DOWN:    next += 1; break;
    case MODAL_KEY_LEFT:    next -= 1; break;
    case MODAL_KEY_RIGHT:   next += 1; break;
    case MODAL_KEY_PAGE_UP: next -= page; break;
    case MODAL_KEY_PAGE_DOWN: next += page; break;
    case MODAL_KEY_HOME:    next = 0; break;
    case MODAL_KEY_END:     next = count - 1; break;
    default:                return false;
    }

    next = modal_focus_clamp(next, count);
    if (next == modal_focus_clamp(current, count)) {
        return false;   /* no movement: caller must not repaint */
    }
    *out = next;
    return true;
}

uint8_t modal_progress_percent(uint32_t done, uint32_t total)
{
    uint64_t scaled;

    if (total == 0) {
        return 0;
    }
    if (done >= total) {
        return 100;
    }
    scaled = (uint64_t)done * 100u;
    return (uint8_t)(scaled / total);
}

void modal_session_stack_init(modal_session_stack_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->depth = 0;
}

int modal_session_stack_depth(const modal_session_stack_t *st)
{
    return st != NULL ? st->depth : 0;
}

bool modal_session_stack_empty(const modal_session_stack_t *st)
{
    return st == NULL || st->depth <= 0;
}

bool modal_session_stack_full(const modal_session_stack_t *st)
{
    return st != NULL && st->depth >= MODAL_SESSION_MAX;
}

const char *modal_session_stack_name(const modal_session_stack_t *st, int from_top)
{
    int idx;

    if (st == NULL || from_top < 0 || from_top >= st->depth) {
        return NULL;
    }
    idx = st->depth - 1 - from_top;
    return st->slot[idx].name;
}

int modal_session_stack_find(const modal_session_stack_t *st, const char *name)
{
    int i;

    if (st == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }
    for (i = st->depth - 1; i >= 0; i--) {
        if (st->slot[i].name != NULL && strcmp(st->slot[i].name, name) == 0) {
            return (st->depth - 1) - i;
        }
    }
    return -1;
}

bool modal_session_stack_push(modal_session_stack_t *st, const char *name)
{
    modal_session_slot_t *slot;

    if (st == NULL || name == NULL || name[0] == '\0' ||
        modal_session_stack_full(st)) {
        return false;
    }
    if (st->depth > 0) {
        st->slot[st->depth - 1].state = MODAL_SESSION_BACK;
    }
    slot = &st->slot[st->depth];
    slot->name = name;
    slot->focus = 0;
    slot->state = MODAL_SESSION_FORE;
    st->depth++;
    return true;
}

bool modal_session_stack_pop(modal_session_stack_t *st)
{
    if (modal_session_stack_empty(st)) {
        return false;
    }
    st->depth--;
    if (st->depth > 0) {
        st->slot[st->depth - 1].state = MODAL_SESSION_FORE;
    }
    return true;
}

bool modal_session_stack_suspend(modal_session_stack_t *st)
{
    if (modal_session_stack_empty(st)) {
        return false;
    }
    st->slot[st->depth - 1].state = MODAL_SESSION_BACK;
    return true;
}

bool modal_session_stack_resume(modal_session_stack_t *st)
{
    if (modal_session_stack_empty(st)) {
        return false;
    }
    st->slot[st->depth - 1].state = MODAL_SESSION_FORE;
    return true;
}

bool modal_session_stack_switch(modal_session_stack_t *st, const char *name)
{
    int from_top;
    int idx;
    int i;

    from_top = modal_session_stack_find(st, name);
    if (from_top < 0) {
        return false;
    }
    idx = st->depth - 1 - from_top;
    if (idx == st->depth - 1) {
        /* Already on top: just make sure it is foreground. */
        st->slot[idx].state = MODAL_SESSION_FORE;
        return true;
    }

    /* Rotate the named slot to the top without disturbing relative order of
     * the sessions that were above it (a stack, not a second scheduler). */
    for (i = idx; i < st->depth - 1; i++) {
        modal_session_slot_t tmp = st->slot[i];
        st->slot[i] = st->slot[i + 1];
        st->slot[i + 1] = tmp;
    }
    for (i = 0; i < st->depth; i++) {
        st->slot[i].state = MODAL_SESSION_BACK;
    }
    st->slot[st->depth - 1].state = MODAL_SESSION_FORE;
    return true;
}

int modal_session_stack_focus(const modal_session_stack_t *st, int from_top)
{
    if (st == NULL || from_top < 0 || from_top >= st->depth) {
        return -1;
    }
    return st->slot[st->depth - 1 - from_top].focus;
}

bool modal_session_stack_set_focus(modal_session_stack_t *st, int from_top, int focus)
{
    if (st == NULL || from_top < 0 || from_top >= st->depth || focus < 0) {
        return false;
    }
    st->slot[st->depth - 1 - from_top].focus = focus;
    return true;
}
