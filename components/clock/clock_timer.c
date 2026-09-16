/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file clock_timer.c
 * @brief Stopwatch slots for the `timer` command (HP palmtop stopwatch parity).
 *
 * A small fixed table of named runs. Each slot holds a name, a running flag,
 * and accumulated microseconds. The `_at` core takes an explicit timestamp so
 * the unit tests stay deterministic without touching hardware timers; the
 * public wrappers sample `esp_timer_get_time()`.
 *
 * This file is pure logic: no transcript, no environment, no batch engine.
 * Rendering and `/v:NAME` storage live in `clock_commands.c` (which reaches
 * the environment through the registered host ops table), and ERRORLEVEL
 * mapping lives in the `command.c` dispatcher.
 */

#include "clock.h"
#include "p4minishell_config.h"
#include "esp_timer.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#ifndef P4_CONFIG_TIMER_SLOTS
#define P4_CONFIG_TIMER_SLOTS 8
#endif

#ifndef P4_CONFIG_TIMER_NAME_BYTES
#define P4_CONFIG_TIMER_NAME_BYTES 16
#endif

/** One stopwatch run. `elapsed_us` freezes while stopped; while running the
 *  live total is `elapsed_us + (now_us - start_us`). */
typedef struct {
    bool in_use;
    bool running;
    char name[P4_CONFIG_TIMER_NAME_BYTES + 1];
    int64_t start_us;
    int64_t elapsed_us;
    uint32_t laps;
} clock_timer_slot_t;

static clock_timer_slot_t s_timer_slots[P4_CONFIG_TIMER_SLOTS];

/** Default slot name used when the command omits `[name]`. */
#define CLOCK_TIMER_DEFAULT_NAME "default"

/** Bounded case-insensitive name compare (names are short by construction). */
static bool clock_timer_name_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncasecmp(a, b, P4_CONFIG_TIMER_NAME_BYTES + 1) == 0;
}

/** Find the slot holding @p name, or NULL when no run uses it. */
static clock_timer_slot_t *clock_timer_lookup(const char *name)
{
    int i;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < P4_CONFIG_TIMER_SLOTS; i++) {
        if (s_timer_slots[i].in_use && clock_timer_name_equals(s_timer_slots[i].name, name)) {
            return &s_timer_slots[i];
        }
    }
    return NULL;
}

/** Find a free slot, or NULL when the table is full. */
static clock_timer_slot_t *clock_timer_alloc(void)
{
    int i;

    for (i = 0; i < P4_CONFIG_TIMER_SLOTS; i++) {
        if (!s_timer_slots[i].in_use) {
            return &s_timer_slots[i];
        }
    }
    return NULL;
}

/** Validate a timer name (non-empty, fits the slot buffer). */
static bool clock_timer_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    return strlen(name) <= (size_t)P4_CONFIG_TIMER_NAME_BYTES;
}

int clock_timer_start_at(const char *name, int64_t now_us)
{
    clock_timer_slot_t *slot;

    if (name == NULL || name[0] == '\0') {
        name = CLOCK_TIMER_DEFAULT_NAME;
    }
    if (!clock_timer_name_valid(name)) {
        return 2;
    }
    slot = clock_timer_lookup(name);
    if (slot == NULL) {
        slot = clock_timer_alloc();
        if (slot == NULL) {
            return 1;
        }
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->in_use = true;
    }
    /* Starting an existing run restarts it from zero (documented). */
    slot->running = true;
    slot->start_us = now_us;
    slot->elapsed_us = 0;
    slot->laps = 0;
    return 0;
}

