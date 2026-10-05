// VibeServer ADMIN state — the ban list, the connection log, and the machine's own vital signs.
//
// ★★★ WHY THIS IS A SEPARATE FILE. local_sdr_shim.cpp is the radio. None of what is below is
// about radio: a ban list is policy, a connection log is history, and /proc/loadavg is the
// weather. Keeping them here means the shim's admin endpoints are ~10 lines of routing each, and
// this file can be reasoned about (and later tested) without a dongle plugged in.
//
// ★★ WHAT THIS DELIBERATELY DOES NOT DO — VPN / proxy / "bot" DETECTION. Doing it honestly needs
// a paid IP-reputation feed, and it means posting every visitor's address to a third party.
// UberSDR, which is far further down this road than we are, does not do it either: it ships free
// local MaxMind GeoLite2 databases and lets the OWNER look up an abuser's ASN and ban the ASN.
// That is the model to follow when we get there. Until then, what is offered here is what we can
// actually stand behind: an address, a range, a rate, and a human's judgement.
//
// ★ Everything here is guarded by ONE mutex per subsystem and copies data out. An admin page
// polling every second must never be able to stall the DSP thread.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <arpa/inet.h>   // inet_pton — cloudflareWorkerAddr
#include <netinet/in.h>
#include "vibe_hwinfo.h"   // ★ presentCpus — the hardware core count
// ★ sysconf(_SC_CLK_TCK) and clock_gettime for the per-process CPU fallback — named rather than
//   relied on transitively, the same lesson <cmath> taught in vibe_bands.h this afternoon.
#include <unistd.h>
#include <deque>
#include <set>
#include <mutex>
#include <string>
#include <functional>
#include <thread>
#include <vector>
#include "vibe_vcio.h"   // ★ the Pi's firmware clock and throttle flags — see readSys()

#if defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_host.h>
  #include <sys/sysctl.h>
#endif

namespace vibeadmin {

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  Small helpers
// ─────────────────────────────────────────────────────────────────────────────────────────────

/** JSON string escaping. Local copy so this header stands alone. */
/**
 * ★★★ A TEXT FRAME THAT IS NOT VALID UTF-8 KILLS THE SOCKET — the browser does it, not us.
 *
 *  RFC 6455 §8.1 requires a client to FAIL THE WEBSOCKET CONNECTION on a text frame whose payload
 *  is not well-formed UTF-8. So one bad byte anywhere in a JSON message takes down the whole
 *  spectrum/control socket, and everything that rides on it — the waterfall AND tuning — while the
 *  AUDIO socket, being a different socket, plays on perfectly. That is the exact shape of the fault
 *  Stuart chased across two different servers (2026-08-28): "the audio is perfect not missed a beat
 *  but the spectrum is not connected and seems to be receiving the odd frame here or there."
 *
 * ★★★ AND THE BAD BYTE COMES OFF THE AIR. RDS text is in the EBU character set and arrives over a
 *  noisy link: with block errors at 87-93% a corrupted group put 0x81 into the PTYN field, which
 *  went into the JSON verbatim. It is INTERMITTENT and it follows the STATION, not the machine —
 *  which is why it looked like a network fault on one receiver and a tunnel fault on another.
 *  Caught live: PTYN "    \x81.DS" killed every reconnect; minutes later the same corrupted field
 *  decoded as "\MXF" — all ASCII — and the page recovered on its own with no refresh.
 *
 * ★★ SO THE WIRE MUST NOT BE ABLE TO CARRY IT. Not "the RDS decoder should transcode properly" —
 *  it should, and that is worth doing — but a JSON writer that CAN emit an invalid byte is one bit
 *  error away from this every time, from any field, on any radio. One choke point, and the class of
 *  fault is gone: PTYN, PS, RadioText, LongPS, RT+ and the owner's own free text all pass here.
 * ★ DROPPED rather than replaced with U+FFFD: this is corruption, not a character we failed to
 *   render, and a station name should not sprout a black diamond because one group was misread.
 *   The same choice the control-character rule below already makes.
 */
inline std::string utf8Clean(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o.push_back((char)c); ++i; continue; }
        // How many continuation bytes should follow, and the smallest code point that is not an
        // overlong encoding of something shorter.
        int need; unsigned cp; unsigned lo;
        if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; lo = 0x80u; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; lo = 0x800u; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; lo = 0x10000u; }
        else { ++i; continue; }                       // 0x80-0xBF or 0xF8+ : never a lead byte
        if (i + (size_t)need >= s.size()) { ++i; continue; }   // truncated at the end of the string
        bool ok = true;
        for (int k = 1; k <= need; k++) {
            const unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // Overlongs, surrogates and anything past U+10FFFF are ill-formed even when the shape fits.
        if (!ok || cp < lo || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) { ++i; continue; }
        o.append(s, i, (size_t)need + 1);
        i += (size_t)need + 1;
    }
    return o;
}

