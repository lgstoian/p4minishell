// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file sync_commands.h
 * @brief P4Sync handshake verb (`sync status`).
 *
 * Read-only capability snapshot for the P4Sync host prototype. The verb
 * lists no stores itself: file, PIM, package, and screenshot transfers
 * reuse the existing `receive` / `send` / `pim` / `pkg` / `asset` /
 * `screenshot` paths.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the `sync` verb.
 *
 * `sync` and `sync status` print the machine-readable `sync.*` handshake
 * lines. Any other shape prints usage.
 *
 * @param argc Argument count (argv[0] is "sync").
 * @param argv Argument words.
 * @return 0 on success, 2 on usage.
 */
int shell_command_sync(int argc, char **argv);

#ifdef __cplusplus
}
#endif
