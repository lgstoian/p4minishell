// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
/**
 * @file sync_commands.c
 * @brief `sync` — read-only P4Sync handshake over the USB-serial console.
 *
 * Prints one capability snapshot the host greps for (`sync.*` lines):
 * protocol tag, board profile, SD/lock state, transfer magics, and byte
 * limits. Store contents are never enumerated here; the host composes
 * inventory from the existing `dir /b`, `db`, `alarm`, `pkg`, and `asset`
 * verbs so this file adds no second walker, parser, scanner, or CRC.
 *
 * The static half of the snapshot is also exposed as a pure query
 * (sync_query_capabilities) so the host protocol can read the same facts
 * without parsing prose or inventing a second handshake. Only lines are
 * added here; every pre-existing `sync.*` line keeps its name and value.
 *
 * Batch-friendly: ERRORLEVEL 0 printed, 2 usage.
 */
#include <string.h>

#include "board_config.h"
#include "command.h"
#include "p4minishell_config.h"
#include "security_commands.h"
#include "shell.h"
#include "storage.h"
#include "sync_commands.h"

/* Keep the firmware-facing short name; the tag itself lives in the header so
 * the pure query and the verb share one literal. */
#define SYNC_PROTO_TAG P4SYNC_PROTO_TAG

void sync_query_capabilities(sync_capabilities_t *out)
{
    if (out == NULL) {
        return;
    }
    out->proto_tag = P4SYNC_PROTO_TAG;
    out->board_id = BOARD_CFG_ID;
    out->send_magic = P4_CONFIG_SERIAL_SEND_MAGIC;
    out->pim_magic = P4_CONFIG_PIM_SYNC_MAGIC;
    out->shot_magic = P4_CONFIG_SCREENSHOT_BMP_MAGIC;
    out->send_max = (uint32_t)P4_CONFIG_SERIAL_SEND_MAX_BYTES;
    out->rx_max = (uint32_t)P4_CONFIG_SERIAL_RX_MAX_BYTES;
    out->pim_rx_max = (uint32_t)P4_CONFIG_PIM_RX_MAX_BYTES;
    /* The screenshot cap is the existing image-size guard used by the
     * screenshot -> view round-trip; no new size constant is introduced. */
    out->shot_max = (uint32_t)P4_CONFIG_IMAGE_MAX_BYTES;
    out->inventory_max = (uint32_t)P4_CONFIG_SYNC_INVENTORY_MAX;
    out->xfer_chunk = (uint32_t)P4_CONFIG_SERIAL_XFER_CHUNK_BYTES;
}

static void shell_command_sync_usage(void)
{
    shell_print_usage("Usage: sync [status]");
}

static void sync_print_limits(const sync_capabilities_t *caps)
{
    /* Pre-existing lines first and unchanged, so host baselines and the
     * conformance suite keep matching; the three additions are additive. */
    shell_print_field("sync.send_magic:", "%s", caps->send_magic);
    shell_print_field("sync.pim_magic:", "%s", caps->pim_magic);
    shell_print_field("sync.shot_magic:", "%s", caps->shot_magic);
    shell_print_field_num("sync.send_max:", (long)caps->send_max);
    shell_print_field_num("sync.rx_max:", (long)caps->rx_max);
    shell_print_field_num("sync.pim_rx_max:", (long)caps->pim_rx_max);
    shell_print_field_num("sync.inventory_max:", (long)caps->inventory_max);
    shell_print_field_num("sync.shot_max:", (long)caps->shot_max);
    shell_print_field_num("sync.xfer_chunk:", (long)caps->xfer_chunk);
}

static void sync_print_state(const sync_capabilities_t *caps)
{
    bool mounted = storage_sd_is_mounted();
    bool unlocked = security_can_reveal_private();

    shell_print_field("sync.proto:", "%s", caps->proto_tag);
    shell_print_field("sync.board:", "%s", caps->board_id);
    shell_print_field("sync.sd:", "%s", mounted ? "mounted" : "absent");
    shell_print_field("sync.lock:", "%s", unlocked ? "unlocked" : "locked");

    if (mounted) {
        storage_space_info_t info;
        memset(&info, 0, sizeof(info));
        if (storage_get_space_info(&info) == 0) {
            shell_print_field("sync.free_bytes:", "%llu", (unsigned long long)info.free_bytes);
            shell_print_field("sync.total_bytes:", "%llu", (unsigned long long)info.total_bytes);
        } else {
            shell_print_muted("sync.free_bytes: unavailable");
        }
    } else {
        shell_print_muted("sync.free_bytes: unavailable");
    }
}

int shell_command_sync(int argc, char **argv)
{
    if (argv == NULL || argc < 1) {
        shell_command_sync_usage();
        return 2;
    }
    if (argc == 1 || (argc == 2 && shell_text_equals_ignore_case(argv[1], "status"))) {
        sync_capabilities_t caps;

        sync_query_capabilities(&caps);
        sync_print_state(&caps);
        sync_print_limits(&caps);
        shell_print_ok("sync: ready (%s)", caps.proto_tag);
        return 0;
    }
    shell_command_sync_usage();
    return 2;
}
