# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Backing BSP component directories for the ESP32-P4 Function EV Board profile.
# board_bsp/ is a thin wrapper over the managed BSP; the managed BSP directory
# is added here (not in the root CMakeLists) so no other board's build can pick
# it up and collide on the shared bsp_* symbols.
#
# CMAKE_CURRENT_LIST_DIR resolves to this file's directory
# (boards/<name>/board_bsp/) regardless of which project includes it, so the
# same hook works for the firmware and the standalone test project.
get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
list(APPEND EXTRA_COMPONENT_DIRS
    "${_repo_root}/managed_components/espressif__esp32_p4_function_ev_board")
unset(_repo_root)
