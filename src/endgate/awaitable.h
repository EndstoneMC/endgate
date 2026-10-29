#pragma once

#include <asio/awaitable.hpp>

namespace endgate {

template <typename T>
using Awaitable = asio::awaitable<T>;

}  // namespace endgate
