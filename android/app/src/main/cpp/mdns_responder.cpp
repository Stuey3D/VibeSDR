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
// Joins 224.0.0.251:5353, and answers questions for our own name with our IPv4 address (A, and
// an NSEC for the AAAA we do not have). It does not browse and it does not cache — this is not a
// general mDNS stack and does not try to be one. What it DOES do is answer the packets real
// resolvers send, the way RFC 6762 says: see respond() for the list, and vibeserver/test-mdns.cpp
// for every shape, byte for byte. scripts/probe-mdns.py asks a live server the same questions.
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
#include <stdlib.h>
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
#include <cstring>
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

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// RECORDS AND RESPONSES
//
// ★★★ ONE QUERY PACKET, ONE RESPONSE, BUILT BY respond() (2026-10-20, Stuart: the Portable "usually" missing from the
//     app's Discovered list, vibeserver-portable.local failing in every browser). This used to be a builder per record
//     shape and a reply per QUESTION, each written into the Answer section. What real resolvers send, and what we now
//     do with it (RFC 6762 / 6763):
//       · several questions in one packet, compressed against each other (Apple packs every pending lookup) — all are
//         matched, and everything we own goes out in ONE response;
//       · A + AAAA asked together — the A answered, and an NSEC saying "this name has no AAAA" (§6.1), so the asker
//         stops waiting for an IPv6 address that will never come. An A answer always carries that NSEC as an
//         additional record, so an asker that only asked for A learns it too;
//       · a browse (PTR) — the PTR in the Answer section and SRV, TXT, A and NSEC as ADDITIONAL records (RFC 6763 §12.1),
//         which is where Android's MdnsDiscoveryManager and mDNSResponder look for them;
//       · KNOWN ANSWERS (§7.1) — a record the asker already holds with at least half its TTL left is NOT sent again: a
//         MUST, and it is what keeps a busy LAN quiet; a packet with the TC bit has more known answers coming, so its
//         reply waits 400–500 ms for them (§7.2 — done in the serve loop);
//       · QU (§5.4) — a query whose every question asks for a unicast reply gets one, to the asker's 5353;
//       · LEGACY UNICAST (§6.7) — a query from any port but 5353 gets its id and questions back, TTL ≤ 10, no cache-flush,
//         to the port it came from.
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════

/** One resource record we own. TTL and class are applied when it is written (normal, legacy or goodbye). */
struct Rec {
    std::string name;      // DNS-encoded, uncompressed
    uint16_t    type;
    bool        unique;    // cache-flush bit on multicast: we alone answer for it (not the shared PTRs)
    std::string rdata;     // uncompressed
};

bool sameRec(const Rec& a, const Rec& b) { return a.type == b.type && a.name == b.name && a.rdata == b.rdata; }

void put16(std::string& p, uint16_t v) { p += (char)(v >> 8); p += (char)(v & 0xFF); }
void put32(std::string& p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p, (uint16_t)(v & 0xFFFF)); }

Rec recA(const std::string& host, uint32_t addr) {
    return {encodeName(host), 1, true, std::string((const char*)&addr, 4)};   // addr is network byte order already
}

/** "This name exists and A is the only type it has" — the negative answer to AAAA (RFC 6762 §6.1). RDATA = next name
 *  (our own, as mDNS does) + window 0 + bitmap length 1 + bitmap; bit 1 (A) is 0x40. */
Rec recNsec(const std::string& host) {
    const std::string name = encodeName(host);
    std::string rd = name;
    rd += (char)0x00; rd += (char)0x01; rd += (char)0x40;
    return {name, 47, true, rd};
}

Rec recPtr(const std::string& host) { return {svcTypeName(), 12, false, svcInstName(host)}; }

Rec recSrv(const std::string& host, uint16_t port) {
    std::string rd;
    put16(rd, 0); put16(rd, 0); put16(rd, port);           // priority, weight, port
    rd += encodeName(host);
    return {svcInstName(host), 33, true, rd};
}

