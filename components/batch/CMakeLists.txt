# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "batch.c" "batch_expr.c" "calc.c" "himem.c" "sync.c" "svc.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        storage
        strutil
        modal
        markdown
        filetype
        tui
        clock
        board_bsp
        p4heap
)
