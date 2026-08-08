// ★★★ AN ADMIN TICKET IS A KEY TO THE HARDWARE. It unlocks bias-T (DC on somebody's feedline),
//     direct sampling, calibration, and the power to kick a listener off a radio. So this file
//     spends most of its length on what must be REFUSED, not on the happy path — a bug here is
//     not a broken feature, it is a stranger with the owner's controls.
#include "vibe_admin_ticket.h"

#include <cstdio>
#include <string>

using namespace vibeadmin;

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what, const std::string& extra = "") {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what); return; }
    failures++;
    std::printf("   FAIL %s %s\n", what, extra.c_str());
}

/** A stand-in MAC. NOT cryptography — the real HMAC-SHA256 is the shim's, already exercised by the
 *  PIN handshake. What must be tested here is the format, the expiry arithmetic and the tamper
 *  checks, and those behave identically over any deterministic MAC that depends on key and
 *  message. Deliberately keyed so a wrong secret produces a different tag. */
static void fakeMac(const void* key, size_t klen, const void* msg, size_t mlen, uint8_t out[32]) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t* k = (const uint8_t*)key;
    const uint8_t* m = (const uint8_t*)msg;
    for (size_t i = 0; i < klen; ++i) { h ^= k[i]; h *= 1099511628211ULL; }
    h ^= 0x5c5c5c5cULL; h *= 1099511628211ULL;
    for (size_t i = 0; i < mlen; ++i) { h ^= m[i]; h *= 1099511628211ULL; }
    for (int i = 0; i < 32; ++i) { out[i] = (uint8_t)(h >> ((i % 8) * 8)); h = h * 6364136223846793005ULL + 1; }
}

int main() {
    const std::string secret = "hunter2-but-longer";
    const int64_t now = 1'754'600'000;   // a fixed "now" — no clock reads in a test

    std::printf("\nA ticket the owner just minted\n");
    {
        const std::string t = mintTicket(secret, now + 600, fakeMac);
        ok(!t.empty(), "it mints");
        ok(t.find('.') != std::string::npos, "it has the expiry.signature shape", t);
        ok(verifyTicket(secret, t, now, fakeMac), "★ and it verifies");
        ok(verifyTicket(secret, t, now + 599, fakeMac), "still valid a second before expiry");
    }

    std::printf("\nA ticket that must NOT be honoured\n");
    {
        const std::string t = mintTicket(secret, now + 600, fakeMac);
        ok(!verifyTicket(secret, t, now + 601 + kTicketSkewSec, fakeMac),
           "★ expired — the whole point of a lease");
        ok(!verifyTicket("a different password", t, now, fakeMac),
           "★ a ticket from a server with another admin password");
        ok(!verifyTicket("", t, now, fakeMac),
           "★ a server with NO admin password never has an admin");
        ok(mintTicket("", now + 600, fakeMac).empty(),
           "★ and never mints one either");

        // ★★ Tamper with each half independently: extending the life must invalidate the
        //    signature, because the expiry is what the signature is OVER.
        const size_t dot = t.find('.');
        const std::string longer = std::to_string(now + 999999) + t.substr(dot);
        ok(!verifyTicket(secret, longer, now, fakeMac),
           "★ giving yourself a longer expiry breaks the signature");
        std::string flipped = t;
        flipped[flipped.size() - 1] = (flipped.back() == 'a' ? 'b' : 'a');
        ok(!verifyTicket(secret, flipped, now, fakeMac), "★ a single flipped signature byte");

        ok(!verifyTicket(secret, mintTicket(secret, now + 86400, fakeMac), now, fakeMac),
           "★ a year-long lease is refused even though it is correctly signed");
    }

    std::printf("\nRubbish off the wire\n");
    {
        ok(!verifyTicket(secret, "", now, fakeMac), "empty");
        ok(!verifyTicket(secret, ".", now, fakeMac), "just a dot");
        ok(!verifyTicket(secret, "abc", now, fakeMac), "no dot at all");
        ok(!verifyTicket(secret, ".deadbeef", now, fakeMac), "no expiry");
        ok(!verifyTicket(secret, std::to_string(now + 60) + ".short", now, fakeMac),
           "signature of the wrong length");
        ok(!verifyTicket(secret, " " + std::to_string(now + 60) + ".x", now, fakeMac),
           "★ leading space — the parser must not be more generous than the minter");
        ok(!verifyTicket(secret, "0x10." + std::string(64, 'a'), now, fakeMac),
           "★ nor accept hex or a sign where an epoch belongs");
        ok(!verifyTicket(secret, "+" + std::to_string(now + 60) + "." + std::string(64, 'a'),
                         now, fakeMac), "★ nor a leading plus");
        ok(!verifyTicket(secret, std::to_string(now + 60) + "." + std::string(64, 'Z'),
                         now, fakeMac), "non-hex in the signature");
        ok(!verifyTicket(secret, std::string(200, '9') + "." + std::string(64, 'a'), now, fakeMac),
           "★ an absurdly long ticket is refused, not parsed");
        ok(!verifyTicket(secret, std::string(64, 'a'), now, fakeMac), "signature with no expiry");
    }

    std::printf("\nOne ticket, every radio — the reason this exists\n");
    {
        // The front door mints it; a radio process, holding only the same secret and NO shared
        // state, must accept it. That is the entire point.
        const std::string fromFrontDoor = mintTicket(secret, now + kTicketTtlSec, fakeMac);
        ok(verifyTicket(secret, fromFrontDoor, now + 5, fakeMac),
           "★ a radio accepts what the front door minted, with nothing shared but the secret");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
