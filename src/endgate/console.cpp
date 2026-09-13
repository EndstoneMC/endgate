#include "endgate/console.h"

#include <cerrno>
#include <cstdio>
#include <iostream>

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
constexpr auto history_file = ".console_history";

bool isInteractive()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}
}  // namespace

Console::Console(ProxyServer &server) : server_(server), logger_(LogManager::getLogger())
{
    constexpr std::size_t max_history_size = 1000;

    if (isInteractive()) {
        rx_ = replxx::Replxx{};
        rx_->bind_key_internal(replxx::Replxx::KEY::control('J'), "commit_line");
        rx_->set_max_history_size(max_history_size);
        rx_->history_load(history_file);
    }
}

Console::~Console()
{
    if (rx_.has_value()) {
        rx_->history_save(history_file);
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
    return !server_.isShutdown();
}

void Console::runCommand(const std::string &command)
{
    if (command == "shutdown" || command == "end" || command == "stop") {
        shutdown();
        return;
    }
    sendMessage("This command does not exist.");
}

void Console::shutdown()
{
    server_.shutdown(true);
}

}  // namespace endgate
