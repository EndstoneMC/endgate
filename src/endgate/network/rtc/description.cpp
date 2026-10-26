#include "endgate/network/rtc/description.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>
#include <string>
#include <utility>

namespace endgate::rtc {

namespace {

constexpr std::size_t MAX_DESCRIPTION_LENGTH = 65536;
constexpr std::size_t MAX_CANDIDATES = 32;
constexpr std::size_t MAX_FINGERPRINTS = 8;
constexpr std::size_t MAX_TOKENS = 8;
constexpr std::string_view LINE_ENDING = "\r\n";

template <typename Number>
std::optional<Number> parseNumber(std::string_view text)
{
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> tokensOf(std::string_view text)
{
    std::vector<std::string_view> tokens;
    for (const auto token : std::views::split(text, ' ')) {
        if (!token.empty()) {
            tokens.emplace_back(token.begin(), token.end());
        }
    }
    return tokens;
}

bool isIceString(std::string_view text, std::size_t minimum, std::size_t maximum)
{
    return text.size() >= minimum && text.size() <= maximum && std::ranges::all_of(text, [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                      c == '/';
           });
}

bool isPrintable(std::string_view text)
{
    return std::ranges::all_of(text, [](char c) { return c >= 0x20 && c <= 0x7E; });
}

}  // namespace

std::optional<IceCandidate> IceCandidate::parse(std::string_view value)
{
    const auto tokens = tokensOf(value);
    if (tokens.size() < 8 || tokens.size() > 64 || tokens[6] != "typ") {
        return std::nullopt;
    }
    IceCandidate candidate;
    const auto component = parseNumber<std::uint16_t>(tokens[1]);
    const auto priority = parseNumber<std::uint32_t>(tokens[3]);
    const auto port = parseNumber<std::uint16_t>(tokens[5]);
    if (!isIceString(tokens[0], 1, 32) || !component || *component == 0 || *component > 256 || !priority || !port ||
        tokens[4].size() > 255 || !isPrintable(tokens[4]) || tokens[7].size() > 32 || !isPrintable(tokens[7]) ||
        !isPrintable(tokens[2])) {
        return std::nullopt;
    }
    candidate.foundation = tokens[0];
    candidate.component = *component;
    candidate.transport = tokens[2];
    candidate.priority = *priority;
    candidate.address = tokens[4];
    candidate.port = *port;
    candidate.type = tokens[7];
    for (std::size_t i = 8; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "raddr" && tokens[i + 1].size() <= 255 && isPrintable(tokens[i + 1])) {
            candidate.related_address = tokens[i + 1];
        }
        else if (tokens[i] == "rport") {
            candidate.related_port = parseNumber<std::uint16_t>(tokens[i + 1]);
        }
    }
    return candidate;
}

std::string IceCandidate::toString() const
{
    auto text = std::format("candidate:{} {} {} {} {} {} typ {}", foundation, component, transport, priority, address,
                            port, type);
    if (related_address && related_port) {
        text += std::format(" raddr {} rport {}", *related_address, *related_port);
    }
    return text;
}

std::optional<Description> Description::parse(std::string_view sdp)
{
    if (sdp.size() > MAX_DESCRIPTION_LENGTH) {
        return std::nullopt;
    }

    struct Level {
        std::optional<std::string> ice_ufrag;
        std::optional<std::string> ice_pwd;
        std::optional<std::vector<std::string>> ice_options;
        std::vector<CertificateFingerprint> fingerprints;
        std::optional<DtlsSetup> setup;
    };

    Description description;
    Level session;
    Level media;
    bool versioned = false;
    bool in_media = false;
    for (const auto range : std::views::split(sdp, '\n')) {
        std::string_view line(range.begin(), range.end());
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (!versioned) {
            if (line != "v=0") {
                return std::nullopt;
            }
            versioned = true;
            continue;
        }
        if (line.size() < 2 || line[1] != '=') {
            return std::nullopt;
        }
        const auto value = line.substr(2);
        if (line[0] == 'm') {
            const auto tokens = tokensOf(value);
            if (in_media || tokens.size() != 4 || tokens[0] != "application" ||
                !parseNumber<std::uint16_t>(tokens[1]) || tokens[2] != "UDP/DTLS/SCTP" ||
                tokens[3] != "webrtc-datachannel") {
                return std::nullopt;
            }
            in_media = true;
            continue;
        }
        if (line[0] != 'a') {
            continue;
        }

        const auto colon = value.find(':');
        const auto key = value.substr(0, colon);
        const auto argument = colon == std::string_view::npos ? std::string_view{} : value.substr(colon + 1);
        auto &level = in_media ? media : session;
        if (key == "group") {
            const auto tokens = tokensOf(argument);
            if (!in_media && !tokens.empty() && tokens.front() == "BUNDLE" && tokens.size() <= MAX_TOKENS + 1) {
                description.bundle.assign(tokens.begin() + 1, tokens.end());
            }
        }
        else if (key == "extmap-allow-mixed") {
            description.extmap_allow_mixed = true;
        }
        else if (key == "msid-semantic" && isPrintable(argument)) {
            description.msid_semantic = argument;
        }
        else if (key == "ice-lite") {
            description.ice_lite = true;
        }
        else if (key == "identity" && !description.identity && isPrintable(argument)) {
            description.identity = argument;
        }
        else if (key == "ice-ufrag") {
            level.ice_ufrag = argument;
        }
        else if (key == "ice-pwd") {
            level.ice_pwd = argument;
        }
        else if (key == "ice-options") {
            const auto tokens = tokensOf(argument) | std::views::take(MAX_TOKENS);
            level.ice_options.emplace(tokens.begin(), tokens.end());
        }
        else if (key == "fingerprint") {
            const auto tokens = tokensOf(argument);
            if (tokens.size() != 2 || tokens[0].size() > 16 || tokens[1].size() > 3 * 64 ||
                !std::ranges::all_of(tokens[1],
                                     [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) || c == ':'; })) {
                return std::nullopt;
            }
            if (level.fingerprints.size() < MAX_FINGERPRINTS) {
                level.fingerprints.push_back({.algorithm = std::string(tokens[0]), .value = std::string(tokens[1])});
            }
        }
        else if (key == "setup") {
            if (argument == "actpass") {
                level.setup = DtlsSetup::ActPass;
            }
            else if (argument == "active") {
                level.setup = DtlsSetup::Active;
            }
            else if (argument == "passive") {
                level.setup = DtlsSetup::Passive;
            }
            else {
                return std::nullopt;
            }
        }
        else if (key == "mid" && in_media && isIceString(argument, 1, 32)) {
            description.mid = argument;
        }
        else if (key == "sctp-port" && in_media) {
            const auto port = parseNumber<std::uint16_t>(argument);
            if (!port || *port == 0) {
                return std::nullopt;
            }
            description.sctp_port = *port;
        }
        else if (key == "max-message-size" && in_media) {
            const auto size = parseNumber<std::uint32_t>(argument);
            if (!size) {
                return std::nullopt;
            }
            description.max_message_size = *size;
        }
        else if (key == "candidate" && in_media) {
            auto candidate = IceCandidate::parse(argument);
            if (candidate && description.candidates.size() < MAX_CANDIDATES) {
                description.candidates.push_back(std::move(*candidate));
            }
        }
        else if (key == "end-of-candidates") {
            description.end_of_candidates = true;
        }
    }

