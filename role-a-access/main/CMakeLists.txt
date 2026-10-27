idf_component_register(
    SRCS "app_main.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES access_core platform_stubs interfaces
)
