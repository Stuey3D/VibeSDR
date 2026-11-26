// vibe_setup_code.h — ★★★ beta96 "SET UP ON ANOTHER DEVICE" (Stuart, 2026-11-26): FIRST-TIME SETUP ONLY.
//
// A TV is set up with a remote, and typing an admin password with a remote is the worst part of it. So a server with NO
// admin password can show a ONE-TIME SETUP CODE on its screen; the owner opens http://<address>/setup on a laptop, types
// the code, and the first and only step the code opens is SETTING the admin password (typed there, twice). From then on
// the laptop is an ordinary admin on the ordinary /setup page — the password's own nonce + HMAC challenge, lockout and
// all (local_sdr_shim.cpp vsAdminProof). Nothing here is a second kind of admin.
//
// ★★★ THE RULES (Stuart's, and the reasons):
//   • ONLY WHILE NO ADMIN PASSWORD EXISTS. With one, the code is refused here and the option is not drawn in the app
//     (owners with a password use "Remote access to server settings" with it). So the code can never be used to TAKE a
//     server — only to finish setting up one nobody has set up.
//   • The code is crypto-random, 8 characters from an alphabet with no look-alikes (no 0/O, 1/I/L), shown as ABCD-EFGH.
//     ~39 bits; with five wrong tries in five minutes, guessing is hopeless.
//   • It dies after 5 MINUTES, when the owner BACKS OUT of the screen showing it (cancel), once USED (single use), or
//     after MAX_WRONG wrong tries (the screen says so).
//   • LAN only — the caller checks the peer (isPrivateIp and not through the tunnel), the same rule as RAW IQ OUT's
//     "Local only" and the RTL-TCP door.
//   • Compared in constant time; never logged (the caller logs "redeemed"/"wrong", never the code).
//   • The code opens a SESSION whose only power is to set the FIRST admin password. The session lives 10 minutes
//     from its last use (the password step should take one), and ends when the password is set, the screen is left,
//     or it times out. The one-time code is never the admin password and never becomes it.
// ★★ HTTPS is not available on a LAN, so the code and then the new password cross the network in clear. That is why the
//    window is short, the code single-use, and the step first-time only. docs/v12/SERVER-CHOICE.md says so plainly.
//
// ★ Header-only, no dependencies beyond the standard library; the random source and the clock are injected, so
//   vibeserver/test-setup-code.cpp proves every rule with a fake clock.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace vibe {
namespace setupcode {

inline constexpr int64_t kCodeTtlMs    = 5 * 60 * 1000;   // ★ Stuart: five minutes
inline constexpr int64_t kSessionIdleMs = 10 * 60 * 1000; // the password step, from its last use
inline constexpr int     kMaxWrong     = 5;               // then the code is dead and the screen says so
inline constexpr int     kCodeLen      = 8;
/** 31 symbols: no 0 O 1 I L. Upper case only; what is typed is upper-cased and stripped of '-' and spaces. */
inline constexpr const char* kAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

using RandFn = uint64_t (*)();

/** Why the code is not live any more ("" while it is, or before one was ever made). */
enum class End { None, Expired, Cancelled, Used, TooManyWrong, PasswordExists };

inline const char* endWord(End e) {
    switch (e) {
        case End::Expired:        return "expired";
        case End::Cancelled:      return "cancelled";
        case End::Used:           return "used";
        case End::TooManyWrong:   return "too_many_wrong";
        case End::PasswordExists: return "password_exists";
        default:                  return "";
    }
}

/** Strip what people type around a code (spaces, '-'), upper-case it. */
inline std::string normalise(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out += c;
        if (out.size() > 32) break;   // bounded: nothing longer is a code
    }
    return out;
}

