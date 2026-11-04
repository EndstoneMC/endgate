#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace endgate {

/**
 * A simple config for bStats.
 *
 * @see org.bstats.config.MetricsConfig
 */
class MetricsConfig {
public:
    /**
     * Reads the config file, creating it first when it does not exist. Throws std::ios_base::failure when the file
     * cannot be written or read.
     */
    MetricsConfig(std::filesystem::path file, bool default_enabled);

    [[nodiscard]] const std::string &getServerUUID() const { return *server_uuid_; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }
    [[nodiscard]] bool isLogErrorsEnabled() const { return log_errors_; }
    [[nodiscard]] bool isLogSentDataEnabled() const { return log_sent_data_; }
    [[nodiscard]] bool isLogResponseStatusTextEnabled() const { return log_response_status_text_; }

    /**
     * Checks whether the config file did exist before or not.
     *
     * @see org.bstats.config.MetricsConfig#didExistBefore
     */
    [[nodiscard]] bool didExistBefore() const { return did_exist_before_; }

private:
    void setupConfig();
    void writeConfig() const;
    void readConfig();

    std::filesystem::path file_;
    bool default_enabled_;
    std::optional<std::string> server_uuid_;
    bool enabled_ = false;
    bool log_errors_ = false;
    bool log_sent_data_ = false;
    bool log_response_status_text_ = false;
    bool did_exist_before_ = true;
};

}  // namespace endgate
