#include "vibe_admin.h"
#include <cstdio>
using namespace vibeadmin;

static int fails = 0;
static void ck(bool got, bool want, const char* what) {
    if (got != want) { printf("FAIL %s (got %d want %d)\n", what, (int)got, (int)want); fails++; }
}
static bool hit(const char* cidr, const char* ip) {
    Ban b; b.cidr = cidr;
    if (!compile(b)) { printf("FAIL compile %s\n", cidr); fails++; return false; }
    return matches(b, ip);
}
static bool bad(const char* cidr) { Ban b; b.cidr = cidr; return !compile(b); }

int main() {
    // v4 exact
    ck(hit("192.0.2.7", "192.0.2.7"), true,  "v4 exact");
    ck(hit("192.0.2.7", "192.0.2.8"), false, "v4 exact miss");
    // v4 ranges
    ck(hit("192.0.2.0/24", "192.0.2.8"),   true,  "v4 /24 in");
    ck(hit("192.0.2.0/24", "192.0.3.8"),   false, "v4 /24 out");
    ck(hit("10.0.0.0/8",   "10.255.3.9"),  true,  "v4 /8 in");
    ck(hit("10.0.0.0/8",   "11.0.0.1"),    false, "v4 /8 out");
    // ★ host bits set by the user must be masked off, else this is a single-address ban
    ck(hit("192.0.2.7/24", "192.0.2.99"),  true,  "v4 host bits masked");
    // non-byte-aligned prefix
    ck(hit("192.0.2.0/25", "192.0.2.127"), true,  "v4 /25 in");
    ck(hit("192.0.2.0/25", "192.0.2.128"), false, "v4 /25 out");
    ck(hit("192.0.2.0/23", "192.0.3.5"),   true,  "v4 /23 in");
    ck(hit("192.0.2.0/23", "192.0.4.5"),   false, "v4 /23 out");
    // /0 and /32
    ck(hit("0.0.0.0/0",    "8.8.8.8"),     true,  "v4 /0 matches all");
    ck(hit("192.0.2.7/32", "192.0.2.7"),   true,  "v4 /32");
    // v6
    ck(hit("2001:db8::1", "2001:db8::1"),      true,  "v6 exact");
    ck(hit("2001:db8::1", "2001:db8::2"),      false, "v6 exact miss");
    ck(hit("2001:db8::/32", "2001:db8:1::9"),  true,  "v6 /32 in");
    ck(hit("2001:db8::/32", "2001:db9:1::9"),  false, "v6 /32 out");
    ck(hit("2001:db8::/33", "2001:db8:7fff::1"), true,  "v6 /33 in");
    ck(hit("2001:db8::/33", "2001:db8:8000::1"), false, "v6 /33 out");
    ck(hit("::1", "::1"),                      true,  "v6 loopback");
    ck(hit("fe80::/10", "fe80::abcd"),         true,  "v6 link-local");
    // families never cross
    ck(hit("0.0.0.0/0", "2001:db8::1"), false, "v4 rule never matches v6");
    ck(hit("::/0",      "192.0.2.7"),   false, "v6 rule never matches v4");
    // rejections
    ck(bad("nonsense"),        true, "reject text");
    ck(bad("192.0.2"),         true, "reject short v4");
    ck(bad("192.0.2.300"),     true, "reject octet > 255");
    ck(bad("192.0.2.0/33"),    true, "reject v4 prefix > 32");
    ck(bad("2001:db8::/129"),  true, "reject v6 prefix > 128");
    ck(bad(""),                true, "reject empty");
    ck(bad("1.2.3.4.5"),       true, "reject long v4");

    // BanList: add, match, remove, expiry, persistence
    {
        const char* p = "/tmp/vibe-ban-test.jsonl";
        remove(p);
        BanList bl; bl.setPath(p);
        std::string err, why;
        ck(bl.add("192.0.2.0/24", "scraper", 0, err), true, "add ok");
        ck(bl.banned("192.0.2.9", &why), true, "banned in range");
        if (why != "scraper") { printf("FAIL reason (%s)\n", why.c_str()); fails++; }
        ck(bl.banned("192.0.3.9"), false, "not banned outside");
        ck(bl.add("bogus", "x", 0, err), false, "add rejects bad cidr");

        BanList bl2; bl2.setPath(p);               // reload from disk
        ck(bl2.banned("192.0.2.9"), true, "survives reload");
        ck(bl2.remove("192.0.2.0/24"), true, "remove ok");
        ck(bl2.banned("192.0.2.9"), false, "gone after remove");

        BanList bl3; bl3.setPath(p);
        ck(bl3.banned("192.0.2.9"), false, "removal persisted");
        remove(p);
    }
    // ConnLog: close attributes to the most recent open record for the session
    {
        ConnLog cl;
        cl.open("192.0.2.1", "sessA", "VibeSDR/1.0");
        cl.open("192.0.2.1", "sessB", "Mozilla");
        cl.close("192.0.2.1", "sessA", "kicked", 4242);
        const std::string j = cl.json();
        if (j.find("\"reason\":\"kicked\"") == std::string::npos) { printf("FAIL conn kicked\n"); fails++; }
        if (j.find("4242") == std::string::npos) { printf("FAIL conn bytes\n"); fails++; }
        ck(cl.uniqueSince(3600) == 1, true, "unique addresses");
        // a refusal with no prior open still lands in the log
        cl.close("192.0.2.55", "", "banned");
        if (cl.json().find("192.0.2.55") == std::string::npos) { printf("FAIL refusal logged\n"); fails++; }
    }
    // ── ★★★ ASN BANS ────────────────────────────────────────────────────────────────────
    {
        // A ban that blocks a whole NETWORK is the most consequential entry in this list, so it
        // gets the most checks — especially the negatives.
        Ban b; b.cidr = "AS15169";
        ck(compile(b), true, "AS15169 compiles");
        ck(b.asn == 15169, true, "and carries the AS number");
        ck(b.valid, true, "and is valid");
        // ★★ AN ASN RULE MUST NEVER MATCH ON ADDRESS BITS. Its `net` is uninitialised, so a
        //    fall-through into the CIDR matcher could block anything at all.
        ck(matches(b, "8.8.8.8"), false, "an ASN rule never matches by address");
        ck(matches(b, "0.0.0.0"), false, "not even 0.0.0.0");

        Ban lower; lower.cidr = "as13335";
        ck(compile(lower) && lower.asn == 13335, true, "lower-case as13335 works");

        for (const char* bad : {"AS", "ASX", "AS0", "AS12x", "AS-1"}) {
            Ban r; r.cidr = bad;
            ck(compile(r), false, (std::string("rejects ") + bad).c_str());
        }

        // End to end through the list, with a resolver standing in for asndb.
        const char* p2 = "/tmp/vibe-ban-asn-test.jsonl";
        remove(p2);
        BanList bl; bl.setPath(p2);
        bl.setAsnResolver([](const std::string& ip, uint32_t& a) {
            if (ip == "8.8.8.8")   { a = 15169; return true; }
            if (ip == "1.1.1.1")   { a = 13335; return true; }
            return false;                      // unknown / private
        });
        std::string err, why;
        ck(bl.add("AS15169", "probe: a network", 0, err), true, "ban a network");
        ck(bl.banned("8.8.8.8", &why), true, "an address in that network is banned");
        ck(why == "probe: a network", true, "with the reason");
        ck(bl.banned("1.1.1.1"), false, "a DIFFERENT network is not");
        // ★★★ An address the resolver cannot place must NOT be banned. Blocking everyone we
        //     cannot identify would turn a missing dataset into a closed receiver.
        ck(bl.banned("192.168.1.5"), false, "an unresolvable address is NOT banned");
        ck(bl.remove("AS15169"), true, "unban the network");
        ck(bl.banned("8.8.8.8"), false, "and it is allowed again");

        // ★ With no resolver at all (a phone, or no data yet) an ASN rule is inert, not fatal.
        BanList noRes; noRes.setPath(p2);
        ck(noRes.add("AS15169", "x", 0, err), true, "ban stored without a resolver");
        ck(noRes.banned("8.8.8.8"), false, "and matches nobody when we cannot resolve");
        remove(p2);
    }

    printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
