/* ★ Windows stand-in for <netinet/ip.h>: IP_TOS is in ws2ipdef.h (Windows ignores it unless policy allows; see vibe_wifi_qos.h).
 *   Everything it needs is in vibe_win_compat.h, which every Windows build force-includes. */
#pragma once
#include "vibe_win_compat.h"
