/**
 * @file p4minishell.c
 * @brief Shell utility and host bridge implementations.
 *
 * Contains the networking bridge functions used by components/networking
 * and the storage/SD/file command bridge functions that forward to the
 * storage component.
 */

#include "p4minishell.h"
#include "storage.h"

/* Forward declarations for shell functions in main.c */
extern void shell_schedule_transcript_appendf(const char *format, ...);
extern void shell_record_errorf(const char *tag, int error, const char *format, ...);
extern void shell_record_warningf(const char *tag, const char *format, ...);
extern void shell_record_infof(const char *tag, const char *format, ...);

void shell_networking_record_error(const char *tag, int error, const char *message)
{
    shell_record_errorf(tag, error, "%s", message);
}

void shell_networking_schedule_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text);
}

void shell_networking_record_warning(const char *tag, const char *message)
{
    shell_record_warningf(tag, "%s", message);
}

void shell_networking_record_info(const char *tag, const char *message)
{
    shell_record_infof(tag, "%s", message);
}

void shell_bridge_sd_command(char *command)           { storage_command_sd(command); }
void shell_bridge_cd_command(int argc, char **argv)    { storage_command_cd(argc, argv); }
void shell_bridge_dir_command(int argc, char **argv)   { storage_command_dir(argc, argv); }
void shell_bridge_copy_command(int argc, char **argv)  { storage_command_copy(argc, argv); }
void shell_bridge_move_command(int argc, char **argv)  { storage_command_move(argc, argv); }
void shell_bridge_del_command(int argc, char **argv)   { storage_command_del(argc, argv); }
void shell_bridge_ren_command(int argc, char **argv, const char *verb) { storage_command_rename(argc, argv, verb); }
void shell_bridge_mkdir_command(int argc, char **argv) { storage_command_mkdir(argc, argv); }
void shell_bridge_rmdir_command(int argc, char **argv) { storage_command_rmdir(argc, argv); }
void shell_bridge_type_command(int argc, char **argv)  { storage_command_type_file(argc, argv); }
void shell_bridge_write_command(int argc, char **argv, bool append_mode) { storage_command_write_file(argc, argv, append_mode); }
void shell_bridge_touch_command(int argc, char **argv) { storage_command_touch(argc, argv); }
void shell_bridge_set_command(int argc, char **argv)   { extern void shell_command_set(int argc, char **argv); shell_command_set(argc, argv); }
void shell_bridge_path_command(int argc, char **argv)  { extern void shell_command_path(int argc, char **argv); shell_command_path(argc, argv); }
void shell_bridge_echo_command(int argc, char **argv)  { extern void shell_command_echo(int argc, char **argv); shell_command_echo(argc, argv); }

void shell_bridge_gpio_command(int argc, char **argv)
{
    extern void shell_execute_gpio_command(int argc, char **argv);
    shell_execute_gpio_command(argc, argv);
}

void shell_bridge_call_command(int argc, char **argv)
{
    char batch_path[256];
    if (argc < 2) {
        extern void shell_transcript_append_text(const char *text);
        shell_transcript_append_text("Usage: call <file.bat> [args]\n");
        return;
    }
    if (!storage_resolve_batch_path(argv[1], batch_path, sizeof(batch_path))) {
        extern void shell_transcript_appendf(const char *format, ...);
        shell_transcript_appendf("call: batch file not found %s\n", argv[1]);
        return;
    }
    storage_execute_batch_file(batch_path, argc - 2, &argv[2]);
}

void shell_bridge_batch_file(const char *path, int argc, char **argv)
{
    storage_execute_batch_file(path, argc, argv);
}
