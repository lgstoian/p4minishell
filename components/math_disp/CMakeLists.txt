# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "src/math_disp.c" "src/math_disp_layout.c" "src/math_disp_render.c" "src/math_disp_parse.c" "src/math_disp_edit.c"
    INCLUDE_DIRS "include"
    # calc.h/calc.c live in components/batch (there is no separate `calc`
    # component); the evaluator and natural-display renderer share them.
    REQUIRES storage strutil batch
    PRIV_REQUIRES log
)