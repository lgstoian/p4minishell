/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_fam.c
 * @brief SD and disk command families.
 *
 * Moved verbatim out of storage_commands.c in v0.35.6. Declared in
 * storage_commands.h; the single dispatcher in components/command/
 * calls these, it never implements them.
 */

#include "storage_commands.h"
#include "storage.h"
#include "shell.h"
#include "strutil.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "board_bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static void shell_sd_print_usage(void);
static void shell_command_sd_info(void);
static void shell_command_sd_ls(char *command);
static void shell_command_sd_stat(char *command);
static void shell_command_sd_cat(char *command);

/* ========================================================================
 * SD COMMAND FAMILY
 * ======================================================================== */

static void shell_sd_print_usage(void)
{
    shell_print_usage("Usage:");
    shell_transcript_append_text("  sd info\n");
    shell_transcript_append_text("  sd ls [path]\n");
    shell_transcript_append_text("  sd stat <path>\n");
    shell_transcript_append_text("  sd cat <path> [max_bytes]\n");
    shell_transcript_append_text("  sd mount          (mount / re-mount after eject)\n");
    shell_transcript_append_text("  sd eject          (safe unmount before card removal)\n");
    shell_transcript_append_text("  sdeject           (alias for sd eject)\n");
    shell_transcript_append_text("Paths: sd:/file.txt, /sdcard/file.txt, or relative-to-sd-root\n");
}

static void shell_command_sd_info(void)
{
    shell_sd_session_t session;
    esp_err_t error;
    struct stat root_stat;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD info mount failed");
        return;
    }

    shell_print_field("sd.mount_point:", "%s", BSP_SD_MOUNT_POINT);
    if (P4_BSP_SDCARD != NULL) {
        char capacity_text[32];
        uint64_t capacity_bytes = (uint64_t)P4_BSP_SDCARD->csd.capacity * (uint64_t)P4_BSP_SDCARD->csd.sector_size;

        shell_sd_format_size(capacity_bytes, capacity_text, sizeof(capacity_text));
        shell_print_field("sd.card_name:", "%s", P4_BSP_SDCARD->cid.name);
        shell_print_field_num("sd.sector_size:", (long)P4_BSP_SDCARD->csd.sector_size);
        shell_print_field("sd.capacity:", "%s", capacity_text);
        shell_print_field_num("sd.max_freq_khz:", (long)P4_BSP_SDCARD->max_freq_khz);
    } else {
        shell_print_muted("sd.card_name: unavailable");
    }

    error = shell_sd_stat_path(BSP_SD_MOUNT_POINT, &root_stat);
    if (error == ESP_OK) {
        shell_print_field("sd.root:", "%s", shell_sd_entry_type(&root_stat));
    } else {
        shell_transcript_appendf("sd.root: unavailable (%s)\n", esp_err_to_name(error));
    }

    /* Filesystem capacity, which is what users actually want before a copy.
     * The card capacity printed above is the raw device size. Always print
     * the fs lines (unavailable on persistent failure) so callers waiting on
     * this output never hang on a missing line. */
    {
        storage_space_info_t space;

        if (storage_get_space_info(&space) == ESP_OK) {
            char text[24];

            shell_sd_format_size(space.total_bytes, text, sizeof(text));
            shell_print_field("sd.fs_total:", "%s", text);
            shell_sd_format_size(space.used_bytes, text, sizeof(text));
            shell_print_field("sd.fs_used:", "%s", text);
            shell_sd_format_size(space.free_bytes, text, sizeof(text));
            shell_print_field("sd.fs_free:", "%s", text);
            shell_print_field_num("sd.cluster_bytes:", (long)space.cluster_bytes);
        } else {
            shell_print_muted("sd.fs_total: unavailable");
            shell_print_muted("sd.fs_used: unavailable");
            shell_print_muted("sd.fs_free: unavailable");
            shell_print_muted("sd.cluster_bytes: unavailable");
        }
    }

    shell_sd_end(&session, "sd info");
}

static void shell_command_sd_ls(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    const char *input_path = argc >= 3 ? argv[2] : BSP_SD_MOUNT_POINT;
    char normalized_path[SHELL_SD_PATH_BYTES];
    char fatfs_path[SHELL_SD_PATH_BYTES];
    char lfn[SHELL_LFN_BYTES];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    shell_sd_session_t session;
    bool dir_open = false;

    if (argc > 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd ls command");
        return;
    }

    error = shell_sd_resolve_path(input_path, normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd ls");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD card not present or mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %s type=%s size=%s\n",
                                 normalized_path,
                                 shell_sd_entry_type(&path_stat),
                                 size_text);
        goto cleanup;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_print_error("sd: invalid FAT path for %s", normalized_path);
        shell_record_errorf("sd", error, "Could not convert directory path %s to FatFs path", normalized_path);
        goto cleanup;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        error = shell_sd_fresult_to_esp_err(result);
        shell_print_error("sd: could not open %s (FatFs=%u)",
                                 normalized_path,
                                 (unsigned int)result);
        shell_record_errorf("sd", error, "Could not open directory %s (FatFs=%u)", normalized_path, (unsigned int)result);
        goto cleanup;
    }
    dir_open = true;

    shell_transcript_appendf("sd: listing %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            shell_transcript_appendf("sd: directory read failed for %s (FatFs=%u)\n",
                                     normalized_path,
                                     (unsigned int)result);
            shell_record_errorf("sd", error, "Directory read failed for %s (FatFs=%u)", normalized_path, (unsigned int)result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("sd: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("sd", "Truncated directory listing for %s", normalized_path);
            break;
        }

        shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
        shell_transcript_appendf("sd: %-4s %s (%s)\n",
                                 (entry_info.fattrib & AM_DIR) ? "DIR" : "FILE",
                                 lfn,
                                 (entry_info.fattrib & AM_DIR) ? "-" : size_text);
        listed_entries++;
    }

