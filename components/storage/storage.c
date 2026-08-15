/**
 * @file storage.c
 * @brief SD session management and filesystem services for P4MiniShell.
 *
 * Owns the guarded SD mount session, all path resolution, VFS-to-FATFS
 * conversion, size formatting, DOS wildcard matching, the RAM-only current
 * working directory, and the shared file helpers used by the DOS file
 * commands. All transcript output and debug logging go through shell.c.
 *
 * State owned here:
 *   - Current working directory (RAM-only, rooted at the SD mount point)
 *   - SD mount tracking and the explicit eject latch
 */

#include "storage.h"
#include "shell.h"
#include "ansi_palette.h"
#include "header.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

/* Backward-compatibility aliases */
#define STORAGE_TAG                     P4_CONFIG_SHELL_TAG
#define SHELL_SD_FATFS_DRIVE            P4_CONFIG_SD_FATFS_DRIVE
#define SHELL_SD_PATH_BYTES             P4_CONFIG_SD_PATH_BYTES
#define SHELL_SD_LIST_LIMIT             P4_CONFIG_SD_LIST_LIMIT
#define SHELL_SD_IO_BUFFER_BYTES        P4_CONFIG_SD_IO_BUFFER_BYTES
#define SHELL_FILE_IO_BUFFER_BYTES      P4_CONFIG_FILE_IO_BUFFER_BYTES
#define SHELL_LFN_BYTES                 P4_CONFIG_LFN_BYTES

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bool s_initialized = false;

/* Shell working directory (RAM-only, not persisted across boots) */
static char s_shell_cwd[SHELL_SD_PATH_BYTES];

/* SD mount tracking */
static bool s_sd_persistent_mounted;  /* true while the card is mounted */
static bool s_sd_ejected;             /* set by sdeject to block auto-remount */

/* First-mount hook: fires once per boot when the card mounts for the first
 * time, so main can generate default boot files and show a welcome. */
static void (*s_sd_first_mount_callback)(void) = NULL;
static bool s_sd_first_mount_fired = false;

/* Pending `<` or pipe input source for the text-processing commands */
static char s_input_redirect[SHELL_SD_PATH_BYTES];
static bool s_input_redirect_active;

/* ========================================================================
 * SD SESSION
 * ======================================================================== */

static void storage_sd_mark_mounted(void);

/**
 * Pre-initialize the shared SDMMC host driver once, synchronously, before any
 * other boot component touches it.
 *
 * The SD card (slot 0, via bsp_sdcard_mount -> esp_vfs_fat_sdmmc_mount) and
 * the ESP-Hosted C6 transport (slot 1, via esp_hosted_init ->
 * eh_host_port_sdio_init) both call sdmmc_host_init() at boot from different
 * tasks. sdmmc_host_init() is NOT thread-safe (it checks/sets the global
 * s_host_ctx.intr_handle without a lock), so a concurrent double-call corrupts
 * the host driver state and makes sdmmc_host_wait_for_event time out, failing
 * BOTH slots (N1). Initializing the host here first makes every later call hit
 * the driver's idempotent "already initialized, skip" branch, so the race can
 * never occur. Call once from app_main before networking_init().
 */
esp_err_t storage_sdmmc_host_preinit(void)
{
    esp_err_t error = sdmmc_host_init();

    if (error != ESP_OK) {
        ESP_LOGW(STORAGE_TAG, "SDMMC host pre-init failed (0x%x); boot components will retry it",
                 (unsigned int)error);
    }
    return error;
}

/**
 * Begin a guarded SD access session.
 *
 * Every SD-touching command goes through this single mount path so a failure
 * can never leak mounted state or dereference missing card metadata. The
 * header SD icon is updated on mount so the status bar stays in sync with the
 * real card state.
 *
 * The mount is persistent: shell_sd_end() deliberately does not unmount.
 * Only the explicit `sdeject` command tears the mount down.
 */
esp_err_t shell_sd_begin(shell_sd_session_t *session)
{
    esp_err_t error;

    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->mounted_here = false;

    /* If already mounted, just return success (persistent mount) */
    if (s_sd_persistent_mounted) {
        storage_sd_ensure_dma_buffer();
        return ESP_OK;
    }

    /* If ejected, refuse to mount unless user runs an explicit command */
    if (s_sd_ejected) {
        return ESP_ERR_INVALID_STATE;
    }

    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        session->mounted_here = true;
        storage_sd_mark_mounted();
        storage_sd_ensure_dma_buffer();
        return ESP_OK;
    }

    if (error == ESP_ERR_INVALID_STATE) {
        /* Already mounted (BSP internal state) */
        storage_sd_mark_mounted();
        storage_sd_ensure_dma_buffer();
        return ESP_OK;
    }

    return error;
}

/**
 * Cache a DMA-capable scratch buffer on the SDMMC host so every card
 * transaction reuses it instead of allocating (and risking failure) from
 * the tight internal heap per operation. Called right after a mount.
 */
void storage_sd_ensure_dma_buffer(void)
{
    if (bsp_sdcard == NULL) {
        return;
    }
    if (bsp_sdcard->host.dma_aligned_buffer != NULL) {
        return;
    }

    size_t sector_size = (bsp_sdcard->csd.sector_size > 0) ? bsp_sdcard->csd.sector_size : 512;
    size_t buffer_size = P4_CONFIG_SD_DMA_BUFFER_BYTES;
    size_t chunk = buffer_size / sector_size;
    if (chunk < 1) {
        chunk = 1;
        buffer_size = sector_size;
    }

    void *buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGW(STORAGE_TAG, "SD DMA buffer pre-allocation failed (%u B); falling back to per-op allocation",
                 (unsigned int)buffer_size);
        return;
    }

    bsp_sdcard->host.dma_aligned_buffer = buffer;
    bsp_sdcard->host.unaligned_multi_block_rw_max_chunk_size = chunk;
    ESP_LOGI(STORAGE_TAG, "SD card DMA scratch buffer cached (%u B, %u block(s) per op)",
             (unsigned int)buffer_size, (unsigned int)chunk);
}

