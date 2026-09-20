# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# board_bsp - ESP32-P4 Function EV Board (JC1060P470C) profile.
#
# This is the thin indirection component the firmware links against. It has no
# sources of its own; it re-exports the vendored/patchable managed BSP
# (espressif__esp32_p4_function_ev_board) and adds board_bsp.h with the small
# set of compatibility macros the firmware uses to stay board-agnostic.
#
# Only the board_bsp directory matching the active -DP4_BOARD= is added to
# EXTRA_COMPONENT_DIRS, so exactly one BSP is in the build.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp32_p4_function_ev_board
)
