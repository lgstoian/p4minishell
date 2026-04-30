idf_component_register(
    SRCS "clock.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_timer
        lwip
)