/** The same TXT keys every existing client already reads. */
Rec recTxt(const std::string& host, bool pin) {
    const std::string k1 = "proto=vibeserver";
    const std::string k2 = pin ? "pin=1" : "pin=0";
    std::string rd;
    rd += (char)k1.size(); rd += k1;
    rd += (char)k2.size(); rd += k2;
    return {svcInstName(host), 16, true, rd};
}

/** The "what KINDS of service are here?" meta-query's answer. */
Rec recMeta() { return {svcMetaName(), 12, false, svcTypeName()}; }

enum class Ttl { Normal, Legacy, Goodbye };

/**
 * Write one DNS message. `qsec`/`qd` = a question section to repeat (legacy unicast only — multicast responses carry
 * none, RFC 6762 §6). No name compression: we are far below any MTU, and pointers are where packets go subtly wrong.
 */
std::string writeMsg(uint16_t txid, const std::string& qsec, uint16_t qd,
                     const std::vector<Rec>& an, const std::vector<Rec>& ar, Ttl ttl) {
    std::string p;
    put16(p, txid);
    put16(p, 0x8400);                  // response, authoritative
    put16(p, qd);
    put16(p, (uint16_t)an.size());
    put16(p, 0);
    put16(p, (uint16_t)ar.size());
    p += qsec;
    auto rr = [&](const Rec& r) {
        p += r.name;
        put16(p, r.type);
        // ★ cache-flush only on a multicast answer we alone own; a legacy asker is a plain DNS client (§6.7)
        put16(p, (r.unique && ttl != Ttl::Legacy) ? 0x8001 : 0x0001);
        put32(p, ttl == Ttl::Goodbye ? 0u : ttl == Ttl::Legacy ? 10u : (uint32_t)kTtl);
        put16(p, (uint16_t)r.rdata.size());
        p += r.rdata;
    };
    for (const auto& r : an) rr(r);
    for (const auto& r : ar) rr(r);
    return p;
}

/** Who we are, as the answers need it: the name we took, our address ON THE ASKER'S NETWORK, and the service (port 0
 *  = name only — Android, where NsdManager publishes the service). */
struct Self {
    std::string host;
    uint32_t    addr = 0;
    uint16_t    svcPort = 0;
    bool        pin = false;
};

/** The records an unsolicited announcement carries (RFC 6762 §8.3): everything we own. */
std::vector<Rec> ownRecords(const Self& s) {
    std::vector<Rec> v{recA(s.host, s.addr)};
    if (s.svcPort) {
        v.push_back(recPtr(s.host));
        v.push_back(recSrv(s.host, s.svcPort));
        v.push_back(recTxt(s.host, s.pin));
    }
    return v;
}

std::string buildAnnounce(const Self& s) {
    return writeMsg(0, std::string(), 0, ownRecords(s), {recNsec(s.host)}, Ttl::Normal);
}

/** "We are going": the SERVICE records with TTL 0 (RFC 6762 §10.1), so browsers drop the server at once instead of
 *  listing it for two more minutes. ★ Not the A record: a radio process behind the front door answers for the same
 *  name and is still there. */
std::string buildGoodbye(const Self& s) {
    if (!s.svcPort) return std::string();
    return writeMsg(0, std::string(), 0, {recPtr(s.host), recSrv(s.host, s.svcPort), recTxt(s.host, s.pin)}, {},
                    Ttl::Goodbye);
}

/** One record from a query's Answer section (a KNOWN ANSWER), by offsets into its packet. */
struct Known {
    const uint8_t* buf; size_t n;
    size_t   nameOff;
    uint16_t type;
    uint32_t ttl;
    size_t   rdOff;
    uint16_t rdLen;
};

