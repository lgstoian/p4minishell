cmake_minimum_required(VERSION 3.16)

# Include the ESP-IDF project cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set test component directories
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ansi
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/shell
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/command
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/networking
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/keyboard
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/windows
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/header
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/clock
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/usb
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/c6ota
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_hosted
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_wifi_remote
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lvgl_port
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lcd_jd9165
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_lcd_touch_gt911
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp32_p4_function_ev_board
)

project(p4minishell_tests)
