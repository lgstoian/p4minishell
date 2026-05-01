#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <unistd.h>
#include <utime.h>

#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "board_config.h"
#include "p4minishell_config.h"
#include "bsp/esp-bsp.h"
#include "storage.h"
#include "header.h"

#define STORAGE_TAG "storage"
#define STORAGE_FATFS_DRIVE        P4_CONFIG_SD_FATFS_DRIVE
#define STORAGE_PATH_BYTES         P4_CONFIG_SD_PATH_BYTES
#define STORAGE_LIST_LIMIT         P4_CONFIG_SD_LIST_LIMIT
#define STORAGE_CAT_DEFAULT_BYTES  P4_CONFIG_SD_CAT_DEFAULT_BYTES
#define STORAGE_CAT_MAX_BYTES      P4_CONFIG_SD_CAT_MAX_BYTES
#define STORAGE_IO_BUFFER_BYTES    P4_CONFIG_SD_IO_BUFFER_BYTES
#define STORAGE_FILE_IO_BUFFER_BYTES P4_CONFIG_FILE_IO_BUFFER_BYTES
#define STORAGE_BATCH_LINE_BYTES   P4_CONFIG_BATCH_LINE_BYTES
#define STORAGE_BATCH_ARGS_MAX     P4_CONFIG_BATCH_ARGS_MAX
#define STORAGE_BATCH_DEPTH_MAX    P4_CONFIG_BATCH_DEPTH_MAX

static const storage_host_ops_t *g_ops;

static char s_cwd[STORAGE_PATH_BYTES];
static bool s_persistent_mounted;
static bool s_ejected;

static void st_append_text(const char *text)
{
    if (g_ops != NULL && g_ops->transcript_append_text != NULL) {
        g_ops->transcript_append_text(text);
    }
}

__attribute__((format(printf, 1, 2)))
static void st_appendf(const char *format, ...)
{
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    st_append_text(buf);
}

