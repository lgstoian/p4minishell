/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file tui_modal.c
 * @brief Modal layer implementation: mode frames, keymaps, chords, scopes.
 */

#include "tui_modal.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================
 * MODE STACK
 * ======================================================================== */

typedef struct {
    char name[16];
    modal_frame_type_t type;
    char map_name[16];
    bool used;
} modal_frame_t;

static modal_frame_t s_frames[P4_CONFIG_MODAL_MAX_FRAMES];
static int s_frame_count = 0;

static modal_label_exec_fn s_exec_cb = NULL;
static modal_env_get_fn s_env_get_cb = NULL;
static modal_env_set_fn s_env_set_cb = NULL;
static modal_status_render_fn s_status_cb = NULL;
static uint32_t s_chord_timeout = P4_CONFIG_MODAL_CHORD_TIMEOUT_MS;

/* ========================================================================
 * KEYMAPS
 * ======================================================================== */

typedef struct {
    char name[16];
    struct {
        char chord[P4_CONFIG_MODAL_MAX_CHORD_BYTES];
        char label[32];
    } bindings[P4_CONFIG_MODAL_MAX_BINDINGS];
    int bind_count;
    bool used;
} modal_map_t;

static modal_map_t s_maps[P4_CONFIG_MODAL_MAX_MAPS];
static char s_leader[2] = {P4_CONFIG_MODAL_LEADER_KEY, '\0'};

/* ========================================================================
 * CHORD BUFFER
 * ======================================================================== */

static char s_pending[P4_CONFIG_MODAL_MAX_PENDING][P4_CONFIG_MODAL_MAX_CHORD_BYTES];
static int s_pending_count = 0;

/* ========================================================================
 * NAMED COMMANDS
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_MODAL_MAX_CMD_NAME_BYTES];
    char label[32];
    char doc[P4_CONFIG_MODAL_MAX_CMD_DOC_BYTES];
    bool used;
} modal_cmd_t;

static modal_cmd_t s_cmds[P4_CONFIG_MODAL_MAX_CMDS];

/* ========================================================================
 * SCOPE BAGS
 * ======================================================================== */

typedef struct {
    char mode_name[16];
    struct {
        char name[16];
        char value[32];
    } vars[P4_CONFIG_MODAL_MAX_SCOPE_VARS];
    int var_count;
    bool used;
} modal_scope_t;

static modal_scope_t s_scopes[P4_CONFIG_MODAL_MAX_SCOPES];

/* ========================================================================
 * GUARDS
 * ======================================================================== */

static struct {
    char label[32];
    bool used;
} s_guards[MODAL_GUARD_KIND_COUNT][MODAL_FRAME_TYPE_COUNT];

/* ========================================================================
 * DIALOGS
 * ======================================================================== */

typedef struct {
    char type[16];
    char title[32];
    char result[P4_CONFIG_MODAL_MAX_DIALOG_RESULT_BYTES];
    int errorlevel;
    bool used;
} modal_dialog_t;

static modal_dialog_t s_dialogs[P4_CONFIG_MODAL_MAX_DIALOGS];
static int s_dialog_count = 0;
static char s_last_result[P4_CONFIG_MODAL_MAX_DIALOG_RESULT_BYTES] = {0};

/* ========================================================================
 * MACROS
 * ======================================================================== */

typedef struct {
    char name[P4_CONFIG_MODAL_MAX_MACRO_NAME_BYTES];
    char events[P4_CONFIG_MODAL_MAX_MACRO_EVENTS][16];
    int event_count;
    bool recording;
    bool used;
} modal_macro_t;

static modal_macro_t s_macros[P4_CONFIG_MODAL_MAX_MACROS];

/* ========================================================================
 * REPEAT
 * ======================================================================== */

static int s_repeat_count = 0;

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

bool modal_init(modal_label_exec_fn exec_cb, modal_env_get_fn get_cb,
                modal_env_set_fn set_cb, modal_status_render_fn status_cb)
{
    s_exec_cb = exec_cb;
    s_env_get_cb = get_cb;
    s_env_set_cb = set_cb;
    s_status_cb = status_cb;
    modal_reset();
    return true;
}

