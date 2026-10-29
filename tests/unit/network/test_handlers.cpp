#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/pipeline.h"
#include "endgate/protocol/cipher.h"
#include "endgate/protocol/compress.h"
#include "endgate/protocol/frame.h"
#include "endgate/protocol/packet.h"

namespace endgate {
namespace {

struct Wire {
    std::vector<ByteBuf> sent;
    std::vector<std::string> unknown;
    std::vector<std::int32_t> versions;
    std::vector<std::string> decoded_bytes;
    std::vector<std::string_view> other_packets;
    std::vector<std::string> errors;
    int flushes{0};

    void channelRead(ByteBufView packet)
    {
        const auto bytes = packet.nioBuffer();
        unknown.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    void channelRead(const DecodedPacket<bp::RequestNetworkSettingsPacket> &decoded)
    {
        versions.push_back(decoded.packet.client_network_version);
        const auto bytes = decoded.bytes.nioBuffer();
        decoded_bytes.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    template <typename Packet>
    void channelRead(const DecodedPacket<Packet> &)
    {
        other_packets.push_back(bp::struct_name<Packet>());
    }
    void exceptionCaught(const std::exception &cause) { errors.emplace_back(cause.what()); }
    void write(ByteBuf batch) { sent.push_back(std::move(batch)); }
    void flush() { ++flushes; }
};

using Chain = ChannelPipeline<Wire, MinecraftCipherDecoder, MinecraftCipherEncoder, MinecraftCompressDecoder,
                              MinecraftCompressEncoder, MinecraftVarintFrameDecoder, MinecraftVarintLengthEncoder,
                              MinecraftDecoder, MinecraftEncoder>;

Chain chainOf(Wire &wire, Direction direction = Direction::Serverbound)
{
    return Chain{wire,
                 MinecraftCipherDecoder{},
                 MinecraftCipherEncoder{},
                 MinecraftCompressDecoder{},
                 MinecraftCompressEncoder{},
                 MinecraftVarintFrameDecoder{},
                 MinecraftVarintLengthEncoder{},
                 MinecraftDecoder{direction},
                 MinecraftEncoder{}};
}

std::string requestNetworkSettings(std::uint32_t header, std::int32_t version)
{
    auto packet = Unpooled::buffer();
    packet.writeUnsignedVarInt(header);
    packet.writeInt(version);
    const auto bytes = packet.nioBuffer();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

ByteBuf copyOf(std::string_view text)
{
    return Unpooled::copiedBuffer(std::as_bytes(std::span{text}));
}

ByteBuf framed(std::initializer_list<std::string_view> packets)
{
    auto batch = Unpooled::buffer();
    for (const auto packet : packets) {
        batch.writeUnsignedVarInt(static_cast<std::uint32_t>(packet.size()));
        batch.writeBytes(std::as_bytes(std::span{packet}));
    }
    return batch;
}

void switchOn(Chain &chain)
{
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    chain.get<MinecraftVarintLengthEncoder>().setEnabled(true);
    chain.get<MinecraftDecoder>().setEnabled(true);
}

void compressWith(Chain &chain, PacketCompressionAlgorithm algorithm, std::uint16_t threshold)
{
    chain.get<MinecraftCompressDecoder>().setCompressionEnabled(true, algorithm);
    chain.get<MinecraftCompressEncoder>().setCompressionEnabled(true, algorithm, threshold);
}

TEST(Forwarding, AnInboundBatchReachesTheSinkWholeAndUntouched)
{
    Wire wire;
    auto chain = chainOf(wire);
    const auto batch = copyOf("\x03"
                              "abc"
                              "\x02"
                              "de");

    chain.fireChannelRead(batch.slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{std::string{"\x03"
                                                                  "abc"
                                                                  "\x02"
                                                                  "de"}}));
    EXPECT_TRUE(wire.errors.empty());
}

TEST(Forwarding, AnOutboundBatchLeavesAsItIsWithoutWaitingForAFlush)
{
    Wire wire;
    auto chain = chainOf(wire);

    chain.write(copyOf("first"));
    chain.write(copyOf("second"));

    ASSERT_EQ(wire.sent.size(), 2U);
    EXPECT_EQ(wire.sent[0], copyOf("first"));
    EXPECT_EQ(wire.sent[1], copyOf("second"));
}

TEST(Forwarding, CompressionStaysOffUntilTheHandshakeTurnsItOn)
{
    Wire wire;
    auto chain = chainOf(wire);
    const auto batch = copyOf("\x01\x02\x03 opaque bytes the proxy cannot read");

    chain.fireChannelRead(batch.slice());
    chain.write(copyOf("opaque"));

    EXPECT_EQ(wire.unknown.size(), 1U);
    EXPECT_EQ(wire.sent[0], copyOf("opaque"));
}

TEST(MinecraftVarintFrameDecoder, SplitsABatchAtEachLength)
{
    Wire wire;
    auto chain = chainOf(wire);
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);

    chain.fireChannelRead(framed({"abc", "d", "efgh"}).slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"abc", "d", "efgh"}));
    EXPECT_TRUE(wire.errors.empty());
}

TEST(MinecraftVarintFrameDecoder, ReadsLengthsLongerThanOneByte)
{
    Wire wire;
    auto chain = chainOf(wire);
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    const std::string big(300, 'x');

    chain.fireChannelRead(framed({big, "y"}).slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{big, "y"}));
}

