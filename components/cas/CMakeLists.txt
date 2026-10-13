# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "src/cas.c" "src/cas_parser.c" "src/cas_simplify.c" "src/cas_diff.c" "src/cas_integrate.c" "src/cas_solve.c" "src/cas_series.c"
    INCLUDE_DIRS "include"
    REQUIRES storage strutil batch
    PRIV_REQUIRES mbedtls
)