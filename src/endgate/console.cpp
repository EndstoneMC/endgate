#include "endgate/console.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#include "endgate/logging/appender.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace endgate {

namespace {
constexpr auto HISTORY_FILE = ".console_history";
}  // namespace

Console::Console(ProxyServer &server) : server_(server), logger_(LogManager::getLogger(*this))
{
    constexpr std::size_t max_history_size = 1000;

#ifdef _WIN32
    const bool interactive = _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    const bool interactive = isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
    if (interactive) {
        rx_ = replxx::Replxx{};
        rx_->bind_key_internal(replxx::Replxx::KEY::control('J'), "commit_line");
        rx_->set_max_history_size(max_history_size);
        rx_->history_load(HISTORY_FILE);
        rx_->set_completion_callback([this](const std::string &input, int &context_len) {
            replxx::Replxx::completions_t completions;
            try {
                for (const std::string &offer :
                     server_.getCommandManager().offerSuggestions(shared_from_this(), input)) {
                    completions.emplace_back(offer);
                }
            }
            catch (const std::exception &e) {
                logger_.error("An error occurred while trying to perform tab completion: {}", e.what());
            }
            const auto word = input.substr(input.find_last_of(' ') + 1);
            context_len =
                static_cast<int>(std::ranges::count_if(word, [](unsigned char c) { return (c & 0xC0) != 0x80; }));
            return completions;
        });
    }
}

Console::~Console()
{
    if (rx_.has_value()) {
        rx_->history_save(HISTORY_FILE);
    }
}

void Console::start()
{
    constexpr auto prompt = "> ";
    constexpr auto whitespace = " \t\r\n";

    TerminalConsoleAppender::setReader(this);

    while (isRunning()) {
        std::string line;
        if (rx_.has_value()) {
            errno = 0;
            const char *input = rx_->input(prompt);
            if (input == nullptr) {
                if (errno == EAGAIN) {
                    shutdown();
                }
                break;
            }
            line = input;
        }
        else if (!std::getline(std::cin, line)) {
            break;
        }

        const auto first = line.find_first_not_of(whitespace);
        line = first == std::string::npos ? std::string{}
                                          : line.substr(first, line.find_last_not_of(whitespace) - first + 1);
        if (line.empty()) {
            continue;
        }

        if (rx_.has_value()) {
            rx_->history_add(line);
        }
        runCommand(line);
    }

    TerminalConsoleAppender::setReader(nullptr);
}

void Console::sendMessage(std::string_view message)
{
    logger_.info("{}", message);
}

void Console::printAbove(std::string_view message)
{
    const bool has_newline = !message.empty() && message.back() == '\n';
    if (rx_.has_value()) {
        rx_->print("%.*s%s", static_cast<int>(message.size()), message.data(), has_newline ? "" : "\n");
        return;
    }

    std::cout.write(message.data(), static_cast<std::streamsize>(message.size()));
    if (!has_newline) {
        std::cout.put('\n');
    }
    std::cout.flush();
}

bool Console::isRunning() const
{
    return !server_.isShuttingDown();
}

void Console::runCommand(const std::string &command)
{
    try {
        if (!server_.getCommandManager().executeImmediatelyAsync(shared_from_this(), command)) {
            sendMessage("§cThis command does not exist.");
            return;
        }
        if (server_.getConfiguration().isLogCommandExecutions()) {
            logger_.info("CONSOLE -> executed command /{}", command);
        }
    }
    catch (const std::exception &e) {
        logger_.error("An error occurred while running this command: {}", e.what());
    }
}

void Console::shutdown()
{
    server_.shutdown(true);
}

}  // namespace endgate
