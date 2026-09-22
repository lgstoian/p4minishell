/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file screen_commands.c
 * @brief The `screen` verb: run declarative UI screens from `.FRM` files.
 *
 * A `.FRM` file is one screen described as flat `KEY=VALUE` lines (the same
 * shape as `APPS/<APP>.APPINFO`, read through the shared `storage_ini`
 * core — no second INI parser). `screen run` translates the keys into an
 * argument vector and calls the existing `dialog` / `list` / `ask` / `form`
 * / `menu` implementations, so there is exactly one copy of every surface:
 * the file only declares, the verbs still render. Results land in the same
 * environment variables with the same ERRORLEVEL contract as the verbs.
 *
 * A multi-screen app is a batch file: several `screen run` calls branched
 * with `if errorlevel` / `goto`, exactly like hand-rolled modal sequences.
 * The format reference lives in `batch.md` §13.
 *
 * For input wizards larger than a couple of screens, `screen flow <file.flow>`
 * runs a declarative *flow*: a flat `KEY=VALUE` navigation graph whose nodes
 * point at `.FRM` files (so no screen-rendering code is duplicated). The flow
 * only routes; the batch file that invoked it acts on the collected variables
 * and the final ERRORLEVEL. See `batch.md` §13.1.
 */

#include "command.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ansi_palette.h"
#include "batch.h"
#include "modal_surf.h"
#include "shell.h"
#include "storage.h"

/** Maximum `items=` / `buttons=` entries one `screen run` accepts. */
#define SCREEN_LIST_MAX 32

/** Scratch for one `KEY=VALUE` read from the screen file. */
#define SCREEN_VALUE_BYTES P4_CONFIG_ENV_VALUE_BYTES

/* The flow table is packed into fixed-width structs; a raised config cap must
 * fail the build rather than silently truncate a step/rule. */
_Static_assert(P4_CONFIG_SCREEN_FLOW_ID_BYTES <= COMMAND_FLOW_ID_BYTES,
               "P4_CONFIG_SCREEN_FLOW_ID_BYTES must fit COMMAND_FLOW_ID_BYTES");
_Static_assert(P4_CONFIG_ENV_NAME_BYTES + 8 <= COMMAND_FLOW_TOKEN_BYTES,
               "flow token width must fit COMMAND_FLOW_TOKEN_BYTES");
_Static_assert(P4_CONFIG_ENV_NAME_BYTES <= COMMAND_FLOW_TARGET_BYTES,
               "flow target width must fit COMMAND_FLOW_TARGET_BYTES");

/**
 * Split @p text on @p delim into @p store, recording up to @p max entries in
 * @p out (pure, unit-tested). Empty entries are dropped, so `a||b` yields two
 * items. Surrounding spaces/tabs are trimmed per entry.
 *
 * @return The entry count (0 when @p text is NULL/empty), or -1 when @p store
 *         cannot hold the split.
 */
int screen_split_list(const char *text, char delim, char *store, size_t store_size,
                      const char **out, int max)
{
    const char *cursor;
    int count = 0;
    size_t used = 0;

    if (store == NULL || store_size == 0 || out == NULL || max <= 0) {
        return -1;
    }
    if (text == NULL || *text == '\0') {
        return 0;
    }
    cursor = text;
    while (*cursor != '\0' && count < max) {
        const char *end = cursor;
        const char *first;
        const char *last;
        size_t len;

        while (*end != '\0' && *end != delim) {
            end++;
        }
        first = cursor;
        while (first < end && (*first == ' ' || *first == '\t')) {
            first++;
        }
        last = end;
        while (last > first && (last[-1] == ' ' || last[-1] == '\t')) {
            last--;
        }
        len = (size_t)(last - first);
        if (len > 0) {
            if (used + len + 1 > store_size) {
                return -1;
            }
            memcpy(store + used, first, len);
            store[used + len] = '\0';
            out[count++] = store + used;
            used += len + 1;
        }
        cursor = (*end == delim) ? end + 1 : end;
    }
    /* Trailing entries past @p max are ignored (callers cap by count). */
    return count;
}

