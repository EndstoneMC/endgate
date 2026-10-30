include(GoogleTest)

add_executable(endgate_proxy_tests
        test_config.cpp
)

target_link_libraries(endgate_proxy_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_proxy_tests)
