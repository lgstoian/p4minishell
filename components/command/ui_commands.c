/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file ui_commands.c
 * @brief `ui` verbs: synthetic touch automation and UI inspection.
 *
 * Verbs (all batch-friendly: ERRORLEVEL, /b bare output, /v:NAME result):
 *   ui tap <x> <y> [ms]          press+release at a point
 *   ui longpress <x> <y> [ms]    hold past the long-press time
 *   ui swipe <x1> <y1> <x2> <y2> [ms] [steps]
 *   ui press <x> <y>             hold (pair with move/release)
 *   ui move <x> <y>              move the held point
 *   ui release                   release the held point
 *   ui target <id> [ms]          tap the center of a listed target
 *   ui key <label> [ms]          tap an on-screen-keyboard key by label
 *   ui targets [/b] [/v:NAME]    enumerate every tappable target
 *   ui hit <x> <y> [/v:NAME]     report the target under a point
 *   ui state [/b] [/v:NAME]      active modal / keyboard / editor / input text
 *
 * While a modal blocks the worker, ui_commands_console() is used by the
 * console reader so the editor/dialog/list UI can be driven over serial.
 */

#include "ui_commands.h"

#include "command.h"
#include "shell.h"
#include "batch.h"
#include "p4minishell_config.h"
#include "ui_test.h"
#include "display.h"
#include "editor_view.h"
#include "keyboard.h"
#include "modal.h"
#include "windows.h"

#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UI_TARGETS_MAX 320

#define SHELL_COMMAND_BYTES P4_CONFIG_COMMAND_BYTES

typedef struct {
    lv_obj_t *obj;
    uint32_t btn_id;      /* UINT32_MAX unless a buttonmatrix key */
    lv_area_t area;       /* absolute coordinates */
    char name[48];
} ui_target_t;

/* Kept in PSRAM: the boot-time internal heap is the scarce resource and a
 * static array here would stop the command worker task from starting. */
static ui_target_t *s_targets;
static int s_target_count;
static bool s_console_mode;

