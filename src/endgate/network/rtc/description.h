#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/ip/udp.hpp>

#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/ice.h"

namespace endgate::rtc {

/**
 * @see rtc::Candidate
 * @see RFC 8839 section 5.1
 */
struct IceCandidate {
    std::string foundation;
    std::uint16_t component{1};
    std::string transport;
    std::uint32_t priority{0};
    std::string address;
    std::uint16_t port{0};
    std::string type;
    std::optional<std::string> related_address;
    std::optional<std::uint16_t> related_port;

    [[nodiscard]] static std::optional<IceCandidate> parse(std::string_view value);
    [[nodiscard]] std::string toString() const;
};

/**
 * An address an answer offers the client. A server-reflexive one carries the local address that NAT
 * maps to it.
 *
 * @see RFC 8445 section 5.1.1
 */
struct CandidateAddress {
    asio::ip::udp::endpoint address;
    std::optional<asio::ip::udp::endpoint> base;
};

/**
 * @see RFC 8842 section 5
 */
enum class DtlsSetup {
    ActPass,
    Active,
    Passive,
};

/**
 * An SDP offer or answer for a session of one data-channel m-section, the only kind NetherNet
 * negotiates.
 *
 * @see rtc::Description
 * @see RFC 8829 section 5
 */
struct Description {
    std::vector<std::string> bundle;
    bool extmap_allow_mixed{false};
    std::optional<std::string> msid_semantic;
    bool ice_lite{false};
    std::optional<std::string> identity;
    std::optional<std::string> mid;
    std::string ice_ufrag;
    std::string ice_pwd;
    std::vector<std::string> ice_options;
    std::vector<CertificateFingerprint> fingerprints;
    DtlsSetup setup{DtlsSetup::ActPass};
    std::uint16_t sctp_port{5000};
    std::uint32_t max_message_size{65536};
    std::vector<IceCandidate> candidates;
    bool end_of_candidates{false};

    [[nodiscard]] static std::optional<Description> parse(std::string_view sdp);

    /**
     * @see RFC 8829 section 5.3
     * @see RFC 8842 section 5.3
     */
    [[nodiscard]] static Description answer(const Description &offer, const IceCredentials &credentials,
                                            const CertificateFingerprint &fingerprint,
                                            std::span<const CandidateAddress> addresses);

    [[nodiscard]] std::string generate(std::uint64_t session_id) const;
};

}  // namespace endgate::rtc