void modal_reset(void)
{
    memset(s_frames, 0, sizeof(s_frames));
    s_frame_count = 0;
    memset(s_maps, 0, sizeof(s_maps));
    s_leader[0] = P4_CONFIG_MODAL_LEADER_KEY;
    s_leader[1] = '\0';
    memset(s_pending, 0, sizeof(s_pending));
    s_pending_count = 0;
    memset(s_cmds, 0, sizeof(s_cmds));
    memset(s_scopes, 0, sizeof(s_scopes));
    memset(s_guards, 0, sizeof(s_guards));
    memset(s_dialogs, 0, sizeof(s_dialogs));
    s_dialog_count = 0;
    s_last_result[0] = '\0';
    memset(s_macros, 0, sizeof(s_macros));
    s_repeat_count = 0;
    s_chord_timeout = P4_CONFIG_MODAL_CHORD_TIMEOUT_MS;
}

void modal_set_exec_callback(modal_label_exec_fn cb)
{
    s_exec_cb = cb;
}

void modal_set_status_render_callback(modal_frame_type_t type, const char *format)
{
    (void)type;
    (void)format;
    /* Status render callback stored for later use by verb layer. */
}

/* ========================================================================
 * MODE STACK
 * ======================================================================== */

bool modal_mode_push(const char *name, modal_frame_type_t type, const char *map_name)
{
    if (s_frame_count >= P4_CONFIG_MODAL_MAX_FRAMES) return false;
    if (name == NULL || name[0] == '\0') return false;

    modal_frame_t *f = &s_frames[s_frame_count];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->type = type;
    if (map_name != NULL) {
        snprintf(f->map_name, sizeof(f->map_name), "%s", map_name);
    }
    f->used = true;
    s_frame_count++;
    return true;
}

bool modal_mode_pop(void)
{
    if (s_frame_count <= 0) return false;

    modal_frame_t *f = &s_frames[s_frame_count - 1];

    /* Run leave guard. */
    if (s_exec_cb != NULL && s_guards[MODAL_GUARD_LEAVE][f->type].used) {
        int el = s_exec_cb(s_guards[MODAL_GUARD_LEAVE][f->type].label, NULL);
        if (el != 0) return false; /* guard veto */
    }

    memset(f, 0, sizeof(*f));
    s_frame_count--;
    return true;
}

bool modal_mode_switch(const char *name)
{
    /* Pop until we find the named frame. */
    for (int i = s_frame_count - 1; i >= 0; i--) {
        if (strcmp(s_frames[i].name, name) == 0) {
            /* Pop everything above it. */
            while (s_frame_count > i + 1) {
                if (!modal_mode_pop()) return false;
            }
            return true;
        }
    }
    /* Not found - push as normal frame. */
    return modal_mode_push(name, MODAL_FRAME_NORMAL, NULL);
}

int modal_mode_top(void)
{
    if (s_frame_count <= 0) return -1;
    return s_frame_count - 1;
}

bool modal_mode_get_name(int depth, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    int idx = s_frame_count - 1 - depth;
    if (idx < 0 || idx >= s_frame_count) return false;
    snprintf(buf, buf_size, "%s", s_frames[idx].name);
    return true;
}

modal_frame_type_t modal_mode_get_type(int depth)
{
    int idx = s_frame_count - 1 - depth;
    if (idx < 0 || idx >= s_frame_count) return MODAL_FRAME_NORMAL;
    return s_frames[idx].type;
}

int modal_mode_count(void) { return s_frame_count; }

int modal_mode_drop_by_type(modal_frame_type_t type)
{
    int dropped = 0;
    for (int i = s_frame_count - 1; i >= 0; i--) {
        if (s_frames[i].type == type) {
            /* Pop from top down to this frame. */
            while (s_frame_count > i) {
                if (!modal_mode_pop()) break;
                dropped++;
            }
        }
    }
    return dropped;
}

void modal_set_chord_timeout(uint32_t ms) { s_chord_timeout = ms; }
uint32_t modal_get_chord_timeout(void) { return s_chord_timeout; }

/* ========================================================================
 * KEYMAPS
 * ======================================================================== */

int modal_map_create(const char *map_name)
{
    if (map_name == NULL || map_name[0] == '\0') return -1;
    int idx = modal_map_find(map_name);
    if (idx >= 0) return idx;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MAPS; i++) {
        if (!s_maps[i].used) {
            memset(&s_maps[i], 0, sizeof(s_maps[i]));
            snprintf(s_maps[i].name, sizeof(s_maps[i].name), "%s", map_name);
            s_maps[i].bind_count = 0;
            s_maps[i].used = true;
            return i;
        }
    }
    return -1;
}

