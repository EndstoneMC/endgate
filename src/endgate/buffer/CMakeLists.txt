add_library(endgate_buffer STATIC
        bytebuf.cpp
)
add_library(endgate::buffer ALIAS endgate_buffer)

target_include_directories(endgate_buffer PUBLIC ${PROJECT_SOURCE_DIR}/src)
