include(GoogleTest)

add_executable(endgate_bstats_tests
        test_base.cpp
        test_charts.cpp
        test_config.cpp
)

target_link_libraries(endgate_bstats_tests PRIVATE endgate::bstats endgate::uuid libdeflate::libdeflate GTest::gtest_main)
gtest_discover_tests(endgate_bstats_tests)