int modal_map_find(const char *map_name)
{
    if (map_name == NULL) return -1;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MAPS; i++) {
        if (s_maps[i].used && strcmp(s_maps[i].name, map_name) == 0) return i;
    }
    return -1;
}

bool modal_bind(const char *map_name, const char *chord, const char *label)
{
    int idx = modal_map_create(map_name);
    if (idx < 0) return false;
    modal_map_t *map = &s_maps[idx];
    if (map->bind_count >= P4_CONFIG_MODAL_MAX_BINDINGS) return false;
    /* Replace existing binding for same chord. */
    for (int i = 0; i < map->bind_count; i++) {
        if (strcmp(map->bindings[i].chord, chord) == 0) {
            snprintf(map->bindings[i].label, sizeof(map->bindings[i].label), "%s", label);
            return true;
        }
    }
    snprintf(map->bindings[map->bind_count].chord, sizeof(map->bindings[map->bind_count].chord), "%s", chord);
    snprintf(map->bindings[map->bind_count].label, sizeof(map->bindings[map->bind_count].label), "%s", label);
    map->bind_count++;
    return true;
}

bool modal_unbind(const char *map_name, const char *chord)
{
    int idx = modal_map_find(map_name);
    if (idx < 0) return false;
    modal_map_t *map = &s_maps[idx];
    for (int i = 0; i < map->bind_count; i++) {
        if (strcmp(map->bindings[i].chord, chord) == 0) {
            /* Shift remaining bindings down. */
            for (int j = i; j < map->bind_count - 1; j++) {
                memcpy(&map->bindings[j], &map->bindings[j + 1], sizeof(map->bindings[j]));
            }
            memset(&map->bindings[map->bind_count - 1], 0, sizeof(map->bindings[0]));
            map->bind_count--;
            return true;
        }
    }
    return false;
}

bool modal_set_leader(const char *key)
{
    if (key == NULL || key[0] == '\0' || key[1] != '\0') return false;
    s_leader[0] = key[0];
    s_leader[1] = '\0';
    return true;
}

const char *modal_get_leader(void) { return s_leader; }

int modal_bind_count(const char *map_name)
{
    int idx = modal_map_find(map_name);
    if (idx < 0) return 0;
    return s_maps[idx].bind_count;
}

bool modal_bind_get_chord(const char *map_name, int index, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    int idx = modal_map_find(map_name);
    if (idx < 0 || index < 0 || index >= s_maps[idx].bind_count) return false;
    snprintf(buf, buf_size, "%s", s_maps[idx].bindings[index].chord);
    return true;
}

bool modal_bind_get_label(const char *map_name, int index, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    int idx = modal_map_find(map_name);
    if (idx < 0 || index < 0 || index >= s_maps[idx].bind_count) return false;
    snprintf(buf, buf_size, "%s", s_maps[idx].bindings[index].label);
    return true;
}

/* ========================================================================
 * CHORD RESOLUTION
 * ======================================================================== */

static void chord_expand_leader(char *chord)
{
    /* Replace "<leader>" prefix with the actual leader key. */
    if (strncmp(chord, "<leader>", 8) == 0) {
        char tmp[P4_CONFIG_MODAL_MAX_CHORD_BYTES];
        snprintf(tmp, sizeof(tmp), "%s%s", s_leader, chord + 8);
        snprintf(chord, P4_CONFIG_MODAL_MAX_CHORD_BYTES, "%s", tmp);
    }
}

