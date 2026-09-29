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

/* Protocol tag the host client asserts before any bulk transfer. Bumping it
 * is a breaking change to the host contract and must be recorded in the
 * changelog alongside the firmware tag. */
#define SYNC_PROTO_TAG "p4sync-1"

static void shell_command_sync_usage(void)
{
    shell_print_usage("Usage: sync [status]");
}

static void sync_print_limits(void)
{
    shell_print_field("sync.send_magic:", "%s", P4_CONFIG_SERIAL_SEND_MAGIC);
    shell_print_field("sync.pim_magic:", "%s", P4_CONFIG_PIM_SYNC_MAGIC);
    shell_print_field("sync.shot_magic:", "%s", P4_CONFIG_SCREENSHOT_BMP_MAGIC);
    shell_print_field_num("sync.send_max:", (long)P4_CONFIG_SERIAL_SEND_MAX_BYTES);
    shell_print_field_num("sync.rx_max:", (long)P4_CONFIG_SERIAL_RX_MAX_BYTES);
    shell_print_field_num("sync.pim_rx_max:", (long)P4_CONFIG_PIM_RX_MAX_BYTES);
    shell_print_field_num("sync.inventory_max:", (long)P4_CONFIG_SYNC_INVENTORY_MAX);
}

static void sync_print_state(void)
{
    bool mounted = storage_sd_is_mounted();
    bool unlocked = security_can_reveal_private();

    shell_print_field("sync.proto:", "%s", SYNC_PROTO_TAG);
    shell_print_field("sync.board:", "%s", BOARD_CFG_ID);
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
        sync_print_state();
        sync_print_limits();
        shell_print_ok("sync: ready (%s)", SYNC_PROTO_TAG);
        return 0;
    }
    shell_command_sync_usage();
    return 2;
}