cleanup:
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "sd ls");
}

static void shell_command_sd_stat(char *command)
{
    char *argv[4];
    int argc = shell_split_args(command, argv, 4);
    char normalized_path[SHELL_SD_PATH_BYTES];
    char size_text[32];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;

    if (argc != 3) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd stat command");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd stat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD stat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        shell_sd_end(&session, "sd stat");
        return;
    }

    shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
    shell_print_field("sd.path:", "%s", normalized_path);
    shell_print_field("sd.type:", "%s", shell_sd_entry_type(&path_stat));
    shell_print_field("sd.size:", "%s", size_text);
    shell_transcript_appendf("sd.mode: 0%o\n", (unsigned int)(path_stat.st_mode & 0777));

    shell_sd_end(&session, "sd stat");
}

static void shell_command_sd_cat(char *command)
{
    char *argv[5];
    int argc = shell_split_args(command, argv, 5);
    char normalized_path[SHELL_SD_PATH_BYTES];
    struct stat path_stat;
    esp_err_t error;
    shell_sd_session_t session;
    FILE *file = NULL;
    size_t max_bytes = SHELL_SD_CAT_DEFAULT_BYTES;
    size_t displayed_bytes = 0;
    bool truncated = false;
    bool ended_with_newline = false;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    if (argc < 3 || argc > 4) {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Usage error for sd cat command");
        return;
    }

    if (argc == 4 && !shell_parse_size_arg(argv[3], 1, SHELL_SD_CAT_MAX_BYTES, &max_bytes)) {
        shell_transcript_appendf("sd: max_bytes must be between 1 and %u\n", (unsigned int)SHELL_SD_CAT_MAX_BYTES);
        shell_record_warningf("sd", "Rejected invalid max_bytes for sd cat");
        return;
    }

    error = shell_sd_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        shell_transcript_append_text("sd: path is too long or invalid\n");
        shell_record_warningf("sd", "Rejected invalid path for sd cat");
        return;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        shell_print_error("sd: SD card not present - insert and retry");
        shell_record_errorf("sd", error, "SD cat mount failed");
        return;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_transcript_appendf("sd: path not found %s\n", normalized_path);
        shell_record_errorf("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_transcript_appendf("sd: %s is not a regular file\n", normalized_path);
        shell_record_warningf("sd", "Rejected non-file path for sd cat: %s", normalized_path);
        goto cleanup;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_print_error("sd: could not open %s (%s)", normalized_path, strerror(errno));
        shell_record_errorf("sd", ESP_FAIL, "Could not open file %s", normalized_path);
        goto cleanup;
    }

    shell_transcript_appendf("sd: preview %s (%u bytes max)\n", normalized_path, (unsigned int)max_bytes);
    while (displayed_bytes < max_bytes) {
        size_t index;
        size_t bytes_to_read = MIN(sizeof(buffer) - 1, max_bytes - displayed_bytes);
        size_t bytes_read = fread(buffer, 1, bytes_to_read, file);

        if (bytes_read == 0) {
            break;
        }

        for (index = 0; index < bytes_read; index++) {
            if (buffer[index] == '\n' || buffer[index] == '\r' || buffer[index] == '\t') {
                continue;
            }
            if (!isprint(buffer[index])) {
                buffer[index] = '.';
            }
        }
        buffer[bytes_read] = '\0';
        ended_with_newline = bytes_read > 0 && buffer[bytes_read - 1] == '\n';

        shell_transcript_appendf("%s", (char *)buffer);
        displayed_bytes += bytes_read;
        if (bytes_read < bytes_to_read) {
            break;
        }
    }

    if (displayed_bytes == 0) {
        shell_transcript_append_text("sd: file is empty\n");
    } else if (!feof(file)) {
        truncated = true;
    }

    if (displayed_bytes > 0 && !ended_with_newline) {
        shell_transcript_append_text("\n");
    }

    if (truncated) {
        shell_transcript_appendf("sd: preview truncated at %u bytes\n", (unsigned int)max_bytes);
    }

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    shell_sd_end(&session, "sd cat");
}

/**
 * `sd` command family entry point.
 *
 * Grouping the storage operations behind one verb lets them share validated
 * argument parsing, bounded output, and a consistent cleanup path. Receives
 * the original unsplit command text because the subcommands re-tokenize it.
 */
