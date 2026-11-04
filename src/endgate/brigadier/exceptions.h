#pragma once

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <string>

#include "endgate/brigadier/reader.h"

namespace endgate::brigadier {

class BuiltInExceptions;

/**
 * @see com.mojang.brigadier.exceptions.CommandExceptionType
 */
class CommandExceptionType {
public:
    CommandExceptionType() = default;
    CommandExceptionType(const CommandExceptionType &) = default;
    CommandExceptionType &operator=(const CommandExceptionType &) = default;
    virtual ~CommandExceptionType() = default;
};

/**
 * The error a command fails with, carrying the input and the cursor it stopped at.
 *
 * @see com.mojang.brigadier.exceptions.CommandSyntaxException
 */
class CommandSyntaxException : public std::exception {
public:
    static constexpr int CONTEXT_AMOUNT = 10;
    static const BuiltInExceptions BUILT_IN_EXCEPTIONS;

    CommandSyntaxException(const CommandExceptionType &type, Message message)
        : type_(&type), message_(std::move(message)), full_message_(message_)
    {
    }

    CommandSyntaxException(const CommandExceptionType &type, Message message, std::string input, int cursor)
        : type_(&type), message_(std::move(message)), input_(std::move(input)), cursor_(cursor)
    {
        full_message_ = message_;
        if (const auto context = getContext()) {
            full_message_ += " at position " + std::to_string(cursor_) + ": " + *context;
        }
    }

    [[nodiscard]] const char *what() const noexcept override { return full_message_.c_str(); }
    [[nodiscard]] std::string getMessage() const { return full_message_; }
    [[nodiscard]] const Message &getRawMessage() const { return message_; }

    [[nodiscard]] std::optional<std::string> getContext() const
    {
        if (!input_ || cursor_ < 0) {
            return std::nullopt;
        }
        std::string builder;
        const int cursor = std::min(static_cast<int>(input_->length()), cursor_);
        if (cursor > CONTEXT_AMOUNT) {
            builder += "...";
        }
        const int start = std::max(0, cursor - CONTEXT_AMOUNT);
        builder += input_->substr(start, cursor - start);
        builder += "<--[HERE]";
        return builder;
    }

    [[nodiscard]] const CommandExceptionType &getType() const { return *type_; }
    [[nodiscard]] const std::optional<std::string> &getInput() const { return input_; }
    [[nodiscard]] int getCursor() const { return cursor_; }

private:
    const CommandExceptionType *type_;
    Message message_;
    std::string full_message_;
    std::optional<std::string> input_;
    int cursor_ = -1;
};

/**
 * @see com.mojang.brigadier.exceptions.SimpleCommandExceptionType
 */
class SimpleCommandExceptionType : public CommandExceptionType {
public:
    explicit SimpleCommandExceptionType(Message message) : message_(std::move(message)) {}

    [[nodiscard]] CommandSyntaxException create() const { return {*this, message_}; }

    [[nodiscard]] CommandSyntaxException createWithContext(const StringReader &reader) const
    {
        return {*this, message_, reader.getString(), reader.getCursor()};
    }

    [[nodiscard]] std::string toString() const { return message_; }

private:
    Message message_;
};

/**
 * @see com.mojang.brigadier.exceptions.DynamicCommandExceptionType
 */
class DynamicCommandExceptionType : public CommandExceptionType {
public:
    explicit DynamicCommandExceptionType(std::function<Message(const std::string &)> function)
        : function_(std::move(function))
    {
    }

    [[nodiscard]] CommandSyntaxException create(const std::string &arg) const { return {*this, function_(arg)}; }

