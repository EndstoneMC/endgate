cmake_minimum_required(VERSION 3.29)
project(endgate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/endgate.toml)
file(READ resources/endgate.toml ENDGATE_DEFAULT_CONFIG)
configure_file(cmake/default_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/endgate/default_config.h @ONLY)

find_package(asio REQUIRED)
find_package(glaze REQUIRED)
find_package(replxx REQUIRED)
find_package(spdlog REQUIRED)
find_package(libdeflate REQUIRED)
find_package(Snappy REQUIRED)

add_library(endgate_raknet STATIC
        src/endgate/network/raknet/ReliabilityLayer.cpp
        src/endgate/network/raknet/CCRakNetSlidingWindow.cpp
        src/endgate/network/raknet/BitStream.cpp
        src/endgate/network/raknet/GetTime.cpp
        src/endgate/network/raknet/RakMemoryOverride.cpp
        src/endgate/network/raknet/RakNetTypes.cpp
)

target_include_directories(endgate_raknet PUBLIC src/endgate/network/raknet)
target_compile_definitions(endgate_raknet PRIVATE _RAKNET_LIB)
target_compile_options(endgate_raknet PRIVATE -w)

add_executable(endgate
        src/main.cpp
        src/endgate/config.cpp
        src/endgate/console.cpp
        src/endgate/server.cpp
        src/endgate/logging/appender.cpp
        src/endgate/logging/formatter.cpp
        src/endgate/logging/manager.cpp
        src/endgate/network/handler.cpp
        src/endgate/network/manager.cpp
        src/endgate/network/offline.cpp
        src/endgate/network/ping.cpp
)

target_include_directories(endgate PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(endgate PRIVATE ENDGATE_VERSION="${PROJECT_VERSION}")

target_link_libraries(endgate PRIVATE
        endgate_raknet
        asio::asio
        glaze::glaze
        replxx::replxx
        spdlog::spdlog
        libdeflate::libdeflate
        Snappy::snappy
)
