/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file storage_path.c
 * @brief Hardware-free path resolution, the shell cwd and the USB slot map.
 *
 * Pure string surgery over the compile-time mount points: no VFS, no FATFS,
 * no SDMMC. Split out of storage.c so the batch environment core and the host
 * unit-test lane can resolve paths without esp_vfs_fat.h. The USB branch of
 * shell_fs_resolve_path() asks the registered mount hook to bring up an
 * attached-but-unmounted slot, exactly as it did when it lived in storage.c.
 *
 * @module storage
 * @layer storage
 * @domain fs
 * @provides cwd, path resolution, USB drive/slot mapping, USB ops registration
 * @status stable
 * @test test/main/test_calc.c
 * @hard-rule pure string/path helpers only - no VFS, FATFS or SD access here
 */

#include "storage_path.h"

#include "board_identity.h"
#include "p4minishell_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The board BSP spells the SD mount point BSP_SD_MOUNT_POINT; this file uses
 * the board identity header instead (which carries no driver includes). */
#define BSP_SD_MOUNT_POINT BOARD_CFG_SD_MOUNT_POINT
#define SHELL_SD_PATH_BYTES P4_CONFIG_SD_PATH_BYTES

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

/* Shell working directory (RAM-only, not persisted across boots). */
static char s_shell_cwd[SHELL_SD_PATH_BYTES];

/* USB MSC operations table (registered by main.c; NULL before that). USB
 * drive letters are DOS-conventional u:/v:/w:/x: for slots 0..3. */
static storage_usb_ops_t s_usb_ops;
static bool s_usb_ops_registered = false;

/* DOS drive letters for the USB MSC slots (slot order, lowercase match). */
static const char s_usb_drive_letters[] = "uvwx";

/* ========================================================================
 * USB MSC SLOT MAP
 * ======================================================================== */

void storage_register_usb_ops(const storage_usb_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_usb_ops, 0, sizeof(s_usb_ops));
        s_usb_ops_registered = false;
        return;
    }
    s_usb_ops = *ops;
    s_usb_ops_registered = true;
}

const storage_usb_ops_t *storage_usb_ops_get(void)
{
    return s_usb_ops_registered ? &s_usb_ops : NULL;
}

char storage_usb_drive_letter(int slot)
{
    if (slot < 0 || slot >= (int)(sizeof(s_usb_drive_letters) - 1)) {
        return '\0';
    }
    return s_usb_drive_letters[slot];
}

int storage_usb_slot_for_drive(char letter)
{
    const char *found;

    if (letter >= 'A' && letter <= 'Z') {
        letter = (char)(letter - 'A' + 'a');
    }
    found = strchr(s_usb_drive_letters, letter);
    if (found == NULL) {
        return -1;
    }
    return (int)(found - s_usb_drive_letters);
}

