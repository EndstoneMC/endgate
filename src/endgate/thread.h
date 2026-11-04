#pragma once

#include <string>

namespace endgate {

/**
 * The calling thread's name. Windows 10 and later keep it in full as the thread's description; elsewhere
 * the operating system keeps only its first 15 characters, so the full name stays with the thread.
 *
 * @see java.lang.Thread
 */
class Thread {
public:
    /**
     * @see java.lang.Thread#getName
     * @see GetThreadDescription
     */
    [[nodiscard]] static std::string getName();

    /**
     * @see java.lang.Thread#setName
     * @see SetThreadDescription
     */
    static void setName(std::string name);
};

}  // namespace endgate