__attribute__((format(printf, 3, 4)))
static void st_record_error(const char *tag, esp_err_t error, const char *format, ...)
{
    char buf[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (g_ops != NULL && g_ops->record_error != NULL) {
        g_ops->record_error(tag, error, buf);
    }
}

__attribute__((format(printf, 2, 3)))
static void st_record_warning(const char *tag, const char *format, ...)
{
    char buf[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (g_ops != NULL && g_ops->record_warning != NULL) {
        g_ops->record_warning(tag, buf);
    }
}

static void st_notify_header(const char *text, uint32_t timeout_ms)
{
    if (g_ops != NULL && g_ops->notify_header != NULL) {
        g_ops->notify_header(text, timeout_ms);
    }
}

static char *st_trim(char *text)
{
    if (text == NULL) return NULL;

    while (isspace((unsigned char)*text)) text++;
    if (*text == '\0') return text;

    char *end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return text;
}

static void st_header_update_sd(int state)
{
    if (g_ops != NULL && g_ops->header_update_sd != NULL) {
        g_ops->header_update_sd(state);
    }
}

static bool st_text_equals_ignore_case(const char *left, const char *right)
{
    if (g_ops != NULL && g_ops->text_equals_ignore_case != NULL) {
        return g_ops->text_equals_ignore_case(left, right);
    }
    return left != NULL && right != NULL && strcasecmp(left, right) == 0;
}

static int st_split_args(char *text, char **argv, int max_args)
{
    if (g_ops != NULL && g_ops->split_args != NULL) {
        return g_ops->split_args(text, argv, max_args);
    }
    return 0;
}

static const char *st_env_get(const char *name)
{
    if (g_ops != NULL && g_ops->env_get != NULL) {
        return g_ops->env_get(name);
    }
    return NULL;
}

void storage_init(const storage_host_ops_t *ops)
{
    g_ops = ops;
}

const char *storage_get_cwd(void)
{
    return s_cwd;
}

void storage_set_cwd_default(void)
{
    snprintf(s_cwd, sizeof(s_cwd), "%s", BSP_SD_MOUNT_POINT);
}

static esp_err_t st_fresult_to_esp_err(FRESULT result)
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

bool storage_is_mounted(void)
{
    return s_persistent_mounted;
}

esp_err_t storage_sd_begin(storage_sd_session_t *session)
{
    esp_err_t error;

    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->mounted_here = false;

    if (s_persistent_mounted) {
        return ESP_OK;
    }

    if (s_ejected) {
        return ESP_ERR_INVALID_STATE;
    }

    error = bsp_sdcard_mount();
    if (error == ESP_OK) {
        session->mounted_here = true;
        s_persistent_mounted = true;
        st_header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    if (error == ESP_ERR_INVALID_STATE) {
        s_persistent_mounted = true;
        st_header_update_sd(HEADER_SD_MOUNTED);
        return ESP_OK;
    }

    return error;
}

void storage_sd_end(storage_sd_session_t *session, const char *operation)
{
    (void)session;
    (void)operation;
}

esp_err_t storage_eject(void)
{
    esp_err_t error;

    error = bsp_sdcard_unmount();
    if (error == ESP_OK) {
        s_persistent_mounted = false;
        s_ejected = true;
        st_append_text("SD card unmounted safely. You may now remove the card.\n");
        st_notify_header("SD card ejected", 3000);
        st_header_update_sd(HEADER_SD_NONE);
    } else if (error == ESP_ERR_INVALID_STATE) {
        st_append_text("SD card is not currently mounted.\n");
    } else {
        st_appendf("SD eject warning: %s (0x%x)\n", esp_err_to_name(error), (unsigned int)error);
        st_record_warning("sd", "Eject unmount warning: %s", esp_err_to_name(error));
    }

    return error;
}

esp_err_t storage_resolve_path(const char *input, char *output, size_t output_size)
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

esp_err_t storage_fs_resolve_path(const char *input, char *output, size_t output_size)
{
    char combined[STORAGE_PATH_BYTES];
    char scratch[STORAGE_PATH_BYTES];
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
        snprintf(output, output_size, "%s", s_cwd[0] != '\0' ? s_cwd : BSP_SD_MOUNT_POINT);
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
    } else if (strcmp(s_cwd, BSP_SD_MOUNT_POINT) == 0) {
        snprintf(combined, sizeof(combined), "/%s", input);
    } else {
        snprintf(combined, sizeof(combined), "%s/%s",
                 s_cwd + strlen(BSP_SD_MOUNT_POINT), input);
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

esp_err_t storage_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size)
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

    written = snprintf(fatfs_path, fatfs_path_size, "%s%s", STORAGE_FATFS_DRIVE, relative_path);
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

esp_err_t storage_stat_path(const char *path, struct stat *st)
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

void storage_format_size(uint64_t size_bytes, char *output, size_t output_size)
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

const char *storage_entry_type(const struct stat *st)
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

bool storage_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out)
{
    char *end = NULL;
    unsigned long parsed_value;

    if (text == NULL || value_out == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed_value = strtoul(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0') {
        return false;
    }

    if (parsed_value < min_value || parsed_value > max_value) {
        return false;
    }

    *value_out = (size_t)parsed_value;
    return true;
}

bool storage_wildcard_match(const char *pattern, const char *name)
{
    if (pattern == NULL || name == NULL) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return true;
            while (*name) {
                if (storage_wildcard_match(pattern, name)) return true;
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

bool storage_path_has_directory_component(const char *path)
{
    return path != NULL && (strchr(path, '/') != NULL || strchr(path, '\\') != NULL);
}

bool storage_path_has_extension(const char *path, const char *extension)
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

    return st_text_equals_ignore_case(path + path_len - ext_len, extension);
}

void storage_join_args(char **argv, int start_index, int argc, char *output, size_t output_size)
{
    int index;
    size_t output_len = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    for (index = start_index; index < argc && output_len + 1 < output_size; index++) {
        size_t arg_len = strlen(argv[index]);
        size_t space = (index > start_index) ? 1 : 0;

        if (output_len + arg_len + space + 1 > output_size) {
            size_t remaining = output_size - output_len - space - 1;
            if (remaining > 0) {
                if (space) output[output_len++] = ' ';
                memcpy(output + output_len, argv[index], remaining);
                output_len += remaining;
            }
            break;
        }

        if (space) output[output_len++] = ' ';
        memcpy(output + output_len, argv[index], arg_len);
        output_len += arg_len;
    }

    output[output_len] = '\0';
}

esp_err_t storage_resolve_target_from_source(const char *source_path,
                                              const char *target_input,
                                              char *target_path,
                                              size_t target_path_size)
{
    char source_dir[STORAGE_PATH_BYTES];
    char *last_sep;

    if (source_path == NULL || target_input == NULL || target_path == NULL || target_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (storage_path_has_directory_component(target_input) ||
        strncmp(target_input, "sd:/", 4) == 0 ||
        target_input[0] == '/') {
        return storage_fs_resolve_path(target_input, target_path, target_path_size);
    }

    snprintf(source_dir, sizeof(source_dir), "%s", source_path);
    last_sep = strrchr(source_dir, '/');
    if (last_sep == NULL || strcmp(source_dir, BSP_SD_MOUNT_POINT) == 0) {
        return storage_fs_resolve_path(target_input, target_path, target_path_size);
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

esp_err_t storage_fs_copy_file(const char *source_path, const char *dest_path)
{
    storage_sd_session_t session;
    FILE *source = NULL;
    FILE *dest = NULL;
    uint8_t buffer[STORAGE_FILE_IO_BUFFER_BYTES];
    esp_err_t error;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    source = fopen(source_path, "rb");
    if (source == NULL) {
        storage_sd_end(&session, "copy");
        return ESP_ERR_NOT_FOUND;
    }

    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        fclose(source);
        storage_sd_end(&session, "copy");
        return ESP_FAIL;
    }

    while (!feof(source)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            fclose(dest);
            fclose(source);
            storage_sd_end(&session, "copy");
            return ESP_FAIL;
        }
    }

    fclose(dest);
    fclose(source);
    storage_sd_end(&session, "copy");
    return ESP_OK;
}

esp_err_t storage_list_directory(const char *normalized_path)
{
    char fatfs_path[STORAGE_PATH_BYTES];
    char lfn[256];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    storage_sd_session_t session;
    bool dir_open = false;
    unsigned int dir_count = 0;
    unsigned int file_count = 0;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = storage_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        storage_sd_end(&session, "dir");
        return error;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        storage_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        st_appendf("%s  %s\n", size_text, normalized_path);
        storage_sd_end(&session, "dir");
        return ESP_OK;
    }

    error = storage_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        storage_sd_end(&session, "dir");
        return error;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        storage_sd_end(&session, "dir");
        return st_fresult_to_esp_err(result);
    }
    dir_open = true;

    st_appendf(" Directory of %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = st_fresult_to_esp_err(result);
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

        if (listed_entries == STORAGE_LIST_LIMIT) {
            st_appendf("dir: listing truncated after %u entries\n", (unsigned int)STORAGE_LIST_LIMIT);
            st_record_warning("dir", "Truncated directory listing for %s", normalized_path);
            error = ESP_OK;
            break;
        }

        if (entry_info.fattrib & AM_DIR) {
            st_appendf("<DIR>      %s\n", lfn);
            dir_count++;
        } else {
            storage_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
            st_appendf("%10s %s\n", size_text, lfn);
            file_count++;
        }
        listed_entries++;
    }

    st_appendf("%u file(s)  %u dir(s)\n", file_count, dir_count);
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    storage_sd_end(&session, "dir");
    return error;
}

esp_err_t storage_print_file_text(const char *normalized_path)
{
    storage_sd_session_t session;
    FILE *file = NULL;
    struct stat path_stat;
    esp_err_t error;
    unsigned char buffer[STORAGE_IO_BUFFER_BYTES + 1];

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    error = storage_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        storage_sd_end(&session, "type");
        return error;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        storage_sd_end(&session, "type");
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        storage_sd_end(&session, "type");
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
        st_appendf("%s", (char *)buffer);
    }

    fclose(file);
    st_append_text("\n");
    storage_sd_end(&session, "type");
    return ESP_OK;
}

esp_err_t storage_write_redirect_output(const char *path, const char *text, bool append_mode)
{
    char resolved_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    error = storage_fs_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        return error;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        storage_sd_end(&session, "redirection");
        return ESP_FAIL;
    }

    if (text != NULL && text[0] != '\0') {
        size_t text_len = strlen(text);
        if (fwrite(text, 1, text_len, file) != text_len) {
            fclose(file);
            storage_sd_end(&session, "redirection");
            return ESP_FAIL;
        }
    }

    fclose(file);
    storage_sd_end(&session, "redirection");
    return ESP_OK;
}

void storage_fs_print_cwd(void)
{
    if (strcmp(s_cwd, BSP_SD_MOUNT_POINT) == 0) {
        st_append_text("\\\n");
        return;
    }

    st_appendf("%s\n", s_cwd + strlen(BSP_SD_MOUNT_POINT));
}

static void st_sd_print_usage(void)
{
    st_append_text("Usage:\n");
    st_append_text("  sd info\n");
    st_append_text("  sd ls [path]\n");
    st_append_text("  sd stat <path>\n");
    st_append_text("  sd cat <path> [max_bytes]\n");
    st_append_text("  sd eject          (safe unmount before card removal)\n");
    st_append_text("  sdeject           (alias for sd eject)\n");
    st_append_text("Paths: sd:/file.txt, /sdcard/file.txt, or relative-to-sd-root\n");
}

void storage_command_sd_eject(void)
{
    (void)storage_eject();
}

static void st_command_sd_info(void)
{
    storage_sd_session_t session;
    esp_err_t error;
    struct stat root_stat;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("sd: SD card not present - insert and retry\n");
        st_record_error("sd", error, "SD info mount failed");
        return;
    }

    st_appendf("sd.mount_point: %s\n", BSP_SD_MOUNT_POINT);
    if (bsp_sdcard != NULL) {
        char capacity_text[32];
        uint64_t capacity_bytes = (uint64_t)bsp_sdcard->csd.capacity * (uint64_t)bsp_sdcard->csd.sector_size;

        storage_format_size(capacity_bytes, capacity_text, sizeof(capacity_text));
        st_appendf("sd.card_name: %s\n", bsp_sdcard->cid.name);
        st_appendf("sd.sector_size: %u\n", (unsigned int)bsp_sdcard->csd.sector_size);
        st_appendf("sd.capacity: %s\n", capacity_text);
        st_appendf("sd.max_freq_khz: %u\n", (unsigned int)bsp_sdcard->max_freq_khz);
    } else {
        st_append_text("sd.card_name: unavailable\n");
    }

    error = storage_stat_path(BSP_SD_MOUNT_POINT, &root_stat);
    if (error == ESP_OK) {
        st_appendf("sd.root: %s\n", storage_entry_type(&root_stat));
    } else {
        st_appendf("sd.root: unavailable (%s)\n", esp_err_to_name(error));
    }

    storage_sd_end(&session, "sd info");
}