/** Read one screen key; empty (not missing) when absent. Never fails. */
static void screen_read_key(const char *resolved, const char *key,
                            char *value, size_t value_size)
{
    if (storage_ini_file_get(resolved, key, value, value_size) != ESP_OK) {
        value[0] = '\0';
    }
}

/** True for `1`/`yes`/`true`/`on` (any case), false for anything else. */
static bool screen_is_truthy(const char *text)
{
    return text != NULL &&
           (shell_text_equals_ignore_case(text, "1") ||
            shell_text_equals_ignore_case(text, "yes") ||
            shell_text_equals_ignore_case(text, "true") ||
            shell_text_equals_ignore_case(text, "on"));
}

/** Append a `/t:<secs>` timeout argument when the screen sets one. */
static void screen_maybe_timeout(const char *timeout_text, char *timeout_arg,
                                 size_t timeout_size, char **slot)
{
    long secs = (timeout_text != NULL && *timeout_text != '\0')
                    ? strtol(timeout_text, NULL, 10)
                    : 0;

    if (secs > 0) {
        snprintf(timeout_arg, timeout_size, "/t:%ld", secs);
        *slot = timeout_arg;
    } else {
        *slot = NULL;
    }
}

/* ========================================================================
 * DECLARATIVE FLOWS (`screen flow <file.flow>`)
 * ========================================================================
 * A flow is a flat `KEY=VALUE` navigation graph (read through the shared
 * `storage_ini_file_foreach`), whose nodes point at `.FRM` files rendered by
 * the existing `screen run`. The engine only routes: it shows a screen, reads
 * the result (ERRORLEVEL + the step's `var`), and picks the next step. The
 * batch file that invoked the flow acts on the collected variables and the
 * final ERRORLEVEL. See `batch.md` §13.1.
 */

/** One screen node of a flow. */
typedef struct {
    char id[P4_CONFIG_SCREEN_FLOW_ID_BYTES];
    char screen[P4_CONFIG_SD_PATH_BYTES];
    char var[P4_CONFIG_ENV_NAME_BYTES];
} screen_flow_step_t;

/** A whole parsed flow (heap-allocated: the driver runs on the recursive
 *  command path). */
typedef struct {
    char start[P4_CONFIG_SCREEN_FLOW_ID_BYTES];
    char title[SCREEN_VALUE_BYTES];
    screen_flow_step_t *steps;
    int step_count;
    screen_flow_rule_t *rules;
    int rule_count;
    bool overflow;
    bool bad_token;
} screen_flow_t;

const char *screen_flow_select(const screen_flow_rule_t *rules, int rule_count,
                               const char *step, int errorlevel,
                               const char *var_value)
{
    int i;

    if (rules == NULL || step == NULL || rule_count <= 0) {
        return NULL;
    }
    for (i = 0; i < rule_count; i++) {
        const char *token = rules[i].token;

        if (!shell_text_equals_ignore_case(rules[i].step, step)) {
            continue;
        }
        if (token[0] == '*' && token[1] == '\0') {
            return rules[i].target;
        }
        if (strncasecmp(token, "err:", 4) == 0) {
            char *end = NULL;
            long want;

            if (token[4] == '\0') {
                continue;   /* `err:` with no number never matches */
            }
            want = strtol(token + 4, &end, 10);
            if (end != token + 4 && *end == '\0' && (int)want == errorlevel) {
                return rules[i].target;
            }
            continue;
        }
        if (strncasecmp(token, "val:", 4) == 0) {
            if (token[4] != '\0' && var_value != NULL && var_value[0] != '\0' &&
                shell_text_equals_ignore_case(token + 4, var_value)) {
                return rules[i].target;
            }
            continue;
        }
        /* An unknown token never matches (the collector flags it). */
    }
    return NULL;
}

