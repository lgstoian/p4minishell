/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file breadcrumb_commands.c
 * @brief `breadcrumb` — the on-device location trail.
 *
 * Prints where the shell currently is as a web-style trail: the working
 * directory, the nested batch files (root first), the background slot, and
 * whether a modal surface owns input. It is the prompt-side counterpart of the
 * repository breadcrumb map (`tools/breadcrumbs.py`): a user or agent can ask
 * "where am I?" from the shell, and a batch file can read the `/b` line.
 *
 * The trail is assembled from the existing accessors only (storage cwd, batch
 * frame stack, background slot, modal runtime); there is no second state store.
 */

#include "command.h"
#include "ansi_palette.h"
#include "batch.h"
#include "modal.h"
#include "shell.h"
#include "storage.h"

#include <stdbool.h>
#include <stdio.h>

void command_breadcrumb_format(const char *cwd, const char *batch_trail,
                               int bg_slot, bool modal_active,
                               char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "cwd=%s | batch=%s | bg=%d | modal=%s",
             (cwd != NULL && cwd[0] != '\0') ? cwd : "?",
             (batch_trail != NULL && batch_trail[0] != '\0') ? batch_trail : "none",
             bg_slot,
             modal_active ? "on" : "off");
}

void shell_command_breadcrumb(int argc, char **argv)
{
    char batch[192];
    char trail[320];
    bool bare = false;

    if (argc >= 2) {
        if (shell_text_equals_ignore_case(argv[1], "/b")) {
            bare = true;
        } else {
            shell_print_usage("Usage: breadcrumb [/b]");
            batch_set_errorlevel(2);
            return;
        }
    }

    batch_frame_trail(batch, sizeof(batch));
    command_breadcrumb_format(shell_get_cwd(), batch, batch_bg_current_slot(),
                              modal_runtime_is_active(), trail, sizeof(trail));

    if (bare) {
        shell_transcript_appendf("%s\n", trail);
    } else {
        shell_print_heading("Breadcrumb");
        shell_transcript_appendf_ansi(SH_LBL "breadcrumb" SH_RST "=" SH_PATH "%s" SH_RST "\n",
                                      trail);
    }
    batch_set_errorlevel(0);
}
