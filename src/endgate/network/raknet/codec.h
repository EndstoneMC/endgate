#pragma once

#include <asio/ip/udp.hpp>

#include "endgate/buffer/bytebuf.h"

namespace endgate::raknet {

/**
 * @see RakNet::SystemAddress
 */
using SystemAddress = asio::ip::udp::endpoint;

[[nodiscard]] bool isAddressReadable(const ByteBufView &buffer);

/**
 * @see RakNet::BitStream::Read(SystemAddress &)
 */
[[nodiscard]] SystemAddress readAddress(ByteBufView &buffer);

/**
 * @see RakNet::BitStream::Write(const SystemAddress &)
 */
void writeAddress(ByteBuf &buffer, const SystemAddress &address);

}  // namespace endgate::raknet
