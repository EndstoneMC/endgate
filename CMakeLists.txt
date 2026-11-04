cmake_minimum_required(VERSION 3.29)
project(endgate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/endgate.default.toml)
file(READ resources/endgate.default.toml ENDGATE_DEFAULT_CONFIG)
configure_file(cmake/default_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/endgate/default_config.h @ONLY)

find_package(asio REQUIRED)
find_package(date REQUIRED)
find_package(glaze REQUIRED)
find_package(replxx REQUIRED)
find_package(spdlog REQUIRED)
find_package(libdeflate REQUIRED)
find_package(Snappy REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

set(BEDROCK_PROTOCOL_VERSION "0.4.6")
include(FetchContent)
FetchContent_Declare(bedrock_protocol
        URL https://github.com/EndstoneMC/bedrock-protocol/releases/download/v${BEDROCK_PROTOCOL_VERSION}/bedrock-protocol-${BEDROCK_PROTOCOL_VERSION}-src.zip
        URL_HASH SHA256=8a039f9dbfcf6e741b0a790bbe988204a383b3da6f54c9465504e9b52385b503
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
        SYSTEM
)
FetchContent_MakeAvailable(bedrock_protocol)

enable_testing()

add_library(endgate_eventloop STATIC src/endgate/network/eventloop.cpp src/endgate/thread.cpp)
add_library(endgate::eventloop ALIAS endgate_eventloop)
target_include_directories(endgate_eventloop PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_eventloop PUBLIC asio::asio)

add_library(endgate_http STATIC src/endgate/network/http.cpp)
add_library(endgate::http ALIAS endgate_http)
target_include_directories(endgate_http PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_http PUBLIC endgate::eventloop OpenSSL::SSL OpenSSL::Crypto)

add_library(endgate_uuid STATIC src/endgate/util/uuid.cpp)
add_library(endgate::uuid ALIAS endgate_uuid)
target_include_directories(endgate_uuid PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_uuid PUBLIC OpenSSL::Crypto)

add_subdirectory(src/endgate/bstats)
add_subdirectory(src/endgate/brigadier)
add_subdirectory(src/endgate/buffer)
add_subdirectory(src/endgate/crypto)
add_subdirectory(src/endgate/network/raknet)
add_subdirectory(src/endgate/network/rtc)
add_subdirectory(src/endgate/network/rtc/sctp)
add_subdirectory(src/endgate/network/nethernet)

add_subdirectory(tests/unit/brigadier)
add_subdirectory(tests/unit/buffer)
add_subdirectory(tests/unit/crypto)
add_subdirectory(tests/unit/nethernet)
add_subdirectory(tests/unit/raknet)
add_subdirectory(tests/unit/rtc)
add_subdirectory(tests/unit/sctp)
add_subdirectory(tests/interop)

add_library(endgate_proxy STATIC
        src/endgate/auth/keys.cpp
        src/endgate/auth/token.cpp
        src/endgate/backend.cpp
        src/endgate/command/builtin/endgate.cpp
        src/endgate/command/builtin/glist.cpp
        src/endgate/command/builtin/send.cpp
        src/endgate/command/builtin/server.cpp
        src/endgate/command/builtin/shutdown.cpp
        src/endgate/command/injector.cpp
        src/endgate/command/manager.cpp
        src/endgate/config.cpp
        src/endgate/connection/backend.cpp
        src/endgate/connection/connection.cpp
        src/endgate/connection/handshake.cpp
        src/endgate/connection/login.cpp
        src/endgate/connection/player.cpp
        src/endgate/connection/raknet.cpp
        src/endgate/connection/settings.cpp
        src/endgate/console.cpp
        src/endgate/logging/appender.cpp
        src/endgate/logging/formatter.cpp
        src/endgate/logging/manager.cpp
        src/endgate/network/endpoint.cpp
        src/endgate/network/manager.cpp
        src/endgate/network/offline.cpp
        src/endgate/network/ping.cpp
        src/endgate/network/query.cpp
        src/endgate/protocol/cipher.cpp
        src/endgate/protocol/compress.cpp
        src/endgate/protocol/entity.cpp
        src/endgate/server.cpp
        src/endgate/playerlist/playerlist.cpp
)
add_library(endgate::proxy ALIAS endgate_proxy)

target_include_directories(endgate_proxy PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(endgate_proxy PUBLIC ENDGATE_VERSION="${PROJECT_VERSION}")

target_link_libraries(endgate_proxy PUBLIC
        bedrock::protocol
        endgate::bstats
        endgate::brigadier
        endgate::crypto
        endgate::eventloop
        endgate::http
        endgate::nethernet
        endgate::raknet
        endgate::uuid
        asio::asio
        date::date
        glaze::glaze
        replxx::replxx
        spdlog::spdlog
        libdeflate::libdeflate
        Snappy::snappy
)
if (WIN32)
    target_link_libraries(endgate_proxy PRIVATE iphlpapi)
endif ()

add_executable(endgate src/main.cpp)
target_link_libraries(endgate PRIVATE endgate::proxy)

add_subdirectory(tests/unit/auth)
add_subdirectory(tests/unit/bstats)
add_subdirectory(tests/unit/command)
add_subdirectory(tests/unit/connection)
add_subdirectory(tests/unit/logging)
add_subdirectory(tests/unit/network)
add_subdirectory(tests/unit/proxy)
add_subdirectory(tests/unit/playerlist)
add_subdirectory(tests/unit/util)
