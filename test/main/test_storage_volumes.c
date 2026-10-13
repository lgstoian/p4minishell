/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_storage_volumes.c
 * @brief Unit tests for the pure DOS-parity helpers: hexdump rows, DEBUG
 * number parsing, volume classification, and MBR decoding.
 *
 * Covers strutil_format_hex_row() (components/strutil/strutil.c),
 * shell_debug_parse_hex_addr()/shell_debug_parse_hex_byte() and the address
 * guards (components/shell/shell.c), storage_volume_of_vfs_path() and
 * storage_mbr_decode_sector() (components/storage/storage.c). All are pure
 * (no SD/USB access) so they are tested directly.
 */

#include "unity.h"
#include "strutil.h"
#include "shell.h"
#include "storage.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ========================================================================
 * HEXDUMP ROWS
 * ======================================================================== */

void test_hex_row_full(void)
{
    uint8_t data[STRUTIL_HEX_ROW_BYTES];
    char row[96];
    int index;
    int rc;

    for (index = 0; index < STRUTIL_HEX_ROW_BYTES; index++) {
        data[index] = (uint8_t)index;
    }
    rc = strutil_format_hex_row(row, sizeof(row), 0x12345678u, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(78, rc);
    TEST_ASSERT_EQUAL_STRING("12345678  "
                             "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  "
                             "|................|\n",
                             row);
}

void test_hex_row_short(void)
{
    const uint8_t data[] = { 0x41, 0x05 };
    char row[96];
    size_t tail;
    int rc;

    rc = strutil_format_hex_row(row, sizeof(row), 0x100u, data, sizeof(data));
    /* 10 offset + 48 hex + 2 " |" + 2 ascii + "|\n" = 64. */
    TEST_ASSERT_EQUAL_INT(64, rc);
    TEST_ASSERT_EQUAL_INT(64, strlen(row));
    TEST_ASSERT_TRUE(strncmp(row, "00000100  41 05 ", 16) == 0);
    /* The 42 padding columns between the data and " |" are all spaces. */
    for (tail = 16; tail < 58; tail++) {
        TEST_ASSERT_EQUAL_CHAR(' ', row[tail]);
    }
    TEST_ASSERT_EQUAL_STRING(" |A.|\n", row + 58);
}

void test_hex_row_rejects(void)
{
    const uint8_t data[] = { 0x41 };
    char row[96];

    TEST_ASSERT_EQUAL_INT(-1, strutil_format_hex_row(NULL, sizeof(row), 0, data, 1));
    TEST_ASSERT_EQUAL_INT(-1, strutil_format_hex_row(row, 0, 0, data, 1));
    TEST_ASSERT_EQUAL_INT(-1, strutil_format_hex_row(row, sizeof(row), 0, NULL, 1));
    TEST_ASSERT_EQUAL_INT(-1, strutil_format_hex_row(row, 10, 0, data, 1));
}

/* ========================================================================
 * DEBUG NUMBER PARSING
 * ======================================================================== */

void test_debug_parse_hex_addr(void)
{
    uint32_t addr = 0;

    TEST_ASSERT_TRUE(shell_debug_parse_hex_addr("0x4FF01000", &addr));
    TEST_ASSERT_EQUAL_UINT32(0x4FF01000u, addr);
    TEST_ASSERT_TRUE(shell_debug_parse_hex_addr("4FF01000", &addr));
    TEST_ASSERT_EQUAL_UINT32(0x4FF01000u, addr);
    TEST_ASSERT_TRUE(shell_debug_parse_hex_addr("0Xff", &addr));
    TEST_ASSERT_EQUAL_UINT32(0xFFu, addr);

    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("XYZ", &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("", &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("0x", &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr(NULL, &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("12", NULL));
    /* Out of range for 32 bits (9 digits would wrap strtoul on 32-bit long). */
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("100000000", &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("FFFFFFFFF", &addr));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_addr("0x100000000", &addr));
}

void test_debug_parse_hex_byte(void)
{
    uint8_t byte = 0;

    TEST_ASSERT_TRUE(shell_debug_parse_hex_byte("FF", &byte));
    TEST_ASSERT_EQUAL_UINT8(0xFF, byte);
    TEST_ASSERT_TRUE(shell_debug_parse_hex_byte("0x0a", &byte));
    TEST_ASSERT_EQUAL_UINT8(0x0A, byte);
    TEST_ASSERT_TRUE(shell_debug_parse_hex_byte("0", &byte));
    TEST_ASSERT_EQUAL_UINT8(0, byte);

    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte("1FF", &byte));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte("100", &byte));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte("GG", &byte));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte("", &byte));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte(NULL, &byte));
    TEST_ASSERT_FALSE(shell_debug_parse_hex_byte("12", NULL));
}

/** Probe cell in internal DRAM (.bss): always byte-accessible RAM. (Whether
 * it also reports executable is target-dependent, so writability is proven
 * against code addresses below instead.) */
