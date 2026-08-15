idf_component_register(
    SRCS "applib.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        shell
        clock
        storage
        heap
        esp_timer
        freertos
)
