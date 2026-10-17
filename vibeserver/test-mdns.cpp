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
    // 8) legacy unicast reply: the asker's id, its question repeated, TTL 10, no cache-flush
    {
        g_legacy = true;
        const uint32_t ip = inet_addr("192.168.86.158");
        std::string q = want + qtail(1);
        std::string r = legacyReply(buildAnswer(host, ip, 0), 0xBEEF, q);
        g_legacy = false;
        const uint8_t* b = U(r);
        CHECK(b[0] == 0xBE && b[1] == 0xEF, "8) legacy: transaction id echoed");
        CHECK(b[4] == 0 && b[5] == 1 && r.compare(12, q.size(), q) == 0, "8) legacy: question repeated");
        const size_t rr = 12 + q.size() + want.size();       // type(2) class(2) ttl(4)
        CHECK(b[rr + 2] == 0x00 && b[rr + 3] == 0x01, "8) legacy: no cache-flush bit");
        CHECK(b[rr + 4] == 0 && b[rr + 5] == 0 && b[rr + 6] == 0 && b[rr + 7] == 10, "8) legacy: TTL 10");
        std::string m = buildAnswer(host, ip, 0);
        const size_t mr = 12 + want.size();
        CHECK((uint8_t)m[mr + 2] == 0x80 && (uint8_t)m[mr + 7] == 120, "8) multicast answer unchanged (flush, 120 s)");
    }

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