inline std::string esc(const std::string& s_raw) {
    // ★ Cleaned FIRST — see utf8Clean. Everything below then only has to worry about JSON syntax.
    const std::string s = utf8Clean(s_raw);
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

/** Wall-clock seconds since the epoch. The ban list and the connection log are both things a
 *  human reads TIMES off, so they must not use a monotonic clock — those restart with the
 *  process and would date every ban to 1970. */
inline long long nowEpoch() { return (long long)time(nullptr); }

/** ★★★ IS THIS ADDRESS CLOUDFLARE'S WORKERS NETWORK (2a06:98c0::/29)? — never a person.
 *
 *  Stuart, 2026-09-29: "we get those cloudflare addresses on all logs on all Pi's I would like to
 *  maybe label them … so people dont think its a dodgy user lingering about." The address was
 *  2a06:98c0:3600::103 — the fixed source address of every request a Cloudflare WORKER makes.
 *  ★★ WHICH WORKER: OUR DIRECTORY. `<slug>.vibeserver.vibesdr.net` proxies the landing page and all
 *     of its HTTP (the WebSocket goes direct — origin.ts socketHost). Its fetch to the tunnel is a
 *     CROSS-ZONE subrequest, and for those Cloudflare "unconditionally replaces" X-Forwarded-For /
 *     X-Real-IP "with an internal Cloudflare address to prevent IP spoofing" (Cloudflare docs,
 *     Request Header Modification). So the `x-forwarded-for: <visitor>` the Worker sets never
 *     arrives, and every page opened through the directory address — the landing page's 15 s
 *     spectrogram refresh, the admin page's /vibeserver/radios poll, a listener's own tab — reports
 *     in as this one address. A REAL visitor behind a masked address, not a bot.
 *  ★★ NOT WARP. Cloudflare WARP users egress from 104.28.0.0/16 and 2a09:bac0::/29, which this does
 *     not match, so a real person on WARP keeps their own row. This range is the Workers egress
 *     alone; nothing a person browses from.
 *  ★ Pure: a prefix test on the parsed address, so a textual variant ("2A06:98C0:3600:0::103")
 *    cannot slip past it. */
inline bool cloudflareWorkerAddr(const std::string& ip) {
    if (ip.find(':') == std::string::npos) return false;
    struct in6_addr a {};
    if (inet_pton(AF_INET6, ip.c_str(), &a) != 1) return false;
    const uint8_t* b = a.s6_addr;
    // 2a06:98c0::/29 — the first 16 bits exactly, then the top 13 bits of the next 16.
    return b[0] == 0x2a && b[1] == 0x06 && b[2] == 0x98 && (b[3] & 0xf8) == 0xc0;
}

/** Read a whole small file (sysfs, /proc). Empty on any failure — a missing sysfs node is the
 *  normal case on a Mac or in a container, not an error to report. */
inline std::string slurp(const char* path, size_t max = 4096) {
    FILE* f = fopen(path, "r");
    if (!f) return {};
    std::string s;
    s.resize(max);
    size_t n = fread(&s[0], 1, max, f);
    fclose(f);
    s.resize(n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0')) s.pop_back();
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  ★★★ THE BAN LIST — addresses and CIDR ranges
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★ CIDR, NOT JUST SINGLE ADDRESSES. A single address is nearly useless against the thing an
// owner actually wants to stop: a residential-proxy pool or a cloud range cycling through
// hundreds of addresses. Banning 192.0.2.7 when the next request comes from 192.0.2.8 is a
// treadmill. `192.0.2.0/24` is one decision that holds.
//
// ★★ EVERY BAN CARRIES A REASON AND AN OPTIONAL EXPIRY. The reason is for the owner reading this
// list in six months with no memory of why — an unexplained ban gets removed "just in case", so
// an unexplained ban is a ban that will not survive. The expiry is because most abuse is a bad
// afternoon, not a bad person, and a permanent ban for a temporary problem is how a public
// receiver quietly shrinks its own audience.

struct Ban {
    std::string cidr;       ///< As the owner typed it: "192.0.2.7", "192.0.2.0/24", "2001:db8::/32"
    std::string reason;
    long long   atEpoch = 0;    ///< When it was added
    long long   untilEpoch = 0; ///< 0 = permanent
    // Parsed form, filled by compile(). Kept beside the text so a malformed entry loaded from
    // disk is VISIBLE in the list rather than silently dropped.
    bool     v6 = false;
    bool     valid = false;
    /** ★★ Non-zero = this is an ASN ban ("AS15169"), not an address ban, and `net`/`bits` are
     *  meaningless. One entry type rather than a second list: an owner thinks "block this", not
     *  "which of my two block lists does this belong in", and every path that already loads,
     *  saves, expires and displays a ban gets ASNs for free. */
    uint32_t asn = 0;
    uint8_t  net[16] = {0};
    int      bits = 0;
};

/** Parse "a.b.c.d", "a.b.c.d/n", "xx::yy" or "xx::yy/n" into the Ban's binary fields. */
inline bool compile(Ban& b) {
    b.valid = false;
    b.asn = 0;
    // ★ "AS15169" or "as15169". Checked BEFORE the address parsers, which would otherwise reject
    //   it as rubbish and leave the owner with a ban that can never match.
    if (b.cidr.size() > 2 && (b.cidr[0] == 'A' || b.cidr[0] == 'a')
                          && (b.cidr[1] == 'S' || b.cidr[1] == 's')) {
        const char* d = b.cidr.c_str() + 2;
        if (!*d) return false;
        for (const char* q = d; *q; q++) if (!isdigit((unsigned char)*q)) return false;
        const unsigned long v = strtoul(d, nullptr, 10);
        if (!v || v > 4294967295UL) return false;   // AS0 means "not routed" — never bannable
        b.asn = (uint32_t)v;
        b.valid = true;
        return true;
    }
    std::string addr = b.cidr;
    int bits = -1;
    const size_t slash = addr.find('/');
    if (slash != std::string::npos) {
        bits = atoi(addr.c_str() + slash + 1);
        addr = addr.substr(0, slash);
    }
    // Trim — a pasted address very often arrives with a space on it.
    while (!addr.empty() && isspace((unsigned char)addr.front())) addr.erase(addr.begin());
    while (!addr.empty() && isspace((unsigned char)addr.back())) addr.pop_back();
    if (addr.empty()) return false;

    const bool v6 = addr.find(':') != std::string::npos;
    memset(b.net, 0, sizeof b.net);
    if (v6) {
        // Hand-rolled rather than inet_pton so this header needs no platform headers beyond libc,
        // and so a "::ffff:1.2.3.4" mapped address is handled the way we want (as v6 text).
        uint16_t groups[8] = {0};
        int gi = 0, dblcolon = -1;
        size_t i = 0;
        if (addr.compare(0, 2, "::") == 0) { dblcolon = 0; i = 2; }
        while (i < addr.size() && gi < 8) {
            if (addr[i] == ':') { dblcolon = gi; i++; continue; }
            size_t j = i;
            unsigned v = 0; int digits = 0;
            while (j < addr.size() && isxdigit((unsigned char)addr[j]) && digits < 4) {
                const char c = addr[j];
                v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (tolower(c) - 'a' + 10));
                j++; digits++;
            }
            if (digits == 0) return false;
            groups[gi++] = (uint16_t)v;
            i = j;
            if (i < addr.size() && addr[i] == ':') i++;
        }
        if (i < addr.size()) return false;   // trailing junk
        // Expand the "::" run of zeroes into place.
        if (dblcolon >= 0 && gi < 8) {
            const int move = gi - dblcolon;
            for (int k = 0; k < move; k++) groups[7 - k] = groups[gi - 1 - k];
            for (int k = dblcolon; k < 8 - move; k++) groups[k] = 0;
        } else if (dblcolon < 0 && gi != 8) {
            return false;
        }
        for (int k = 0; k < 8; k++) { b.net[k * 2] = (uint8_t)(groups[k] >> 8); b.net[k * 2 + 1] = (uint8_t)groups[k]; }
        b.v6 = true;
        b.bits = (bits < 0) ? 128 : bits;
        if (b.bits < 0 || b.bits > 128) return false;
    } else {
        unsigned o[4] = {0};
        int n = 0;
        size_t i = 0;
        while (i < addr.size() && n < 4) {
            unsigned v = 0; int digits = 0;
            while (i < addr.size() && isdigit((unsigned char)addr[i]) && digits < 3) {
                v = v * 10 + (unsigned)(addr[i] - '0'); i++; digits++;
            }
            if (digits == 0 || v > 255) return false;
            o[n++] = v;
            if (i < addr.size()) { if (addr[i] != '.') return false; i++; }
        }
        if (n != 4 || i < addr.size()) return false;
        for (int k = 0; k < 4; k++) b.net[k] = (uint8_t)o[k];
        b.v6 = false;
        b.bits = (bits < 0) ? 32 : bits;
        if (b.bits < 0 || b.bits > 32) return false;
    }
    // Normalise: mask the host bits off, so `net` holds the NETWORK its name promises. Typing
    // "192.0.2.7/24" (what a person writes when they mean "the /24 this abuser is in") stores
    // 192.0.2.0/24.
    // ★★ THIS IS NOT WHAT MAKES THAT CASE WORK — matches() only ever compares the first `bits`
    //    bits, so it never looks at the host bits either way. Verified by deleting this loop: the
    //    whole matcher test still passes. It is kept because `net` should mean what it says for
    //    anything that later compares or de-duplicates whole networks, and it costs four
    //    iterations. Do not write a comment here claiming it fixes a matching bug: it does not,
    //    and the next person will spend an afternoon proving that.
    const int bytes = b.v6 ? 16 : 4;
    for (int k = 0; k < bytes; k++) {
        const int lo = k * 8, hi = lo + 8;
        if (b.bits >= hi) continue;
        if (b.bits <= lo) { b.net[k] = 0; continue; }
        b.net[k] &= (uint8_t)(0xFF << (hi - b.bits));
    }
    b.valid = true;
    return true;
}

/** Does `ip` (a plain address, no port, no mask) fall inside this ban? */
inline bool matches(const Ban& b, const std::string& ip) {
    if (!b.valid) return false;
    // ★★ An ASN rule has no address range. Falling through would compare uninitialised `net`
    //    and could match anything — the worst possible failure for a rule that blocks a whole
    //    network. ASN rules are resolved by BanList::banned(), which has the lookup.
    if (b.asn) return false;
    Ban probe;
    probe.cidr = ip;
    if (!compile(probe)) return false;
    if (probe.v6 != b.v6) return false;
    const int full = b.bits / 8, rem = b.bits % 8;
    if (full && memcmp(probe.net, b.net, (size_t)full) != 0) return false;
    if (rem) {
        const uint8_t mask = (uint8_t)(0xFF << (8 - rem));
        if ((probe.net[full] & mask) != (b.net[full] & mask)) return false;
    }
    return true;
}

/** The ban list, persisted as one JSON line per entry.
 *
 *  ★★ LINE-PER-ENTRY, NOT A JSON DOCUMENT. Appending a ban must not require rewriting and
 *  re-parsing the whole file, and a file truncated by a power cut mid-write should cost the
 *  owner the last ban, not all of them. The Pi this runs on gets its power pulled regularly. */
class BanList {
public:
    /** ★★ HOW AN ADDRESS BECOMES A NETWORK. Supplied by the daemon (asndb), because this header
     *  compiles into the phone too and an app has no 43 MB routing table. Unset = ASN rules
     *  simply never match, which is the correct behaviour on a server with no data: refusing
     *  everybody because we cannot tell would be far worse than letting them in. */
    using AsnResolver = std::function<bool(const std::string& ip, uint32_t& asn)>;
    void setAsnResolver(AsnResolver r) { std::lock_guard<std::mutex> lk(mtx_); asn_ = std::move(r); }

    void setPath(std::string p) {
        { std::lock_guard<std::mutex> lk(mtx_); path_ = std::move(p); }
        load();
    }

    /** True if this address is banned right now. Prunes expired entries as it goes.
     *  ★ Called on every connection, so it must be cheap: the common case is an empty list and
     *    this returns after one atomic-ish check under an uncontended lock. */
    bool banned(const std::string& ip, std::string* reasonOut = nullptr) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (bans_.empty()) return false;
        const long long now = nowEpoch();
        bool pruned = false;
        // ★ Resolve the caller's network AT MOST ONCE, and only if an ASN rule actually exists.
        //   A binary search over 574k ranges is cheap but not free, and the overwhelmingly common
        //   case is a ban list with no ASN rules in it at all.
        bool asnDone = false; uint32_t theirAsn = 0;
        auto theirNetwork = [&]() -> uint32_t {
            if (!asnDone) { asnDone = true; if (asn_) { uint32_t a = 0; if (asn_(ip, a)) theirAsn = a; } }
            return theirAsn;
        };
        for (size_t i = 0; i < bans_.size();) {
            if (bans_[i].untilEpoch > 0 && bans_[i].untilEpoch <= now) {
                bans_.erase(bans_.begin() + (long)i); pruned = true; continue;
            }
            if (bans_[i].asn) {
                const uint32_t a = theirNetwork();
                if (a && a == bans_[i].asn) {
                    if (reasonOut) *reasonOut = bans_[i].reason;
                    if (pruned) saveLocked();
                    return true;
                }
                i++;
                continue;
            }
            if (matches(bans_[i], ip)) {
                if (reasonOut) *reasonOut = bans_[i].reason;
                if (pruned) saveLocked();
                return true;
            }
            i++;
        }
        if (pruned) saveLocked();
        return false;
    }

    /** Add or replace a ban. `mins` <= 0 means permanent. Returns false with `err` set when the
     *  address does not parse — which the page must SHOW, because a typo that silently becomes
     *  no ban at all is how an owner ends up thinking the feature is broken. */
    bool add(const std::string& cidr, const std::string& reason, int mins, std::string& err) {
        Ban b;
        b.cidr = cidr;
        b.reason = reason;
        b.atEpoch = nowEpoch();
        b.untilEpoch = mins > 0 ? b.atEpoch + (long long)mins * 60 : 0;
        if (!compile(b)) {
            // ★ Name every form it accepts. The old text said only "IP address or CIDR range",
            //   which reads as "AS numbers are not supported here" to someone who just typed one.
            err = "not an address, a CIDR range, or an AS number: " + cidr;
            return false;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (bans_.size() >= kMax) { err = "ban list is full"; return false; }
        for (auto& e : bans_) {
            if (e.cidr == b.cidr) { e = b; saveLocked(); return true; }
        }
        bans_.push_back(b);
        saveLocked();
        return true;
    }

    bool remove(const std::string& cidr) {
        std::lock_guard<std::mutex> lk(mtx_);
        const size_t before = bans_.size();
        bans_.erase(std::remove_if(bans_.begin(), bans_.end(),
                                   [&](const Ban& b) { return b.cidr == cidr; }),
                    bans_.end());
        if (bans_.size() == before) return false;
        saveLocked();
        return true;
    }

    std::string json() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string j = "[";
        for (size_t i = 0; i < bans_.size(); i++) {
            const Ban& b = bans_[i];
            if (i) j += ',';
            j += "{\"cidr\":\"" + esc(b.cidr) + "\",\"asn\":" + std::to_string(b.asn)
               + ",\"reason\":\"" + esc(b.reason)
               + "\",\"at\":" + std::to_string(b.atEpoch)
               + ",\"until\":" + std::to_string(b.untilEpoch)
               + ",\"valid\":" + (b.valid ? "true" : "false") + "}";
        }
        return j + "]";
    }

private:
    static const size_t kMax = 2000;

    void load() {
        std::lock_guard<std::mutex> lk(mtx_);
        bans_.clear();
        if (path_.empty()) return;
        FILE* f = fopen(path_.c_str(), "r");
        if (!f) return;
        char line[1024];
        while (fgets(line, sizeof line, f) && bans_.size() < kMax) {
            Ban b;
            b.cidr   = field(line, "\"cidr\":\"");
            b.reason = field(line, "\"reason\":\"");
            b.atEpoch    = numField(line, "\"at\":");
            b.untilEpoch = numField(line, "\"until\":");
            if (b.cidr.empty()) continue;
            compile(b);            // ★ keep it even if invalid, so the owner can SEE and delete it
            bans_.push_back(b);
        }
        fclose(f);
    }

    void saveLocked() {
        if (path_.empty()) return;
        const std::string tmp = path_ + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return;
        for (const auto& b : bans_) {
            fprintf(f, "{\"cidr\":\"%s\",\"reason\":\"%s\",\"at\":%lld,\"until\":%lld}\n",
                    esc(b.cidr).c_str(), esc(b.reason).c_str(), b.atEpoch, b.untilEpoch);
        }
        fclose(f);
        rename(tmp.c_str(), path_.c_str());   // atomic replace; see the line-per-entry note above
    }

    static std::string field(const char* line, const char* key) {
        const char* p = strstr(line, key);
        if (!p) return {};
        p += strlen(key);
        std::string out;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { p++; out += (*p == 'n' ? '\n' : *p); p++; continue; }
            out += *p++;
        }
        return out;
    }
    static long long numField(const char* line, const char* key) {
        const char* p = strstr(line, key);
        return p ? atoll(p + strlen(key)) : 0;
    }

    std::mutex       mtx_;
    std::string      path_;
    std::vector<Ban> bans_;
    AsnResolver      asn_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  ★★★ THE CONNECTION LOG
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★ THE FIELD THAT MATTERS MOST IS *WHY IT ENDED*. "127 connections yesterday" tells an owner
// nothing. "41 of them ended in `banned`, all from one /24" tells them what to do. A log of
// arrivals with no outcomes is a counter wearing a log's clothes.
//
// ★ In memory and bounded. This is a Pi with an SD card; an unbounded on-disk log of every
// connection is a way to wear the card out and fill the rootfs, and nobody has ever wanted
// last March's connections. Kept across a restart is explicitly NOT promised.

struct ConnRec {
    long long   atEpoch = 0;      ///< When it opened
    long long   endEpoch = 0;     ///< 0 = still open
    std::string ip;
    std::string session;
    std::string agent;            ///< User-Agent, trimmed — tells an owner app-vs-browser-vs-bot
    std::string cc;               ///< ISO-3166 country, or empty when unknown. See geoip.cpp.
    std::string endReason;        ///< "closed" | "kicked" | "banned" | "queue-full" | "busy" | "timeout"
    /* ★★★ WHAT THEY ACTUALLY DID — the visit VERDICT. Stuart, 2026-09-21: "if a user has come for a
     *  couple of mins and heard nothing but static and then left that I really want to know about,
     *  likewise if they tuned about and heard a few stations then i know someone is gunuinely
     *  interested and found stuff to listen to."
     *  Duration and bytes cannot answer that — five minutes of static and five minutes of a station
     *  they enjoyed are the same row. stops/heard/bestSnr are computed from the SNR we already
     *  measure for the S meter (channel peak minus noise floor) and then discard.
     *  ★ -1 stops = not measured (an old record, or a close that never carried one), which is NOT
     *    the same as 0 stops = they stayed and found nothing. The page must not conflate them. */
    int         visitStops = -1;
    int         visitHeard = 0;
    float       visitBestSnr = 0;
    /** ★ Only set when they PARKED — one stop for the whole visit. He asked for the frequency only
     *  in that case: "dont need exact frequencies unless they literally only stayed on Heart FM". */
    double      parkedHz = 0;
    /** ★★★ AUDIO BYTES THE SERVER ACTUALLY SENT THIS VISIT — the other half of "did they hear
     *  anything". The SNR tally says whether there was a signal where they sat; this says whether
     *  any sound left the server at all. A web visitor who never pressed play, an app that kept the
     *  receiver muted (muting closes the audio socket) and a scanner all take a spectrum socket and
     *  NO audio — and a signal they could not hear is not "heard".
     *  ★ -1 = not known (an old record, or a legacy client with no session id to pair its audio
     *    socket with), which is NOT the same as 0 = we know nothing was sent. */
    long long   audioBytes = -1;
    uint64_t    bytes = 0;
    /** ★★★ IQ BUFFERS THIS VISIT LOST. The live monitor has always shown a drop count, but it
     *  lives on the per-listener channel and dies with them — so the number was visible only
     *  while you happened to be watching, and the history, which is what you read afterwards,
     *  had nothing. A session that delivered plenty of bytes AND dropped steadily is a different
     *  story from one that delivered plenty and dropped none: the first is a link that could not
     *  keep up, the second is a listener who simply left. */
    uint64_t    drops = 0;
    /** ★★★ DID THIS SESSION USE THE ADMIN PASSWORD?
     *
     *  An owner who can see WHO held an admin session, and from where, can spot a compromised
     *  password — which is otherwise invisible: an intruder with the password looks exactly like
     *  an ordinary listener in this log, and every protection here (the ban list, the frequency
     *  limits, the session limit) is one they can lift.
     *  ★ Stuart, 2026-08-12: "list when someone is in an admin session, as it will show if an
     *    admin password has been compromised."
     *  ★★ Recorded as a FACT about the connection, not derived later from a live flag: `adminOk`
     *     is per-process and clears on restart, so anything computed from it would forget exactly
     *     the history this is for. */
    bool        admin = false;
    /** ★★★ HOW MANY SOCKETS OF THIS VISIT ARE STILL OPEN. A listener holds a spectrum socket AND
     *  an audio socket; the log used to open on the spectrum one alone and close on whichever
     *  ended first, which worked only as long as the two never raced. They do: the client's
     *  reachability KNOCK is still closing when the real socket lands, so open() found a row that
     *  was still open, gave up, and started a second one. On the demo that was 104 of 246 rows
     *  stamped ZERO SECONDS — a log that reads as a stream of people bouncing off the server when
     *  in fact the median visit was two and a half minutes (2026-08-20).
     *  ★★ So a visit is one row and the row closes when its LAST socket does. Transient — not
     *     serialised, and a value restored from disk starts at zero, which close() treats as
     *     "already the last one". */
    int         live = 0;
};

/**
 * ★★★ RESOLVERS FOR THE CONNECTION LOG, injected by the shim.
 *
 * This header cannot call geoip/asndb directly — they live beside the daemon — but the log needs
 * them for a reason worth stating: the country stored on a record is a SNAPSHOT taken the moment
 * the connection opened. If the GeoIP database was missing, stale or simply had no entry for that
 * range at that moment, the row keeps the wrong answer FOR EVER, even after the data is fixed. On
 * the demo that showed as a work laptop logged in the US while the live table had it right
 * (Stuart, 2026-08-14) — and the history had NO network column at all, because none was ever
 * recorded.
 * ★★ So the log derives BOTH at render time, exactly as the live table does, and falls back to the
 *    stored value only when a fresh lookup has nothing to say. One derivation, two tables — the
 *    same rule as every other value that appears twice.
 */
inline std::function<std::string(const std::string&)>& ccResolver() {
    static std::function<std::string(const std::string&)> f;
    return f;
}
inline std::function<std::string(const std::string&)>& netResolver() {
    static std::function<std::string(const std::string&)> f;
    return f;
}

class ConnLog {
public:
    /** ★★★ PERSIST THE LOG. It used to live only in memory, and I wrote down that surviving a
     *  restart was "explicitly NOT promised" — reasoning about SD-card wear. That was the wrong
     *  call and Stuart found it immediately (2026-08-07): VibeServer restarts on EVERY UPDATE, so
     *  the history vanished precisely when an owner would want it, and the whole point of this log
     *  is noticing a pattern ACROSS time. A log that forgets on restart is a live view, not a log.
     *  ★★ The wear argument was also just wrong in scale: a closed connection is ~150 bytes. Even
     *     a thousand a day is 150 KB — against a spectrogram that writes 3 MB on a timer.
     *  ★ Empty path = memory only, which is every phone. */
    void setPath(std::string p) {
        { std::lock_guard<std::mutex> lk(mtx_); path_ = std::move(p); }
        load();
    }

    /** ★★★ CALLED FROM THE DAEMON'S 1 Hz LOOP, never from a socket thread. Appending on close
     *  would put a file write on the connection path — fine for a person hanging up, awful under
     *  a scan, where every REFUSAL is also a close and a burst of them becomes a burst of writes.
     *  Same reasoning as the spectrogram's saveIfDue: the network path raises a flag, the loop
     *  does the I/O. */
    void saveIfDue() {
        /* ★★★ THE FILE IS WRITTEN WITHOUT mtx_. The append (and, every few thousand rows, a rotation
         *  of thirteen files) used to happen under the lock the socket-close path waits for while it
         *  holds clientMtx — and an SD card that stalls a write for a moment stalled the DSP with it.
         *  The rows are swapped out under mtx_ and written under fileMtx_, which nothing near the
         *  audio path ever takes. fileMtx_ is taken FIRST so two flushes still append in order. */
        std::lock_guard<std::mutex> fk(fileMtx_);
        std::vector<ConnRec> rows;
        std::string path;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (path_.empty() || !dirty_) return;
            dirty_ = false;
            rows.swap(pending_);
            path = path_;
        }
        FILE* f = fopen(path.c_str(), "a");
        if (!f) return;
        for (const auto& r : rows) {
            fprintf(f, "{\"at\":%lld,\"end\":%lld,\"ip\":\"%s\",\"session\":\"%s\","
                       "\"agent\":\"%s\",\"cc\":\"%s\",\"reason\":\"%s\",\"bytes\":%llu,\"drops\":%llu,"
                       "\"admin\":%s%s}\n",
                    r.atEpoch, r.endEpoch, esc(r.ip).c_str(), esc(r.session).c_str(),
                    esc(r.agent).c_str(), esc(r.cc).c_str(), esc(r.endReason).c_str(),
                    (unsigned long long)r.bytes, (unsigned long long)r.drops,
                    r.admin ? "true" : "false", visitJson(r).c_str());
            ++written_;
        }
        fclose(f);
        /* ★★★ ROTATE, DO NOT TRUNCATE. This called rewriteLocked(), whose own comment says it writes
         *  the in-memory tail back out "discarding whatever the file held beyond it" — so history was
         *  permanently cut to kMax (2000) rows every time the file passed 4000.
         *  ★★ MEASURED on the Pi 500, 2026-09-20: the busiest radio takes 465 connections a day, so
         *     2000 rows is about FOUR DAYS. Stuart believed he had months, and on the QUIET radios he
         *     did (19/day → 105 days) — which is exactly why this was never noticed.
         *  ★★ A year is not expensive: ~170k rows at ~150 bytes is ~25 MB, well inside the 200 MB the
         *     journal is now capped at. The in-RAM deque stays at kMax because that is the LIVE VIEW;
         *     the file is the LOG, and the two had quietly become the same thing here.
         *  ★ Rotation rather than one huge file so retention drops a whole archive with an unlink
         *    instead of rewriting 25 MB on a Pi. */
        if (written_ > kMax * 2) { rotateIfDue(path); written_ = 0; }
    }

    void open(const std::string& ip, const std::string& session, const std::string& agent,
              const std::string& cc = "") {
        std::lock_guard<std::mutex> lk(mtx_);
        ++gen_;
        const long long now = nowEpoch();

        // ★★★ THE CLIENT KNOCKS BEFORE IT COMES IN, AND THAT IS NOT A VISIT. The web client opens
        //     a throwaway spectrum socket to test reachability and the PIN, closes it, then opens
        //     the real one — so EVERY listener produced two rows, one of them a one-second stub
        //     with a couple of KB. On the demo that was 111 of 218 records, and 61 of 71 sessions
        //     had one (Stuart, 2026-08-19: "fix the list so that it is genuine connections").
        //
        //  ★★★ COALESCED, NOT FILTERED, AND NOT MARKED BY THE CLIENT. A `probe=1` flag would be
        //      the obvious fix and it is the wrong one: anything that lets a CALLER say "do not
        //      log me" is a hole, and the same request asked to keep seeing "bots and malware
        //      trying to connect". So the server decides, from evidence the caller cannot forge —
        //      the stub shares this connection's SESSION and closed a moment ago having carried
        //      almost nothing. A scanner opening one socket still gets exactly one row.
        //
        //  ★★ Reopening the stub rather than adding a row also keeps the session's start time
        //     honest: the visit began when they first knocked, not when the second socket landed.
        //  ★ Session-keyed only. With no session there is nothing to be sure of, and merging on
        //    address alone would fold two different people behind one NAT into one visit.
        if (!session.empty()) {
            for (auto it = recs_.rbegin(); it != recs_.rend(); ++it) {
                if (it->session != session) continue;
                // ★★★ STILL OPEN = THE SECOND SOCKET OF THE SAME VISIT. This is the case the
                //     coalescer below was missing: it only knew how to absorb a stub that had
                //     already CLOSED, and the knock frequently has not by the time the real
                //     socket arrives. Falling through to a new row is what produced the pairs.
                // ★ Insurance: a row that somehow never closed must not swallow every later
                //   visit from the same tab for ever. Half a day is far longer than any real
                //   listen and far shorter than "for ever".
                if (it->endEpoch == 0 && (now - it->atEpoch) > 12 * 3600) break;
                if (it->endEpoch == 0) {
                    ++it->live;
                    if (it->agent.empty()) it->agent = agent.substr(0, 160);
                    if (it->cc.empty())    it->cc = cc;
                    return;
                }
                const bool closedJustNow = it->endEpoch > 0 && (now - it->endEpoch) <= 2;
                const bool carriedNothing = it->bytes < 20000;
                const bool wasBrief = (it->endEpoch - it->atEpoch) <= 1;
                if (closedJustNow && carriedNothing && wasBrief) {
                    it->endEpoch = 0;                 // open again — same visit, second socket
                    it->bytes = 0;
                    it->live = 1;
                    if (it->agent.empty()) it->agent = agent.substr(0, 160);
                    if (it->cc.empty())    it->cc = cc;
                    return;
                }
                break;                                // the newest row for this session is not a stub
            }
        }

        ConnRec r;
        r.atEpoch = now;
        r.live = 1;
        r.ip = ip;
        r.session = session;
        r.cc = cc;
        r.agent = agent.substr(0, 160);
        recs_.push_back(std::move(r));
        while (recs_.size() > kMax) recs_.pop_front();
    }

    /** Close the most recent still-open record for this session (or address, if no session).
     *  ★ MOST RECENT, searching backwards: one address can hold several sessions over an evening
     *    and closing the oldest would attribute a 3-second bounce to a 2-hour listen. */
    /** Mark the still-open record for this session as an ADMIN session.
     *  ★ Called at the moment the credential is verified, not at close: a session evicted or
     *    banned mid-flight must still be recorded as having held admin. */
    /** ★★★ REMEMBER IT EVEN IF THE ROW IS NOT HERE YET. Admin is proved during the HANDSHAKE, and
     *  the connection is not recorded until the spectrum socket is accepted several hundred lines
     *  later — so for a NEW session this marked nothing at all, and the owner's own app appeared in
     *  the history with no ADMIN badge while the server log showed it evicting the occupant
     *  (Stuart, 2026-08-15: "oddly no admin badge though"). A browser only ever got its badge
     *  because a LATER socket re-marked a row that by then existed.
     *  ★★ Fixed HERE rather than by reordering the caller: any future code that proves a
     *     credential before opening a row would hit the same silence, and the log is the thing
     *     that has to be right. open() consults the pending set.
     *  ★ Bounded, because it is fed by unauthenticated traffic: a session that never opens a row
     *     would otherwise leave an entry for ever. */
    void markAdmin(const std::string& ip, const std::string& session) {
        std::lock_guard<std::mutex> lk(mtx_);
        ++gen_;
        for (auto it = recs_.rbegin(); it != recs_.rend(); ++it) {
            if (it->endEpoch) continue;
            const bool hit = session.empty() ? (it->ip == ip) : (it->session == session);
            if (!hit) continue;
            it->admin = true;
            return;
        }
        if (!session.empty()) {
            if (pendingAdmin_.size() > 64) pendingAdmin_.erase(pendingAdmin_.begin());
            pendingAdmin_.insert(session);
        }
    }

    /** ★ `stops` defaults to -1 = "no verdict with this close", which is not the same as 0 stops.
     *  Only the path that actually watched the listener passes one; every other caller (timeout,
     *  idle, banned) keeps its existing signature and records nothing, honestly. */
    void close(const std::string& ip, const std::string& session,
               const char* reason, uint64_t bytes = 0, uint64_t drops = 0,
               int stops = -1, int heard = 0, float bestSnr = 0, double parkedHz = 0,
               long long audioBytes = -1) {
        std::lock_guard<std::mutex> lk(mtx_);
        ++gen_;
        for (auto it = recs_.rbegin(); it != recs_.rend(); ++it) {
            if (it->endEpoch) continue;
            const bool hit = session.empty() ? (it->ip == ip) : (it->session == session);
            if (!hit) continue;
            mergeInto(*it, bytes, drops, stops, heard, bestSnr, parkedHz, audioBytes);
            // ★★★ CLOSE ON THE LAST SOCKET, NOT THE FIRST. A visit holds two; ending the row when
            //     the first one goes stamped the visit with the length of whichever socket died
            //     soonest, which on a reconnect is zero. A record restored from disk has live 0
            //     and is treated as its own last socket, so nothing can be left open for ever.
            if (it->live > 1) { --it->live; return; }
            it->live = 0;
            it->endEpoch = nowEpoch();
            it->endReason = reason;
            pending_.push_back(*it);
            dirty_ = true;
            return;
        }
        // ★★★ NOT EVERY UNMATCHED CLOSE IS A REFUSAL — AND ASSUMING SO INVENTS HISTORY.
        //     A listener holds SEVERAL sockets (spectrum, audio, decoder) and the log deliberately
        //     OPENS on the spectrum one alone, "or logging both would double-count every ordinary
        //     browser". But it CLOSES on any of them: the first close matches the open record and
        //     records the real duration, and every later one finds nothing still open and falls
        //     through to here — manufacturing a phantom 0-second row for a connection that had
        //     just been logged correctly.
        //     ★★ On the demo that doubled the log: every visitor appeared as a real listen PLUS a
        //        0s entry, which reads as somebody connecting and instantly bouncing — the exact
        //        pattern an owner would examine for abuse. Stuart, 2026-08-11, on a Ukrainian
        //        address: "is this a bot or spam... a very distinct connection pattern 0 seconds
        //        then 2:50". It was neither: it was our own audio socket closing.
        //     ★ Conditional open, unconditional close — the same shape as the phantom
        //       "connected now" entries fixed on 2026-08-10, which were the mirror image
        //       (unconditional open, conditional close). Check BOTH ends when you fix one.
        //
        //     So: if we have already logged this SESSION, this close is a duplicate, not a
        //     refusal. Only synthesise when there is genuinely nothing on record.
        //     ★★ Session only — never by address. Two refusals from one IP are two events an
        //        owner wants to see (that is what a scan looks like); two closes carrying the same
        //        session id are one connection.
        if (!session.empty()) {
            for (auto it = recs_.rbegin(); it != recs_.rend(); ++it)
                if (it->session == session) {
                    /* ★★★ BUT KEEP WHAT IT CARRIES. A visit ended for a REASON (timeout, idle,
                     *  kicked) is closed by that path first, with no session id and no figures —
                     *  and the spectrum socket's own close, which holds the verdict, the audio and
                     *  the byte total, then lands here. Returning discarded all three, so every
                     *  row a limit had ended read "—" in Heard. Merge them into the closed row
                     *  (and into its unflushed copy, so the file agrees). */
                    mergeClosedLocked(*it, bytes, drops, stops, heard, bestSnr, parkedHz, audioBytes);
                    return;
                }
        }
        // Never opened (refused before we logged it) — record the refusal itself, which is
        // precisely the event an owner is looking for.
        ConnRec r;
        r.atEpoch = r.endEpoch = nowEpoch();
        r.ip = ip; r.session = session; r.endReason = reason;
        pending_.push_back(r);
        dirty_ = true;
        recs_.push_back(std::move(r));
        while (recs_.size() > kMax) recs_.pop_front();
    }

    /** ★★★ THE AUDIO SOCKET'S FIGURE, WHICHEVER SOCKET GOES LAST. A visit's audio socket often
     *  closes AFTER its spectrum socket (the spectrum close ends the row), and then the audio it
     *  delivered would never reach the record. The caller passes the session's running audio
     *  total; the newest row for that session takes it if it is larger.
     *  @return true when that row is already CLOSED — the caller's running total can be dropped,
     *          nothing later will ask for it. */
    bool noteAudio(const std::string& session, long long audioBytes) {
        if (session.empty() || audioBytes < 0) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        ++gen_;
        for (auto it = recs_.rbegin(); it != recs_.rend(); ++it) {
            if (it->session != session) continue;
            if (!it->endEpoch) { if (audioBytes > it->audioBytes) it->audioBytes = audioBytes; return false; }
            mergeClosedLocked(*it, 0, 0, -1, 0, 0, 0, audioBytes);
            return true;
        }
        return true;    // ★ no row for it at all (refused before it opened): nothing will ask
    }

    /** Newest first, capped.
     *
     *  ★★★ SNAPSHOT UNDER THE LOCK, FORMAT OUTSIDE IT — THIS LOCK IS ONE STEP FROM THE DSP.
     *      This used to build the whole page — 300 rows, a country AND an ASN lookup for each, every
     *      string escaped — while holding mtx_. And mtx_ is taken by close() and noteAudio() from the
     *      WebSocket close path WHILE THAT PATH HOLDS clientMtx, which the DSP thread (feedClient-
     *      Channels) and the audio fan-out (allAudioSocks) take on every block. So: the admin page
     *      polls this every 2 s → a listener's socket closes meanwhile (every web visit opens and
     *      closes a reachability KNOCK) → it waits on mtx_ holding clientMtx → the DSP waits on
     *      clientMtx → "IQ overrun — dropping a buffer (the DSP thread was blocked)". Measured on
     *      an M4 with 2000 rows and the real tables: a close() waited up to 1.15 s behind back-to-
     *      back admin reads; the Pi 2 is ~15-20x slower per read (2026-10-01, Pi 2 overruns at
     *      16:43:51/53 with CONNECTION HISTORY open).
     *  ★★ Now the lock covers a copy of the rows and nothing else; every lookup and every byte of
     *     JSON happens after it is released.
     *  ★ AND THE ANSWER IS CACHED until the log changes. The page asks every 2 s and the log
     *    changes when somebody connects or leaves — almost never, by comparison. The lookups'
     *    answers can move too (the country/ASN tables refresh weekly), so a cached page is also
     *    retired after kJsonCacheSecs whatever happens. */
    std::string json(size_t limit = 300) {
        const long long now = nowEpoch();
        uint64_t gen;
        { std::lock_guard<std::mutex> lk(mtx_); gen = gen_; }
        {
            std::lock_guard<std::mutex> ck(cacheMtx_);
            if (cacheValid_ && cacheGen_ == gen && cacheLimit_ == limit
                && now - cacheAt_ < kJsonCacheSecs && now >= cacheAt_)
                return cache_;
        }
        std::vector<ConnRec> rows;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            gen = gen_;
            rows.reserve(std::min(limit, recs_.size()));
            size_t n = 0;
            for (auto it = recs_.rbegin(); it != recs_.rend() && n < limit; ++it, ++n)
                rows.push_back(*it);
        }
        std::string out = formatRows(rows);
        {
            std::lock_guard<std::mutex> ck(cacheMtx_);
            cache_ = out; cacheGen_ = gen; cacheLimit_ = limit; cacheAt_ = now; cacheValid_ = true;
        }
        return out;
    }

    /** ★ Bumped by every change to the rows json() draws — the cache's key. Exposed for the tests. */
    uint64_t generation() { std::lock_guard<std::mutex> lk(mtx_); return gen_; }

