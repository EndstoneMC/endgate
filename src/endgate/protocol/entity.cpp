#include "endgate/protocol/entity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <bedrock/protocol.hpp>
#include <bedrock/protocol/packet.hpp>
#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>

#include "endgate/network/codec.h"
#include "endgate/protocol/packet.h"

namespace endgate {

namespace {

template <typename T>
consteval bool carriesActorIds();

template <typename T>
consteval bool isActorDataInt64()
{
    if constexpr (bp::Reflected<T>) {
        return bp::struct_name<T>() == "DataItemInt64Payload";
    }
    else {
        return false;
    }
}

static_assert(isActorDataInt64<bp::DataItemInt64Payload_<MAXIMUM_VERSION.getProtocol()>>(),
              "actor data's int64 item is no longer named DataItemInt64Payload, so its actor IDs go unrewritten");

template <typename T>
struct CarriesActorIds {
    static consteval bool get()
    {
        if constexpr (std::is_same_v<T, bp::ActorRuntimeID> || std::is_same_v<T, bp::ActorUniqueID> ||
                      isActorDataInt64<T>()) {
            return true;
        }
        else if constexpr (bp::Reflected<T>) {
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (carriesActorIds<std::remove_cvref_t<decltype(bp::field_get<I>(std::declval<T &>()))>>() || ...);
            }(std::make_index_sequence<bp::field_count<T>()>{});
        }
        else {
            return false;
        }
    }
};

template <typename T>
struct CarriesActorIds<std::optional<T>> {
    static consteval bool get() { return carriesActorIds<T>(); }
};

template <typename T, typename A>
struct CarriesActorIds<std::vector<T, A>> {
    static consteval bool get() { return carriesActorIds<T>(); }
};

template <typename T, std::size_t N>
struct CarriesActorIds<std::array<T, N>> {
    static consteval bool get() { return carriesActorIds<T>(); }
};

template <typename K, typename V, typename C, typename A>
struct CarriesActorIds<std::map<K, V, C, A>> {
    static consteval bool get() { return carriesActorIds<V>(); }
};

template <typename... T>
struct CarriesActorIds<std::variant<T...>> {
    static consteval bool get() { return (carriesActorIds<T>() || ...); }
};

template <typename T>
consteval bool carriesActorIds()
{
    return CarriesActorIds<T>::get();
}

template <typename T>
void rewriteActorIds(T &value, const EntityMap &map)
{
    if constexpr (!carriesActorIds<T>()) {
        return;
    }
    else if constexpr (std::is_same_v<T, bp::ActorRuntimeID> || std::is_same_v<T, bp::ActorUniqueID>) {
        value = map.rewrite(value);
    }
    else if constexpr (isActorDataInt64<T>()) {
        value.value = map.rewrite(bp::ActorUniqueID{value.value});
    }
    else if constexpr (bp::Reflected<T>) {
        bp::for_each_field(value, [&](auto &field) { rewriteActorIds(field, map); });
    }
    else if constexpr (requires { value.index(); }) {
        std::visit([&](auto &alternative) { rewriteActorIds(alternative, map); }, value);
    }
    else if constexpr (requires { value.has_value(); }) {
        if (value.has_value()) {
            rewriteActorIds(*value, map);
        }
    }
    else if constexpr (requires { typename T::mapped_type; }) {
        for (auto &entry : value) {
            rewriteActorIds(entry.second, map);
        }
    }
    else {
        for (auto &element : value) {
            rewriteActorIds(element, map);
        }
    }
}

using PacketRewriter = ByteBuf (*)(const EntityMap &map, std::uint32_t header, ByteBufView body);

template <typename Packet>
ByteBuf rewritePacket(const EntityMap &map, std::uint32_t header, ByteBufView body)
{
    const auto bytes = body.nioBuffer();
    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()}};
    auto packet = bp::deserialize<Packet>(reader);
    if (!packet) {
        throw DecoderException(std::format("packet {} failed to decode: {}", Packet::Id, packet.error().message()));
    }
    if (reader.getUnreadLength() != 0) {
        throw DecoderException(std::format("packet {} left {} bytes unread", Packet::Id, reader.getUnreadLength()));
    }
    rewriteActorIds(*packet, map);
    std::string encoded;
    bp::BinaryWriter writer{encoded};
    bp::serialize(writer, *packet);
    auto out = Unpooled::buffer();
    out.writeUnsignedVarInt(header);
    out.writeBytes(std::as_bytes(std::span{encoded}));
    return out;
}

template <int Version, int Id>
consteval PacketRewriter rewriterOf()
{
    if constexpr (bp::has_packet_v<Version, Id>) {
        if constexpr (carriesActorIds<bp::packet_of_t<Version, Id>>()) {
            return &rewritePacket<bp::packet_of_t<Version, Id>>;
        }
        else {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
}

template <int Version, int... Ids>
consteval std::array<PacketRewriter, sizeof...(Ids)> rewritersOf(std::integer_sequence<int, Ids...>)
{
    return {rewriterOf<Version, Ids>()...};
}

template <int Version>
constexpr auto REWRITERS =
    rewritersOf<Version>(std::make_integer_sequence<int, static_cast<int>(bp::MinecraftPacketIds_<Version>::EndId)>{});

}  // namespace

EntityMap::EntityMap(bp::ActorRuntimeID client_runtime_id, bp::ActorUniqueID client_unique_id,
                     bp::ActorRuntimeID server_runtime_id, bp::ActorUniqueID server_unique_id)
    : client_runtime_id_(client_runtime_id), client_unique_id_(client_unique_id), server_runtime_id_(server_runtime_id),
      server_unique_id_(server_unique_id)
{
}

bool EntityMap::isIdentity() const
{
    return client_runtime_id_ == server_runtime_id_ && client_unique_id_ == server_unique_id_;
}

bp::ActorRuntimeID EntityMap::rewrite(bp::ActorRuntimeID id) const
{
    if (id == client_runtime_id_) {
        return server_runtime_id_;
    }
    if (id == server_runtime_id_) {
        return client_runtime_id_;
    }
    return id;
}

bp::ActorUniqueID EntityMap::rewrite(bp::ActorUniqueID id) const
{
    if (id == client_unique_id_) {
        return server_unique_id_;
    }
    if (id == server_unique_id_) {
        return client_unique_id_;
    }
    return id;
}

std::optional<ByteBuf> EntityMap::rewrite(int protocol, ByteBufView packet) const
{
    const auto header = packet.readUnsignedVarInt();
    const auto id = header & MinecraftDecoder::PACKET_ID_MASK;
    std::optional<ByteBuf> rewritten;
    visitProtocolVersion(protocol, [&]<int Version>() {
        const auto &rewriters = REWRITERS<Version>;
        if (id < rewriters.size() && rewriters.at(id) != nullptr) {
            rewritten = rewriters.at(id)(*this, header, packet);
        }
        return true;
    });
    return rewritten;
}

}  // namespace endgate
