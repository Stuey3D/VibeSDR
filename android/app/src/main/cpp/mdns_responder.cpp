// mDNS hostname responder — makes the server reachable as "vibesdr.local".
//
// WHY THIS EXISTS AT ALL
//
// We already advertise a SERVICE (_vibesdr._tcp) through Android's NsdManager, and that
// is what makes the phone appear in the app's Discovered list. But a browser typing
// "vibesdr.local" is not looking for a service — it is resolving a HOSTNAME, which needs
// an A record. NsdManager cannot publish one: it registers services, not hosts. So the
// only way to answer that query is to speak mDNS ourselves.
//
// WHAT IT DOES
//
// Joins 224.0.0.251:5353, and answers QTYPE=A (and ANY) questions for our own name with
// our IPv4 address. It does not browse and it does not cache — this is not a general mDNS
// stack and does not try to be one.
//
// ★★★ AND, WHEN ASKED TO, IT PUBLISHES THE SERVICE (_vibesdr._tcp) TOO.
//
// That used to be Android's job alone: NsdManager registers the service, so this file
// only ever needed the A record, and it said so. On LINUX nothing filled that gap —
// mdnsStart() published a hostname and nothing else — so `vibeserver.local` resolved
// while the Pi NEVER appeared in the app's, the watch's or any client's Discovered list.
// Every Linux VibeServer has been undiscoverable since the daemon shipped (Stuart,
// 2026-08-12). The Mac had the same hole in Full mode, patched separately with NetService.
//
// ★★ Publishing is OPT-IN, via the port argument. Android passes none and keeps using
//    NsdManager: two registrations for one service would answer the same question twice
//    and a resolver takes whichever arrives first. One publisher per platform.
//
// NAME COLLISIONS (RFC 6762 §8)
//
// Two phones both called "vibesdr" would otherwise both answer the same query, and the
// resolver would get whichever reply arrived first — a coin toss. So on start-up we
// PROBE: ask the network whether anyone already answers for our name, three times,
// 250 ms apart. If anybody does, we take the next name (vibesdr-2, vibesdr-3, …) and
// probe again. This is the standard mechanism and it is why the second phone on the
// network names itself without being told to.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#if !defined(__ANDROID__) || (defined(__ANDROID_API__) && __ANDROID_API__ >= 24)
#include <ifaddrs.h>
#define VIBE_MDNS_FOLLOW_IP 1
#endif
// ★★★ EVERY INTERFACE, BY ITS OWN ADDRESS — LINUX VIBESERVER ONLY (Stuart's Portable road test, 2026-10-14: on the
//     box's own hotspot its name did not resolve; he had to type http://10.42.0.1). The group was joined with
//     imr_interface = INADDR_ANY and packets sent with no IP_MULTICAST_IF, and Linux resolves BOTH through the routing
//     table — "which interface does 224.0.0.251 route out of?". On a hotspot (NetworkManager's shared mode) there is no
//     default route at all: the join failed with ENODEV and every send with ENETUNREACH, so the responder heard no
//     question and announced nothing. avahi kept answering for the HOSTNAME (it joins per interface), which is why
//     VibeServerSetup.local worked on the setup hotspot — but the name chosen in setup, and the _vibesdr._tcp service
//     the apps browse for, are ours, and were silent there. The same hole on a box with Ethernet AND its hotspot: the
//     route picks one interface and the other network never hears the name.
//     So, here: join on every multicast interface by its address, send out of each interface explicitly, and answer a
//     question with the address of the interface the asker shares (their subnet). Android keeps its proven
//     INADDR_ANY path (NsdManager does the service there; its Wi-Fi always has a route); iOS never serves.
#if defined(__linux__) && !defined(__ANDROID__) && defined(VIBE_MDNS_FOLLOW_IP)
#include <net/if.h>
#define VIBE_MDNS_PER_IFACE 1
#endif

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// Portable logging. iOS links this same shim (a prebuilt libvibelocalsdr_ios.a), and
// android/log.h does not exist there — including it unconditionally simply fails to
// build. iOS never SERVES (no USB host), but the shim references these symbols, so the
// file must still compile and link there.
#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VibeMdns", __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) do { fprintf(stderr, "VibeMdns: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

