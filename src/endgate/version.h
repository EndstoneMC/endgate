#pragma once

#include <string>
#include <utility>

namespace endgate {

/**
 * Represents the version of the proxy.
 *
 * @see com.velocitypowered.api.util.ProxyVersion
 */
class ProxyVersion {
public:
    ProxyVersion(std::string name, std::string vendor, std::string version)
        : name_(std::move(name)), vendor_(std::move(vendor)), version_(std::move(version))
    {
    }

    [[nodiscard]] const std::string &getName() const { return name_; }

    [[nodiscard]] const std::string &getVendor() const { return vendor_; }

    [[nodiscard]] const std::string &getVersion() const { return version_; }

private:
    std::string name_;
    std::string vendor_;
    std::string version_;
};

}  // namespace endgate
