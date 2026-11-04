#include "endgate/bstats/config.h"

#include <array>
#include <fstream>
#include <ios>
#include <sstream>
#include <string_view>
#include <utility>

#include <glaze/toml.hpp>

#include "endgate/util/uuid.h"

namespace endgate {

namespace {

constexpr glz::opts LENIENT_READ{.format = glz::TOML, .error_on_unknown_keys = false};

constexpr std::array<std::string_view, 5> COMMENTS{
    "bStats (https://bStats.org) collects some basic information for plugin authors, like",
    "how many people use their plugin and their total player count. It's recommended to keep",
    "bStats enabled, but if you're not comfortable with this, you can turn this setting off.",
    "There is no performance penalty associated with having metrics enabled, and data sent to",
    "bStats is fully anonymous.",
};

struct Document {
    bool enabled = true;
    std::optional<std::string> server_uuid;
    bool log_errors = false;
    bool log_sent_data = false;
    bool log_response_status_text = false;

    struct glaze {
        using T = Document;
        static constexpr auto value = glz::object(  //
            "enabled", &T::enabled,                 //
            "server-uuid", &T::server_uuid,         //
            "log-errors", &T::log_errors,           //
            "log-sent-data", &T::log_sent_data,     //
            "log-response-status-text", &T::log_response_status_text);
    };
};

}  // namespace

MetricsConfig::MetricsConfig(std::filesystem::path file, bool default_enabled)
    : file_(std::move(file)), default_enabled_(default_enabled)
{
    setupConfig();
}

void MetricsConfig::setupConfig()
{
    if (!std::filesystem::exists(file_)) {
        did_exist_before_ = false;
        writeConfig();
    }
    readConfig();
    if (!server_uuid_) {
        writeConfig();
        readConfig();
    }
}

void MetricsConfig::writeConfig() const
{
    const Document document{
        .enabled = default_enabled_,
        .server_uuid = UUID::randomUUID().toString(),
    };
    const auto body = glz::write_toml(document);
    if (!body) {
        throw std::ios_base::failure(glz::format_error(body.error()));
    }

    if (file_.has_parent_path()) {
        std::filesystem::create_directories(file_.parent_path());
    }
    std::ofstream output(file_);
    if (!output) {
        throw std::ios_base::failure("unable to write " + file_.string());
    }
    for (const auto comment : COMMENTS) {
        output << "# " << comment << '\n';
    }
    output << *body;
}

void MetricsConfig::readConfig()
{
    std::ifstream input(file_);
    if (!input) {
        throw std::ios_base::failure("unable to read " + file_.string());
    }
    std::ostringstream text;
    text << input.rdbuf();
    const auto content = text.str();

    Document document;
    if (const auto error = glz::read<LENIENT_READ>(document, content)) {
        throw std::ios_base::failure(glz::format_error(error, content));
    }
    enabled_ = document.enabled;
    server_uuid_ = std::move(document.server_uuid);
    log_errors_ = document.log_errors;
    log_sent_data_ = document.log_sent_data;
    log_response_status_text_ = document.log_response_status_text;
}

}  // namespace endgate