namespace vibe {
namespace {

constexpr const char* kGroup = "224.0.0.251";
constexpr uint16_t    kPort  = 5353;
constexpr int         kTtl   = 120;      // seconds; RFC 6762 recommends 120 for A records

std::thread            g_thread;
std::atomic<bool>      g_run{false};
std::string            g_host;           // "vibesdr" — WITHOUT the .local
uint32_t               g_addr = 0;       // our IPv4, network byte order
std::atomic<bool>      g_conflict{false};
// ★ A LEGACY UNICAST question (RFC 6762 §6.7 — asked from a port other than 5353: dig, nslookup, a plain resolver)
//   is answered straight back to the asker's port, with its transaction id, its question repeated, a TTL of at most
//   10 s and no cache-flush bit. Set only while one such question is being answered (the responder is one thread).
bool                   g_legacy = false;
uint32_t ttlOut()   { return g_legacy ? 10u : (uint32_t)kTtl; }
uint16_t classOut() { return g_legacy ? 0x0001 : 0x8001; }      // CLASS IN (+ cache-flush on multicast)
// ── The service, when we are the one publishing it (0 = we are not) ──────────────────
uint16_t               g_svcPort = 0;
bool                   g_svcPin  = false;

// ★★ THE CONTRACT IS FIXED BY THE CLIENTS THAT ALREADY EXIST — VibeMdnsModule.kt,
//    src/services/mdns.ts and the Mac's NetService all agree on these exact strings, and a
//    discovery that does not match them finds nothing while looking perfectly healthy.
constexpr const char* kSvcType = "_vibesdr._tcp";

/** Encode "vibesdr.local" as DNS labels: \7vibesdr\5local\0 */
std::string encodeName(const std::string& host) {
    std::string out;
    out += (char)host.size();
    out += host;
    out += (char)5;
    out += "local";
    out += '\0';
    return out;
}

/** Encode a dotted name ("vibesdr._vibesdr._tcp.local") as DNS labels. */
std::string encodeDotted(const std::string& dotted) {
    std::string out;
    size_t i = 0;
    while (i < dotted.size()) {
        size_t dot = dotted.find('.', i);
        if (dot == std::string::npos) dot = dotted.size();
        const size_t len = dot - i;
        if (len == 0 || len > 63) return std::string();     // malformed — publish nothing
        out += (char)len;
        out += dotted.substr(i, len);
        i = dot + 1;
    }
    out += '\0';
    return out;
}

std::string svcTypeName()               { return encodeDotted(std::string(kSvcType) + ".local"); }
std::string svcInstName(const std::string& host) {
    return encodeDotted(host + "." + kSvcType + ".local");
}
/** The meta-query browsers use to ask "what KINDS of service are here?" */
std::string svcMetaName()               { return encodeDotted("_services._dns-sd._udp.local"); }

/** Where the name at `pos` ends in the packet (one past its last byte), or 0 if it is malformed.
 *  A compression pointer ENDS the name in place (2 bytes) — where it points does not matter here. */
size_t skipName(const uint8_t* buf, size_t len, size_t pos) {
    size_t i = pos;
    while (i < len) {
        const uint8_t l = buf[i];
        if (l == 0) return i + 1;
        if ((l & 0xC0) == 0xC0) return (i + 2 <= len) ? i + 2 : 0;
        if ((l & 0xC0) != 0) return 0;                            // 0x40/0x80: reserved label types
        i += 1 + l;
    }
    return 0;
}

/** Case-insensitive compare of a DNS-encoded name in `buf` at `pos` against ours (`want`, uncompressed).
 *  Returns the length the name occupies AT `pos` (to skip it), or 0 if it doesn't match.
 *
 *  ★★★ COMPRESSION POINTERS ARE FOLLOWED (2026-10-17, Stuart: vibeserver-portable.local "not reachable for most of
 *      today" while vibepocket.local — avahi, same box — always was). This used to say "a QUESTION never uses them"
 *      and returned no-match on the first pointer. Wrong: Apple's mDNSResponder (every Mac, iPhone, iPad) packs the
 *      questions of ALL its pending lookups into one packet and compresses the later names against the earlier ones —
 *      "vibeserver-portable" + pointer to the "local" of the question before it, or AAAA then A by pointer to the
 *      whole name. Our question was only ever answered when it happened to be FIRST in the packet and uncompressed,
 *      so the name resolved or not depending on what else the asker was looking up — a busy Apple LAN (TVs, amps,
 *      phones) made it mostly "not". Measured from the Mac against the Pi 500 and the Portable: a lone A question was
 *      answered; "AAAA + A(ptr)" got only the NSEC; "other.local + ours(ptr .local)" got nothing. avahi answered all
 *      three. Every published name and the _vibesdr._tcp browse went through this parser, so discovery had the same
 *      hole. */
size_t matchName(const uint8_t* buf, size_t len, size_t pos, const std::string& want) {
    const size_t end = skipName(buf, len, pos);
    if (!end) return 0;
    size_t i = pos, w = 0;
    for (int hops = 0; i < len;) {
        uint8_t l = buf[i];
        if (l == 0) {
            return (w == want.size() - 1) ? (end - pos) : 0;      // want ends with the \0
        }
        if ((l & 0xC0) == 0xC0) {
            if (i + 1 >= len || ++hops > 16) return 0;            // a pointer loop is a malformed packet
            const size_t to = ((size_t)(l & 0x3F) << 8) | buf[i + 1];
            if (to >= i) return 0;                                // pointers only ever point BACK
            i = to;
            continue;
        }
        if ((l & 0xC0) != 0) return 0;
        if (i + 1 + l > len) return 0;
        if (w >= want.size() || (uint8_t)want[w] != l) return 0;
        for (uint8_t k = 0; k < l; ++k) {
            char a = (char)buf[i + 1 + k];
            char b = want[w + 1 + k];
            if (a >= 'A' && a <= 'Z') a += 32;                    // mDNS names are case-insensitive
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
        w += 1 + l;
        i += 1 + l;
    }
    return 0;
}

/** Build an mDNS response carrying one A record for our name. */
std::string buildAnswer(const std::string& host, uint32_t addr, uint16_t txid) {
    const std::string name = encodeName(host);
    std::string p;
    auto u16 = [&](uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); };
    auto u32 = [&](uint32_t v) {
        p += (char)(v >> 24); p += (char)((v >> 16) & 0xFF);
        p += (char)((v >> 8) & 0xFF); p += (char)(v & 0xFF);
    };
    u16(txid);
    u16(0x8400);        // response, authoritative
    u16(0);             // questions
    u16(1);             // answers
    u16(0);             // authority
    u16(0);             // additional
    p += name;
    u16(1);             // TYPE A
    u16(classOut());        // CLASS IN, cache-flush bit — we are authoritative for this name
    u32(ttlOut());
    u16(4);
    p.append((const char*)&addr, 4);     // already network byte order
    return p;
}

/**
 * The service records, as one response: PTR (what is here) + SRV (where) + TXT (how) + A.
 *
 * ★★★ ALL FOUR IN ONE PACKET, because that is what makes discovery INSTANT rather than a
 *     three-round-trip conversation. A browser that gets a bare PTR must then ask for SRV,
 *     then TXT, then A — and on a phone waking its Wi-Fi that is the difference between a
 *     server appearing at once and appearing "eventually", which reads as not appearing.
 * ★★ NO NAME COMPRESSION. Pointers would save a hundred bytes and are a common source of
 *    subtly wrong packets; we are nowhere near the MTU, so the whole names are written out.
 * ★ The cache-flush bit is set on SRV/TXT/A (we are authoritative for those) but NOT on the
 *   PTR: several servers legitimately share one service type, and flushing there would tell
 *   resolvers to forget everyone else's.
 */
std::string buildService(const std::string& host, uint32_t addr, uint16_t txid, bool includePtr) {
    const std::string inst = svcInstName(host);
    const std::string type = svcTypeName();
    const std::string hostN = encodeName(host);
    if (inst.empty() || type.empty()) return std::string();

    std::string p;
    auto u16 = [&](uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); };
    auto u32 = [&](uint32_t v) {
        p += (char)(v >> 24); p += (char)((v >> 16) & 0xFF);
        p += (char)((v >> 8) & 0xFF); p += (char)(v & 0xFF);
    };

