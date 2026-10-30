include(GoogleTest)

add_executable(endgate_util_tests
        test_ratelimit.cpp
)

target_link_libraries(endgate_util_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_util_tests)