void shell_command_sd(char *command)
{
    char *argv[5];
    int argc;
    char *cmd_copy = (command != NULL) ? strdup(command) : NULL;

    /* The sub-handlers below re-tokenize the preserved copy; a NULL copy
     * must never reach them. */
    if (command != NULL && cmd_copy == NULL) {
        shell_print_error("sd: out of memory");
        shell_record_errorf("sd", ESP_ERR_NO_MEM, "Out of memory copying sd command");
        return;
    }

    /* shell_split_args() writes token terminators into the buffer in place,
     * so `command` would be truncated to just "sd" for the sub-handlers.
     * Hand them a preserved heap copy instead. */
    argc = shell_split_args(command, argv, 5);

    if (argc <= 1) {
        shell_command_sd_info();
        shell_sd_print_usage();
    } else if (strcmp(argv[1], "help") == 0) {
        shell_sd_print_usage();
    } else if (strcmp(argv[1], "info") == 0) {
        if (argc != 2) {
            shell_sd_print_usage();
            shell_record_warningf("sd", "Usage error for sd info command");
        } else {
            shell_command_sd_info();
        }
    } else if (strcmp(argv[1], "ls") == 0) {
        shell_command_sd_ls(cmd_copy);
    } else if (strcmp(argv[1], "stat") == 0) {
        shell_command_sd_stat(cmd_copy);
    } else if (strcmp(argv[1], "cat") == 0) {
        shell_command_sd_cat(cmd_copy);
    } else if (strcmp(argv[1], "mount") == 0) {
        shell_command_sd_mount();
    } else if (strcmp(argv[1], "eject") == 0) {
        shell_command_sd_eject();
    } else {
        shell_sd_print_usage();
        shell_record_warningf("sd", "Unknown sd subcommand: %s", argv[1]);
    }

    free(cmd_copy);
}

/* ========================================================================
 * DISK COMMAND FAMILY (diskpart-style disk and partition management)
 * ========================================================================
 * Physical-disk operations on the SD card: geometry, MBR partition-table
 * inspection and editing, and a diskpart-style format alias. The storage
 * module owns the low-level mechanics; these handlers own parsing and the
 * destructive-confirmation contract. Raw disk access stays SD-only by design
 * (USB sticks are addressed as files, not disks); the storage_volume_t
 * plumbing is shared with the volume-aware file verbs.
 */

static void shell_disk_print_usage(void)
{
    shell_print_usage("Usage:");
    shell_transcript_append_text("  disk list                     show the physical disk(s)\n");
    shell_transcript_append_text("  disk detail                   show disk geometry and the MBR partition table\n");
    shell_transcript_append_text("  disk clean                    remove the partition table (destructive)\n");
    shell_transcript_append_text("  disk create partition primary [size=N]  create a primary partition (N in MB)\n");
    shell_transcript_append_text("  disk delete partition N       delete MBR partition N (1-4)\n");
    shell_transcript_append_text("  disk active N|none            mark MBR partition N bootable (destructive)\n");
    shell_transcript_append_text("  disk type N <hex>             set the MBR partition type byte (destructive)\n");
    shell_transcript_append_text("  disk mbr [save <file>|restore <file>]   hexdump (or back up / restore) sector 0\n");
    shell_transcript_append_text("  disk verify [count=N]         read-scan the surface for bad sectors\n");
    shell_transcript_append_text("  disk geometry                 show C/H/S translation and alignment\n");
    shell_transcript_append_text("  disk image save|restore <file> [sectors=N]  raw sector backup/restore\n");
    shell_transcript_append_text("  disk format [fs=FAT32] [label=X] [au=size] [quick]   format the volume (diskpart style)\n");
    shell_transcript_append_text("After clean or create, run 'format' (or 'disk format') to create the filesystem.\n");
}

/** Friendly name for an MBR partition-type byte. */
static const char *shell_disk_partition_type_name(uint8_t type)
{
    switch (type) {
    case 0x01:
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT12/FAT16";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x05:
    case 0x0F:
        return "Extended";
    case 0x07:
        return "NTFS/exFAT";
    default:
        return "Unknown";
    }
}

static void shell_disk_report_geometry(const storage_disk_info_t *info)
{
    char capacity_text[32];

    shell_sd_format_size(info->capacity_bytes, capacity_text, sizeof(capacity_text));
    shell_print_field("disk.number:", "%u", 0);
    shell_print_field("disk.name:", "%s", info->card_name);
    shell_print_field("disk.capacity:", "%s", capacity_text);
    shell_print_field_num("disk.sector_size:", (long)info->sector_size);
    shell_print_field_num("disk.sector_count:", (long)info->sector_count);
    shell_print_field_num("disk.max_freq_khz:", (long)info->max_freq_khz);
}