/** Mark the card as mounted, update the header icon, and fire the one-shot
 *  first-mount callback so main can generate defaults + show a welcome. */
static void storage_sd_mark_mounted(void)
{
    bool first = !s_sd_persistent_mounted && !s_sd_first_mount_fired;

    s_sd_persistent_mounted = true;
    header_update_sd(HEADER_SD_MOUNTED);
    if (first) {
        s_sd_first_mount_fired = true;
        if (s_sd_first_mount_callback != NULL) {
            s_sd_first_mount_callback();
        }
    }
}

void storage_register_sd_first_mount_callback(void (*callback)(void))
{
    s_sd_first_mount_callback = callback;
}

void shell_command_sd_mount(void)
{
    shell_sd_session_t session;
    esp_err_t error;

    /* Clear the eject latch so a re-inserted card can be mounted again
     * without rebooting, then try the normal mount path. */
    s_sd_ejected = false;
    error = shell_sd_begin(&session);
    if (error == ESP_OK) {
        shell_print_ok("SD card mounted");
    } else {
        shell_print_error("sd: SD card not present - insert and retry");
    }
    shell_sd_end(&session, "sd");
}

void shell_sd_end(shell_sd_session_t *session, const char *operation)
{
    /* Persistent mount: do NOT unmount after each command.
     * Only unmount on explicit sdeject command.
     * This keeps the SD card accessible and the header icon accurate. */
    (void)session;
    (void)operation;
}

bool storage_sd_is_mounted(void)
{
    /* Persistent mount tracking rather than stat(), which only succeeds
     * while the card is already mounted. */
    return s_sd_persistent_mounted;
}

