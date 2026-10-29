include(GoogleTest)

add_executable(endgate_network_tests
        test_eventloop.cpp
)

target_link_libraries(endgate_network_tests PRIVATE endgate::eventloop GTest::gtest_main)
gtest_discover_tests(endgate_network_tests)