int clock_timer_stop_at(const char *name, int64_t now_us, int64_t *elapsed_ms_out)
{
    clock_timer_slot_t *slot;
    int64_t total_us;

    if (name == NULL || name[0] == '\0') {
        name = CLOCK_TIMER_DEFAULT_NAME;
    }
    if (!clock_timer_name_valid(name)) {
        return 2;
    }
    slot = clock_timer_lookup(name);
    if (slot == NULL || !slot->running) {
        return 1;
    }
    total_us = slot->elapsed_us + (now_us - slot->start_us);
    if (total_us < 0) {
        total_us = 0;
    }
    slot->elapsed_us = total_us;
    slot->running = false;
    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = total_us / 1000;
    }
    return 0;
}

int clock_timer_lap_at(const char *name, int64_t now_us, int64_t *elapsed_ms_out)
{
    clock_timer_slot_t *slot;
    int64_t total_us;

    if (name == NULL || name[0] == '\0') {
        name = CLOCK_TIMER_DEFAULT_NAME;
    }
    if (!clock_timer_name_valid(name)) {
        return 2;
    }
    slot = clock_timer_lookup(name);
    if (slot == NULL || !slot->running) {
        return 1;
    }
    total_us = slot->elapsed_us + (now_us - slot->start_us);
    if (total_us < 0) {
        total_us = 0;
    }
    if (slot->laps < UINT32_MAX) {
        slot->laps++;
    }
    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = total_us / 1000;
    }
    return 0;
}

int clock_timer_status_at(const char *name, int64_t now_us, bool *running_out,
                          int64_t *elapsed_ms_out, uint32_t *laps_out)
{
    clock_timer_slot_t *slot;
    int64_t total_us;

    if (name == NULL || name[0] == '\0') {
        name = CLOCK_TIMER_DEFAULT_NAME;
    }
    if (!clock_timer_name_valid(name)) {
        return 2;
    }
    slot = clock_timer_lookup(name);
    if (slot == NULL) {
        return 1;
    }
    total_us = slot->elapsed_us;
    if (slot->running) {
        total_us += now_us - slot->start_us;
        if (total_us < 0) {
            total_us = 0;
        }
    }
    if (running_out != NULL) {
        *running_out = slot->running;
    }
    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = total_us / 1000;
    }
    if (laps_out != NULL) {
        *laps_out = slot->laps;
    }
    return 0;
}

int clock_timer_slot_count(void)
{
    int i;
    int count = 0;

    for (i = 0; i < P4_CONFIG_TIMER_SLOTS; i++) {
        if (s_timer_slots[i].in_use) {
            count++;
        }
    }
    return count;
}

bool clock_timer_get_slot(int index, char *name_out, size_t name_size,
                          bool *running_out, int64_t *elapsed_ms_out)
{
    int64_t now_us;
    int64_t total_us;
    int seen = 0;
    int i;

    if (name_out == NULL || name_size == 0) {
        return false;
    }
    for (i = 0; i < P4_CONFIG_TIMER_SLOTS; i++) {
        if (!s_timer_slots[i].in_use) {
            continue;
        }
        if (seen == index) {
            now_us = esp_timer_get_time();
            total_us = s_timer_slots[i].elapsed_us;
            if (s_timer_slots[i].running) {
                total_us += now_us - s_timer_slots[i].start_us;
                if (total_us < 0) {
                    total_us = 0;
                }
            }
            snprintf(name_out, name_size, "%s", s_timer_slots[i].name);
            if (running_out != NULL) {
                *running_out = s_timer_slots[i].running;
            }
            if (elapsed_ms_out != NULL) {
                *elapsed_ms_out = total_us / 1000;
            }
            return true;
        }
        seen++;
    }
    return false;
}

int clock_timer_start(const char *name)
{
    return clock_timer_start_at(name, esp_timer_get_time());
}

int clock_timer_stop(const char *name, int64_t *elapsed_ms_out)
{
    return clock_timer_stop_at(name, esp_timer_get_time(), elapsed_ms_out);
}

int clock_timer_lap(const char *name, int64_t *elapsed_ms_out)
{
    return clock_timer_lap_at(name, esp_timer_get_time(), elapsed_ms_out);
}