static ui_target_t *ui_targets_buffer(void)
{
    if (s_targets == NULL) {
        s_targets = heap_caps_malloc(UI_TARGETS_MAX * sizeof(ui_target_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_targets == NULL) {
            s_targets = malloc(UI_TARGETS_MAX * sizeof(ui_target_t));
        }
    }
    return s_targets;
}

/* ========================================================================
 * OUTPUT
 * ======================================================================== */

static void ui_out(const char *format, ...)
{
    char buf[256];
    va_list args;

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (s_console_mode) {
        fputs(buf, stdout);
        fflush(stdout);
    } else {
        shell_transcript_appendf("%s", buf);
    }
}

/** Serial-only output: used for long listings (the widget target list) so an
 *  automation run does not bloat the LVGL transcript and starve the port
 *  lock. */
static void ui_out_raw(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

/* ========================================================================
 * TARGET ENUMERATION
 * ======================================================================== */

static void ui_target_add(lv_obj_t *obj, uint32_t btn_id, const lv_area_t *area,
                          const char *name)
{
    ui_target_t *t;

    if (s_targets == NULL || s_target_count >= UI_TARGETS_MAX) {
        return;
    }
    t = &s_targets[s_target_count++];
    t->obj = obj;
    t->btn_id = btn_id;
    t->area = *area;
    snprintf(t->name, sizeof(t->name), "%s", (name != NULL) ? name : "?");
}

static void ui_target_label(lv_obj_t *obj, char *out, size_t out_size)
{
    int32_t count;
    int32_t i;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        if (text != NULL && text[0] != '\0') {
            snprintf(out, out_size, "%.32s", text);
            return;
        }
    }
    count = (int32_t)lv_obj_get_child_count(obj);
    for (i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (child != NULL && lv_obj_check_type(child, &lv_label_class)) {
            const char *text = lv_label_get_text(child);
            if (text != NULL && text[0] != '\0') {
                snprintf(out, out_size, "%.32s", text);
                return;
            }
        }
    }

    /* No label: name by the (public) widget class. */
    if (lv_obj_check_type(obj, &lv_button_class)) {
        snprintf(out, out_size, "button");
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        snprintf(out, out_size, "textarea");
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        snprintf(out, out_size, "slider");
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        snprintf(out, out_size, "switch");
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        snprintf(out, out_size, "checkbox");
    } else if (lv_obj_check_type(obj, &lv_roller_class)) {
        snprintf(out, out_size, "roller");
    } else if (lv_obj_check_type(obj, &lv_image_class)) {
        snprintf(out, out_size, "image");
    } else {
        snprintf(out, out_size, "obj");
    }
}

static void ui_collect_targets(lv_obj_t *parent)
{
    int32_t count = (int32_t)lv_obj_get_child_count(parent);
    int32_t i;

    for (i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        int w;
        int h;

        if (child == NULL || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        w = (int)lv_obj_get_width(child);
        h = (int)lv_obj_get_height(child);

        if (lv_obj_has_class(child, &lv_buttonmatrix_class)) {
            lv_area_t base;
            uint32_t b;

            /* Button areas are object-local; make them absolute. */
            lv_obj_get_coords(child, &base);
            for (b = 0; ; b++) {
                lv_area_t area;
                char name[48];
                const char *text;

                if (!lv_buttonmatrix_get_button_area(child, b, &area)) {
                    break;
                }
                area.x1 += base.x1;
                area.x2 += base.x1;
                area.y1 += base.y1;
                area.y2 += base.y1;
                text = lv_buttonmatrix_get_button_text(child, b);
                snprintf(name, sizeof(name), "kbd:%.32s", (text != NULL) ? text : "?");
                ui_target_add(child, b, &area, name);
            }
        } else if (w > 0 && h > 0 && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t area;
            char name[48];

            lv_obj_get_coords(child, &area);
            ui_target_label(child, name, sizeof(name));
            ui_target_add(child, UINT32_MAX, &area, name);
        }

        ui_collect_targets(child);
    }
}

/** Rebuild the target list under the LVGL lock. */
static bool ui_build_targets(void)
{
    if (ui_targets_buffer() == NULL) {
        return false;
    }
    if (!lvgl_port_lock(3000)) {
        return false;
    }
    s_target_count = 0;
    ui_collect_targets(lv_screen_active());
    lvgl_port_unlock();
    return true;
}

static bool ui_area_contains(const lv_area_t *area, int x, int y)
{
    return x >= area->x1 && x <= area->x2 && y >= area->y1 && y <= area->y2;
}

/** Center of an on-screen-keyboard button with @p label. LVGL lock held by
 *  the caller? No: takes the lock itself. */
static bool ui_key_center(const char *label, int *x_out, int *y_out)
{
    lv_obj_t *kb = windows_get_keyboard();
    bool found = false;

    if (kb == NULL || label == NULL) {
        return false;
    }
    if (!lvgl_port_lock(3000)) {
        return false;
    }
    if (lv_obj_has_class(kb, &lv_buttonmatrix_class)) {
        lv_area_t base;
        uint32_t b;

        lv_obj_get_coords(kb, &base);
        for (b = 0; ; b++) {
            lv_area_t area;
            const char *text;

            if (!lv_buttonmatrix_get_button_area(kb, b, &area)) {
                break;
            }
            text = lv_buttonmatrix_get_button_text(kb, b);
            if (text != NULL && strcmp(text, label) == 0) {
                *x_out = base.x1 + area.x1 + (area.x2 - area.x1) / 2;
                *y_out = base.y1 + area.y1 + (area.y2 - area.y1) / 2;
                found = true;
                break;
            }
        }
    }
    lvgl_port_unlock();
    return found;
}

/* ========================================================================
 * VERB HANDLERS
 * ======================================================================== */

static int ui_cmd_targets(bool bare, const char *var)
{
    int i;

    if (!ui_build_targets()) {
        ui_out("ui targets: not ready\n");
        return 1;
    }
    if (!bare) {
        ui_out("ui.targets: %d\n", s_target_count);
    }
    for (i = 0; i < s_target_count; i++) {
        const ui_target_t *t = &s_targets[i];
        /* Name last: it can contain spaces (e.g. list rows "1. one"). */
        ui_out_raw("%d %d %d %d %d %s\n", i,
                   (int)t->area.x1, (int)t->area.y1,
                   (int)(t->area.x2 - t->area.x1 + 1),
                   (int)(t->area.y2 - t->area.y1 + 1),
                   t->name);
    }
    if (var != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_target_count);
        shell_env_set(var, buf);
    }
    return 0;
}

static int ui_cmd_hit(int x, int y, const char *var)
{
    int i;
    int found = -1;

    if (!ui_build_targets()) {
        return 1;
    }
    /* Later entries are deeper in the tree (on top): prefer them. */
    for (i = 0; i < s_target_count; i++) {
        if (ui_area_contains(&s_targets[i].area, x, y)) {
            found = i;
        }
    }
    if (found >= 0) {
        ui_out("ui.hit: %d %s\n", found, s_targets[found].name);
        if (var != NULL) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", found);
            shell_env_set(var, buf);
        }
    } else {
        ui_out("ui.hit: none\n");
        if (var != NULL) {
            shell_env_set(var, "-1");
        }
    }
    return 0;
}

