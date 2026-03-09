#pragma once

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
constexpr socket_t invalid_socket_v = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket_v = -1;
#endif
