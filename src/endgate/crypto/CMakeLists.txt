add_library(endgate_crypto STATIC encryption.cpp jose.cpp)
add_library(endgate::crypto ALIAS endgate_crypto)

target_include_directories(endgate_crypto PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_crypto
        PUBLIC OpenSSL::Crypto
        PRIVATE glaze::glaze
)
