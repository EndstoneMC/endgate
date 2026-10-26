add_library(endgate_raknet STATIC
        codec.cpp
        message.cpp
        datagram.cpp
        congestion.cpp
        reliability.cpp
        connection.cpp
        offline.cpp
)

target_include_directories(endgate_raknet PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_raknet PUBLIC asio::asio)
