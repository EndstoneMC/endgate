add_library(endgate_nethernet STATIC
        candidates.cpp
        framing.cpp
        identity.cpp
        session.cpp
        signaling.cpp
)
add_library(endgate::nethernet ALIAS endgate_nethernet)

target_include_directories(endgate_nethernet PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_nethernet
        PUBLIC endgate::rtc endgate::sctp asio::asio OpenSSL::SSL OpenSSL::Crypto
        PRIVATE glaze::glaze
)
