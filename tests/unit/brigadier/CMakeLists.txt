include(GoogleTest)

add_executable(endgate_brigadier_tests
        test_arguments.cpp
        test_builder.cpp
        test_completion.cpp
        test_context.cpp
        test_dispatcher.cpp
        test_exceptions.cpp
        test_reader.cpp
        test_redirect.cpp
        test_suggestion.cpp
        test_tree.cpp
        test_usages.cpp
)

target_link_libraries(endgate_brigadier_tests PRIVATE endgate::brigadier GTest::gtest_main)
gtest_discover_tests(endgate_brigadier_tests)
