// ★★★ WHOSE ADDRESS IS IT? Behind a reverse proxy the TCP peer is the PROXY, so without this
//     every listener shares one identity: a ban hits everyone, every flag shows the proxy's
//     country, and one bad admin password locks out the world (Saber, 2026-08-08, running nginx:
//     "the logs spammed to me 127.0.0.1 connected and not an ip").
//
// ★★★ THE HEADER IS ATTACKER-CONTROLLED. X-Forwarded-For is just text a client can type, so
//     honouring it from ANY peer would let a stranger forge an address and walk straight through
//     the ban list — strictly worse than not having the feature. Trust is opt-in, per address,
//     and this file exists mainly to prove that the untrusted cases are REFUSED.
#include "vibe_proxy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace vibeproxy;

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}
static void eq(const std::string& got, const std::string& want, const char* what) {
    ok(got == want, what, "got \"" + got + "\" want \"" + want + "\"");
}

int main() {
    std::printf("\nRefusing to believe an untrusted peer\n");
    {
        TrustedProxies tp;                       // nothing trusted — the default
        eq(clientAddress(tp, "203.0.113.9", "198.51.100.1", ""), "203.0.113.9",
           "★ with no trusted proxies the header is IGNORED");

        tp.set({"127.0.0.1"});
        eq(clientAddress(tp, "203.0.113.9", "198.51.100.1", ""), "203.0.113.9",
           "★ a stranger forging X-Forwarded-For does NOT get to choose their address");
        eq(clientAddress(tp, "203.0.113.9", "10.0.0.1, 8.8.8.8", ""), "203.0.113.9",
           "★ nor by stuffing a whole chain into it");
    }

    std::printf("\nBelieving a proxy the owner named\n");
    {
        TrustedProxies tp;
        tp.set({"127.0.0.1"});
        eq(clientAddress(tp, "127.0.0.1", "198.51.100.1", ""), "198.51.100.1",
           "★ nginx on loopback: the real client is used");
        eq(clientAddress(tp, "127.0.0.1", "", "198.51.100.7"), "198.51.100.7",
           "X-Real-IP is honoured when there is no X-Forwarded-For");
        eq(clientAddress(tp, "127.0.0.1", "198.51.100.1", "10.9.9.9"), "198.51.100.1",
           "X-Forwarded-For wins over X-Real-IP");
        eq(clientAddress(tp, "127.0.0.1", "  198.51.100.1  ", ""), "198.51.100.1",
           "surrounding whitespace is trimmed");
    }

    std::printf("\nA chain of proxies\n");
    {
        TrustedProxies tp;
        tp.set({"127.0.0.1", "10.0.0.0/8"});
        // client -> cloudflare(203.0.113.5) -> nginx(10.0.0.2) -> us. XFF = "client, cf, nginx-ish"
        // ★★ TAKE THE RIGHTMOST ENTRY THAT IS NOT ITSELF TRUSTED. Taking the LEFTMOST (the usual
        //    first instinct) takes whatever the CLIENT put there — it prepends freely, so the
        //    leftmost is forgeable even through a trusted proxy. Walking right-to-left past our
        //    own infrastructure lands on the first address our infrastructure actually observed.
        eq(clientAddress(tp, "10.0.0.2", "203.0.113.5, 10.0.0.9, 10.0.0.3", ""), "203.0.113.5",
           "★ the rightmost UNTRUSTED hop is the client");
        eq(clientAddress(tp, "10.0.0.2", "1.2.3.4, 203.0.113.5", ""), "203.0.113.5",
           "★ a forged entry prepended by the client is skipped over");
        // Every hop trusted and nothing else: nothing observable is left, so fall back to the peer
        // rather than inventing one.
        eq(clientAddress(tp, "10.0.0.2", "10.0.0.7, 10.0.0.8", ""), "10.0.0.2",
           "an all-trusted chain falls back to the peer");
    }

    std::printf("\nRefusing rubbish in the header\n");
    {
        TrustedProxies tp;
        tp.set({"127.0.0.1"});
        eq(clientAddress(tp, "127.0.0.1", "not-an-ip", ""), "127.0.0.1",
           "★ a non-address is refused, not stored");
        eq(clientAddress(tp, "127.0.0.1", "999.1.1.1", ""), "127.0.0.1",
           "★ an out-of-range octet is refused");
        eq(clientAddress(tp, "127.0.0.1", "", ""), "127.0.0.1",
           "no header at all -> the peer");
        eq(clientAddress(tp, "127.0.0.1", ",,, ,", ""), "127.0.0.1",
           "a header of separators -> the peer");
        // ★ Length cap: the header is unbounded input and lands in logs and a ban list.
        eq(clientAddress(tp, "127.0.0.1", std::string(9000, 'x'), ""), "127.0.0.1",
           "an absurdly long header is refused");
    }

    std::printf("\nCIDR trust\n");
    {
        TrustedProxies tp;
        tp.set({"192.168.1.0/24"});
        eq(clientAddress(tp, "192.168.1.50", "198.51.100.1", ""), "198.51.100.1",
           "a proxy inside a trusted CIDR is believed");
        eq(clientAddress(tp, "192.168.2.50", "198.51.100.1", ""), "192.168.2.50",
           "★ one outside it is not");
        TrustedProxies bad;
        bad.set({"not a cidr", "192.168.1.0/24"});
        eq(clientAddress(bad, "192.168.1.50", "198.51.100.1", ""), "198.51.100.1",
           "★ a malformed entry is dropped without disabling the valid ones");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