static uint32_t s_debug_probe_cell;

void test_debug_addr_guards(void)
{
    uint32_t dram_addr = (uint32_t)(uintptr_t)&s_debug_probe_cell;
    uint32_t code_addr = (uint32_t)(uintptr_t)&test_debug_addr_guards;

    /* Empty ranges and 32-bit wraparound never validate (pure logic). */
    TEST_ASSERT_FALSE(shell_debug_addr_readable(dram_addr, 0));
    TEST_ASSERT_FALSE(shell_debug_addr_writable(dram_addr, 0));
    TEST_ASSERT_FALSE(shell_debug_addr_readable(UINT32_MAX, 2));
    TEST_ASSERT_FALSE(shell_debug_addr_writable(UINT32_MAX, 2));

    /* Internal DRAM is readable RAM. */
    TEST_ASSERT_TRUE(shell_debug_addr_readable(dram_addr, sizeof(s_debug_probe_cell)));

    /* Internal DRAM is poke-writable (the guard only excludes known code
     * windows: flash IROM, RTC fast memory). */
    TEST_ASSERT_TRUE(shell_debug_addr_writable(dram_addr, sizeof(s_debug_probe_cell)));

    /* Code addresses are never poke-writable (flash is a known code window;
     * flash is not byte-accessible RAM either, so readability is not
     * asserted). The YES gate and audit log cover the actual write path. */
    TEST_ASSERT_FALSE(shell_debug_addr_writable(code_addr, 1));
}

/* ========================================================================
 * VOLUME CLASSIFICATION (resolved VFS paths only)
 * ======================================================================== */

void test_volume_of_vfs_path(void)
{
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("/sdcard/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("/sdcard"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("sd:/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path(NULL));

    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_USB0, storage_volume_of_vfs_path("/usb0/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_USB1, storage_volume_of_vfs_path("/usb1"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_USB2, storage_volume_of_vfs_path("/usb2/dir/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_USB3, storage_volume_of_vfs_path("/usb3/"));

    /* Prefix lookalikes and out-of-range slots stay on SD. */
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("/usb0x/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("/usb9/file.txt"));
    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_SD, storage_volume_of_vfs_path("/usb/file.txt"));

    TEST_ASSERT_EQUAL_INT(STORAGE_VOLUME_USB0 + 0, STORAGE_VOLUME_USB0);
    TEST_ASSERT_EQUAL_INT('u', storage_usb_drive_letter(0));
    TEST_ASSERT_EQUAL_INT('x', storage_usb_drive_letter(3));
    TEST_ASSERT_EQUAL_INT(0, storage_usb_slot_for_drive('u'));
    TEST_ASSERT_EQUAL_INT(3, storage_usb_slot_for_drive('X'));
}

/* ========================================================================
 * MBR DECODING
 * ======================================================================== */

void test_mbr_decode_rejects(void)
{
    uint8_t sector[512];
    storage_mbr_t mbr;

    memset(sector, 0, sizeof(sector));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, storage_mbr_decode_sector(NULL, &mbr));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, storage_mbr_decode_sector(sector, NULL));

    /* Zero sector: no 0x55AA signature, valid=false. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_mbr_decode_sector(sector, &mbr));
    TEST_ASSERT_FALSE(mbr.valid);
}

void test_mbr_decode_entries(void)
{
    uint8_t sector[512];
    storage_mbr_t mbr;
    uint8_t *pte;

    memset(sector, 0, sizeof(sector));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    /* PTE0: bootable FAT32-LBA, start LBA 0x800, 0x10000 sectors. */
    pte = sector + 446;
    pte[0] = 0x80;
    pte[4] = 0x0C;
    pte[8] = 0x00;
    pte[9] = 0x08;
    pte[10] = 0x00;
    pte[11] = 0x00;
    pte[12] = 0x00;
    pte[13] = 0x00;
    pte[14] = 0x01;
    pte[15] = 0x00;

    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_mbr_decode_sector(sector, &mbr));
    TEST_ASSERT_TRUE(mbr.valid);
    TEST_ASSERT_TRUE(mbr.partitions[0].bootable);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr.partitions[0].type);
    TEST_ASSERT_EQUAL_UINT32(0x800u, mbr.partitions[0].start_lba);
    TEST_ASSERT_EQUAL_UINT32(0x10000u, mbr.partitions[0].size_lba);

    /* PTE1..3 are zeroed: unused, never bootable. */
    TEST_ASSERT_FALSE(mbr.partitions[1].bootable);
    TEST_ASSERT_EQUAL_UINT8(0, mbr.partitions[1].type);
    TEST_ASSERT_EQUAL_UINT32(0, mbr.partitions[1].start_lba);
    TEST_ASSERT_EQUAL_UINT32(0, mbr.partitions[1].size_lba);
}
