#include <cstddef>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "endgate/util/uuid.h"

namespace endgate {
namespace {

TEST(Uuid, NameUuidFromBytesIsVersion3)
{
    constexpr std::string_view name;
    const auto uuid = UUID::nameUUIDFromBytes(std::as_bytes(std::span{name}));
    EXPECT_EQ(uuid.toString(), "d41d8cd9-8f00-3204-a980-0998ecf8427e");
}

TEST(Uuid, FromStringReadsWhatToStringWrites)
{
    const auto uuid = UUID::fromString("d41d8cd9-8f00-3204-a980-0998ecf8427e");
    ASSERT_TRUE(uuid.has_value());
    EXPECT_EQ(uuid->toString(), "d41d8cd9-8f00-3204-a980-0998ecf8427e");
    EXPECT_EQ(uuid->getMostSignificantBits(), 0xd41d8cd98f003204);
    EXPECT_EQ(uuid->getLeastSignificantBits(), 0xa9800998ecf8427e);
}

TEST(Uuid, FromStringRefusesWhatIsNotAUuid)
{
    EXPECT_FALSE(UUID::fromString(""));
    EXPECT_FALSE(UUID::fromString("d41d8cd98f00320 4a9800998ecf8427e"));
    EXPECT_FALSE(UUID::fromString("d41d8cd9-8f00-3204-a980-0998ecf8427"));
    EXPECT_FALSE(UUID::fromString("d41d8cd9-8f00-3204-a980-0998ecf8427g"));
    EXPECT_FALSE(UUID::fromString("d41d8cd9-8f00-3204-a980-0998ecf8427e0"));
    EXPECT_FALSE(UUID::fromString("+41d8cd9-8f00-3204-a980-0998ecf8427e"));
}

TEST(Uuid, RandomUuidIsVersion4)
{
    const auto uuid = UUID::randomUUID();
    EXPECT_EQ(uuid.bytes[6] >> 4, 4);
    EXPECT_EQ(uuid.bytes[8] >> 6, 2);
    EXPECT_NE(uuid, UUID::randomUUID());
}

TEST(Uuid, OfflinePlayerMatchesJava)
{
    EXPECT_EQ(UuidUtils::generateOfflinePlayerUuid("Notch").toString(), "b50ad385-829d-3141-a216-7e7d7539ba7f");
}

TEST(Uuid, XuidPlayerMatchesBedrock)
{
    // md5("pocket-auth-1-xuid:2535428388216482") with the version and variant bits set, from python hashlib
    EXPECT_EQ(UuidUtils::generateXuidPlayerUuid("2535428388216482").toString(), "a60d883a-0b25-3afd-ab1e-cd8236db9c9e");
}

}  // namespace
}  // namespace endgate