TEST(MinecraftVarintFrameDecoder, RejectsMalformedLengths)
{
    Wire wire;
    auto chain = chainOf(wire);
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);

    auto zero = Unpooled::buffer();
    zero.writeUnsignedVarInt(0);
    chain.fireChannelRead(zero.slice());

    auto overrun = Unpooled::buffer();
    overrun.writeUnsignedVarInt(9);
    overrun.writeBytes(std::as_bytes(std::span{std::string_view{"abc"}}));
    chain.fireChannelRead(overrun.slice());

    auto truncated = Unpooled::buffer();
    truncated.writeByte(0x80);
    chain.fireChannelRead(truncated.slice());

    auto too_long = Unpooled::buffer();
    too_long.writeBytes(std::as_bytes(std::span{std::string_view{"\x80\x80\x80\x80\x80\x01"
                                                                 "a"}}));
    chain.fireChannelRead(too_long.slice());

    auto huge = Unpooled::buffer();
    huge.writeUnsignedVarInt(0xFFFFFFFF);
    huge.writeByte('a');
    chain.fireChannelRead(huge.slice());

    EXPECT_TRUE(wire.unknown.empty());
    EXPECT_EQ(wire.errors.size(), 5U);
    EXPECT_EQ(wire.errors[0], "packet length is zero");
    EXPECT_EQ(wire.errors[1], "packet length 9 exceeds the 3 bytes left in the batch");
}

TEST(MinecraftVarintFrameDecoder, KeepsThePacketsAheadOfAMalformedLength)
{
    Wire wire;
    auto chain = chainOf(wire);
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    auto batch = framed({"ok"});
    batch.writeUnsignedVarInt(50);
    batch.writeByte('z');

    chain.fireChannelRead(batch.slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"ok"}));
    EXPECT_EQ(wire.errors.size(), 1U);
}

TEST(MinecraftVarintLengthEncoder, JoinsPacketsIntoOneBatchAtFlush)
{
    Wire wire;
    auto chain = chainOf(wire);
    chain.get<MinecraftVarintLengthEncoder>().setEnabled(true);

    chain.write(copyOf("abc"));
    chain.write(copyOf("d"));
    EXPECT_TRUE(wire.sent.empty());

    chain.flush();
    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(wire.sent[0], framed({"abc", "d"}));
    EXPECT_EQ(wire.flushes, 1);

    chain.flush();
    EXPECT_EQ(wire.sent.size(), 1U);
}

TEST(MinecraftDecoder, DecodesRegisteredPacketsAndPassesTheRestOnWhole)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);
    const auto request = requestNetworkSettings(193, 2168);

    chain.fireChannelRead(framed({request, "\x09q"}).slice());

    EXPECT_EQ(wire.versions, (std::vector<std::int32_t>{2168}));
    EXPECT_EQ(wire.decoded_bytes, (std::vector<std::string>{request}));
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"\x09q"}));
    EXPECT_TRUE(wire.errors.empty());
}