/** Every known answer in `buf`. Stops quietly at the first malformed record (what was read so far still counts). */
void collectKnown(const uint8_t* buf, size_t n, std::vector<Known>& out) {
    if (n < 12) return;
    const uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    const uint16_t an = (uint16_t)((buf[6] << 8) | buf[7]);
    size_t off = 12;
    for (uint16_t q = 0; q < qd; ++q) {
        off = skipName(buf, n, off);
        if (!off || off + 4 > n) return;
        off += 4;
    }
    for (uint16_t a = 0; a < an; ++a) {
        const size_t nameOff = off;
        off = skipName(buf, n, off);
        if (!off || off + 10 > n) return;
        Known k{buf, n, nameOff, (uint16_t)((buf[off] << 8) | buf[off + 1]),
                ((uint32_t)buf[off + 4] << 24) | ((uint32_t)buf[off + 5] << 16) | ((uint32_t)buf[off + 6] << 8) | buf[off + 7],
                off + 10, (uint16_t)((buf[off + 8] << 8) | buf[off + 9])};
        off += 10 + k.rdLen;
        if (off > n) return;
        out.push_back(k);
    }
}

/** Does the asker already hold `r` with at least half its TTL left (RFC 6762 §7.1)? Then it must not be sent. */
bool alreadyKnown(const Rec& r, const std::vector<Known>& ks) {
    for (const auto& k : ks) {
        if (k.type != r.type || k.ttl < (uint32_t)kTtl / 2) continue;
        if (!matchName(k.buf, k.n, k.nameOff, r.name)) continue;
        const uint8_t* rd = k.buf + k.rdOff;
        if (r.type == 12) {                                   // PTR: the target name, maybe compressed
            if (matchName(k.buf, k.rdOff + k.rdLen, k.rdOff, r.rdata)) return true;
        } else if (r.type == 33) {                            // SRV: priority/weight/port, then the target name
            if (k.rdLen > 6 && std::memcmp(rd, r.rdata.data(), 6) == 0 &&
                matchName(k.buf, k.rdOff + k.rdLen, k.rdOff + 6, r.rdata.substr(6)))
                return true;
        } else if (k.rdLen == r.rdata.size() && std::memcmp(rd, r.rdata.data(), k.rdLen) == 0) {
            return true;                                      // A, TXT, an uncompressed NSEC
        }
    }
    return false;
}

/** What respond() decided: a packet, and whether it goes back to the asker alone (legacy: its own port; QU: 5353)
 *  rather than to the group. An empty packet = stay silent. */
struct Response {
    std::string pkt;
    bool        unicast = false;
};

/** A query packet: the first one asks; any more are the known-answer continuations of a TC query (§7.2). */
struct Pkt { const uint8_t* buf; size_t n; };

/**
 * ★★★ THE WHOLE DECISION, WITH NO SOCKET IN SIGHT — so test-mdns can feed it the exact bytes real resolvers send.
 * `legacy` = the query came from a port other than 5353.
 */
