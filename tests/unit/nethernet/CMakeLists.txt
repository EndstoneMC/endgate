add_executable(endgate_nethernet_tests
        test_identity.cpp
)

target_link_libraries(endgate_nethernet_tests PRIVATE endgate_nethernet GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(endgate_nethernet_tests)
