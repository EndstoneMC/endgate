include(GoogleTest)

add_executable(endgate_os_tests
        test_os.cpp
)

target_link_libraries(endgate_os_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_os_tests)
