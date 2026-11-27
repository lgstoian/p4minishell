/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file archive_format_internal.h
 * @brief Shared USTAR field offsets for archive.c and the pure format core.
 *
 * Internal to components/archive. Keeping the offsets in one header prevents
 * the stream reader and the memory scanner from drifting apart.
 */
#ifndef P4MINISHELL_ARCHIVE_FORMAT_INTERNAL_H
#define P4MINISHELL_ARCHIVE_FORMAT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define ARCHIVE_BLOCK 512

#define USTAR_NAME 0
#define USTAR_NAME_LEN 100
#define USTAR_MODE 100
#define USTAR_UID 108
#define USTAR_SIZE 124
#define USTAR_SIZE_LEN 12
#define USTAR_MTIME 136
#define USTAR_CHKSUM 148
#define USTAR_CHKSUM_LEN 8
#define USTAR_TYPE 156
#define USTAR_MAGIC 257
#define USTAR_PREFIX 345
#define USTAR_PREFIX_LEN 155

/** Bounded path copy used by both the stream and memory cores. */
void archive_copy_path(char *dst, size_t dst_size, const char *src);

#endif /* P4MINISHELL_ARCHIVE_FORMAT_INTERNAL_H */
