# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "state_machine.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}"
    REQUIRES
        freertos
        p4heap
)