static void shell_command_disk_list(void)
{
    storage_disk_info_t info;
    esp_err_t error;
    int slot;

    error = storage_disk_get_info(STORAGE_VOLUME_SD, &info);
    if (error != ESP_OK) {
        shell_print_error("disk: no SD card available (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk list could not read the card");
        return;
    }

    shell_print_heading("Physical Disks");
    shell_transcript_append_text("  [0] SD card:\n");
    shell_disk_report_geometry(&info);

    /* USB MSC slots share the listing; partition tools stay SD-only. */
    for (slot = 0; slot < P4_CONFIG_USB_MSC_MAX_DEVICES; slot++) {
        uint32_t sector_size = 0;
        uint32_t sector_count = 0;
        char label[64];
        char capacity_text[32];
        char drive[8];

        if (!storage_usb_volume_geometry(slot, &sector_size, &sector_count,
                                           label, sizeof(label))) {
            continue;
        }
        shell_sd_format_size((uint64_t)sector_size * (uint64_t)sector_count,
                             capacity_text, sizeof(capacity_text));
        snprintf(drive, sizeof(drive), "%c:",
                 storage_usb_drive_letter(slot) != '\0' ?
                 (char)toupper((unsigned char)storage_usb_drive_letter(slot)) : '?');
        shell_transcript_appendf_ansi("  " SH_NUM "[%d]" SH_RST " USB %s (%s, %s):\n",
                                      slot + 1, drive, label[0] != '\0' ? label : "disk",
                                      capacity_text);
        shell_print_field_num("disk.sector_size:", (long)sector_size);
        shell_print_field_num("disk.sector_count:", (long)sector_count);
    }
}

static void shell_command_disk_detail(void)
{
    storage_disk_info_t info;
    storage_mbr_t mbr;
    esp_err_t error;
    int index;

    error = storage_disk_get_info(STORAGE_VOLUME_SD, &info);
    if (error != ESP_OK) {
        shell_print_error("disk: no SD card available (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk detail could not read the card");
        return;
    }

    shell_print_heading("Disk 0 - %s", info.card_name);
    shell_disk_report_geometry(&info);

    error = storage_disk_read_mbr(STORAGE_VOLUME_SD, &mbr);
    if (error != ESP_OK) {
        shell_print_error("disk: could not read the partition table (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk detail MBR read failed");
        return;
    }

    shell_print_heading("MBR Partition Table");
    if (!mbr.valid) {
        shell_print_warning("disk: no valid MBR partition table (0x55AA signature missing)");
        shell_transcript_append_text("  Run 'disk create partition primary' then 'format' to create one.\n");
        return;
    }

    for (index = 0; index < 4; index++) {
        const storage_partition_t *part = &mbr.partitions[index];
        char size_text[24];

        if (part->type == 0 && part->size_lba == 0) {
            shell_transcript_appendf_ansi(SH_MUTE "  Partition %d: <unused>" SH_RST "\n", index + 1);
            continue;
        }

        shell_sd_format_size((uint64_t)part->size_lba * (uint64_t)info.sector_size,
                             size_text, sizeof(size_text));
        shell_transcript_appendf_ansi("  " SH_LBL "Partition %d:" SH_RST
                                      " " SH_USAGE "type=0x%02X %s" SH_RST
                                      " " SH_LBL "start" SH_RST "=" SH_NUM "%" PRIu32 SH_RST
                                      " " SH_LBL "size" SH_RST "=" SH_NUM "%s" SH_RST "%s\n",
                                      index + 1,
                                      part->type, shell_disk_partition_type_name(part->type),
                                      part->start_lba, size_text,
                                      part->bootable ? " (boot)" : "");
    }
}

static int shell_command_disk_clean(void)
{
    esp_err_t error;

    if (!shell_confirm_destructive("disk clean",
                                   "WARNING: This will remove ALL partitions on the SD card.",
                                   "  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n")) {
        shell_print_warning("disk: clean cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk clean by user");
        return 1;
    }

    error = storage_disk_clean(STORAGE_VOLUME_SD);
    if (error != ESP_OK) {
        shell_print_error("disk: clean failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk clean failed");
        return 1;
    }

    shell_print_ok("disk: partition table removed");
    shell_transcript_append_text("  Run 'disk create partition primary' then 'format' to recreate a filesystem.\n");
    return 0;
}

/**
 * Parse a partition-size token. diskpart uses megabytes by default; an
 * optional K/M/G suffix overrides that. @p bytes_out receives bytes.
 */
static bool shell_disk_parse_partition_size(const char *text, uint64_t *bytes_out)
{
    char *end;
    unsigned long value;
    uint64_t multiplier = 1024ULL * 1024ULL;

    if (text == NULL || bytes_out == NULL || *text == '\0') {
        return false;
    }

    value = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    if (*end == 'k' || *end == 'K') {
        multiplier = 1024ULL;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1024ULL * 1024ULL;
        end++;
    } else if (*end == 'g' || *end == 'G') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        end++;
    }

    if (*end != '\0' || value == 0) {
        return false;
    }

    *bytes_out = (uint64_t)value * multiplier;
    return true;
}

