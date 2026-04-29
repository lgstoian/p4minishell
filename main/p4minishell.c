/**
 * @file p4minishell.c
 * @brief Shell utility and host bridge implementations.
 *
 * Contains the networking bridge functions used by components/networking.
 * The c6ota and usb bridge functions remain in main.c because ESP-IDF's
 * component model requires them to be in the app (main) component for
 * cross-component linking.
 */

#include "p4minishell.h"

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
