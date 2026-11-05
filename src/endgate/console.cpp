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
    constexpr int max_hint_rows = 8;

#ifdef _WIN32
    const bool interactive = _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    const bool interactive = isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
    if (interactive) {
        rx_ = replxx::Replxx{};
        rx_->bind_key_internal(replxx::Replxx::KEY::control('J'), "commit_line");
        rx_->set_max_history_size(max_history_size);
        rx_->set_immediate_completion(false);
        rx_->set_max_hint_rows(max_hint_rows);
        rx_->bind_key(replxx::Replxx::KEY::TAB, [this, &rx = *rx_](char32_t code) {
            return complete(rx, replxx::Replxx::ACTION::COMPLETE_NEXT, code);
        });
        rx_->bind_key(replxx::Replxx::KEY::shift(replxx::Replxx::KEY::TAB), [this, &rx = *rx_](char32_t code) {
            return complete(rx, replxx::Replxx::ACTION::COMPLETE_PREVIOUS, code);
        });
        rx_->history_load(HISTORY_FILE);
        rx_->set_completion_callback([this](const std::string &input, int &context_len) {
            replxx::Replxx::completions_t completions;
            candidates_.clear();
            completer_ran_ = true;
            try {
                for (const std::string &offer :
                     server_.getCommandManager().offerSuggestions(shared_from_this(), input)) {
                    completions.emplace_back(offer);
                    candidates_.push_back(offer);
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
        rx_->set_hint_callback([this](const std::string &input, int &context_len, replxx::Replxx::Color &color) {
            context_len = 0;
            color = replxx::Replxx::Color::GRAY;
            return (completing_ || input == tab_line_) && candidates_.size() > 1 ? candidates_
                                                                                 : replxx::Replxx::hints_t{};
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
            candidates_.clear();
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

void Console::sendErrorMessage(std::string_view message)
{
    logger_.error("{}", message);
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
            sendErrorMessage("This command does not exist.");
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

replxx::Replxx::ACTION_RESULT Console::complete(replxx::Replxx &rx, replxx::Replxx::ACTION action, char32_t code)
{
    completer_ran_ = false;
    completing_ = true;
    const auto result = rx.invoke(action, code);
    completing_ = false;
    tab_line_ = rx.get_state().text();
    if (!completer_ran_ || candidates_.size() != 1) {
        return result;
    }

    const std::string text = rx.get_state().text();
    const auto length = std::ranges::count_if(text, [](unsigned char c) { return (c & 0xC0) != 0x80; });
    const auto cursor = rx.get_state().cursor_position();
    if (cursor == length) {
        rx.invoke(replxx::Replxx::ACTION::MOVE_CURSOR_TO_END_OF_LINE, code);
        rx.set_state({(text + ' ').c_str(), cursor + 1});
    }
    return result;
}

void Console::shutdown()
{
    server_.shutdown(true);
}

}  // namespace endgate