inline bool ctEqual(const std::string& a, const std::string& b) {
    // ★ Constant time in the CONTENT; the length is public (always kCodeLen).
    unsigned diff = (unsigned)(a.size() ^ b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

/** ABCD-EFGH, for the screen. */
inline std::string pretty(const std::string& code) {
    return code.size() == 8 ? code.substr(0, 4) + "-" + code.substr(4) : code;
}

class SetupCodes {
public:
    struct Redeem { bool ok = false; std::string session; std::string why; int triesLeft = 0; };

    /** A new code — refused ("") while an admin password exists. Replaces any earlier one. */
    std::string issue(bool adminPasswordSet, int64_t nowMs, RandFn rnd) {
        std::lock_guard<std::mutex> lk(mtx_);
        endSessionLocked();
        if (adminPasswordSet) { code_.clear(); end_ = End::PasswordExists; return ""; }
        std::string c;
        // ★ Rejection sampling: 31 does not divide 2^64, so the top of the range is thrown away — no symbol is likelier.
        const uint64_t lim = UINT64_MAX - (UINT64_MAX % 31);
        while ((int)c.size() < kCodeLen) {
            const uint64_t r = rnd();
            if (r >= lim) continue;
            c += kAlphabet[r % 31];
        }
        code_ = c; expires_ = nowMs + kCodeTtlMs; wrong_ = 0; end_ = End::None; issued_ = true;
        return c;
    }

    /** The owner backed out of the screen: the code AND any session die. */
    void cancel() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!code_.empty()) end_ = End::Cancelled;
        code_.clear();
        endSessionLocked();
    }

    /** A laptop typed `typed`. `lan` = the caller checked the peer is on the local network (and not via the tunnel). */
    Redeem redeem(const std::string& typed, bool lan, bool adminPasswordSet, int64_t nowMs, RandFn rnd) {
        std::lock_guard<std::mutex> lk(mtx_);
        Redeem r;
        if (!lan) { r.why = "not_local"; return r; }          // ★ not counted: a stranger cannot kill the owner's code
        expireLocked(nowMs);
        if (adminPasswordSet) { code_.clear(); end_ = End::PasswordExists; r.why = "password_exists"; return r; }
        if (code_.empty()) { r.why = end_ == End::None ? "no_code" : endWord(end_); return r; }
        if (!ctEqual(normalise(typed), code_)) {
            ++wrong_;
            r.triesLeft = kMaxWrong - wrong_;
            if (wrong_ >= kMaxWrong) { code_.clear(); end_ = End::TooManyWrong; r.why = "too_many_wrong"; r.triesLeft = 0; }
            else r.why = "wrong";
            return r;
        }
        // ★ Single use: the code is gone the moment it works.
        code_.clear(); end_ = End::Used;
        session_ = hex128(rnd); sessionSeen_ = nowMs;
        r.ok = true; r.session = session_;
        return r;
    }

    /** Does `token` hold the live session? A use renews it. Constant time. */
    bool sessionOk(const std::string& token, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked(nowMs);
        if (session_.empty() || token.size() != session_.size() || !ctEqual(token, session_)) return false;
        sessionSeen_ = nowMs;
        return true;
    }

    /** The password was set (or the owner left): the session is over. */
    void endSession() { std::lock_guard<std::mutex> lk(mtx_); endSessionLocked(); }
    void setSessionPeer(const std::string& p) { std::lock_guard<std::mutex> lk(mtx_); if (!session_.empty()) peer_ = p.substr(0, 64); }
    void markPasswordSet() { std::lock_guard<std::mutex> lk(mtx_); passwordSet_ = true; endSessionLocked(); }

    /** For the screen showing the code (JSON). Never contains the session token. */
    std::string stateJson(int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mtx_);
        expireLocked(nowMs);
        std::string j = "{\"active\":";
        j += code_.empty() ? "false" : "true";
        j += ",\"secondsLeft\":" + std::to_string(code_.empty() ? 0 : (int)((expires_ - nowMs + 999) / 1000));
        j += ",\"wrong\":" + std::to_string(wrong_);
        j += ",\"session\":"; j += session_.empty() ? "false" : "true";
        j += ",\"peer\":\"" + jsonSafe(peer_) + "\"";
        j += ",\"ended\":\"" + std::string(endWord(end_)) + "\"";
        j += ",\"passwordSet\":"; j += passwordSet_ ? "true" : "false";
        j += "}";
        return j;
    }

    bool codeLive(int64_t nowMs) { std::lock_guard<std::mutex> lk(mtx_); expireLocked(nowMs); return !code_.empty(); }
    bool sessionLive(int64_t nowMs) { std::lock_guard<std::mutex> lk(mtx_); expireLocked(nowMs); return !session_.empty(); }

private:
    void expireLocked(int64_t nowMs) {
        if (!code_.empty() && nowMs >= expires_) { code_.clear(); end_ = End::Expired; }
        if (!session_.empty() && nowMs - sessionSeen_ >= kSessionIdleMs) endSessionLocked();
    }
    void endSessionLocked() { session_.clear(); peer_.clear(); }
    static std::string hex128(RandFn rnd) {
        static const char* k = "0123456789abcdef";
        std::string out;
        for (int w = 0; w < 2; ++w) { uint64_t v = rnd(); for (int i = 0; i < 16; ++i) { out += k[v & 15]; v >>= 4; } }
        return out;
    }
    static std::string jsonSafe(const std::string& s) {
        std::string o;
        for (char c : s) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) o += c;
        return o;
    }

    std::mutex mtx_;
    std::string code_, session_, peer_;
    int64_t expires_ = 0, sessionSeen_ = 0;
    int wrong_ = 0;
    End end_ = End::None;
    bool issued_ = false, passwordSet_ = false;
};

}  // namespace setupcode
}  // namespace vibe
