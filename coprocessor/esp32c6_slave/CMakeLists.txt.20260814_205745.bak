cmake_minimum_required(VERSION 3.5)

set(ESP_HOSTED_SLAVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_hosted/slave")
set(ESP_HOSTED_SLAVE_MAIN_DIR "${ESP_HOSTED_SLAVE_DIR}/main")

add_compile_options(-Wl,--wrap=esp_wifi_init)

file(READ "${ESP_HOSTED_SLAVE_MAIN_DIR}/esp_hosted_coprocessor_fw_ver.h" HEADER_CONTENTS)
set(PROJECT_VERSION_REGEX "#define[ \t]+PROJECT_VERSION_MAJOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_MINOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_PATCH_1[ \t]+([0-9]+)")
string(REGEX MATCH "${PROJECT_VERSION_REGEX}" PROJECT_VERSION_MATCH "${HEADER_CONTENTS}")

if(PROJECT_VERSION_MATCH)
    set(PROJECT_VERSION_MAJOR_1 "${CMAKE_MATCH_1}")
    set(PROJECT_VERSION_MINOR_1 "${CMAKE_MATCH_2}")
    set(PROJECT_VERSION_PATCH_1 "${CMAKE_MATCH_3}")
    set(PROJECT_VER "${PROJECT_VERSION_MAJOR_1}.${PROJECT_VERSION_MINOR_1}.${PROJECT_VERSION_PATCH_1}")
    message(STATUS "Setting PROJECT_VER to: ${PROJECT_VER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXCLUDE_COMPONENTS lwip esp_netif)

set(COMPONENTS esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm)

project(esp32c6_hosted_slave)
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)

set(common_dir "${ESP_HOSTED_SLAVE_MAIN_DIR}/common")
message(STATUS "common: ${common_dir}")

if(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
    if(EXISTS "${common_dir}/esp_hosted_lwip_src_port_hook.h")
        idf_component_get_property(lwip lwip COMPONENT_LIB)
        if(TARGET ${lwip})
            get_target_property(lwip_type ${lwip} TYPE)
            if(NOT lwip_type STREQUAL "INTERFACE_LIBRARY")
                message(STATUS "********** Configuring LWIP for network split port configs **********")
                target_compile_options(${lwip} PRIVATE "-include" "${common_dir}/esp_hosted_lwip_src_port_hook.h")
            endif()
        endif()
    endif()
endif()

message(*************************************************************************************)
message("                    Building ESP-Hosted-MCU FW :: ${PROJECT_VER} ")
message(*************************************************************************************)