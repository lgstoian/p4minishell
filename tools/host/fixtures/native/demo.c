/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file demo.c
 * @brief Tiny fixed-ABI payload source for make_native_payload.py fixtures.
 *
 * The export table lives in its own section so the host signer can place it
 * first. It is the full 16-byte header plus two offsets; entry offsets are
 * constants because exports are always first (8 + 4*2 = 16). No ELF metadata
 * is shipped — objcopy extracts only these raw bytes and .text.
 */

#include <stdint.h>

/* "P4EX" | version 1, count 2 | entry 0 @16 | entry 1 @20 (little-endian). */
const uint32_t p4ex_exports[4]
    __attribute__((section(".p4ex.exports"), used)) = {
        0x58453450u, /* bytes 50 34 45 58 = "P4EX" */
        0x00020001u, /* u16 version=1, u16 count=2 (little-endian) */
        16u,
        20u
    };

int p4ex_entry_main(void *api, int argc, char **argv)
{
    (void)api;
    (void)argc;
    (void)argv;
    return 0;
}
