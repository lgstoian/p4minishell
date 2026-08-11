idf_component_register(
    SRCS
        "clock.c"
        "clock_commands.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_timer
        lwip
)