    const uint16_t nAns = includePtr ? 4 : 3;
    u16(txid);
    u16(0x8400);            // response, authoritative
    u16(0);                 // questions
    u16(nAns);              // answers
    u16(0);                 // authority
    u16(0);                 // additional

    if (includePtr) {
        p += type;
        u16(12);            // TYPE PTR
        u16(0x0001);        // CLASS IN — no cache-flush, see above
        u32(ttlOut());
        u16((uint16_t)inst.size());
        p += inst;
    }

    // SRV: priority, weight, port, target
    p += inst;
    u16(33);                // TYPE SRV
    u16(classOut());            // CLASS IN + cache-flush
    u32(ttlOut());
    u16((uint16_t)(6 + hostN.size()));
    u16(0); u16(0); u16(g_svcPort);
    p += hostN;

    // TXT: the same keys every existing client already reads.
    {
        const std::string k1 = "proto=vibeserver";
        const std::string k2 = g_svcPin ? "pin=1" : "pin=0";
        std::string rd;
        rd += (char)k1.size(); rd += k1;
        rd += (char)k2.size(); rd += k2;
        p += inst;
        u16(16);            // TYPE TXT
        u16(classOut());
        u32(ttlOut());
        u16((uint16_t)rd.size());
        p += rd;
    }

