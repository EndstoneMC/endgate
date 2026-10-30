add_executable(endgate_nethernet_echo nethernet_echo.cpp)
target_link_libraries(endgate_nethernet_echo PRIVATE endgate::nethernet)

add_executable(endgate_nethernet_dial nethernet_dial.cpp)
target_link_libraries(endgate_nethernet_dial PRIVATE endgate::nethernet)

add_executable(endgate_bedrock_peer bedrock_peer.cpp)
target_link_libraries(endgate_bedrock_peer PRIVATE bedrock::protocol endgate::crypto glaze::glaze
        libdeflate::libdeflate OpenSSL::Crypto)
