// test-mdns — the mDNS responder's question parser against the packet shapes real askers send.
//
// ★★★ 2026-10-17: vibeserver-portable.local was "not reachable for most of the day" while the same box's avahi name
//     (vibepocket.local) always was. The responder returned no-match on any compression pointer in a question, and
//     Apple's mDNSResponder packs several lookups into one packet, compressing the later names. Shapes 2–4 below are
//     the ones measured on the LAN: a lone A was answered, AAAA + A(ptr) got only the NSEC, other.local + ours(ptr)
//     got nothing.
//
// The parser lives in an anonymous namespace, so the source is included whole (no sockets are opened here).
#include "mdns_responder.cpp"

#include <cstdio>
#include <string>

using namespace vibe;

static int g_fail = 0;
#define CHECK(cond, what) do { if (cond) std::printf("ok   %s\n", what); \
                               else { std::printf("FAIL %s\n", what); ++g_fail; } } while (0)

static std::string hdr(int qd) {
    std::string h(12, '\0');
    h[5] = (char)qd;
    return h;
}
static std::string qtail(uint16_t type) {
    std::string t;
    t += (char)(type >> 8); t += (char)(type & 0xFF); t += (char)0; t += (char)1;
    return t;
}
static const uint8_t* U(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

/** Walk every question the way the responder's serve loop does; true if `want` is asked with `type`. */
static bool asks(const std::string& pkt, const std::string& want, uint16_t type) {
    const size_t n = pkt.size();
    const uint8_t* buf = U(pkt);
    const int qd = (buf[4] << 8) | buf[5];
    size_t off = 12;
    for (int q = 0; q < qd && off + 4 < n; ++q) {
        const bool hit = matchName(buf, n, off, want) != 0;
        const size_t skip = skipName(buf, n, off);
        if (!skip || skip + 4 > n) return false;
        const uint16_t qt = (uint16_t)((buf[skip] << 8) | buf[skip + 1]);
        if (hit && qt == type) return true;
        off = skip + 4;
    }
    return false;
}

// ── respond(): whole packets in, whole responses out ────────────────────────────────────────────────────────────
// ★★★ 2026-10-20: the Portable (11.0.4) took 4.3 s to appear in `dns-sd -B _vibesdr._tcp` while every other server
//     appeared at once, its .local name failed in browsers, an A+AAAA query got no NSEC and a legacy query got nothing.
//     Each shape below is a packet a real resolver sends (mDNSResponder packs and compresses, Android asks QU first,
//     dig/nslookup ask from a random port); each check is what RFC 6762/6763 says must come back.

struct RR { std::string sec; std::string name; uint16_t type; uint16_t cls; uint32_t ttl; std::string rdata; };
struct Parsed { uint16_t id = 0, flags = 0, qd = 0; std::vector<RR> rrs; bool ok = false; };

static std::string nameAt(const std::string& p, size_t& i) {     // reads a (possibly compressed) name, advances i
    std::string out;
    size_t j = i;
    bool jumped = false;
    for (int hops = 0; j < p.size() && hops < 20;) {
        const uint8_t l = (uint8_t)p[j];
        if (l == 0) { if (!jumped) i = j + 1; break; }
        if ((l & 0xC0) == 0xC0) {
            if (!jumped) i = j + 2;
            jumped = true;
            j = (size_t)(((l & 0x3F) << 8) | (uint8_t)p[j + 1]);
            ++hops;
            continue;
        }
        out += p.substr(j + 1, l) + ".";
        j += 1 + l;
    }
    return out;
}
static Parsed parse(const std::string& p) {
    Parsed r;
    if (p.size() < 12) return r;
    auto g16 = [&](size_t o) { return (uint16_t)(((uint8_t)p[o] << 8) | (uint8_t)p[o + 1]); };
    r.id = g16(0); r.flags = g16(2); r.qd = g16(4);
    const int an = g16(6), ns = g16(8), ar = g16(10);
    size_t i = 12;
    for (int q = 0; q < r.qd; ++q) { nameAt(p, i); i += 4; }
    const char* secs[3] = {"an", "ns", "ar"};
    const int cnt[3] = {an, ns, ar};
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < cnt[s]; ++k) {
            RR x;
            x.sec = secs[s];
            x.name = nameAt(p, i);
            if (i + 10 > p.size()) return r;
            x.type = g16(i); x.cls = g16(i + 2);
            x.ttl = ((uint32_t)g16(i + 4) << 16) | g16(i + 6);
            const uint16_t rl = g16(i + 8);
            x.rdata = p.substr(i + 10, rl);
            i += 10 + rl;
            r.rrs.push_back(x);
        }
    r.ok = i == p.size();
    return r;
}
static int count(const Parsed& p, const char* sec, uint16_t type) {
    int c = 0;
    for (const auto& x : p.rrs) c += (x.sec == sec && x.type == type);
    return c;
}
static const RR* find(const Parsed& p, const char* sec, uint16_t type) {
    for (const auto& x : p.rrs) if (x.sec == sec && x.type == type) return &x;
    return nullptr;
}
static std::string q(const std::string& encName, uint16_t type, bool qu = false) {
    std::string t = encName;
    t += (char)(type >> 8); t += (char)(type & 0xFF); t += (char)(qu ? 0x80 : 0x00); t += (char)1;
    return t;
}
static std::string hdr2(uint16_t id, uint16_t flags, int qd, int an) {
    std::string h(12, '\0');
    h[0] = (char)(id >> 8); h[1] = (char)(id & 0xFF);
    h[2] = (char)(flags >> 8); h[3] = (char)(flags & 0xFF);
    h[5] = (char)qd; h[7] = (char)an;
    return h;
}
/** A known-answer record as an asker writes it (name may be a pointer), class IN, the given TTL. */
static std::string ka(const std::string& name, uint16_t type, uint32_t ttl, const std::string& rdata) {
    std::string r = name;
    r += (char)(type >> 8); r += (char)(type & 0xFF); r += (char)0; r += (char)1;
    r += (char)(ttl >> 24); r += (char)(ttl >> 16); r += (char)(ttl >> 8); r += (char)ttl;
    r += (char)(rdata.size() >> 8); r += (char)(rdata.size() & 0xFF);
    return r + rdata;
}
static Response ask(const std::vector<std::string>& pkts, const Self& s, bool legacy = false) {
    std::vector<Pkt> ps;
    for (const auto& p : pkts) ps.push_back({U(p), p.size()});
    return respond(ps, s, legacy);
}