private:
    /** The rows as the page reads them. Called with NO lock held — see json(). The lookups are
     *  memoised per address for the one call: a listener who visits twenty times is one lookup. */
    static std::string formatRows(const std::vector<ConnRec>& rows) {
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> memo;  // ip -> cc, net
        auto look = [&memo](const ConnRec& r) -> const std::pair<std::string, std::string>& {
            for (auto& m : memo) if (m.first == r.ip) return m.second;
            memo.push_back({ r.ip, { liveCc(r.ip, std::string()), liveNet(r.ip) } });
            return memo.back().second;
        };
        // ★ A linear memo, not a hash map: there are at most 300 rows and most share a handful
        //   of addresses, and it keeps this header free of another include.
        memo.reserve(64);
        std::string j;
        j.reserve(rows.size() * 400 + 2);
        j += '[';
        size_t n = 0;
        for (auto rit = rows.begin(); rit != rows.end(); ++rit, ++n) {
            const ConnRec* it = &*rit;
            const auto& ln = look(*it);
            const std::string& cc = ln.first.empty() ? it->cc : ln.first;
            if (n) j += ',';
            j += "{\"at\":" + std::to_string(it->atEpoch)
               + ",\"end\":" + std::to_string(it->endEpoch)
               + ",\"ip\":\"" + esc(it->ip) + "\""
               + ",\"session\":\"" + esc(it->session) + "\""
               + ",\"agent\":\"" + esc(it->agent) + "\""
               // ★ Fresh lookup first, stored snapshot second — see ccResolver().
               + ",\"cc\":\"" + esc(cc) + "\""
               + ",\"net\":\"" + esc(ln.second) + "\""
               + ",\"reason\":\"" + esc(it->endReason) + "\""
               + ",\"bytes\":" + std::to_string(it->bytes)
               + ",\"drops\":" + std::to_string(it->drops)
               /* ★★★ THE VERDICT GOES TO THE PAGE TOO. THREE writers serialise a ConnRec — the
                *  append in saveIfDue, the rewrite, and THIS, which is the one the admin page
                *  actually reads. I patched the two file writers and not this one, and the tests
                *  failed on all four verdict assertions: the field was on disk and invisible.
                *  Same shape as "ONE RULE, TWO READERS" — ask who ELSE serialises this. */
               + ",\"admin\":" + (it->admin ? "true" : "false") + visitJson(*it)
               // ★ Our own directory's Worker, not a person — see cloudflareWorkerAddr. The page
               //   labels the row and leaves it out of visitor and country counts.
               + (cloudflareWorkerAddr(it->ip) ? ",\"cfw\":true" : "") + "}";
        }
        j += ']';
        return j;
    }

