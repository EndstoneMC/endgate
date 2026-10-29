include(GoogleTest)

add_executable(endgate_network_tests
        test_endpoint.cpp
        test_eventloop.cpp
        test_manager.cpp
        test_pipeline.cpp
        test_registry.cpp
)

target_link_libraries(endgate_network_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_network_tests)
