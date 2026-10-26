#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/description.h"

namespace endgate::rtc {
namespace {

std::string crlf(std::string_view text)
{
    std::string out;
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
}

const std::string MojangGuideOffer = crlf(R"(v=0
o=- 123456789 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:abcd
a=ice-pwd:abcdefghijklmnopqrstuvwx
a=ice-options:trickle
a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99
a=setup:actpass
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
a=candidate:1 1 udp 2130706431 192.168.1.100 12345 typ host
a=candidate:2 1 udp 1694498815 203.0.113.50 54321 typ srflx raddr 192.168.1.100 rport 12345
a=end-of-candidates
)");

const std::string MojangGuideAnswer = crlf(R"(v=0
o=- 987654321 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:wxyz
a=ice-pwd:zyxwvutsrqponmlkjihgfedcb
a=ice-options:trickle
a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00
a=identity:eyJpZHAiOnsiZG9tYWluIjoicGFydG5lci5leGFtcGxlIiwicHJvdG9jb2wi
a=setup:active
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
a=candidate:1 1 udp 2130706431 10.0.0.50 23456 typ host
a=end-of-candidates
)");

const std::string ConstructedLibwebrtcOffer = crlf(R"(v=0
o=- 4611731400430051336 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
a=identity:eyJhc3NlcnRpb24iOiJ7fSIsImlkcCI6eyJkb21haW4iOiJhdXRoLm1pbmVjcmFmdC5vcmciLCJwcm90b2NvbCI6ImRlZmF1bHQifX0=
m=application 52641 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 192.168.1.10
a=candidate:2045264012 1 udp 2122260223 192.168.1.10 52641 typ host generation 0 network-id 1 network-cost 10
a=candidate:3316475512 1 udp 2122194687 fd00::1:2 52642 typ host generation 0 network-id 2 network-cost 10
a=ice-ufrag:7Edb
a=ice-pwd:2JFG1T3n7r6m2f5sJ9n1lB8m
a=ice-options:trickle renomination
a=fingerprint:sha-256 4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B:19:E5:7C:AB:4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF
a=setup:actpass
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
)");

const std::string CapturedClientOffer = crlf(R"(v=0
o=- 8262678597354354447 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
a=identity:eyJhc3NlcnRpb24iOiJ7XCJmaW5nZXJwcmludHNcIjpcIlwiLFwidG9rZW5cIjpcIlwifSIsImlkcCI6eyJkb21haW4iOiJodHRwczovL2F1dGhvcml6YXRpb24uZnJhbmNoaXNlLm1pbmVjcmFmdC1zZXJ2aWNlcy5uZXQvIiwicHJvdG9jb2wiOiJkZWZhdWx0In19
m=application 51855 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 192.0.2.10
a=candidate:3494126524 1 udp 2122260223 192.0.2.10 51855 typ host generation 0 network-id 4
a=candidate:2196473220 1 udp 2122194687 198.51.100.20 51856 typ host generation 0 network-id 1 network-cost 10
a=ice-ufrag:t6mk
a=ice-pwd:K2r6gSr8elsFGxr6FWjjsgwk
a=ice-options:trickle
a=fingerprint:sha-256 84:BB:6F:D6:98:A6:76:B4:3E:76:28:C1:7A:E2:26:31:EA:D8:D4:AF:D0:64:6F:FB:86:07:80:F5:E2:67:E9:5E
a=setup:actpass
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
)");

const CertificateFingerprint MojangGuideAnswerFingerprint{
    .algorithm = "sha-256",
    .value = "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00"};

TEST(DescriptionTest, ParsesTheMojangGuideOffer)
{
    const auto offer = Description::parse(MojangGuideOffer);
    ASSERT_TRUE(offer);
    EXPECT_EQ(offer->bundle, std::vector<std::string>{"0"});
    EXPECT_TRUE(offer->extmap_allow_mixed);
    EXPECT_EQ(offer->msid_semantic, " WMS");
    EXPECT_FALSE(offer->ice_lite);
    EXPECT_FALSE(offer->identity);
    EXPECT_EQ(offer->mid, "0");
    EXPECT_EQ(offer->ice_ufrag, "abcd");
    EXPECT_EQ(offer->ice_pwd, "abcdefghijklmnopqrstuvwx");
    EXPECT_EQ(offer->ice_options, std::vector<std::string>{"trickle"});
    ASSERT_EQ(offer->fingerprints.size(), 1U);
    EXPECT_EQ(offer->fingerprints[0].algorithm, "sha-256");
    EXPECT_EQ(offer->setup, DtlsSetup::ActPass);
    EXPECT_EQ(offer->sctp_port, 5000);
    EXPECT_EQ(offer->max_message_size, 262144U);
    EXPECT_TRUE(offer->end_of_candidates);
    ASSERT_EQ(offer->candidates.size(), 2U);
    EXPECT_EQ(offer->candidates[0].address, "192.168.1.100");
    EXPECT_EQ(offer->candidates[0].port, 12345);
    EXPECT_EQ(offer->candidates[0].priority, 2130706431U);
    EXPECT_EQ(offer->candidates[1].type, "srflx");
    EXPECT_EQ(offer->candidates[1].related_address, "192.168.1.100");
    EXPECT_EQ(offer->candidates[1].related_port, 12345);
}

TEST(DescriptionTest, ParsesTheMojangGuideAnswer)
{
    const auto answer = Description::parse(MojangGuideAnswer);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->setup, DtlsSetup::Active);
    EXPECT_EQ(answer->identity, "eyJpZHAiOnsiZG9tYWluIjoicGFydG5lci5leGFtcGxlIiwicHJvdG9jb2wi");
    EXPECT_EQ(answer->ice_pwd, "zyxwvutsrqponmlkjihgfedcb");
}

TEST(DescriptionTest, ParsesAConstructedLibwebrtcOffer)
{
    const auto offer = Description::parse(ConstructedLibwebrtcOffer);
    ASSERT_TRUE(offer);
    EXPECT_TRUE(offer->identity);
    EXPECT_EQ(offer->ice_ufrag, "7Edb");
    EXPECT_EQ(offer->ice_options, (std::vector<std::string>{"trickle", "renomination"}));
    ASSERT_EQ(offer->candidates.size(), 2U);
    EXPECT_EQ(offer->candidates[1].address, "fd00::1:2");
    EXPECT_EQ(offer->candidates[1].foundation, "3316475512");
    EXPECT_FALSE(offer->end_of_candidates);
}

TEST(DescriptionTest, ParsesAnOfferCapturedFromARealClient)
{
    const auto offer = Description::parse(CapturedClientOffer);
    ASSERT_TRUE(offer);
    EXPECT_EQ(offer->bundle, std::vector<std::string>{"0"});
    EXPECT_EQ(offer->identity,
              "eyJhc3NlcnRpb24iOiJ7XCJmaW5nZXJwcmludHNcIjpcIlwiLFwidG9rZW5cIjpcIlwifSIsImlkcCI6eyJkb21haW4iOiJodHRwczov"
              "L2F1dGhvcml6YXRpb24uZnJhbmNoaXNlLm1pbmVjcmFmdC1zZXJ2aWNlcy5uZXQvIiwicHJvdG9jb2wiOiJkZWZhdWx0In19");
    EXPECT_EQ(offer->mid, "0");
    EXPECT_EQ(offer->ice_ufrag, "t6mk");
    EXPECT_EQ(offer->ice_pwd, "K2r6gSr8elsFGxr6FWjjsgwk");
    EXPECT_EQ(offer->ice_options, std::vector<std::string>{"trickle"});
    ASSERT_EQ(offer->fingerprints.size(), 1U);
    EXPECT_EQ(offer->setup, DtlsSetup::ActPass);
    EXPECT_EQ(offer->sctp_port, 5000);
    EXPECT_EQ(offer->max_message_size, 262144U);
    EXPECT_FALSE(offer->end_of_candidates);
    ASSERT_EQ(offer->candidates.size(), 2U);
    EXPECT_EQ(offer->candidates[0].address, "192.0.2.10");
    EXPECT_EQ(offer->candidates[0].port, 51855);
    EXPECT_EQ(offer->candidates[0].priority, 2122260223U);
    EXPECT_EQ(offer->candidates[0].type, "host");
    EXPECT_EQ(offer->candidates[1].address, "198.51.100.20");
    EXPECT_EQ(offer->candidates[1].port, 51856);
}

TEST(DescriptionTest, AnswersTheMojangGuideOfferAsTheGuideDoes)
{
    const auto offer = Description::parse(MojangGuideOffer);
    ASSERT_TRUE(offer);
    const std::vector<CandidateAddress> addresses{{.address = {asio::ip::make_address("10.0.0.50"), 23456}}};
    const auto answer = Description::answer(*offer, {.ufrag = "wxyz", .pwd = "zyxwvutsrqponmlkjihgfedcb"},
                                            MojangGuideAnswerFingerprint, addresses);

    EXPECT_EQ(answer.generate(987654321), crlf(R"(v=0
o=- 987654321 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
a=ice-lite
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:wxyz
a=ice-pwd:zyxwvutsrqponmlkjihgfedcb
a=ice-options:trickle
a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00
a=setup:active
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
a=candidate:1 1 udp 2130706431 10.0.0.50 23456 typ host
a=end-of-candidates
)"));
}

TEST(DescriptionTest, DoesNotMirrorRenomination)
{
    const auto offer = Description::parse(ConstructedLibwebrtcOffer);
    ASSERT_TRUE(offer);
    const auto answer = Description::answer(*offer, IceCredentials::generate(), MojangGuideAnswerFingerprint, {});
    EXPECT_EQ(answer.ice_options, std::vector<std::string>{"trickle"});
}

TEST(DescriptionTest, PicksTheSetupOppositeTheOffer)
{
    for (const auto [offered, answered] :
         {std::pair{"actpass", DtlsSetup::Active}, std::pair{"active", DtlsSetup::Passive},
          std::pair{"passive", DtlsSetup::Active}}) {
        auto sdp = MojangGuideOffer;
        sdp.replace(sdp.find("actpass"), 7, offered);
        const auto offer = Description::parse(sdp);
        ASSERT_TRUE(offer);
        EXPECT_EQ(Description::answer(*offer, IceCredentials::generate(), MojangGuideAnswerFingerprint, {}).setup,
                  answered);
    }
}

TEST(DescriptionTest, RanksHostCandidatesInTheOrderGiven)
{
    const auto offer = Description::parse(MojangGuideOffer);
    ASSERT_TRUE(offer);
    const std::vector<CandidateAddress> addresses{{.address = {asio::ip::make_address("203.0.113.9"), 19132}},
                                                  {.address = {asio::ip::make_address("2001:db8::9"), 19132}},
                                                  {.address = {asio::ip::make_address("::ffff:10.1.2.3"), 19132}}};
    const auto answer =
        Description::answer(*offer, IceCredentials::generate(), MojangGuideAnswerFingerprint, addresses);
    ASSERT_EQ(answer.candidates.size(), 3U);
    EXPECT_EQ(answer.candidates[0].priority, 2130706431U);
    EXPECT_EQ(answer.candidates[1].priority, 2130706175U);
    EXPECT_EQ(answer.candidates[1].address, "2001:db8::9");
    EXPECT_EQ(answer.candidates[2].address, "10.1.2.3");
    EXPECT_NE(answer.candidates[0].foundation, answer.candidates[1].foundation);
}

TEST(DescriptionTest, OffersAnAddressBehindNatAsServerReflexive)
{
    const auto offer = Description::parse(CapturedClientOffer);
    ASSERT_TRUE(offer);
    const std::vector<CandidateAddress> addresses{
        {.address = {asio::ip::make_address("203.0.113.9"), 19134},
         .base = asio::ip::udp::endpoint{asio::ip::make_address("10.0.0.5"), 19132}},
        {.address = {asio::ip::make_address("10.0.0.5"), 19132}},
    };
    const auto answer =
        Description::answer(*offer, IceCredentials::generate(), MojangGuideAnswerFingerprint, addresses);
    ASSERT_EQ(answer.candidates.size(), 2U);
    EXPECT_EQ(answer.candidates[0].type, "srflx");
    EXPECT_EQ(answer.candidates[0].priority, 1694498815U);
    EXPECT_EQ(answer.candidates[0].toString(),
              "candidate:1 1 udp 1694498815 203.0.113.9 19134 typ srflx raddr 10.0.0.5 rport 19132");
    EXPECT_EQ(answer.candidates[1].type, "host");
    EXPECT_EQ(answer.candidates[1].priority, 2130706175U);
    EXPECT_FALSE(answer.candidates[1].related_address);
    const auto generated = answer.generate(1);
    EXPECT_NE(
        generated.find("a=candidate:1 1 udp 1694498815 203.0.113.9 19134 typ srflx raddr 10.0.0.5 rport 19132\r\n"),
        std::string::npos)
        << generated;
}

TEST(DescriptionTest, ReadsBackWhatItWrites)
{
    const auto offer = Description::parse(ConstructedLibwebrtcOffer);
    ASSERT_TRUE(offer);
    const std::vector<CandidateAddress> addresses{{.address = {asio::ip::make_address("198.51.100.1"), 19132}}};
    const auto credentials = IceCredentials::generate();
    auto answer = Description::answer(*offer, credentials, MojangGuideAnswerFingerprint, addresses);
    answer.identity = "aWRlbnRpdHk=";
    const auto parsed = Description::parse(answer.generate(1));
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->ice_lite);
    EXPECT_EQ(parsed->identity, "aWRlbnRpdHk=");
    EXPECT_EQ(parsed->ice_ufrag, credentials.ufrag);
    EXPECT_EQ(parsed->ice_pwd, credentials.pwd);
    EXPECT_EQ(parsed->setup, DtlsSetup::Active);
    EXPECT_EQ(parsed->fingerprints[0].value, MojangGuideAnswerFingerprint.value);
    EXPECT_EQ(parsed->candidates[0].toString(), answer.candidates[0].toString());
}

TEST(DescriptionTest, AcceptsBareLineFeedsAndSessionLevelCredentials)
{
    const std::string sdp = R"(v=0
o=- 1 2 IN IP4 127.0.0.1
s=-
t=0 0
a=ice-ufrag:sess
a=ice-pwd:sessionpasswordsessionpw
a=fingerprint:sha-1 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
a=ice-ufrag:medi
a=mid:data
)";
    const auto description = Description::parse(sdp);
    ASSERT_TRUE(description);
    EXPECT_EQ(description->ice_ufrag, "medi");
    EXPECT_EQ(description->ice_pwd, "sessionpasswordsessionpw");
    EXPECT_EQ(description->fingerprints[0].algorithm, "sha-1");
    EXPECT_EQ(description->mid, "data");
    EXPECT_EQ(description->max_message_size, 65536U);
    EXPECT_TRUE(description->bundle.empty());
}