public:
    /** The country for a stored record: today's answer if we have one, else what was recorded. */
    static std::string liveCc(const std::string& ip, const std::string& stored) {
        if (ccResolver()) { const std::string v = ccResolver()(ip); if (!v.empty()) return v; }
        return stored;
    }
    /** The network label. Never stored, so this is the only source — hence history showed none. */
    static std::string liveNet(const std::string& ip) {
        return netResolver() ? netResolver()(ip) : std::string();
    }

    /** ★★ TOP COUNTRIES, counted by DISTINCT ADDRESS rather than by connection.
     *  Counting connections would let one person who reloads forty times outrank a country that
     *  sent forty different listeners — which is the opposite of what the chart is for. */
    /*  ★★★ COUNTED OUTSIDE THE LOCK, AND IN n log n. This was a linear search of every distinct
     *      address for every row, under mtx_ — quadratic in the log's depth (2000 rows) on the lock
     *      the socket-close path waits for while holding clientMtx (see json()). Now the lock covers
     *      copying (cc, address) pairs — short strings, no allocation — and the counting happens
     *      after it is released. */
    std::string topCountriesJson(long long secs, size_t limit = 8) {
        const long long cut = nowEpoch() - secs;
        struct Hit { std::string cc, ip; size_t order; };
        std::vector<Hit> hits;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            hits.reserve(recs_.size());
            for (const auto& r : recs_) {
                if (r.atEpoch < cut || r.cc.empty()) continue;
                hits.push_back({ r.cc, r.ip, hits.size() });
            }
        }
        // ★ Cloudflare's Worker egress is nobody's country.
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [](const Hit& h) { return cloudflareWorkerAddr(h.ip); }), hits.end());
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if (a.cc != b.cc) return a.cc < b.cc;
            if (a.ip != b.ip) return a.ip < b.ip;
            return a.order < b.order; });
        // One entry per country: distinct addresses, and where the country FIRST appeared, so ties
        // keep the order they always had (first seen first).
        struct Cc { std::string cc; size_t n, first; };
        std::vector<Cc> byCc;
        for (size_t i = 0; i < hits.size(); i++) {
            const bool newCc = byCc.empty() || byCc.back().cc != hits[i].cc;
            if (newCc) { byCc.push_back({ hits[i].cc, 0, hits[i].order }); }
            Cc& c = byCc.back();
            if (newCc || hits[i].ip != hits[i - 1].ip) c.n++;
            c.first = std::min(c.first, hits[i].order);
        }
        std::stable_sort(byCc.begin(), byCc.end(), [](const Cc& a, const Cc& b) {
            return a.n != b.n ? a.n > b.n : a.first < b.first; });
        std::string j = "[";
        for (size_t i = 0; i < byCc.size() && i < limit; i++) {
            if (i) j += ',';
            j += "{\"cc\":\"" + esc(byCc[i].cc) + "\",\"n\":" + std::to_string(byCc[i].n) + "}";
        }
        return j + "]";
    }

    /** ★★★ WHAT WENT LOOKING FOR SOMETHING THAT IS NOT HERE — the scanners.
     *
     *  ★★★ NONE OF THIS WAS RECORDED. The connection log only ever sees an ACCEPTED WebSocket, so
     *      anything probing for /wp-login.php, /.env, /admin.php or a PHP shell got a bare 404 and
     *      left no trace at all. An owner asking "is anything trying to get in" had nothing to
     *      read (Stuart, 2026-08-19: "we also need to see bots and malware trying to connect").
     *
     *  ★★ THE PATH IS THE EVIDENCE, so it is kept — a 404 for /favicon.ico is a browser and a 404
     *     for /.git/config is somebody's scanner, and a count alone cannot tell them apart.
     *  ★ Trimmed hard and capped: this is attacker-controlled text arriving at whatever rate they
     *    choose, and it must never become a way to fill a Pi's memory or its disk.
     */
    struct ProbeRec { long long atEpoch = 0; std::string ip, cc, path, agent; int n = 1; };

    void noteScan(const std::string& ip, const std::string& path, const std::string& agent,
                  const std::string& cc = "") {
        if (ip.empty() || path.empty()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        const long long now = nowEpoch();
        // ★★ ONE ROW PER ADDRESS+PATH, COUNTED. A scanner walks a list of a hundred URLs and would
        //    otherwise bury every real connection in the panel above it; and one that retries the
        //    same path a thousand times is one FACT, not a thousand.
        for (auto it = scans_.rbegin(); it != scans_.rend(); ++it) {
            if (it->ip == ip && it->path == path) { it->n++; it->atEpoch = now; return; }
        }
        ProbeRec r;
        r.atEpoch = now; r.ip = ip; r.cc = cc;
        r.path = path.substr(0, 120);
        r.agent = agent.substr(0, 120);
        scans_.push_back(std::move(r));
        while (scans_.size() > kMaxScans) scans_.pop_front();
    }

    /** ★ Copied under the lock, formatted (with its country lookups) outside it — see json(). */
    std::string scansJson(size_t limit = 60) {
        std::vector<ProbeRec> rows;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            rows.reserve(std::min(limit, scans_.size()));
            size_t n = 0;
            for (auto it = scans_.rbegin(); it != scans_.rend() && n < limit; ++it, ++n)
                rows.push_back(*it);
        }
        std::string j = "[";
        size_t n = 0;
        for (auto it = rows.begin(); it != rows.end(); ++it, ++n) {
            if (n) j += ',';
            j += "{\"at\":" + std::to_string(it->atEpoch)
               + ",\"ip\":\"" + esc(it->ip) + "\""
               + ",\"cc\":\"" + esc(liveCc(it->ip, it->cc)) + "\""
               + ",\"path\":\"" + esc(it->path) + "\""
               + ",\"agent\":\"" + esc(it->agent) + "\""
               + ",\"n\":" + std::to_string(it->n)
               + (cloudflareWorkerAddr(it->ip) ? ",\"cfw\":true" : "") + "}";
        }
        return j + "]";
    }

    /** How many distinct addresses connected in the last `secs`. The one number that says
     *  whether the receiver is being USED or being SCANNED. */
    /*  ★★★ Same shape as topCountriesJson, and it was the WORST of them: a linear search of every
     *      address seen for every row, under mtx_, asked twice by every status poll — 3.3 ms per
     *      call on an M4 with 2000 rows, so the better part of 100 ms of lock on a Pi 2 per 2 s
     *      poll. Copy the addresses under the lock; sort and count outside it. */
    int uniqueSince(long long secs) {
        const long long cut = nowEpoch() - secs;
        std::vector<std::string> seen;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            seen.reserve(recs_.size());
            for (const auto& r : recs_)
                if (r.atEpoch >= cut) seen.push_back(r.ip);
        }
        // ★ not a visitor — see cloudflareWorkerAddr
        seen.erase(std::remove_if(seen.begin(), seen.end(),
                                  [](const std::string& ip) { return cloudflareWorkerAddr(ip); }), seen.end());
        std::sort(seen.begin(), seen.end());
        return (int)(std::unique(seen.begin(), seen.end()) - seen.begin());
    }