static void runRespondTests() {
    Self s;
    s.host = "vibeserver-portable";
    s.addr = inet_addr("192.168.86.158");
    s.svcPort = 48000;
    s.pin = false;
    Self nameOnly = s;
    nameOnly.svcPort = 0;
    const std::string H = encodeName(s.host);
    const std::string T = svcTypeName();
    const std::string I = svcInstName(s.host);
    const std::string addr4((const char*)&s.addr, 4);

    // 10) A + AAAA in one packet (AAAA by pointer) → ONE response: A answered, NSEC for the missing AAAA
    {
        Response r = ask({hdr2(0, 0, 2, 0) + q(H, 1) + q(std::string("\xC0\x0C", 2), 28)}, s);
        Parsed p = parse(r.pkt);
        CHECK(p.ok && !r.unicast, "10) A+AAAA: one well-formed multicast response");
        CHECK(count(p, "an", 1) == 1 && count(p, "an", 47) == 1, "10) A+AAAA: A and NSEC both answered");
        const RR* a = find(p, "an", 1);
        CHECK(a && a->rdata == addr4 && a->ttl == 120 && (a->cls & 0x8000), "10) A: our address, 120 s, cache-flush");
        const RR* n = find(p, "an", 47);
        CHECK(n && n->rdata.size() == H.size() + 3 && (uint8_t)n->rdata.back() == 0x40, "10) NSEC: types = {A} only");
        CHECK(p.qd == 0 && p.id == 0 && (p.flags & 0x8400) == 0x8400, "10) multicast: no questions, id 0, QR+AA");
    }
    // 11) A alone → A, and the NSEC rides along as an ADDITIONAL (§6.1) so the asker never waits on AAAA
    {
        Parsed p = parse(ask({hdr2(0, 0, 1, 0) + q(H, 1)}, s).pkt);
        CHECK(count(p, "an", 1) == 1 && count(p, "ar", 47) == 1, "11) lone A: NSEC in additionals");
    }
    // 12) AAAA alone → NSEC answered, A as an additional
    {
        Parsed p = parse(ask({hdr2(0, 0, 1, 0) + q(H, 28)}, s).pkt);
        CHECK(count(p, "an", 47) == 1 && count(p, "ar", 1) == 1 && count(p, "an", 28) == 0,
              "12) lone AAAA: NSEC answer + A additional, never a fake AAAA");
    }
    // 13) browse: PTR answered; SRV, TXT, A, NSEC as ADDITIONALS (RFC 6763 §12.1); PTR shared (no flush)
    {
        Parsed p = parse(ask({hdr2(0, 0, 1, 0) + q(T, 12)}, s).pkt);
        CHECK(p.ok && count(p, "an", 12) == 1 && p.rrs.size() == 5, "13) browse: 1 answer + 4 additionals");
        CHECK(count(p, "ar", 33) == 1 && count(p, "ar", 16) == 1 && count(p, "ar", 1) == 1 && count(p, "ar", 47) == 1,
              "13) browse: SRV, TXT, A, NSEC in additionals");
        const RR* ptr = find(p, "an", 12);
        CHECK(ptr && !(ptr->cls & 0x8000) && ptr->rdata == I, "13) PTR: no cache-flush, points at our instance");
        const RR* srv = find(p, "ar", 33);
        CHECK(srv && (uint8_t)srv->rdata[4] == (48000 >> 8) && (uint8_t)srv->rdata[5] == (48000 & 0xFF)
              && srv->rdata.substr(6) == H, "13) SRV: port 48000, target our host");
        const RR* txt = find(p, "ar", 16);
        CHECK(txt && txt->rdata.find("proto=vibeserver") != std::string::npos
              && txt->rdata.find("pin=0") != std::string::npos, "13) TXT: proto + pin keys");
    }
    // 14) Apple's packed browse: _airplay._tcp, then _vibesdr + pointer to its "_tcp.local", then _raop the same way —
    //     the shape that 11.0.4 never answered (the 4.3 s wait was the asker's retries until ours came first)
    {
        const std::string air = encodeDotted("_airplay._tcp.local");
        const size_t tcpAt = 12 + 1 + 8;
        std::string ptr2;
        ptr2 += (char)(0xC0 | (tcpAt >> 8)); ptr2 += (char)(tcpAt & 0xFF);
        std::string p = hdr2(0, 0, 3, 0) + q(air, 12) + q(std::string("\x08_vibesdr", 9) + ptr2, 12)
                      + q(std::string("\x05_raop", 6) + ptr2, 12);
        Parsed r = parse(ask({p}, s).pkt);
        CHECK(count(r, "an", 12) == 1 && find(r, "an", 12)->rdata == I, "14) packed browse (ptr to _tcp.local): answered");
    }
    // 15) known-answer suppression (§7.1): the asker holds our PTR with ≥ half TTL → silence; with less → answered
    {
        const std::string fresh = hdr2(0, 0, 1, 1) + q(T, 12) + ka(std::string("\xC0\x0C", 2), 12, 4500, I);
        CHECK(ask({fresh}, s).pkt.empty(), "15) KA: our PTR already held (TTL 4500, name by pointer) → silent");
        const std::string stale = hdr2(0, 0, 1, 1) + q(T, 12) + ka(std::string("\xC0\x0C", 2), 12, 10, I);
        CHECK(count(parse(ask({stale}, s).pkt), "an", 12) == 1, "15) KA: TTL 10 (below half) → answered");
        const std::string other = hdr2(0, 0, 1, 1) + q(T, 12)
                                + ka(std::string("\xC0\x0C", 2), 12, 4500, svcInstName("living-room-pi"));
        CHECK(count(parse(ask({other}, s).pkt), "an", 12) == 1, "15) KA: someone ELSE's PTR does not silence ours");
        // the PTR target in the known answer compressed against the question's type name
        std::string ptrRd;
        ptrRd += (char)s.host.size(); ptrRd += s.host; ptrRd += "\xC0\x0C";
        const std::string compressed = hdr2(0, 0, 1, 1) + q(T, 12) + ka(std::string("\xC0\x0C", 2), 12, 4500, ptrRd);
        CHECK(ask({compressed}, s).pkt.empty(), "15) KA: PTR target compressed against the question → still matched");
        // A known: the A question is silent; the NSEC that rides along must not answer on its own
        const std::string aKnown = hdr2(0, 0, 1, 1) + q(H, 1) + ka(std::string("\xC0\x0C", 2), 1, 120, addr4);
        CHECK(ask({aKnown}, s).pkt.empty(), "15) KA: A already held → silent (additionals never answer alone)");
        // a resolve where the SRV is held but the TXT is not: TXT only
        std::string srvRd;
        srvRd += std::string("\0\0\0\0", 4); srvRd += (char)(48000 >> 8); srvRd += (char)(48000 & 0xFF); srvRd += H;
        const size_t instAt = 12;
        std::string ptrI;
        ptrI += (char)(0xC0 | (instAt >> 8)); ptrI += (char)(instAt & 0xFF);
        const std::string res = hdr2(0, 0, 2, 1) + q(I, 33) + q(ptrI, 16) + ka(ptrI, 33, 120, srvRd);
        Parsed r = parse(ask({res}, s).pkt);
        CHECK(count(r, "an", 33) == 0 && count(r, "an", 16) == 1, "15) KA: SRV held, TXT not → TXT only");
    }
    // 16) TC (§7.2): the known answers continue in a second packet — respond() given both stays silent
    {
        const std::string first = hdr2(0, 0x0200, 1, 0) + q(T, 12);
        const std::string cont  = hdr2(0, 0, 0, 1) + ka(T, 12, 4500, I);
        CHECK(truncated(U(first), first.size()) && !truncated(U(cont), cont.size()), "16) TC bit read");
        CHECK(ask({first, cont}, s).pkt.empty(), "16) TC + continuation holding our PTR → silent");
        CHECK(count(parse(ask({first}, s).pkt), "an", 12) == 1, "16) TC with no continuation (deadline) → answered");
    }
    // 17) QU (§5.4): every answered question QU → unicast; one QM among them → multicast
    {
        Response r = ask({hdr2(0, 0, 1, 0) + q(H, 1, true)}, s);
        CHECK(!r.pkt.empty() && r.unicast, "17) QU A → unicast reply");
        Response m = ask({hdr2(0, 0, 2, 0) + q(H, 1, true) + q(T, 12)}, s);
        CHECK(!m.pkt.empty() && !m.unicast, "17) QU + QM in one packet → multicast");
        Response o = ask({hdr2(0, 0, 2, 0) + q(encodeName("living-room-tv"), 1) + q(H, 1, true)}, s);
        CHECK(o.unicast, "17) someone else's QM question does not force multicast");
    }
    // 18) legacy unicast (§6.7): id + questions echoed (verbatim, pointers intact), TTL 10, no cache-flush, unicast
    {
        const std::string pkt = hdr2(0xBEEF, 0x0100, 2, 0) + q(H, 1) + q(std::string("\xC0\x0C", 2), 28);
        Response r = ask({pkt}, s, true);
        Parsed p = parse(r.pkt);
        CHECK(r.unicast && p.ok, "18) legacy: unicast, well-formed");
        CHECK(p.id == 0xBEEF && p.qd == 2 && r.pkt.compare(12, pkt.size() - 12, pkt, 12) == 0,
              "18) legacy: id and both questions echoed verbatim");
        bool allTen = !p.rrs.empty(), noFlush = true;
        for (const auto& x : p.rrs) { allTen = allTen && x.ttl == 10; noFlush = noFlush && !(x.cls & 0x8000); }
        CHECK(allTen && noFlush, "18) legacy: every record TTL 10, no cache-flush");
        CHECK(count(p, "an", 1) == 1 && count(p, "an", 47) == 1, "18) legacy: A + NSEC");
        // a legacy browse (some apps resolve with plain unicast DNS to 224.0.0.251:5353)
        Parsed b = parse(ask({hdr2(7, 0, 1, 0) + q(T, 12)}, s, true).pkt);
        CHECK(b.id == 7 && count(b, "an", 12) == 1 && count(b, "ar", 33) == 1, "18) legacy browse: PTR + SRV");
    }
    // 19) instance SRV+TXT (what a resolve asks), and ANY on each name
    {
        Parsed p = parse(ask({hdr2(0, 0, 2, 0) + q(I, 33) + q(std::string("\xC0\x0C", 2), 16)}, s).pkt);
        CHECK(count(p, "an", 33) == 1 && count(p, "an", 16) == 1 && count(p, "ar", 1) == 1 && count(p, "an", 12) == 0,
              "19) resolve: SRV+TXT answered, A additional, no PTR");
        Parsed any = parse(ask({hdr2(0, 0, 1, 0) + q(I, 255)}, s).pkt);
        CHECK(count(any, "an", 33) == 1 && count(any, "an", 16) == 1, "19) ANY on the instance: SRV + TXT");
        Parsed meta = parse(ask({hdr2(0, 0, 1, 0) + q(encodeDotted("_services._dns-sd._udp.local"), 12)}, s).pkt);
        CHECK(count(meta, "an", 12) == 1 && find(meta, "an", 12)->rdata == T, "19) meta-query: our service type");
    }
    // 20) a name-only responder (Android: NsdManager owns the service) answers the name and never the service
    {
        CHECK(ask({hdr2(0, 0, 1, 0) + q(T, 12)}, nameOnly).pkt.empty(), "20) name-only: browse not answered");
        CHECK(count(parse(ask({hdr2(0, 0, 1, 0) + q(H, 1)}, nameOnly).pkt), "an", 1) == 1, "20) name-only: A answered");
    }
    // 21) silence where silence is right: other names, responses, non-zero opcode, class CH, a malformed packet
    {
        CHECK(ask({hdr2(0, 0, 1, 0) + q(encodeName("living-room-tv"), 1)}, s).pkt.empty(), "21) other name → silent");
        CHECK(ask({hdr2(0, 0x8400, 1, 0) + q(H, 1)}, s).pkt.empty(), "21) a response is not a question");
        CHECK(ask({hdr2(0, 0x2800, 1, 0) + q(H, 1)}, s).pkt.empty(), "21) opcode 5 (update) → silent");
        std::string ch = H + std::string("\x00\x01\x00\x03", 4);
        CHECK(ask({hdr2(0, 0, 1, 0) + ch}, s).pkt.empty(), "21) class CH → silent");
        CHECK(ask({hdr2(0, 0, 3, 0) + q(H, 1).substr(0, 5)}, s).pkt.empty(), "21) truncated question → silent, no overrun");
        // a known-answer section that lies about its length must not overrun, and must not suppress
        std::string lying = hdr2(0, 0, 1, 1) + q(T, 12) + std::string("\xC0\x0C\x00\x0C\x00\x01\x00\x00\x11\x94\x00\xFF", 12);
        CHECK(count(parse(ask({lying}, s).pkt), "an", 12) == 1, "21) KA with rdlength past the end: ignored, answered");
    }
    // 22) announcement and goodbye
    {
        Parsed a = parse(buildAnnounce(s));
        CHECK(a.ok && count(a, "an", 1) == 1 && count(a, "an", 12) == 1 && count(a, "an", 33) == 1
              && count(a, "an", 16) == 1 && count(a, "ar", 47) == 1, "22) announce: A PTR SRV TXT + NSEC");
        Parsed g = parse(buildGoodbye(s));
        bool zero = !g.rrs.empty();
        for (const auto& x : g.rrs) zero = zero && x.ttl == 0;
        CHECK(g.ok && zero && count(g, "an", 12) == 1 && count(g, "an", 1) == 0,
              "22) goodbye: service records at TTL 0, never the A (the radio behind the door still answers)");
        CHECK(buildGoodbye(nameOnly).empty(), "22) name-only: no goodbye");
    }
    // 23) answering with the address of the asker's network (the Portable on Ethernet + its own hotspot)
    {
        Self hot = s;
        hot.addr = inet_addr("10.42.0.1");
        Parsed p = parse(ask({hdr2(0, 0, 1, 0) + q(H, 1)}, hot).pkt);
        const uint32_t want10 = inet_addr("10.42.0.1");
        CHECK(find(p, "an", 1) && find(p, "an", 1)->rdata == std::string((const char*)&want10, 4),
              "23) the address given is the one respond() is told (the asker's interface)");
    }
}

