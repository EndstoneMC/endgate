include(GoogleTest)

add_executable(endgate_playerlist_tests
        test_playerlist.cpp
)

target_include_directories(endgate_playerlist_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../network)
target_link_libraries(endgate_playerlist_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_playerlist_tests)
