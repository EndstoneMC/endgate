include(GoogleTest)

add_executable(endgate_command_tests
        test_commands.cpp
        test_manager.cpp
        test_source.cpp
)

target_include_directories(endgate_command_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../network)
target_link_libraries(endgate_command_tests PRIVATE endgate::proxy GTest::gtest_main)
gtest_discover_tests(endgate_command_tests)