Response respond(const std::vector<Pkt>& pkts, const Self& self, bool legacy) {
    Response out;
    if (pkts.empty() || pkts[0].n < 12 || self.host.empty()) return out;
    const std::string want = encodeName(self.host);
    const std::string wantInst = self.svcPort ? svcInstName(self.host) : std::string();
    const std::string wantType = self.svcPort ? svcTypeName() : std::string();
    const std::string wantMeta = self.svcPort ? svcMetaName() : std::string();

    std::vector<Rec> an;
    auto addAn = [&](const Rec& r) {
        for (const auto& x : an) if (sameRec(x, r)) return;
        an.push_back(r);
    };
    bool anyMulticastQ = false;                 // a question we answer that did NOT ask for a unicast reply
    std::string qsec;                           // legacy: the first packet's questions, repeated verbatim
    uint16_t    qsecCount = 0;
    std::vector<Known> known;

    for (size_t pi = 0; pi < pkts.size(); ++pi) {
        const uint8_t* buf = pkts[pi].buf;
        const size_t n = pkts[pi].n;
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                         // a response, not a query
        if ((buf[2] >> 3) & 0x0F) continue;                  // OPCODE ≠ 0: not a standard query — ignored (§18.3)
        const uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        size_t off = 12;
        for (uint16_t q = 0; q < qd; ++q) {
            const bool isHost = matchName(buf, n, off, want) != 0;
            const bool isInst = !wantInst.empty() && matchName(buf, n, off, wantInst);
            const bool isType = !wantType.empty() && matchName(buf, n, off, wantType);
            const bool isMeta = !wantMeta.empty() && matchName(buf, n, off, wantMeta);
            const size_t skip = skipName(buf, n, off);       // however it matched (a pointer ends a name in 2 bytes)
            if (!skip || skip + 4 > n) break;
            const uint16_t qtype  = (uint16_t)((buf[skip] << 8) | buf[skip + 1]);
            const uint16_t qclass = (uint16_t)(((buf[skip + 2] & 0x7F) << 8) | buf[skip + 3]);
            const bool qu = (buf[skip + 2] & 0x80) != 0;
            off = skip + 4;
            if (pi == 0) { qsec.assign((const char*)buf + 12, off - 12); qsecCount = (uint16_t)(q + 1); }
            if (qclass != 1 && qclass != 255) continue;      // IN or ANY only
            const bool any = qtype == 255;
            bool hit = false;
            if (isHost) {
                if (qtype == 1 || any) { addAn(recA(self.host, self.addr)); hit = true; }
                if (qtype == 28)       { addAn(recNsec(self.host)); hit = true; }   // no AAAA: say so
            }
            if (isType && (qtype == 12 || any)) { addAn(recPtr(self.host)); hit = true; }
            if (isInst) {
                if (qtype == 33 || any) { addAn(recSrv(self.host, self.svcPort)); hit = true; }
                if (qtype == 16 || any) { addAn(recTxt(self.host, self.pin)); hit = true; }
            }
            if (isMeta && (qtype == 12 || any)) { addAn(recMeta()); hit = true; }
            if (hit && !qu) anyMulticastQ = true;
        }
        collectKnown(buf, n, known);
    }

    // Known-answer suppression (§7.1). Not for a legacy asker: it never sends any, and owes us no list.
    if (!legacy) {
        std::vector<Rec> keep;
        for (const auto& r : an) if (!alreadyKnown(r, known)) keep.push_back(r);
        an.swap(keep);
    }
    if (an.empty()) return out;

    // Additional records (RFC 6763 §12, RFC 6762 §6.1): whatever the asker would have to ask for next.
    std::vector<Rec> ar;
    auto addAr = [&](const Rec& r) {
        for (const auto& x : an) if (sameRec(x, r)) return;
        for (const auto& x : ar) if (sameRec(x, r)) return;
        if (!legacy && alreadyKnown(r, known)) return;
        ar.push_back(r);
    };
    bool hasPtr = false, hasSrv = false, hasHost = false;
    for (const auto& r : an) {
        hasPtr  = hasPtr  || (r.type == 12 && r.name == wantType && !wantType.empty());
        hasSrv  = hasSrv  || r.type == 33;
        hasHost = hasHost || ((r.type == 1 || r.type == 47) && r.name == want);
    }
    if (hasPtr) {
        addAr(recSrv(self.host, self.svcPort));
        addAr(recTxt(self.host, self.pin));
    }
    if (hasPtr || hasSrv || hasHost) {
        addAr(recA(self.host, self.addr));
        addAr(recNsec(self.host));
    }

    if (legacy) {
        uint16_t txid = (uint16_t)((pkts[0].buf[0] << 8) | pkts[0].buf[1]);
        // ★ The question section is copied byte for byte: it starts at offset 12 in both packets, so any compression
        //   pointers inside it (into earlier questions) still point at the same bytes.
        out.pkt = writeMsg(txid, qsec, qsecCount, an, ar, Ttl::Legacy);
        out.unicast = true;
    } else {
        out.pkt = writeMsg(0, std::string(), 0, an, ar, Ttl::Normal);
        out.unicast = !anyMulticastQ;          // every answered question was QU
    }
    return out;
}

