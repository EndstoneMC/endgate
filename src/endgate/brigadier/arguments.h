#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <typeinfo>
#include <variant>
#include <vector>

#include "endgate/brigadier/reader.h"

namespace endgate::brigadier {

template <typename S>
class CommandContext;

using ArgumentValue = std::variant<bool, int, std::int64_t, float, double, std::string>;

/**
 * Turns the text at the reader's cursor into an argument's value.
 *
 * @see com.mojang.brigadier.arguments.ArgumentType
 */
class ArgumentType {
public:
    ArgumentType() = default;
    ArgumentType(const ArgumentType &) = default;
    ArgumentType &operator=(const ArgumentType &) = default;
    virtual ~ArgumentType() = default;

    [[nodiscard]] virtual ArgumentValue parse(StringReader &reader) const = 0;
    [[nodiscard]] virtual std::vector<std::string> getExamples() const { return {}; }
    [[nodiscard]] virtual std::string toString() const = 0;

    [[nodiscard]] virtual bool isInstanceOf(const std::type_info &target) const
    {
        return target == typeid(ArgumentType);
    }

    template <std::derived_from<ArgumentType> T>
    [[nodiscard]] T *as()
    {
        return is<T>() ? static_cast<T *>(this) : nullptr;
    }

    template <std::derived_from<ArgumentType> T>
    [[nodiscard]] const T *as() const
    {
        return is<T>() ? static_cast<const T *>(this) : nullptr;
    }

    template <std::derived_from<ArgumentType> T>
    [[nodiscard]] bool is() const
    {
        return isInstanceOf(typeid(T));
    }
};

/**
 * @see com.mojang.brigadier.arguments.StringArgumentType
 */
class StringArgumentType : public ArgumentType {
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    enum class StringType : std::uint8_t {
        SingleWord,
        QuotablePhrase,
        GreedyPhrase,
    };

    static std::shared_ptr<StringArgumentType> word() { return create(StringType::SingleWord); }
    static std::shared_ptr<StringArgumentType> string() { return create(StringType::QuotablePhrase); }
    static std::shared_ptr<StringArgumentType> greedyString() { return create(StringType::GreedyPhrase); }

    template <typename S>
    static std::string getString(const CommandContext<S> &context, const std::string &name)
    {
        return context.template getArgument<std::string>(name);
    }

    StringArgumentType(Passkey, StringType type) : type_(type) {}

    [[nodiscard]] StringType getType() const { return type_; }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(StringArgumentType) || ArgumentType::isInstanceOf(target);
    }

    [[nodiscard]] ArgumentValue parse(StringReader &reader) const override
    {
        if (type_ == StringType::GreedyPhrase) {
            std::string text = reader.getRemaining();
            reader.setCursor(reader.getTotalLength());
            return text;
        }
        if (type_ == StringType::SingleWord) {
            return reader.readUnquotedString();
        }
        return reader.readString();
    }

    [[nodiscard]] std::string toString() const override { return "string()"; }

    [[nodiscard]] std::vector<std::string> getExamples() const override
    {
        switch (type_) {
        case StringType::SingleWord:
            return {"word", "words_with_underscores"};
        case StringType::QuotablePhrase:
            return {"\"quoted phrase\"", "word", "\"\""};
        case StringType::GreedyPhrase:
            return {"word", "words with spaces", "\"and symbols\""};
        }
        return {};
    }

    static std::string escapeIfRequired(const std::string &input)
    {
        for (const char c : input) {
            if (!StringReader::isAllowedInUnquotedString(c)) {
                return escape(input);
            }
        }
        return input;
    }

private:
    static std::shared_ptr<StringArgumentType> create(StringType type)
    {
        return std::make_shared<StringArgumentType>(Passkey{}, type);
    }

    static std::string escape(const std::string &input)
    {
        std::string result = "\"";
        for (const char c : input) {
            if (c == '\\' || c == '"') {
                result += '\\';
            }
            result += c;
        }
        result += '"';
        return result;
    }

    StringType type_;
};

}  // namespace endgate::brigadier
