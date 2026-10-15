/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_trap.c
 * @brief `trap` verb — structured error handling.
 *
 * Runtime failures (bad `set /a`, failed `call`, `dim` bounds violation)
 * currently either continue silently or abort the script. `trap` redirects
 * them to a label, scoped per call frame so a subroutine can recover and
 * `return` cleanly.
 *
 * Implementation: a per-frame trap handler label stored in the batch task
 * context. When an error condition is detected (errorlevel >= 2 from certain
 * verbs, or manual `trap /raise`), the handler is invoked as a goto.
 */

#include "batch.h"
#include "shell.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRAP_MSG_BYTES   P4_CONFIG_TRAP_MSG_BYTES
#define TRAP_DEPTH_MAX   P4_CONFIG_TRAP_DEPTH_MAX
#define TRAP_NAME_BYTES  P4_CONFIG_ENV_NAME_BYTES

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */
/* Trap handler stack per batch task context. We extend the batch_task_ctx_t
 * via the existing mechanism: trap state is stored in the global foreground
 * context (s_fg_ctx) or bg context, mirroring how errorlevel/goto work.
 *
 * Since batch_task_ctx_t is defined in batch.c and we can't modify it from
 * here without adding fields, we use a small static array keyed by bg slot.
 * For the foreground task (slot 0) we use a static variable. */

/* Trap handler label (the label to jump to on error) */
static char s_trap_handler[TRAP_DEPTH_MAX][TRAP_NAME_BYTES];
static int s_trap_level = 0;       /* current frame depth for fg */
static char s_trap_msg[TRAP_MSG_BYTES]; /* %ERRMSG% content */

/* Per-bg-slot trap state (same pattern as other bg context) */
typedef struct {
    char handler[TRAP_DEPTH_MAX][TRAP_NAME_BYTES];
    int level;
} trap_bg_state_t;

static trap_bg_state_t *s_trap_bg = NULL;

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static trap_bg_state_t *trap_state(void)
{
    int slot = batch_bg_current_slot();
    if (slot > 0 && s_trap_bg != NULL && slot < P4_CONFIG_BG_TASKS) {
        return &s_trap_bg[slot];
    }
    /* Use a slot-0 wrapper static (fg) */
    static trap_bg_state_t fg_wrap;
    return &fg_wrap;
}

/* ========================================================================
 * VERB IMPLEMENTATION
 * ======================================================================== */

void shell_command_trap(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage(
            "Usage: trap :label                 install handler for this frame\n"
            "       trap /on errorlevel|exit|int\n"
            "       trap /raise CODE \"message\"  manual raise\n"
            "       trap /clear                 remove handler");
        batch_set_errorlevel(2);
        return;
    }

    if (argv[1][0] == '/' && argv[1][1] != '\0') {
        char flag = (char)toupper((unsigned char)argv[1][1]);

        /* /raise CODE "message" - manual raise */
        if (flag == 'R' && argc >= 3) {
            int code = (int)strtol(argv[2], NULL, 10);
            const char *msg = (argc >= 4) ? argv[3] : "trap raised";
            char code_buf[12];
            snprintf(code_buf, sizeof(code_buf), "%d", code);
            shell_env_set("ERR", code_buf);
            shell_env_set("ERRMSG", msg);
            batch_set_errorlevel(code);
            /* Would trigger handler if installed - handled by the engine */
            return;
        }

        /* /clear - remove handler */
        if (flag == 'C') {
            trap_bg_state_t *st = trap_state();
            if (st->level > 0) {
                st->level--;
                st->handler[st->level][0] = '\0';
            }
            batch_set_errorlevel(0);
            return;
        }

        shell_print_error("trap: unknown switch /%s", argv[1] + 1);
        batch_set_errorlevel(2);
        return;
    }

    /* Install handler: trap :label */
    const char *label = argv[1];
    if (label[0] != ':') {
        shell_print_error("trap: handler must be a :label");
        batch_set_errorlevel(2);
        return;
    }

    trap_bg_state_t *st = trap_state();
    if (st->level >= TRAP_DEPTH_MAX) {
        shell_print_error("trap: handler nesting too deep (max %d)", TRAP_DEPTH_MAX);
        batch_set_errorlevel(1);
        return;
    }

    snprintf(st->handler[st->level], TRAP_NAME_BYTES, "%s", label);
    st->level++;
    batch_set_errorlevel(0);
}

/* ========================================================================
 * ENGINE HOOK (called from batch.c dispatch when errorlevel is set)
 * ======================================================================== */

/* These functions are called by the batch engine to check/install/clear
 * trap handlers. Declared in batch.h for use by the dispatcher. */

bool trap_handler_installed(void)
{
    trap_bg_state_t *st = trap_state();
    return st->level > 0 && st->handler[st->level - 1][0] != '\0';
}

const char *trap_handler_label(void)
{
    trap_bg_state_t *st = trap_state();
    if (st->level > 0) {
        return st->handler[st->level - 1];
    }
    return NULL;
}

void trap_handler_pop(void)
{
    trap_bg_state_t *st = trap_state();
    if (st->level > 0) {
        st->level--;
        st->handler[st->level][0] = '\0';
    }
}

void trap_set_message(const char *msg)
{
    if (msg != NULL) {
        snprintf(s_trap_msg, sizeof(s_trap_msg), "%s", msg);
        shell_env_set("ERRMSG", s_trap_msg);
    }
}

void trap_init(void)
{
    memset(s_trap_handler, 0, sizeof(s_trap_handler));
    s_trap_level = 0;
    s_trap_msg[0] = '\0';
}
