#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace endgate::brigadier {

class SimpleCommandExceptionType;
class DynamicCommandExceptionType;

using Message = std::string;

/**
 * Reads a command line one character at a time, with the cursor Brigadier's arguments parse from.
 *
 * @see com.mojang.brigadier.StringReader
 * @see com.mojang.brigadier.ImmutableStringReader
 */
class StringReader {
public:
    StringReader(const StringReader &other) = default;
    explicit StringReader(std::string string) : string_(std::move(string)) {}

    [[nodiscard]] const std::string &getString() const { return string_; }
    void setCursor(int cursor) { cursor_ = cursor; }
    [[nodiscard]] int getRemainingLength() const { return getTotalLength() - cursor_; }
    [[nodiscard]] int getTotalLength() const { return static_cast<int>(string_.length()); }
    [[nodiscard]] int getCursor() const { return cursor_; }
    [[nodiscard]] std::string getRead() const { return string_.substr(0, cursor_); }
    [[nodiscard]] std::string getRemaining() const { return string_.substr(cursor_); }
    [[nodiscard]] bool canRead(int length) const { return cursor_ + length <= getTotalLength(); }
    [[nodiscard]] bool canRead() const { return canRead(1); }
    [[nodiscard]] char peek() const { return string_.at(cursor_); }
    [[nodiscard]] char peek(int offset) const { return string_.at(cursor_ + offset); }
    char read() { return string_.at(cursor_++); }
    void skip() { cursor_++; }

    static bool isAllowedNumber(char c);
    static bool isQuotedStringStart(char c);
    void skipWhitespace();
    int readInt();
    std::int64_t readLong();
    double readDouble();
    float readFloat();
    static bool isAllowedInUnquotedString(char c);
    std::string readUnquotedString();
    std::string readQuotedString();
    std::string readStringUntil(char terminator);
    std::string readString();
    bool readBoolean();
    void expect(char c);

private:
    template <typename T>
    T readNumber(const SimpleCommandExceptionType &expected, const DynamicCommandExceptionType &invalid);

    static constexpr char SYNTAX_ESCAPE = '\\';
    static constexpr char SYNTAX_DOUBLE_QUOTE = '"';
    static constexpr char SYNTAX_SINGLE_QUOTE = '\'';

    std::string string_;
    int cursor_ = 0;
};

}  // namespace endgate::brigadier
