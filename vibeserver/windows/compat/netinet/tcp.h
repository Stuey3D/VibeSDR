/* ★ Windows stand-in for <netinet/tcp.h>: TCP_NODELAY is in winsock2.h; TCP_KEEPIDLE/KEEPINTVL/KEEPCNT in ws2ipdef.h.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
