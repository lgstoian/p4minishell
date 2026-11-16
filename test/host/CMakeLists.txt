# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
#
# Host unit-test lane (ESP-IDF "linux" preview target).
#
# Builds a subset of the shared test/main/test_*.c sources as a native
# executable so the pure logic runs on every PR without a board. This project
# never affects the firmware image, the on-target Unity app (test/), or any
# board profile: it has its own sdkconfig, its own component list, and no
# hardware components.
#
# See test/README.md §Host suite for the include/exclude policy and
# tools/host_test_manifest.json for the single source of truth.
#
# Build (Linux or WSL2, ESP-IDF v5.5.5 sourced):
#   cd test/host
#   idf.py build
#   ./build/p4minishell_host_tests.elf
#
# The target is pinned by the set(IDF_TARGET "linux") below, so no
# set-target step is needed. ("linux" is a preview target: if you ever do run
# `idf.py --preview set-target linux`, the --preview flag is mandatory.)
cmake_minimum_required(VERSION 3.16)

# The host toolchain IS the target: no cross-compiler, no board, no flash.
set(IDF_TARGET "linux")

# The shared component headers resolve board/p4minishell config through the
# generated config directory, exactly as the on-target test project does.
# Only headers are staged; nothing here is compiled for a board.
set(P4_BOARD "jc1060p470c" CACHE STRING "Board profile whose headers are staged")

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/config)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../../boards/${P4_BOARD}/board_config.h
               ${CMAKE_BINARY_DIR}/config/board_config.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../../boards/${P4_BOARD}/board_identity.h
               ${CMAKE_BINARY_DIR}/config/board_identity.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../../p4minishell_config.h
               ${CMAKE_BINARY_DIR}/config/p4minishell_config.h COPYONLY)

# Host-only configuration. Never layered on a board profile.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.host" CACHE FILEPATH
    "sdkconfig file for the host lane (sdkconfig.host)" FORCE)

# Components allowed on the host. Phase 0 probe set: no LVGL, no drivers.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/strutil
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/json
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ansi
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/gfx
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/p4heap
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/filetype
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/markdown
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(p4minishell_host_tests)

# The shared p4minishell_config.h lives in the repository root.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
