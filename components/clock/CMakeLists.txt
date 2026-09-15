idf_component_register(
    SRCS
        "clock.c"
        "clock_commands.c"
        "clock_timer.c"
        "clock_rtc.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_timer
        lwip
        nvs_flash
        esp_driver_i2c
        esp_hw_support
)
