cmake_minimum_required(VERSION 3.20)

project(MetalXRHost LANGUAGES C)

set(METALXR_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../MetalXRProtocol")

add_library(metalxr_protocol_host STATIC
    "${METALXR_PROTOCOL_DIR}/src/metalxr_protocol.c"
)

target_include_directories(metalxr_protocol_host
    PUBLIC
        "${METALXR_PROTOCOL_DIR}/include"
)

set_target_properties(metalxr_protocol_host PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

add_executable(metalxr_host_encoder
    src/metalxr_host_encoder.c
)

add_executable(metalxr_host_streamer
    src/metalxr_host_streamer.c
)

set_target_properties(metalxr_host_encoder PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

set_target_properties(metalxr_host_streamer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

if(APPLE)
    target_link_libraries(metalxr_host_streamer
        PRIVATE
            metalxr_protocol_host
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
    )

    target_link_libraries(metalxr_host_encoder
        PRIVATE
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
    )
else()
    message(FATAL_ERROR "MetalXR host encoder currently requires macOS VideoToolbox.")
endif()
