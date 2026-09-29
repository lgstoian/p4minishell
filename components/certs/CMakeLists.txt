# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "certs.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls storage strutil
    PRIV_REQUIRES esp-tls
)
