#include "endgate/auth/keys.h"

#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_future.hpp>
#include <date/date.h>
#include <glaze/json.hpp>

#include "endgate/logging/manager.h"

namespace endgate {

namespace {

constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

struct AuthEnvironment {
    std::string service_uri;

    struct glaze {
        using T = AuthEnvironment;
        static constexpr auto value = glz::object("serviceUri", &T::service_uri);
    };
};

struct AuthEnvironments {
    AuthEnvironment prod;

    struct glaze {
        using T = AuthEnvironments;
        static constexpr auto value = glz::object("prod", &T::prod);
    };
};

struct ServiceEnvironments {
    AuthEnvironments auth;

    struct glaze {
        using T = ServiceEnvironments;
        static constexpr auto value = glz::object("auth", &T::auth);
    };
};

struct DiscoveryResult {
    ServiceEnvironments service_environments;

    struct glaze {
        using T = DiscoveryResult;
        static constexpr auto value = glz::object("serviceEnvironments", &T::service_environments);
    };
};

struct Discovery {
    DiscoveryResult result;

    struct glaze {
        using T = Discovery;
        static constexpr auto value = glz::object("result", &T::result);
    };
};

struct OpenIdConfiguration {
    std::string jwks_uri;
    std::string issuer;

    struct glaze {
        using T = OpenIdConfiguration;
        static constexpr auto value = glz::object("jwks_uri", &T::jwks_uri, "issuer", &T::issuer);
    };
};

struct JsonWebKeySet {
    std::vector<glz::raw_json> keys;

    struct glaze {
        using T = JsonWebKeySet;
        static constexpr auto value = glz::object("keys", &T::keys);
    };
};

struct KeyId {
    std::string kid;

