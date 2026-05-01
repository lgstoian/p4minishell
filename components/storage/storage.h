#ifndef P4MINISHELL_STORAGE_H
#define P4MINISHELL_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted_here;
} storage_sd_session_t;

typedef struct {
    void (*transcript_append_text)(const char *text);
    void (*schedule_transcript_append_text)(const char *text);
    void (*record_error)(const char *tag, esp_err_t error, const char *message);
    void (*record_warning)(const char *tag, const char *message);
    void (*record_info)(const char *tag, const char *message);
    void (*notify_header)(const char *text, uint32_t timeout_ms);
    void (*header_update_sd)(int state);
    bool (*text_equals_ignore_case)(const char *left, const char *right);
    int (*split_args)(char *text, char **argv, int max_args);
    const char *(*env_get)(const char *name);
    void (*execute_command)(const char *command);
} storage_host_ops_t;

void storage_init(const storage_host_ops_t *ops);

const char *storage_get_cwd(void);
void storage_set_cwd_default(void);

bool storage_is_mounted(void) __attribute__((warn_unused_result));

esp_err_t storage_sd_begin(storage_sd_session_t *session);
void storage_sd_end(storage_sd_session_t *session, const char *operation);
esp_err_t storage_eject(void);

esp_err_t storage_resolve_path(const char *input, char *output, size_t output_size);
esp_err_t storage_fs_resolve_path(const char *input, char *output, size_t output_size);
esp_err_t storage_vfs_to_fatfs_path(const char *vfs_path, char *fatfs_path, size_t fatfs_path_size);

esp_err_t storage_stat_path(const char *path, struct stat *st);
void storage_format_size(uint64_t size_bytes, char *output, size_t output_size);
const char *storage_entry_type(const struct stat *st);
bool storage_parse_size_arg(const char *text, size_t min_value, size_t max_value, size_t *value_out);

bool storage_wildcard_match(const char *pattern, const char *name);
bool storage_path_has_directory_component(const char *path);
bool storage_path_has_extension(const char *path, const char *extension);
void storage_join_args(char **argv, int start_index, int argc, char *output, size_t output_size);

esp_err_t storage_fs_copy_file(const char *source_path, const char *dest_path);
esp_err_t storage_list_directory(const char *normalized_path);
esp_err_t storage_print_file_text(const char *normalized_path);
esp_err_t storage_write_redirect_output(const char *path, const char *text, bool append_mode);
esp_err_t storage_resolve_target_from_source(const char *source_path, const char *target_input,
                                              char *target_path, size_t target_path_size);

void storage_fs_print_cwd(void);

void storage_command_sd(char *command);
void storage_command_sd_eject(void);

void storage_command_cd(int argc, char **argv);
void storage_command_dir(int argc, char **argv);
void storage_command_copy(int argc, char **argv);
void storage_command_del(int argc, char **argv);
void storage_command_rename(int argc, char **argv, const char *verb);
void storage_command_mkdir(int argc, char **argv);
void storage_command_rmdir(int argc, char **argv);
void storage_command_type_file(int argc, char **argv);
void storage_command_write_file(int argc, char **argv, bool append_mode);
void storage_command_touch(int argc, char **argv);
void storage_command_move(int argc, char **argv);

void storage_command_attrib(int argc, char **argv);
void storage_command_label(int argc, char **argv);
void storage_command_xcopy(int argc, char **argv);

esp_err_t storage_execute_batch_file(const char *path, int argc, char **argv);
bool storage_resolve_batch_path(const char *command_name, char *resolved_path, size_t resolved_path_size);

#ifdef __cplusplus
}
#endif

#endif