    auto ice_ufrag = media.ice_ufrag ? media.ice_ufrag : session.ice_ufrag;
    auto ice_pwd = media.ice_pwd ? media.ice_pwd : session.ice_pwd;
    if (!in_media || !ice_ufrag || !isIceString(*ice_ufrag, 4, 256) || !ice_pwd || !isIceString(*ice_pwd, 22, 256)) {
        return std::nullopt;
    }
    description.ice_ufrag = std::move(*ice_ufrag);
    description.ice_pwd = std::move(*ice_pwd);
    description.ice_options = media.ice_options.value_or(session.ice_options.value_or(std::vector<std::string>{}));
    description.fingerprints = media.fingerprints.empty() ? session.fingerprints : media.fingerprints;
    description.setup = media.setup.value_or(session.setup.value_or(DtlsSetup::ActPass));
    if (description.fingerprints.empty()) {
        return std::nullopt;
    }
    return description;
}

Description Description::answer(const Description &offer, const IceCredentials &credentials,
                                const CertificateFingerprint &fingerprint, std::span<const CandidateAddress> addresses)
{
    Description answer;
    answer.mid = offer.mid;
    if (offer.mid && std::ranges::contains(offer.bundle, *offer.mid)) {
        answer.bundle = {*offer.mid};
    }
    answer.extmap_allow_mixed = offer.extmap_allow_mixed;
    answer.msid_semantic = offer.msid_semantic;
    answer.ice_lite = true;
    answer.ice_ufrag = credentials.ufrag;
    answer.ice_pwd = credentials.pwd;
    if (std::ranges::contains(offer.ice_options, "trickle")) {
        answer.ice_options = {"trickle"};
    }
    answer.fingerprints = {fingerprint};
    answer.setup = offer.setup == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active;
    answer.sctp_port = 5000;
    answer.max_message_size = 262144;
    for (std::size_t index = 0; index < std::min(addresses.size(), MAX_CANDIDATES); ++index) {
        constexpr std::uint32_t host_type_preference = 126;
        constexpr std::uint32_t server_reflexive_type_preference = 100;
        const auto unmapped = [](const asio::ip::address &address) {
            return address.is_v6() && address.to_v6().is_v4_mapped()
                     ? asio::ip::address{asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6())}
                     : address;
        };
        const auto &[address, base] = addresses[index];
        const auto type_preference = base ? server_reflexive_type_preference : host_type_preference;
        answer.candidates.push_back({
            .foundation = std::to_string(index + 1),
            .component = 1,
            .transport = "udp",
            .priority = (type_preference << 24U) | ((65535U - static_cast<std::uint32_t>(index)) << 8U) | 255U,
            .address = unmapped(address.address()).to_string(),
            .port = address.port(),
            .type = base ? "srflx" : "host",
            .related_address = base ? std::optional{unmapped(base->address()).to_string()} : std::nullopt,
            .related_port = base ? std::optional{base->port()} : std::nullopt,
        });
    }
    answer.end_of_candidates = true;
    return answer;
}