static int ui_cmd_state(bool bare, const char *var)
{
    char input[128] = "";
    char ghost[64] = "";
    char search[96] = "";
    const char *modal;
    const char *mode;
    const char *nav_str;
    bool kb_visible;
    keyboard_nav_key_state_t nav_key;
    editor_view_state_t ed;
    char line[896];

    if (!lvgl_port_lock(3000)) {
        return 1;
    }
    modal = modal_runtime_active_name();
    kb_visible = keyboard_is_visible();
    mode = keyboard_mode_name(keyboard_get_mode());
    editor_view_get_state(&ed);
    {
        lv_obj_t *il = windows_get_input_line();
        if (il != NULL) {
            const char *text = lv_textarea_get_text(il);
            if (text != NULL) {
                snprintf(input, sizeof(input), "%.64s", text);
            }
        }
    }
    {
        lv_obj_t *gl = windows_get_input_ghost();
        if (gl != NULL && !lv_obj_has_flag(gl, LV_OBJ_FLAG_HIDDEN)) {
            const char *gtext = lv_label_get_text(gl);
            if (gtext != NULL) {
                snprintf(ghost, sizeof(ghost), "%.32s", gtext);
            }
        }
    }
    {
        lv_obj_t *sl = windows_get_search_label();
        if (sl != NULL && !lv_obj_has_flag(sl, LV_OBJ_FLAG_HIDDEN)) {
            const char *stext = lv_label_get_text(sl);
            if (stext != NULL) {
                snprintf(search, sizeof(search), "%.64s", stext);
            }
        }
    }
    lvgl_port_unlock();

    /* Takes its own recursive lock: report the symbols page's Nav key state so
     * tests can assert situational greying in shell vs editor context. */
    nav_key = keyboard_nav_key_state();
    nav_str = (nav_key == KEYBOARD_NAV_KEY_DISABLED) ? "off" :
              (nav_key == KEYBOARD_NAV_KEY_ENABLED) ? "on" : "na";

    snprintf(line, sizeof(line),
             "modal=%s keyboard=%s mode=%s editor=%s modified=%d row=%u col=%u "
             "lines=%u readonly=%d preview=%d wrap=%d nav=%s path=%.160s input=%s "
             "ghost=%s search=%s\n",
             (modal != NULL) ? modal : "none",
             kb_visible ? "visible" : "hidden",
             (mode != NULL) ? mode : "?",
             ed.open ? "open" : "closed",
             ed.modified ? 1 : 0,
             (unsigned)ed.cursor_row, (unsigned)ed.cursor_col,
             (unsigned)ed.line_count, ed.readonly ? 1 : 0,
             ed.preview ? 1 : 0, ed.wrap ? 1 : 0, nav_str,
             (ed.path[0] != '\0') ? ed.path : "(unnamed)",
             input, ghost, search);
    if (!bare) {
        ui_out("ui.state: ");
    }
    ui_out("%s", line);
    if (var != NULL) {
        shell_env_set(var, line);
    }
    return 0;
}

/* ========================================================================
 * DISPATCH
 * ======================================================================== */