int main() {
    const std::string host = "vibeserver-portable";
    const std::string want = encodeName(host);

    // 1) one question, no compression — the only shape that ever worked
    {
        std::string p = hdr(1) + want + qtail(1);
        CHECK(asks(p, want, 1), "1) lone A question");
    }
    // 2) A, then AAAA by pointer to the first name (offset 12)
    {
        std::string p = hdr(2) + want + qtail(1) + std::string("\xC0\x0C", 2) + qtail(28);
        CHECK(asks(p, want, 1), "2) A + AAAA(ptr): A seen");
        CHECK(asks(p, want, 28), "2) A + AAAA(ptr): AAAA seen through the pointer");
    }
    // 3) AAAA first, then A by pointer — the A used to be lost (only the NSEC went out)
    {
        std::string p = hdr(2) + want + qtail(28) + std::string("\xC0\x0C", 2) + qtail(1);
        CHECK(asks(p, want, 1), "3) AAAA + A(ptr): A seen through the pointer");
    }
    // 4) someone else's question first; ours is "vibeserver-portable" + pointer to that question's "local"
    {
        const std::string other = encodeName("living-room-tv");
        const size_t localAt = 12 + 1 + std::string("living-room-tv").size();
        std::string ours;
        ours += (char)host.size(); ours += host;
        ours += (char)(0xC0 | (localAt >> 8)); ours += (char)(localAt & 0xFF);
        std::string p = hdr(2) + other + qtail(1) + ours + qtail(1);
        CHECK(asks(p, want, 1), "4) other.local + ours(ptr .local): ours seen");
        CHECK(!asks(p, encodeName("living-room"), 1), "4) a prefix of another name is not a match");
    }
    // 5) case-blind through a pointer
    {
        std::string p = hdr(2) + encodeName("VibeServer-Portable") + qtail(28) + std::string("\xC0\x0C", 2) + qtail(1);
        CHECK(asks(p, want, 1), "5) mixed case + pointer");
    }
    // 6) hostile packets: a pointer to itself, a forward pointer, a truncated pointer — no match, no hang, no overrun
    {
        std::string self = hdr(1) + std::string("\xC0\x0C", 2) + qtail(1);
        CHECK(!asks(self, want, 1), "6) pointer to itself is refused");
        std::string fwd = hdr(1) + std::string("\xC0\x20", 2) + qtail(1) + std::string(32, '\0') + want;
        CHECK(!asks(fwd, want, 1), "6) forward pointer is refused");
        std::string cut = hdr(1) + std::string("\xC0", 1);
        CHECK(skipName(U(cut), cut.size(), 12) == 0, "6) truncated pointer is malformed");
        std::string reserved = hdr(1) + std::string("\x40", 1) + want + qtail(1);
        CHECK(!asks(reserved, want, 1), "6) reserved label type is refused");
    }
    // 7) the skip length: a name that ends in a pointer occupies exactly label bytes + 2
    {
        std::string p = hdr(2) + want + qtail(1) + std::string("\x03" "abc" "\xC0\x0C", 6) + qtail(1);
        const size_t second = 12 + want.size() + 4;
        CHECK(skipName(U(p), p.size(), second) == second + 6, "7) skip stops two bytes after the pointer");
        CHECK(matchName(U(p), p.size(), 12, want) == want.size(), "7) match returns the bytes at the question");
    }
    runRespondTests();

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