    // A, so the resolver never has to ask where the target lives.
    p += hostN;
    u16(1);
    u16(classOut());
    u32(ttlOut());
    u16(4);
    p.append((const char*)&addr, 4);
    return p;
}

/** Answer the "what service types are here?" meta-query with our type. */
std::string buildMetaPtr(uint16_t txid) {
    const std::string meta = svcMetaName(), type = svcTypeName();
    if (meta.empty() || type.empty()) return std::string();
    std::string p;
    auto u16 = [&](uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); };
    auto u32 = [&](uint32_t v) {
        p += (char)(v >> 24); p += (char)((v >> 16) & 0xFF);
        p += (char)((v >> 8) & 0xFF); p += (char)(v & 0xFF);
    };
    u16(txid); u16(0x8400); u16(0); u16(1); u16(0); u16(0);
    p += meta;
    u16(12); u16(0x0001); u32(ttlOut());
    u16((uint16_t)type.size());
    p += type;
    return p;
}

/**
 * A NEGATIVE answer: "this name exists, and the only record type it has is A".
 *
 * Without this, an AAAA (IPv6) query for our name goes UNANSWERED — and a resolver asks
 * for A and AAAA together, then WAITS for the missing reply to time out before using the
 * IPv4 address it already had. That wait is why connecting by .local felt markedly
 * slower than connecting by IP. Silence is not "no"; NSEC is how mDNS says "no".
 *
 * RDATA = <next domain name> <window 0> <bitmap length 1> <bitmap>, and the bitmap's
 * high bit of byte 0 is type 1 (A) — so 0x40.
 */
std::string buildNsec(const std::string& host, uint16_t txid) {
    const std::string name = encodeName(host);
    std::string p;
    auto u16 = [&](uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); };
    auto u32 = [&](uint32_t v) {
        p += (char)(v >> 24); p += (char)((v >> 16) & 0xFF);
        p += (char)((v >> 8) & 0xFF); p += (char)(v & 0xFF);
    };
    u16(txid);
    u16(0x8400);            // response, authoritative
    u16(0); u16(1); u16(0); u16(0);
    p += name;
    u16(47);                // TYPE NSEC
    u16(classOut());            // CLASS IN + cache-flush
    u32(ttlOut());
    std::string rdata = name;      // "next name" is our own, as mDNS does
    rdata += (char)0x00;           // window 0
    rdata += (char)0x01;           // bitmap length
    rdata += (char)0x40;           // bit for type 1 (A) only — no AAAA
    u16((uint16_t)rdata.size());
    p += rdata;
    return p;
}

/** A response rebuilt for a legacy unicast asker: its transaction id and its question (`question` = name + QTYPE +
 *  QCLASS, uncompressed) in front of the answers. The builders already wrote TTL ≤ 10 and no cache-flush (g_legacy). */
std::string legacyReply(const std::string& ans, uint16_t txid, const std::string& question) {
    if (ans.size() < 12 || question.empty()) return ans;
    std::string p = ans;
    p[0] = (char)(txid >> 8); p[1] = (char)(txid & 0xFF);
    p[4] = 0; p[5] = 1;                                       // QDCOUNT = 1
    p.insert(12, question);
    return p;
}

/** Ask whether anyone else already answers for `host`. */
std::string buildProbe(const std::string& host) {
    const std::string name = encodeName(host);
    std::string p;
    auto u16 = [&](uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); };
    u16(0);
    u16(0);             // standard query
    u16(1);             // one question
    u16(0); u16(0); u16(0);
    p += name;
    u16(255);           // QTYPE ANY — "does ANYTHING exist under this name?"
    u16(1);             // CLASS IN
    return p;
}

int openSocket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    // Essential: the system's own mDNS daemon is already bound to 5353. Without this we
    // simply fail to bind and never see a single query.
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(kPort);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) {
        LOGI("mDNS bind failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kGroup);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
        LOGI("mDNS join failed: %s (is the MulticastLock held?)", strerror(errno));
    }
    unsigned char ttl = 255;                      // RFC 6762: mDNS uses TTL 255
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    unsigned char loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    return fd;
}

