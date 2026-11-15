/* ★ Windows stand-in for <ifaddrs.h>: getifaddrs() built on GetAdaptersAddresses, one entry per unicast
 *   address of every adapter, with the same fields the core reads on Linux and macOS.
 * ★★ ifa_name is the adapter's FRIENDLY name in UTF-8 ("Ethernet", "Wi-Fi") — what an owner sees in
 *    Windows' own settings, so a log line naming it is a name they can find. Loopback is reported with
 *    IFF_LOOPBACK, as on Linux, so the "skip loopback" filters work unchanged.
 * ★ Netmask comes from OnLinkPrefixLength. The whole list is ONE allocation (freeifaddrs frees it). */
#pragma once
#include "vibe_win_compat.h"
#include "net/if.h"
struct ifaddrs {
    struct ifaddrs*  ifa_next;
    char*            ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_broadaddr;
    void*            ifa_data;
};
#define ifa_dstaddr ifa_broadaddr
struct vibe_ifa_slot_ {
    struct ifaddrs ifa;
    struct sockaddr_storage addr, mask;
    char name[128];
};
static inline int getifaddrs(struct ifaddrs** out) {
    *out = NULL;
    ULONG sz = 16 * 1024;
    IP_ADAPTER_ADDRESSES* aa = NULL;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES*)malloc(sz);
        if (!aa) { errno = ENOMEM; return -1; }
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz);
    }
    if (r != NO_ERROR) { free(aa); errno = EIO; return -1; }
    size_t n = 0;
    for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next)
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) n++;
    struct vibe_ifa_slot_* s = (struct vibe_ifa_slot_*)calloc(n ? n : 1, sizeof *s);
    if (!s) { free(aa); errno = ENOMEM; return -1; }
    size_t i = 0;
    for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next, i++) {
            struct vibe_ifa_slot_* e = &s[i];
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, e->name, (int)sizeof e->name, NULL, NULL);
            e->ifa.ifa_name = e->name;
            unsigned f = IFF_MULTICAST;
            if (a->OperStatus == IfOperStatusUp) f |= IFF_UP | IFF_RUNNING;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) f |= IFF_LOOPBACK; else f |= IFF_BROADCAST;
            e->ifa.ifa_flags = f;
            int len = u->Address.iSockaddrLength;
            if (len > (int)sizeof e->addr) len = (int)sizeof e->addr;
            memcpy(&e->addr, u->Address.lpSockaddr, (size_t)len);
            e->ifa.ifa_addr = (struct sockaddr*)&e->addr;
            unsigned pl = u->OnLinkPrefixLength;
            if (e->addr.ss_family == AF_INET) {
                struct sockaddr_in* m = (struct sockaddr_in*)&e->mask;
                m->sin_family = AF_INET;
                m->sin_addr.s_addr = pl == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - (pl > 32 ? 32 : pl)));
            } else if (e->addr.ss_family == AF_INET6) {
                struct sockaddr_in6* m = (struct sockaddr_in6*)&e->mask;
                m->sin6_family = AF_INET6;
                for (unsigned b = 0; b < 128 && b < pl; b++) m->sin6_addr.s6_addr[b / 8] |= (unsigned char)(0x80 >> (b % 8));
            }
            e->ifa.ifa_netmask = (struct sockaddr*)&e->mask;
            e->ifa.ifa_next = (i + 1 < n) ? &s[i + 1].ifa : NULL;
        }
    }
    free(aa);
    *out = n ? &s[0].ifa : NULL;
    if (!n) free(s);
    return 0;
}
static inline void freeifaddrs(struct ifaddrs* p) { free(p); }
