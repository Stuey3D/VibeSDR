// vibe_admin_ticket.h — an admin credential that works across PROCESSES.
//
// ★★★ WHY THE EXISTING ONE CANNOT. VsAuth::verify() only accepts a nonce it finds in its OWN
//     `issued` map. With several radios there is a process per radio plus the front door, each
//     with its own map — so a credential minted at the landing page is rejected by every radio by
//     construction. That is why entering the admin password on the landing page could never work,
//     however many times you retried it (Stuart, 2026-08-08: "Server refused the connection").
//
// ★★★ A TICKET CARRIES ITS OWN PROOF. It is `2.<origin>.<expiry>.<HMAC(adminSecret,
//     "vsadmin2|origin|expiry")>` (the v1 form was `<expiry>.<HMAC(…, "vsadmin1|"+expiry)>`, still
//     honoured until its own expiry — see verifyTicket), so any process holding the same admin secret can check it with NO shared state and no
//     coordination. The secret never leaves the server and never crosses the wire.
//
// ★★ IT IS A BEARER TOKEN, so it is deliberately SHORT-LIVED. Whoever holds it is admin until it
//    expires; ten minutes bounds that, and changing the admin password invalidates every ticket
//    ever issued. The alternative — keeping the owner's PASSWORD in the browser so each radio can
//    be signed for separately — is strictly worse: it does not expire, and it hands over the
//    credential itself rather than a lease on it.
// ★★ AND A RENEWAL DOES NOT RESET THE CLOCK (audit 2026-10-03). A ticket renews itself, so ten
//    minutes bounded nothing on its own; the origin inside it ends every chain of renewals
//    kTicketMaxLifeSec after the password was last typed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vibeadmin {

/** HMAC-SHA256. Injected rather than included so this header stays testable on its own — the real
 *  one lives in the shim beside the PIN handshake that already uses it. */
using MacFn = void (*)(const void* key, size_t klen, const void* msg, size_t mlen, uint8_t out[32]);

/** Default lease. Long enough to walk from the landing page into a radio and take a slot; short
 *  enough that a ticket left in a closed laptop is worthless by the time anyone finds it. */
inline constexpr int kTicketTtlSec = 600;

/** ★ Clocks are not identical across a reboot or an NTP step, and every process here reads the
 *  same wall clock — but a ticket minted a second before a step would fail for no reason the
 *  owner could understand. A small tolerance, not a large one. */
inline constexpr int kTicketSkewSec = 30;

namespace detail {

inline std::string toHexBytes(const uint8_t* p, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += k[p[i] >> 4]; out += k[p[i] & 15]; }
    return out;
}

/** ★★ CONSTANT TIME. A byte-at-a-time comparison that returns early leaks how much of the MAC an
 *  attacker guessed correctly, which turns forgery into 32 cheap searches instead of one
 *  impossible one. */
inline bool ctEqualStr(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

inline std::string macFor(const std::string& secret, int64_t expiry, MacFn mac) {
    // ★ The version prefix is INSIDE the signed text. Without it, a future ticket format could be
    //   accepted by an old server that reads it as something else entirely.
    const std::string msg = "vsadmin1|" + std::to_string(expiry);
    uint8_t out[32];
    mac(secret.data(), secret.size(), msg.data(), msg.size(), out);
    return toHexBytes(out, 32);
}

/** The version-2 MAC: over the ORIGIN as well as the expiry, under its own prefix, so a v1
 *  signature can never be replayed as a v2 one (or the reverse) — the signed texts differ. */
inline std::string macForV2(const std::string& secret, int64_t origin, int64_t expiry, MacFn mac) {
    const std::string msg = "vsadmin2|" + std::to_string(origin) + "|" + std::to_string(expiry);
    uint8_t out[32];
    mac(secret.data(), secret.size(), msg.data(), msg.size(), out);
    return toHexBytes(out, 32);
}

/** ★ Digits only, 1..19 of them. strtoll would happily accept " +12", "0x10" and leading
 *  whitespace, and a parser that is more generous than the minter is where forgeries start. */
inline bool parseEpoch(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 19) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    out = (int64_t)strtoll(s.c_str(), nullptr, 10);
    return true;
}

inline bool isLowerHex64(const std::string& sig) {
    if (sig.size() != 64) return false;
    for (char c : sig)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

}  // namespace detail

/** ★★★ THE ABSOLUTE LIFETIME OF AN ADMIN SESSION, counted from the PASSWORD (audit 2026-10-03).
 *  The renewal route accepts a valid TICKET as admin proof and mints a fresh one — that is what
 *  keeps an owner's tab admin across radios without re-typing the password. But with only an
 *  expiry inside, a renewal could not tell a ticket minted from the password a minute ago from one
 *  that had been renewing itself for a month: ONE leaked ticket, renewed every few minutes, was
 *  admin FOR EVER. Now the time of the original password proof — the ORIGIN — is signed into every
 *  ticket and carried across renewals unchanged, and nothing is honoured or renewed past this long
 *  after it. The ten-minute lease bounds one ticket; this bounds the CHAIN. Twelve hours is a long
 *  day at the radio; past it the owner types the password once more. */
inline constexpr int kTicketMaxLifeSec = 12 * 3600;

/** What a valid ticket says about itself. `origin` is when the owner last proved the PASSWORD —
 *  the start of the chain of renewals this ticket belongs to. */