static int shell_command_disk_create(int argc, char **argv)
{
    uint64_t size_bytes = 0;
    esp_err_t error;
    int index;

    /* disk create partition primary [size=N] */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk create command");
        return 2;
    }
    if (strcmp(argv[3], "primary") != 0) {
        shell_print_error("disk: only 'primary' partitions are supported");
        shell_record_warningf("disk", "Unsupported partition type: %s", argv[3]);
        return 2;
    }

    for (index = 4; index < argc; index++) {
        if (strncmp(argv[index], "size=", 5) == 0) {
            if (!shell_disk_parse_partition_size(argv[index] + 5, &size_bytes)) {
                shell_print_error("disk: invalid partition size %s", argv[index] + 5);
                return 2;
            }
        } else {
            shell_print_error("disk: unexpected argument %s", argv[index]);
            shell_disk_print_usage();
            return 2;
        }
    }

    error = storage_disk_create_primary_partition(STORAGE_VOLUME_SD, size_bytes);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("disk: no free partition slot (all 4 MBR entries are used)");
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: create partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk create partition failed");
        return 1;
    }

    shell_print_ok("disk: primary partition created");
    shell_transcript_append_text("  Run 'format' (or 'disk format') to create the filesystem.\n");
    return 0;
}

static int shell_command_disk_delete(int argc, char **argv)
{
    char *end;
    unsigned long partition_index;
    esp_err_t error;

    /* disk delete partition N (1-4) */
    if (argc < 4 || strcmp(argv[2], "partition") != 0) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk delete command");
        return 2;
    }

    partition_index = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' || partition_index < 1 || partition_index > 4) {
        shell_print_error("disk: partition number must be 1-4");
        return 2;
    }

    if (!shell_confirm_destructive("disk delete",
                                   "WARNING: This will remove partition information from the SD card.",
                                   "")) {
        shell_print_warning("disk: delete cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk delete by user");
        return 1;
    }

    error = storage_disk_delete_partition(STORAGE_VOLUME_SD, (unsigned)(partition_index - 1));
    if (error != ESP_OK) {
        shell_print_error("disk: delete partition failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk delete partition failed");
        return 1;
    }

    shell_print_ok("disk: partition %lu removed", partition_index);
    shell_transcript_append_text("  Run 'format' (or 'disk format') to recreate a filesystem.\n");
    return 0;
}

static int shell_command_disk_format(int argc, char **argv)
{
    const char *fs_type = NULL;
    const char *label = NULL;
    uint32_t alloc_unit = P4_CONFIG_FORMAT_ALLOC_UNIT_BYTES;
    bool alloc_set = false;
    int index;

    /* disk format [fs=FAT32] [label=X] [au=size] [quick] */
    for (index = 2; index < argc; index++) {
        const char *token = argv[index];

        if (strncmp(token, "fs=", 3) == 0) {
            fs_type = token + 3;
        } else if (strncmp(token, "label=", 6) == 0) {
            label = token + 6;
            if (strlen(label) > 11) {
                shell_print_error("disk format: volume label must be 11 characters or fewer");
                return 2;
            }
        } else if (strncmp(token, "au=", 3) == 0) {
            if (!shell_parse_alloc_unit(token + 3, &alloc_unit) ||
                alloc_unit < P4_CONFIG_FORMAT_ALLOC_UNIT_MIN ||
                alloc_unit > P4_CONFIG_FORMAT_ALLOC_UNIT_MAX) {
                shell_print_error("disk format: invalid allocation unit size %s", token + 3);
                return 2;
            }
            alloc_set = true;
        } else if (strcmp(token, "quick") == 0) {
            /* Accepted for diskpart familiarity; the format is always quick. */
        } else {
            shell_print_error("disk format: unexpected argument %s", token);
            shell_disk_print_usage();
            return 2;
        }
    }

    /* Same filesystem validation as FORMAT.COM. */
    if (fs_type != NULL) {
        if (shell_text_equals_ignore_case(fs_type, "EXFAT")) {
            shell_print_warning("disk format: exFAT is not supported in this firmware build");
            shell_transcript_append_text("  Formatting as FAT32 instead.\n");
        } else if (!shell_text_equals_ignore_case(fs_type, "FAT") &&
                   !shell_text_equals_ignore_case(fs_type, "FAT32")) {
            shell_print_error("disk format: unsupported filesystem type %s", fs_type);
            shell_transcript_append_text("  Supported: FAT, FAT32\n");
            return 2;
        }
    }

    return shell_format_execute("disk format", fs_type, label, alloc_unit, alloc_set);
}

/** Parse a 1-based MBR partition number (1-4) or report usage. */
static bool shell_disk_parse_partition_number(const char *text, int *index_1based_out)
{
    char *end;
    long value;

    if (text == NULL || index_1based_out == NULL) {
        return false;
    }
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 4) {
        shell_print_error("disk: partition number must be 1-4");
        return false;
    }
    *index_1based_out = (int)value;
    return true;
}

