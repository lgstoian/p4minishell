# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "src/browser_cache.c"
    INCLUDE_DIRS "include"
    REQUIRES tui markdown p4heap
    PRIV_REQUIRES freertos
)
