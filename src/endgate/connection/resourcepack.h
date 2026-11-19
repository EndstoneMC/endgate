#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "endgate/util/uuid.h"

namespace endgate {

/**
 * Represents a resource pack the proxy serves to its players: the archive and the identity its manifest
 * gives it.
 *
 * @see com.velocitypowered.api.proxy.player.ResourcePackInfo
 * @see com.velocitypowered.proxy.connection.player.VelocityResourcePackInfo
 */
class ResourcePackInfo {
public:
    static constexpr std::uint32_t CHUNK_SIZE = 1024 * 1024;

    [[nodiscard]] static std::expected<ResourcePackInfo, std::string> read(const std::filesystem::path &path);

    /**
     * @see com.velocitypowered.api.proxy.player.ResourcePackInfo#getId
     */
    [[nodiscard]] const UUID &getId() const { return id_; }

    [[nodiscard]] const std::string &getVersion() const { return version_; }

    /**
     * The pack's id and version as a client names the pack: `<uuid>_<version>`.
     *
     * @see PackIdVersion::asString
     */
    [[nodiscard]] std::string getPackIdVersion() const;

    /**
     * The archive's SHA-256.
     *
     * @see com.velocitypowered.api.proxy.player.ResourcePackInfo#getHash
     */
    [[nodiscard]] const std::string &getHash() const { return hash_; }

    [[nodiscard]] std::span<const std::byte> getFile() const { return file_; }
    [[nodiscard]] bool isRayTracingCapable() const { return ray_tracing_capable_; }
    [[nodiscard]] std::uint32_t getChunkCount() const;
    [[nodiscard]] std::span<const std::byte> getChunk(std::uint32_t chunk) const;

private:
    UUID id_;
    std::string version_;
    std::string hash_;
    std::vector<std::byte> file_;
    bool ray_tracing_capable_ = false;
};

}  // namespace endgate
