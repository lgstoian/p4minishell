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
 * @provides modal option parsers (/t: seconds, /v: variable)
 * @status stable
 * @test test/main/test_modal.c, test/main/test_svc.c
 * @hard-rule pure string parsing only - no LVGL, display or storage here
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
