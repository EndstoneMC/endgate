#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <bedrock/protocol/network.h>
#include <bedrock/protocol/packet.hpp>

#include "endgate/network/version.h"

namespace endgate {

namespace bp = bedrock::protocol;

/**
 * @see com.velocitypowered.proxy.protocol.ProtocolUtils.Direction
 */
enum class Direction : std::uint8_t {
    Serverbound,
    Clientbound,
};

/**
 * The states a connection passes through, each registering the packets it decodes in each direction.
 *
 * @see com.velocitypowered.proxy.protocol.StateRegistry
 */
enum class StateRegistry : std::uint8_t {
    Login,
    Play,
};

template <typename... Packets>
struct TypeList {};

/**
 * The packets one state decodes in one direction, named by their id in bedrock-protocol's newest
 * protocol. Each version resolves a name to its own packet type, which must exist and name the same
 * packet at every supported version.
 *
 * @see com.velocitypowered.proxy.protocol.StateRegistry.PacketRegistry
 */
template <bp::MinecraftPacketIds... Ids>
struct PacketRegistry {
    /**
     * @see com.velocitypowered.proxy.protocol.StateRegistry.PacketRegistry.ProtocolRegistry
     */
    template <int Version>
    struct ProtocolRegistry {
        static_assert((bp::has_packet_v<Version, static_cast<int>(Ids)> && ...),
                      "a registered packet resolves to nothing at this version; include its module");
        static_assert(((bp::struct_name<bp::packet_of_t<Version, static_cast<int>(Ids)>>() ==
                        bp::struct_name<bp::packet_of_t<MAXIMUM_VERSION.getProtocol(), static_cast<int>(Ids)>>()) &&
                       ...),
                      "a registered id names another packet at this version");

        using Packets = TypeList<bp::packet_of_t<Version, static_cast<int>(Ids)>...>;

        /**
         * Calls the visitor with the type of the packet registered under the id, if there is one.
         *
         * @see com.velocitypowered.proxy.protocol.StateRegistry.PacketRegistry.ProtocolRegistry#createPacket
         */
        template <typename Visitor>
        static bool createPacket(int id, Visitor &&visitor)
        {
            return ((id == static_cast<int>(Ids) &&
                     (visitor.template operator()<bp::packet_of_t<Version, static_cast<int>(Ids)>>(), true)) ||
                    ...);
        }
    };
};

template <StateRegistry State>
struct PacketRegistries;

template <>
struct PacketRegistries<StateRegistry::Login> {
    using Serverbound = PacketRegistry<bp::MinecraftPacketIds::RequestNetworkSettings, bp::MinecraftPacketIds::Login,
                                       bp::MinecraftPacketIds::ClientToServerHandshake>;
    using Clientbound =
        PacketRegistry<bp::MinecraftPacketIds::NetworkSettings, bp::MinecraftPacketIds::PlayStatus,
                       bp::MinecraftPacketIds::ServerToClientHandshake, bp::MinecraftPacketIds::Disconnect>;
};

template <>
struct PacketRegistries<StateRegistry::Play> {
    using Serverbound = PacketRegistry<>;
    using Clientbound = PacketRegistry<bp::MinecraftPacketIds::Disconnect>;
};

template <StateRegistry State, Direction D, int Version>
using ProtocolRegistryOf =
    std::conditional_t<D == Direction::Serverbound, typename PacketRegistries<State>::Serverbound,
                       typename PacketRegistries<State>::Clientbound>::template ProtocolRegistry<Version>;

/**
 * Calls the visitor with the registry of the state in the direction at the version, and returns
 * whether it took the packet.
 *
 * @see com.velocitypowered.proxy.protocol.StateRegistry#getProtocolRegistry
 */
template <typename Visitor>
bool getProtocolRegistry(StateRegistry state, Direction direction, int version, Visitor &&visitor)
{
    const auto at = [&]<int Version>() {
        switch (state) {
        case StateRegistry::Login:
            return direction == Direction::Serverbound
                     ? visitor.template
                       operator()<ProtocolRegistryOf<StateRegistry::Login, Direction::Serverbound, Version>>()
                     : visitor.template
                       operator()<ProtocolRegistryOf<StateRegistry::Login, Direction::Clientbound, Version>>();
        case StateRegistry::Play:
            return direction == Direction::Serverbound
                     ? visitor.template
                       operator()<ProtocolRegistryOf<StateRegistry::Play, Direction::Serverbound, Version>>()
                     : visitor.template
                       operator()<ProtocolRegistryOf<StateRegistry::Play, Direction::Clientbound, Version>>();
        }
        return false;
    };
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((SUPPORTED_VERSIONS.at(I).getProtocol() == version &&
                 at.template operator()<SUPPORTED_VERSIONS.at(I).getProtocol()>()) ||
                ...);
    }(std::make_index_sequence<SUPPORTED_VERSIONS.size()>{});
}

template <typename Packet>
bool isPacketOf(int version)
{
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((SUPPORTED_VERSIONS.at(I).getProtocol() == version &&
                 std::is_same_v<bp::packet_of_t<SUPPORTED_VERSIONS.at(I).getProtocol(), Packet::Id>, Packet>) ||
                ...);
    }(std::make_index_sequence<SUPPORTED_VERSIONS.size()>{});
}

template <typename List, typename Packet>
struct AppendUnique;

template <typename... Packets, typename Packet>
struct AppendUnique<TypeList<Packets...>, Packet> {
    using Type = std::conditional_t<(std::is_same_v<Packet, Packets> || ...), TypeList<Packets...>,
                                    TypeList<Packets..., Packet>>;
};

template <typename List, typename Other>
struct MergeUnique;

template <typename List>
struct MergeUnique<List, TypeList<>> {
    using Type = List;
};

template <typename List, typename Packet, typename... Rest>
struct MergeUnique<List, TypeList<Packet, Rest...>> {
    using Type = MergeUnique<typename AppendUnique<List, Packet>::Type, TypeList<Rest...>>::Type;
};

template <typename List, StateRegistry State, int Version>
using WithState = MergeUnique<
    typename MergeUnique<List, typename ProtocolRegistryOf<State, Direction::Serverbound, Version>::Packets>::Type,
    typename ProtocolRegistryOf<State, Direction::Clientbound, Version>::Packets>::Type;

template <typename List, std::size_t I = 0>
struct CollectRegistered {
    using Type =
        CollectRegistered<WithState<WithState<List, StateRegistry::Login, SUPPORTED_VERSIONS.at(I).getProtocol()>,
                                    StateRegistry::Play, SUPPORTED_VERSIONS.at(I).getProtocol()>,
                          I + 1>::Type;
};

template <typename List>
struct CollectRegistered<List, SUPPORTED_VERSIONS.size()> {
    using Type = List;
};

using RegisteredPackets = CollectRegistered<TypeList<>>::Type;

}  // namespace endgate
