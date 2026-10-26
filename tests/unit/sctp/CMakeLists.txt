include(GoogleTest)

add_executable(endgate_sctp_tests
        test_crc32c.cpp
)

target_link_libraries(endgate_sctp_tests PRIVATE endgate_sctp GTest::gtest_main)
gtest_discover_tests(endgate_sctp_tests)