std::string Description::generate(std::uint64_t session_id) const
{
    std::string sdp;
    const auto line = [&sdp](std::string_view text) {
        sdp += text;
        sdp += LINE_ENDING;
    };
    line("v=0");
    line(std::format("o=- {} 2 IN IP4 127.0.0.1", session_id));
    line("s=-");
    line("t=0 0");
    if (!bundle.empty()) {
        std::string group = "a=group:BUNDLE";
        for (const auto &bundled : bundle) {
            group += ' ';
            group += bundled;
        }
        line(group);
    }
    if (extmap_allow_mixed) {
        line("a=extmap-allow-mixed");
    }
    if (msid_semantic) {
        line(std::format("a=msid-semantic:{}", *msid_semantic));
    }
    if (ice_lite) {
        line("a=ice-lite");
    }
    if (identity) {
        line(std::format("a=identity:{}", *identity));
    }
    line("m=application 9 UDP/DTLS/SCTP webrtc-datachannel");
    line("c=IN IP4 0.0.0.0");
    line(std::format("a=ice-ufrag:{}", ice_ufrag));
    line(std::format("a=ice-pwd:{}", ice_pwd));
    if (!ice_options.empty()) {
        std::string options = "a=ice-options:";
        for (const auto &option : ice_options) {
            options += option;
            options += ' ';
        }
        options.pop_back();
        line(options);
    }
    for (const auto &fingerprint : fingerprints) {
        line(std::format("a=fingerprint:{} {}", fingerprint.algorithm, fingerprint.value));
    }
    constexpr std::array setups{"actpass", "active", "passive"};
    line(std::format("a=setup:{}", setups[static_cast<std::size_t>(setup)]));
    if (mid) {
        line(std::format("a=mid:{}", *mid));
    }
    line(std::format("a=sctp-port:{}", sctp_port));
    line(std::format("a=max-message-size:{}", max_message_size));
    for (const auto &candidate : candidates) {
        line(std::format("a={}", candidate.toString()));
    }
    if (end_of_candidates) {
        line("a=end-of-candidates");
    }
    return sdp;
}

}  // namespace endgate::rtc
