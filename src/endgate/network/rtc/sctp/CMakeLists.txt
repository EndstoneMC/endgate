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
target_link_libraries(endgate_sctp PUBLIC asio::asio PRIVATE OpenSSL::Crypto)
