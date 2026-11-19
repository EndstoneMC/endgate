#include "endgate/connection/resourcepack.h"

#include <libdeflate.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glaze/json.hpp>
#include <openssl/evp.h>

#include "endgate/buffer/bytebuf.h"

namespace endgate {

namespace {

struct ManifestHeader {
    std::string uuid;
    std::variant<std::array<int, 3>, std::string> version;

    struct glaze {
        using T = ManifestHeader;
        static constexpr auto value = glz::object("uuid", &T::uuid, "version", &T::version);
    };
};

struct ManifestModule {
    std::string type;

    struct glaze {
        using T = ManifestModule;
        static constexpr auto value = glz::object("type", &T::type);
    };
};

struct Manifest {
    ManifestHeader header;
    std::vector<ManifestModule> modules;
    std::vector<std::string> capabilities;

    struct glaze {
        using T = Manifest;
        static constexpr auto value = glz::object(  //
            "header", &T::header,                   //
            "modules", &T::modules,                 //
            "capabilities", &T::capabilities);
    };
};

constexpr glz::opts MANIFEST_READ{.comments = true, .error_on_unknown_keys = false};

constexpr std::uint32_t END_OF_CENTRAL_DIRECTORY = 0x06054b50;
constexpr std::uint32_t CENTRAL_DIRECTORY_HEADER = 0x02014b50;
constexpr std::uint32_t LOCAL_FILE_HEADER = 0x04034b50;
constexpr std::uint16_t STORED = 0;
constexpr std::uint16_t DEFLATED = 8;
constexpr std::size_t MAX_MANIFEST_SIZE = 1024 * 1024;

std::optional<std::string> readManifest(ByteBufView archive)
{
    constexpr std::size_t end_record_size = 22;
    constexpr std::size_t max_comment_size = 0xffff;

    if (archive.readableBytes() < end_record_size) {
        return std::nullopt;
    }
    const auto last = archive.readableBytes() - end_record_size;
    const auto first = last > max_comment_size ? last - max_comment_size : 0;
    std::optional<std::size_t> end_record;
    for (auto offset = last + 1; offset-- > first;) {
        if (archive.getUnsignedIntLE(offset) == END_OF_CENTRAL_DIRECTORY) {
            end_record = offset;
            break;
        }
    }
    if (!end_record) {
        return std::nullopt;
    }

    const auto entries = archive.getUnsignedShortLE(*end_record + 10);
    std::size_t entry = archive.getUnsignedIntLE(*end_record + 16);
    std::optional<std::size_t> manifest;
    std::size_t manifest_depth = 0;
    for (std::uint16_t i = 0; i < entries; ++i) {
        if (archive.getUnsignedIntLE(entry) != CENTRAL_DIRECTORY_HEADER) {
            return std::nullopt;
        }
        const auto name_length = archive.getUnsignedShortLE(entry + 28);
        const auto extra_length = archive.getUnsignedShortLE(entry + 30);
        const auto comment_length = archive.getUnsignedShortLE(entry + 32);
        const auto name_bytes = archive.slice(entry + 46, name_length).nioBuffer();
        const std::string_view name{reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size()};
        const auto depth = static_cast<std::size_t>(std::ranges::count(name, '/'));
        if ((name == "manifest.json" || name.ends_with("/manifest.json")) && depth <= 1 &&
            (!manifest || depth < manifest_depth)) {
            manifest = entry;
            manifest_depth = depth;
        }
        entry += 46U + name_length + extra_length + comment_length;
    }
    if (!manifest) {
        return std::nullopt;
    }

    const auto method = archive.getUnsignedShortLE(*manifest + 10);
    const auto compressed_size = archive.getUnsignedIntLE(*manifest + 20);
    const auto size = archive.getUnsignedIntLE(*manifest + 24);
    const std::size_t local = archive.getUnsignedIntLE(*manifest + 42);
    if (archive.getUnsignedIntLE(local) != LOCAL_FILE_HEADER || size > MAX_MANIFEST_SIZE) {
        return std::nullopt;
    }
    const auto data_offset =
        local + 30U + archive.getUnsignedShortLE(local + 26) + archive.getUnsignedShortLE(local + 28);
    const auto data = archive.slice(data_offset, compressed_size).nioBuffer();
    if (method == STORED) {
        return std::string{reinterpret_cast<const char *>(data.data()), data.size()};
    }
    if (method != DEFLATED) {
        return std::nullopt;
    }
    const std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> decompressor(
        libdeflate_alloc_decompressor(), libdeflate_free_decompressor);
    std::string text(size, '\0');
    if (!decompressor || libdeflate_deflate_decompress(decompressor.get(), data.data(), data.size(), text.data(),
                                                       text.size(), nullptr) != LIBDEFLATE_SUCCESS) {
        return std::nullopt;
    }
    return text;
}

}  // namespace

std::expected<ResourcePackInfo, std::string> ResourcePackInfo::read(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("unable to read resource pack {}", path.string()));
    }
    ResourcePackInfo pack;
    std::ranges::transform(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(),
                           std::back_inserter(pack.file_), [](char c) { return static_cast<std::byte>(c); });

    std::optional<std::string> text;
    try {
        text = readManifest(Unpooled::wrappedBuffer(pack.file_));
    }
    catch (const std::out_of_range &) {
        text.reset();
    }
    if (!text) {
        return std::unexpected(
            std::format("resource pack {} is not a zip archive with a manifest.json", path.string()));
    }
    Manifest manifest;
    if (const auto error = glz::read<MANIFEST_READ>(manifest, *text)) {
        return std::unexpected(std::format("resource pack {} has an invalid manifest.json: {}", path.string(),
                                           glz::format_error(error, *text)));
    }
    if (std::ranges::none_of(manifest.modules,
                             [](const ManifestModule &module) { return module.type == "resources"; })) {
        return std::unexpected(std::format("{} is not a resource pack", path.string()));
    }
    const auto id = UUID::fromString(manifest.header.uuid);
    if (!id) {
        return std::unexpected(
            std::format("resource pack {} has an invalid uuid '{}'", path.string(), manifest.header.uuid));
    }
    pack.id_ = *id;
    if (const auto *const version = std::get_if<std::array<int, 3>>(&manifest.header.version)) {
        pack.version_ = std::format("{}.{}.{}", (*version)[0], (*version)[1], (*version)[2]);
    }
    else {
        pack.version_ = std::get<std::string>(manifest.header.version);
    }
    pack.ray_tracing_capable_ = std::ranges::contains(manifest.capabilities, "raytraced");

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(pack.file_.data(), pack.file_.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) {
        return std::unexpected(std::format("unable to hash resource pack {}", path.string()));
    }
    pack.hash_.assign(reinterpret_cast<const char *>(digest.data()), length);
    return pack;
}

std::string ResourcePackInfo::getPackIdVersion() const
{
    return std::format("{}_{}", id_.toString(), version_);
}

std::uint32_t ResourcePackInfo::getChunkCount() const
{
    return static_cast<std::uint32_t>((file_.size() + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

std::span<const std::byte> ResourcePackInfo::getChunk(std::uint32_t chunk) const
{
    const auto offset = static_cast<std::size_t>(chunk) * CHUNK_SIZE;
    if (offset >= file_.size()) {
        return {};
    }
    return std::span{file_}.subspan(offset, std::min<std::size_t>(CHUNK_SIZE, file_.size() - offset));
}

}  // namespace endgate
