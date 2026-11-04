#include "endgate/brigadier/reader.h"

#include <charconv>
#include <system_error>

#include "endgate/brigadier/exceptions.h"

namespace endgate::brigadier {

bool StringReader::isAllowedNumber(char c)
{
    return (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool StringReader::isQuotedStringStart(char c)
{
    return c == SYNTAX_DOUBLE_QUOTE || c == SYNTAX_SINGLE_QUOTE;
}

void StringReader::skipWhitespace()
{
    while (canRead() && (peek() == ' ' || (peek() >= '\t' && peek() <= '\r') || (peek() >= 0x1C && peek() <= 0x1F))) {
        skip();
    }
}

template <typename T>
T StringReader::readNumber(const SimpleCommandExceptionType &expected, const DynamicCommandExceptionType &invalid)
{
    const int start = cursor_;
    while (canRead() && isAllowedNumber(peek())) {
        skip();
    }
    const std::string number = string_.substr(start, cursor_ - start);
    if (number.empty()) {
        throw expected.createWithContext(*this);
    }
    T value{};
    const char *end = number.data() + number.size();
    const auto result = std::from_chars(number.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        cursor_ = start;
        throw invalid.createWithContext(*this, number);
    }
    return value;
}

int StringReader::readInt()
{
    return readNumber<int>(CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedInt(),
                           CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidInt());
}

std::int64_t StringReader::readLong()
{
    return readNumber<std::int64_t>(CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedLong(),
                                    CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidLong());
}

double StringReader::readDouble()
{
    return readNumber<double>(CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedDouble(),
                              CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidDouble());
}

float StringReader::readFloat()
{
    return readNumber<float>(CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedFloat(),
                             CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidFloat());
}

bool StringReader::isAllowedInUnquotedString(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' ||
           c == '.' || c == '+';
}

std::string StringReader::readUnquotedString()
{
    const int start = cursor_;
    while (canRead() && isAllowedInUnquotedString(peek())) {
        skip();
    }
    return string_.substr(start, cursor_ - start);
}

std::string StringReader::readQuotedString()
{
    if (!canRead()) {
        return "";
    }
    const char next = peek();
    if (!isQuotedStringStart(next)) {
        throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedStartOfQuote().createWithContext(*this);
    }
    skip();
    return readStringUntil(next);
}

std::string StringReader::readStringUntil(char terminator)
{
    std::string result;
    bool escaped = false;
    while (canRead()) {
        const char c = read();
        if (escaped) {
            if (c == terminator || c == SYNTAX_ESCAPE) {
                result += c;
                escaped = false;
            }
            else {
                setCursor(getCursor() - 1);
                throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidEscape().createWithContext(
                    *this, std::string(1, c));
            }
        }
        else if (c == SYNTAX_ESCAPE) {
            escaped = true;
        }
        else if (c == terminator) {
            return result;
        }
        else {
            result += c;
        }
    }
    throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedEndOfQuote().createWithContext(*this);
}

std::string StringReader::readString()
{
    if (!canRead()) {
        return "";
    }
    const char next = peek();
    if (isQuotedStringStart(next)) {
        skip();
        return readStringUntil(next);
    }
    return readUnquotedString();
}

bool StringReader::readBoolean()
{
    const int start = cursor_;
    const std::string value = readString();
    if (value.empty()) {
        throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedBool().createWithContext(*this);
    }
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    cursor_ = start;
    throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerInvalidBool().createWithContext(*this, value);
}

void StringReader::expect(char c)
{
    if (!canRead() || peek() != c) {
        throw CommandSyntaxException::BUILT_IN_EXCEPTIONS.readerExpectedSymbol().createWithContext(*this,
                                                                                                   std::string(1, c));
    }
    skip();
}

}  // namespace endgate::brigadier
