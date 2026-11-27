// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file sync_commands.h
 * @brief P4Sync handshake verb (`sync status`) and its pure capability query.
 *
 * Read-only capability snapshot for the P4Sync host prototype. The verb
 * lists no stores itself: file, PIM, package, and screenshot transfers
 * reuse the existing `receive` / `send` / `pim` / `pkg` / `asset` /
 * `screenshot` paths.
 *
 * The host protocol reads the machine-readable `sync.*` lines. The pure
 * query below exposes the same constants to any in-process caller without a
 * second framing layer, store walk or CRC: it only assembles values that are
 * already compile-time facts of this firmware build.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handshake tag the host asserts before any bulk transfer. Bumping it is a
 *  breaking change to the host contract. */
#define P4SYNC_PROTO_TAG "p4sync-1"

/**
 * Static capabilities advertised by `sync status`.
 *
 * Every field comes from an existing config macro or board identity; no
 * store is enumerated and no transport state is captured here. The dynamic
 * SD/lock/free-space lines are printed by the verb itself, not by this query.
 */
typedef struct {
    const char *proto_tag;   /**< P4SYNC_PROTO_TAG. */
    const char *board_id;    /**< Active board profile slug. */
    const char *send_magic;  /**< SDFX framing magic. */
    const char *pim_magic;   /**< PIMX framing magic. */
    const char *shot_magic;  /**< BMPX framing magic. */
    uint32_t send_max;       /**< Max bytes of one `send` stream. */
    uint32_t rx_max;         /**< Max bytes of one `receive` payload. */
    uint32_t pim_rx_max;     /**< Max bytes of one PIMX payload. */
    uint32_t shot_max;       /**< Max bytes of one screenshot image. */
    uint32_t inventory_max;  /**< Max host-requested inventory entries. */
    uint32_t xfer_chunk;     /**< Bytes per serial transfer chunk. */
} sync_capabilities_t;

/**
 * Fill @p out with this build's static P4Sync capabilities.
 *
 * Pure: no storage, serial, display or lock access; safe to call from any
 * context (including a host-side test that links only the config macros).
 * @param out Destination; ignored when NULL.
 */
void sync_query_capabilities(sync_capabilities_t *out);

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