TEST(MinecraftDecoder, DecodesOnlyWhatItsStateRegistersInItsDirection)
{
    const auto request = requestNetworkSettings(193, 2168);
    for (const auto direction : {Direction::Serverbound, Direction::Clientbound}) {
        for (const auto state : {StateRegistry::Login, StateRegistry::Play}) {
            Wire wire;
            auto chain = chainOf(wire, direction);
            switchOn(chain);
            chain.get<MinecraftDecoder>().setState(state);

            chain.fireChannelRead(framed({request}).slice());

            const auto registered = direction == Direction::Serverbound && state == StateRegistry::Login;
            EXPECT_EQ(wire.versions.size(), registered ? 1U : 0U);
            EXPECT_EQ(wire.unknown.size(), registered ? 0U : 1U);
        }
    }
}

TEST(MinecraftDecoder, IgnoresTheSubIdsInTheHeader)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);

    chain.fireChannelRead(framed({requestNetworkSettings(193U | (1U << 10U) | (2U << 12U), 3)}).slice());

    EXPECT_EQ(wire.versions, (std::vector<std::int32_t>{3}));
}

TEST(MinecraftDecoder, RejectsARegisteredPacketThatLeavesBytesUnreadOrRunsShort)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);
    const auto request = requestNetworkSettings(193, 2168);

    for (const auto &packet : {request + "\xEE", request.substr(0, 4), std::string{"\x80"}}) {
        chain.fireChannelRead(framed({packet}).slice());
    }

    EXPECT_TRUE(wire.versions.empty());
    ASSERT_EQ(wire.errors.size(), 3U);
    EXPECT_EQ(wire.errors[0], "packet 193 left 1 bytes unread");
    EXPECT_TRUE(wire.errors[1].starts_with("packet 193 failed to decode: ")) << wire.errors[1];
}

TEST(MinecraftEncoder, EncodesAPacketOfItsVersionBehindItsHeaderAndPassesBytesOn)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);

    chain.write(bp::RequestNetworkSettingsPacket{.client_network_version = 2168});
    chain.write(copyOf("\x09q"));
    chain.flush();

    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(wire.sent[0], framed({requestNetworkSettings(193, 2168), "\x09q"}));
}

TEST(MinecraftEncoder, RefusesAPacketOfAnotherVersion)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);

    chain.write(bp::DisconnectPacket_<800>{});

    EXPECT_TRUE(wire.sent.empty());
    EXPECT_EQ(wire.errors, (std::vector<std::string>{"DisconnectPacket is not a packet of protocol 2168"}));
}

TEST(Pipeline, AWrittenBatchComesBackAsTheSamePackets)
{
    for (const auto algorithm : {PacketCompressionAlgorithm::ZLib, PacketCompressionAlgorithm::Snappy}) {
        Wire wire;
        auto chain = chainOf(wire);
        switchOn(chain);
        compressWith(chain, algorithm, 16);
        const std::string repetitive(400, 'a');

        chain.write(bp::RequestNetworkSettingsPacket{.client_network_version = 123456});
        chain.write(copyOf(repetitive));
        chain.write(copyOf("tail"));
        chain.flush();
        ASSERT_EQ(wire.sent.size(), 1U);
        EXPECT_EQ(wire.sent[0].getUnsignedByte(0), static_cast<std::uint8_t>(algorithm));
        EXPECT_LT(wire.sent[0].readableBytes(), 100U);

        chain.fireChannelRead(wire.sent[0].slice());
        EXPECT_EQ(wire.versions, (std::vector<std::int32_t>{123456}));
        EXPECT_EQ(wire.unknown, (std::vector<std::string>{repetitive, "tail"}));
        EXPECT_TRUE(wire.errors.empty());
    }
}