void shell_command_sd_eject(void)
{
    esp_err_t error;

    /* Force unmount the SD card unconditionally (not session-guarded).
     * This is the safe-removal path: unmounts even if mounted by another session. */
    error = bsp_sdcard_unmount();
    if (error == ESP_OK) {
        s_sd_persistent_mounted = false;
        s_sd_ejected = true;
        /* The cached DMA scratch buffer belongs to the (now gone) card
         * handle; release it so the internal DMA-capable heap is reclaimed
         * and a re-inserted card gets a fresh buffer on next mount. */
        if (bsp_sdcard != NULL) {
            if (bsp_sdcard->host.dma_aligned_buffer != NULL) {
                free(bsp_sdcard->host.dma_aligned_buffer);
                bsp_sdcard->host.dma_aligned_buffer = NULL;
                bsp_sdcard->host.unaligned_multi_block_rw_max_chunk_size = 0;
            }
        }
        shell_print_ok("SD card unmounted safely. You may now remove the card.");
        shell_header_notify("SD card ejected", P4_CONFIG_HEADER_NOTIFY_TIMEOUT_MS);
        header_update_sd(HEADER_SD_NONE);
    } else if (error == ESP_ERR_INVALID_STATE) {
        shell_print_muted("SD card is not currently mounted.");
    } else {
        shell_print_warning("SD eject warning: %s (0x%x)", esp_err_to_name(error), (unsigned int)error);
        shell_record_warningf("sd", "Eject unmount warning: %s", esp_err_to_name(error));
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

esp_err_t shell_sd_stat_path(const char *path, struct stat *st)
{
    if (path == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, st) == 0) {
        return ESP_OK;
    }

    if (errno == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_FAIL;
}

esp_err_t shell_sd_fresult_to_esp_err(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return ESP_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ESP_ERR_NOT_FOUND;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
    case FR_INVALID_DRIVE:
        return ESP_ERR_INVALID_ARG;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return ESP_ERR_INVALID_STATE;
    case FR_NOT_ENOUGH_CORE:
        return ESP_ERR_NO_MEM;
    case FR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

/**
 * Convert a VFS path under the SD mount point to a raw FATFS path.
 *
 * The direct FATFS API is used for directory listings because it exposes long
 * file names (CONFIG_FATFS_LFN_HEAP with MAX_LFN=255) and the 8.3 alternate
 * name, which the POSIX dirent path does not surface.
 */
esp_err_t shell_sd_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size)
{
    const char *relative_path;
    int written;

    if (vfs_path == NULL || fatfs_path == NULL || fatfs_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(vfs_path, BSP_SD_MOUNT_POINT) == 0) {
        relative_path = "/";
    } else if (strncmp(vfs_path, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        relative_path = vfs_path + strlen(BSP_SD_MOUNT_POINT);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(fatfs_path, fatfs_path_size, "%s%s", SHELL_SD_FATFS_DRIVE, relative_path);
    if (written < 0) {
        fatfs_path[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= fatfs_path_size) {
        fatfs_path[0] = '\0';
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
    int written;
    size_t index;

    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (input == NULL || input[0] == '\0') {
        snprintf(output, output_size, "%s", s_shell_cwd[0] != '\0' ? s_shell_cwd : BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    if (strncmp(input, "sd:/", 4) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input + 4);
    } else if (strcmp(input, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/");
    } else if (strncmp(input, BSP_SD_MOUNT_POINT "/", strlen(BSP_SD_MOUNT_POINT) + 1) == 0) {
        snprintf(combined, sizeof(combined), "%s", input + strlen(BSP_SD_MOUNT_POINT));
    } else if (input[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", input);
    } else if (strcmp(s_shell_cwd, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input);
    } else {
        snprintf(combined,
                 sizeof(combined),
                 "%s/%s",
                 s_shell_cwd + strlen(BSP_SD_MOUNT_POINT),
                 input);
    }

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

    written = snprintf(output, output_size, "%s", BSP_SD_MOUNT_POINT);
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

    if (path == NULL || extension == NULL) {
        return false;
    }

    path_len = strlen(path);
    ext_len = strlen(extension);
    if (path_len < ext_len) {
        return false;
    }

    return shell_text_equals_ignore_case(path + path_len - ext_len, extension);
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
 * FORMATTING AND MATCHING
 * ======================================================================== */

void shell_sd_format_size(uint64_t size_bytes, char *output, size_t output_size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)size_bytes;
    size_t unit_index = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    while (value >= 1024.0 && unit_index < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(output, output_size, "%llu %s", (unsigned long long)size_bytes, units[unit_index]);
    } else {
        snprintf(output, output_size, "%.1f %s", value, units[unit_index]);
    }
}

const char *shell_sd_entry_type(const struct stat *st)
{
    if (st == NULL) {
        return "unknown";
    }

    if (S_ISDIR(st->st_mode)) {
        return "dir";
    }

    if (S_ISREG(st->st_mode)) {
        return "file";
    }

    return "other";
}

/* DOS-style wildcard matching: `*` spans any run of characters, `?` matches
 * exactly one, and comparisons are case-insensitive like FAT itself. */
bool shell_wildcard_match(const char *pattern, const char *name)
{
    if (pattern == NULL || name == NULL) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return true;
            while (*name) {
                if (shell_wildcard_match(pattern, name)) return true;
                name++;
            }
            return false;
        } else if (*pattern == '?') {
            if (*name == '\0') return false;
            pattern++;
            name++;
        } else {
            if (toupper((unsigned char)*pattern) != toupper((unsigned char)*name))
                return false;
            pattern++;
            name++;
        }
    }
    return *name == '\0';
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

void shell_fs_print_cwd(void)
{
    /* Print the full VFS path so the transcript agrees with the prompt, which
     * also renders the full path (e.g. "/sdcard" at the root, not "\"). */
    shell_transcript_appendf("%s\n", s_shell_cwd);
}

/* ========================================================================
 * SHARED FILE OPERATIONS
 * ======================================================================== */

/**
 * Copy one regular file with guardrails.
 *
 * Before transferring a byte this refuses to copy a file onto itself, and
 * checks that the destination volume has room for the whole source plus the
 * configured safety margin. Without the precheck a large copy would fill the
 * card and leave a truncated destination behind.
 *
 * Files above P4_CONFIG_COPY_PROGRESS_THRESHOLD report percentage progress,
 * so a multi-megabyte transfer does not look like a hang.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG when source and destination are the
 *         same file, ESP_ERR_NOT_FOUND when the source is missing,
 *         ESP_ERR_NO_MEM when free space is insufficient, or ESP_FAIL.
 */
esp_err_t shell_fs_copy_file(const char *source_path, const char *dest_path)
{
    shell_sd_session_t session;
    FILE *source = NULL;
    FILE *dest = NULL;
    uint8_t buffer[SHELL_FILE_IO_BUFFER_BYTES];
    uint64_t source_bytes;
    uint64_t dest_bytes;
    uint64_t copied = 0;
    int last_progress_pct = 0;
    bool report_progress;
    esp_err_t error;

    if (source_path == NULL || dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Copying a file onto itself would open the destination with "wb",
     * truncating the source to zero before the first read. */
    if (storage_paths_are_same(source_path, dest_path)) {
        shell_print_error("copy: %s and the destination are the same file", source_path);
        return ESP_ERR_INVALID_ARG;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    source_bytes = storage_get_file_size(source_path);
    dest_bytes = storage_get_file_size(dest_path);

    /* An existing destination is truncated, so its blocks come back to the
     * free pool and only the difference is actually needed. */
    if (!storage_check_free_space(source_bytes, dest_bytes, "copy")) {
        shell_sd_end(&session, "copy");
        return ESP_ERR_NO_MEM;
    }

    source = fopen(source_path, "rb");
    if (source == NULL) {
        shell_sd_end(&session, "copy");
        return ESP_ERR_NOT_FOUND;
    }

    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        char dest_fatfs[SHELL_SD_PATH_BYTES];
        FILINFO dest_info;

        fclose(source);

        /* Now that attributes are carried across copies, a read-only
         * destination is a realistic reason for this failure. Say so instead
         * of reporting a bare error, and point at the fix. */
        memset(&dest_info, 0, sizeof(dest_info));
        if (shell_sd_vfs_to_fatfs_path(dest_path, dest_fatfs, sizeof(dest_fatfs)) == ESP_OK &&
            f_stat(dest_fatfs, &dest_info) == FR_OK &&
            (dest_info.fattrib & AM_RDO) != 0) {
            shell_print_error("copy: %s is read-only (use attrib -R to clear it)", dest_path);
        }

        shell_sd_end(&session, "copy");
        return ESP_FAIL;
    }

    report_progress = (source_bytes >= P4_CONFIG_COPY_PROGRESS_THRESHOLD);

    while (!feof(source)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            fclose(dest);
            fclose(source);
            /* Remove the partial destination so a failed copy never leaves a
             * truncated file that looks complete. */
            (void)unlink(dest_path);
            shell_print_error("copy: write failed, removed the partial %s", dest_path);
            shell_sd_end(&session, "copy");
            return ESP_FAIL;
        }

        copied += bytes_read;

        if (report_progress && source_bytes > 0) {
            int pct = (int)((copied * 100ULL) / source_bytes);

            if (pct >= last_progress_pct + P4_CONFIG_COPY_PROGRESS_STEP_PCT) {
                last_progress_pct = pct - (pct % P4_CONFIG_COPY_PROGRESS_STEP_PCT);
                shell_transcript_appendf_ansi(SH_LBL "copy:" SH_RST " " SH_NUM "%d%%" SH_RST "\n", last_progress_pct);
            }
        }
    }

    fclose(dest);
    fclose(source);

    /* Carry the source's R/H/S/A attributes across, after the data is written
     * because a read-only destination could not have been opened for writing.
     * A failure here is reported but does not fail the copy: the file content
     * is already correct, and losing an archive bit is not worth discarding
     * a completed transfer. */
    {
        esp_err_t attr_error = storage_copy_attributes(source_path, dest_path);

        if (attr_error != ESP_OK) {
            shell_record_warningf("copy",
                                  "Copied %s but could not carry its attributes (%s)",
                                  source_path,
                                  esp_err_to_name(attr_error));
        }
    }

    shell_sd_end(&session, "copy");
    return ESP_OK;
}

esp_err_t storage_copy_attributes(const char *source_path, const char *dest_path)
{
    char source_fatfs[SHELL_SD_PATH_BYTES];
    char dest_fatfs[SHELL_SD_PATH_BYTES];
    FILINFO info;
    FRESULT result;
    esp_err_t error;

    if (source_path == NULL || dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = shell_sd_vfs_to_fatfs_path(source_path, source_fatfs, sizeof(source_fatfs));
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_vfs_to_fatfs_path(dest_path, dest_fatfs, sizeof(dest_fatfs));
    if (error != ESP_OK) {
        return error;
    }

    memset(&info, 0, sizeof(info));
    result = f_stat(source_fatfs, &info);
    if (result != FR_OK) {
        return shell_sd_fresult_to_esp_err(result);
    }

    /* Carry only the user-visible attribute bits. AM_DIR is structural and
     * must never be forced onto a destination. */
    result = f_chmod(dest_fatfs,
                     info.fattrib & (AM_RDO | AM_HID | AM_SYS | AM_ARC),
                     AM_RDO | AM_HID | AM_SYS | AM_ARC);

    return shell_sd_fresult_to_esp_err(result);
}

esp_err_t shell_list_directory_path(const char *normalized_path)
{
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
    unsigned int dir_count = 0;
    unsigned int file_count = 0;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        shell_sd_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        shell_transcript_appendf("%s  %s\n", size_text, normalized_path);
        shell_sd_end(&session, "dir");
        return ESP_OK;
    }

    error = shell_sd_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_sd_end(&session, "dir");
        return error;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        shell_sd_end(&session, "dir");
        return shell_sd_fresult_to_esp_err(result);
    }
    dir_open = true;

    shell_transcript_appendf_ansi(SH_HEAD " Directory of %s" SH_RST "\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = shell_sd_fresult_to_esp_err(result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            error = ESP_OK;
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == SHELL_SD_LIST_LIMIT) {
            shell_transcript_appendf("dir: listing truncated after %u entries\n", (unsigned int)SHELL_SD_LIST_LIMIT);
            shell_record_warningf("dir", "Truncated directory listing for %s", normalized_path);
            error = ESP_OK;
            break;
        }

        if (entry_info.fattrib & AM_DIR) {
            shell_transcript_appendf_ansi(SH_SIZE "<DIR>" SH_RST "      " SH_DIR "%s" SH_RST "\n", lfn);
            dir_count++;
        } else {
            shell_sd_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
            shell_transcript_appendf_ansi(SH_SIZE "%10s" SH_RST " " SH_FILE "%s" SH_RST "\n", size_text, lfn);
            file_count++;
        }
        listed_entries++;
    }

    shell_transcript_appendf_ansi(SH_NUM "%u" SH_RST " " SH_LBL "file(s)" SH_RST "  " SH_NUM "%u" SH_RST " " SH_LBL "dir(s)" SH_RST "\n", file_count, dir_count);
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    shell_sd_end(&session, "dir");
    return error;
}

esp_err_t shell_print_file_text(const char *normalized_path)
{
    shell_sd_session_t session;
    FILE *file = NULL;
    struct stat path_stat;
    esp_err_t error;
    unsigned char buffer[SHELL_SD_IO_BUFFER_BYTES + 1];

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        shell_sd_end(&session, "type");
        return error;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        shell_sd_end(&session, "type");
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "type");
        return ESP_FAIL;
    }

    while (!feof(file)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
        size_t index;

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
        shell_transcript_appendf("%s", (char *)buffer);
    }

    fclose(file);
    shell_transcript_append_text("\n");
    shell_sd_end(&session, "type");
    return ESP_OK;
}

/* ========================================================================
 * WILDCARD EXPANSION (for `for` loops)
 * ======================================================================== */

/**
 * Split a pattern into its directory part and filename-wildcard part.
 * Mutates @p pattern in place by null-terminating the directory and returning
 * a pointer to the name. For `sub\*.txt` the directory is `sub` and the name
 * is `*.txt`; for `*.txt` the directory is empty and the name is the whole
 * pattern.
 */
static char *storage_split_pattern(char *pattern, char **dir_out)
{
    char *last_sep = NULL;
    char *p = pattern;

    while (*p != '\0') {
        if (*p == '\\' || *p == '/') {
            last_sep = p;
        }
        p++;
    }

    if (last_sep == NULL) {
        *dir_out = "";
        return pattern;
    }

    *last_sep = '\0';
    *dir_out = pattern;
    return last_sep + 1;
}

esp_err_t storage_expand_wildcard(const char *pattern, char ***tokens_out, int *count_out)
{
    shell_sd_session_t session;
    char pattern_copy[SHELL_SD_PATH_BYTES];
    char *name_pattern;
    char *dir_part;
    char dir_resolved[SHELL_SD_PATH_BYTES];
    char fatfs_path[SHELL_SD_PATH_BYTES];
    char lfn[SHELL_LFN_BYTES];
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    char **tokens = NULL;
    int count = 0;
    int capacity = 0;

    if (pattern == NULL || tokens_out == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *tokens_out = NULL;
    *count_out = 0;

    if (strlen(pattern) >= sizeof(pattern_copy)) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(pattern_copy, sizeof(pattern_copy), "%s", pattern);

    name_pattern = storage_split_pattern(pattern_copy, &dir_part);

    /* Resolve the directory portion against the SD root / cwd. An empty
     * directory means the current working directory. */
    {
        char *resolve_input = (dir_part[0] != '\0') ? dir_part : ".";
        error = shell_fs_resolve_path(resolve_input, dir_resolved, sizeof(dir_resolved));
        if (error != ESP_OK) {
            return error;
        }
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_vfs_to_fatfs_path(dir_resolved, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        shell_sd_end(&session, "wildcard");
        return error;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        shell_sd_end(&session, "wildcard");
        return shell_sd_fresult_to_esp_err(result);
    }

    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK || entry_info.fname[0] == '\0') {
            break;
        }

        if (strcmp(entry_info.fname, ".") == 0 || strcmp(entry_info.fname, "..") == 0) {
            continue;
        }
        if (entry_info.fattrib & AM_DIR) {
            continue;   /* file-only wildcard for basic set form */
        }
        if (!shell_wildcard_match(name_pattern, entry_info.fname)) {
            continue;
        }

        if (count >= SHELL_SD_LIST_LIMIT) {
            shell_record_warningf("wildcard", "Wildcard expansion truncated at %d entries", SHELL_SD_LIST_LIMIT);
            break;
        }

        /* Grow the token array. Start small and double; heap-allocated so it
         * never lives on the recursive command/batch stack. */
        if (count >= capacity) {
            int new_cap = (capacity == 0) ? 8 : capacity * 2;
            char **grown = realloc(tokens, sizeof(char *) * new_cap);
            if (grown == NULL) {
                free(tokens);
                f_closedir(&dir);
                shell_sd_end(&session, "wildcard");
                return ESP_ERR_NO_MEM;
            }
            tokens = grown;
            capacity = new_cap;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        size_t token_len = strlen(dir_resolved) + 1 + strlen(lfn) + 1;
        char *token = malloc(token_len);
        if (token == NULL) {
            storage_free_wildcard_expansion(tokens, count);
            f_closedir(&dir);
            shell_sd_end(&session, "wildcard");
            return ESP_ERR_NO_MEM;
        }
        if (dir_resolved[0] != '\0' && strcmp(dir_resolved, BSP_SD_MOUNT_POINT) != 0) {
            snprintf(token, token_len, "%s/%s", dir_resolved, lfn);
        } else {
            snprintf(token, token_len, "%s", lfn);
        }
        tokens[count++] = token;
    }

    f_closedir(&dir);
    shell_sd_end(&session, "wildcard");

    *tokens_out = tokens;
    *count_out = count;
    return ESP_OK;
}

void storage_free_wildcard_expansion(char **tokens, int count)
{
    if (tokens == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

esp_err_t shell_write_redirect_output(const char *path, const char *text, bool append_mode)
{
    char resolved_path[SHELL_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    error = shell_fs_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        shell_sd_end(&session, "redirection");
        return ESP_FAIL;
    }

    if (text != NULL && text[0] != '\0') {
        size_t text_len = strlen(text);
        if (fwrite(text, 1, text_len, file) != text_len) {
            fclose(file);
            shell_sd_end(&session, "redirection");
            return ESP_FAIL;
        }
    }

    fclose(file);
    shell_sd_end(&session, "redirection");
    return ESP_OK;
}

/* ========================================================================
 * VOLUME CAPACITY AND GUARDRAILS
 * ======================================================================== */

esp_err_t storage_get_space_info(storage_space_info_t *info_out)
{
    shell_sd_session_t session;
    FATFS *fatfs = NULL;
    DWORD free_clusters = 0;
    FRESULT result;
    esp_err_t error;

    if (info_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info_out, 0, sizeof(*info_out));

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    /* f_getfree() also hands back the FATFS object, which carries the
     * cluster geometry needed to turn cluster counts into bytes. */
    result = f_getfree(SHELL_SD_FATFS_DRIVE, &free_clusters, &fatfs);
    if (result != FR_OK || fatfs == NULL) {
        shell_sd_end(&session, "space");
        return shell_sd_fresult_to_esp_err(result);
    }

    /* n_fatent counts the FAT entries; the first two are reserved, so the
     * usable cluster count is n_fatent - 2. */
    info_out->total_clusters = (fatfs->n_fatent > 2) ? (uint32_t)(fatfs->n_fatent - 2) : 0;
    info_out->free_clusters = (uint32_t)free_clusters;

    /* FATFS only carries a per-volume `ssize` when the build supports more
     * than one sector size; otherwise every volume uses FF_MAX_SS. */
#if FF_MAX_SS != FF_MIN_SS
    info_out->cluster_bytes = (uint32_t)fatfs->csize * (uint32_t)fatfs->ssize;
#else
    info_out->cluster_bytes = (uint32_t)fatfs->csize * (uint32_t)FF_MAX_SS;
#endif
    info_out->total_bytes = (uint64_t)info_out->total_clusters * (uint64_t)info_out->cluster_bytes;
    info_out->free_bytes = (uint64_t)info_out->free_clusters * (uint64_t)info_out->cluster_bytes;
    info_out->used_bytes = (info_out->total_bytes >= info_out->free_bytes)
                               ? (info_out->total_bytes - info_out->free_bytes)
                               : 0;

    shell_sd_end(&session, "space");
    return ESP_OK;
}

/* ========================================================================
 * DISK AND PARTITION SERVICES (diskpart / DOS FORMAT style)
 * ========================================================================
 * Physical-disk operations for the SD card. The command layer owns argument
 * parsing and the destructive-confirmation contract; the mechanics here keep
 * storage.c as the single owner of mount state and low-level FATFS/sdmmc
 * access. Volume targeting is parameterized (storage_volume_t) so a future
 * USB OTG MSC volume can be added without changing the command surface.
 */

/* MBR partition-table layout, matching FatFs ff.c's own constants. */
#define STORAGE_MBR_TABLE_OFFSET      446
#define STORAGE_MBR_PTE_SIZE          16
#define STORAGE_MBR_SIGNATURE_OFFSET  510
#define STORAGE_PTE_BOOT_FLAG         0
#define STORAGE_PTE_TYPE_OFFSET       4
#define STORAGE_PTE_START_OFFSET      8
#define STORAGE_PTE_SIZE_OFFSET       12
#define STORAGE_PTE_BOOTABLE          0x80
/* FAT32 with LBA addressing; the partition type `format` produces for a
 * modern (>= 2 GiB) SD card. FAT12/16 use 0x01/0x04/0x06/0x0E. */
#define STORAGE_PARTITION_TYPE_FAT32_LBA 0x0C

/** Decode a little-endian DWORD from an MBR PTE. */
static uint32_t storage_mbr_get_dword(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static void storage_mbr_put_dword(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static esp_err_t storage_volume_require_sd(storage_volume_t vol)
{
    if (vol != STORAGE_VOLUME_SD) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t storage_disk_get_info(storage_volume_t vol, storage_disk_info_t *out)
{
    shell_sd_session_t session;
    esp_err_t error;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }

    memset(out, 0, sizeof(*out));

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    if (bsp_sdcard != NULL) {
        snprintf(out->card_name, sizeof(out->card_name), "%s", bsp_sdcard->cid.name);
        out->sector_size = bsp_sdcard->csd.sector_size;
        out->sector_count = bsp_sdcard->csd.capacity;
        out->capacity_bytes = (uint64_t)out->sector_size * (uint64_t)out->sector_count;
        out->max_freq_khz = bsp_sdcard->max_freq_khz;
    }

    shell_sd_end(&session, "disk info");
    return (bsp_sdcard != NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t storage_disk_read_mbr(storage_volume_t vol, storage_mbr_t *out)
{
    shell_sd_session_t session;
    uint8_t sector[512];
    esp_err_t error;
    int index;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }

    memset(out, 0, sizeof(*out));

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }
    if (bsp_sdcard == NULL) {
        shell_sd_end(&session, "disk detail");
        return ESP_ERR_INVALID_STATE;
    }

    error = sdmmc_read_sectors(bsp_sdcard, sector, 0, 1);
    shell_sd_end(&session, "disk detail");
    if (error != ESP_OK) {
        return error;
    }

    if (sector[STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
        sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        /* No valid partition table; the struct stays valid=false. */
        return ESP_OK;
    }

    out->valid = true;
    for (index = 0; index < 4; index++) {
        const uint8_t *pte = sector + STORAGE_MBR_TABLE_OFFSET + index * STORAGE_MBR_PTE_SIZE;

        out->partitions[index].bootable = (pte[STORAGE_PTE_BOOT_FLAG] == STORAGE_PTE_BOOTABLE);
        out->partitions[index].type = pte[STORAGE_PTE_TYPE_OFFSET];
        out->partitions[index].start_lba = storage_mbr_get_dword(pte + STORAGE_PTE_START_OFFSET);
        out->partitions[index].size_lba = storage_mbr_get_dword(pte + STORAGE_PTE_SIZE_OFFSET);
    }

    return ESP_OK;
}

esp_err_t storage_disk_clean(storage_volume_t vol)
{
    shell_sd_session_t session;
    uint8_t sector[512];
    esp_err_t error;

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }
    if (bsp_sdcard == NULL) {
        shell_sd_end(&session, "disk clean");
        return ESP_ERR_INVALID_STATE;
    }

    /* Unmount the FATFS volume so no stale partition cache survives the MBR
     * rewrite. The card stays initialized, so `format` can run afterwards. */
    f_mount(NULL, SHELL_SD_FATFS_DRIVE, 0);

    memset(sector, 0, sizeof(sector));
    sector[STORAGE_MBR_SIGNATURE_OFFSET] = 0x55;
    sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    error = sdmmc_write_sectors(bsp_sdcard, sector, 0, 1);

    /* Even on write failure the volume is unmounted and the old partition
     * table may be gone, so reflect "card present, no filesystem". */
    header_update_sd(HEADER_SD_INSERTED);
    shell_sd_end(&session, "disk clean");
    return error;
}

esp_err_t storage_disk_create_primary_partition(storage_volume_t vol, uint64_t size_bytes)
{
    shell_sd_session_t session;
    uint8_t sector[512];
    uint64_t total_sectors;
    uint64_t start_lba;
    uint64_t size_lba;
    int slot = -1;
    int index;
    esp_err_t error;

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }
    if (bsp_sdcard == NULL) {
        shell_sd_end(&session, "disk create");
        return ESP_ERR_INVALID_STATE;
    }

    error = sdmmc_read_sectors(bsp_sdcard, sector, 0, 1);
    if (error != ESP_OK) {
        shell_sd_end(&session, "disk create");
        return error;
    }

    /* Find the first free partition-table slot (no type and no size). */
    for (index = 0; index < 4; index++) {
        const uint8_t *pte = sector + STORAGE_MBR_TABLE_OFFSET + index * STORAGE_MBR_PTE_SIZE;
        bool occupied = (pte[STORAGE_PTE_TYPE_OFFSET] != 0) ||
                        (storage_mbr_get_dword(pte + STORAGE_PTE_SIZE_OFFSET) != 0);

        if (!occupied && slot == -1) {
            slot = index;
        }
    }
    if (slot == -1) {
        shell_sd_end(&session, "disk create");
        return ESP_ERR_INVALID_STATE;
    }

    /* Size and align the partition. */
    total_sectors = (uint64_t)bsp_sdcard->csd.capacity;
    start_lba = P4_CONFIG_DISK_PARTITION_ALIGN_SECTORS;
    if (size_bytes > 0) {
        size_lba = size_bytes / (uint64_t)bsp_sdcard->csd.sector_size;
    } else {
        size_lba = 0;
    }
    if (size_lba == 0 || start_lba + size_lba > total_sectors) {
        size_lba = (start_lba < total_sectors) ? (total_sectors - start_lba) : 0;
    }
    if (size_lba == 0) {
        shell_sd_end(&session, "disk create");
        return ESP_ERR_INVALID_STATE;
    }

    {
        uint8_t *pte = sector + STORAGE_MBR_TABLE_OFFSET + slot * STORAGE_MBR_PTE_SIZE;

        memset(pte, 0, STORAGE_MBR_PTE_SIZE);
        pte[STORAGE_PTE_TYPE_OFFSET] = STORAGE_PARTITION_TYPE_FAT32_LBA;
        storage_mbr_put_dword(pte + STORAGE_PTE_START_OFFSET, (uint32_t)start_lba);
        storage_mbr_put_dword(pte + STORAGE_PTE_SIZE_OFFSET, (uint32_t)size_lba);
    }
    sector[STORAGE_MBR_SIGNATURE_OFFSET] = 0x55;
    sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    /* Unmount before writing so the mounted volume cannot cache a stale
     * partition table. The card stays initialized for `format`. */
    f_mount(NULL, SHELL_SD_FATFS_DRIVE, 0);
    error = sdmmc_write_sectors(bsp_sdcard, sector, 0, 1);

    header_update_sd(HEADER_SD_INSERTED);
    shell_sd_end(&session, "disk create");
    return error;
}

esp_err_t storage_disk_delete_partition(storage_volume_t vol, unsigned partition_index)
{
    shell_sd_session_t session;
    uint8_t sector[512];
    esp_err_t error;

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }
    if (partition_index >= 4) {
        return ESP_ERR_INVALID_ARG;
    }

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }
    if (bsp_sdcard == NULL) {
        shell_sd_end(&session, "disk delete");
        return ESP_ERR_INVALID_STATE;
    }

    error = sdmmc_read_sectors(bsp_sdcard, sector, 0, 1);
    if (error != ESP_OK) {
        shell_sd_end(&session, "disk delete");
        return error;
    }

    memset(sector + STORAGE_MBR_TABLE_OFFSET + partition_index * STORAGE_MBR_PTE_SIZE,
           0, STORAGE_MBR_PTE_SIZE);
    sector[STORAGE_MBR_SIGNATURE_OFFSET] = 0x55;
    sector[STORAGE_MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    f_mount(NULL, SHELL_SD_FATFS_DRIVE, 0);
    error = sdmmc_write_sectors(bsp_sdcard, sector, 0, 1);

    header_update_sd(HEADER_SD_INSERTED);
    shell_sd_end(&session, "disk delete");
    return error;
}

esp_err_t storage_format_volume(storage_volume_t vol, const storage_format_opts_t *opts)
{
    esp_vfs_fat_mount_config_t cfg;
    esp_err_t error;

    if (opts == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    error = storage_volume_require_sd(vol);
    if (error != ESP_OK) {
        return error;
    }

    /* esp_vfs_fat_sdcard_format_cfg() needs an initialized card whose FATFS
     * context is still registered (a mount or the disk commands keep it so). */
    if (bsp_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The standard IDF helper unmounts, formats with a size-appropriate FAT
     * type (FAT12/16 for small volumes, FAT32 for large), and remounts. The
     * allocation-unit size comes straight from the /A: option; 0 lets FATFS
     * choose. max_files must match the BSP mount so the remount's VFS
     * registration is consistent. */
    cfg.format_if_mount_failed = false;
    cfg.max_files = 8;
    cfg.allocation_unit_size = opts->alloc_unit_bytes;
    cfg.disk_status_check_enable = false;
    cfg.use_one_fat = false;

    error = esp_vfs_fat_sdcard_format_cfg(BSP_SD_MOUNT_POINT, bsp_sdcard, &cfg);
    if (error != ESP_OK) {
        return error;
    }

    /* Apply the requested label to the freshly formatted volume. */
    if (opts->label[0] != '\0') {
        char drive_label[24];
        char padded[12];
        size_t length = strlen(opts->label);
        size_t position;

        memset(padded, ' ', 11);
        padded[11] = '\0';
        for (position = 0; position < length && position < 11; position++) {
            padded[position] = (char)toupper((unsigned char)opts->label[position]);
        }
        snprintf(drive_label, sizeof(drive_label), "%s%s", SHELL_SD_FATFS_DRIVE, padded);
        (void)f_setlabel(drive_label);
    }

    header_update_sd(HEADER_SD_MOUNTED);
    return ESP_OK;
}

const char *storage_get_fat_type(void)
{
    shell_sd_session_t session;
    FATFS *fatfs = NULL;
    DWORD free_clusters = 0;

    if (shell_sd_begin(&session) != ESP_OK) {
        return "unknown";
    }
    if (f_getfree(SHELL_SD_FATFS_DRIVE, &free_clusters, &fatfs) != FR_OK || fatfs == NULL) {
        shell_sd_end(&session, "fat type");
        return "unknown";
    }
    shell_sd_end(&session, "fat type");

    switch (fatfs->fs_type) {
    case FS_FAT12:
        return "FAT12";
    case FS_FAT16:
        return "FAT16";
    case FS_FAT32:
        return "FAT32";
    default:
        return "unknown";
    }
}

uint64_t storage_get_file_size(const char *resolved_path)
{
    struct stat st;

    if (resolved_path == NULL || resolved_path[0] == '\0') {
        return 0;
    }

    if (shell_sd_stat_path(resolved_path, &st) != ESP_OK) {
        return 0;
    }

    if (!S_ISREG(st.st_mode)) {
        return 0;
    }

    return (uint64_t)st.st_size;
}

bool storage_paths_are_same(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    /* Both sides have already been through shell_fs_resolve_path(), which
     * collapses `.` and `..` and produces one canonical spelling, so a plain
     * comparison is exact. FAT is case-insensitive, so compare that way. */
    return strcasecmp(left, right) == 0;
}

bool storage_check_free_space(uint64_t needed_bytes, uint64_t reclaim_bytes, const char *operation)
{
    storage_space_info_t info;
    char needed_text[32];
    char free_text[32];
    uint64_t effective_free;
    esp_err_t error;

    if (needed_bytes == 0) {
        return true;
    }

    error = storage_get_space_info(&info);
    if (error != ESP_OK) {
        /* Capacity is unknown. Allow the operation rather than blocking a
         * legitimate write on a query failure; the write itself will still
         * report an honest error if the card really is full. */
        shell_record_warningf(operation != NULL ? operation : "storage",
                              "Could not read free space (%s); proceeding without the check",
                              esp_err_to_name(error));
        return true;
    }

    /* Space the destination currently occupies is released by the write, so
     * an in-place overwrite only needs the difference. */
    effective_free = info.free_bytes + reclaim_bytes;

    if (effective_free >= needed_bytes &&
        (effective_free - needed_bytes) >= P4_CONFIG_STORAGE_FREE_MARGIN_BYTES) {
        return true;
    }

    shell_sd_format_size(needed_bytes, needed_text, sizeof(needed_text));
    shell_sd_format_size(effective_free, free_text, sizeof(free_text));

    shell_print_error("%s: not enough free space (need %s, available %s)",
                             operation != NULL ? operation : "storage",
                             needed_text,
                             free_text);
    shell_record_warningf(operation != NULL ? operation : "storage",
                          "Refused a write needing %llu bytes with %llu available",
                          (unsigned long long)needed_bytes,
                          (unsigned long long)effective_free);
    return false;
}

/* ========================================================================
 * INPUT REDIRECTION
 * ======================================================================== */

void storage_set_input_redirect(const char *resolved_path)
{
    if (resolved_path == NULL || resolved_path[0] == '\0') {
        storage_clear_input_redirect();
        return;
    }

    snprintf(s_input_redirect, sizeof(s_input_redirect), "%s", resolved_path);
    s_input_redirect_active = true;
}

const char *storage_get_input_redirect(void)
{
    return s_input_redirect_active ? s_input_redirect : NULL;
}

void storage_clear_input_redirect(void)
{
    s_input_redirect[0] = '\0';
    s_input_redirect_active = false;
}

esp_err_t storage_resolve_input_source(const char *argument, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    output[0] = '\0';

    /* An explicit filename always wins, matching DOS behavior where
     * `sort file.txt < other.txt` reads file.txt. */
    if (argument != NULL && argument[0] != '\0') {
        return shell_fs_resolve_path(argument, output, output_size);
    }

    if (!s_input_redirect_active) {
        return ESP_ERR_NOT_FOUND;
    }

    if (snprintf(output, output_size, "%s", s_input_redirect) >= (int)output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

void storage_init(void)
{
    if (s_initialized) {
        return;
    }

    snprintf(s_shell_cwd, sizeof(s_shell_cwd), "%s", BSP_SD_MOUNT_POINT);
    s_sd_persistent_mounted = false;
    s_sd_ejected = false;
    s_sd_first_mount_fired = false;
    storage_clear_input_redirect();

    s_initialized = true;
    ESP_LOGI(STORAGE_TAG, "Storage module initialized");
}

bool storage_is_initialized(void)
{
    return s_initialized;
}