void storage_usb_slot_base(int slot, char *out, size_t out_size)
{
    const char *base = P4_CONFIG_USB_MSC_BASE_PATH;
    size_t len;

    if (out == NULL || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", base);
    if (slot <= 0) {
        return;
    }
    len = strlen(out);
    if (len > 0 && out[len - 1] >= '0' && out[len - 1] <= '9' &&
        slot >= 0 && slot <= 9) {
        out[len - 1] = (char)('0' + slot);
    } else if (slot >= 0 && slot <= 9) {
        snprintf(out, out_size, "%s%d", base, slot);
    }
}

/* ========================================================================
 * PATH RESOLUTION AND CONVERSION
 * ======================================================================== */

esp_err_t shell_sd_resolve_path(const char *input, char *output, size_t output_size)
{
    int written;
    const char *source = input;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source == NULL || source[0] == '\0') {
        source = BSP_SD_MOUNT_POINT;
    }

    if (strncmp(source, "sd:/", 4) == 0) {
        written = snprintf(output, output_size, "%s%s", BSP_SD_MOUNT_POINT, source + 3);
    } else if (strncmp(source, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0 ||
               strcmp(source, BSP_SD_MOUNT_POINT) == 0) {
        written = snprintf(output, output_size, "%s", source);
    } else if (source[0] == '/') {
        written = snprintf(output, output_size, "%s", source);
    } else {
        written = snprintf(output, output_size, "%s/%s", BSP_SD_MOUNT_POINT, source);
    }

    if (written < 0) {
        output[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t shell_fs_resolve_path(const char *input, char *output, size_t output_size)
{
    char combined[SHELL_SD_PATH_BYTES];
    char scratch[SHELL_SD_PATH_BYTES];
    char *segments[32];
    size_t segment_count = 0;
    char *token;
    char *context = NULL;
    const char *mount_base = BSP_SD_MOUNT_POINT;
    char usb_base[32];
    int written;
    size_t index;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (input == NULL || input[0] == '\0') {
        snprintf(output, output_size, "%s", s_shell_cwd[0] != '\0' ? s_shell_cwd : BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    /* USB volume prefixes resolve against the slot base instead of the SD
     * mount: `usb:/…` and `usbN:/…` (slot N), `u:`/`v:`/`w:`/`x:` drive
     * letters (optionally followed by `/…`). Paths stay absolute from the
     * USB root; there is no per-drive cwd. A connected-but-unmounted slot
     * is automounted idempotently so DOS verbs work without a prior
     * `usb ls`. */
    {
        int slot = -1;
        const char *rest = NULL;
        const storage_usb_ops_t *usb_ops = storage_usb_ops_get();

        if (strncmp(input, "usb:/", 5) == 0) {
            slot = 0;
            rest = input + 4;
        } else if ((input[0] == 'u' || input[0] == 'U' ||
                    input[0] == 'v' || input[0] == 'V' ||
                    input[0] == 'w' || input[0] == 'W' ||
                    input[0] == 'x' || input[0] == 'X') && input[1] == ':') {
            slot = storage_usb_slot_for_drive(input[0]);
            rest = input + 2;
        } else if (strncmp(input, "usb", 3) == 0 && input[3] >= '0' && input[3] <= '9' &&
                   input[4] == ':' && input[5] == '/') {
            slot = input[3] - '0';
            rest = input + 5;
        }
        if (slot >= 0 && slot < P4_CONFIG_USB_MSC_MAX_DEVICES && rest != NULL) {
            storage_usb_slot_base(slot, usb_base, sizeof(usb_base));
            mount_base = usb_base;
            snprintf(combined, sizeof(combined), "/%s", rest[0] == '/' ? rest + 1 : rest);
                if (usb_ops != NULL && usb_ops->is_connected != NULL &&
                    usb_ops->is_mounted != NULL && usb_ops->mount != NULL &&
                    usb_ops->is_connected(slot) && !usb_ops->is_mounted(slot)) {
                    usb_ops->mount(slot);
                }
                goto normalize;
        }
        /* Absolute /usbN paths (e.g. from completion) map the same way. */
        for (slot = 0; slot < P4_CONFIG_USB_MSC_MAX_DEVICES; slot++) {
            size_t len;

            storage_usb_slot_base(slot, usb_base, sizeof(usb_base));
            len = strlen(usb_base);
            if (strncmp(input, usb_base, len) == 0 &&
                (input[len] == '\0' || input[len] == '/')) {
                mount_base = usb_base;
                snprintf(combined, sizeof(combined), "/%s", input + len + (input[len] == '/' ? 1 : 0));
                if (usb_ops != NULL && usb_ops->is_connected != NULL &&
                    usb_ops->is_mounted != NULL && usb_ops->mount != NULL &&
                    usb_ops->is_connected(slot) && !usb_ops->is_mounted(slot)) {
                    usb_ops->mount(slot);
                }
                goto normalize;
            }
        }
    }

    if (strncmp(input, "sd:/", 4) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input + 4);
    } else if (strcmp(input, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/");
    } else if (strncmp(input, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        snprintf(combined, sizeof(combined), "%s", input + strlen(BSP_SD_MOUNT_POINT));
    } else if (input[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", input);
    } else if (s_shell_cwd[0] == '\0' || strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        /* Empty cwd means storage_init() has not run yet (host tests, early
         * boot): treat it as the mount root, exactly like an unset cwd. */
        snprintf(combined, sizeof(combined), "/%s", input);
    } else {
        snprintf(combined,
                 sizeof(combined),
                 "%s/%s",
                 s_shell_cwd + strlen(BSP_SD_MOUNT_POINT),
                 input);
    }

normalize:
    snprintf(scratch, sizeof(scratch), "%s", combined);
    token = strtok_r(scratch, "/\\", &context);
    while (token != NULL && segment_count < (sizeof(segments) / sizeof(segments[0]))) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        if (strcmp(token, "..") == 0) {
            if (segment_count > 0) {
                segment_count--;
            }
            token = strtok_r(NULL, "/\\", &context);
            continue;
        }

        segments[segment_count++] = token;
        token = strtok_r(NULL, "/\\", &context);
    }

    written = snprintf(output, output_size, "%s", mount_base);
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    for (index = 0; index < segment_count; index++) {
        size_t used = strlen(output);
        written = snprintf(output + used, output_size - used, "/%s", segments[index]);
        if (written < 0 || (size_t)written >= output_size - used) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

bool shell_path_has_directory_component(const char *path)
{
    return path != NULL && (strchr(path, '/') != NULL || strchr(path, '\\') != NULL);
}

bool shell_path_has_extension(const char *path, const char *extension)
{
    size_t path_len;
    size_t ext_len;
    size_t index;

    if (path == NULL || extension == NULL) {
        return false;
    }

    path_len = strlen(path);
    ext_len = strlen(extension);
    if (path_len < ext_len) {
        return false;
    }

    /* Case-insensitive compare without pulling a shell helper in: this file
     * must stay usable before the shell component is linked (host lane). */
    for (index = 0; index < ext_len; index++) {
        if (tolower((unsigned char)path[path_len - ext_len + index]) !=
            tolower((unsigned char)extension[index])) {
            return false;
        }
    }
    return true;
}

esp_err_t shell_resolve_target_from_source(const char *source_path,
                                           const char *target_input,
                                           char *target_path,
                                           size_t target_path_size)
{
    char source_dir[SHELL_SD_PATH_BYTES];
    char *last_sep;

    if (source_path == NULL || target_input == NULL || target_path == NULL || target_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (shell_path_has_directory_component(target_input) ||
        strncmp(target_input, "sd:/", 4) == 0 ||
        target_input[0] == '/') {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    snprintf(source_dir, sizeof(source_dir), "%s", source_path);
    last_sep = strrchr(source_dir, '/');
    if (last_sep == NULL || strcmp(source_dir, BSP_SD_MOUNT_POINT) == 0) {
        return shell_fs_resolve_path(target_input, target_path, target_path_size);
    }

    *last_sep = '\0';
    if (source_dir[0] == '\0') {
        snprintf(source_dir, sizeof(source_dir), "%s", BSP_SD_MOUNT_POINT);
    }

    if (snprintf(target_path, target_path_size, "%s/%s", source_dir, target_input) >= (int)target_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* ========================================================================
 * CURRENT WORKING DIRECTORY
 * ======================================================================== */

const char *shell_get_cwd(void)
{
    return s_shell_cwd;
}

void storage_set_cwd(const char *absolute_path)
{
    if (absolute_path == NULL || absolute_path[0] == '\0') {
        return;
    }

    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", absolute_path);
}
