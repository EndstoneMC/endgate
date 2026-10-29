add_library(endgate_rtc STATIC
        certificate.cpp
        description.cpp
        dtls.cpp
        ice.cpp
        stun.cpp
)
add_library(endgate::rtc ALIAS endgate_rtc)

target_include_directories(endgate_rtc PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_rtc
        PUBLIC endgate::buffer asio::asio OpenSSL::SSL OpenSSL::Crypto
        PRIVATE libdeflate::libdeflate
)
