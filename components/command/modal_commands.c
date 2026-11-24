/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file modal_commands.c
 * @brief Modal layer verbs: mode, mbind, mcmd, mdialog, guard, scope,
 *        palette, mmacro, pending, mstatus, repeat, cancel, buffer,
 *        plus the shared-runtime surface verbs mpick / mprog / mformx / mstack.
 *
 * These verbs drive the modal layer (components/tui/tui_modal.{h,c}).
 * Label execution and env access are routed through callbacks registered
 * at init so the modal core stays a leaf.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "batch.h"
#include "tui.h"
#include "tui_modal.h"
#include "tui_pane.h"
#include "modal.h"
#include "modal_surf.h"
#include "command.h"
#include "p4minishell_config.h"

/* ========================================================================
 * Callback routing
 * ================================================================ */

static int modal_exec_label(const char *label, void *context)
{
    (void)context;
    if (label == NULL || label[0] == '\0') return 0;
    shell_execute_command((char *)label);
    return batch_get_errorlevel();
}

static const char *modal_env_get(const char *name)
{
    return shell_env_get(name);
}

static int modal_env_set(const char *name, const char *value)
{
    return shell_env_set(name, value) == ESP_OK ? 0 : 1;
}

bool modal_layer_init(void)
{
    return modal_init(modal_exec_label, modal_env_get, modal_env_set, NULL);
}

/* ========================================================================
 * Helper: parse frame type string
 * ================================================================ */

static modal_frame_type_t modal_type_from_str(const char *s)
{
    if (shell_text_equals_ignore_case(s, "normal")) return MODAL_FRAME_NORMAL;
    if (shell_text_equals_ignore_case(s, "insert")) return MODAL_FRAME_INSERT;
    if (shell_text_equals_ignore_case(s, "dialog")) return MODAL_FRAME_DIALOG;
    if (shell_text_equals_ignore_case(s, "palette")) return MODAL_FRAME_PALETTE;
    if (shell_text_equals_ignore_case(s, "macro")) return MODAL_FRAME_MACRO;
    return MODAL_FRAME_NORMAL;
}

static const char *modal_type_str(modal_frame_type_t type)
{
    switch (type) {
    case MODAL_FRAME_NORMAL: return "normal";
    case MODAL_FRAME_INSERT: return "insert";
    case MODAL_FRAME_DIALOG: return "dialog";
    case MODAL_FRAME_PALETTE: return "palette";
    case MODAL_FRAME_MACRO: return "macro";
    default: return "unknown";
    }
}

/* ========================================================================
 * mode — manage the mode stack
 * ================================================================ */