TEST(MinecraftCompressEncoder, LeavesABatchUnderTheThresholdUncompressed)
{
    Wire wire;
    auto chain = chainOf(wire);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 64);

    chain.write(copyOf("short"));

    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(wire.sent[0].getUnsignedByte(0), 0xFF);
    EXPECT_EQ(wire.sent[0].slice(1, 5), copyOf("short"));

    chain.fireChannelRead(wire.sent[0].slice());
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"short"}));
}

TEST(MinecraftCompressEncoder, SendsABatchThatDoesNotShrinkUncompressed)
{
    Wire wire;
    auto chain = chainOf(wire);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 1);
    std::string noise(64, '\0');
    std::uint32_t state = 12345;
    for (auto &c : noise) {
        state = state * 1664525U + 1013904223U;
        c = static_cast<char>(state >> 24U);
    }

    chain.write(copyOf(noise));

    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(wire.sent[0].getUnsignedByte(0), 0xFF);
    chain.fireChannelRead(wire.sent[0].slice());
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{noise}));
}

TEST(MinecraftCompressEncoder, WithoutTheHeaderByteAlwaysCompresses)
{
    Wire wire;
    auto chain = chainOf(wire);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 0);
    chain.get<MinecraftCompressDecoder>().setHybridCompressionEnabled(false);
    chain.get<MinecraftCompressEncoder>().setHybridCompressionEnabled(false);
    const std::string repetitive(200, 'b');

    chain.write(copyOf(repetitive));
    chain.write(copyOf("x"));

    ASSERT_EQ(wire.sent.size(), 2U);
    EXPECT_LT(wire.sent[0].readableBytes(), 50U);
    chain.fireChannelRead(wire.sent[0].slice());
    chain.fireChannelRead(wire.sent[1].slice());
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{repetitive, "x"}));
}

TEST(MinecraftCompressDecoder, RejectsABatchItCannotRead)
{
    Wire wire;
    auto chain = chainOf(wire);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 16);

    chain.fireChannelRead(ByteBufView{});
    chain.fireChannelRead(copyOf("\x07"
                                 "payload")
                              .slice());
    chain.fireChannelRead(copyOf(std::string_view{"\x00"
                                                  "not deflate",
                                                  12})
                              .slice());
    chain.get<MinecraftCompressDecoder>().setCompressionEnabled(true, PacketCompressionAlgorithm::Snappy);
    chain.fireChannelRead(copyOf("\x01"
                                 "\xFF\xFF\xFF\xFF\xFF")
                              .slice());

    EXPECT_TRUE(wire.unknown.empty());
    EXPECT_EQ(wire.errors, (std::vector<std::string>{"batch is empty", "batch names an unknown compression algorithm",
                                                     "batch is corrupt", "batch is corrupt"}));
}

TEST(MinecraftCompressDecoder, StopsABatchThatExpandsBeyondThePermittedSize)
{
    Wire wire;
    auto chain = chainOf(wire);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 16);
    chain.get<MinecraftCompressEncoder>().setCompressionEnabled(true, PacketCompressionAlgorithm::ZLib, 16);

    auto bomb = Unpooled::buffer();
    bomb.writeZero(70U * 1024U * 1024U);
    chain.write(std::move(bomb));
    ASSERT_EQ(wire.sent.size(), 1U);

    chain.fireChannelRead(wire.sent[0].slice());
    EXPECT_TRUE(wire.unknown.empty());
    EXPECT_EQ(wire.errors, (std::vector<std::string>{"batch expands beyond the permitted size"}));
}

std::array<std::byte, MinecraftCipher::KEY_SIZE> testKey()
{
    std::array<std::byte, MinecraftCipher::KEY_SIZE> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key.at(i) = static_cast<std::byte>(i * 7 + 3);
    }
    return key;
}

