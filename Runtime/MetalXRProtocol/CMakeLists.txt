cmake_minimum_required(VERSION 3.20)

project(MetalXRProtocol LANGUAGES C)

add_library(metalxr_protocol STATIC
    src/metalxr_protocol.c
)

target_include_directories(metalxr_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(metalxr_protocol PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

add_executable(metalxr_protocol_loopback
    src/metalxr_protocol_loopback.c
)

target_link_libraries(metalxr_protocol_loopback
    PRIVATE
        metalxr_protocol
)

set_target_properties(metalxr_protocol_loopback PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