void sendTo(int fd, const std::string& pkt, const sockaddr_in& dst) {
    sendto(fd, pkt.data(), pkt.size(), 0, (const sockaddr*)&dst, sizeof dst);
}

#ifdef VIBE_MDNS_PER_IFACE
/** One IPv4 interface we answer on: its address and netmask (network byte order). */
struct Ifc { uint32_t addr; uint32_t mask; };

/** Every UP, MULTICAST, non-loopback IPv4 interface address — a Wi-Fi client, the box's own hotspot, Ethernet. */
std::vector<Ifc> listIfcs() {
    std::vector<Ifc> out;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || !(p->ifa_flags & IFF_MULTICAST) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        const uint32_t a = reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr;
        if (!a || (ntohl(a) >> 24) == 127) continue;
        const uint32_t m = p->ifa_netmask ? reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr.s_addr
                                          : 0xFFFFFFFFu;
        bool dup = false;
        for (const auto& i : out) dup = dup || i.addr == a;
        if (!dup) out.push_back({a, m});
    }
    freeifaddrs(ifa);
    return out;
}

bool sameIfcs(const std::vector<Ifc>& a, const std::vector<Ifc>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
        bool hit = false;
        for (const auto& y : b) hit = hit || (x.addr == y.addr && x.mask == y.mask);
        if (!hit) return false;
    }
    return true;
}

/** Join the group on each interface, BY ITS ADDRESS — no routing lookup, so a hotspot with no default route joins too.
 *  True if any join SUCCEEDED, i.e. a membership was missing (EADDRINUSE = still held): the caller re-announces. */
bool joinEach(int fd, const std::vector<Ifc>& ifs) {
    bool any = false;
    for (const auto& i : ifs) {
        ip_mreq m{};
        m.imr_multiaddr.s_addr = inet_addr(kGroup);
        m.imr_interface.s_addr = i.addr;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0) any = true;
    }
    return any;
}

/** Send out of the interface that owns `via` (IP_MULTICAST_IF) — a link-local group needs no route that way. */
void sendVia(int fd, const std::string& pkt, const sockaddr_in& dst, uint32_t via) {
    in_addr a{};
    a.s_addr = via;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
    sendTo(fd, pkt, dst);
}

/** Our address on the asker's network (the interface whose subnet holds it), else `fallback`. */
uint32_t addrFor(const std::vector<Ifc>& ifs, uint32_t peer, uint32_t fallback) {
    for (const auto& i : ifs)
        if ((i.addr & i.mask) == (peer & i.mask)) return i.addr;
    return fallback;
}
#endif

#ifdef VIBE_MDNS_FOLLOW_IP
/** The machine's address NOW, by the same preference as the server's primaryIpv4() (a private LAN
 *  address over anything else). 0 when there is none — between networks. */
uint32_t currentIpv4() {
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return 0;
    uint32_t out = 0;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        const uint32_t a = reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr;
        const uint32_t h = ntohl(a);
        if ((h >> 24) == 127) continue;
        const bool lan = (h >> 16) == 0xC0A8 || (h >> 24) == 10;
        if (!out || lan) out = a;
    }
    freeifaddrs(ifa);
    return out;
}
#endif