/** The TC bit: "more known answers follow in the next packet(s)". */
bool truncated(const uint8_t* buf, size_t n) { return n >= 12 && (buf[2] & 0x02); }

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

    // ── Announce (RFC 6762 §8.3) ─────────────────────────────────────────────
    // ★ The SERVICE too, unasked: a client that is already listening learns about this server the moment it starts,
    //   instead of only when it next happens to browse — which is what "the server took a while to appear" actually is.
    // ★★ THREE ANNOUNCEMENTS, 1 s THEN 2 s APART, at start AND after every network change (2026-10-20). A change used to
    //    get ONE announcement — and the moment the Portable has just joined a network (hotspot ↔ client) is exactly when
    //    a packet is likeliest to be lost: the station is still settling its power save and its multicast membership.
    //    One lost packet and every listener on the new network kept the old answer, or none, until it next asked.
    using Clock = std::chrono::steady_clock;
    Self self;
    self.host = host;
    self.addr = addr;
    self.svcPort = g_svcPort;
    self.pin = g_svcPin;
    auto selfAt = [&](uint32_t a) { Self s = self; s.addr = a; return s; };
    int burstLeft = 0;
    Clock::time_point nextAnnounce = Clock::now();
    auto startBurst = [&]() { burstLeft = 3; nextAnnounce = Clock::now(); };
    startBurst();
    if (g_svcPort)
        LOGI("mDNS service published: %s.%s.local on port %u", host.c_str(), kSvcType,
             (unsigned)g_svcPort);

    // ★ A query with the TC bit has more known answers coming from the same asker (RFC 6762 §7.2): it is held here for
    //   400–500 ms, its continuation packets are added to it, and it is answered once — knowing everything the asker
    //   already holds. A continuation without TC completes the list and lets it be answered at once.
    struct Held {
        sockaddr_in from;
        std::vector<std::string> pkts;
        Clock::time_point due;
    };
    std::vector<Held> held;

#ifdef VIBE_MDNS_PER_IFACE
    // ★★★ OUR ADDRESS ON THE ASKER'S NETWORK — and if we have none there, LOOK AGAIN before answering (2026-10-20).
    //     Interfaces were re-read every 5 s, so for up to 5 s after a hotspot ↔ client switch a question from the NEW
    //     network matched no interface and was answered with the fallback — the address on the network the box had just
    //     LEFT (10.42.0.1), with the cache-flush bit: the asker cached a wrong address for two minutes. Now a question we
    //     cannot place re-reads the interfaces (one getifaddrs), and a change found that way is announced at once.
    auto hereFor = [&](uint32_t peer) {
        uint32_t a = addrFor(ifs, peer, 0);
        if (a) return a;
        std::vector<Ifc> nowIfs = listIfcs();
        if (!sameIfcs(nowIfs, ifs)) {
            ifs = std::move(nowIfs);
            joinEach(fd, ifs);
            const uint32_t now = currentIpv4();
            if (now) { addr = now; g_addr = now; }
            LOGI("mDNS: the interfaces changed (seen on a question) — announcing");
            startBurst();
            a = addrFor(ifs, peer, 0);
        }
        if (a) return a;
        const uint32_t now = currentIpv4();               // a routed or link-local asker: our primary address NOW
        return now ? now : addr;
    };
#else
    auto hereFor = [&](uint32_t) { return addr; };
