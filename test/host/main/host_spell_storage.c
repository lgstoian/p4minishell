/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file host_spell_storage.c
 * @brief Host-lane backing for the two storage-backed spellchecker operations.
 *
 * `components/editor/editor_spell.c` is the hardware-free core: the wordlist
 * sets, the lookups and the tokenizer. Its two write paths (`spell learn` /
 * `spell forget`) end in the calls declared in editor_spell_internal.h, which
 * the firmware implements against the SD card in editor_spell_sd.c.
 *
 * The host lane has no SD card, so it provides those two operations here. They
 * report "unavailable", which is exactly what the core returns on a board with
 * no mounted card. Nothing is faked: no file, no dictionary and no hardware is
 * simulated, and the firmware never compiles this file.
 *
 * @module test/host
 * @layer test
 * @domain testing
 * @provides host-lane no-card backing for spell learn/forget
 * @status stable
 * @test tools/host_unit_run.py
 * @hard-rule host lane only - never part of the firmware image
 */

#include <stdbool.h>

#include "editor_spell_internal.h"

bool editor_spell_user_append(const char *norm)
{
    (void)norm;
    return false; /* No SD card on the host lane. */
}

bool editor_spell_user_remove(const char *norm)
{
    (void)norm;
    return false; /* No SD card on the host lane. */
}