/** Find a step by id (case-insensitive), or NULL. */
static screen_flow_step_t *screen_flow_find_step(screen_flow_t *flow, const char *id)
{
    int i;

    for (i = 0; i < flow->step_count; i++) {
        if (shell_text_equals_ignore_case(flow->steps[i].id, id)) {
            return &flow->steps[i];
        }
    }
    return NULL;
}

/** Find-or-add a step (bounded by P4_CONFIG_SCREEN_FLOW_STEPS). */
static screen_flow_step_t *screen_flow_step_for(screen_flow_t *flow, const char *id)
{
    screen_flow_step_t *step = screen_flow_find_step(flow, id);

    if (step != NULL) {
        return step;
    }
    if (flow->step_count >= P4_CONFIG_SCREEN_FLOW_STEPS) {
        flow->overflow = true;
        return NULL;
    }
    step = &flow->steps[flow->step_count++];
    memset(step, 0, sizeof(*step));
    snprintf(step->id, sizeof(step->id), "%s", id);
    return step;
}

/** `storage_ini_file_foreach` callback: fill @p ctx (a screen_flow_t). */
static bool screen_flow_collect_cb(const char *key, const char *value, void *ctx)
{
    screen_flow_t *flow = (screen_flow_t *)ctx;
    const char *dot = strchr(key, '.');
    char id[COMMAND_FLOW_ID_BYTES];
    const char *field;
    size_t id_len;

    if (dot == NULL) {
        return true;   /* not a flow key; ignore */
    }
    id_len = (size_t)(dot - key);
    if (id_len == 0 || id_len >= sizeof(id)) {
        return true;
    }
    memcpy(id, key, id_len);
    id[id_len] = '\0';
    field = dot + 1;

    if (shell_text_equals_ignore_case(id, "flow")) {
        if (shell_text_equals_ignore_case(field, "start")) {
            snprintf(flow->start, sizeof(flow->start), "%s", value);
        } else if (shell_text_equals_ignore_case(field, "title")) {
            snprintf(flow->title, sizeof(flow->title), "%s", value);
        }
        return true;
    }

    if (shell_text_equals_ignore_case(field, "screen")) {
        screen_flow_step_t *step = screen_flow_step_for(flow, id);
        if (step == NULL) {
            return false;   /* overflow */
        }
        snprintf(step->screen, sizeof(step->screen), "%s", value);
        return true;
    }
    if (shell_text_equals_ignore_case(field, "var")) {
        screen_flow_step_t *step = screen_flow_step_for(flow, id);
        if (step == NULL) {
            return false;
        }
        snprintf(step->var, sizeof(step->var), "%s", value);
        return true;
    }
    /* Anything else on a step is a routing token. Validate its shape here so a
     * typo is reported instead of silently never matching. */
    if (field[0] != '*' &&
        strncasecmp(field, "err:", 4) != 0 &&
        strncasecmp(field, "val:", 4) != 0) {
        flow->bad_token = true;
        return true;
    }
    if (flow->rule_count >= P4_CONFIG_SCREEN_FLOW_RULES) {
        flow->overflow = true;
        return false;
    }
    {
        screen_flow_rule_t *rule = &flow->rules[flow->rule_count++];

        memset(rule, 0, sizeof(*rule));
        snprintf(rule->step, sizeof(rule->step), "%s", id);
        snprintf(rule->token, sizeof(rule->token), "%s", field);
        snprintf(rule->target, sizeof(rule->target), "%s", value);
    }
    return true;
}

