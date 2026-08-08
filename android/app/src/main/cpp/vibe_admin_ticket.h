// vibe_admin_ticket.h — an admin credential that works across PROCESSES.
//
// ★★★ WHY THE EXISTING ONE CANNOT. VsAuth::verify() only accepts a nonce it finds in its OWN
//     `issued` map. With several radios there is a process per radio plus the front door, each
//     with its own map — so a credential minted at the landing page is rejected by every radio by
//     construction. That is why entering the admin password on the landing page could never work,
//     however many times you retried it (Stuart, 2026-08-08: "Server refused the connection").
//
// ★★★ A TICKET CARRIES ITS OWN PROOF. It is `<expiry>.<HMAC(adminSecret, "vsadmin1|"+expiry)>`,
//     so any process holding the same admin secret can check it with NO shared state and no
//     coordination. The secret never leaves the server and never crosses the wire.
//
// ★★ IT IS A BEARER TOKEN, so it is deliberately SHORT-LIVED. Whoever holds it is admin until it
//    expires; ten minutes bounds that, and changing the admin password invalidates every ticket
//    ever issued. The alternative — keeping the owner's PASSWORD in the browser so each radio can
//    be signed for separately — is strictly worse: it does not expire, and it hands over the
//    credential itself rather than a lease on it.
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

}  // namespace detail

/** Mint a ticket valid until `expiryEpochSec`. Empty for an empty secret — a server with no admin
 *  password has no admin to be. */
inline std::string mintTicket(const std::string& secret, int64_t expiryEpochSec, MacFn mac) {
    if (secret.empty() || !mac) return "";
    return std::to_string(expiryEpochSec) + "." + detail::macFor(secret, expiryEpochSec, mac);
}

/**
 * Check a ticket. Returns false for anything that is not currently valid.
 *
 * ★ `maxTtlSec` bounds how far ahead the expiry may sit. Forging one needs the secret, so this is
 *   not what stops an attacker — it stops a ticket minted by some future or misconfigured version
 *   with a year-long lease from being honoured here.
 */
inline bool verifyTicket(const std::string& secret, const std::string& ticket, int64_t nowSec,
                         MacFn mac, int maxTtlSec = kTicketTtlSec) {
    if (secret.empty() || ticket.empty() || !mac) return false;
    if (ticket.size() > 128) return false;                  // bounded input, it comes off the wire

    const size_t dot = ticket.find('.');
    if (dot == std::string::npos || dot == 0) return false;

    // ★ Digits only. strtoll would happily accept " +12", "0x10" and leading whitespace, and a
    //   parser that is more generous than the minter is where forgeries start.
    for (size_t i = 0; i < dot; ++i) if (ticket[i] < '0' || ticket[i] > '9') return false;
    if (dot > 19) return false;                             // cannot be a plausible epoch

    const std::string sig = ticket.substr(dot + 1);
    if (sig.size() != 64) return false;
    for (char c : sig)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;

    const int64_t expiry = (int64_t)strtoll(ticket.substr(0, dot).c_str(), nullptr, 10);
    if (nowSec > expiry + kTicketSkewSec) return false;                    // expired
    if (expiry - nowSec > (int64_t)maxTtlSec + kTicketSkewSec) return false;  // absurdly far ahead

    return detail::ctEqualStr(detail::macFor(secret, expiry, mac), sig);
}

}  // namespace vibeadmin
