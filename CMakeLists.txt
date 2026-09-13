cmake_minimum_required(VERSION 3.29)
project(endgate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(asio REQUIRED)
find_package(glaze REQUIRED)
find_package(LibDataChannel REQUIRED)
find_package(replxx REQUIRED)
find_package(spdlog REQUIRED)

add_executable(endgate src/main.cpp)

target_link_libraries(endgate PRIVATE
        asio::asio
        glaze::glaze
        LibDataChannel::LibDataChannel
        replxx::replxx
        spdlog::spdlog
)
