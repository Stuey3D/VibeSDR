/* ★ Windows stand-in for <netdb.h>: getaddrinfo / freeaddrinfo / gai_strerror are in ws2tcpip.h.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
