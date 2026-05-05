cmake_minimum_required(VERSION 3.20)

project(MetalXRRuntimeSkeleton LANGUAGES C)

add_library(metalxr_runtime SHARED
    src/metalxr_runtime.c
)

target_include_directories(metalxr_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(metalxr_runtime PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    OUTPUT_NAME metalxr_runtime
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(APPLE)
    set_target_properties(metalxr_runtime PROPERTIES
        INSTALL_NAME_DIR "@rpath"
    )
endif()
