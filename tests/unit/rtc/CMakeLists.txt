add_executable(endgate_rtc_tests
        test_ice.cpp
        test_stun.cpp
)

target_link_libraries(endgate_rtc_tests PRIVATE endgate_rtc libdeflate::libdeflate GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(endgate_rtc_tests)