private:
    static const size_t kMax = 2000;
    /** ★★ THE FILE IS THE LOG; kMax above is only the LIVE VIEW's depth. Rotate the current file,
     *  then let the oldest archive fall off the end. A count of fixed-size archives bounds BOTH the
     *  bytes and, on any real receiver, the time — where an age rule alone would have no ceiling on
     *  a receiver busier than any we have seen. ~48 MB and ≈ a year on Stuart's busiest radio. */
    static const long   kRotateBytes  = 4L * 1024 * 1024;
    static const int    kKeepArchives = 12;
    /** ★★ The visit verdict, and ONLY when there is one. An absent field means "not measured",
     *  which a 0 would not: 0 stops is a real answer (they stayed and never settled anywhere) and
     *  the page must be able to tell those apart. Keeps the ordinary row the size it always was. */
    static std::string visitJson(const ConnRec& r) {
        char b[160];
        // ★ Audio stands on its own: DAB, or a visit too short to settle anywhere, has an audio
        //   figure and no SNR verdict, and the page needs the first even without the second.
        std::string out;
        if (r.audioBytes >= 0) {
            std::snprintf(b, sizeof b, ",\"audio\":%lld", r.audioBytes);
            out = b;
        }
        if (r.visitStops < 0) return out;
        std::snprintf(b, sizeof b, ",\"stops\":%d,\"heard\":%d,\"bestSnr\":%.1f",
                      r.visitStops, r.visitHeard, (double)r.visitBestSnr);
        out += b;
        // ★ The frequency only when they PARKED — one stop for the whole visit.
        if (r.visitStops == 1 && r.parkedHz > 0) {
            std::snprintf(b, sizeof b, ",\"parkedHz\":%.0f", r.parkedHz);
            out += b;
        }
        return out;
    }
    /** Fold one close's figures into a record. Every figure is a RUNNING TOTAL or a verdict, so
     *  the richer answer wins and a later, emptier close can never wipe a real one. */
    static void mergeInto(ConnRec& r, uint64_t bytes, uint64_t drops, int stops, int heard,
                          float bestSnr, double parkedHz, long long audioBytes) {
        // ★★★ THE BYTES ARE THE SESSION'S RUNNING TOTAL, not this socket's, so the LARGEST
        //     figure seen is the true one — taking whatever the last caller passed let a
        //     socket that carried almost nothing overwrite a megabyte count.
        if (bytes > r.bytes) r.bytes = bytes;
        if (drops > r.drops) r.drops = drops;
        /* ★★ A VISIT IS ONE VERDICT, NOT ONE PER SOCKET. Several sockets close for one visit
         *  and only the spectrum one watched the dial, so the tallies are MERGED by taking the
         *  richer answer rather than letting a later, emptier close wipe the real one — the
         *  same reasoning as the bytes above, which had exactly this bug. */
        if (stops >= 0 && stops >= r.visitStops) {
            r.visitStops   = stops;
            r.visitHeard   = heard;
            r.visitBestSnr = bestSnr;
            r.parkedHz     = parkedHz;
        }
        if (audioBytes > r.audioBytes) r.audioBytes = audioBytes;
    }
    /** mergeInto for a row that has ALREADY closed: the in-memory row, and its copy still waiting
     *  in pending_ if the 1 Hz flush has not written it yet. A row already on disk keeps what it
     *  was written with — the page (which reads memory) is still right until the next restart. */
    void mergeClosedLocked(ConnRec& r, uint64_t bytes, uint64_t drops, int stops, int heard,
                           float bestSnr, double parkedHz, long long audioBytes) {
        mergeInto(r, bytes, drops, stops, heard, bestSnr, parkedHz, audioBytes);
        for (auto& p : pending_)
            if (p.session == r.session && p.atEpoch == r.atEpoch)
                mergeInto(p, bytes, drops, stops, heard, bestSnr, parkedHz, audioBytes);
    }
    /** Called under fileMtx_ only — it touches the files, never the rows. */
    static void rotateIfDue(const std::string& path_) {
        if (path_.empty()) return;
        long sz = 0;
        if (FILE* f = fopen(path_.c_str(), "rb")) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
        if (sz < kRotateBytes) return;
        char buf[1200], buf2[1200];
        std::snprintf(buf, sizeof buf, "%s.%d", path_.c_str(), kKeepArchives);
        remove(buf);                                   // the oldest falls off the end
        for (int i = kKeepArchives - 1; i >= 1; --i) { // .11 <- .10 <- … <- .1
            std::snprintf(buf,  sizeof buf,  "%s.%d", path_.c_str(), i);
            std::snprintf(buf2, sizeof buf2, "%s.%d", path_.c_str(), i + 1);
            rename(buf, buf2);                         // a no-op when `buf` is absent
        }
        std::snprintf(buf, sizeof buf, "%s.1", path_.c_str());
        rename(path_.c_str(), buf);
        // ★ The next append recreates the current file. Nothing is lost — recs_ is the live view and
        //   the archive holds the history.
    }
    /** ★ Far smaller than the connection log: this is a curiosity, not a record to keep, and it is
     *  filled by whoever is scanning rather than by people using the receiver. */
    static const size_t kMaxScans = 200;
    std::deque<ProbeRec> scans_;

    void load() {
        std::lock_guard<std::mutex> lk(mtx_);
        ++gen_;
        recs_.clear();
        if (path_.empty()) return;
        /* ★★ READ THE NEWEST ARCHIVE FIRST, THEN THE CURRENT FILE, so the deque ends up holding the
         *  newest kMax rows across a rotation boundary. Without this, a restart just after the file
         *  rotated would show an almost EMPTY connection page — which reads exactly like "the log
         *  was wiped", the very fault this rotation was added to stop. One archive is enough: it is
         *  4 MB (~28k rows), far more than kMax. */
        char prev[1200];
        std::snprintf(prev, sizeof prev, "%s.1", path_.c_str());
        for (const char* which : { (const char*)prev, path_.c_str() }) loadFileLocked(which);
    }
    void loadFileLocked(const char* which) {
        FILE* f = fopen(which, "r");
        if (!f) return;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            ConnRec r;
            r.atEpoch   = numField(line, "\"at\":");
            r.endEpoch  = numField(line, "\"end\":");
            r.bytes     = (uint64_t)numField(line, "\"bytes\":");
            r.drops     = (uint64_t)numField(line, "\"drops\":");
            r.ip        = field(line, "\"ip\":\"");
            r.session   = field(line, "\"session\":\"");
            r.agent     = field(line, "\"agent\":\"");
            r.cc        = field(line, "\"cc\":\"");
            r.endReason = field(line, "\"reason\":\"");
            // ★ Absent in records written before this existed — false is the honest reading.
            r.admin     = strstr(line, "\"admin\":true") != nullptr;
            /* ★ Absent on every record written before the verdict existed — and -1 ("not measured")
             *  is the honest reading for those, which is the struct default. Only read one when the
             *  field is actually there; a 0 default would claim we watched and found nothing. */
            if (strstr(line, "\"stops\":")) {
                r.visitStops   = (int)numField(line, "\"stops\":");
                r.visitHeard   = (int)numField(line, "\"heard\":");
                r.visitBestSnr = (float)numField(line, "\"bestSnr\":");
                r.parkedHz     = (double)numField(line, "\"parkedHz\":");
            }
            // ★ Absent = not known (-1), exactly as for the verdict above.
            if (strstr(line, "\"audio\":")) r.audioBytes = numField(line, "\"audio\":");
            if (!r.atEpoch) continue;
            recs_.push_back(std::move(r));
            // ★ Keep only the newest in memory; the file may hold more than we display.
            while (recs_.size() > kMax) recs_.pop_front();
        }
        fclose(f);
        /* ★ How many rows are in the CURRENT file — the rotation trigger counts appends since the
         *  last rotate, so an archive's rows must not inflate it. recs_ may hold rows from the
         *  archive too, which is why this is not simply recs_.size() any more. */
        if (path_ == which) {
            size_t n = 0;
            if (FILE* g = fopen(which, "r")) {
                char l[1024];
                while (fgets(l, sizeof l, g)) ++n;
                fclose(g);
            }
            written_ = n;
        }
    }

    /** Write the in-memory tail back out, discarding whatever the file held beyond it. */
    void rewriteLocked() {
        const std::string tmp = path_ + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return;
        for (const auto& r : recs_) {
            if (!r.endEpoch) continue;          // still open — it will be written when it closes
            fprintf(f, "{\"at\":%lld,\"end\":%lld,\"ip\":\"%s\",\"session\":\"%s\","
                       "\"agent\":\"%s\",\"cc\":\"%s\",\"reason\":\"%s\",\"bytes\":%llu,\"drops\":%llu,"
                       "\"admin\":%s%s}\n",
                    r.atEpoch, r.endEpoch, esc(r.ip).c_str(), esc(r.session).c_str(),
                    esc(r.agent).c_str(), esc(r.cc).c_str(), esc(r.endReason).c_str(),
                    (unsigned long long)r.bytes, (unsigned long long)r.drops,
                    r.admin ? "true" : "false", visitJson(r).c_str());
        }
        fclose(f);
        rename(tmp.c_str(), path_.c_str());
    }

    static std::string field(const char* line, const char* key) {
        const char* p = strstr(line, key);
        if (!p) return {};
        p += strlen(key);
        std::string out;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { p++; out += (*p == 'n' ? '\n' : *p); p++; continue; }
            out += *p++;
        }
        return out;
    }
    static long long numField(const char* line, const char* key) {
        const char* p = strstr(line, key);
        return p ? atoll(p + strlen(key)) : 0;
    }

    std::mutex           mtx_;
    std::string          path_;
    std::deque<ConnRec>  recs_;
    /** Sessions that proved admin BEFORE their row was opened — see markAdmin. Consumed by open(). */
    std::set<std::string> pendingAdmin_;
    std::vector<ConnRec> pending_;      // closed since the last flush
    bool                 dirty_ = false;
    size_t               written_ = 0;
    /** Bumped under mtx_ by everything that changes a row json() draws. */
    uint64_t             gen_ = 0;
    /** ★ json()'s last answer — see the note there. Its own lock: a reader copying 100 KB out of
     *  the cache must not hold the lock the socket-close path needs. */
    static const long long kJsonCacheSecs = 30;
    std::mutex           cacheMtx_;
    std::string          cache_;
    uint64_t             cacheGen_ = 0;
    size_t               cacheLimit_ = 0;
    long long            cacheAt_ = 0;
    bool                 cacheValid_ = false;
    /** ★ Serialises the FILE (append + rotation) without holding mtx_ — see saveIfDue. */
    std::mutex           fileMtx_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  ★★★ THE MACHINE — load, temperature, memory, uptime
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★★ THE THRESHOLDS ARE RELATIVE TO CORE COUNT, which is the only way one number works on both
// a 4-core Pi 500 and whatever else this ends up on. Load 3.5 is idle on a 16-core box and on
// fire on a dual-core one. UberSDR uses the same rule (warning at 1x cores, critical at 2x) and
// it is the standard reading of a load average, so we are not inventing a scale.
//
// ★★ IT REPORTS WHAT IT COULD NOT READ, rather than reporting a zero. A CPU temperature of 0 °C
// on a page is indistinguishable from a very cold Pi, and an owner who believes it will not
// investigate the fan. Absent is absent.