/** Parse + validate a flow file. @return true when runnable. */
static bool screen_flow_load(const char *resolved, screen_flow_t *flow)
{
    esp_err_t error;
    int i;

    memset(flow, 0, sizeof(*flow));
    flow->steps = calloc(P4_CONFIG_SCREEN_FLOW_STEPS, sizeof(*flow->steps));
    flow->rules = calloc(P4_CONFIG_SCREEN_FLOW_RULES, sizeof(*flow->rules));
    if (flow->steps == NULL || flow->rules == NULL) {
        shell_print_error("screen flow: out of memory");
        return false;
    }
    error = storage_ini_file_foreach(resolved, screen_flow_collect_cb, flow);
    if (error == ESP_ERR_NOT_FOUND) {
        shell_print_error("screen flow: cannot read %s", resolved);
        return false;
    }
    if (flow->overflow) {
        shell_print_error("screen flow: %s has too many steps (%d max) or rules (%d max)",
                          resolved, P4_CONFIG_SCREEN_FLOW_STEPS,
                          P4_CONFIG_SCREEN_FLOW_RULES);
        return false;
    }
    if (flow->bad_token) {
        shell_print_error("screen flow: %s has an unknown rule (use *, err:<n>, or val:<text>)",
                          resolved);
        return false;
    }
    if (flow->start[0] == '\0') {
        shell_print_error("screen flow: %s has no flow.start=", resolved);
        return false;
    }
    if (screen_flow_find_step(flow, flow->start) == NULL) {
        shell_print_error("screen flow: start step '%s' has no .screen=", flow->start);
        return false;
    }
    /* Every rule must name a known step and a reachable target. */
    for (i = 0; i < flow->rule_count; i++) {
        if (screen_flow_find_step(flow, flow->rules[i].step) == NULL) {
            shell_print_error("screen flow: rule on unknown step '%s'", flow->rules[i].step);
            return false;
        }
        if (!shell_text_equals_ignore_case(flow->rules[i].target, "end") &&
            !shell_text_equals_ignore_case(flow->rules[i].target, "exit") &&
            screen_flow_find_step(flow, flow->rules[i].target) == NULL) {
            shell_print_error("screen flow: step '%s' targets unknown step '%s'",
                              flow->rules[i].step, flow->rules[i].target);
            return false;
        }
    }
    /* Every step must name a screen file. */
    for (i = 0; i < flow->step_count; i++) {
        if (flow->steps[i].screen[0] == '\0') {
            shell_print_error("screen flow: step '%s' has no .screen=", flow->steps[i].id);
            return false;
        }
    }
    return true;
}

static void screen_flow_free(screen_flow_t *flow)
{
    free(flow->steps);
    free(flow->rules);
    flow->steps = NULL;
    flow->rules = NULL;
}

/** `screen flow <file.flow>`: walk the declared navigation graph. */
static void screen_flow_run(const char *resolved)
{
    screen_flow_t flow;
    const char *current;
    int iterations = 0;
    int last_errorlevel = 0;

    if (!screen_flow_load(resolved, &flow)) {
        screen_flow_free(&flow);
        batch_set_errorlevel(1);
        return;
    }

    current = flow.start;
    while (current != NULL) {
        screen_flow_step_t *step;
        const char *var_value = NULL;
        const char *target;

        if (shell_abort_requested()) {
            shell_transcript_append_text("^C\n");
            shell_mark_foreground_break();
            shell_clear_abort();
            last_errorlevel = 1;
            break;
        }
        if (iterations++ >= P4_CONFIG_SCREEN_FLOW_ITER_MAX) {
            shell_print_error("screen flow: step limit (%d) reached - check the flow for a loop",
                              P4_CONFIG_SCREEN_FLOW_ITER_MAX);
            last_errorlevel = 1;
            break;
        }
        step = screen_flow_find_step(&flow, current);
        if (step == NULL) {
            /* Validated on load, so this is unreachable; stay honest. */
            shell_print_error("screen flow: lost step '%s'", current);
            last_errorlevel = 1;
            break;
        }

        shell_print_muted("flow: %s", step->id);
        {
            char *sargv[3] = { "screen", "run", step->screen };

            shell_command_screen(3, sargv);
        }
        last_errorlevel = batch_get_errorlevel();
        if (step->var[0] != '\0') {
            var_value = shell_env_get(step->var);
        }
        target = screen_flow_select(flow.rules, flow.rule_count, current,
                                    last_errorlevel, var_value);
        if (target == NULL ||
            shell_text_equals_ignore_case(target, "end") ||
            shell_text_equals_ignore_case(target, "exit")) {
            break;
        }
        current = target;
    }

    screen_flow_free(&flow);
    batch_set_errorlevel(last_errorlevel);
}

