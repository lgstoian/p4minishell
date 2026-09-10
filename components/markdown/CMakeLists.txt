idf_component_register(
    SRCS "markdown.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        esp_system
)