    [[nodiscard]] CommandSyntaxException createWithContext(const StringReader &reader, const std::string &arg) const
    {
        return {*this, function_(arg), reader.getString(), reader.getCursor()};
    }

private:
    std::function<Message(const std::string &)> function_;
};

/**
 * @see com.mojang.brigadier.exceptions.BuiltInExceptions
 * @see com.mojang.brigadier.exceptions.BuiltInExceptionProvider
 */
class BuiltInExceptions {
public:
    [[nodiscard]] const DynamicCommandExceptionType &literalIncorrect() const { return literal_incorrect_; }

    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedStartOfQuote() const
    {
        return reader_expected_start_of_quote_;
    }

    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedEndOfQuote() const
    {
        return reader_expected_end_of_quote_;
    }

    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidEscape() const { return reader_invalid_escape_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidBool() const { return reader_invalid_bool_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidInt() const { return reader_invalid_int_; }
    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedInt() const { return reader_expected_int_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidLong() const { return reader_invalid_long_; }
    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedLong() const { return reader_expected_long_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidDouble() const { return reader_invalid_double_; }
    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedDouble() const { return reader_expected_double_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerInvalidFloat() const { return reader_invalid_float_; }
    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedFloat() const { return reader_expected_float_; }
    [[nodiscard]] const SimpleCommandExceptionType &readerExpectedBool() const { return reader_expected_bool_; }
    [[nodiscard]] const DynamicCommandExceptionType &readerExpectedSymbol() const { return reader_expected_symbol_; }

    [[nodiscard]] const SimpleCommandExceptionType &dispatcherUnknownCommand() const
    {
        return dispatcher_unknown_command_;
    }

    [[nodiscard]] const SimpleCommandExceptionType &dispatcherUnknownArgument() const
    {
        return dispatcher_unknown_argument_;
    }

    [[nodiscard]] const SimpleCommandExceptionType &dispatcherExpectedArgumentSeparator() const
    {
        return dispatcher_expected_argument_separator_;
    }

    [[nodiscard]] const DynamicCommandExceptionType &dispatcherParseException() const
    {
        return dispatcher_parse_exception_;
    }

private:
    DynamicCommandExceptionType literal_incorrect_{
        [](const std::string &expected) { return "Expected literal " + expected; }};
    SimpleCommandExceptionType reader_expected_start_of_quote_{"Expected quote to start a string"};
    SimpleCommandExceptionType reader_expected_end_of_quote_{"Unclosed quoted string"};
    DynamicCommandExceptionType reader_invalid_escape_{
        [](const std::string &character) { return "Invalid escape sequence '" + character + "' in quoted string"; }};
    DynamicCommandExceptionType reader_invalid_bool_{
        [](const std::string &value) { return "Invalid bool, expected true or false but found '" + value + "'"; }};
    DynamicCommandExceptionType reader_invalid_int_{
        [](const std::string &value) { return "Invalid integer '" + value + "'"; }};
    SimpleCommandExceptionType reader_expected_int_{"Expected integer"};
    DynamicCommandExceptionType reader_invalid_long_{
        [](const std::string &value) { return "Invalid long '" + value + "'"; }};
    SimpleCommandExceptionType reader_expected_long_{"Expected long"};
    DynamicCommandExceptionType reader_invalid_double_{
        [](const std::string &value) { return "Invalid double '" + value + "'"; }};
    SimpleCommandExceptionType reader_expected_double_{"Expected double"};
    DynamicCommandExceptionType reader_invalid_float_{
        [](const std::string &value) { return "Invalid float '" + value + "'"; }};
    SimpleCommandExceptionType reader_expected_float_{"Expected float"};
    SimpleCommandExceptionType reader_expected_bool_{"Expected bool"};
    DynamicCommandExceptionType reader_expected_symbol_{
        [](const std::string &symbol) { return "Expected '" + symbol + "'"; }};
    SimpleCommandExceptionType dispatcher_unknown_command_{"Unknown command"};
    SimpleCommandExceptionType dispatcher_unknown_argument_{"Incorrect argument for command"};
    SimpleCommandExceptionType dispatcher_expected_argument_separator_{
        "Expected whitespace to end one argument, but found trailing data"};
    DynamicCommandExceptionType dispatcher_parse_exception_{
        [](const std::string &message) { return "Could not parse command: " + message; }};
};

inline const BuiltInExceptions CommandSyntaxException::BUILT_IN_EXCEPTIONS{};

}  // namespace endgate::brigadier