/** `screen info` on a `.FLOW` file: summarise its steps and rules. */
static void screen_flow_info(const char *resolved)
{
    screen_flow_t flow;
    int i;

    if (!screen_flow_load(resolved, &flow)) {
        screen_flow_free(&flow);
        batch_set_errorlevel(1);
        return;
    }
    shell_transcript_appendf_ansi(SH_HEAD "Flow" SH_RST "\n");
    shell_print_field("file:", "%s", resolved);
    if (flow.title[0] != '\0') {
        shell_print_field("title:", "%s", flow.title);
    }
    shell_print_field("start:", "%s", flow.start);
    shell_print_field_num("steps:", flow.step_count);
    shell_print_field_num("rules:", flow.rule_count);
    /* Plain (uncoloured) lines: `shell_transcript_appendf` does not expand
     * `@`-specifiers, and a machine-readable listing is easier to assert. */
    for (i = 0; i < flow.step_count; i++) {
        shell_transcript_appendf("  %s -> %s\n",
                                 flow.steps[i].id, flow.steps[i].screen);
    }
    for (i = 0; i < flow.rule_count; i++) {
        shell_transcript_appendf("  %s.%s -> %s\n", flow.rules[i].step,
                                 flow.rules[i].token, flow.rules[i].target);
    }
    screen_flow_free(&flow);
    batch_set_errorlevel(0);
}

/** `screen info <file>`: describe the screen without showing any UI. */
static void screen_command_info(const char *resolved)
{
    char type[SCREEN_VALUE_BYTES];
    char title[SCREEN_VALUE_BYTES];
    char timeout[SCREEN_VALUE_BYTES];
    char probe[SCREEN_VALUE_BYTES];

    screen_read_key(resolved, "type", type, sizeof(type));
    if (type[0] == '\0') {
        shell_print_error("screen: %s has no type= (dialog|list|ask|form|menu)", resolved);
        batch_set_errorlevel(1);
        return;
    }
    screen_read_key(resolved, "title", title, sizeof(title));
    screen_read_key(resolved, "timeout", timeout, sizeof(timeout));

    shell_transcript_appendf_ansi(SH_HEAD "Screen" SH_RST "\n");
    shell_print_field("file:", "%s", resolved);
    shell_print_field("type:", "%s", type);
    if (title[0] != '\0') {
        shell_print_field("title:", "%s", title);
    }
    if (timeout[0] != '\0') {
        shell_print_field("timeout:", "%ss", timeout);
    }
    if (shell_text_equals_ignore_case(type, "form")) {
        int fields = 0;

        for (int i = 0; i < MODAL_FORM_MAX_FIELDS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "field%d", i);
            screen_read_key(resolved, key, probe, sizeof(probe));
            if (probe[0] != '\0') {
                fields++;
            }
        }
        shell_print_field_num("fields:", fields);
    } else if (shell_text_equals_ignore_case(type, "list") ||
               shell_text_equals_ignore_case(type, "menu")) {
        screen_read_key(resolved, "items", probe, sizeof(probe));
        shell_print_field("items:", "%s", probe[0] != '\0' ? probe : "(none)");
    }
    batch_set_errorlevel(0);
}