bool shell_command_mode(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mode /push NAME [/type T] [/map M] | mode /pop | mode /switch NAME | mode /top | mode /list | mode /count | mode /drop TYPE | mode /timeout MS");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "push")) {
        if (argc < 3) { shell_print_usage("Usage: mode /push NAME [/type T] [/map M]"); batch_set_errorlevel(2); return false; }
        modal_frame_type_t type = MODAL_FRAME_NORMAL;
        const char *map_name = NULL;
        for (int i = 3; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/type") && i + 1 < argc) { type = modal_type_from_str(argv[i + 1]); i++; }
            else if (shell_text_equals_ignore_case(argv[i], "/map") && i + 1 < argc) { map_name = argv[i + 1]; i++; }
        }
        if (!modal_mode_push(argv[2], type, map_name)) { shell_print_error("mode /push: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "pop")) {
        if (!modal_mode_pop()) { shell_print_error("mode /pop: guard veto or empty"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "switch")) {
        if (argc < 3) { shell_print_usage("Usage: mode /switch NAME"); batch_set_errorlevel(2); return false; }
        if (!modal_mode_switch(argv[2])) { shell_print_error("mode /switch: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "top")) {
        int top = modal_mode_top();
        if (top < 0) { shell_transcript_appendf("mode/top: empty\n"); batch_set_errorlevel(0); return true; }
        char name[16];
        modal_mode_get_name(0, name, sizeof(name));
        modal_frame_type_t type = modal_mode_get_type(0);
        shell_env_set("M_NAME", name);
        shell_env_set("M_TYPE", modal_type_str(type));
        shell_env_set("M_DEPTH", "0");
        shell_transcript_appendf("mode/top: %s %s depth=%d\n", name, modal_type_str(type), top);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = modal_mode_count();
        shell_transcript_appendf("mode/list: %d frame(s)\n", count);
        for (int i = count - 1; i >= 0; i--) {
            char name[16];
            modal_mode_get_name(count - 1 - i, name, sizeof(name));
            shell_transcript_appendf("  [%d] %s %s\n", count - 1 - i, name, modal_type_str(modal_mode_get_type(count - 1 - i)));
        }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "count")) {
        shell_transcript_appendf("%d\n", modal_mode_count());
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "drop")) {
        if (argc < 3) { shell_print_usage("Usage: mode /drop TYPE"); batch_set_errorlevel(2); return false; }
        int dropped = modal_mode_drop_by_type(modal_type_from_str(argv[2]));
        shell_transcript_appendf("mode/drop: dropped %d frame(s)\n", dropped);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "timeout")) {
        if (argc < 3) { shell_print_usage("Usage: mode /timeout MS"); batch_set_errorlevel(2); return false; }
        modal_set_chord_timeout((uint32_t)atoi(argv[2]));
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mode /push|/pop|/switch|/top|/list|/count|/drop|/timeout");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * mbind — chords to commands, per mode
 * ================================================================ */

bool shell_command_mbind(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mbind /map NAME CHORD LABEL | mbind /unbind NAME CHORD | mbind /leader KEY | mbind /list NAME");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "map")) {
        if (argc < 5) { shell_print_usage("Usage: mbind /map NAME CHORD LABEL"); batch_set_errorlevel(2); return false; }
        if (!modal_bind(argv[2], argv[3], argv[4])) { shell_print_error("mbind /map: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "unbind")) {
        if (argc < 4) { shell_print_usage("Usage: mbind /unbind NAME CHORD"); batch_set_errorlevel(2); return false; }
        if (!modal_unbind(argv[2], argv[3])) { shell_print_error("mbind /unbind: not found"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "leader")) {
        if (argc < 3) { shell_print_usage("Usage: mbind /leader KEY"); batch_set_errorlevel(2); return false; }
        if (!modal_set_leader(argv[2])) { shell_print_error("mbind /leader: need single char"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        if (argc < 3) { shell_print_usage("Usage: mbind /list NAME"); batch_set_errorlevel(2); return false; }
        int count = modal_bind_count(argv[2]);
        shell_transcript_appendf("mbind/list: %s has %d binding(s)\n", argv[2], count);
        for (int i = 0; i < count; i++) {
            char chord[32], label[32];
            modal_bind_get_chord(argv[2], i, chord, sizeof(chord));
            modal_bind_get_label(argv[2], i, label, sizeof(label));
            shell_transcript_appendf("  %-16s -> %s\n", chord, label);
        }
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mbind /map|/unbind|/leader|/list");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * mcmd — named commands and dispatch
 * ================================================================ */

bool shell_command_mcmd(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mcmd /def NAME LABEL [/doc D] | mcmd /list | mcmd /run NAME | mcmd /undef NAME");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "def")) {
        if (argc < 4) { shell_print_usage("Usage: mcmd /def NAME LABEL [/doc D]"); batch_set_errorlevel(2); return false; }
        const char *doc = NULL;
        for (int i = 4; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/doc") && i + 1 < argc) { doc = argv[i + 1]; break; }
        }
        if (!modal_cmd_def(argv[2], argv[3], doc)) { shell_print_error("mcmd /def: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = modal_cmd_count();
        shell_transcript_appendf("mcmd/list: %d command(s)\n", count);
        for (int i = 0; i < count; i++) {
            char name[16];
            modal_cmd_get_name(i, name, sizeof(name));
            const char *doc = modal_cmd_get_doc(name);
            shell_transcript_appendf("  %-16s %s\n", name, doc != NULL ? doc : "");
        }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "run")) {
        if (argc < 3) { shell_print_usage("Usage: mcmd /run NAME"); batch_set_errorlevel(2); return false; }
        const char *label = modal_cmd_find(argv[2]);
        if (label == NULL) { shell_print_error("mcmd /run: '%s' not defined", argv[2]); batch_set_errorlevel(1); return false; }
        shell_execute_command((char *)label);
        batch_set_errorlevel(batch_get_errorlevel());
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "undef")) {
        if (argc < 3) { shell_print_usage("Usage: mcmd /undef NAME"); batch_set_errorlevel(2); return false; }
        if (!modal_cmd_undef(argv[2])) { shell_print_error("mcmd /undef: '%s' not found", argv[2]); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mcmd /def|/list|/run|/undef");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * mdialog — blocking nested dialogs with return values
 * ================================================================ */

bool shell_command_mdialog(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mdialog /confirm TEXT [/var V] | mdialog /input TEXT [/default D] [/var V] | mdialog /result V");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "confirm")) {
        if (argc < 3) { shell_print_usage("Usage: mdialog /confirm TEXT [/var V]"); batch_set_errorlevel(2); return false; }
        const char *var = NULL;
        for (int i = 3; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/var") && i + 1 < argc) { var = argv[i + 1]; break; }
        }
        /* For now, just log and set result. Full dialog UI is modal-surface. */
        shell_transcript_appendf("mdialog: %s\n", argv[2]);
        modal_dialog_push("confirm", argv[2]);
        modal_dialog_set_result("1");
        modal_dialog_pop();
        if (var != NULL) shell_env_set(var, "1");
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "input")) {
        if (argc < 3) { shell_print_usage("Usage: mdialog /input TEXT [/default D] [/var V]"); batch_set_errorlevel(2); return false; }
        const char *var = NULL;
        const char *def = "";
        for (int i = 3; i < argc; i++) {
            if (shell_text_equals_ignore_case(argv[i], "/var") && i + 1 < argc) { var = argv[i + 1]; i++; }
            else if (shell_text_equals_ignore_case(argv[i], "/default") && i + 1 < argc) { def = argv[i + 1]; i++; }
        }
        modal_dialog_push("input", argv[2]);
        modal_dialog_set_result(def);
        modal_dialog_pop();
        if (var != NULL) shell_env_set(var, def);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "result")) {
        if (argc < 3) { shell_print_usage("Usage: mdialog /result V"); batch_set_errorlevel(2); return false; }
        shell_env_set(argv[2], modal_dialog_get_result());
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mdialog /confirm|/input|/result");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * guard — transition hooks and veto
 * ================================================================ */

bool shell_command_guard(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: guard /on  enter|leave TYPE LABEL | guard /off enter|leave TYPE | guard /list");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "on")) {
        if (argc < 5) { shell_print_usage("Usage: guard /on enter|leave TYPE LABEL"); batch_set_errorlevel(2); return false; }
        modal_guard_kind_t kind = shell_text_equals_ignore_case(argv[2], "leave") ? MODAL_GUARD_LEAVE : MODAL_GUARD_ENTER;
        modal_frame_type_t type = modal_type_from_str(argv[3]);
        if (!modal_guard_set(kind, type, argv[4])) { shell_print_error("guard /on: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "off")) {
        if (argc < 4) { shell_print_usage("Usage: guard /off enter|leave TYPE"); batch_set_errorlevel(2); return false; }
        modal_guard_kind_t kind = shell_text_equals_ignore_case(argv[2], "leave") ? MODAL_GUARD_LEAVE : MODAL_GUARD_ENTER;
        modal_frame_type_t type = modal_type_from_str(argv[3]);
        if (!modal_guard_clear(kind, type)) { shell_print_error("guard /off: not found"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        for (int k = 0; k < MODAL_GUARD_KIND_COUNT; k++) {
            for (int t = 0; t < MODAL_FRAME_TYPE_COUNT; t++) {
                const char *label = modal_guard_get(k, t);
                if (label != NULL) {
                    shell_transcript_appendf("guard %s %s -> %s\n",
                        k == MODAL_GUARD_ENTER ? "enter" : "leave", modal_type_str(t), label);
                }
            }
        }
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: guard /on|/off|/list");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * scope — mode-scoped variable bags
 * ================================================================ */

bool shell_command_scope(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: scope /save MODE VAR [VAR ...] | scope /restore MODE | scope /list MODE | scope /clear MODE");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "save")) {
        if (argc < 4) { shell_print_usage("Usage: scope /save MODE VAR [VAR ...]"); batch_set_errorlevel(2); return false; }
        int count = argc - 3;
        const char **vars = (const char **)&argv[3];
        if (!modal_scope_save(argv[2], count, vars)) { shell_print_error("scope /save: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "restore")) {
        if (argc < 3) { shell_print_usage("Usage: scope /restore MODE"); batch_set_errorlevel(2); return false; }
        if (!modal_scope_restore(argv[2])) { shell_print_error("scope /restore: not found"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        if (argc < 3) { shell_print_usage("Usage: scope /list MODE"); batch_set_errorlevel(2); return false; }
        char buf[256];
        int count = modal_scope_list(argv[2], buf, sizeof(buf));
        shell_transcript_appendf("scope/list: %s = %d var(s): %s\n", argv[2], count, buf);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        if (argc < 3) { shell_print_usage("Usage: scope /clear MODE"); batch_set_errorlevel(2); return false; }
        if (!modal_scope_clear(argv[2])) { shell_print_error("scope /clear: not found"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: scope /save|/restore|/list|/clear");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * palette — fuzzy command dispatch
 * ================================================================ */

bool shell_command_palette(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: palette /open | palette /close | palette /source NAME");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "open")) {
        modal_mode_push("palette", MODAL_FRAME_PALETTE, NULL);
        shell_transcript_appendf("palette: open\n");
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (modal_mode_count() > 0) modal_mode_pop();
        shell_transcript_appendf("palette: close\n");
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "source")) {
        shell_transcript_appendf("palette: source %s\n", argc > 2 ? argv[2] : "none");
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: palette /open|/close|/source");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * mmacro — record and replay
 * ================================================================ */

bool shell_command_mmacro(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mmacro /record NAME | mmacro /stop | mmacro /replay NAME [N] | mmacro /list");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "record")) {
        if (argc < 3) { shell_print_usage("Usage: mmacro /record NAME"); batch_set_errorlevel(2); return false; }
        if (!modal_macro_record(argv[2])) { shell_print_error("mmacro /record: failed"); batch_set_errorlevel(1); return false; }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "stop")) {
        int count = modal_macro_stop();
        shell_transcript_appendf("mmacro/stop: captured %d event(s)\n", count);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "replay")) {
        if (argc < 3) { shell_print_usage("Usage: mmacro /replay NAME [N]"); batch_set_errorlevel(2); return false; }
        int times = (argc > 3) ? atoi(argv[3]) : 1;
        int el = modal_macro_replay(argv[2], times);
        batch_set_errorlevel(el);
        return el == 0;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        int count = modal_macro_count();
        shell_transcript_appendf("mmacro/list: %d macro(s)\n", count);
        for (int i = 0; i < count; i++) {
            char name[16];
            modal_macro_get_name(i, name, sizeof(name));
            shell_transcript_appendf("  %s\n", name);
        }
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mmacro /record|/stop|/replay|/list");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * pending — inspect and clear the chord buffer
 * ================================================================ */

bool shell_command_pending(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: pending /get | pending /clear | pending /timeout MS");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "get")) {
        char buf[128];
        if (modal_chord_pending(buf, sizeof(buf))) shell_transcript_appendf("%s\n", buf);
        else shell_transcript_appendf("(empty)\n");
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        modal_chord_clear();
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "timeout")) {
        if (argc < 3) { shell_print_usage("Usage: pending /timeout MS"); batch_set_errorlevel(2); return false; }
        modal_set_chord_timeout((uint32_t)atoi(argv[2]));
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: pending /get|/clear|/timeout");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * mstatus — per-mode statusline binding
 * ================================================================ */

bool shell_command_mstatus(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: mstatus /set TYPE FORMAT | mstatus /render");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "set")) {
        if (argc < 4) { shell_print_usage("Usage: mstatus /set TYPE FORMAT"); batch_set_errorlevel(2); return false; }
        modal_frame_type_t type = modal_type_from_str(argv[2]);
        modal_set_status_render_callback(type, argv[3]);
        shell_transcript_appendf("mstatus/set: %s = %s\n", argv[2], argv[3]);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "render")) {
        shell_transcript_appendf("mstatus/render: mode count=%d\n", modal_mode_count());
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: mstatus /set|/render");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * repeat — repeat the last command with a count
 * ================================================================ */

bool shell_command_repeat(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: repeat /last [N] | repeat /set N | repeat /clear");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "last")) {
        int n = (argc > 2) ? atoi(argv[2]) : modal_get_repeat();
        for (int i = 0; i < n; i++) {
            /* Re-run last command from history would need more infra. */
            shell_transcript_appendf("repeat: %d/%d\n", i + 1, n);
        }
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "set")) {
        if (argc < 3) { shell_print_usage("Usage: repeat /set N"); batch_set_errorlevel(2); return false; }
        modal_set_repeat(atoi(argv[2]));
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        modal_clear_repeat();
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: repeat /last|/set|/clear");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * cancel — unwind modal state
 * ================================================================ */

bool shell_command_cancel(int argc, char **argv)
{
    if (argc < 2 || (
        !shell_text_equals_ignore_case(argv[1], "to") &&
        !shell_text_equals_ignore_case(argv[1], "all") &&
        !shell_text_equals_ignore_case(argv[1], "code"))) {
        /* Default: pop one frame. */
        if (modal_mode_count() <= 0) { shell_transcript_appendf("cancel: stack empty\n"); batch_set_errorlevel(0); return true; }
        modal_mode_pop();
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "to")) {
        if (argc < 3) { shell_print_usage("Usage: cancel /to NAME"); batch_set_errorlevel(2); return false; }
        modal_mode_switch(argv[2]);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "all")) {
        while (modal_mode_count() > 0) modal_mode_pop();
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "code")) {
        if (argc < 3) { shell_print_usage("Usage: cancel /code N"); batch_set_errorlevel(2); return false; }
        int code = atoi(argv[2]);
        while (modal_mode_count() > 0) modal_mode_pop();
        batch_set_errorlevel(code);
        return true;
    }

    batch_set_errorlevel(0);
    return true;
}

/* ========================================================================
 * buffer — modal text buffer
 * ================================================================ */

bool shell_command_buffer(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: buffer /open NAME | buffer /close NAME | buffer /list | buffer /mode NAME MODE");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "open")) {
        if (argc < 3) { shell_print_usage("Usage: buffer /open NAME"); batch_set_errorlevel(2); return false; }
        shell_transcript_appendf("buffer/open: %s\n", argv[2]);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (argc < 3) { shell_print_usage("Usage: buffer /close NAME"); batch_set_errorlevel(2); return false; }
        shell_transcript_appendf("buffer/close: %s\n", argv[2]);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "list")) {
        shell_transcript_appendf("buffer/list: %d buffer(s)\n", 0);
        batch_set_errorlevel(0); return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "mode")) {
        if (argc < 4) { shell_print_usage("Usage: buffer /mode NAME MODE"); batch_set_errorlevel(2); return false; }
        shell_transcript_appendf("buffer/mode: %s -> %s\n", argv[2], argv[3]);
        batch_set_errorlevel(0); return true;
    }

    shell_print_usage("Usage: buffer /open|/close|/list|/mode");
    batch_set_errorlevel(2);
    return false;
}

/* ========================================================================
 * SHARED-RUNTIME SURFACE VERBS (v1.5 additive expansion)
 * ========================================================================
 * These four verbs drive the ONE shared modal runtime in components/modal.
 * They use the same /t:SECS and /v:NAME parsers as the older surfaces and the
 * runtime's existing input routing; no new scheduler, display or input path.
 * Dispatch in command.c and help in shell_core.c are wired by the integrator.
 */

#define MODAL_VERB_MAX_ITEMS 32
#define MODAL_VERB_VALUE_BYTES 512

/** Split a `a|b|c` argument into a borrowed pointer array. */
static int modal_split_items(char *spec, const char **out, int capacity)
{
    int count = 0;
    char *p = spec;

    if (spec == NULL || out == NULL || capacity <= 0) {
        return 0;
    }
    out[count++] = spec;
    while (*p != '\0' && count < capacity) {
        if (*p == '|') {
            *p = '\0';
            out[count++] = p + 1;
        }
        p++;
    }
    return count;
}

/** Write an integer result to a /v: variable (no-op when var is NULL). */
static void modal_verb_set_var(const char *var, int value)
{
    char buf[32];

    if (var == NULL) {
        return;
    }
    snprintf(buf, sizeof(buf), "%d", value);
    (void)shell_env_set(var, buf);
}

/* ---- mpick — scrollable picker ---- */

bool shell_command_mpick(int argc, char **argv)
{
    const char *items_arg = NULL;
    const char *title = NULL;
    const char *var = NULL;
    const char *items[MODAL_VERB_MAX_ITEMS];
    uint32_t timeout_ms = 0;
    char *copy;
    int initial = 0;
    int count;
    int selected = -1;
    int i;

    for (i = 2; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "/items") && i + 1 < argc) {
            items_arg = argv[++i];
        } else if (shell_text_equals_ignore_case(argv[i], "/title") && i + 1 < argc) {
            title = argv[++i];
        } else if (shell_text_equals_ignore_case(argv[i], "/selected") && i + 1 < argc) {
            initial = atoi(argv[++i]);
        } else if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            /* /t: handled by the shared parser. */
        } else if (modal_parse_var_arg(argv[i], &var)) {
            /* /v: handled by the shared parser. */
        }
    }
    if (items_arg == NULL || items_arg[0] == '\0') {
        shell_print_usage("Usage: mpick /items A|B|C [/title T] [/selected N] [/t:SECS] [/v:VAR]");
        batch_set_errorlevel(2);
        return false;
    }
    copy = (char *)malloc(strlen(items_arg) + 1);
    if (copy == NULL) {
        shell_print_error("mpick: out of memory");
        batch_set_errorlevel(1);
        return false;
    }
    strcpy(copy, items_arg);
    count = modal_split_items(copy, items, MODAL_VERB_MAX_ITEMS);
    (void)modal_picker_run(title, items, count, initial, timeout_ms, &selected);
    free(copy);
    modal_verb_set_var(var, selected);
    batch_set_errorlevel(selected >= 0 ? 0 : 1);
    return selected >= 0;
}

/* ---- mprog — async progress surface ---- */

bool shell_command_mprog(int argc, char **argv)
{
    const char *title = NULL;
    const char *var = NULL;
    uint32_t timeout_ms = 0;
    uint32_t total = 100;
    uint32_t step = 1;
    uint32_t tick_ms = 250;
    int i;
    int result;

    if (argc < 3 || !shell_text_equals_ignore_case(argv[1], "/run")) {
        shell_print_usage("Usage: mprog /run TITLE [/total N] [/step N] [/tick MS] [/t:SECS] [/v:VAR]");
        batch_set_errorlevel(2);
        return false;
    }
    title = argv[2];
    for (i = 3; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "/total") && i + 1 < argc) {
            long n = atol(argv[++i]);
            total = n > 0 ? (uint32_t)n : 100;
        } else if (shell_text_equals_ignore_case(argv[i], "/step") && i + 1 < argc) {
            long n = atol(argv[++i]);
            step = n >= 0 ? (uint32_t)n : 0;
        } else if (shell_text_equals_ignore_case(argv[i], "/tick") && i + 1 < argc) {
            long n = atol(argv[++i]);
            tick_ms = n > 0 ? (uint32_t)n : 250;
        } else if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            /* /t: */
        } else if (modal_parse_var_arg(argv[i], &var)) {
            /* /v: */
        }
    }
    result = modal_progress_run(title, title, total, tick_ms, step, timeout_ms);
    modal_verb_set_var(var, result);
    batch_set_errorlevel(result == 0 ? 0 : 1);
    return result == 0;
}

/* ---- mformx — quick form with focus walking ---- */

bool shell_command_mformx(int argc, char **argv)
{
    modal_form_field_t fields[MODAL_FORM_MAX_FIELDS];
    const char *specs[MODAL_FORM_MAX_FIELDS];
    char values[MODAL_FORM_MAX_FIELDS][64];
    const char *title = NULL;
    const char *var = NULL;
    const char *accept = NULL;
    const char *cancel = NULL;
    uint32_t timeout_ms = 0;
    char joined[MODAL_VERB_VALUE_BYTES];
    char *copy = NULL;
    int count;
    int focus = 0;
    int result;
    int i;
    size_t used = 0;

    if (argc < 4 || !shell_text_equals_ignore_case(argv[1], "/run")) {
        shell_print_usage("Usage: mformx /run TITLE LABEL?|LABEL!|LABEL* [/accept S] [/cancel S] [/t:SECS] [/v:VAR]");
        batch_set_errorlevel(2);
        return false;
    }
    title = argv[2];
    for (i = 3; i < argc; i++) {
        if (shell_text_equals_ignore_case(argv[i], "/accept") && i + 1 < argc) {
            accept = argv[++i];
        } else if (shell_text_equals_ignore_case(argv[i], "/cancel") && i + 1 < argc) {
            cancel = argv[++i];
        } else if (modal_parse_timeout_arg(argv[i], &timeout_ms)) {
            /* /t: */
        } else if (modal_parse_var_arg(argv[i], &var)) {
            /* /v: */
        } else if (copy == NULL) {
            copy = (char *)argv[i];   /* the pipe-separated spec (borrowed) */
        }
    }
    /* The spec may need splitting; make a private writable copy. */
    if (copy == NULL) {
        shell_print_usage("Usage: mformx /run TITLE SPEC [/t:SECS] [/v:VAR]");
        batch_set_errorlevel(2);
        return false;
    }
    i = (int)strlen(copy);
    char *scratch = (char *)malloc((size_t)i + 1);
    if (scratch == NULL) {
        shell_print_error("mformx: out of memory");
        batch_set_errorlevel(1);
        return false;
    }
    strcpy(scratch, copy);

    memset(fields, 0, sizeof(fields));
    memset(specs, 0, sizeof(specs));
    memset(values, 0, sizeof(values));
    count = modal_split_items(scratch, specs, MODAL_FORM_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        char *label = (char *)specs[i];
        size_t len = strlen(label);

        fields[i].label = label;
        fields[i].value = values[i];
        fields[i].value_size = sizeof(values[i]);
        if (len > 0 && (label[len - 1] == '?' || label[len - 1] == '!' || label[len - 1] == '*')) {
            char marker = label[len - 1];
            label[len - 1] = '\0';
            fields[i].type = marker == '!' ? MODAL_FORM_PASSWORD :
                             marker == '*' ? MODAL_FORM_CHECK : MODAL_FORM_TEXT;
        } else {
            fields[i].type = MODAL_FORM_TEXT;
        }
    }
    result = modal_form_run_ex(title, fields, count, accept, cancel, timeout_ms, &focus);
    free(scratch);
    if (var != NULL) {
        joined[0] = '\0';
        for (i = 0; i < count && used + sizeof(values[i]) + 1 < sizeof(joined); i++) {
            if (i > 0 && used + 1 < sizeof(joined)) {
                joined[used++] = '|';
            }
            used += (size_t)snprintf(joined + used, sizeof(joined) - used, "%s", values[i]);
        }
        (void)shell_env_set(var, joined);
    }
    batch_set_errorlevel(result == 0 ? 0 : 1);
    return result == 0;
}

/* ---- mstack — suspended modal sessions (app switching) ---- */

bool shell_command_mstack(int argc, char **argv)
{
    const char *name;
    int depth;
    int value;
    uint8_t panel = 0;
    uint8_t scrim = 0;

    if (argc < 2) {
        shell_print_usage("Usage: mstack /push NAME | /pop | /suspend | /resume | /switch NAME | /top | /list | /depth | /find NAME | /focus N | /alpha PANEL SCRIM | /clear");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "push") && argc >= 3) {
        if (!modal_session_push(argv[2], NULL, NULL)) {
            shell_print_error("mstack /push: failed (full or bad name)");
            batch_set_errorlevel(1);
            return false;
        }
        shell_transcript_appendf("mstack/push: %s\n", argv[2]);
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "pop")) {
        bool ok = modal_session_pop();
        shell_transcript_appendf("mstack/pop: %s\n", ok ? "ok" : "empty");
        batch_set_errorlevel(ok ? 0 : 1);
        return ok;
    }
    if (shell_text_equals_ignore_case(argv[1], "suspend")) {
        bool ok = modal_session_suspend();
        batch_set_errorlevel(ok ? 0 : 1);
        return ok;
    }
    if (shell_text_equals_ignore_case(argv[1], "resume")) {
        bool ok = modal_session_resume();
        batch_set_errorlevel(ok ? 0 : 1);
        return ok;
    }
    if (shell_text_equals_ignore_case(argv[1], "switch") && argc >= 3) {
        bool ok = modal_session_switch(argv[2]);
        batch_set_errorlevel(ok ? 0 : 1);
        return ok;
    }
    if (shell_text_equals_ignore_case(argv[1], "top")) {
        name = modal_session_top_name();
        shell_transcript_appendf("mstack/top: %s\n", name != NULL ? name : "(empty)");
        (void)shell_env_set("MSTACK_TOP", name != NULL ? name : "");
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "list")) {
        char depth_buf[16];

        depth = modal_session_depth();
        shell_transcript_appendf("mstack/list: %d session(s)\n", depth);
        snprintf(depth_buf, sizeof(depth_buf), "%d", depth);
        (void)shell_env_set("MSTACK_DEPTH", depth_buf);
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "depth")) {
        char depth_buf[16];

        depth = modal_session_depth();
        shell_transcript_appendf("%d\n", depth);
        snprintf(depth_buf, sizeof(depth_buf), "%d", depth);
        (void)shell_env_set("MSTACK_DEPTH", depth_buf);
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "find") && argc >= 3) {
        int found = modal_session_find(argv[2]);
        shell_transcript_appendf("mstack/find: %s = %d\n", argv[2], found);
        batch_set_errorlevel(found >= 0 ? 0 : 1);
        return found >= 0;
    }
    if (shell_text_equals_ignore_case(argv[1], "focus")) {
        if (argc >= 3) {
            bool ok = modal_session_focus_set(atoi(argv[2]));
            batch_set_errorlevel(ok ? 0 : 1);
            return ok;
        }
        value = modal_session_focus_get();
        shell_transcript_appendf("mstack/focus: %d\n", value);
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "alpha") && argc >= 4) {
        panel = (uint8_t)atoi(argv[2]);
        scrim = (uint8_t)atoi(argv[3]);
        modal_surf_set_alpha(panel, scrim);
        modal_surf_get_alpha(&panel, &scrim);
        shell_transcript_appendf("mstack/alpha: panel=%u scrim=%u\n",
                                 (unsigned)panel, (unsigned)scrim);
        batch_set_errorlevel(0);
        return true;
    }
    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        modal_session_clear();
        batch_set_errorlevel(0);
        return true;
    }

    shell_print_usage("Usage: mstack /push|/pop|/suspend|/resume|/switch|/top|/list|/depth|/find|/focus|/alpha|/clear");
    batch_set_errorlevel(2);
    return false;
}