bool modal_chord_push(const char *stroke, char *label_out, size_t label_size)
{
    if (s_pending_count >= P4_CONFIG_MODAL_MAX_PENDING) {
        modal_chord_clear();
        return false;
    }
    snprintf(s_pending[s_pending_count], sizeof(s_pending[s_pending_count]), "%s", stroke);
    s_pending_count++;

    /* Build the current chord string. */
    char chord[128];
    int off = 0;
    for (int i = 0; i < s_pending_count; i++) {
        if (i > 0) chord[off++] = ' ';
        int len = snprintf(chord + off, sizeof(chord) - off, "%s", s_pending[i]);
        off += len;
    }

    /* Try to resolve against the active map. */
    modal_frame_type_t top_type = modal_mode_get_type(0);
    (void)top_type;

    /* For now, just check if any map has this exact chord. */
    for (int m = 0; m < P4_CONFIG_MODAL_MAX_MAPS; m++) {
        if (!s_maps[m].used) continue;
        modal_map_t *map = &s_maps[m];
        for (int b = 0; b < map->bind_count; b++) {
            char expanded[128];
            snprintf(expanded, sizeof(expanded), "%s", map->bindings[b].chord);
            chord_expand_leader(expanded);
            if (strcmp(expanded, chord) == 0) {
                snprintf(label_out, label_size, "%s", map->bindings[b].label);
                modal_chord_clear();
                return true;
            }
        }
    }

    /* Check if any binding has this as a prefix. */
    for (int m = 0; m < P4_CONFIG_MODAL_MAX_MAPS; m++) {
        if (!s_maps[m].used) continue;
        modal_map_t *map = &s_maps[m];
        for (int b = 0; b < map->bind_count; b++) {
            char expanded[128];
            snprintf(expanded, sizeof(expanded), "%s", map->bindings[b].chord);
            chord_expand_leader(expanded);
            if (strncmp(expanded, chord, strlen(chord)) == 0 &&
                strlen(expanded) > strlen(chord)) {
                /* Partial match - keep pending. */
                return false;
            }
        }
    }

    /* No match at all - clear buffer. */
    modal_chord_clear();
    return false;
}

bool modal_chord_pending(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    if (s_pending_count == 0) {
        buf[0] = '\0';
        return false;
    }
    int off = 0;
    for (int i = 0; i < s_pending_count; i++) {
        if (i > 0) buf[off++] = ' ';
        off += snprintf(buf + off, buf_size - off, "%s", s_pending[i]);
    }
    return true;
}

void modal_chord_clear(void)
{
    memset(s_pending, 0, sizeof(s_pending));
    s_pending_count = 0;
}

/* ========================================================================
 * NAMED COMMANDS
 * ======================================================================== */

bool modal_cmd_def(const char *name, const char *label, const char *doc)
{
    if (name == NULL || name[0] == '\0' || label == NULL) return false;
    /* Replace existing. */
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (s_cmds[i].used && strcmp(s_cmds[i].name, name) == 0) {
            snprintf(s_cmds[i].label, sizeof(s_cmds[i].label), "%s", label);
            if (doc != NULL) snprintf(s_cmds[i].doc, sizeof(s_cmds[i].doc), "%s", doc);
            return true;
        }
    }
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (!s_cmds[i].used) {
            memset(&s_cmds[i], 0, sizeof(s_cmds[i]));
            snprintf(s_cmds[i].name, sizeof(s_cmds[i].name), "%s", name);
            snprintf(s_cmds[i].label, sizeof(s_cmds[i].label), "%s", label);
            if (doc != NULL) snprintf(s_cmds[i].doc, sizeof(s_cmds[i].doc), "%s", doc);
            s_cmds[i].used = true;
            return true;
        }
    }
    return false;
}

bool modal_cmd_undef(const char *name)
{
    if (name == NULL) return false;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (s_cmds[i].used && strcmp(s_cmds[i].name, name) == 0) {
            memset(&s_cmds[i], 0, sizeof(s_cmds[i]));
            return true;
        }
    }
    return false;
}

const char *modal_cmd_find(const char *name)
{
    if (name == NULL) return NULL;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (s_cmds[i].used && strcmp(s_cmds[i].name, name) == 0) return s_cmds[i].label;
    }
    return NULL;
}

const char *modal_cmd_get_doc(const char *name)
{
    if (name == NULL) return NULL;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (s_cmds[i].used && strcmp(s_cmds[i].name, name) == 0) return s_cmds[i].doc;
    }
    return NULL;
}

int modal_cmd_count(void)
{
    int n = 0;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) if (s_cmds[i].used) n++;
    return n;
}

bool modal_cmd_get_name(int index, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    int n = 0;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_CMDS; i++) {
        if (s_cmds[i].used) {
            if (n == index) {
                snprintf(buf, buf_size, "%s", s_cmds[i].name);
                return true;
            }
            n++;
        }
    }
    return false;
}

int modal_cmd_run(const char *name)
{
    const char *label = modal_cmd_find(name);
    if (label == NULL || s_exec_cb == NULL) return 2;
    return s_exec_cb(label, NULL);
}

/* ========================================================================
 * SCOPE BAGS
 * ======================================================================== */

