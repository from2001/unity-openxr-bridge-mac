cmake_minimum_required(VERSION 3.20)

project(MetalXRHost LANGUAGES C)

add_executable(metalxr_host_encoder
    src/metalxr_host_encoder.c
)

set_target_properties(metalxr_host_encoder PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

if(APPLE)
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
