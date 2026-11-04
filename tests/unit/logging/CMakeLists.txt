include(GoogleTest)

add_executable(endgate_logging_tests
        test_formatter.cpp
        test_manager.cpp
)

target_link_libraries(endgate_logging_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_logging_tests)