bool modal_scope_save(const char *mode_name, int count, const char **vars)
{
    if (mode_name == NULL || vars == NULL || count <= 0) return false;
    if (count > P4_CONFIG_MODAL_MAX_SCOPE_VARS) count = P4_CONFIG_MODAL_MAX_SCOPE_VARS;

    /* Find existing or free slot. */
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_SCOPES; i++) {
        if (s_scopes[i].used && strcmp(s_scopes[i].mode_name, mode_name) == 0) {
            /* Update existing. */
            s_scopes[i].var_count = count;
            for (int v = 0; v < count; v++) {
                snprintf(s_scopes[i].vars[v].name, sizeof(s_scopes[i].vars[v].name), "%s", vars[v]);
                const char *val = s_env_get_cb != NULL ? s_env_get_cb(vars[v]) : NULL;
                snprintf(s_scopes[i].vars[v].value, sizeof(s_scopes[i].vars[v].value), "%s", val != NULL ? val : "");
            }
            return true;
        }
    }
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_SCOPES; i++) {
        if (!s_scopes[i].used) {
            memset(&s_scopes[i], 0, sizeof(s_scopes[i]));
            snprintf(s_scopes[i].mode_name, sizeof(s_scopes[i].mode_name), "%s", mode_name);
            s_scopes[i].var_count = count;
            for (int v = 0; v < count; v++) {
                snprintf(s_scopes[i].vars[v].name, sizeof(s_scopes[i].vars[v].name), "%s", vars[v]);
                const char *val = s_env_get_cb != NULL ? s_env_get_cb(vars[v]) : NULL;
                snprintf(s_scopes[i].vars[v].value, sizeof(s_scopes[i].vars[v].value), "%s", val != NULL ? val : "");
            }
            s_scopes[i].used = true;
            return true;
        }
    }
    return false;
}

bool modal_scope_restore(const char *mode_name)
{
    if (mode_name == NULL || s_env_set_cb == NULL) return false;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_SCOPES; i++) {
        if (s_scopes[i].used && strcmp(s_scopes[i].mode_name, mode_name) == 0) {
            for (int v = 0; v < s_scopes[i].var_count; v++) {
                s_env_set_cb(s_scopes[i].vars[v].name, s_scopes[i].vars[v].value);
            }
            return true;
        }
    }
    return false;
}

int modal_scope_list(const char *mode_name, char *buf, size_t buf_size)
{
    if (mode_name == NULL || buf == NULL || buf_size == 0) return 0;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_SCOPES; i++) {
        if (s_scopes[i].used && strcmp(s_scopes[i].mode_name, mode_name) == 0) {
            int off = 0;
            for (int v = 0; v < s_scopes[i].var_count; v++) {
                if (v > 0) off += snprintf(buf + off, buf_size - off, " ");
                off += snprintf(buf + off, buf_size - off, "%s=%s",
                                s_scopes[i].vars[v].name, s_scopes[i].vars[v].value);
            }
            return s_scopes[i].var_count;
        }
    }
    return 0;
}

