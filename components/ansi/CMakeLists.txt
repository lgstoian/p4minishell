# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS ansi.c
    INCLUDE_DIRS . "${CMAKE_SOURCE_DIR}"
    REQUIRES esp_system
)
