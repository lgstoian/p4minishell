idf_build_get_property(target IDF_TARGET)
idf_build_get_property(idf_path IDF_PATH)

if(${target} STREQUAL "linux")
    return()
endif()

set(managed_usb_dir "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__usb")
set(srcs)
set(include)
set(priv_includes)
set(priv_requires esp_mm)

if(${target} STREQUAL "esp32p4")
    list(APPEND priv_requires esp_psram)
endif()

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires esp_driver_gpio)
else()
    list(APPEND priv_requires esp_hal_usb esp_hw_support)
endif()

if(CONFIG_SOC_USB_OTG_SUPPORTED)
    list(APPEND srcs
        "${managed_usb_dir}/src/hcd_dwc.c"
        "${managed_usb_dir}/src/enum.c"
        "${managed_usb_dir}/src/hub.c"
        "${managed_usb_dir}/src/usb_helpers.c"
        "${managed_usb_dir}/src/usb_host.c"
        "${managed_usb_dir}/src/usb_private.c"
        "${managed_usb_dir}/src/usbh.c"
    )
    list(APPEND include "${managed_usb_dir}/include")
    list(APPEND priv_includes "${managed_usb_dir}/private_include")

    if(${IDF_VERSION_MAJOR} LESS 6)
        set(esp_idf_usb_dir "${idf_path}/components/usb")

        list(APPEND srcs "${esp_idf_usb_dir}/usb_phy.c")
        list(APPEND include "${esp_idf_usb_dir}/include")
    endif()
endif()

if(CONFIG_USB_HOST_HUBS_SUPPORTED)
    list(APPEND srcs
        "${managed_usb_dir}/src/ext_hub.c"
        "${managed_usb_dir}/src/ext_port.c"
    )
endif()

idf_component_register(
    SRCS ${srcs} "${CMAKE_CURRENT_LIST_DIR}/usb.c"
    INCLUDE_DIRS ${include} "${CMAKE_CURRENT_LIST_DIR}"
    PRIV_INCLUDE_DIRS ${priv_includes}
    REQUIRES
        ansi
        usb_host_hid
        usb_host_msc
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_COMPILER_STATIC_ANALYZER AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE "${managed_usb_dir}/src/usb_host.c" PROPERTY COMPILE_FLAGS -fno-analyzer)
endif()

set(remote_wake_hal_supported OFF)
set(idf_version_string "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

if(idf_version_string VERSION_GREATER_EQUAL "5.4.4" AND idf_version_string VERSION_LESS "5.5.0")
    set(remote_wake_hal_supported ON)
elseif(idf_version_string VERSION_GREATER_EQUAL "5.5.5")
    set(remote_wake_hal_supported ON)
endif()

if(remote_wake_hal_supported)
    # Match the definition in usb_host.h exactly so translation units that
    # include the header do not emit macro redefinition warnings.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "REMOTE_WAKE_HAL_SUPPORTED=(1)")
endif()