    struct glaze {
        using T = KeyId;
        static constexpr auto value = glz::object("kid", &T::kid);
    };
};

struct Fetched {
    std::map<std::string, PublicKey, std::less<>> keys;
    std::string issuer;
    std::chrono::system_clock::duration offset;
};

std::optional<std::chrono::system_clock::time_point> parseHttpDate(std::string_view text)
{
    std::istringstream stream{std::string(text)};
    date::sys_seconds parsed;
    stream >> date::parse("%a, %d %b %Y %H:%M:%S GMT", parsed);
    if (stream.fail() || stream.peek() != std::istringstream::traits_type::eof()) {
        return std::nullopt;
    }
    return parsed;
}

Awaitable<std::expected<Fetched, std::string>> download(const HttpClient &client, std::string discovery_uri,
                                                        std::string user_agent)
{
    const std::vector<std::pair<std::string, std::string>> headers{{"User-Agent", std::move(user_agent)}};

    const auto discovery = co_await client.sendAsync({.uri = std::move(discovery_uri), .headers = headers});
    if (!discovery) {
        co_return std::unexpected(discovery.error());
    }
    Discovery services;
    if (discovery->statusCode() / 100 != 2 || glz::read<LENIENT_READ>(services, discovery->body())) {
        co_return std::unexpected(std::format("discovery answered {}", discovery->statusCode()));
    }
    auto auth = std::move(services.result.service_environments.auth.prod.service_uri);
    while (auth.ends_with('/')) {
        auth.pop_back();
    }

    const auto openid =
        co_await client.sendAsync({.uri = auth + "/.well-known/openid-configuration", .headers = headers});
    if (!openid) {
        co_return std::unexpected(openid.error());
    }
    OpenIdConfiguration configuration;
    if (openid->statusCode() / 100 != 2 || glz::read<LENIENT_READ>(configuration, openid->body()) ||
        configuration.jwks_uri.empty() || configuration.issuer.empty()) {
        co_return std::unexpected(
            std::format("the OpenID configuration at {} answered {}", auth, openid->statusCode()));
    }
    std::chrono::system_clock::duration offset{};
    if (const auto date = openid->firstValue("date")) {
        if (const auto service_time = parseHttpDate(*date)) {
            offset = *service_time - std::chrono::system_clock::now();
        }
    }

    const auto jwks = co_await client.sendAsync({.uri = configuration.jwks_uri, .headers = headers});
    if (!jwks) {
        co_return std::unexpected(jwks.error());
    }
    JsonWebKeySet set;
    if (jwks->statusCode() / 100 != 2 || glz::read<LENIENT_READ>(set, jwks->body())) {
        co_return std::unexpected(
            std::format("the key set at {} answered {}", configuration.jwks_uri, jwks->statusCode()));
    }
    Fetched fetched{.keys = {}, .issuer = std::move(configuration.issuer), .offset = offset};
    for (const auto &entry : set.keys) {
        KeyId id;
        if (glz::read<LENIENT_READ>(id, entry.str) || id.kid.empty()) {
            continue;
        }
        auto key = decodeJsonWebKey(entry.str);
        if (!key) {
            continue;
        }
        const auto bits = EVP_PKEY_get_bits(key.get());
        if (EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_RSA && bits != 1024 && bits != 2048 && bits != 4096) {
            continue;
        }
        fetched.keys.try_emplace(std::move(id.kid), std::move(key));
    }
    if (fetched.keys.empty()) {
        co_return std::unexpected(std::format("the key set at {} holds no usable keys", configuration.jwks_uri));
    }
    co_return fetched;
}

}  // namespace

MinecraftServiceKeyManager::MinecraftServiceKeyManager(HttpClient client, std::string user_agent,
                                                       std::string discovery_uri)
    : logger_(LogManager::getLogger(*this)), client_(std::move(client)), user_agent_(std::move(user_agent)),
      discovery_uri_(std::move(discovery_uri))
{
}

MinecraftServiceKeyManager::~MinecraftServiceKeyManager()
{
    shutdown();
}

void MinecraftServiceKeyManager::start()
{
    {
        const std::scoped_lock lock(mutex_);
        fetching_ = true;
        last_fetch_started_ = std::chrono::steady_clock::now();
    }
    asio::co_spawn(client_.executor().getExecutor(), fetch(), asio::use_future).wait();
}

void MinecraftServiceKeyManager::shutdown()
{
    const std::scoped_lock lock(mutex_);
    shut_down_ = true;
    if (next_fetch_) {
        next_fetch_->cancel();
    }
}

bool MinecraftServiceKeyManager::hasKeys() const
{
    const std::scoped_lock lock(mutex_);
    return !keys_.empty();
}

PublicKey MinecraftServiceKeyManager::getKey(std::string_view key_id) const
{
    const std::scoped_lock lock(mutex_);
    const auto key = keys_.find(key_id);
    return key == keys_.end() ? nullptr : key->second;
}

std::string MinecraftServiceKeyManager::getIssuer() const
{
    const std::scoped_lock lock(mutex_);
    return issuer_;
}

std::chrono::system_clock::time_point MinecraftServiceKeyManager::now() const
{
    const std::scoped_lock lock(mutex_);
    return std::chrono::system_clock::now() + offset_;
}

void MinecraftServiceKeyManager::onSignatureFailure()
{
    constexpr std::chrono::seconds refetch_after{600};

    const std::scoped_lock lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (shut_down_ || fetching_ || now - last_fetch_started_ <= refetch_after) {
        return;
    }
    fetching_ = true;
    last_fetch_started_ = now;
    asio::co_spawn(client_.executor().getExecutor(), fetch(), asio::detached);
}

Awaitable<void> MinecraftServiceKeyManager::fetch()
{
    constexpr std::chrono::seconds clock_warning{601};

    const auto self = shared_from_this();
    auto fetched = co_await download(client_, discovery_uri_, user_agent_);

    const std::scoped_lock lock(mutex_);
    fetching_ = false;
    if (!fetched) {
        if (keys_.empty()) {
            logger_.error("Could not connect to Minecraft services. This is required to accept connections in "
                          "online mode.");
            logger_.debug("Unable to fetch the keys of Minecraft services: {}", fetched.error());
        }
        else {
            logger_.warn("Unable to refresh the keys of Minecraft services: {}", fetched.error());
        }
    }
    else {
        keys_ = std::move(fetched->keys);
        issuer_ = std::move(fetched->issuer);
        offset_ = fetched->offset;
        logger_.debug("Fetched {} keys from Minecraft services", keys_.size());
        if (std::chrono::abs(offset_) >= clock_warning) {
            logger_.warn("The system clock is {}s off the clock of Minecraft services",
                         std::chrono::duration_cast<std::chrono::seconds>(offset_).count());
        }
    }
    scheduleNextFetch();
}

void MinecraftServiceKeyManager::scheduleNextFetch()
{
    constexpr std::chrono::seconds retry_after{60};
    constexpr std::chrono::days renew_after{7};

    if (shut_down_) {
        return;
    }
    if (next_fetch_) {
        next_fetch_->cancel();
    }
    const auto delay = keys_.empty() ? std::chrono::steady_clock::duration{retry_after}
                                     : last_fetch_started_ + renew_after - std::chrono::steady_clock::now();
    next_fetch_ = client_.executor().schedule(
        [weak = weak_from_this()] {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            const std::scoped_lock lock(self->mutex_);
            if (self->shut_down_ || self->fetching_) {
                return;
            }
            self->fetching_ = true;
            self->last_fetch_started_ = std::chrono::steady_clock::now();
            asio::co_spawn(self->client_.executor().getExecutor(), self->fetch(), asio::detached);
        },
        delay);
}

}  // namespace endgate
