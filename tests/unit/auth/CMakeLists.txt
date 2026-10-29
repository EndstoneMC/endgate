include(GoogleTest)

add_executable(endgate_auth_tests
        test_token.cpp
)

target_link_libraries(endgate_auth_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_auth_tests)
