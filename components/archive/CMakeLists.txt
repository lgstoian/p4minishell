idf_component_register(
    SRCS "archive.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}"
    REQUIRES
        storage
        shell
)