/** ★★★ CPU USAGE, AS A PERCENTAGE — and it is NOT the load average.
 *
 *  Stuart, 2026-08-06: *"I prefer and understand the usage more than load."* He is right, and the
 *  two genuinely answer different questions:
 *    • USAGE is "how much of this machine is being used right now", 0-100%. It is what every task
 *      manager shows and what everyone means by "how busy is it".
 *    • LOAD is a count of runnable tasks averaged over minutes. It can exceed the core count, and
 *      "2.4" means nothing without knowing there are 4 cores. It is the better number for spotting
 *      a machine that is over-committed, and it is the worse number for a glance.
 *  ★★ SO SHOW BOTH, usage first. Dropping load would lose the one that catches a Pi thrashing on
 *     I/O, where usage can look calm.
 *
 *  ★★★ IT IS A DELTA, WHICH MEANS IT NEEDS TWO SAMPLES. /proc/stat is monotonic since boot, so
 *  reading it once gives the average since power-on — which on a box up for two days is a
 *  permanently flat number that looks plausible and answers nothing. The FIRST call therefore
 *  reports "not available" rather than that lie; from the second call on it is the usage since
 *  the previous call. The admin page polls every 2 s, so the first reading appears immediately.
 */
struct CpuSample { unsigned long long busy = 0, total = 0; bool valid = false; };

inline CpuSample readCpuSample() {
    CpuSample s;
#if defined(__APPLE__)
    // ★ The same busy/total delta as /proc/stat, from mach. CPU_STATE_IDLE is the not-busy part;
    //   macOS has no iowait state, so there is no equivalent of the iowait trap here.
    host_cpu_load_info_data_t info{};
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &cnt) == KERN_SUCCESS) {
        for (int i = 0; i < CPU_STATE_MAX; i++) s.total += info.cpu_ticks[i];
        s.busy = s.total - info.cpu_ticks[CPU_STATE_IDLE];
        s.valid = s.total > 0;
    }
#elif defined(__linux__)
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return s;
    char line[512];
    if (fgets(line, sizeof line, f) && strncmp(line, "cpu ", 4) == 0) {
        // user nice system idle iowait irq softirq steal guest guest_nice
        unsigned long long v[10] = {0};
        const int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (n >= 4) {
            for (int i = 0; i < n; i++) s.total += v[i];
            // ★ idle AND iowait both count as NOT BUSY. Counting iowait as busy is a common
            //   mistake that makes an idle machine waiting on a slow SD card read as 100%.
            s.busy = s.total - v[3] - (n > 4 ? v[4] : 0);
            s.valid = true;
        }
    }
    fclose(f);
#endif
    return s;
}

/** ★★★ THIS PROCESS'S OWN CPU, as a fraction of ONE core — the fallback for a machine that will
 *  not let us read the whole picture.
 *
 *  ★★★ ON ANDROID, AN APP CANNOT READ /proc/stat. SELinux and hidepid keep it to the shell, so the
 *      admin page served BY THE PHONE said "CPU USAGE not available" while the app's own status
 *      screen, three inches away, showed "45% of 1 core" — because the Kotlin side reads
 *      /proc/self/stat, which a process may always read about itself (Stuart, 2026-08-20: "what I
 *      dont understand is on the phone screen I can see the actual CPU usage").
 *  ★★ Two readers of one machine disagreeing is the shape this file keeps meeting. The cure is the
 *     same each time: ONE source. So the page now falls back to exactly what the app reads, and
 *     SAYS which of the two numbers it is showing — see SysStats::cpuIsProcess. A percentage that
 *     silently changes meaning between platforms would be worse than the blank it replaces.
 *  ★ Percent of one core, so >100 is possible and meaningful on a multi-core phone — the same
 *    convention the app screen and the DSP benchmarks already use.
 */
struct SelfCpuSample { unsigned long long ticks = 0; double wall = 0; bool valid = false; };

inline SelfCpuSample readSelfCpu() {
    SelfCpuSample s;
#if defined(__linux__)
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return s;
    char buf[1024];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!n) return s;
    buf[n] = 0;
    // ★ Fields are space separated, but field 2 is the executable name IN PARENTHESES and may
    //   itself contain spaces — so counting from the start is wrong. Everyone who reads this file
    //   scans from the LAST ')' instead; utime is field 14, stime 15, i.e. 12 and 13 after it.
    const char* p2 = strrchr(buf, ')');
    if (!p2) return s;
    unsigned long long utime = 0, stime = 0;
    int field = 2;                       // the ')' closes field 2
    for (const char* q = p2 + 1; *q; ) {
        while (*q == ' ') ++q;
        if (!*q) break;
        ++field;
        if (field == 14) utime = strtoull(q, nullptr, 10);
        else if (field == 15) { stime = strtoull(q, nullptr, 10); break; }
        while (*q && *q != ' ') ++q;
    }
    const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    s.ticks = (utime + stime) * 1000ull / (unsigned long long)hz;   // milliseconds of CPU
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s.wall = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    s.valid = s.wall > 0;
#endif
    return s;
}

/** True when the last cpuUsagePct() answer described THIS PROCESS rather than the machine. */
inline bool& cpuPctIsProcess() { static bool v = false; return v; }

