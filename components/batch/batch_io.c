/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file batch_io.c
 * @brief Holder for the batch verbs' I/O backend table (see batch_io.h).
 *
 * Hardware-free by construction: this file only stores a pointer and reads it
 * back, so the host unit-test lane links it and the firmware installs the real
 * table from `batch_init()`. There is no fallback implementation here and no
 * default that pretends a card or a key source exists.
 *
 * @module batch
 * @layer batch
 * @domain core
 * @provides batch I/O backend registration + lookup
 * @status stable
 * @test test/main/test_batch_expr.c, test/main/test_svc.c
 * @hard-rule no FATFS, VFS, display or board dependency - a pointer only
 */

#include "batch_io.h"

/* Installed by batch_init() on the board; NULL on the host lane and before
 * batch_init(). Boot-lived, never freed. */
static const batch_io_ops_t *s_io_ops = NULL;

void batch_register_io_ops(const batch_io_ops_t *ops)
{
    s_io_ops = ops;
}

const batch_io_ops_t *batch_io_ops_get(void)
{
    return s_io_ops;
}
