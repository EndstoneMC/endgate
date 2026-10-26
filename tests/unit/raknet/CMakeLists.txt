include(GoogleTest)

add_executable(endgate_raknet_tests
        test_codec.cpp
        test_message.cpp
        test_datagram.cpp
        test_congestion.cpp
        test_reliability.cpp
        test_connection.cpp
)

target_link_libraries(endgate_raknet_tests PRIVATE endgate::raknet GTest::gtest_main)
gtest_discover_tests(endgate_raknet_tests)
