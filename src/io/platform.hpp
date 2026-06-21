#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t invalid_socket_v = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket_v = -1;
#endif