TEST(DescriptionTest, RejectsWhatItCannotAnswer)
{
    const auto replaced = [](std::string_view from, std::string_view to) {
        auto sdp = MojangGuideOffer;
        sdp.replace(sdp.find(from), from.size(), to);
        return sdp;
    };
    EXPECT_FALSE(Description::parse(""));
    EXPECT_FALSE(Description::parse(replaced("v=0", "v=1")));
    EXPECT_FALSE(Description::parse(replaced("UDP/DTLS/SCTP", "DTLS/SCTP")));
    EXPECT_FALSE(Description::parse(
        replaced("m=application 9 UDP/DTLS/SCTP webrtc-datachannel", "m=audio 9 UDP/TLS/RTP/SAVPF 111")));
    EXPECT_FALSE(Description::parse(MojangGuideOffer + "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"));
    EXPECT_FALSE(Description::parse(replaced("a=ice-ufrag:abcd", "a=ice-ufrag:abc")));
    EXPECT_FALSE(Description::parse(replaced("a=ice-ufrag:abcd", "a=ice-ufrag:ab;d")));
    EXPECT_FALSE(Description::parse(replaced("a=ice-pwd:abcdefghijklmnopqrstuvwx", "a=ice-pwd:short")));
    EXPECT_FALSE(Description::parse(replaced("a=ice-pwd:", "a=x-pwd:")));
    EXPECT_FALSE(Description::parse(replaced("a=fingerprint:", "a=x-fingerprint:")));
    EXPECT_FALSE(Description::parse(replaced("sha-256 AA:", "sha-256 ZZ:")));
    EXPECT_FALSE(Description::parse(replaced("a=setup:actpass", "a=setup:holdconn")));
    EXPECT_FALSE(Description::parse(replaced("a=sctp-port:5000", "a=sctp-port:70000")));
    EXPECT_FALSE(Description::parse(replaced("a=max-message-size:262144", "a=max-message-size:-1")));
    EXPECT_FALSE(Description::parse(replaced("s=-", "garbage")));
    EXPECT_FALSE(Description::parse(MojangGuideOffer + std::string(65536, 'a')));
}

TEST(DescriptionTest, SkipsBrokenCandidatesAndBoundsTheRest)
{
    auto sdp = MojangGuideOffer;
    sdp += "a=candidate:broken\r\n";
    sdp += "a=candidate:1 0 udp 1 192.0.2.1 1 typ host\r\n";
    for (int i = 0; i < 40; ++i) {
        sdp += "a=candidate:9 1 udp 1 192.0.2.1 " + std::to_string(1000 + i) + " typ host\r\n";
    }
    const auto description = Description::parse(sdp);
    ASSERT_TRUE(description);
    EXPECT_EQ(description->candidates.size(), 32U);
    EXPECT_EQ(description->candidates[2].port, 1000);
}

}  // namespace
}  // namespace endgate::rtc
