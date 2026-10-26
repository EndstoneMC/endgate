add_library(endgate_sctp STATIC
        crc32c.cpp
        packet.cpp
)

target_include_directories(endgate_sctp PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_sctp PUBLIC asio::asio PRIVATE OpenSSL::Crypto)
