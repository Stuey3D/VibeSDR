/* ★ Windows stand-in for <sys/un.h>: sockaddr_un is <afunix.h> (Windows 10 1803+).
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
