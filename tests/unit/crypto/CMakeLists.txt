add_executable(endgate_crypto_tests
        test_jose.cpp
)

target_link_libraries(endgate_crypto_tests PRIVATE endgate::crypto GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(endgate_crypto_tests)