/** Percent busy since the previous call, or -1 until there is a previous call. */
inline double cpuUsagePct() {
    static std::mutex  mtx;
    static CpuSample   prev;
    std::lock_guard<std::mutex> lk(mtx);
    const CpuSample now = readCpuSample();
    if (!now.valid) {
        // ★ The machine-wide view is closed to us (an Android app). Report our own, and say so.
        static SelfCpuSample selfPrev;
        const SelfCpuSample selfNow = readSelfCpu();
        if (!selfNow.valid) { cpuPctIsProcess() = false; return -1; }
        const double dWall = selfNow.wall - selfPrev.wall;
        const bool first = !selfPrev.valid || dWall <= 0 || selfNow.ticks < selfPrev.ticks;
        const double dCpu = first ? 0 : (double)(selfNow.ticks - selfPrev.ticks);
        selfPrev = selfNow;
        if (first) { cpuPctIsProcess() = false; return -1; }   // ★ one sample is not a rate
        cpuPctIsProcess() = true;
        const double pct = 100.0 * dCpu / dWall;
        return pct < 0 ? 0 : pct;
    }
    cpuPctIsProcess() = false;
    if (!prev.valid || now.total <= prev.total) { prev = now; return -1; }
    const double dTotal = (double)(now.total - prev.total);
    const double dBusy  = (double)(now.busy  - prev.busy);
    prev = now;
    if (dTotal <= 0) return -1;
    const double pct = 100.0 * dBusy / dTotal;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

struct SysStats {
    bool   haveLoad = false;
    double load1 = 0, load5 = 0, load15 = 0;
    /** Percent of the whole machine in use since the last sample; <0 = not yet known. */
    double cpuPct = -1;
    /** ★ The percentage above is THIS PROCESS on one core, not the machine — see cpuUsagePct().
     *  The page must say which, or the same number means two different things on two platforms. */
    bool   cpuIsProcess = false;
    int    cores = 0;
    bool   haveTemp = false;
    double tempC = 0;
    bool   haveMem = false;
    long long memTotalKB = 0, memAvailKB = 0;
    bool   haveUptime = false;
    double uptimeSec = 0;
    std::string governor;
    long long   cpuKHz = 0;

    /** ★★★ IS THE POWER SUPPLY COPING? On a Raspberry Pi this is the fault that looks like every
     *  other fault: USB devices drop out, streams die, and nothing in any log says "your PSU".
     *  It cost an evening here — three SDRs declaring 1900 mA against a 600 mA default budget
     *  produced 66 over-current events and a radio that would not stay up, and the only place it
     *  was written down was `dmesg`, which nobody reads on an appliance in a cupboard.
     *  ★★ LATCHED, not sampled. A brownout an hour ago is exactly what you want reported: by the
     *     time an owner opens the page the voltage has usually recovered, and a page showing "all
     *     fine" would be telling the truth about this instant and lying about the machine.
     *  ★ Read from the rpi_volt hwmon, NOT `vcgencmd` — a shell per sample is not on, and the
     *    alarm is latched there by the kernel. (The firmware's own flags now come from the mailbox
     *    below, where the service user is in `video`; this stays as the latch and the fallback.) */
    bool haveVolt = false;
    bool underVoltageNow = false;
    bool underVoltageEver = false;

    /** ★★★ THE PI'S FIRMWARE CLOCK — what the ARM is ACTUALLY running at. When present, cpuKHz above
     *  IS this figure, because sysfs reports the clock the kernel asked for and the firmware quietly
     *  delivers less under a sagging supply (Pi 500, 2026-09-30: sysfs 2400, measured 1000). See
     *  vibe_vcio.h. Absent everywhere else, and nothing changes there. */
    bool      haveFw = false;
    long long fwMaxKHz = 0;          ///< the firmware's maximum ARM clock
    long long fwAskedKHz = 0;        ///< what sysfs said — kept so the page can show the gap
    long long fwThrottled = -1;      ///< get_throttled bits, -1 = not readable
};

/** ★ Fold a firmware reading into the stats. Separate from readSys() so it can be tested without a
 *  Pi: the mailbox transport is swappable (vibevcio::mboxOverride) and this is the only decision.
 *  ★★ The MEASURED clock replaces cpuKHz outright — every reader of cpuKHz (the tile, the history
 *     graph) wanted the truth and was getting the request. */
inline void applyFirmwareClock(SysStats& s, const vibevcio::Reading& r) {
    if (!r.ok || r.armHz <= 0 || r.armMaxHz <= 0) return;
    s.haveFw      = true;
    s.fwAskedKHz  = s.cpuKHz;
    s.cpuKHz      = r.armHz / 1000;
    s.fwMaxKHz    = r.armMaxHz / 1000;
    s.fwThrottled = r.throttled;
}

inline SysStats readSys() {
    SysStats s;
#if defined(__APPLE__)
    // ★★ macOS HAS NO /proc AT ALL, so everything here comes from sysctl and mach. Verified on a
    //    real machine rather than assumed (2026-08-07).
    {
        struct loadavg la;
        size_t sz = sizeof la;
        if (sysctlbyname("vm.loadavg", &la, &sz, nullptr, 0) == 0 && la.fscale) {
            s.load1  = (double)la.ldavg[0] / la.fscale;
            s.load5  = (double)la.ldavg[1] / la.fscale;
            s.load15 = (double)la.ldavg[2] / la.fscale;
            s.haveLoad = true;
        }
    }
    {
        uint64_t mem = 0; size_t sz = sizeof mem;
        if (sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0) == 0 && mem) {
            s.memTotalKB = (long long)(mem / 1024);
            // ★ "Available" on macOS is free + inactive + speculative: inactive pages are
            //   reclaimable on demand, so counting only `free` reports a Mac as permanently
            //   almost-full — which is true of every Mac and useful to nobody.
            vm_size_t page = 0;
            vm_statistics64_data_t vm{};
            mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
            if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
                host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                  (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
                const uint64_t avail = (uint64_t)(vm.free_count + vm.inactive_count
                                                + vm.speculative_count) * page;
                s.memAvailKB = (long long)(avail / 1024);
            }
            s.haveMem = true;
        }
    }
    {
        struct timeval bt{};
        size_t sz = sizeof bt;
        if (sysctlbyname("kern.boottime", &bt, &sz, nullptr, 0) == 0 && bt.tv_sec) {
            s.uptimeSec = (double)(time(nullptr) - bt.tv_sec);
            s.haveUptime = s.uptimeSec > 0;
        }
    }
    // ★★★ NO TEMPERATURE ON macOS, DELIBERATELY. There is no public API for it — reading it needs
    //     private SMC/IOKit calls, which are undocumented, break across releases, and are exactly
    //     the sort of thing that draws attention during notarisation. The panel already reports a
    //     missing sensor honestly as "not available", so the cost of leaving it out is a card
    //     that tells the truth rather than a number that might be a lie.
#elif defined(__linux__)
    {
        const std::string la = slurp("/proc/loadavg", 128);
        if (!la.empty() && sscanf(la.c_str(), "%lf %lf %lf", &s.load1, &s.load5, &s.load15) == 3)
            s.haveLoad = true;
    }
    {
        // ★ Try the common thermal zones in turn. The Pi's CPU is zone 0; other boards vary, and
        //   a container has none at all — which is a legitimate answer, not a failure.
        static const char* kZones[] = {
            "/sys/class/thermal/thermal_zone0/temp",
            "/sys/class/thermal/thermal_zone1/temp",
            "/sys/devices/virtual/thermal/thermal_zone0/temp",
        };
        for (const char* z : kZones) {
            const std::string t = slurp(z, 32);
            if (t.empty()) continue;
            const double milli = atof(t.c_str());
            if (milli <= 0) continue;
            // Some drivers report degrees, most report millidegrees. 200 is not a plausible
            // temperature and 200000 is not a plausible one either — pick by magnitude.
            s.tempC = milli > 1000 ? milli / 1000.0 : milli;
            s.haveTemp = true;
            break;
        }
    }
#if defined(__linux__)
    {
        // The alarm lives under whichever hwmon calls itself rpi_volt — the number moves between
        // kernels, so find it by NAME rather than hard-coding hwmon2.
        static std::string voltPath = [] {
            for (int i = 0; i < 8; i++) {
                char nameF[64]; snprintf(nameF, sizeof nameF, "/sys/class/hwmon/hwmon%d/name", i);
                std::string n = slurp(nameF, 32);
                while (!n.empty() && (n.back() == '\n' || n.back() == ' ')) n.pop_back();
                if (n == "rpi_volt") {
                    char f[80]; snprintf(f, sizeof f, "/sys/class/hwmon/hwmon%d/in0_lcrit_alarm", i);
                    return std::string(f);
                }
            }
            return std::string();
        }();
        static bool everSeen = false;
        if (!voltPath.empty()) {
            const std::string v = slurp(voltPath.c_str(), 16);
            if (!v.empty()) {
                s.haveVolt = true;
                s.underVoltageNow = (atoi(v.c_str()) != 0);
                if (s.underVoltageNow) everSeen = true;
                s.underVoltageEver = everSeen;
            }
        }
    }
#endif
    {
        const std::string mi = slurp("/proc/meminfo", 2048);
        if (!mi.empty()) {
            const char* p = strstr(mi.c_str(), "MemTotal:");
            const char* q = strstr(mi.c_str(), "MemAvailable:");
            if (p) s.memTotalKB = atoll(p + 9);
            if (q) s.memAvailKB = atoll(q + 13);
            else {
                // ★★ MemAvailable arrived in Linux 3.14. Android 5.x kernels (3.4 / 3.10 — Kiko's Moto G,
                //   2026-10-01) don't have it, so it read as 0 and SERVER HEALTH showed RAM at 100 %
                //   forever. The kernel's own pre-3.14 estimate: free + buffers + page cache.
                const char* f = strstr(mi.c_str(), "MemFree:");
                const char* b = strstr(mi.c_str(), "Buffers:");
                const char* c = strstr(mi.c_str(), "\nCached:");
                long long kb = 0;
                if (f) kb += atoll(f + 8);
                if (b) kb += atoll(b + 8);
                if (c) kb += atoll(c + 8);
                if (s.memTotalKB > 0 && kb > s.memTotalKB) kb = s.memTotalKB;
                s.memAvailKB = kb;
            }
            s.haveMem = s.memTotalKB > 0;
        }
    }
    {
        const std::string up = slurp("/proc/uptime", 64);
        if (!up.empty()) { s.uptimeSec = atof(up.c_str()); s.haveUptime = s.uptimeSec > 0; }
    }
    s.governor = slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", 64);
    {
        // ★★ AVERAGE EVERY CORE, NOT cpu0. Reading one core reported 2400 MHz while btop showed
        //    2.3 GHz across the four — ours was the optimistic number, and on a machine being
        //    watched for supply dips the optimistic number is the useless one. The cores clock
        //    independently, so one of them is not the machine (Stuart, 2026-08-08: "our clock
        //    speed report is more optimistic").
        // ★ cpuinfo_cur_freq is what the hardware IS doing; scaling_cur_freq is what the governor
        //   ASKED for. Prefer the former where the kernel exposes it, per core.
        long long sum = 0; int n = 0;
        for (int c = 0; c < 64; ++c) {
            char path[128];
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", c);
            std::string f = slurp(path, 64);
            if (f.empty()) {
                snprintf(path, sizeof path,
                         "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
                f = slurp(path, 64);
            }
            if (f.empty()) { if (c == 0) continue; else break; }
            const long long khz = atoll(f.c_str());
            if (khz > 0) { sum += khz; n++; }
        }
        if (n > 0) s.cpuKHz = sum / n;
    }
    applyFirmwareClock(s, vibevcio::read());   // ★ a silent no-op off a Pi, or without `video`
#endif
    s.cpuPct = cpuUsagePct();
    s.cpuIsProcess = cpuPctIsProcess();
    /* ★★ EVERY CORE THE CHIP HAS, NOT JUST THE ONES AWAKE (Kiko, 2026-10-05: the TV box's admin page "only shows 2
     *  or 3 CPU cores"). hardware_concurrency() is the ONLINE count on Android/bionic, and a TV box or phone switches
     *  idle cores off — so "load 1.2 of 2 cores" bounced between 2 and 3 on a quad-core box. presentCpus() reads
     *  /sys/devices/system/cpu/present (the hardware), the same fix the directory's hardware card got 2026-09-27;
     *  it falls back to the online count where /sys is absent (macOS). Load thresholds follow it: the kernel wakes
     *  offline cores under load, so the hardware is the right yardstick. */
    s.cores = (int)std::max(1L, vibe::presentCpus());
    return s;
}

/** "ok" | "warning" | "critical", from the 15-minute load against the core count.
 *  ★★ STILL DRIVEN BY LOAD, NOT BY USAGE, deliberately. Usage is the better number to READ and
 *  the worse one to ALARM on: it is instantaneous, so it spikes to 100% every time the FFT runs
 *  and would light the card up constantly. The 15-minute load is a mean over minutes, which is
 *  what "this machine is genuinely in trouble" actually looks like. */
inline const char* loadStatus(const SysStats& s) {
    if (!s.haveLoad || s.cores <= 0) return "unknown";
    if (s.load15 >= s.cores * 2.0) return "critical";
    if (s.load15 >= s.cores * 1.0) return "warning";
    return "ok";
}

/** "ok" | "warning" | "critical" from the CPU temperature.
 *  ★ 70/80 °C are the Pi's own numbers: the firmware soft-throttles at 80 and hard-throttles at
 *    85, so 80 is not a scare figure — past it the receiver IS running slower. */
/** ★★★ TEMPERATURE ONLY. This used to fold UNDER-VOLTAGE into the temperature verdict, so a Pi on
 *  a weak supply turned the TEMP card red and told the owner to check their cooling — at 45 °C,
 *  which is a perfectly healthy chip. The advice was not merely unnecessary, it pointed at the
 *  wrong component entirely: the Pi's throttling in that state is POWER related, and there is a
 *  POWER card three inches away already saying so (Stuart, 2026-08-08: "the throttles are not
 *  temperature they are power related so the warning under the temp box is plain wrong").
 *  ★★ One reading, one card. Ranking power above temperature INSIDE the temperature status also
 *  double-reported the same fault in two places while hiding the actual temperature behind it.
 *  ★ Thresholds sit where the hardware acts, not where a cautious guess would: a Pi 5 soft-
 *  throttles around 80 °C and hard-throttles at 85, so 80 is "it is happening now" and 75 is
 *  "close enough to mention". Anything below that is a chip doing its job. */
inline const char* tempStatus(const SysStats& s) {
    if (!s.haveTemp) return "unknown";
    if (s.tempC >= 80.0) return "critical";
    if (s.tempC >= 75.0) return "warning";
    return "ok";
}

inline std::string sysJson(const SysStats& s) {
    std::string j = "{\"cores\":" + std::to_string(s.cores);
    if (s.haveLoad) {
        char b[128];
        snprintf(b, sizeof b, ",\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f", s.load1, s.load5, s.load15);
        j += b;
    }
    if (s.cpuPct >= 0) {
        char b[48]; snprintf(b, sizeof b, ",\"cpuPct\":%.1f", s.cpuPct); j += b;
        if (s.cpuIsProcess) j += ",\"cpuIsProcess\":true";
    }
    j += std::string(",\"loadStatus\":\"") + loadStatus(s) + "\"";
    if (s.haveTemp) { char b[64]; snprintf(b, sizeof b, ",\"tempC\":%.1f", s.tempC); j += b; }
    if (s.haveVolt) {
        j += ",\"underVoltage\":";     j += s.underVoltageNow  ? "true" : "false";
        j += ",\"underVoltageEver\":"; j += s.underVoltageEver ? "true" : "false";
    }
    j += std::string(",\"tempStatus\":\"") + tempStatus(s) + "\"";
    if (s.haveMem) {
        j += ",\"memTotalKB\":" + std::to_string(s.memTotalKB)
           + ",\"memAvailKB\":" + std::to_string(s.memAvailKB);
    }
    if (s.haveUptime) j += ",\"uptimeSec\":" + std::to_string((long long)s.uptimeSec);
    if (!s.governor.empty()) j += ",\"governor\":\"" + esc(s.governor) + "\"";
    if (s.cpuKHz > 0)        j += ",\"cpuKHz\":" + std::to_string(s.cpuKHz);
    /* ★ ADDITIVE: an older admin page ignores these and shows cpuKHz, which is now the measured
     *  figure anyway. A newer one says it is the firmware's and whether it is being held down. */
    if (s.haveFw) {
        j += ",\"cpuFirmware\":true,\"cpuMaxKHz\":" + std::to_string(s.fwMaxKHz);
        if (s.fwAskedKHz > 0)   j += ",\"cpuAskedKHz\":" + std::to_string(s.fwAskedKHz);
        if (s.fwThrottled >= 0) j += ",\"throttled\":" + std::to_string(s.fwThrottled);
    }
    return j + "}";
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  ★★★ HISTORY — so the graphs have something to draw
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★ A LIVE NUMBER CANNOT ANSWER THE QUESTION AN OWNER IS ASKING. "Load is 2.1" is meaningless
// on its own; "load has been climbing for an hour" is a diagnosis. One sample a second for an
// hour is 3600 entries of 24 bytes — trivial — and it is exactly the window in which somebody
// notices something is wrong and goes to look.

struct HistSample {
    long long atEpoch = 0;
    float     load1 = 0;
    float     tempC = 0;
    uint16_t  listeners = 0;
    uint32_t  kbps = 0;
    /** ★ CPU clock, MHz. On a Pi the governor drops this when the supply sags, so the clock is
     *  where an under-voltage event becomes VISIBLE — the temperature stays fine and the load
     *  looks normal while the machine quietly runs slower (Stuart, 2026-08-08, running `ondemand`
     *  to see whether he can ride out the dips). A number without its history cannot show that. */
    uint16_t  mhz = 0;
};

class History {
public:
    void push(const HistSample& s) {
        std::lock_guard<std::mutex> lk(mtx_);
        samples_.push_back(s);
        while (samples_.size() > kMax) samples_.pop_front();
    }
    /** `after` > 0: only the samples taken AFTER that epoch second, and the reply says so
     *  (`"after":N`) — the admin page asks for the new tail every 2 s instead of the whole hour
     *  (~100 KB, up the same uplink as the audio). A reply without `after` is the whole ring, which
     *  is what an older page (or an older server, to a newer page) gets. */
    std::string json(long long after) {
        std::vector<HistSample> rows;
        {   // ★ Copied under the lock, formatted outside it.
            std::lock_guard<std::mutex> lk(mtx_);
            rows.reserve(samples_.size());
            for (const auto& s : samples_) if (s.atEpoch > after) rows.push_back(s);
        }
        // ★ ARRAY OF ARRAYS, not array of objects: 3600 copies of the key names is ~200 KB of
        //   text for 90 KB of data, on a link the owner may be reaching over 4G from a field.
        // ★ APPENDED, never inserted: the reader indexes by position, so a new column in the
        //   middle would silently shift every existing series by one.
        std::string j = "{\"fields\":[\"at\",\"load1\",\"tempC\",\"listeners\",\"kbps\",\"mhz\"],";
        if (after > 0) j += "\"after\":" + std::to_string(after) + ",";
        j += "\"rows\":[";
        j.reserve(j.size() + rows.size() * 40 + 2);
        bool first = true;
        for (const auto& s : rows) {
            char b[128];
            snprintf(b, sizeof b, "%s[%lld,%.2f,%.1f,%u,%u,%u]", first ? "" : ",",
                     s.atEpoch, s.load1, s.tempC, (unsigned)s.listeners, (unsigned)s.kbps,
                     (unsigned)s.mhz);
            j += b;
            first = false;
        }
        return j + "]}";
    }
private:
    static const size_t kMax = 3600;   // one hour at 1 Hz
    std::mutex             mtx_;
    std::deque<HistSample> samples_;
};

/**
 * ★★★ THE OWNER'S NOTICE TO LISTENERS — "antenna maintenance in progress".
 *
 *     Born of a real afternoon: the antenna needed work, and the choice was to power the server
 *     down (leaving a dead link on the website) or leave it up while people watched the spectrum
 *     jump about and concluded the receiver was rubbish (Stuart, 2026-08-13). Both are worse than
 *     simply SAYING so. Nothing about the radio is wrong in that moment — only the explanation is
 *     missing.
 *
 * ★★★ TWO KINDS, AND BOTH ARE REAL. A TIMED notice is for work you are doing right now — you say
 *     how long it will take and it dies on its own, so the failure mode is "it vanished while I
 *     was still up a ladder" rather than "it told everyone for a fortnight that the antenna was
 *     being worked on". A banner nobody remembers to clear ages into a lie, and every later
 *     warning is trusted less for it.
 *     ★★ An INDEFINITE one is for a fault you have not fixed yet: "noticed the antenna being
 *        faulty whilst I was out at work today ... left it up indefinitely as I don't know when I
 *        will get around to fixing it" (Stuart, 2026-08-13). Forcing a duration there would make
 *        the owner either lie about a repair time or re-post it every hour.
 *     ★ Which is why CLEARING is emptying the TEXT, not setting zero minutes: zero minutes means
 *       "no end", and a control where "0" silently means "delete" is one nobody trusts.
 *
 * ★★ A FILE, in the shared data directory, exactly like the ban list. On a multi-radio machine the
 *    front door and every radio are SEPARATE PROCESSES: a notice set through the door's admin page
 *    would otherwise be invisible on the very radios people are listening to. Re-read when the
 *    file's mtime changes, so it costs a stat and appears everywhere within a second.
 */
class Notice {
public:
    void setPath(std::string p) {
        std::lock_guard<std::mutex> lk(mtx_);
        path_ = std::move(p);
        mtime_ = 0;                       // force a read
    }

    /**
     * Post a notice. `minutes` <= 0 means INDEFINITE — up until the owner clears it.
     * An EMPTY `text` clears it, whatever the minutes say.
     * Returns false only if there is nowhere to write it: a server with no data directory cannot
     * share a notice between its processes.
     */
    bool set(const std::string& text, int minutes, std::string& err) {
        std::string path;
        { std::lock_guard<std::mutex> lk(mtx_); path = path_; }
        if (path.empty()) { err = "this server has no data directory to store a notice in"; return false; }
        const long long until = text.empty() ? 0
                              : minutes <= 0 ? 0
                              : (long long)::time(nullptr) + (long long)minutes * 60;
        FILE* f = fopen(path.c_str(), "w");
        if (!f) { err = "could not write " + path; return false; }
        // ★ Written even when CLEARING, rather than deleting the file: every process notices a
        //   change by mtime, and a deleted file is a change nobody is watching for.
        fprintf(f, "{\"text\":\"%s\",\"until\":%lld}\n", esc(text).c_str(), until);
        fclose(f);
        { std::lock_guard<std::mutex> lk(mtx_); mtime_ = 0; }
        return true;
    }

    /** The notice to show right now, or "" — cleared, expired, or never set. */
    std::string current() {
        refresh();
        std::lock_guard<std::mutex> lk(mtx_);
        if (text_.empty()) return std::string();
        if (until_ > 0 && (long long)::time(nullptr) >= until_) return std::string();
        return text_;
    }

    /** -1 = showing with no end date · 0 = nothing showing · >0 = seconds left.
     *  ★ The three states are DISTINCT on purpose: the admin page has to be able to say "until you
     *    clear it" rather than showing an indefinite notice as though it were about to expire. */
    long long secondsLeft() {
        refresh();
        std::lock_guard<std::mutex> lk(mtx_);
        if (text_.empty()) return 0;
        if (until_ <= 0) return -1;
        const long long left = until_ - (long long)::time(nullptr);
        return left > 0 ? left : 0;
    }

private:
    static std::string esc(const std::string& in) {
        std::string o;
        for (char c : in) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c < 0x20) o += ' ';    // one line, always
            else o += c;
        }
        return o;
    }

    void refresh() {
        std::string path;
        { std::lock_guard<std::mutex> lk(mtx_); path = path_; }
        if (path.empty()) return;
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            text_.clear(); until_ = 0;
            return;
        }
        const long long m = (long long)st.st_mtime;
        { std::lock_guard<std::mutex> lk(mtx_); if (m == mtime_) return; }

        std::string body;
        if (FILE* f = fopen(path.c_str(), "r")) {
            char buf[1024];
            while (fgets(buf, sizeof buf, f)) body += buf;
            fclose(f);
        }
        std::string text;
        long long until = 0;
        const size_t t = body.find("\"text\":\"");
        if (t != std::string::npos) {
            size_t i = t + 8;
            while (i < body.size() && body[i] != '"') {
                if (body[i] == '\\' && i + 1 < body.size()) ++i;
                text += body[i++];
            }
        }
        const size_t u = body.find("\"until\":");
        if (u != std::string::npos) until = atoll(body.c_str() + u + 8);

        std::lock_guard<std::mutex> lk(mtx_);
        mtime_ = m; text_ = text; until_ = until;
    }

    std::mutex  mtx_;
    std::string path_, text_;
    long long   until_ = 0, mtime_ = 0;
};

}  // namespace vibeadmin
