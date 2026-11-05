include(GoogleTest)

add_executable(endgate_connection_tests
        test_future.cpp
        test_command_request.cpp
        test_login.cpp
        test_forwarding.cpp
        test_play.cpp
        test_routing.cpp
)

target_include_directories(endgate_connection_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../network)
target_link_libraries(endgate_connection_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_connection_tests)
