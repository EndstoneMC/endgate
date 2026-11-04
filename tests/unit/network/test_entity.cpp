#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <bedrock/protocol/actor.h>
#include <bedrock/protocol/level.h>
#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>
#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"
#include "endgate/network/pipeline.h"
#include "endgate/network/version.h"
#include "endgate/protocol/entity.h"

namespace endgate {
namespace {

constexpr int PROTOCOL = MAXIMUM_VERSION.getProtocol();

using SetActorMotionPacket = bp::packet_of_t<PROTOCOL, 40>;
using SetActorDataPacket = bp::packet_of_t<PROTOCOL, 39>;
using SetTimePacket = bp::packet_of_t<PROTOCOL, 10>;

const EntityMap MAP{bp::ActorRuntimeID{1}, bp::ActorUniqueID{10}, bp::ActorRuntimeID{7}, bp::ActorUniqueID{70}};

struct Wire {
    std::vector<ByteBuf> read;
    std::vector<ByteBuf> sent;
    std::vector<std::string> errors;

    void channelRead(ByteBufView packet) { read.push_back(packet.copy()); }
    void exceptionCaught(const std::exception_ptr &cause)
    {
        try {
            std::rethrow_exception(cause);
        }
        catch (const std::exception &error) {
            errors.emplace_back(error.what());
        }
    }
    void write(ByteBuf packet) { sent.push_back(std::move(packet)); }
    void flush() {}
};

using Chain = ChannelPipeline<Wire, EntityRewriteHandler>;

template <typename Packet>
ByteBuf encode(const Packet &packet, std::uint32_t header = Packet::Id)
{
    std::string body;
    bp::BinaryWriter writer{body};
    bp::serialize(writer, packet);
    auto out = Unpooled::buffer();
    out.writeUnsignedVarInt(header);
    out.writeBytes(std::as_bytes(std::span{body}));
    return out;
}

template <typename Packet>
Packet decode(const ByteBuf &bytes)
{
    auto view = bytes.slice();
    std::ignore = view.readUnsignedVarInt();
    const auto body = view.nioBuffer();
    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(body.data()), body.size()}};
    return bp::deserialize<Packet>(reader).value();
}

TEST(EntityMap, SwapsThePlayersIdsWithTheBackends)
{
    EXPECT_EQ(MAP.rewrite(bp::ActorRuntimeID{7}), bp::ActorRuntimeID{1});
    EXPECT_EQ(MAP.rewrite(bp::ActorRuntimeID{1}), bp::ActorRuntimeID{7});
    EXPECT_EQ(MAP.rewrite(bp::ActorRuntimeID{3}), bp::ActorRuntimeID{3});
    EXPECT_EQ(MAP.rewrite(bp::ActorUniqueID{70}), bp::ActorUniqueID{10});
    EXPECT_EQ(MAP.rewrite(bp::ActorUniqueID{10}), bp::ActorUniqueID{70});
    EXPECT_EQ(MAP.rewrite(bp::ActorUniqueID{30}), bp::ActorUniqueID{30});
}

TEST(EntityMap, IsTheIdentityWhenTheBackendGaveTheClientsIds)
{
    EXPECT_TRUE(EntityMap{}.isIdentity());
    EXPECT_TRUE((EntityMap{bp::ActorRuntimeID{1}, bp::ActorUniqueID{10}, bp::ActorRuntimeID{1}, bp::ActorUniqueID{10}}
                     .isIdentity()));
    EXPECT_FALSE(MAP.isIdentity());
}

TEST(EntityMap, RewritesTheActorIdsInAPacket)
{
    const auto packet = encode(SetActorMotionPacket{.runtime_id = bp::ActorRuntimeID{7}, .motion = {.x = 0.5F}});

    const auto rewritten = MAP.rewrite(PROTOCOL, packet.slice());

    ASSERT_TRUE(rewritten.has_value());
    const auto motion = decode<SetActorMotionPacket>(*rewritten);
    EXPECT_EQ(motion.runtime_id, bp::ActorRuntimeID{1});
    EXPECT_EQ(motion.motion.x, 0.5F);
}

