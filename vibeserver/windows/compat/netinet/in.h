/* ★ Windows stand-in for <netinet/in.h>: sockaddr_in / sockaddr_in6 / IPPROTO_* live in winsock2.h + ws2tcpip.h.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
