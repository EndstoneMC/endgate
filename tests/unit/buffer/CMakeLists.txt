include(GoogleTest)

add_executable(endgate_buffer_tests
        test_bytebuf.cpp
)

target_link_libraries(endgate_buffer_tests PRIVATE endgate::buffer GTest::gtest_main)
gtest_discover_tests(endgate_buffer_tests)
