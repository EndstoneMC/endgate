add_library(endgate_rtc STATIC
        ice.cpp
        stun.cpp
)

target_include_directories(endgate_rtc PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_rtc
        PUBLIC asio::asio OpenSSL::SSL OpenSSL::Crypto
        PRIVATE libdeflate::libdeflate
)
