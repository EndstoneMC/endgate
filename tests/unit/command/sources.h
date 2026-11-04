#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "endgate/command/source.h"

namespace endgate {

class TestSource final : public CommandSource {
public:
    explicit TestSource(Tristate fallback = Tristate::True) : fallback_(fallback) {}

    void sendMessage(std::string_view message) override { messages.emplace_back(message); }

    [[nodiscard]] Tristate getPermissionValue(const std::string &permission) const override
    {
        const auto found = permissions.find(permission);
        return found == permissions.end() ? fallback_ : found->second;
    }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(TestSource) || CommandSource::isInstanceOf(target);
    }

    std::vector<std::string> messages;
    std::map<std::string, Tristate> permissions;

private:
    Tristate fallback_;
};

}  // namespace endgate
