#include "endgate/network/query.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/config.h"
#include "endgate/connection/player.h"
#include "endgate/logging/manager.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

namespace {

constexpr std::uint8_t QUERY_MAGIC_FIRST = 0xFE;
constexpr std::uint8_t QUERY_MAGIC_SECOND = 0xFD;
constexpr std::uint8_t QUERY_TYPE_HANDSHAKE = 0x09;
constexpr std::uint8_t QUERY_TYPE_STAT = 0x00;
constexpr std::size_t QUERY_HEADER_SIZE = 7;
constexpr auto SESSION_LIFETIME = std::chrono::seconds(30);
constexpr std::array<std::uint8_t, 11> QUERY_RESPONSE_FULL_PADDING{0x73, 0x70, 0x6C, 0x69, 0x74, 0x6E,
                                                                   0x75, 0x6D, 0x00, 0x80, 0x00};
constexpr std::array<std::uint8_t, 10> QUERY_RESPONSE_FULL_PADDING2{0x01, 0x70, 0x6C, 0x61, 0x79,
                                                                    0x65, 0x72, 0x5F, 0x00, 0x00};

void writeString(ByteBuf &buf, std::string_view string)
{
    buf.writeBytes(std::as_bytes(std::span{string}));
    buf.writeByte(0x00);
}

class ResponseWriter {
public:
    ResponseWriter(ByteBuf &buf, bool is_basic) : buf_(buf), is_basic_(is_basic)
    {
        if (!is_basic) {
            buf.writeBytes(std::as_bytes(std::span{QUERY_RESPONSE_FULL_PADDING}));
        }
    }

    void write(std::string_view key, std::string_view value)
    {
        if (is_basic_) {
            constexpr std::array<std::string_view, 7> basic{"hostname",   "gametype", "map",   "numplayers",
                                                            "maxplayers", "hostport", "hostip"};
            if (std::ranges::find(basic, key) == basic.end()) {
                return;
            }
            if (key == "hostport") {
                std::uint16_t port = 0;
                std::from_chars(value.data(), value.data() + value.size(), port);
                buf_.writeShortLE(port);
            }
            else {
                writeString(buf_, value);
            }
        }
        else {
            writeString(buf_, key);
            writeString(buf_, value);
        }
    }

    void write(std::string_view key, int value) { write(key, std::to_string(value)); }

    void writePlayers(const std::vector<std::string> &players)
    {
        if (is_basic_) {
            return;
        }
        buf_.writeByte(0x00);
        buf_.writeBytes(std::as_bytes(std::span{QUERY_RESPONSE_FULL_PADDING2}));
        for (const auto &player : players) {
            writeString(buf_, player);
        }
        buf_.writeByte(0x00);
    }

    void writePlugins(std::string_view server_version)
    {
        if (is_basic_) {
            return;
        }
        write("plugins", std::string{server_version} + ": ");
    }

private:
    ByteBuf &buf_;
    bool is_basic_;
};

}  // namespace

GameSpyQueryHandler::GameSpyQueryHandler(ProxyServer &server) : server_(server), logger_(LogManager::getLogger(*this))
{
}

bool GameSpyQueryHandler::isQuery(std::span<const std::byte> datagram)
{
    return datagram.size() >= 2 && datagram[0] == static_cast<std::byte>(QUERY_MAGIC_FIRST) &&
           datagram[1] == static_cast<std::byte>(QUERY_MAGIC_SECOND);
}

