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

static char s_trap_msg[TRAP_MSG_BYTES]; /* TRAP_MSG content */

/* Per-bg-slot trap state (same pattern as other bg context) */
typedef struct {
    char handler[TRAP_DEPTH_MAX][TRAP_NAME_BYTES];
    int events[TRAP_DEPTH_MAX];      /**< BATCH_TRAP_* bit mask per handler. */
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
 * EVENT MASK + SHARED FIRE PATH (used by /on, /raise and the frame loop)
 * ======================================================================== */

int batch_trap_parse_events(const char *text)
{
    const char *cursor = text;
    int events = 0;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, ',');
        size_t len = (end != NULL) ? (size_t)(end - cursor) : strlen(cursor);

        if (len == 10 && strncasecmp(cursor, "errorlevel", 10) == 0) {
            events |= BATCH_TRAP_ERRORLEVEL;
        } else if (len == 4 && strncasecmp(cursor, "exit", 4) == 0) {
            events |= BATCH_TRAP_EXIT;
        } else if (len == 3 && strncasecmp(cursor, "int", 3) == 0) {
            events |= BATCH_TRAP_INT;
        } else {
            return 0;  /* unknown event word: refuse the whole install */
        }
        cursor = (end != NULL) ? end + 1 : NULL;
    }
    return events;
}

bool batch_trap_on_event(int events, const char *label)
{
    trap_bg_state_t *st = trap_state();

    if (events == 0 || label == NULL || label[0] == '\0') {
        return false;
    }
    if (st->level >= TRAP_DEPTH_MAX) {
        shell_print_error("trap: handler nesting too deep (max %d)", TRAP_DEPTH_MAX);
        batch_set_errorlevel(1);
        return false;
    }
    snprintf(st->handler[st->level], TRAP_NAME_BYTES, "%s", label);
    st->events[st->level] = events;
    st->level++;
    batch_set_errorlevel(0);
    return true;
}

bool batch_trap_fire_event(int event)
{
    trap_bg_state_t *st = trap_state();
    char label[TRAP_NAME_BYTES];
    char code_buf[16];

    if (event == 0 || st->level <= 0) {
        return false;
    }
    if ((st->events[st->level - 1] & event) == 0 ||
        st->handler[st->level - 1][0] == '\0') {
        return false;
    }

    /* Copy out BEFORE the one-shot pop clears the slot: batch_goto_label
     * refuses an empty label, so passing the live slot would silently drop
     * the jump. */
    snprintf(label, sizeof(label), "%s", st->handler[st->level - 1]);
    snprintf(code_buf, sizeof(code_buf), "%d", batch_get_errorlevel());
    (void)shell_env_set("TRAP_CODE", code_buf);
    if (event & BATCH_TRAP_INT) {
        snprintf(s_trap_msg, sizeof(s_trap_msg), "int, errorlevel %s", code_buf);
    } else if (event & BATCH_TRAP_EXIT) {
        snprintf(s_trap_msg, sizeof(s_trap_msg), "exit, errorlevel %s", code_buf);
    } else {
        /* Existing documented contract for the default errorlevel trap. */
        snprintf(s_trap_msg, sizeof(s_trap_msg), "errorlevel %s", code_buf);
    }
    (void)shell_env_set("TRAP_MSG", s_trap_msg);

    /* One-shot: pop before the jump so an error inside the handler cannot
     * re-enter the same handler forever. */
    st->handler[st->level - 1][0] = '\0';
    st->events[st->level - 1] = 0;
    st->level--;

    batch_goto_label(label);
    batch_set_errorlevel(0);
    return true;
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
            /* ERR / ERRMSG are read-only virtual slots (see batch_env.c) and
             * refuse shell_env_set(). Use ordinary variables so the values are
             * actually observable by the script. */
            snprintf(code_buf, sizeof(code_buf), "%d", code);
            (void)shell_env_set("TRAP_CODE", code_buf);
            trap_set_message(msg);
            batch_set_errorlevel(code);
            return;
        }

        /* /on EVENT[,EVENT...] :label — subscribe a handler to explicit
         * events instead of the implicit errorlevel-only default. */
        if (flag == 'O' && argc >= 4) {
            int events = batch_trap_parse_events(argv[2]);
            const char *label = argv[3];

            if (events == 0) {
                shell_print_error("trap /on: expected errorlevel|exit|int");
                batch_set_errorlevel(2);
                return;
            }
            if (label[0] != ':') {
                shell_print_error("trap /on: handler must be a :label");
                batch_set_errorlevel(2);
                return;
            }
            (void)batch_trap_on_event(events, label);
            return;
        }

        /* /clear - remove handler */
        if (flag == 'C') {
            trap_bg_state_t *st = trap_state();
            if (st->level > 0) {
                st->level--;
                st->handler[st->level][0] = '\0';
                st->events[st->level] = 0;
            }
            batch_set_errorlevel(0);
            return;
        }

        shell_print_error("trap: unknown switch /%s", argv[1] + 1);
        batch_set_errorlevel(2);
        return;
    }

    /* Install handler: trap :label (implicit errorlevel event). */
    const char *label = argv[1];

    if (label[0] != ':') {
        shell_print_error("trap: handler must be a :label");
        batch_set_errorlevel(2);
        return;
    }
    (void)batch_trap_on_event(BATCH_TRAP_ERRORLEVEL, label);
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
        st->events[st->level] = 0;
    }
}

void trap_set_message(const char *msg)
{
    if (msg != NULL) {
        snprintf(s_trap_msg, sizeof(s_trap_msg), "%s", msg);
        (void)shell_env_set("TRAP_MSG", s_trap_msg);
    }
}

bool batch_trap_maybe_fire(void)
{
    /* The default frame-loop hook: only a handler subscribed to ERRORLEVEL
     * fires, exactly as before. /on exit and /on int are fired explicitly by
     * the frame loop through batch_trap_fire_event(). */
    if (batch_get_errorlevel() < 2) {
        return false;
    }
    return batch_trap_fire_event(BATCH_TRAP_ERRORLEVEL);
}

void trap_init(void)
{
    s_trap_msg[0] = '\0';
    if (s_trap_bg != NULL) {
        memset(s_trap_bg, 0, P4_CONFIG_BG_TASKS * sizeof(*s_trap_bg));
    } else {
        s_trap_bg = calloc(P4_CONFIG_BG_TASKS, sizeof(*s_trap_bg));
    }
}
