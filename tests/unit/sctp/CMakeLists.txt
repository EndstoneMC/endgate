include(GoogleTest)

add_executable(endgate_sctp_tests
        test_association.cpp
        test_chunks.cpp
        test_cookie.cpp
        test_crc32c.cpp
        test_datachannel.cpp
        test_dcep.cpp
        test_packet.cpp
        test_receiver.cpp
        test_sender.cpp
        test_sequence.cpp
        test_stream_reset.cpp
        test_transfer.cpp
)

target_link_libraries(endgate_sctp_tests PRIVATE endgate_sctp GTest::gtest_main)
gtest_discover_tests(endgate_sctp_tests)