#endif

    auto send = [&](const Response& r, const sockaddr_in& asker, bool legacy, uint32_t here) {
        if (r.pkt.empty()) return;
        if (legacy) { sendTo(fd, r.pkt, asker); return; }     // its own port, its own id
        if (r.unicast) {                                      // QU: straight back to the asker's 5353
            sockaddr_in to = asker;
            to.sin_port = htons(kPort);
            sendTo(fd, r.pkt, to);
            return;
        }
#ifdef VIBE_MDNS_PER_IFACE
        sendVia(fd, r.pkt, group, here);                      // out of the interface the asker shares
#else
        (void)here;
        sendTo(fd, r.pkt, group);
#endif
    };
    auto answerHeld = [&](Held& h) {
        std::vector<Pkt> ps;
        for (const auto& s : h.pkts) ps.push_back({(const uint8_t*)s.data(), s.size()});
        const uint32_t here = hereFor(h.from.sin_addr.s_addr);
        send(respond(ps, selfAt(here), false), h.from, false, here);
    };

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
    (void)lastIp;                                     // unused where the address is not followed (Android < 24)
    while (g_run.load()) {
        if (burstLeft > 0 && Clock::now() >= nextAnnounce) {
            toGroup([&](uint32_t a) { return buildAnnounce(selfAt(a)); });
            --burstLeft;
            nextAnnounce = Clock::now() + std::chrono::seconds(burstLeft == 2 ? 1 : 2);
        }
#ifdef VIBE_MDNS_FOLLOW_IP
        // ★★★ FOLLOW THE ADDRESS (2026-10-10, the pocket box). The address was taken once, at start —
        //     right for a Pi that never moves, wrong for a box that goes from its own hotspot (10.42.0.1)
        //     to the owner's Wi-Fi to their phone's hotspot without restarting: the name kept answering
        //     with an address on a network the box had left. ★ Looked at every 2 s (one getifaddrs — it was
        //     5 s, and every one of those seconds after a switch is a question answered with the old address
        //     on Android, which has no per-interface lookup); on a change the new address is announced.
        {
            const time_t nowT = time(nullptr);
            if (nowT - lastIp >= 2 || nowT < lastIp) {
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
                    startBurst();
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
                    startBurst();
                }
            }
        }
        // Held TC queries whose wait is over.
        for (size_t i = 0; i < held.size();) {
            if (Clock::now() >= held[i].due) { answerHeld(held[i]); held.erase(held.begin() + (long)i); }
            else ++i;
        }

        // Sleep until the next thing that has a time: a packet, a held query's deadline, an announcement, or 1 s.
        auto wait = std::chrono::milliseconds(1000);
        const auto nowC = Clock::now();
        auto sooner = [&](Clock::time_point t) {
            const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(t - nowC);
            if (d < wait) wait = d < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : d;
        };
        for (const auto& h : held) sooner(h.due);
        if (burstLeft > 0) sooner(nextAnnounce);
        timeval tv{(time_t)(wait.count() / 1000), (suseconds_t)((wait.count() % 1000) * 1000)};
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        if (select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;

        uint8_t buf[9000];                             // mDNS allows up to 9000 bytes (§17); a jumbo KA list is legal
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                   // a response, not a question
        const bool legacy = ntohs(from.sin_port) != kPort;
        if (legacy) {                                  // a plain DNS client: no known answers, no TC, answer now
            const uint32_t here = hereFor(from.sin_addr.s_addr);
            send(respond({{buf, (size_t)n}}, selfAt(here), true), from, true, here);
            continue;
        }
        // A continuation of a held TC query from the same asker: its known answers join the wait.
        Held* h = nullptr;
        for (auto& x : held)
            if (x.from.sin_addr.s_addr == from.sin_addr.s_addr && x.from.sin_port == from.sin_port) h = &x;
        if (h) {
            h->pkts.emplace_back((const char*)buf, (size_t)n);
            if (truncated(buf, n)) h->due = Clock::now() + std::chrono::milliseconds(400 + rand() % 100);
            else                   h->due = Clock::now();      // the list is complete
            continue;
        }
        const uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qd == 0) continue;                         // known answers with no question we are holding: nothing asked
        if (truncated(buf, n)) {
            if (held.size() >= 16) { answerHeld(held.front()); held.erase(held.begin()); }
            held.push_back({from, {std::string((const char*)buf, (size_t)n)},
                            Clock::now() + std::chrono::milliseconds(400 + rand() % 100)});
            continue;
        }
        const uint32_t here = hereFor(from.sin_addr.s_addr);
        send(respond({{buf, (size_t)n}}, selfAt(here), false), from, false, here);
    }
    // ★ Going: say so for the service, so the Discovered lists drop this server now rather than in two minutes.
    toGroup([&](uint32_t a) { return buildGoodbye(selfAt(a)); });
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