static void st_command_sd_ls(char *command)
{
    char *argv[4];
    int argc = st_split_args(command, argv, 4);
    const char *input_path = argc >= 3 ? argv[2] : BSP_SD_MOUNT_POINT;
    char normalized_path[STORAGE_PATH_BYTES];
    char fatfs_path[STORAGE_PATH_BYTES];
    char lfn[256];
    char size_text[32];
    struct stat path_stat;
    FF_DIR dir;
    FILINFO entry_info;
    FRESULT result;
    esp_err_t error;
    size_t listed_entries = 0;
    storage_sd_session_t session;
    bool dir_open = false;

    if (argc > 3) {
        st_sd_print_usage();
        st_record_warning("sd", "Usage error for sd ls command");
        return;
    }

    error = storage_resolve_path(input_path, normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        st_append_text("sd: path is too long or invalid\n");
        st_record_warning("sd", "Rejected invalid path for sd ls");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("sd: SD card not present - insert and retry\n");
        st_record_error("sd", error, "SD card not present or mount failed");
        return;
    }

    error = storage_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        st_appendf("sd: path not found %s\n", normalized_path);
        st_record_error("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISDIR(path_stat.st_mode)) {
        storage_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
        st_appendf("sd: %s type=%s size=%s\n",
                    normalized_path,
                    storage_entry_type(&path_stat),
                    size_text);
        goto cleanup;
    }

    error = storage_vfs_to_fatfs_path(normalized_path, fatfs_path, sizeof(fatfs_path));
    if (error != ESP_OK) {
        st_appendf("sd: invalid FAT path for %s\n", normalized_path);
        st_record_error("sd", error, "Could not convert directory path %s to FatFs path", normalized_path);
        goto cleanup;
    }

    memset(&dir, 0, sizeof(dir));
    memset(&entry_info, 0, sizeof(entry_info));
    result = f_opendir(&dir, fatfs_path);
    if (result != FR_OK) {
        error = st_fresult_to_esp_err(result);
        st_appendf("sd: could not open %s (FatFs=%u)\n", normalized_path, (unsigned int)result);
        st_record_error("sd", error, "Could not open directory %s (FatFs=%u)", normalized_path, (unsigned int)result);
        goto cleanup;
    }
    dir_open = true;

    st_appendf("sd: listing %s\n", normalized_path);
    while (true) {
        result = f_readdir(&dir, &entry_info);
        if (result != FR_OK) {
            error = st_fresult_to_esp_err(result);
            st_appendf("sd: directory read failed for %s (FatFs=%u)\n", normalized_path, (unsigned int)result);
            st_record_error("sd", error, "Directory read failed for %s (FatFs=%u)", normalized_path, (unsigned int)result);
            break;
        }

        if (entry_info.fname[0] == '\0') {
            break;
        }

        snprintf(lfn, sizeof(lfn), "%s", entry_info.fname);
        if (strcmp(lfn, ".") == 0 || strcmp(lfn, "..") == 0) {
            continue;
        }

        if (listed_entries == STORAGE_LIST_LIMIT) {
            st_appendf("sd: listing truncated after %u entries\n", (unsigned int)STORAGE_LIST_LIMIT);
            st_record_warning("sd", "Truncated directory listing for %s", normalized_path);
            break;
        }

        storage_format_size((uint64_t)entry_info.fsize, size_text, sizeof(size_text));
        st_appendf("sd: %-4s %s (%s)\n",
                    (entry_info.fattrib & AM_DIR) ? "DIR" : "FILE",
                    lfn,
                    (entry_info.fattrib & AM_DIR) ? "-" : size_text);
        listed_entries++;
    }

cleanup:
    if (dir_open) {
        (void)f_closedir(&dir);
    }
    storage_sd_end(&session, "sd ls");
}

