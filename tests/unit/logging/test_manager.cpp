#include <gtest/gtest.h>

#include "endgate/logging/manager.h"

namespace endgate {
namespace {

struct Console {};

template <typename T>
struct NetherNetConnection {};

class Session {
public:
    virtual ~Session() = default;
};

class DialerSession final : public Session {};

TEST(LogManager, NamesALoggerAfterTheObjectsClass)
{
    EXPECT_EQ(LogManager::getLogger(Console{}).name(), "Console");
}

TEST(LogManager, LeavesTheTemplateArgumentsOut)
{
    EXPECT_EQ(LogManager::getLogger(NetherNetConnection<DialerSession>{}).name(), "NetherNetConnection");
}

TEST(LogManager, NamesAPolymorphicObjectAfterItsDynamicClass)
{
    const DialerSession session;
    const Session &base = session;

    EXPECT_EQ(LogManager::getLogger(base).name(), "DialerSession");
}

TEST(LogManager, TakesAStringAsTheName)
{
    EXPECT_EQ(LogManager::getLogger("main").name(), "main");
}

}  // namespace
}  // namespace endgate
