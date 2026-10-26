/* ★ Windows stand-in for <arpa/inet.h>: inet_pton / inet_ntop / htons are in ws2tcpip.h.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