static void st_command_sd_stat(char *command)
{
    char *argv[4];
    int argc = st_split_args(command, argv, 4);
    char normalized_path[STORAGE_PATH_BYTES];
    char size_text[32];
    struct stat path_stat;
    esp_err_t error;
    storage_sd_session_t session;

    if (argc != 3) {
        st_sd_print_usage();
        st_record_warning("sd", "Usage error for sd stat command");
        return;
    }

    error = storage_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        st_append_text("sd: path is too long or invalid\n");
        st_record_warning("sd", "Rejected invalid path for sd stat");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("sd: SD card not present - insert and retry\n");
        st_record_error("sd", error, "SD stat mount failed");
        return;
    }

    error = storage_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        st_appendf("sd: path not found %s\n", normalized_path);
        st_record_error("sd", error, "Could not stat path %s", normalized_path);
        storage_sd_end(&session, "sd stat");
        return;
    }

    storage_format_size((uint64_t)path_stat.st_size, size_text, sizeof(size_text));
    st_appendf("sd.path: %s\n", normalized_path);
    st_appendf("sd.type: %s\n", storage_entry_type(&path_stat));
    st_appendf("sd.size: %s\n", size_text);
    st_appendf("sd.mode: 0%o\n", (unsigned int)(path_stat.st_mode & 0777));

    storage_sd_end(&session, "sd stat");
}

static void st_command_sd_cat(char *command)
{
    char *argv[5];
    int argc = st_split_args(command, argv, 5);
    char normalized_path[STORAGE_PATH_BYTES];
    struct stat path_stat;
    esp_err_t error;
    storage_sd_session_t session;
    FILE *file = NULL;
    size_t max_bytes = STORAGE_CAT_DEFAULT_BYTES;
    size_t displayed_bytes = 0;
    bool truncated = false;
    bool ended_with_newline = false;
    unsigned char buffer[STORAGE_IO_BUFFER_BYTES + 1];

    if (argc < 3 || argc > 4) {
        st_sd_print_usage();
        st_record_warning("sd", "Usage error for sd cat command");
        return;
    }

    if (argc == 4 && !storage_parse_size_arg(argv[3], 1, STORAGE_CAT_MAX_BYTES, &max_bytes)) {
        st_appendf("sd: max_bytes must be between 1 and %u\n", (unsigned int)STORAGE_CAT_MAX_BYTES);
        st_record_warning("sd", "Rejected invalid max_bytes for sd cat");
        return;
    }

    error = storage_resolve_path(argv[2], normalized_path, sizeof(normalized_path));
    if (error != ESP_OK) {
        st_append_text("sd: path is too long or invalid\n");
        st_record_warning("sd", "Rejected invalid path for sd cat");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("sd: SD card not present - insert and retry\n");
        st_record_error("sd", error, "SD cat mount failed");
        return;
    }

    error = storage_stat_path(normalized_path, &path_stat);
    if (error != ESP_OK) {
        st_appendf("sd: path not found %s\n", normalized_path);
        st_record_error("sd", error, "Could not stat path %s", normalized_path);
        goto cleanup;
    }

    if (!S_ISREG(path_stat.st_mode)) {
        st_appendf("sd: %s is not a regular file\n", normalized_path);
        st_record_warning("sd", "Rejected non-file path for sd cat: %s", normalized_path);
        goto cleanup;
    }

    file = fopen(normalized_path, "rb");
    if (file == NULL) {
        st_appendf("sd: could not open %s (%s)\n", normalized_path, strerror(errno));
        st_record_error("sd", ESP_FAIL, "Could not open file %s", normalized_path);
        goto cleanup;
    }

    st_appendf("sd: preview %s (%u bytes max)\n", normalized_path, (unsigned int)max_bytes);
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

        st_appendf("%s", (char *)buffer);
        displayed_bytes += bytes_read;
        if (bytes_read < bytes_to_read) {
            break;
        }
    }

    if (displayed_bytes == 0) {
        st_append_text("sd: file is empty\n");
    } else if (!feof(file)) {
        truncated = true;
    }

    if (displayed_bytes > 0 && !ended_with_newline) {
        st_append_text("\n");
    }

    if (truncated) {
        st_appendf("sd: preview truncated at %u bytes\n", (unsigned int)max_bytes);
    }

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    storage_sd_end(&session, "sd cat");
}

void storage_command_sd(char *command)
{
    char *argv[5];
    int argc = st_split_args(command, argv, 5);

    if (argc <= 1) {
        st_command_sd_info();
        st_sd_print_usage();
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        st_sd_print_usage();
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc != 2) {
            st_sd_print_usage();
            st_record_warning("sd", "Usage error for sd info command");
            return;
        }
        st_command_sd_info();
        return;
    }

    if (strcmp(argv[1], "ls") == 0) {
        st_command_sd_ls(command);
        return;
    }

    if (strcmp(argv[1], "stat") == 0) {
        st_command_sd_stat(command);
        return;
    }

    if (strcmp(argv[1], "cat") == 0) {
        st_command_sd_cat(command);
        return;
    }

    if (strcmp(argv[1], "eject") == 0) {
        storage_command_sd_eject();
        return;
    }

    st_sd_print_usage();
    st_record_warning("sd", "Unknown sd subcommand: %s", argv[1]);
}

