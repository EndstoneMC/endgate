add_library(endgate_nethernet STATIC
        identity.cpp
)

target_include_directories(endgate_nethernet PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_nethernet
        PUBLIC asio::asio OpenSSL::SSL OpenSSL::Crypto
        PRIVATE glaze::glaze
)