/** Parse a hex byte token (00-FF, optional 0x prefix) for `disk type`. */
static bool shell_disk_parse_hex_byte(const char *text, uint8_t *byte_out)
{
    char *end;
    unsigned long value;

    if (text == NULL || byte_out == NULL) {
        return false;
    }
    value = strtoul(text, &end, 16);
    if (end == text || *end != '\0' || value > 0xFF) {
        shell_print_error("disk: type must be a hex byte 00-FF");
        return false;
    }
    *byte_out = (uint8_t)value;
    return true;
}

/** Print one 16-byte hexdump row through the shared strutil formatter. */
static void shell_disk_print_hex_row(uint32_t base, const uint8_t *data, size_t len)
{
    char row[80];

    if (strutil_format_hex_row(row, sizeof(row), base, data, len) >= 0) {
        shell_transcript_append_text(row);
    }
}

/** Progress printer shared by `disk verify` and `disk image`. */
static void shell_disk_scan_progress(uint32_t done_sectors, uint32_t total_sectors)
{
    unsigned percent;

    if (total_sectors == 0) {
        return;
    }
    percent = (unsigned)((uint64_t)done_sectors * 100ULL / (uint64_t)total_sectors);
    shell_transcript_appendf_ansi(SH_MUTE "  disk: %u%% (%lu/%lu sectors)\n" SH_RST,
                                  percent, (unsigned long)done_sectors,
                                  (unsigned long)total_sectors);
}

static int shell_command_disk_active(int argc, char **argv)
{
    int number;
    esp_err_t error;

    /* disk active N|none */
    if (argc != 3) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk active command");
        return 2;
    }
    if (shell_text_equals_ignore_case(argv[2], "none")) {
        number = 0;
    } else if (!shell_disk_parse_partition_number(argv[2], &number)) {
        return 2;
    }

    if (!shell_confirm_destructive("disk active",
                                   "WARNING: This will rewrite the MBR boot flags on the SD card.",
                                   "")) {
        shell_print_warning("disk: active cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk active by user");
        return 1;
    }

    error = storage_disk_set_active(STORAGE_VOLUME_SD, number);
    if (error == ESP_ERR_INVALID_STATE) {
        shell_print_error("disk: no valid MBR partition table (run 'disk create partition primary' first)");
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: active failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk active failed");
        return 1;
    }

    if (number == 0) {
        shell_print_ok("disk: all boot flags cleared");
    } else {
        shell_print_ok("disk: partition %d marked bootable", number);
    }
    return 0;
}

static int shell_command_disk_type(int argc, char **argv)
{
    int number;
    uint8_t type;
    esp_err_t error;

    /* disk type N <hex> */
    if (argc != 4) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk type command");
        return 2;
    }
    if (!shell_disk_parse_partition_number(argv[2], &number)) {
        return 2;
    }
    if (!shell_disk_parse_hex_byte(argv[3], &type)) {
        return 2;
    }

    if (!shell_confirm_destructive("disk type",
                                   "WARNING: This will rewrite an MBR partition-type byte on the SD card.",
                                   "")) {
        shell_print_warning("disk: type cancelled, the card was not modified");
        shell_record_warningf("disk", "Cancelled disk type by user");
        return 1;
    }

    error = storage_disk_set_partition_type(STORAGE_VOLUME_SD, (unsigned)(number - 1), type);
    if (error != ESP_OK) {
        shell_print_error("disk: type failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk type failed");
        return 1;
    }

    shell_print_ok("disk: partition %d type set to 0x%02X (%s)", number,
                   (unsigned int)type, shell_disk_partition_type_name(type));
    return 0;
}

