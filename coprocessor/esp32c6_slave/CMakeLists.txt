# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.5)

# C6 board profile. The ESP-Hosted coprocessor is board-specific: the C6 picks
# its board personality (SDIO/RF defaults) from boards/<C6_BOARD>/sdkconfig.defaults,
# layered on top of the common sdkconfig.defaults. Build one image per board:
#   idf.py -DC6_BOARD=jc1060p470c build
#   idf.py -DC6_BOARD=m5stack_tab5 build
set(C6_BOARD "jc1060p470c" CACHE STRING "C6 board profile (boards/<name>/sdkconfig.defaults)")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/boards/${C6_BOARD}/sdkconfig.defaults")

# Per-board generated sdkconfig so the two images can be configured side by
# side without one reconfigure overwriting the other's board options.
if(NOT C6_SDKCONFIG)
    set(C6_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${C6_BOARD}")
endif()
set(SDKCONFIG "${C6_SDKCONFIG}" CACHE FILEPATH
    "sdkconfig file for the active C6 board profile (sdkconfig.<C6_BOARD>)" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_hosted_slave)
