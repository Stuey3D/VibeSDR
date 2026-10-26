/* ★ Windows stand-in for <sys/socket.h>: Winsock2 carries the BSD socket API (socket, bind, send, recv, setsockopt, …).
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
