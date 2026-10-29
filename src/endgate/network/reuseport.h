#pragma once

#include <asio/detail/socket_option.hpp>
#include <sys/socket.h>

namespace endgate {

using ReusePort = asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;

}  // namespace endgate