void storage_command_cd(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    struct stat path_stat;
    storage_sd_session_t session;
    esp_err_t error;

    if (argc == 1) {
        storage_fs_print_cwd();
        return;
    }

    if (argc != 2) {
        st_append_text("Usage: cd <path>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("cd: invalid path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("cd: SD card not present - insert and retry\n");
        return;
    }

    error = storage_stat_path(resolved_path, &path_stat);
    if (error != ESP_OK || !S_ISDIR(path_stat.st_mode)) {
        st_appendf("cd: path not found %s\n", argv[1]);
        storage_sd_end(&session, "cd");
        return;
    }

    storage_sd_end(&session, "cd");

    snprintf(s_cwd, sizeof(s_cwd), "%s", resolved_path);
    storage_fs_print_cwd();
}

void storage_command_dir(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    char dir_part[STORAGE_PATH_BYTES];
    const char *pattern = NULL;
    esp_err_t error;

    if (argc > 2) {
        st_append_text("Usage: dir [path|pattern]\n");
        return;
    }

    if (argc == 2 && (strchr(argv[1], '*') != NULL || strchr(argv[1], '?') != NULL)) {
        const char *last_sep = strrchr(argv[1], '/');
        if (last_sep == NULL) last_sep = strrchr(argv[1], '\\');
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[1]);
            if (dir_len >= sizeof(dir_part)) dir_len = sizeof(dir_part) - 1;
            memcpy(dir_part, argv[1], dir_len);
            dir_part[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(dir_part, sizeof(dir_part), ".");
            pattern = argv[1];
        }
        error = storage_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
    } else {
        error = storage_fs_resolve_path(argc == 2 ? argv[1] : NULL, resolved_path, sizeof(resolved_path));
    }

    if (error != ESP_OK) {
        st_append_text("dir: invalid path\n");
        return;
    }

    if (pattern != NULL) {
        DIR *d = opendir(resolved_path);
        if (d == NULL) {
            st_appendf("dir: cannot open %s\n", resolved_path);
            return;
        }
        struct dirent *entry;
        int count = 0;
        int dirs = 0, files = 0;
        st_appendf("\n Directory of %s\n\n", resolved_path);
        while ((entry = readdir(d)) != NULL && count < STORAGE_LIST_LIMIT) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            if (!storage_wildcard_match(pattern, entry->d_name))
                continue;
            char fp[STORAGE_PATH_BYTES + 256];
            snprintf(fp, sizeof(fp), "%s/%s", resolved_path, entry->d_name);
            struct stat st;
            if (stat(fp, &st) != 0) continue;
            char size_str[16];
            if (S_ISDIR(st.st_mode)) {
                snprintf(size_str, sizeof(size_str), "<DIR>");
                dirs++;
            } else {
                storage_format_size((uint64_t)st.st_size, size_str, sizeof(size_str));
                files++;
            }
            FILINFO fi;
            char sfn[16] = "";
            if (f_stat(fp, &fi) == FR_OK && fi.altname[0] != '\0') {
                char upper[256];
                snprintf(upper, sizeof(upper), "%s", entry->d_name);
                for (char *p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
                if (strcmp(fi.altname, upper) != 0)
                    snprintf(sfn, sizeof(sfn), " [%s]", fi.altname);
            }
            st_appendf("  %8s  %s%s\n", size_str, entry->d_name, sfn);
            count++;
        }
        closedir(d);
        st_appendf("       %d File(s)\n", files);
        st_appendf("       %d Dir(s)\n", dirs);
        return;
    }

    error = storage_list_directory(resolved_path);
    if (error == ESP_ERR_NOT_FOUND) {
        st_appendf("dir: path not found %s\n", resolved_path);
    } else if (error != ESP_OK) {
        st_appendf("dir: failed to read %s (%s)\n", resolved_path, esp_err_to_name(error));
    }
}

void storage_command_copy(int argc, char **argv)
{
    char source_path[STORAGE_PATH_BYTES];
    char dest_path[STORAGE_PATH_BYTES];
    char src_dir[STORAGE_PATH_BYTES];
    const char *pattern = NULL;
    esp_err_t error;

    if (argc != 3) {
        st_append_text("Usage: copy <source|pattern> <destination>\n");
        return;
    }

    if (strchr(argv[1], '*') != NULL || strchr(argv[1], '?') != NULL) {
        const char *last_sep = strrchr(argv[1], '/');
        if (last_sep == NULL) last_sep = strrchr(argv[1], '\\');
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[1]);
            if (dir_len >= sizeof(src_dir)) dir_len = sizeof(src_dir) - 1;
            memcpy(src_dir, argv[1], dir_len);
            src_dir[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(src_dir, sizeof(src_dir), ".");
            pattern = argv[1];
        }
        error = storage_fs_resolve_path(src_dir, source_path, sizeof(source_path));
        if (error != ESP_OK) {
            st_append_text("copy: invalid source path\n");
            return;
        }
        error = storage_fs_resolve_path(argv[2], dest_path, sizeof(dest_path));
        if (error != ESP_OK) {
            st_append_text("copy: invalid destination path\n");
            return;
        }

        struct stat dst_st;
        if (stat(dest_path, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode)) {
            st_append_text("copy: destination must be a directory for wildcard copy\n");
            return;
        }

        DIR *d = opendir(source_path);
        if (d == NULL) {
            st_appendf("copy: cannot open %s\n", source_path);
            return;
        }
        struct dirent *entry;
        int copied = 0;
        while ((entry = readdir(d)) != NULL) {
            if (!storage_wildcard_match(pattern, entry->d_name))
                continue;
            char sf[STORAGE_PATH_BYTES + 256], df[STORAGE_PATH_BYTES + 256];
            snprintf(sf, sizeof(sf), "%s/%s", source_path, entry->d_name);
            snprintf(df, sizeof(df), "%s/%s", dest_path, entry->d_name);
            struct stat cs;
            if (stat(sf, &cs) != 0 || S_ISDIR(cs.st_mode))
                continue;
            if (storage_fs_copy_file(sf, df) == ESP_OK) {
                st_appendf("  %s\n", entry->d_name);
                copied++;
            } else {
                st_appendf("  %s (failed)\n", entry->d_name);
            }
        }
        closedir(d);
        st_appendf("copy: %d file(s) copied\n", copied);
        return;
    }

    error = storage_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        st_append_text("copy: invalid source path\n");
        return;
    }

    error = storage_fs_resolve_path(argv[2], dest_path, sizeof(dest_path));
    if (error != ESP_OK) {
        st_append_text("copy: invalid destination path\n");
        return;
    }

    error = storage_fs_copy_file(source_path, dest_path);
    if (error != ESP_OK) {
        st_appendf("copy: failed (%s)\n", esp_err_to_name(error));
        return;
    }

    st_appendf("1 file(s) copied to %s\n", dest_path);
}

void storage_command_del(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    char dir_part[STORAGE_PATH_BYTES];
    const char *pattern = NULL;
    storage_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        st_append_text("Usage: del <path|pattern>\n");
        return;
    }

    if (strchr(argv[1], '*') != NULL || strchr(argv[1], '?') != NULL) {
        const char *last_sep = strrchr(argv[1], '/');
        if (last_sep == NULL) last_sep = strrchr(argv[1], '\\');
        if (last_sep != NULL) {
            size_t dir_len = (size_t)(last_sep - argv[1]);
            if (dir_len >= sizeof(dir_part)) dir_len = sizeof(dir_part) - 1;
            memcpy(dir_part, argv[1], dir_len);
            dir_part[dir_len] = '\0';
            pattern = last_sep + 1;
        } else {
            snprintf(dir_part, sizeof(dir_part), ".");
            pattern = argv[1];
        }
        error = storage_fs_resolve_path(dir_part, resolved_path, sizeof(resolved_path));
        if (error != ESP_OK) {
            st_append_text("del: invalid path\n");
            return;
        }

        error = storage_sd_begin(&session);
        if (error != ESP_OK) {
            st_append_text("del: SD card not present\n");
            return;
        }

        DIR *d = opendir(resolved_path);
        if (d == NULL) {
            st_appendf("del: cannot open %s\n", resolved_path);
            storage_sd_end(&session, "del");
            return;
        }
        struct dirent *entry;
        int deleted = 0;
        while ((entry = readdir(d)) != NULL) {
            if (!storage_wildcard_match(pattern, entry->d_name))
                continue;
            char fp[STORAGE_PATH_BYTES + 256];
            snprintf(fp, sizeof(fp), "%s/%s", resolved_path, entry->d_name);
            if (unlink(fp) == 0) {
                st_appendf("  Deleted %s\n", entry->d_name);
                deleted++;
            } else {
                st_appendf("  Failed: %s (%s)\n", entry->d_name, strerror(errno));
            }
        }
        closedir(d);
        st_appendf("del: %d file(s) deleted\n", deleted);
        storage_sd_end(&session, "del");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("del: invalid path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("del: SD card not present - insert and retry\n");
        return;
    }

    if (unlink(resolved_path) != 0) {
        st_appendf("del: failed to delete %s (%s)\n", resolved_path, strerror(errno));
        storage_sd_end(&session, "del");
        return;
    }

    storage_sd_end(&session, "del");
    st_appendf("Deleted %s\n", resolved_path);
}

