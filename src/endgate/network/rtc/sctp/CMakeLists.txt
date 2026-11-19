add_library(endgate_sctp STATIC
        association.cpp
        cookie.cpp
        crc32c.cpp
        dcep.cpp
        packet.cpp
        receiver.cpp
        sender.cpp
        transport.cpp
)
add_library(endgate::sctp ALIAS endgate_sctp)

target_include_directories(endgate_sctp PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_sctp PUBLIC endgate::buffer asio::asio PRIVATE OpenSSL::Crypto)

if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} /clang:--print-libgcc-file-name
            OUTPUT_VARIABLE ENDGATE_CLANG_BUILTINS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY
    )
    cmake_path(CONVERT "${ENDGATE_CLANG_BUILTINS}" TO_CMAKE_PATH_LIST ENDGATE_CLANG_BUILTINS NORMALIZE)
    if (NOT EXISTS "${ENDGATE_CLANG_BUILTINS}")
        message(FATAL_ERROR "clang builtins library not found: ${ENDGATE_CLANG_BUILTINS}")
    endif ()
    target_link_libraries(endgate_sctp PUBLIC "${ENDGATE_CLANG_BUILTINS}")
endif ()
