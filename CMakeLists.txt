cmake_minimum_required(VERSION 3.29)
project(endgate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(asio REQUIRED)
find_package(glaze REQUIRED)
find_package(LibDataChannel REQUIRED)
find_package(replxx REQUIRED)
find_package(spdlog REQUIRED)
find_package(libdeflate REQUIRED)

add_executable(endgate
        src/main.cpp
        src/endgate/console.cpp
        src/endgate/server.cpp
        src/endgate/logging/appender.cpp
        src/endgate/logging/formatter.cpp
        src/endgate/logging/manager.cpp
)

target_include_directories(endgate PRIVATE src)
target_compile_definitions(endgate PRIVATE ENDGATE_VERSION="${PROJECT_VERSION}")

target_link_libraries(endgate PRIVATE
        asio::asio
        glaze::glaze
        LibDataChannel::LibDataChannel
        replxx::replxx
        spdlog::spdlog
        libdeflate::libdeflate
)
