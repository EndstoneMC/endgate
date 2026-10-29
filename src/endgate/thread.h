#pragma once

#include <string>

namespace endgate {

/**
 * The calling thread's name, kept in full for the log; the operating system keeps only its first 15
 * characters.
 *
 * @see java.lang.Thread
 */
class Thread {
public:
    /**
     * @see java.lang.Thread#getName
     */
    [[nodiscard]] static const std::string &getName();

    /**
     * @see java.lang.Thread#setName
     */
    static void setName(std::string name);
};

}  // namespace endgate
