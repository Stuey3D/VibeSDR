/* ★ Windows stand-in for <sys/ioctl.h>: ioctl() is not offered: FIONREAD on a socket is ioctlsocket(), spelled at the call site.
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
