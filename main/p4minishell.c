/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file p4minishell.c
 * @brief Networking host bridge implementations.
 *
 * components/networking reaches the shell through a networking_host_ops_t
 * table whose record_* entries take a tag argument. These adapters map that
 * signature onto the variadic shell core API. They live in the app (main)
 * component because ESP-IDF resolves the extern symbols from there.
 *
 * The c6ota and usb bridge functions live in main.c for the same reason.
 */

#include "p4minishell.h"
#include "shell.h"

void shell_networking_record_error(const char *tag, esp_err_t error, const char *message)
{
    shell_record_errorf(tag, error, "%s", message != NULL ? message : "");
}

void shell_networking_schedule_text(const char *text)
{
    shell_schedule_transcript_appendf("%s", text != NULL ? text : "");
}

void shell_networking_record_warning(const char *tag, const char *message)
{
    shell_record_warningf(tag, "%s", message != NULL ? message : "");
}

void shell_networking_record_info(const char *tag, const char *message)
{
    shell_record_infof(tag, "%s", message != NULL ? message : "");
}
