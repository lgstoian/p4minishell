# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "gfx.c" "gfx_font.c" "gfx_view.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
)
