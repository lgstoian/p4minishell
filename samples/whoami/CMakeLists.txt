# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
# Sample out-of-tree-style native app (see samples/whoami/README.md).
idf_component_register(
    SRCS "whoami.c"
    INCLUDE_DIRS "."
    REQUIRES applib
)