void storage_command_rename(int argc, char **argv, const char *verb)
{
    char source_path[STORAGE_PATH_BYTES];
    char target_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        st_appendf("Usage: %s <source> <destination>\n", verb);
        return;
    }

    error = storage_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        st_appendf("%s: invalid source path\n", verb);
        return;
    }

    error = storage_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        st_appendf("%s: invalid destination path\n", verb);
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_appendf("%s: SD card not present - insert and retry\n", verb);
        return;
    }

    if (rename(source_path, target_path) != 0) {
        st_appendf("%s: failed (%s)\n", verb, strerror(errno));
        storage_sd_end(&session, verb);
        return;
    }

    storage_sd_end(&session, verb);
    st_appendf("%s -> %s\n", source_path, target_path);
}

void storage_command_mkdir(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        st_append_text("Usage: mkdir <path>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("mkdir: invalid path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("mkdir: SD card not present - insert and retry\n");
        return;
    }

    if (mkdir(resolved_path, 0775) != 0) {
        st_appendf("mkdir: failed to create %s (%s)\n", resolved_path, strerror(errno));
        storage_sd_end(&session, "mkdir");
        return;
    }

    storage_sd_end(&session, "mkdir");
    st_appendf("Created directory %s\n", resolved_path);
}

void storage_command_rmdir(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    esp_err_t error;

    if (argc != 2) {
        st_append_text("Usage: rmdir <path>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("rmdir: invalid path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("rmdir: SD card not present - insert and retry\n");
        return;
    }

    if (rmdir(resolved_path) != 0) {
        st_appendf("rmdir: failed to remove %s (%s)\n", resolved_path, strerror(errno));
        storage_sd_end(&session, "rmdir");
        return;
    }

    storage_sd_end(&session, "rmdir");
    st_appendf("Removed directory %s\n", resolved_path);
}

void storage_command_type_file(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    esp_err_t error;

    if (argc != 2) {
        st_append_text("Usage: type <path>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("type: invalid path\n");
        return;
    }

    error = storage_print_file_text(resolved_path);
    if (error == ESP_ERR_INVALID_ARG) {
        st_appendf("type: %s is not a regular text file\n", resolved_path);
    } else if (error != ESP_OK) {
        st_appendf("type: failed to read %s (%s)\n", resolved_path, esp_err_to_name(error));
    }
}

void storage_command_write_file(int argc, char **argv, bool append_mode)
{
    char resolved_path[STORAGE_PATH_BYTES];
    char text[STORAGE_BATCH_LINE_BYTES];
    storage_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc < 3) {
        st_appendf("Usage: %s <path> <text>\n", append_mode ? "append" : "write");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_appendf("%s: invalid path\n", append_mode ? "append" : "write");
        return;
    }

    storage_join_args(argv, 2, argc, text, sizeof(text));
    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_appendf("%s: SD card not present - insert and retry\n", append_mode ? "append" : "write");
        return;
    }

    file = fopen(resolved_path, append_mode ? "ab" : "wb");
    if (file == NULL) {
        st_appendf("%s: failed to open %s (%s)\n",
                    append_mode ? "append" : "write",
                    resolved_path,
                    strerror(errno));
        storage_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    if (fwrite(text, 1, strlen(text), file) != strlen(text) || fwrite("\n", 1, 1, file) != 1) {
        st_appendf("%s: failed while writing %s\n", append_mode ? "append" : "write", resolved_path);
        fclose(file);
        storage_sd_end(&session, append_mode ? "append" : "write");
        return;
    }

    fclose(file);
    storage_sd_end(&session, append_mode ? "append" : "write");
    st_appendf("%s: %s\n", append_mode ? "Appended" : "Wrote", resolved_path);
}

void storage_command_touch(int argc, char **argv)
{
    char resolved_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    if (argc != 2) {
        st_append_text("Usage: touch <path>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], resolved_path, sizeof(resolved_path));
    if (error != ESP_OK) {
        st_append_text("touch: invalid path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("touch: SD card not present - insert and retry\n");
        return;
    }

    file = fopen(resolved_path, "ab");
    if (file == NULL) {
        st_appendf("touch: failed to open %s (%s)\n", resolved_path, strerror(errno));
        storage_sd_end(&session, "touch");
        return;
    }

    fclose(file);
    (void)utime(resolved_path, NULL);
    storage_sd_end(&session, "touch");
    st_appendf("Touched %s\n", resolved_path);
}

void storage_command_move(int argc, char **argv)
{
    char source_path[STORAGE_PATH_BYTES];
    char target_path[STORAGE_PATH_BYTES];
    storage_sd_session_t session;
    esp_err_t error;

    if (argc != 3) {
        st_append_text("Usage: move <source> <destination>\n");
        return;
    }

    error = storage_fs_resolve_path(argv[1], source_path, sizeof(source_path));
    if (error != ESP_OK) {
        st_append_text("move: invalid source path\n");
        return;
    }

    error = storage_resolve_target_from_source(source_path, argv[2], target_path, sizeof(target_path));
    if (error != ESP_OK) {
        st_append_text("move: invalid destination path\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("move: SD card not present - insert and retry\n");
        return;
    }

    if (rename(source_path, target_path) == 0) {
        storage_sd_end(&session, "move");
        st_appendf("Moved %s -> %s\n", source_path, target_path);
        return;
    }

    storage_sd_end(&session, "move");
    error = storage_fs_copy_file(source_path, target_path);
    if (error != ESP_OK) {
        st_appendf("move: failed to copy %s (%s)\n", source_path, esp_err_to_name(error));
        return;
    }

    error = storage_sd_begin(&session);
    if (error == ESP_OK) {
        if (unlink(source_path) != 0) {
            st_appendf("move: warning, copied but could not remove %s (%s)\n",
                        source_path, strerror(errno));
        }
        storage_sd_end(&session, "move");
    }
    st_appendf("Moved %s -> %s\n", source_path, target_path);
}

void storage_command_attrib(int argc, char **argv)
{
    storage_sd_session_t session;
    esp_err_t error;
    char resolved[STORAGE_PATH_BYTES];
    FILINFO info;
    FRESULT fr;
    BYTE attr = 0;
    bool set_attr = false;
    char attr_char = 0;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("attrib: SD card not present\n");
        return;
    }

    if (argc >= 2 && (argv[1][0] == '+' || argv[1][0] == '-') &&
        argv[1][1] != '\0' && argv[1][2] == '\0') {
        set_attr = (argv[1][0] == '+');
        attr_char = (char)toupper((unsigned char)argv[1][1]);

        switch (attr_char) {
        case 'R': attr = AM_RDO; break;
        case 'H': attr = AM_HID; break;
        case 'S': attr = AM_SYS; break;
        case 'A': attr = AM_ARC; break;
        default:
            st_append_text("attrib: invalid attribute. Use R, H, S, or A\n");
            storage_sd_end(&session, "attrib");
            return;
        }

        if (argc < 3) {
            st_append_text("attrib: path required when setting/clearing attributes\n");
            storage_sd_end(&session, "attrib");
            return;
        }

        error = storage_resolve_path(argv[2], resolved, sizeof(resolved));
        if (error != ESP_OK) {
            st_append_text("attrib: invalid path\n");
            storage_sd_end(&session, "attrib");
            return;
        }

        fr = f_stat(resolved, &info);
        if (fr != FR_OK) {
            st_appendf("attrib: cannot access %s\n", argv[2]);
            storage_sd_end(&session, "attrib");
            return;
        }

        if (set_attr) info.fattrib |= attr;
        else           info.fattrib &= ~attr;

        fr = f_chmod(resolved, info.fattrib, AM_RDO | AM_HID | AM_SYS | AM_ARC);
        if (fr != FR_OK) {
            st_append_text("attrib: failed to change attributes\n");
        } else {
            st_appendf("attrib: %c%c set for %s\n",
                         set_attr ? '+' : '-', attr_char, argv[2]);
        }
        storage_sd_end(&session, "attrib");
        return;
    }

    const char *path = (argc >= 2) ? argv[1] : ".";
    error = storage_resolve_path(path, resolved, sizeof(resolved));
    if (error != ESP_OK) {
        st_append_text("attrib: invalid path\n");
        storage_sd_end(&session, "attrib");
        return;
    }

    fr = f_stat(resolved, &info);
    if (fr == FR_OK && !(info.fattrib & AM_DIR)) {
        char a[5] = {
            (info.fattrib & AM_RDO) ? 'R' : '-',
            (info.fattrib & AM_HID) ? 'H' : '-',
            (info.fattrib & AM_SYS) ? 'S' : '-',
            (info.fattrib & AM_ARC) ? 'A' : '-', 0
        };
        st_appendf("  %s  %s\n", a, info.fname);
    } else {
        DIR *d = opendir(resolved);
        if (d == NULL) {
            st_appendf("attrib: cannot access %s\n", path);
            storage_sd_end(&session, "attrib");
            return;
        }
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(d)) != NULL && count < STORAGE_LIST_LIMIT) {
            char fp[STORAGE_PATH_BYTES + 256];
            snprintf(fp, sizeof(fp), "%s/%s", resolved, entry->d_name);
            if (f_stat(fp, &info) == FR_OK) {
                char a[5] = {
                    (info.fattrib & AM_RDO) ? 'R' : '-',
                    (info.fattrib & AM_HID) ? 'H' : '-',
                    (info.fattrib & AM_SYS) ? 'S' : '-',
                    (info.fattrib & AM_ARC) ? 'A' : '-', 0
                };
                st_appendf("  %s  %s  %s\n", a,
                    (info.fattrib & AM_DIR) ? "<DIR>" : "     ",
                    entry->d_name);
                count++;
            }
        }
        closedir(d);
    }
    storage_sd_end(&session, "attrib");
}

void storage_command_label(int argc, char **argv)
{
    storage_sd_session_t session;
    esp_err_t error;
    char label[12];
    FRESULT fr;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("label: SD card not present\n");
        return;
    }

    if (argc >= 2) {
        if (strlen(argv[1]) > 11) {
            st_append_text("label: volume name must be 11 characters or fewer\n");
            storage_sd_end(&session, "label");
            return;
        }
        memset(label, ' ', 11);
        label[11] = '\0';
        size_t len = strlen(argv[1]);
        for (size_t i = 0; i < len && i < 11; i++)
            label[i] = (char)toupper((unsigned char)argv[1][i]);
        fr = f_setlabel(label);
        if (fr == FR_OK)
            st_appendf("label: volume label set to \"%s\"\n", argv[1]);
        else
            st_append_text("label: failed to set volume label\n");
    } else {
        fr = f_getlabel("0:", label, NULL);
        if (fr == FR_OK) {
            int end = 10;
            while (end >= 0 && label[end] == ' ') end--;
            label[end + 1] = '\0';
            st_appendf("label: %s\n", label[0] ? label : "(no volume label)");
        } else {
            st_append_text("label: failed to read volume label\n");
        }
    }
    storage_sd_end(&session, "label");
}

void storage_command_xcopy(int argc, char **argv)
{
    storage_sd_session_t session;
    esp_err_t error;
    char src_resolved[STORAGE_PATH_BYTES];
    char dst_resolved[STORAGE_PATH_BYTES];
    bool recursive = false;
    const char *src_arg = NULL;
    const char *dst_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && toupper((unsigned char)argv[i][1]) == 'S')
            recursive = true;
        else if (src_arg == NULL) src_arg = argv[i];
        else if (dst_arg == NULL) dst_arg = argv[i];
    }
    if (src_arg == NULL || dst_arg == NULL) {
        st_append_text("Usage: xcopy <source> <destination> [/S]\n");
        st_append_text("  /S  Copy directories and subdirectories\n");
        return;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("xcopy: SD card not present\n");
        return;
    }

    error = storage_resolve_path(src_arg, src_resolved, sizeof(src_resolved));
    if (error != ESP_OK) {
        st_append_text("xcopy: invalid source path\n");
        storage_sd_end(&session, "xcopy");
        return;
    }
    error = storage_resolve_path(dst_arg, dst_resolved, sizeof(dst_resolved));
    if (error != ESP_OK) {
        st_append_text("xcopy: invalid destination path\n");
        storage_sd_end(&session, "xcopy");
        return;
    }

    struct stat src_st;
    if (stat(src_resolved, &src_st) != 0) {
        st_appendf("xcopy: source not found: %s\n", src_arg);
        storage_sd_end(&session, "xcopy");
        return;
    }

    if (S_ISDIR(src_st.st_mode)) {
        if (!recursive) {
            st_append_text("xcopy: use /S to copy directories\n");
            storage_sd_end(&session, "xcopy");
            return;
        }
        struct stat dst_st;
        if (stat(dst_resolved, &dst_st) != 0) mkdir(dst_resolved, 0755);

        DIR *d = opendir(src_resolved);
        if (d == NULL) {
            st_append_text("xcopy: cannot open source directory\n");
            storage_sd_end(&session, "xcopy");
            return;
        }
        struct dirent *entry;
        int copied = 0;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char sc[STORAGE_PATH_BYTES + 256], dc[STORAGE_PATH_BYTES + 256];
            snprintf(sc, sizeof(sc), "%s/%s", src_resolved, entry->d_name);
            snprintf(dc, sizeof(dc), "%s/%s", dst_resolved, entry->d_name);
            struct stat cs;
            if (stat(sc, &cs) != 0) continue;
            if (S_ISDIR(cs.st_mode)) {
                char *xa[4] = { "xcopy", sc, dc, "/S" };
                storage_command_xcopy(4, xa);
            } else {
                if (storage_fs_copy_file(sc, dc) == ESP_OK) {
                    st_appendf("  %s\n", entry->d_name);
                    copied++;
                } else {
                    st_appendf("  %s (failed)\n", entry->d_name);
                }
            }
        }
        closedir(d);
        st_appendf("xcopy: %d file(s) copied\n", copied);
    } else {
        error = storage_fs_copy_file(src_resolved, dst_resolved);
        if (error == ESP_OK)
            st_append_text("xcopy: 1 file copied\n");
        else
            st_appendf("xcopy: copy failed (%s)\n", esp_err_to_name(error));
    }
    storage_sd_end(&session, "xcopy");
}

