#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include <bedrock/protocol/actor.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/pipeline.h"
#include "endgate/network/version.h"

namespace endgate {

namespace bp = bedrock::protocol;

/**
 * Rewrites the player's actor IDs within packets.
 *
 * @see net.md_5.bungee.entitymap.EntityMap
 */
class EntityMap {
public:
    EntityMap() = default;
    EntityMap(bp::ActorRuntimeID client_runtime_id, bp::ActorUniqueID client_unique_id,
              bp::ActorRuntimeID server_runtime_id, bp::ActorUniqueID server_unique_id);

    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] bp::ActorRuntimeID rewrite(bp::ActorRuntimeID id) const;
    [[nodiscard]] bp::ActorUniqueID rewrite(bp::ActorUniqueID id) const;

    [[nodiscard]] std::optional<ByteBuf> rewrite(int protocol, ByteBufView packet) const;

private:
    bp::ActorRuntimeID client_runtime_id_;
    bp::ActorUniqueID client_unique_id_;
    bp::ActorRuntimeID server_runtime_id_;
    bp::ActorUniqueID server_unique_id_;
};

class EntityRewriteHandler : public ChannelDuplexHandler {
public:
    void setProtocolVersion(ProtocolVersion version) { version_ = version; }

    void setEntityMap(const EntityMap &map)
    {
        map_ = map;
        enabled_ = !map.isIdentity();
    }

    template <typename Context, typename Message>
    void channelRead(Context &ctx, Message &&message)
    {
        if constexpr (std::is_same_v<std::remove_cvref_t<Message>, ByteBufView>) {
            if (enabled_) {
                if (const auto rewritten = map_.rewrite(version_.getProtocol(), message)) {
                    ctx.fireChannelRead(rewritten->slice());
                    return;
                }
            }
        }
        ctx.fireChannelRead(std::forward<Message>(message));
    }

    template <typename Context, typename Message>
    void write(Context &ctx, Message &&message)
    {
        if constexpr (std::is_same_v<std::remove_cvref_t<Message>, ByteBuf>) {
            if (enabled_) {
                if (auto rewritten = map_.rewrite(version_.getProtocol(), message.slice())) {
                    ctx.write(std::move(*rewritten));
                    return;
                }
            }
        }
        ctx.write(std::forward<Message>(message));
    }

private:
    EntityMap map_;
    ProtocolVersion version_{MINIMUM_VERSION};
    bool enabled_{false};
};

}  // namespace endgate
