#pragma once

#include <string>

namespace endgate::os {

/**
 * The name of the operating system, as the JDK names it.
 *
 * @see java.lang.System#getProperty("os.name")
 * @see src/java.base/windows/native/libjava/java_props_md.c
 * @see src/java.base/unix/native/libjava/java_props_md.c
 */
[[nodiscard]] std::string name();

/**
 * The version of the operating system: the NT version on Windows, the kernel release elsewhere.
 *
 * @see java.lang.System#getProperty("os.version")
 * @see src/java.base/windows/native/libjava/java_props_md.c
 * @see src/java.base/unix/native/libjava/java_props_md.c
 */
[[nodiscard]] std::string version();

/**
 * The architecture the program was built for: amd64 or aarch64.
 *
 * @see java.lang.System#getProperty("os.arch")
 * @see src/java.base/windows/native/libjava/java_props_md.c
 * @see make/autoconf/platform.m4
 */
[[nodiscard]] std::string arch();

/**
 * The number of processors the program may use: the affinity of the process, limited by the job object on Windows and
 * by the cgroup CPU quota on Linux.
 *
 * @see java.lang.Runtime#availableProcessors
 * @see os::active_processor_count in src/hotspot/os/windows/os_windows.cpp
 * @see os::active_processor_count in src/hotspot/os/linux/os_linux.cpp
 */
[[nodiscard]] int active_processor_count();

/**
 * The name Windows gets from its version, build number and product type.
 *
 * @see src/java.base/windows/native/libjava/java_props_md.c
 */
[[nodiscard]] std::string windows_name(int major, int minor, int build, bool workstation, bool is_64bit);

/**
 * The number of processors a cgroup CPU quota allows, never more than the host has.
 *
 * @see CgroupUtil::processor_count in src/hotspot/os/linux/cgroupUtil_linux.cpp
 */
[[nodiscard]] int cgroup_processor_count(int host_cpus, int quota, int period);

}  // namespace endgate::os