void loop(std::string base, uint32_t addr) {
#ifdef VIBE_MDNS_FOLLOW_IP
    // ★★★ NO ADDRESS YET IS NOT "NEVER" (Stuart's pocket box, 2026-10-10: its name resolved by hand but the apps never
    //     listed it). VibeServer starts at boot before the Wi-Fi has joined anything, so there was no address, the
    //     responder was never started, and nothing ever started it later. Now it waits here — one look every 2 s —
    //     and announces the moment the machine has an address; the follow below keeps it right after that.
    while (addr == 0 && g_run.load()) {
        addr = currentIpv4();
        if (!addr) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!g_run.load()) return;
    g_addr = addr;
    LOGI("mDNS: announcing %s.local at %s", base.c_str(), inet_ntoa(*(in_addr*)&addr));
#endif
    int fd = openSocket();
    if (fd < 0) return;

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_addr.s_addr = inet_addr(kGroup);
    group.sin_port = htons(kPort);

    // ★ Multicast to the group: out of EVERY interface, each packet carrying that interface's own address
    //   (build(addr) makes it). Without per-interface support, the one address as before.
#ifdef VIBE_MDNS_PER_IFACE
    std::vector<Ifc> ifs = listIfcs();
    joinEach(fd, ifs);
#endif
    auto toGroup = [&](auto build) {
#ifdef VIBE_MDNS_PER_IFACE
        if (!ifs.empty()) {
            for (const auto& i : ifs) {
                const std::string pkt = build(i.addr);
                if (!pkt.empty()) sendVia(fd, pkt, group, i.addr);
            }
            return;
        }
#endif
        const std::string pkt = build(addr);
        if (!pkt.empty()) sendTo(fd, pkt, group);
    };
    // ★ Is this source address one of OURS (our own multicast looped back)? Every interface's, not just the first.
    auto isOurs = [&](uint32_t a) {
#ifdef VIBE_MDNS_PER_IFACE
        for (const auto& i : ifs) if (i.addr == a) return true;
#endif
        return a == addr;
    };

    // ── Probe, then rename on conflict (RFC 6762 §8) ─────────────────────────
    // This is what makes a SECOND phone on the network call itself vibesdr-2 without
    // anybody configuring it.
    std::string host = base;
    for (int attempt = 1; attempt <= 9 && g_run.load(); ++attempt) {
        g_conflict.store(false);
        const std::string probe = buildProbe(host);
        std::string want = encodeName(host);

        for (int i = 0; i < 3 && g_run.load(); ++i) {
            toGroup([&](uint32_t) { return probe; });
            // Listen 250ms for somebody answering for this name.
            for (int t = 0; t < 25 && g_run.load(); ++t) {
                timeval tv{0, 10000};
                fd_set rd;
                FD_ZERO(&rd);
                FD_SET(fd, &rd);
                if (select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                uint8_t buf[1500];
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                if (n < 12) continue;
                // A RESPONSE (QR bit) whose answer name is ours = somebody else owns it.
                if ((buf[2] & 0x80) && !isOurs(from.sin_addr.s_addr)) {
                    size_t off = 12;
                    uint16_t qd = (buf[4] << 8) | buf[5];
                    for (uint16_t q = 0; q < qd && off && off < (size_t)n; ++q) {
                        off = skipName(buf, n, off);
                        if (off) off += 4;
                    }
                    if (off && off < (size_t)n && matchName(buf, n, off, want)) {
                        g_conflict.store(true);
                    }
                }
            }
            if (g_conflict.load()) break;
        }
        if (!g_conflict.load()) break;                 // the name is ours
        host = base + "-" + std::to_string(attempt + 1);
        LOGI("mDNS name taken, trying %s.local", host.c_str());
    }
    if (!g_run.load()) { close(fd); return; }

    g_host = host;
    LOGI("mDNS responder up: %s.local -> %s", host.c_str(),
         inet_ntoa(*(in_addr*)&addr));

    // Announce ourselves twice, a second apart, so resolvers learn the name without
    // having to ask (RFC 6762 §8.3).
    const std::string want = encodeName(host);
    const std::string wantInst = svcInstName(host);
    const std::string wantType = svcTypeName();
    const std::string wantMeta = svcMetaName();
    // ★ Our name (and the service, when we publish it) to the group, on every interface.
    auto announce = [&]() {
        toGroup([&](uint32_t a) { return buildAnswer(host, a, 0); });
        if (g_svcPort) toGroup([&](uint32_t a) { return buildService(host, a, 0, true); });
    };
    for (int i = 0; i < 2 && g_run.load(); ++i) {
        // ★ Announce the SERVICE unasked too. A client that is already listening learns about
        //   this server the moment it starts, instead of only when it next happens to browse —
        //   which is what "the server took a while to appear" actually is.
        announce();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (g_svcPort)
        LOGI("mDNS service published: %s.%s.local on port %u", host.c_str(), kSvcType,
             (unsigned)g_svcPort);

    // ── Serve ────────────────────────────────────────────────────────────────
    // ★★★ RE-JOIN THE GROUP, FOR EVER. A multicast membership does NOT survive the interface it
    //     was made on going away, and on a phone that happens without anything else breaking:
    //     leave the screen off, Wi-Fi power-saves and re-associates, and the membership is
    //     silently gone. The socket stays open, the server keeps serving on its IP, TCP is
    //     untouched — and `vibesdr-moto-g35.local` simply stops resolving, for ever, because
    //     nothing ever re-joins (Stuart, 2026-08-20: it "isn't reactivating", with the screen off).
    //
    //  ★★ IP_ADD_MEMBERSHIP IS THE TEST AS WELL AS THE CURE. Re-adding a membership we still hold
    //     fails with EADDRINUSE, which we ignore; if it SUCCEEDS we had lost it, and that is
    //     exactly when the network needs to hear our name again — so a successful re-join
    //     re-announces. No timer guessing, no polling of interface state, no false announcements.
    //
    //  ★ 20 s: fast enough that a listener who reaches for the name after a power-save blip finds
    //    it, slow enough to be invisible — this is two setsockopt calls a minute.
    time_t lastJoin = time(nullptr);
    time_t lastIp = lastJoin;
    while (g_run.load()) {
#ifdef VIBE_MDNS_FOLLOW_IP
        // ★★★ FOLLOW THE ADDRESS (2026-10-10, the pocket box). The address was taken once, at start —
        //     right for a Pi that never moves, wrong for a box that goes from its own hotspot (10.42.0.1)
        //     to the owner's Wi-Fi to their phone's hotspot without restarting: the name kept answering
        //     with an address on a network the box had left. Looked at every 5 s (one getifaddrs);
        //     on a change the new address is announced at once, as a fresh start would.
        {
            const time_t nowT = time(nullptr);
            if (nowT - lastIp >= 5 || nowT < lastIp) {
                lastIp = nowT;
                const uint32_t now = currentIpv4();
#ifdef VIBE_MDNS_PER_IFACE
                // ★ The INTERFACES are what changed, as often as the address: the box's hotspot coming up beside
                //   Ethernet adds one without changing the "primary" address at all.
                std::vector<Ifc> nowIfs = listIfcs();
                const bool ifsChanged = !sameIfcs(nowIfs, ifs);
                if (ifsChanged) ifs = std::move(nowIfs);
#else
                const bool ifsChanged = false;
#endif
                if ((now && now != addr) || ifsChanged) {
                    if (now) { addr = now; g_addr = now; }
                    LOGI("mDNS: the address changed to %s — announcing it", inet_ntoa(*(in_addr*)&addr));
#ifdef VIBE_MDNS_PER_IFACE
                    joinEach(fd, ifs);
#else
                    ip_mreq re{};
                    re.imr_multiaddr.s_addr = inet_addr(kGroup);
                    re.imr_interface.s_addr = htonl(INADDR_ANY);
                    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &re, sizeof re);   // EADDRINUSE = still held
#endif
                    announce();
                }
            }
        }
#endif
        {
            const time_t nowT = time(nullptr);
            if (nowT - lastJoin >= 20 || nowT < lastJoin) {
                lastJoin = nowT;
#ifdef VIBE_MDNS_PER_IFACE
                const bool rejoined = joinEach(fd, ifs);
#else
                ip_mreq re{};
                re.imr_multiaddr.s_addr = inet_addr(kGroup);
                re.imr_interface.s_addr = htonl(INADDR_ANY);
                const bool rejoined = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &re, sizeof re) == 0;
#endif
                if (rejoined) {
                    LOGI("mDNS multicast membership had been lost — re-joined and re-announcing");
                    announce();
                }
            }
        }
        timeval tv{1, 0};
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        if (select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;

        uint8_t buf[1500];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                   // a response, not a question
        // ★ Answer with our address on the ASKER's network, out of that interface (a phone on the hotspot must be
        //   told 10.42.0.1, not the Ethernet address it cannot reach).
        bool unicastReply = false;
        // ★ Legacy unicast: the asker's own port, NOT 5353 — this used to rewrite the port to 5353, so `dig -p 5353
        //   @224.0.0.251` and every plain resolver were answered on a port nobody was listening on.
        const bool legacy = ntohs(from.sin_port) != kPort;
        const uint16_t txid = (uint16_t)((buf[0] << 8) | buf[1]);
        std::string qEcho;                             // the question being answered, uncompressed (legacy only)
        g_legacy = legacy;
#ifdef VIBE_MDNS_PER_IFACE
        const uint32_t here = addrFor(ifs, from.sin_addr.s_addr, addr);
        auto reply = [&](const std::string& ans) {
            if (ans.empty()) return;
            if (legacy)            sendTo(fd, legacyReply(ans, txid, qEcho), from);
            else if (unicastReply) { from.sin_port = htons(kPort); sendTo(fd, ans, from); }
            else                   sendVia(fd, ans, group, here);
        };
#else
        const uint32_t here = addr;
        auto reply = [&](const std::string& ans) {
            if (ans.empty()) return;
            if (legacy)            sendTo(fd, legacyReply(ans, txid, qEcho), from);
            else if (unicastReply) { from.sin_port = htons(kPort); sendTo(fd, ans, from); }
            else                   sendTo(fd, ans, group);
        };
#endif

        uint16_t qd = (buf[4] << 8) | buf[5];
        size_t off = 12;
        for (uint16_t q = 0; q < qd && off + 4 < (size_t)n; ++q) {
            size_t used = matchName(buf, n, off, want);
            // ★ Which of OUR names is being asked about? Checked before the question is
            //   consumed, since matchName reads from the same offset.
            const bool isInst = g_svcPort && !wantInst.empty() && matchName(buf, n, off, wantInst);
            const bool isType = g_svcPort && !wantType.empty() && matchName(buf, n, off, wantType);
            const bool isMeta = g_svcPort && !wantMeta.empty() && matchName(buf, n, off, wantMeta);
            // Skip this question's name however it turned out (a pointer ends it in two bytes).
            size_t skip = skipName(buf, n, off);
            if (!skip || skip + 4 > (size_t)n) break;
            uint16_t qtype = (buf[skip] << 8) | buf[skip + 1];
            const bool unicast = (buf[skip + 2] & 0x80) != 0;   // QU bit
            unicastReply = unicast;
            off = skip + 4;
            if (legacy) {
                qEcho = isMeta ? wantMeta : isType ? wantType : isInst ? wantInst : want;
                qEcho.append((const char*)buf + skip, 2);           // QTYPE as asked
                qEcho += (char)0x00; qEcho += (char)0x01;            // QCLASS IN (QU bit cleared)
            }

            // ── The service questions ────────────────────────────────────────────────────
            if (isType || isInst || isMeta) {
                std::string ans;
                if (isMeta) {
                    if (qtype == 12 || qtype == 255) ans = buildMetaPtr(0);
                } else if (isType) {
                    // A browse. PTR + everything needed to use it, in one packet.
                    if (qtype == 12 || qtype == 255) ans = buildService(host, here, 0, true);
                } else {
                    // The instance itself — SRV/TXT/ANY. No PTR: it was not asked for, and
                    // this name is not where a PTR lives.
                    if (qtype == 33 || qtype == 16 || qtype == 255)
                        ans = buildService(host, here, 0, false);
                }
                reply(ans);
                continue;
            }

            if (!used) continue;

            std::string ans;
            if (qtype == 1 || qtype == 255) {
                ans = buildAnswer(host, here, 0);          // A — our address on the asker's network
            } else if (qtype == 28) {
                // AAAA — we have none. ANSWER anyway, with NSEC: leaving it unanswered
                // makes the resolver wait out a timeout before using the A record it
                // already holds, which is exactly what made .local feel slow.
                ans = buildNsec(host, 0);
            } else {
                continue;
            }
            // Honour the QU bit: a resolver that asked for a unicast reply gets one.
            reply(ans);
        }
        g_legacy = false;                              // announcements stay multicast-shaped
    }
    close(fd);
}

}  // namespace