/** `screen run <file>`: render the described screen through its verb. */
static void screen_command_run(const char *resolved)
{
    char type[SCREEN_VALUE_BYTES];
    char title[SCREEN_VALUE_BYTES];
    char timeout_text[SCREEN_VALUE_BYTES];
    char timeout_arg[16];
    char *timeout_slot = NULL;

    screen_read_key(resolved, "type", type, sizeof(type));
    screen_read_key(resolved, "title", title, sizeof(title));
    screen_read_key(resolved, "timeout", timeout_text, sizeof(timeout_text));
    screen_maybe_timeout(timeout_text, timeout_arg, sizeof(timeout_arg), &timeout_slot);

    if (shell_text_equals_ignore_case(type, "dialog")) {
        char message[SCREEN_VALUE_BYTES];
        char buttons[SCREEN_VALUE_BYTES];
        char store[SCREEN_VALUE_BYTES];
        const char *parts[SCREEN_LIST_MAX];
        int count;
        char *args[8];
        int argc = 0;

        screen_read_key(resolved, "message", message, sizeof(message));
        if (title[0] == '\0' || message[0] == '\0') {
            shell_print_usage("Usage: screen run <file>  (dialog needs title= and message=)");
            batch_set_errorlevel(2);
            return;
        }
        screen_read_key(resolved, "buttons", buttons, sizeof(buttons));
        count = screen_split_list(buttons[0] != '\0' ? buttons : "OK", '|',
                                  store, sizeof(store), parts, 2);
        if (count < 0) {
            shell_print_error("screen: dialog buttons do not fit");
            batch_set_errorlevel(1);
            return;
        }
        args[argc++] = "dialog";
        if (timeout_slot != NULL) {
            args[argc++] = timeout_slot;
        }
        args[argc++] = title;
        args[argc++] = message;
        for (int i = 0; i < count; i++) {
            args[argc++] = (char *)parts[i];
        }
        shell_command_dialog(argc, args);
        return;
    }

    if (shell_text_equals_ignore_case(type, "list") ||
        shell_text_equals_ignore_case(type, "menu")) {
        char items[SCREEN_VALUE_BYTES];
        char var[SCREEN_VALUE_BYTES];
        char store[SCREEN_VALUE_BYTES];
        const char *parts[SCREEN_LIST_MAX];
        int count;
        char var_arg[SCREEN_VALUE_BYTES + 4];
        /* argv: verb + /t + /v + title + items (heap pointers stay valid
         * through the synchronous verb call). */
        char *args[SCREEN_LIST_MAX + 5];
        int argc = 0;
        bool is_menu = shell_text_equals_ignore_case(type, "menu");

        screen_read_key(resolved, "items", items, sizeof(items));
        if (!is_menu && title[0] == '\0') {
            shell_print_usage("Usage: screen run <file>  (list needs title= and items=)");
            batch_set_errorlevel(2);
            return;
        }
        if (items[0] == '\0') {
            shell_print_usage("Usage: screen run <file>  (list/menu needs items=a|b|c)");
            batch_set_errorlevel(2);
            return;
        }
        count = screen_split_list(items, '|', store, sizeof(store), parts, SCREEN_LIST_MAX);
        if (count <= 0) {
            shell_print_error("screen: no usable items in %s", resolved);
            batch_set_errorlevel(1);
            return;
        }
        screen_read_key(resolved, "var", var, sizeof(var));
        args[argc++] = is_menu ? "menu" : "list";
        if (!is_menu) {
            if (timeout_slot != NULL) {
                args[argc++] = timeout_slot;
            }
            if (var[0] != '\0') {
                snprintf(var_arg, sizeof(var_arg), "/v:%s", var);
                args[argc++] = var_arg;
            }
            args[argc++] = title;
        }
        for (int i = 0; i < count; i++) {
            args[argc++] = (char *)parts[i];
        }
        if (is_menu) {
            shell_command_menu(argc, args);
        } else {
            shell_command_list(argc, args);
        }
        return;
    }

    if (shell_text_equals_ignore_case(type, "ask")) {
        char prompt[SCREEN_VALUE_BYTES];
        char def[SCREEN_VALUE_BYTES];
        char var[SCREEN_VALUE_BYTES];
        char pw[SCREEN_VALUE_BYTES];
        char var_arg[SCREEN_VALUE_BYTES + 4];
        char *args[8];
        int argc = 0;

        screen_read_key(resolved, "prompt", prompt, sizeof(prompt));
        if (prompt[0] == '\0') {
            shell_print_usage("Usage: screen run <file>  (ask needs prompt=)");
            batch_set_errorlevel(2);
            return;
        }
        screen_read_key(resolved, "default", def, sizeof(def));
        screen_read_key(resolved, "var", var, sizeof(var));
        screen_read_key(resolved, "password", pw, sizeof(pw));
        args[argc++] = "ask";
        if (timeout_slot != NULL) {
            args[argc++] = timeout_slot;
        }
        if (var[0] != '\0') {
            snprintf(var_arg, sizeof(var_arg), "/v:%s", var);
            args[argc++] = var_arg;
        }
        if (screen_is_truthy(pw)) {
            args[argc++] = "/p";
        }
        args[argc++] = prompt;
        if (def[0] != '\0') {
            args[argc++] = def;
        }
        shell_command_ask(argc, args);
        return;
    }

    if (shell_text_equals_ignore_case(type, "form")) {
        /* Field specs pass through verbatim (`Label=type[:arg]:VAR`), so the
         * command-layer field parser stays the single implementation. */
        char specs[MODAL_FORM_MAX_FIELDS][SCREEN_VALUE_BYTES];
        char *args[MODAL_FORM_MAX_FIELDS + 4];
        int argc = 0;
        int fields = 0;

        if (title[0] == '\0') {
            shell_print_usage("Usage: screen run <file>  (form needs title= and fieldN=)");
            batch_set_errorlevel(2);
            return;
        }
        for (int i = 0; i < MODAL_FORM_MAX_FIELDS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "field%d", i);
            screen_read_key(resolved, key, specs[i], sizeof(specs[i]));
            if (specs[i][0] != '\0') {
                fields++;
            }
        }
        if (fields == 0) {
            shell_print_usage("Usage: screen run <file>  (form needs title= and fieldN=)");
            batch_set_errorlevel(2);
            return;
        }
        args[argc++] = "form";
        if (timeout_slot != NULL) {
            args[argc++] = timeout_slot;
        }
        args[argc++] = title;
        for (int i = 0; i < MODAL_FORM_MAX_FIELDS; i++) {
            if (specs[i][0] != '\0') {
                args[argc++] = specs[i];
            }
        }
        shell_command_form(argc, args);
        return;
    }

    if (type[0] == '\0') {
        shell_print_usage("Usage: screen run <file>  (file needs type=dialog|list|ask|form|menu)");
    } else {
        shell_print_error("screen: unknown type '%s' (dialog|list|ask|form|menu)", type);
    }
    batch_set_errorlevel(2);
}

