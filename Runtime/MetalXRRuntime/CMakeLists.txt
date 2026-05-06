cmake_minimum_required(VERSION 3.20)

project(MetalXRRuntime LANGUAGES C)

set(METALXR_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../MetalXRProtocol")

add_library(metalxr_runtime SHARED
    src/metalxr_runtime.c
    "${METALXR_PROTOCOL_DIR}/src/metalxr_shared_state.c"
)

target_include_directories(metalxr_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        "${METALXR_PROTOCOL_DIR}/include"
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