struct TicketInfo {
    int     version = 0;
    int64_t origin  = 0;
    int64_t expiry  = 0;
};

/**
 * Mint a version-2 ticket: `2.<origin>.<expiry>.<HMAC(secret, "vsadmin2|origin|expiry")>`.
 * Every field is digits, dots or lowercase hex, so it is URL-safe as it stands (Jr checks that),
 * and it is under 100 characters.
 *
 * ★ `originEpochSec` is NOW for a password proof, and the presented ticket's own origin for a
 *   renewal — never now. `expiryEpochSec` is clamped to origin + kTicketMaxLifeSec, so a renewal
 *   near the end of the chain gets only what is left, never a fresh ten minutes past the line.
 * ★ Empty for an empty secret: a server with no admin password has no admin to be.
 */
inline std::string mintTicket(const std::string& secret, int64_t originEpochSec,
                              int64_t expiryEpochSec, MacFn mac) {
    if (secret.empty() || !mac || originEpochSec <= 0) return "";
    if (expiryEpochSec > originEpochSec + kTicketMaxLifeSec)
        expiryEpochSec = originEpochSec + kTicketMaxLifeSec;
    return "2." + std::to_string(originEpochSec) + "." + std::to_string(expiryEpochSec) + "."
         + detail::macForV2(secret, originEpochSec, expiryEpochSec, mac);
}

/** ★ THE OLD FORMAT, `<expiry>.<HMAC(secret, "vsadmin1|"+expiry)>`. Nothing in the server mints it
 *  any more — it is kept so the tests can prove that a ticket already in flight when the server
 *  updated is still honoured until its own ten-minute expiry, and no longer. */
inline std::string mintLegacyTicket(const std::string& secret, int64_t expiryEpochSec, MacFn mac) {
    if (secret.empty() || !mac) return "";
    return std::to_string(expiryEpochSec) + "." + detail::macFor(secret, expiryEpochSec, mac);
}

/**
 * Check a ticket. Returns false for anything that is not currently valid; on success fills `info`
 * when one is given.
 *
 * ★ `maxTtlSec` bounds how far ahead the expiry may sit. Forging one needs the secret, so this is
 *   not what stops an attacker — it stops a ticket minted by some future or misconfigured version
 *   with a year-long lease from being honoured here.
 * ★★ A version-2 ticket is ALSO refused once kTicketMaxLifeSec has passed since its origin, or when
 *    its origin lies in the future — whatever its own expiry says.
 * ★ A version-1 ticket (no origin) is accepted until its own expiry, so a tab that was admin when
 *   the server updated stays admin. Its origin is taken as expiry − kTicketTtlSec — when it was
 *   minted — so renewing it starts a chain that ends twelve hours after THAT, not after now: an old
 *   ticket cannot be laundered into a fresh twelve hours.
 */
inline bool verifyTicket(const std::string& secret, const std::string& ticket, int64_t nowSec,
                         MacFn mac, TicketInfo* info = nullptr, int maxTtlSec = kTicketTtlSec) {
    if (secret.empty() || ticket.empty() || !mac) return false;
    if (ticket.size() > 128) return false;                  // bounded input, it comes off the wire

    TicketInfo ti;
    std::string sig, expected;
    if (ticket.size() > 2 && ticket[0] == '2' && ticket[1] == '.') {
        // ── version 2: 2.<origin>.<expiry>.<sig>  (a v1 expiry is ten digits, never a lone "2")
        const size_t d2 = ticket.find('.', 2);
        if (d2 == std::string::npos) return false;
        const size_t d3 = ticket.find('.', d2 + 1);
        if (d3 == std::string::npos) return false;
        if (!detail::parseEpoch(ticket.substr(2, d2 - 2), ti.origin)) return false;
        if (!detail::parseEpoch(ticket.substr(d2 + 1, d3 - d2 - 1), ti.expiry)) return false;
        sig = ticket.substr(d3 + 1);
        if (!detail::isLowerHex64(sig)) return false;       // also refuses a fourth field
        ti.version = 2;
        if (ti.origin <= 0) return false;
        if (ti.origin > nowSec + kTicketSkewSec) return false;               // from the future
        if (nowSec - ti.origin > (int64_t)kTicketMaxLifeSec) return false;   // ★ the chain is over
        if (ti.expiry > ti.origin + kTicketMaxLifeSec) return false;         // the minter clamps this
        expected = detail::macForV2(secret, ti.origin, ti.expiry, mac);
    } else {
        // ── version 1 (legacy): <expiry>.<sig>
        const size_t dot = ticket.find('.');
        if (dot == std::string::npos || dot == 0) return false;
        if (!detail::parseEpoch(ticket.substr(0, dot), ti.expiry)) return false;
        sig = ticket.substr(dot + 1);
        if (!detail::isLowerHex64(sig)) return false;
        ti.version = 1;
        ti.origin  = ti.expiry - kTicketTtlSec;
        expected = detail::macFor(secret, ti.expiry, mac);
    }

    if (nowSec > ti.expiry + kTicketSkewSec) return false;                       // expired
    if (ti.expiry - nowSec > (int64_t)maxTtlSec + kTicketSkewSec) return false;  // absurdly far ahead

    if (!detail::ctEqualStr(expected, sig)) return false;
    if (info) *info = ti;
    return true;
}

}  // namespace vibeadmin
