/* ★ Windows stand-in for <sys/uio.h>: No readv/writev; nothing in the core needs them on Windows.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
