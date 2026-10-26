add_executable(endgate_nethernet_tests
        test_framing.cpp
        test_identity.cpp
        test_signaling.cpp
)

target_link_libraries(endgate_nethernet_tests PRIVATE endgate::nethernet endgate::rtc GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(endgate_nethernet_tests)