bool modal_scope_clear(const char *mode_name)
{
    if (mode_name == NULL) return false;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_SCOPES; i++) {
        if (s_scopes[i].used && strcmp(s_scopes[i].mode_name, mode_name) == 0) {
            memset(&s_scopes[i], 0, sizeof(s_scopes[i]));
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * GUARDS
 * ======================================================================== */

bool modal_guard_set(modal_guard_kind_t kind, modal_frame_type_t type, const char *label)
{
    if (kind < 0 || kind >= MODAL_GUARD_KIND_COUNT) return false;
    if (type < 0 || type >= MODAL_FRAME_TYPE_COUNT) return false;
    if (label == NULL || label[0] == '\0') return false;
    snprintf(s_guards[kind][type].label, sizeof(s_guards[kind][type].label), "%s", label);
    s_guards[kind][type].used = true;
    return true;
}

bool modal_guard_clear(modal_guard_kind_t kind, modal_frame_type_t type)
{
    if (kind < 0 || kind >= MODAL_GUARD_KIND_COUNT) return false;
    if (type < 0 || type >= MODAL_FRAME_TYPE_COUNT) return false;
    memset(&s_guards[kind][type], 0, sizeof(s_guards[kind][type]));
    return true;
}

const char *modal_guard_get(modal_guard_kind_t kind, modal_frame_type_t type)
{
    if (kind < 0 || kind >= MODAL_GUARD_KIND_COUNT) return NULL;
    if (type < 0 || type >= MODAL_FRAME_TYPE_COUNT) return NULL;
    if (!s_guards[kind][type].used) return NULL;
    return s_guards[kind][type].label;
}

/* ========================================================================
 * DIALOGS
 * ======================================================================== */

bool modal_dialog_push(const char *type, const char *title)
{
    if (s_dialog_count >= P4_CONFIG_MODAL_MAX_DIALOGS) return false;
    modal_dialog_t *d = &s_dialogs[s_dialog_count];
    memset(d, 0, sizeof(*d));
    snprintf(d->type, sizeof(d->type), "%s", type != NULL ? type : "dialog");
    snprintf(d->title, sizeof(d->title), "%s", title != NULL ? title : "");
    d->result[0] = '\0';
    d->errorlevel = 0;
    d->used = true;
    s_dialog_count++;
    return true;
}

void modal_dialog_set_result(const char *value)
{
    if (s_dialog_count <= 0) return;
    modal_dialog_t *d = &s_dialogs[s_dialog_count - 1];
    snprintf(d->result, sizeof(d->result), "%s", value != NULL ? value : "");
    snprintf(s_last_result, sizeof(s_last_result), "%s", value != NULL ? value : "");
}

const char *modal_dialog_get_result(void) { return s_last_result; }

int modal_dialog_pop(void)
{
    if (s_dialog_count <= 0) return 0;
    modal_dialog_t *d = &s_dialogs[s_dialog_count - 1];
    int el = d->errorlevel;
    memset(d, 0, sizeof(*d));
    s_dialog_count--;
    return el;
}

int modal_dialog_count(void) { return s_dialog_count; }

/* ========================================================================
 * MACROS
 * ======================================================================== */

bool modal_macro_record(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    /* Find existing or free slot. */
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (s_macros[i].used && strcmp(s_macros[i].name, name) == 0) {
            s_macros[i].recording = true;
            s_macros[i].event_count = 0;
            return true;
        }
    }
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (!s_macros[i].used) {
            memset(&s_macros[i], 0, sizeof(s_macros[i]));
            snprintf(s_macros[i].name, sizeof(s_macros[i].name), "%s", name);
            s_macros[i].recording = true;
            s_macros[i].event_count = 0;
            s_macros[i].used = true;
            return true;
        }
    }
    return false;
}

int modal_macro_stop(void)
{
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (s_macros[i].used && s_macros[i].recording) {
            s_macros[i].recording = false;
            return s_macros[i].event_count;
        }
    }
    return 0;
}

bool modal_macro_capture(const char *event)
{
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (s_macros[i].used && s_macros[i].recording) {
            if (s_macros[i].event_count >= P4_CONFIG_MODAL_MAX_MACRO_EVENTS) return false;
            snprintf(s_macros[i].events[s_macros[i].event_count],
                     sizeof(s_macros[i].events[s_macros[i].event_count]), "%s", event);
            s_macros[i].event_count++;
            return true;
        }
    }
    return false;
}

int modal_macro_replay(const char *name, int times)
{
    if (name == NULL || times <= 0) return 2;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (s_macros[i].used && strcmp(s_macros[i].name, name) == 0) {
            int last_el = 0;
            for (int t = 0; t < times; t++) {
                for (int e = 0; e < s_macros[i].event_count; e++) {
                    if (s_exec_cb != NULL) {
                        last_el = s_exec_cb(s_macros[i].events[e], NULL);
                        if (last_el != 0) return last_el;
                    }
                }
            }
            return last_el;
        }
    }
    return 2; /* not found */
}

int modal_macro_count(void)
{
    int n = 0;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) if (s_macros[i].used) n++;
    return n;
}

bool modal_macro_get_name(int index, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) return false;
    int n = 0;
    for (int i = 0; i < P4_CONFIG_MODAL_MAX_MACROS; i++) {
        if (s_macros[i].used) {
            if (n == index) {
                snprintf(buf, buf_size, "%s", s_macros[i].name);
                return true;
            }
            n++;
        }
    }
    return false;
}

/* ========================================================================
 * REPEAT
 * ======================================================================== */

void modal_set_repeat(int count) { s_repeat_count = count > 0 ? count : 0; }
int modal_get_repeat(void) { int r = s_repeat_count; s_repeat_count = 0; return r > 0 ? r : 1; }
void modal_clear_repeat(void) { s_repeat_count = 0; }