static int ui_run(int argc, char **argv)
{
    const char *sub;
    int pos[8];
    int npos = 0;
    bool bare = false;
    const char *var = NULL;
    int i;

    if (argc < 2) {
        ui_out("Usage: ui <tap|longpress|swipe|press|move|release|target|key|"
               "targets|hit|state> [...]\n");
        return 2;
    }
    sub = argv[1];

    for (i = 2; i < argc && npos < 8; i++) {
        if (strcmp(argv[i], "/b") == 0) {
            bare = true;
        } else if (strncmp(argv[i], "/v:", 3) == 0) {
            var = argv[i] + 3;
        } else {
            pos[npos++] = i;
        }
    }

    if (shell_text_equals_ignore_case(sub, "targets")) {
        return ui_cmd_targets(bare, var);
    }
    if (shell_text_equals_ignore_case(sub, "state")) {
        return ui_cmd_state(bare, var);
    }
    if (shell_text_equals_ignore_case(sub, "hit")) {
        if (npos < 2) {
            ui_out("Usage: ui hit <x> <y> [/b] [/v:NAME]\n");
            return 2;
        }
        return ui_cmd_hit(atoi(argv[pos[0]]), atoi(argv[pos[1]]), var);
    }
    if (shell_text_equals_ignore_case(sub, "tap")) {
        uint32_t ms = (npos >= 3) ? (uint32_t)atoi(argv[pos[2]]) : 80;
        if (npos < 2) {
            ui_out("Usage: ui tap <x> <y> [ms]\n");
            return 2;
        }
        return (ui_test_tap(atoi(argv[pos[0]]), atoi(argv[pos[1]]), ms) == ESP_OK)
                   ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "longpress")) {
        uint32_t ms = (npos >= 3) ? (uint32_t)atoi(argv[pos[2]]) : 800;
        if (npos < 2) {
            ui_out("Usage: ui longpress <x> <y> [ms]\n");
            return 2;
        }
        return (ui_test_long_press(atoi(argv[pos[0]]), atoi(argv[pos[1]]), ms) == ESP_OK)
                   ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "swipe")) {
        uint32_t ms = (npos >= 5) ? (uint32_t)atoi(argv[pos[4]]) : 300;
        int steps = (npos >= 6) ? atoi(argv[pos[5]]) : 10;
        if (npos < 4) {
            ui_out("Usage: ui swipe <x1> <y1> <x2> <y2> [ms] [steps]\n");
            return 2;
        }
        return (ui_test_swipe(atoi(argv[pos[0]]), atoi(argv[pos[1]]),
                              atoi(argv[pos[2]]), atoi(argv[pos[3]]),
                              ms, steps) == ESP_OK) ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "press")) {
        if (npos < 2) {
            ui_out("Usage: ui press <x> <y>\n");
            return 2;
        }
        return (ui_test_press(atoi(argv[pos[0]]), atoi(argv[pos[1]])) == ESP_OK)
                   ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "move")) {
        if (npos < 2) {
            ui_out("Usage: ui move <x> <y>\n");
            return 2;
        }
        return (ui_test_move(atoi(argv[pos[0]]), atoi(argv[pos[1]])) == ESP_OK)
                   ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "release")) {
        return (ui_test_release() == ESP_OK) ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "key")) {
        int x;
        int y;
        uint32_t ms = (npos >= 2) ? (uint32_t)atoi(argv[pos[1]]) : 80;
        if (npos < 1 || !ui_key_center(argv[pos[0]], &x, &y)) {
            ui_out("ui key: no key '%s'\n", (npos >= 1) ? argv[pos[0]] : "");
            return 1;
        }
        return (ui_test_tap(x, y, ms) == ESP_OK) ? 0 : 1;
    }
    if (shell_text_equals_ignore_case(sub, "target")) {
        int id;
        lv_obj_t *obj;
        uint32_t btn;

        if (npos < 1 || !ui_build_targets()) {
            ui_out("Usage: ui target <id>\n");
            return 2;
        }
        id = atoi(argv[pos[0]]);
        if (id < 0 || id >= s_target_count) {
            ui_out("ui target: bad id %d (0..%d)\n", id, s_target_count - 1);
            return 1;
        }
        obj = s_targets[id].obj;
        btn = s_targets[id].btn_id;

        /* Semantic activation: modal widgets live in the auto-scrolling
         * transcript, so a coordinate tap is racy (the view can snap back
         * before the tap is processed). Send the widget's own activation
         * event instead; `ui tap` remains the raw-coordinate path. */
        if (lvgl_port_lock(3000)) {
            if (btn == UINT32_MAX) {
                lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
            } else {
                lv_buttonmatrix_set_selected_button(obj, btn);
                lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, &btn);
            }
            lvgl_port_unlock();
        }
        ui_out("ui.target: %s activated\n", s_targets[id].name);
        return 0;
    }

    ui_out("ui: unknown verb '%s'\n", sub);
    return 2;
}

/* ========================================================================
 * PUBLIC ENTRY POINTS
 * ======================================================================== */

bool shell_command_ui(int argc, char **argv)
{
    int rc;

    if (!ui_test_is_ready()) {
        (void)ui_test_init();
    }
    s_console_mode = false;
    rc = ui_run(argc, argv);
    batch_set_errorlevel(rc);
    return true;
}

bool ui_commands_console(const char *line)
{
    char buf[SHELL_COMMAND_BYTES];
    char *argv[24];
    int argc;

    if (line == NULL) {
        return false;
    }
    snprintf(buf, sizeof(buf), "%s", line);
    argc = shell_split_args(buf, argv, 24);
    if (argc < 1 || !shell_text_equals_ignore_case(argv[0], "ui")) {
        return false;
    }

    if (!ui_test_is_ready()) {
        (void)ui_test_init();
    }
    s_console_mode = true;
    (void)ui_run(argc, argv);
    s_console_mode = false;
    return true;
}
