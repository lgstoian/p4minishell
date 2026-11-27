/* Host mock of the shell transcript/print surface used by power verbs. */
#ifndef POWER_GATE_SHELL_H
#define POWER_GATE_SHELL_H
#include <stdarg.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void shell_transcript_appendf_ansi(const char *fmt, ...);
void shell_transcript_appendf(const char *fmt, ...);
void shell_print_usage(const char *fmt, ...);
void shell_print_error(const char *fmt, ...);
void shell_print_warning(const char *fmt, ...);
void shell_print_muted(const char *fmt, ...);
void shell_record_warningf(const char *domain, const char *fmt, ...);
void shell_record_errorf(const char *domain, esp_err_t error, const char *fmt, ...);
bool shell_parse_percentage_arg(const char *text, int *percentage_out);
bool shell_text_equals_ignore_case(const char *left, const char *right);
#ifdef __cplusplus
}
#endif
#endif