std::string textOf(const ByteBuf &buf)
{
    const auto bytes = buf.nioBuffer();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

TEST(MinecraftCipher, SealsAsAesCtrFromTheFirstTwelveKeyBytesWithAChecksumBehind)
{
    const auto key = testKey();
    const std::string plaintext = "\x00a batch the cipher seals";
    MinecraftCipher cipher(key, MinecraftCipher::Mode::Encrypt);
    auto batch = copyOf(plaintext);

    cipher.seal(batch);

    std::array<unsigned char, 8> counter{};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(md.get(), EVP_sha256(), nullptr);
    EVP_DigestUpdate(md.get(), counter.data(), counter.size());
    EVP_DigestUpdate(md.get(), plaintext.data(), plaintext.size());
    EVP_DigestUpdate(md.get(), key.data(), key.size());
    EVP_DigestFinal_ex(md.get(), digest.data(), &digest_length);
    auto sealed = plaintext + std::string(reinterpret_cast<const char *>(digest.data()), 8);

    std::array<unsigned char, 16> iv{};
    std::memcpy(iv.data(), key.data(), 12);
    iv.at(15) = 2;
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctr(EVP_CIPHER_CTX_new(),
                                                                              EVP_CIPHER_CTX_free);
    int produced = 0;
    EVP_EncryptInit_ex(ctr.get(), EVP_aes_256_ctr(), nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                       iv.data());
    EVP_EncryptUpdate(ctr.get(), reinterpret_cast<unsigned char *>(sealed.data()), &produced,
                      reinterpret_cast<const unsigned char *>(sealed.data()), static_cast<int>(sealed.size()));

    EXPECT_EQ(textOf(batch), sealed);
}

TEST(MinecraftCipher, OpensWhatItSealsBatchAfterBatch)
{
    const auto key = testKey();
    MinecraftCipher encrypt(key, MinecraftCipher::Mode::Encrypt);
    MinecraftCipher decrypt(key, MinecraftCipher::Mode::Decrypt);

    std::vector<std::string> sealed;
    for (const auto *const plaintext : {"same", "same", std::string(5000, 'x').c_str()}) {
        auto batch = copyOf(plaintext);
        encrypt.seal(batch);
        sealed.push_back(textOf(batch));
        EXPECT_EQ(textOf(decrypt.open(batch.slice())), plaintext);
    }
    EXPECT_NE(sealed[0], sealed[1]);
}

TEST(MinecraftCipher, RejectsATamperedOrShortBatch)
{
    const auto key = testKey();
    MinecraftCipher encrypt(key, MinecraftCipher::Mode::Encrypt);
    auto batch = copyOf("a batch someone changed on the way");
    encrypt.seal(batch);
    batch.setByte(3, batch.getUnsignedByte(3) ^ 0x01U);

    MinecraftCipher tampered(key, MinecraftCipher::Mode::Decrypt);
    EXPECT_THROW(
        {
            try {
                (void)tampered.open(batch.slice());
            }
            catch (const DecoderException &cause) {
                EXPECT_STREQ(cause.what(), "batch checksum does not match");
                throw;
            }
        },
        DecoderException);
    MinecraftCipher short_batch(key, MinecraftCipher::Mode::Decrypt);
    EXPECT_THROW((void)short_batch.open(copyOf("seven").slice()), DecoderException);
}

TEST(Pipeline, AnEncryptedCompressedBatchComesBackAsTheSamePackets)
{
    Wire wire;
    auto chain = chainOf(wire);
    switchOn(chain);
    compressWith(chain, PacketCompressionAlgorithm::ZLib, 16);
    const auto key = testKey();
    chain.get<MinecraftCipherDecoder>().enableEncryption(key);
    chain.get<MinecraftCipherEncoder>().enableEncryption(key);
    const std::string repetitive(400, 'a');

    for (int round = 0; round < 2; ++round) {
        chain.write(bp::RequestNetworkSettingsPacket{.client_network_version = 2225});
        chain.write(copyOf(repetitive));
        chain.flush();
    }
    ASSERT_EQ(wire.sent.size(), 2U);
    EXPECT_NE(wire.sent[0], wire.sent[1]);
    for (const auto &batch : wire.sent) {
        chain.fireChannelRead(batch.slice());
    }

    EXPECT_EQ(wire.versions, (std::vector<std::int32_t>{2225, 2225}));
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{repetitive, repetitive}));
    EXPECT_TRUE(wire.errors.empty());
}

}  // namespace
}  // namespace endgate
