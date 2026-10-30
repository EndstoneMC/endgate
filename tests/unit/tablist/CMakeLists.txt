include(GoogleTest)

add_executable(endgate_tablist_tests
        test_tablist.cpp
)

target_include_directories(endgate_tablist_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../network)
target_link_libraries(endgate_tablist_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_tablist_tests)
