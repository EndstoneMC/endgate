#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/cipher.h"
#include "endgate/network/compress.h"
#include "endgate/network/frame.h"
#include "endgate/network/packet.h"
#include "endgate/network/pipeline.h"

namespace endgate {
namespace {

struct Ping {
    static constexpr std::uint32_t ID = 0x2A;

    std::int32_t value{};

    static Ping decode(ByteBufView &in) { return {in.readVarInt()}; }
    void encode(ByteBuf &out) const { out.writeVarInt(value); }
};

struct Wire {
    std::vector<ByteBuf> sent;
    std::vector<std::string> unknown;
    std::vector<std::int32_t> pings;
    std::vector<std::string> errors;
    int flushes{0};

    void channelRead(ByteBufView packet)
    {
        const auto bytes = packet.nioBuffer();
        unknown.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    void channelRead(const Ping &ping) { pings.push_back(ping.value); }
    void exceptionCaught(const std::exception &cause) { errors.emplace_back(cause.what()); }
    void write(ByteBuf batch) { sent.push_back(std::move(batch)); }
    void flush() { ++flushes; }
};

using Chain = ChannelPipeline<Wire, MinecraftCipherDecoder, MinecraftCipherEncoder, MinecraftCompressDecoder,
                              MinecraftCompressEncoder, MinecraftVarintFrameDecoder, MinecraftVarintLengthEncoder,
                              MinecraftDecoder<Ping>, MinecraftEncoder<Ping>>;

ByteBuf copyOf(std::string_view text)
{
    return Unpooled::copiedBuffer(std::as_bytes(std::span{text}));
}

ByteBuf framed(std::initializer_list<std::string_view> packets)
{
    auto batch = Unpooled::buffer();
    for (const auto packet : packets) {
        batch.writeUnsignedVarInt(static_cast<std::uint32_t>(packet.size()))
            .writeBytes(std::as_bytes(std::span{packet}));
    }
    return batch;
}

void switchOn(Chain &chain)
{
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    chain.get<MinecraftVarintLengthEncoder>().setEnabled(true);
    chain.get<MinecraftDecoder<Ping>>().setEnabled(true);
}

void compressWith(Chain &chain, PacketCompressionAlgorithm algorithm, std::uint16_t threshold)
{
    chain.get<MinecraftCompressDecoder>().setCompressionEnabled(true, algorithm);
    chain.get<MinecraftCompressEncoder>().setCompressionEnabled(true, algorithm, threshold);
}

TEST(Forwarding, AnInboundBatchReachesTheSinkWholeAndUntouched)
{
    Wire wire;
    Chain chain{wire};
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
    Chain chain{wire};

    chain.write(copyOf("first"));
    chain.write(copyOf("second"));

    ASSERT_EQ(wire.sent.size(), 2U);
    EXPECT_EQ(wire.sent[0], copyOf("first"));
    EXPECT_EQ(wire.sent[1], copyOf("second"));
}

TEST(Forwarding, CompressionStaysOffUntilTheHandshakeTurnsItOn)
{
    Wire wire;
    Chain chain{wire};
    const auto batch = copyOf("\x01\x02\x03 opaque bytes the proxy cannot read");

    chain.fireChannelRead(batch.slice());
    chain.write(copyOf("opaque"));

    EXPECT_EQ(wire.unknown.size(), 1U);
    EXPECT_EQ(wire.sent[0], copyOf("opaque"));
}

TEST(MinecraftVarintFrameDecoder, SplitsABatchAtEachLength)
{
    Wire wire;
    Chain chain{wire};
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);

    chain.fireChannelRead(framed({"abc", "d", "efgh"}).slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"abc", "d", "efgh"}));
    EXPECT_TRUE(wire.errors.empty());
}

TEST(MinecraftVarintFrameDecoder, ReadsLengthsLongerThanOneByte)
{
    Wire wire;
    Chain chain{wire};
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    const std::string big(300, 'x');

    chain.fireChannelRead(framed({big, "y"}).slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{big, "y"}));
}

TEST(MinecraftVarintFrameDecoder, RejectsMalformedLengths)
{
    Wire wire;
    Chain chain{wire};
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);

    auto zero = Unpooled::buffer();
    zero.writeUnsignedVarInt(0);
    chain.fireChannelRead(zero.slice());

    auto overrun = Unpooled::buffer();
    overrun.writeUnsignedVarInt(9).writeBytes(std::as_bytes(std::span{std::string_view{"abc"}}));
    chain.fireChannelRead(overrun.slice());

    auto truncated = Unpooled::buffer();
    truncated.writeByte(0x80);
    chain.fireChannelRead(truncated.slice());

    auto too_long = Unpooled::buffer();
    too_long.writeBytes(std::as_bytes(std::span{std::string_view{"\x80\x80\x80\x80\x80\x01"
                                                                 "a"}}));
    chain.fireChannelRead(too_long.slice());

    auto huge = Unpooled::buffer();
    huge.writeUnsignedVarInt(0xFFFFFFFF).writeByte('a');
    chain.fireChannelRead(huge.slice());

    EXPECT_TRUE(wire.unknown.empty());
    EXPECT_EQ(wire.errors.size(), 5U);
    EXPECT_EQ(wire.errors[0], "packet length is zero");
    EXPECT_EQ(wire.errors[1], "packet length 9 exceeds the 3 bytes left in the batch");
}

TEST(MinecraftVarintFrameDecoder, KeepsThePacketsAheadOfAMalformedLength)
{
    Wire wire;
    Chain chain{wire};
    chain.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    auto batch = framed({"ok"});
    batch.writeUnsignedVarInt(50).writeByte('z');

    chain.fireChannelRead(batch.slice());

    EXPECT_EQ(wire.unknown, (std::vector<std::string>{"ok"}));
    EXPECT_EQ(wire.errors.size(), 1U);
}

