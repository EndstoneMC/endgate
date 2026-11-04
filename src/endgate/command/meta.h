#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace endgate {

/**
 * Contains the metadata of a registered command.
 *
 * @see com.velocitypowered.api.command.CommandMeta
 * @see com.velocitypowered.proxy.command.VelocityCommandMeta
 */
class CommandMeta {
public:
    /**
     * Provides a fluent interface to create CommandMetas.
     *
     * @see com.velocitypowered.api.command.CommandMeta.Builder
     * @see com.velocitypowered.proxy.command.VelocityCommandMeta.Builder
     */
    class Builder {
    public:
        explicit Builder(const std::string &alias) { add(alias); }

        /**
         * Specifies additional aliases that can be used to run the command.
         *
         * @see com.velocitypowered.api.command.CommandMeta.Builder#aliases
         */
        Builder &aliases(const std::vector<std::string> &aliases)
        {
            for (const std::string &alias : aliases) {
                add(alias);
            }
            return *this;
        }

        /**
         * @see com.velocitypowered.api.command.CommandMeta.Builder#build
         */
        [[nodiscard]] CommandMeta build() const { return CommandMeta(aliases_); }

    private:
        void add(std::string alias)
        {
            std::ranges::transform(alias, alias.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::ranges::find(aliases_, alias) == aliases_.end()) {
                aliases_.push_back(std::move(alias));
            }
        }

        std::vector<std::string> aliases_;
    };

    /**
     * Returns the case-insensitive aliases of the command, in lowercase.
     *
     * @see com.velocitypowered.api.command.CommandMeta#getAliases
     */
    [[nodiscard]] const std::vector<std::string> &getAliases() const { return aliases_; }

    bool operator==(const CommandMeta &) const = default;

private:
    explicit CommandMeta(std::vector<std::string> aliases) : aliases_(std::move(aliases)) {}

    std::vector<std::string> aliases_;
};

}  // namespace endgate