TEST(EntityMap, RewritesOnlyTheInt64ItemsOfActorData)
{
    SetActorDataPacket packet{.id = bp::ActorRuntimeID{3}};
    packet.packed_items.data.push_back({.id = static_cast<std::uint32_t>(bp::ActorDataIDs_<PROTOCOL>::Owner),
                                        .payload = bp::DataItemInt64Payload_<PROTOCOL>{.value = 70}});
    packet.packed_items.data.push_back({.id = static_cast<std::uint32_t>(bp::ActorDataIDs_<PROTOCOL>::Variant),
                                        .payload = bp::DataItemIntPayload_<PROTOCOL>{.value = 70}});

    const auto rewritten = MAP.rewrite(PROTOCOL, encode(packet).slice());

    ASSERT_TRUE(rewritten.has_value());
    const auto items = decode<SetActorDataPacket>(*rewritten).packed_items.data;
    ASSERT_EQ(items.size(), 2U);
    EXPECT_EQ(std::get<bp::DataItemInt64Payload_<PROTOCOL>>(items[0].payload).value, 10);
    EXPECT_EQ(std::get<bp::DataItemIntPayload_<PROTOCOL>>(items[1].payload).value, 70);
}

TEST(EntityMap, KeepsTheSubClientBitsOfTheHeader)
{
    constexpr std::uint32_t header = 40U | (2U << 10U) | (3U << 12U);
    const auto packet = encode(SetActorMotionPacket{.runtime_id = bp::ActorRuntimeID{7}}, header);

    const auto rewritten = MAP.rewrite(PROTOCOL, packet.slice());

    ASSERT_TRUE(rewritten.has_value());
    auto view = rewritten->slice();
    EXPECT_EQ(view.readUnsignedVarInt(), header);
}

TEST(EntityMap, LeavesAPacketWithoutActorIds)
{
    EXPECT_FALSE(MAP.rewrite(PROTOCOL, encode(SetTimePacket{.time = 70}).slice()).has_value());
}

TEST(EntityMap, RefusesAPacketThatDoesNotDecode)
{
    auto packet = Unpooled::buffer();
    packet.writeUnsignedVarInt(40);

    EXPECT_THROW(std::ignore = MAP.rewrite(PROTOCOL, packet.slice()), DecoderException);
}

TEST(EntityRewriteHandler, PassesPacketsOnUntilTheBackendsIdsDiffer)
{
    Wire wire;
    Chain chain{wire, EntityRewriteHandler{}};
    chain.get<EntityRewriteHandler>().setProtocolVersion(MAXIMUM_VERSION);
    const auto packet = encode(SetActorMotionPacket{.runtime_id = bp::ActorRuntimeID{7}});

    chain.fireChannelRead(packet.slice());
    chain.write(packet.copy());

    ASSERT_EQ(wire.read.size(), 1U);
    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(decode<SetActorMotionPacket>(wire.read[0]).runtime_id, bp::ActorRuntimeID{7});
    EXPECT_EQ(decode<SetActorMotionPacket>(wire.sent[0]).runtime_id, bp::ActorRuntimeID{7});
}

TEST(EntityRewriteHandler, RewritesBothDirections)
{
    Wire wire;
    Chain chain{wire, EntityRewriteHandler{}};
    chain.get<EntityRewriteHandler>().setProtocolVersion(MAXIMUM_VERSION);
    chain.get<EntityRewriteHandler>().setEntityMap(MAP);

    chain.fireChannelRead(encode(SetActorMotionPacket{.runtime_id = bp::ActorRuntimeID{7}}).slice());
    chain.write(encode(SetActorMotionPacket{.runtime_id = bp::ActorRuntimeID{1}}));

    ASSERT_EQ(wire.read.size(), 1U);
    ASSERT_EQ(wire.sent.size(), 1U);
    EXPECT_EQ(decode<SetActorMotionPacket>(wire.read[0]).runtime_id, bp::ActorRuntimeID{1});
    EXPECT_EQ(decode<SetActorMotionPacket>(wire.sent[0]).runtime_id, bp::ActorRuntimeID{7});
    EXPECT_TRUE(wire.errors.empty());
}

}  // namespace
}  // namespace endgate