TEST(MinecraftVarintLengthEncoder, JoinsPacketsIntoOneBatchAtFlush)
{
    Wire wire;
    Chain chain{wire};
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
    Chain chain{wire};
    switchOn(chain);
    auto ping = Unpooled::buffer();
    ping.writeUnsignedVarInt(Ping::ID).writeVarInt(-7);
    auto other = Unpooled::buffer();
    other.writeUnsignedVarInt(0x09).writeByte('q');
    auto batch = Unpooled::buffer();
    batch.writeUnsignedVarInt(static_cast<std::uint32_t>(ping.readableBytes())).writeBytes(ping.nioBuffer());
    batch.writeUnsignedVarInt(static_cast<std::uint32_t>(other.readableBytes())).writeBytes(other.nioBuffer());

    chain.fireChannelRead(batch.slice());

    EXPECT_EQ(wire.pings, (std::vector<std::int32_t>{-7}));
    EXPECT_EQ(wire.unknown, (std::vector<std::string>{std::string{"\x09"
                                                                  "q"}}));
    EXPECT_TRUE(wire.errors.empty());
}

TEST(MinecraftDecoder, IgnoresTheSubIdsInTheHeader)
{
    Wire wire;
    Chain chain{wire};
    switchOn(chain);
    auto ping = Unpooled::buffer();
    ping.writeUnsignedVarInt(Ping::ID | (1U << 10U) | (2U << 12U)).writeVarInt(3);
    auto batch = Unpooled::buffer();
    batch.writeUnsignedVarInt(static_cast<std::uint32_t>(ping.readableBytes())).writeBytes(ping.nioBuffer());

    chain.fireChannelRead(batch.slice());

    EXPECT_EQ(wire.pings, (std::vector<std::int32_t>{3}));
}

TEST(MinecraftDecoder, RejectsARegisteredPacketThatLeavesBytesUnreadOrRunsShort)
{
    Wire wire;
    Chain chain{wire};
    switchOn(chain);

    auto trailing = Unpooled::buffer();
    trailing.writeUnsignedVarInt(Ping::ID).writeVarInt(1).writeByte(0xEE);
    auto short_packet = Unpooled::buffer();
    short_packet.writeUnsignedVarInt(Ping::ID);
    auto empty_header = Unpooled::buffer();
    empty_header.writeByte(0x80);
    for (const auto *packet : {&trailing, &short_packet, &empty_header}) {
        auto batch = Unpooled::buffer();
        batch.writeUnsignedVarInt(static_cast<std::uint32_t>(packet->readableBytes())).writeBytes(packet->nioBuffer());
        chain.fireChannelRead(batch.slice());
    }

    EXPECT_TRUE(wire.pings.empty());
    EXPECT_EQ(wire.errors.size(), 3U);
    EXPECT_EQ(wire.errors[0], "packet 42 left 1 bytes unread");
}

TEST(MinecraftEncoder, EncodesRegisteredPacketsBehindTheirHeaderAndPassesBytesOn)
{
    Wire wire;
    Chain chain{wire};
    switchOn(chain);

    chain.write(Ping{.value = -7});
    chain.write(copyOf("\x09"
                       "q"));
    chain.flush();

    ASSERT_EQ(wire.sent.size(), 1U);
    auto expected = Unpooled::buffer();
    expected.writeUnsignedVarInt(2).writeUnsignedVarInt(Ping::ID).writeVarInt(-7);
    expected.writeUnsignedVarInt(2).writeByte(0x09).writeByte('q');
    EXPECT_EQ(wire.sent[0], expected);
}

TEST(Pipeline, AWrittenBatchComesBackAsTheSamePackets)
{
    for (const auto algorithm : {PacketCompressionAlgorithm::ZLib, PacketCompressionAlgorithm::Snappy}) {
        Wire wire;
        Chain chain{wire};
        switchOn(chain);
        compressWith(chain, algorithm, 16);
        const std::string repetitive(400, 'a');

        chain.write(Ping{.value = 123456});
        chain.write(copyOf(repetitive));
        chain.write(copyOf("tail"));
        chain.flush();
        ASSERT_EQ(wire.sent.size(), 1U);
        EXPECT_EQ(wire.sent[0].getUnsignedByte(0), static_cast<std::uint8_t>(algorithm));
        EXPECT_LT(wire.sent[0].readableBytes(), 100U);

        chain.fireChannelRead(wire.sent[0].slice());
        EXPECT_EQ(wire.pings, (std::vector<std::int32_t>{123456}));
        EXPECT_EQ(wire.unknown, (std::vector<std::string>{repetitive, "tail"}));
        EXPECT_TRUE(wire.errors.empty());
    }
}

TEST(MinecraftCompressEncoder, LeavesABatchUnderTheThresholdUncompressed)
{
    Wire wire;
    Chain chain{wire};
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
    Chain chain{wire};
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
    Chain chain{wire};
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
    Chain chain{wire};
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
    Chain chain{wire};
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

TEST(MinecraftCipher, FailsLoudlyOnceEnabledSinceEncryptionIsNotImplemented)
{
    Wire wire;
    Chain chain{wire};
    chain.get<MinecraftCipherDecoder>().setEnabled(true);
    chain.get<MinecraftCipherEncoder>().setEnabled(true);

    chain.fireChannelRead(copyOf("secret").slice());
    chain.write(copyOf("secret"));

    EXPECT_TRUE(wire.unknown.empty());
    EXPECT_TRUE(wire.sent.empty());
    EXPECT_EQ(wire.errors,
              (std::vector<std::string>{"encryption is not supported yet", "encryption is not supported yet"}));
}

}  // namespace
}  // namespace endgate
