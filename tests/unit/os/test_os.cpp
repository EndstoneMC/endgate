#include <regex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "endgate/os.h"

namespace endgate {
namespace {

TEST(Os, ArchIsWhatTheJdkCallsTheCpu)
{
    const auto arch = os::arch();
    EXPECT_TRUE(arch == "amd64" || arch == "aarch64") << arch;
}

TEST(Os, VersionIsMajorDotMinorOnWindowsAndTheKernelReleaseElsewhere)
{
#ifdef _WIN32
    EXPECT_TRUE(std::regex_match(os::version(), std::regex(R"(\d+\.\d+)"))) << os::version();
#else
    EXPECT_TRUE(std::regex_match(os::version(), std::regex(R"(\d+\.\d+.*)"))) << os::version();
#endif
}

TEST(Os, NameIsTheJdksOnThisPlatform)
{
#ifdef _WIN32
    EXPECT_TRUE(os::name().starts_with("Windows ")) << os::name();
#else
    EXPECT_EQ(os::name(), "Linux");
#endif
}

TEST(Os, ActiveProcessorCountIsAtLeastOneAndNoMoreThanTheMachineHas)
{
    EXPECT_GE(os::active_processor_count(), 1);
    EXPECT_LE(os::active_processor_count(), static_cast<int>(std::thread::hardware_concurrency()));
}

TEST(Os, WindowsNamesWorkstationsByBuild)
{
    EXPECT_EQ(os::windows_name(10, 0, 19045, true, true), "Windows 10");
    EXPECT_EQ(os::windows_name(10, 0, 21999, true, true), "Windows 10");
    EXPECT_EQ(os::windows_name(10, 0, 22000, true, true), "Windows 11");
    EXPECT_EQ(os::windows_name(10, 0, 26200, true, true), "Windows 11");
}

TEST(Os, WindowsNamesServersByBuild)
{
    EXPECT_EQ(os::windows_name(10, 0, 14393, false, true), "Windows Server 2016");
    EXPECT_EQ(os::windows_name(10, 0, 17762, false, true), "Windows Server 2016");
    EXPECT_EQ(os::windows_name(10, 0, 17763, false, true), "Windows Server 2019");
    EXPECT_EQ(os::windows_name(10, 0, 20348, false, true), "Windows Server 2022");
    EXPECT_EQ(os::windows_name(10, 0, 26040, false, true), "Windows Server 2025");
}

TEST(Os, WindowsNamesTheOlderVersions)
{
    EXPECT_EQ(os::windows_name(6, 1, 7601, true, true), "Windows 7");
    EXPECT_EQ(os::windows_name(6, 3, 9600, true, true), "Windows 8.1");
    EXPECT_EQ(os::windows_name(6, 1, 7601, false, true), "Windows Server 2008 R2");
    EXPECT_EQ(os::windows_name(5, 2, 3790, true, true), "Windows XP");
    EXPECT_EQ(os::windows_name(5, 2, 3790, false, true), "Windows 2003");
    EXPECT_EQ(os::windows_name(11, 0, 30000, true, true), "Windows NT (unknown)");
}

TEST(Os, CgroupQuotaRoundsUpToWholeProcessors)
{
    EXPECT_EQ(os::cgroup_processor_count(8, 150000, 100000), 2);
    EXPECT_EQ(os::cgroup_processor_count(8, 100000, 100000), 1);
    EXPECT_EQ(os::cgroup_processor_count(8, 50000, 100000), 1);
}

TEST(Os, CgroupQuotaNeverExceedsTheHost)
{
    EXPECT_EQ(os::cgroup_processor_count(4, 800000, 100000), 4);
}

TEST(Os, NoCgroupQuotaLeavesTheHostCount)
{
    EXPECT_EQ(os::cgroup_processor_count(8, -1, 100000), 8);
    EXPECT_EQ(os::cgroup_processor_count(8, -2, 100000), 8);
    EXPECT_EQ(os::cgroup_processor_count(8, 200000, 0), 8);
    EXPECT_EQ(os::cgroup_processor_count(8, 200000, -2), 8);
}

}  // namespace
}  // namespace endgate
