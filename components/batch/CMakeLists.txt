# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "batch.c" "batch_expr.c" "calc.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        storage
        modal
        markdown
        filetype
        tui
        clock
        espressif__esp32_p4_function_ev_board
)
