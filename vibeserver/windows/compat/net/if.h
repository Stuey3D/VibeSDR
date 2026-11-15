/* ★ Windows stand-in for <net/if.h>: the interface flags getifaddrs() reports (see ifaddrs.h), and
 *   if_nametoindex / if_indextoname, which iphlpapi provides under the same names. */
#pragma once
#include "vibe_win_compat.h"
#include <netioapi.h>
/* ★ ws2ipdef.h defines some of these (with its own values); each is guarded on its own. */
#ifndef IFF_UP
#define IFF_UP 0x1
#endif
#ifndef IFF_BROADCAST
#define IFF_BROADCAST 0x2
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x4
#endif
#ifndef IFF_POINTOPOINT
#define IFF_POINTOPOINT 0x10
#endif
#ifndef IFF_RUNNING
#define IFF_RUNNING 0x40
#endif
#ifndef IFF_MULTICAST
#define IFF_MULTICAST 0x8
#endif
#ifndef IFNAMSIZ
#define IFNAMSIZ 256
#endif