/**
 * Start answering for "<host>.local" with `ipv4` (dotted string). Idempotent.
 *
 * @param servicePort  publish `_vibesdr._tcp` on this port as well. 0 = HOSTNAME ONLY, which is
 *                     what Android wants: NsdManager already registers the service there, and two
 *                     publishers answering one question is worse than one.
 * @param pinRequired  goes in the TXT record as pin=1, so a client can show the lock before it
 *                     connects rather than discovering it by being refused.
 */
void mdnsStart(const std::string& host, const std::string& ipv4,
               uint16_t servicePort, bool pinRequired) {
    if (g_run.load()) return;
    if (host.empty()) return;
#ifdef VIBE_MDNS_FOLLOW_IP
    // ★ No address yet (booting before the network): start anyway — loop() waits for one.
    uint32_t addr = ipv4.empty() ? 0 : inet_addr(ipv4.c_str());
#else
    if (ipv4.empty()) return;
    uint32_t addr = inet_addr(ipv4.c_str());
#endif
    if (addr == INADDR_NONE) return;
    g_addr = addr;
    g_svcPort = servicePort;
    g_svcPin = pinRequired;
    g_run.store(true);
    g_thread = std::thread(loop, host, addr);
}

void mdnsStop() {
    if (!g_run.load()) return;
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
    g_host.clear();
    g_svcPort = 0;
    g_svcPin = false;
}

/** The name we actually took — may differ from the one asked for, if it was taken. */
std::string mdnsHost() { return g_host; }

}  // namespace vibe
