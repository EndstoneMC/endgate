#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "endgate/buffer/bytebuf.h"

namespace endgate::rtc::sctp {

inline std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

template <typename Buf>
    requires std::derived_from<Buf, AbstractByteBuf>
std::vector<std::byte> bytesOf(const Buf &buf)
{
    const auto span = buf.nioBuffer();
    return {span.begin(), span.end()};
}

}  // namespace endgate::rtc::sctp