bool GameSpyQueryHandler::queryBind(EventLoop &event_loop, const asio::ip::address &address, std::uint16_t port)
{
    asio::error_code ec;
    const asio::ip::udp::endpoint local{address, port};
    const auto socket = std::make_shared<asio::ip::udp::socket>(event_loop.getExecutor());
    if (socket->open(local.protocol(), ec) ||
        (address.is_v6() && address.is_unspecified() && socket->set_option(asio::ip::v6_only(false), ec)) ||
        socket->bind(local, ec) || socket->non_blocking(true, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }
    socket_ = socket;
    logger_.info("Listening for GS4 query on {}:{}", address.to_string(), socket->local_endpoint().port());
    event_loop.execute([this, socket] { asio::co_spawn(socket->get_executor(), receive(socket), asio::detached); });
    return true;
}

void GameSpyQueryHandler::close()
{
    if (auto socket = std::move(socket_)) {
        asio::post(socket->get_executor(), [socket] {
            asio::error_code ignored;
            std::ignore = socket->close(ignored);
        });
    }
}

Awaitable<void> GameSpyQueryHandler::receive(std::shared_ptr<asio::ip::udp::socket> socket)
{
    std::array<std::byte, 2048> buffer{};
    asio::ip::udp::endpoint sender;
    for (;;) {
        const auto [ec, size] =
            co_await socket->async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        channelRead0(std::span{buffer}.first(size), sender, *socket);
    }
}

void GameSpyQueryHandler::channelRead0(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender,
                                       asio::ip::udp::socket &socket)
{
    if (datagram.size() < QUERY_HEADER_SIZE) {
        return;
    }
    auto query_message = Unpooled::wrappedBuffer(datagram);
    const auto sender_address = sender.address();

    if (!isQuery(datagram)) {
        return;
    }
    query_message.skipBytes(2);

    const auto type = query_message.readUnsignedByte();
    const auto session_id = query_message.readInt();

    ByteBuf response = Unpooled::buffer();
    switch (type) {
    case QUERY_TYPE_HANDSHAKE: {
        const auto challenge_token = static_cast<std::int32_t>(std::random_device{}());
        {
            const std::scoped_lock lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(sessions_, [now](const auto &session) { return session.second.second <= now; });
            sessions_.insert_or_assign(sender_address, std::pair{challenge_token, now + SESSION_LIFETIME});
        }
        response.writeByte(QUERY_TYPE_HANDSHAKE);
        response.writeInt(static_cast<std::uint32_t>(session_id));
        writeString(response, std::to_string(challenge_token));
        break;
    }

    case QUERY_TYPE_STAT: {
        if (!query_message.isReadable(4)) {
            return;
        }
        const auto challenge_token = query_message.readInt();
        {
            const std::scoped_lock lock(mutex_);
            const auto session = sessions_.find(sender_address);
            if (session == sessions_.end() || session->second.second <= std::chrono::steady_clock::now() ||
                session->second.first != challenge_token) {
                return;
            }
        }
        if (query_message.readableBytes() != 0 && query_message.readableBytes() != 4) {
            return;
        }
        response = createStatResponse(session_id, !query_message.isReadable());
        break;
    }
    default:
        return;
    }

    asio::error_code ignored;
    socket.send_to(asio::buffer(response.nioBuffer()), sender, 0, ignored);
}

ByteBuf GameSpyQueryHandler::createStatResponse(std::int32_t session_id, bool is_basic)
{
    const auto &configuration = server_.getConfiguration();
    const auto &bind = configuration.getBind();
    const auto colon = bind.rfind(':');
    const auto host = bind.substr(0, colon);
    int port = 0;
    if (colon != std::string::npos) {
        const auto digits = std::string_view{bind}.substr(colon + 1);
        std::from_chars(digits.data(), digits.data() + digits.size(), port);
    }

    std::vector<std::string> players;
    for (const auto &player : server_.getAllPlayers()) {
        players.push_back(player->getUsername());
    }

    ByteBuf response = Unpooled::buffer();
    response.writeByte(QUERY_TYPE_STAT);
    response.writeInt(static_cast<std::uint32_t>(session_id));

    ResponseWriter writer(response, is_basic);
    writer.write("hostname", configuration.getMotd());
    writer.write("gametype", "SMP");
    writer.write("game_id", "MINECRAFTPE");
    writer.write("version", MAXIMUM_VERSION.getName());
    writer.writePlugins(ProxyServer::getVersion().getName());
    writer.write("map", configuration.getQueryMap());
    writer.write("numplayers", static_cast<int>(players.size()));
    writer.write("maxplayers", configuration.getShowMaxPlayers());
    writer.write("hostport", port);
    writer.write("hostip", host);
    writer.writePlayers(players);
    return response;
}

}  // namespace endgate
