add_executable(endgate_rtc_tests
        test_certificate.cpp
        test_description.cpp
        test_dtls.cpp
        test_ice.cpp
        test_stun.cpp
)

target_link_libraries(endgate_rtc_tests PRIVATE endgate::rtc libdeflate::libdeflate GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(endgate_rtc_tests)
