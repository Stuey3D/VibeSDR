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
        const std::string t = mintTicket(secret, now, now + 600, fakeMac);
        ok(!t.empty(), "it mints");
        ok(t.find('.') != std::string::npos, "it has the expiry.signature shape", t);
        ok(verifyTicket(secret, t, now, fakeMac), "★ and it verifies");
        ok(verifyTicket(secret, t, now + 599, fakeMac), "still valid a second before expiry");
    }

    std::printf("\nA ticket that must NOT be honoured\n");
    {
        const std::string t = mintTicket(secret, now, now + 600, fakeMac);
        ok(!verifyTicket(secret, t, now + 601 + kTicketSkewSec, fakeMac),
           "★ expired — the whole point of a lease");
        ok(!verifyTicket("a different password", t, now, fakeMac),
           "★ a ticket from a server with another admin password");
        ok(!verifyTicket("", t, now, fakeMac),
           "★ a server with NO admin password never has an admin");
        ok(mintTicket("", now, now + 600, fakeMac).empty(),
           "★ and never mints one either");

        // ★★ Tamper with each field independently: extending the life must invalidate the
        //    signature, because the origin and expiry are what the signature is OVER.
        ok(t.rfind("2.", 0) == 0, "it is a version-2 ticket", t);
        const size_t d2 = t.find('.', 2), d3 = t.find('.', d2 + 1);
        const std::string sigPart = t.substr(d3);
        const std::string longer = "2." + std::to_string(now) + "." + std::to_string(now + 300 + 600) + sigPart;
        ok(!verifyTicket(secret, longer, now, fakeMac),
           "★ giving yourself a longer expiry breaks the signature");
        const std::string younger = "2." + std::to_string(now + 3600) + "." + std::to_string(now + 600) + sigPart;
        ok(!verifyTicket(secret, younger, now + 3600, fakeMac),
           "★ giving yourself a LATER origin (more chain left) breaks the signature");
        std::string flipped = t;
        flipped[flipped.size() - 1] = (flipped.back() == 'a' ? 'b' : 'a');
        ok(!verifyTicket(secret, flipped, now, fakeMac), "★ a single flipped signature byte");

        ok(!verifyTicket(secret, mintLegacyTicket(secret, now + 86400, fakeMac), now, fakeMac),
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
        // ★ The version-2 shape gets the same suspicion, field by field.
        const std::string s64(64, 'a');
        ok(!verifyTicket(secret, "2." + std::to_string(now) + "." + s64, now, fakeMac),
           "v2 with only two fields");
        ok(!verifyTicket(secret, "2.." + std::to_string(now + 60) + "." + s64, now, fakeMac),
           "v2 with an empty origin");
        ok(!verifyTicket(secret, "2.+" + std::to_string(now) + "." + std::to_string(now + 60) + "." + s64,
                         now, fakeMac), "★ v2 with a signed origin");
        ok(!verifyTicket(secret, "2." + std::to_string(now) + "." + std::to_string(now + 60) + ".x." + s64,
                         now, fakeMac), "v2 with a fifth field");
        ok(!verifyTicket(secret, "2.0." + std::to_string(now + 60) + "." + s64, now, fakeMac),
           "v2 with a zero origin");
    }

    std::printf("\nThe chain of renewals has an END (audit 2026-10-03)\n");
    {
        // ★★★ The renewal route mints from a ticket, keeping its ORIGIN. Simulate an owner (or
        //     a thief) renewing every five minutes from one password proof and check that the
        //     chain dies kTicketMaxLifeSec after the password, however diligently it is renewed.
        const int64_t pw = now;                                   // the password was typed here
        std::string t = mintTicket(secret, pw, pw + kTicketTtlSec, fakeMac);
        int64_t clock = pw;
        int renewed = 0;
        bool died = false;
        for (int i = 0; i < 1000; ++i) {
            clock += 300;
            TicketInfo ti;
            if (!verifyTicket(secret, t, clock, fakeMac, &ti)) { died = true; break; }
            if (ti.origin != pw) { ok(false, "★ a renewal moved the origin"); break; }
            t = mintTicket(secret, ti.origin, clock + kTicketTtlSec, fakeMac);
            if (t.empty()) break;
            ++renewed;
        }
        ok(died, "★★ a ticket renewed every five minutes still dies");
        ok(clock - pw <= (int64_t)kTicketMaxLifeSec + 300,
           "★★ and it dies no later than the absolute lifetime", std::to_string(clock - pw));
        ok(clock - pw > (int64_t)kTicketMaxLifeSec - 600,
           "★ but not early — the owner gets the whole twelve hours", std::to_string(clock - pw));
        ok(renewed > 100, "it really was renewed, many times", std::to_string(renewed));

        // A renewal near the end is CLAMPED, not handed a fresh ten minutes past the line.
        const int64_t late = pw + kTicketMaxLifeSec - 60;
        const std::string near = mintTicket(secret, pw, late + kTicketTtlSec, fakeMac);
        TicketInfo ni;
        ok(verifyTicket(secret, near, late, fakeMac, &ni), "a renewal a minute before the end verifies");
        ok(ni.expiry == pw + kTicketMaxLifeSec, "★ but its expiry is clamped to the end of the chain",
           std::to_string(ni.expiry - pw));
        ok(!verifyTicket(secret, near, pw + kTicketMaxLifeSec + 1, fakeMac),
           "★ and it is refused a second past the end, inside its own skew");

        // An origin in the future is nonsense — refused, even correctly signed.
        ok(!verifyTicket(secret, mintTicket(secret, now + 3600, now + 3600 + 600, fakeMac), now, fakeMac),
           "★ an origin in the future is refused");
        // A correctly signed ticket whose expiry sits past origin + max life (only a broken minter
        // could make one) is refused too.
        {
            const int64_t o = now - 100, e = o + kTicketMaxLifeSec + 50;
            uint8_t mac[32];
            const std::string msg = "vsadmin2|" + std::to_string(o) + "|" + std::to_string(e);
            fakeMac(secret.data(), secret.size(), msg.data(), msg.size(), mac);
            static const char* hx = "0123456789abcdef";
            std::string sig; for (int i = 0; i < 32; ++i) { sig += hx[mac[i] >> 4]; sig += hx[mac[i] & 15]; }
            ok(!verifyTicket(secret, "2." + std::to_string(o) + "." + std::to_string(e) + "." + sig,
                             now, fakeMac, nullptr, kTicketMaxLifeSec * 2),
               "★ an expiry past the end of its own chain is refused");
        }
    }

    std::printf("\nA ticket from BEFORE the update (version 1)\n");
    {
        // ★ A tab that was admin when the server updated holds a v1 ticket. It must keep working
        //   until its own short expiry — and not a second longer, and not be renewable into a
        //   fresh twelve hours.
        const std::string v1 = mintLegacyTicket(secret, now + kTicketTtlSec, fakeMac);
        TicketInfo ti;
        ok(verifyTicket(secret, v1, now + 10, fakeMac, &ti), "★ an in-flight v1 ticket is still honoured");
        ok(ti.version == 1, "it is recognised as v1");
        ok(ti.origin == now, "★ its origin is when it was minted (expiry − ttl)",
           std::to_string(ti.origin - now));
        ok(!verifyTicket(secret, v1, now + kTicketTtlSec + kTicketSkewSec + 1, fakeMac),
           "★ and refused past its own expiry");
        const std::string renewed = mintTicket(secret, ti.origin, now + 10 + kTicketTtlSec, fakeMac);
        ok(!verifyTicket(secret, renewed, now + kTicketMaxLifeSec + 1, fakeMac),
           "★ a chain started from a v1 ticket ends twelve hours after IT was minted");
        // A v1 signature is over a different text, so it cannot be re-dressed as v2.
        const std::string v1sig = v1.substr(v1.find('.'));
        ok(!verifyTicket(secret, "2." + std::to_string(now) + "." + std::to_string(now + kTicketTtlSec) + v1sig,
                         now, fakeMac), "★ a v1 signature re-dressed as v2 is refused");
    }

    std::printf("\nThe wire shape the apps rely on\n");
    {
        // ★ Jr validates a ticket as URL-safe and at most 512 characters before pasting it into
        //   a query; the web client stores it opaquely. Prove the new format passes both.
        const std::string t = mintTicket(secret, now, now + kTicketTtlSec, fakeMac);
        bool safe = true;
        for (char c : t) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.')) safe = false;
        ok(safe, "★ digits, dots and lowercase hex only", t);
        ok(t.size() <= 128, "★ short — well inside Jr's 512 and the server's own 128 cap",
           std::to_string(t.size()));
    }

    std::printf("\nOne ticket, every radio — the reason this exists\n");
    {
        // The front door mints it; a radio process, holding only the same secret and NO shared
        // state, must accept it. That is the entire point.
        const std::string fromFrontDoor = mintTicket(secret, now, now + kTicketTtlSec, fakeMac);
        ok(verifyTicket(secret, fromFrontDoor, now + 5, fakeMac),
           "★ a radio accepts what the front door minted, with nothing shared but the secret");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