bool storage_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size)
{
    char candidate[STORAGE_PATH_BYTES];
    char path_copy[P4_CONFIG_ENV_VALUE_BYTES];
    char *entry;
    char *context = NULL;
    struct stat st;
    storage_sd_session_t session;
    esp_err_t error;
    const char *path_env = st_env_get("PATH");

    if (command_name == NULL || command_name[0] == '\0' || resolved_path == NULL || resolved_path_size == 0) {
        return false;
    }

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        return false;
    }

    error = storage_fs_resolve_path(command_name, candidate, sizeof(candidate));
    if (error == ESP_OK && storage_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
        snprintf(resolved_path, resolved_path_size, "%s", candidate);
        storage_sd_end(&session, "call");
        return true;
    }

    if (!storage_path_has_extension(command_name, ".bat")) {
        char with_ext[STORAGE_PATH_BYTES];

        snprintf(with_ext, sizeof(with_ext), "%s.bat", command_name);
        error = storage_fs_resolve_path(with_ext, candidate, sizeof(candidate));
        if (error == ESP_OK && storage_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
            snprintf(resolved_path, resolved_path_size, "%s", candidate);
            storage_sd_end(&session, "call");
            return true;
        }
    }

    if (path_env != NULL && path_env[0] != '\0' && !storage_path_has_directory_component(command_name)) {
        snprintf(path_copy, sizeof(path_copy), "%s", path_env);
        entry = strtok_r(path_copy, ";", &context);
        while (entry != NULL) {
            char dir_path[STORAGE_PATH_BYTES];

            if (storage_fs_resolve_path(entry, dir_path, sizeof(dir_path)) == ESP_OK) {
                int written = snprintf(candidate, sizeof(candidate), "%s/%s", dir_path, command_name);
                if (written > 0 && (size_t)written < sizeof(candidate) &&
                    storage_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                    snprintf(resolved_path, resolved_path_size, "%s", candidate);
                    storage_sd_end(&session, "call");
                    return true;
                }

                if (!storage_path_has_extension(command_name, ".bat")) {
                    written = snprintf(candidate, sizeof(candidate), "%s/%s.bat", dir_path, command_name);
                    if (written > 0 && (size_t)written < sizeof(candidate) &&
                        storage_stat_path(candidate, &st) == ESP_OK && S_ISREG(st.st_mode)) {
                        snprintf(resolved_path, resolved_path_size, "%s", candidate);
                        storage_sd_end(&session, "call");
                        return true;
                    }
                }
            }

            entry = strtok_r(NULL, ";", &context);
        }
    }

    storage_sd_end(&session, "call");
    return false;
}

esp_err_t storage_execute_batch_file(const char *path, int argc, char **argv)
{
    storage_sd_session_t session;
    FILE *file = NULL;
    esp_err_t error;

    error = storage_sd_begin(&session);
    if (error != ESP_OK) {
        st_append_text("call: SD card not present - insert and retry\n");
        return error;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        storage_sd_end(&session, "call");
        return ESP_ERR_NOT_FOUND;
    }

    while (!feof(file)) {
        char line[STORAGE_BATCH_LINE_BYTES];
        if (fgets(line, sizeof(line), file) == NULL) {
            break;
        }
        char *trimmed = st_trim(line);
        size_t line_len = strlen(trimmed);
        while (line_len > 0 && (trimmed[line_len - 1] == '\n' || trimmed[line_len - 1] == '\r')) {
            trimmed[--line_len] = '\0';
        }
        if (trimmed[0] == '@') {
            trimmed = st_trim(trimmed + 1);
        }
        if (trimmed[0] == '\0') {
            continue;
        }
        if (g_ops != NULL && g_ops->execute_command != NULL) {
            g_ops->execute_command(trimmed);
        }
    }

    fclose(file);
    storage_sd_end(&session, "call");
    return ESP_OK;
}