/** True when @p resolved is a flow file (has a `flow.start=` key). */
static bool screen_is_flow(const char *resolved)
{
    char probe[P4_CONFIG_SCREEN_FLOW_ID_BYTES];

    probe[0] = '\0';
    return storage_ini_file_get(resolved, "flow.start", probe, sizeof(probe)) == ESP_OK &&
           probe[0] != '\0';
}

void shell_command_screen(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    bool is_flow;

    if (argc < 3 || (!shell_text_equals_ignore_case(argv[1], "run") &&
                     !shell_text_equals_ignore_case(argv[1], "info") &&
                     !shell_text_equals_ignore_case(argv[1], "flow"))) {
        shell_print_usage("Usage: screen run <file.frm> | screen flow <file.flow> | screen info <file>");
        batch_set_errorlevel(2);
        return;
    }
    if (shell_fs_resolve_path(argv[2], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("screen: invalid path %s", argv[2]);
        batch_set_errorlevel(2);
        return;
    }

    if (shell_text_equals_ignore_case(argv[1], "flow")) {
        screen_flow_run(resolved);
        return;
    }

    is_flow = screen_is_flow(resolved);
    if (shell_text_equals_ignore_case(argv[1], "info")) {
        if (is_flow) {
            screen_flow_info(resolved);
        } else {
            screen_command_info(resolved);
        }
        return;
    }
    /* `run`: a flow is walked with the explicit `flow` subcommand. */
    if (is_flow) {
        shell_print_error("screen: %s is a flow; use `screen flow %s`", argv[2], argv[2]);
        batch_set_errorlevel(2);
        return;
    }
    screen_command_run(resolved);
}
