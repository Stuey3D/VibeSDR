/* ★ Windows stand-in for <poll.h>: poll() is WSAPoll — see vibe_win_compat.h.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