static int shell_command_disk_mbr(int argc, char **argv)
{
    /* disk mbr [save <file> | restore <file>] */
    if (argc == 2) {
        uint8_t sector[512];
        esp_err_t error;
        unsigned row;

        error = storage_disk_read_sectors(STORAGE_VOLUME_SD, 0, sector, 1);
        if (error != ESP_OK) {
            shell_print_error("disk: MBR read failed (%s)", esp_err_to_name(error));
            shell_record_errorf("disk", error, "Disk MBR read failed");
            return 1;
        }
        shell_print_heading("Disk 0 MBR (sector 0)");
        for (row = 0; row < 512; row += 16) {
            shell_disk_print_hex_row(row, sector + row, 16);
        }
        return 0;
    }

    if (argc == 4 && shell_text_equals_ignore_case(argv[2], "save")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        uint8_t sector[512];
        FILE *out;
        size_t wrote;
        esp_err_t error;

        if (shell_fs_resolve_path(argv[3], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("disk mbr save: invalid path %s", argv[3]);
            return 2;
        }
        error = storage_disk_read_sectors(STORAGE_VOLUME_SD, 0, sector, 1);
        if (error != ESP_OK) {
            shell_print_error("disk: MBR read failed (%s)", esp_err_to_name(error));
            shell_record_errorf("disk", error, "Disk MBR save read failed");
            return 1;
        }
        if (!storage_check_free_space(sizeof(sector), sizeof(sector), "disk mbr save")) {
            shell_print_error("disk mbr save: not enough free space");
            return 1;
        }
        out = fopen(resolved, "wb");
        if (out == NULL) {
            shell_print_error("disk mbr save: cannot open %s", argv[3]);
            return 1;
        }
        wrote = fwrite(sector, 1, sizeof(sector), out);
        fclose(out);
        if (wrote != sizeof(sector)) {
            shell_print_error("disk mbr save: short write to %s", argv[3]);
            shell_record_errorf("disk", ESP_FAIL, "Disk MBR save short write");
            return 1;
        }
        shell_print_ok("disk: MBR saved to %s (512 bytes)", resolved);
        return 0;
    }

    if (argc == 4 && shell_text_equals_ignore_case(argv[2], "restore")) {
        char resolved[P4_CONFIG_SD_PATH_BYTES];
        FILE *in;
        uint8_t sector[512];
        size_t got;

        if (shell_fs_resolve_path(argv[3], resolved, sizeof(resolved)) != ESP_OK) {
            shell_print_error("disk mbr restore: invalid path %s", argv[3]);
            return 2;
        }
        in = fopen(resolved, "rb");
        if (in == NULL) {
            shell_print_error("disk mbr restore: cannot open %s", argv[3]);
            return 1;
        }
        got = fread(sector, 1, sizeof(sector), in);
        fclose(in);
        if (got != sizeof(sector)) {
            shell_print_error("disk mbr restore: %s is not a 512-byte MBR image", argv[3]);
            return 1;
        }
        if (sector[510] != 0x55 || sector[511] != 0xAA) {
            shell_print_error("disk mbr restore: %s lacks the 0x55AA signature", argv[3]);
            return 1;
        }
        if (!shell_confirm_destructive("disk mbr restore",
                                       "WARNING: This will overwrite sector 0 (the partition table) on the SD card.",
                                       "")) {
            shell_print_warning("disk: MBR restore cancelled, the card was not modified");
            shell_record_warningf("disk", "Cancelled disk MBR restore by user");
            return 1;
        }
        /* Reuse the guarded single-sector write primitive. */
        {
            esp_err_t error = storage_disk_write_sectors(STORAGE_VOLUME_SD, 0, sector, 1);

            if (error != ESP_OK) {
                shell_print_error("disk mbr restore failed (%s)", esp_err_to_name(error));
                shell_record_errorf("disk", error, "Disk MBR restore failed");
                return 1;
            }
        }
        shell_print_ok("disk: MBR restored from %s", resolved);
        return 0;
    }

    shell_disk_print_usage();
    shell_record_warningf("disk", "Usage error for disk mbr command");
    return 2;
}

/** Parse an optional `count=N` / `sectors=N` token (decimal sector count). */
static bool shell_disk_parse_count_token(const char *token, const char *key, uint32_t *count_out)
{
    char *end;
    unsigned long value;
    size_t key_len;

    if (token == NULL || key == NULL || count_out == NULL) {
        return false;
    }
    key_len = strlen(key);
    if (strncmp(token, key, key_len) != 0 || token[key_len] != '=') {
        return false;
    }
    value = strtoul(token + key_len + 1, &end, 10);
    if (end == token + key_len + 1 || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return false;
    }
    *count_out = (uint32_t)value;
    return true;
}

static int shell_command_disk_verify(int argc, char **argv)
{
    uint32_t max_sectors = 0;
    uint32_t bad = 0;
    uint32_t done = 0;
    esp_err_t error;
    int index;

    /* disk verify [count=N] */
    for (index = 2; index < argc; index++) {
        if (!shell_disk_parse_count_token(argv[index], "count", &max_sectors)) {
            shell_print_error("disk verify: unexpected argument %s", argv[index]);
            shell_disk_print_usage();
            return 2;
        }
    }

    shell_transcript_append_text("  disk: verifying surface (Ctrl+C aborts)…\n");
    error = storage_disk_verify(STORAGE_VOLUME_SD, max_sectors, &bad, &done,
                                shell_disk_scan_progress);
    if (error == ESP_ERR_TIMEOUT) {
        shell_print_warning("disk: verify aborted after %lu sectors (%lu bad)",
                            (unsigned long)done, (unsigned long)bad);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: verify failed (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk verify failed");
        return 1;
    }

    if (bad == 0) {
        shell_print_ok("disk: verify clean (%lu sectors read)", (unsigned long)done);
    } else {
        shell_print_error("disk: verify found %lu bad sectors of %lu read",
                          (unsigned long)bad, (unsigned long)done);
        shell_record_errorf("disk", ESP_FAIL, "Disk verify found bad sectors");
    }
    return bad == 0 ? 0 : 1;
}

static int shell_command_disk_geometry(void)
{
    storage_disk_info_t info;
    esp_err_t error;
    uint64_t total_sectors;
    uint32_t heads = 255;
    uint32_t sectors_per_track = 63;
    uint32_t cylinders;

    error = storage_disk_get_info(STORAGE_VOLUME_SD, &info);
    if (error != ESP_OK) {
        shell_print_error("disk: no SD card available (%s)", esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk geometry could not read the card");
        return 1;
    }
    if (info.sector_size != 512 || info.sector_count == 0) {
        shell_print_error("disk: unsupported sector size %lu",
                          (unsigned long)info.sector_size);
        return 1;
    }

    /* Classic BIOS translation (255 heads, 63 sectors/track), the same
     * geometry FDISK reports for large disks. */
    total_sectors = (uint64_t)info.sector_count;
    cylinders = (uint32_t)(total_sectors / ((uint64_t)heads * sectors_per_track));

    shell_print_heading("Disk 0 Geometry");
    shell_print_field_num("disk.heads:", (long)heads);
    shell_print_field_num("disk.sectors_per_track:", (long)sectors_per_track);
    shell_print_field_num("disk.cylinders:", (long)cylinders);
    shell_print_field_num("disk.total_sectors:", (long)info.sector_count);
    shell_print_field_num("disk.sector_size:", (long)info.sector_size);
    shell_print_field_num("disk.align_sectors:", (long)P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS);
    return 0;
}

static int shell_command_disk_image(int argc, char **argv)
{
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    uint32_t sectors = 0;
    uint32_t done = 0;
    esp_err_t error;
    int index;
    bool save;

    /* disk image save|restore <file> [sectors=N] */
    if (argc < 4) {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk image command");
        return 2;
    }
    if (shell_text_equals_ignore_case(argv[2], "save")) {
        save = true;
    } else if (shell_text_equals_ignore_case(argv[2], "restore")) {
        save = false;
    } else {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Usage error for disk image command");
        return 2;
    }
    for (index = 4; index < argc; index++) {
        if (!shell_disk_parse_count_token(argv[index], "sectors", &sectors)) {
            shell_print_error("disk image: unexpected argument %s", argv[index]);
            shell_disk_print_usage();
            return 2;
        }
    }
    if (shell_fs_resolve_path(argv[3], resolved, sizeof(resolved)) != ESP_OK) {
        shell_print_error("disk image: invalid path %s", argv[3]);
        return 2;
    }

    if (!save) {
        if (!shell_confirm_destructive("disk image restore",
                                       "WARNING: This will overwrite the SD card from sector 0 with the image file.",
                                       "")) {
            shell_print_warning("disk: image restore cancelled, the card was not modified");
            shell_record_warningf("disk", "Cancelled disk image restore by user");
            return 1;
        }
        error = storage_disk_image_restore(STORAGE_VOLUME_SD, resolved, &done, shell_disk_scan_progress);
    } else {
        shell_transcript_append_text("  disk: reading surface (Ctrl+C aborts)…\n");
        error = storage_disk_image_save(STORAGE_VOLUME_SD, resolved, sectors, &done, shell_disk_scan_progress);
    }

    if (error == ESP_ERR_TIMEOUT) {
        shell_print_warning("disk: image %s aborted after %lu sectors",
                            save ? "save" : "restore", (unsigned long)done);
        return 1;
    }
    if (error != ESP_OK) {
        shell_print_error("disk: image %s failed (%s)", save ? "save" : "restore",
                          esp_err_to_name(error));
        shell_record_errorf("disk", error, "Disk image failed");
        return 1;
    }

    shell_print_ok("disk: image %s complete (%lu sectors)", save ? "saved" : "restored",
                   (unsigned long)done);
    return 0;
}

/**
 * `disk` command family entry point.
 */
int shell_command_disk(char *command)
{
    char *argv[8];
    int argc;
    int rc = 2;

    argc = shell_split_args(command, argv, 8);

    if (argc <= 1) {
        shell_command_disk_list();
        shell_disk_print_usage();
        rc = 2;
    } else if (strcmp(argv[1], "help") == 0) {
        shell_disk_print_usage();
        rc = 0;
    } else if (strcmp(argv[1], "list") == 0) {
        shell_command_disk_list();
        rc = 0;
    } else if (strcmp(argv[1], "detail") == 0) {
        shell_command_disk_detail();
        rc = 0;
    } else if (strcmp(argv[1], "clean") == 0) {
        rc = shell_command_disk_clean();
    } else if (strcmp(argv[1], "create") == 0) {
        rc = shell_command_disk_create(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        rc = shell_command_disk_delete(argc, argv);
    } else if (strcmp(argv[1], "active") == 0) {
        rc = shell_command_disk_active(argc, argv);
    } else if (strcmp(argv[1], "type") == 0) {
        rc = shell_command_disk_type(argc, argv);
    } else if (strcmp(argv[1], "mbr") == 0) {
        rc = shell_command_disk_mbr(argc, argv);
    } else if (strcmp(argv[1], "verify") == 0) {
        rc = shell_command_disk_verify(argc, argv);
    } else if (strcmp(argv[1], "geometry") == 0) {
        rc = shell_command_disk_geometry();
    } else if (strcmp(argv[1], "image") == 0) {
        rc = shell_command_disk_image(argc, argv);
    } else if (strcmp(argv[1], "format") == 0) {
        rc = shell_command_disk_format(argc, argv);
    } else {
        shell_disk_print_usage();
        shell_record_warningf("disk", "Unknown disk subcommand: %s", argv[1]);
        rc = 2;
    }

    return rc;
}
