// ★★★ beta96 "Set up on another device" — vibe_setup_code.h, FIRST-TIME SETUP ONLY. What must hold:
//   1. A code is 8 symbols from the no-look-alike alphabet, and is REFUSED (none issued) while an admin password exists.
//   2. It dies after 5 minutes, on cancel (the owner backed out), once used (single use), and after 5 wrong tries.
//   3. A peer not on the local network is refused WITHOUT counting (a stranger cannot burn the owner's code).
//   4. Typing is forgiving (lower case, ABCD-EFGH, spaces) but a near miss is a miss.
//   5. The session it opens: a random token, renewed by use, gone after 10 idle minutes, on cancel, when the password is
//      set; and never in the state JSON the screen reads.
//   6. Symbols are uniform enough (no symbol hugely favoured over 31 × 4000 draws) — the rejection sampling works.
#include "vibe_setup_code.h"
#include "vibe_android_setup.h"   // ★ the lock-out guard (remote access is the device's switch)
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>

using namespace vibe::setupcode;

static int fails = 0, passes = 0;
static void ok(bool c, const char* what) { if (!c) { std::printf("  FAIL %s\n", what); ++fails; } else ++passes; }

static std::mt19937_64 g_rng(12345);
static uint64_t rnd() { return g_rng(); }
static uint64_t seq = 0;
static uint64_t badRnd() { return UINT64_MAX - (seq++ % 3); }   // the top of the range: must be rejected, never used

int main() {
    const int64_t T0 = 1'000'000;
    {   // 1. shape and the password rule
        SetupCodes s;
        std::string c = s.issue(false, T0, &rnd);
        ok(c.size() == 8, "a code is 8 symbols");
        bool alpha = true; for (char ch : c) if (!std::strchr(kAlphabet, ch)) alpha = false;
        ok(alpha, "every symbol is from the no-look-alike alphabet");
        ok(pretty(c).size() == 9 && pretty(c)[4] == '-', "shown as ABCD-EFGH");
        ok(s.codeLive(T0), "it is live");
        ok(s.issue(true, T0, &rnd).empty(), "REFUSED while an admin password exists");
        ok(!s.codeLive(T0), "and the earlier code is gone with it");
        ok(s.stateJson(T0).find("\"ended\":\"password_exists\"") != std::string::npos, "the screen is told why");
        std::string c2 = s.issue(false, T0, &rnd);
        auto r = s.redeem(c2, true, /*adminPasswordSet*/true, T0 + 10, &rnd);
        ok(!r.ok && r.why == "password_exists", "a password set after the code was shown: the code is refused at use too");
    }
    {   // 2a. five minutes
        SetupCodes s;
        std::string c = s.issue(false, T0, &rnd);
        ok(s.codeLive(T0 + kCodeTtlMs - 1), "alive at 4:59.999");
        ok(!s.codeLive(T0 + kCodeTtlMs), "dead at 5:00");
        auto r = s.redeem(c, true, false, T0 + kCodeTtlMs + 5, &rnd);
        ok(!r.ok && r.why == "expired", "an expired code says expired");
    }
    {   // 2b. back out
        SetupCodes s;
        std::string c = s.issue(false, T0, &rnd);
        s.cancel();
        auto r = s.redeem(c, true, false, T0 + 1000, &rnd);
        ok(!r.ok && r.why == "cancelled", "backing out kills the code");
    }
    {   // 2c. single use + 4. forgiving typing + 5. session
        SetupCodes s;
        std::string c = s.issue(false, T0, &rnd);
        std::string typed = pretty(c);
        for (auto& ch : typed) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        typed = " " + typed + " ";
        auto r = s.redeem(typed, true, false, T0 + 1000, &rnd);
        ok(r.ok && r.session.size() == 32, "lower case with a dash and spaces works, and opens a 128-bit session");
        auto again = s.redeem(c, true, false, T0 + 2000, &rnd);
        ok(!again.ok && again.why == "used", "SINGLE USE: the same code again is refused");
        ok(s.sessionOk(r.session, T0 + 3000), "the session token works");
        ok(!s.sessionOk(r.session.substr(0, 31) + (r.session[31] == 'a' ? "b" : "a"), T0 + 3000), "a near token does not");
        ok(s.sessionOk(r.session, T0 + 3000 + kSessionIdleMs - 1), "renewed by use");
        ok(!s.sessionOk(r.session, T0 + 3000 + 2 * kSessionIdleMs), "gone after ten idle minutes");
        ok(s.stateJson(T0).find(r.session) == std::string::npos, "the token never appears in the screen's state");
    }
    {   // 5b. password set ends the session; cancel ends it
        SetupCodes s;
        auto r = s.redeem(s.issue(false, T0, &rnd), true, false, T0 + 1, &rnd);
        s.setSessionPeer("192.168.1.20");
        ok(s.stateJson(T0 + 2).find("\"session\":true") != std::string::npos
           && s.stateJson(T0 + 2).find("192.168.1.20") != std::string::npos, "the screen sees 'setup in progress on 192.168.1.20'");
        s.markPasswordSet();
        ok(!s.sessionOk(r.session, T0 + 3), "setting the password ends the session");
        ok(s.stateJson(T0 + 3).find("\"passwordSet\":true") != std::string::npos, "and the screen is told");
        ok(s.windowOpen() && s.stateJson(T0 + 3).find("\"inProgress\":true") != std::string::npos,
           "after the password the WINDOW stays open: the laptop carries on as an admin, the TV still says in progress");
        s.finish();
        ok(!s.windowOpen() && s.stateJson(T0 + 4).find("\"finished\":true") != std::string::npos
           && s.stateJson(T0 + 4).find("\"inProgress\":false") != std::string::npos, "Finish closes it: the TV goes back to its status");
        SetupCodes s2;
        auto r2 = s2.redeem(s2.issue(false, T0, &rnd), true, false, T0 + 1, &rnd);
        s2.cancel();
        ok(!s2.sessionOk(r2.session, T0 + 2), "backing out ends a session in progress");
        ok(!s2.windowOpen(), "and no window is left open behind it");
    }
    {   // 2d. wrong tries + 3. LAN only
        SetupCodes s;
        std::string c = s.issue(false, T0, &rnd);
        for (int i = 0; i < 20; ++i) {
            auto r = s.redeem("ZZZZZZZZ", /*lan*/false, false, T0 + i, &rnd);
            if (r.ok || r.why != "not_local") { ok(false, "a non-local peer is refused as not_local"); break; }
        }
        ok(s.codeLive(T0 + 30), "…and twenty of those did not burn the code");
        std::string miss = c; miss[7] = miss[7] == 'A' ? 'B' : 'A';
        for (int i = 1; i <= 4; ++i) {
            auto r = s.redeem(miss, true, false, T0 + 100 + i, &rnd);
            ok(!r.ok && r.why == "wrong" && r.triesLeft == kMaxWrong - i, "a near miss is a miss, and the tries count down");
        }
        auto r5 = s.redeem(miss, true, false, T0 + 200, &rnd);
        ok(!r5.ok && r5.why == "too_many_wrong" && r5.triesLeft == 0, "the fifth wrong try kills the code");
        auto late = s.redeem(c, true, false, T0 + 300, &rnd);
        ok(!late.ok && late.why == "too_many_wrong", "and then even the right code is refused");
        ok(s.stateJson(T0 + 300).find("too_many_wrong") != std::string::npos, "the screen says so");
    }
    {   // 6. uniform + rejection
        SetupCodes s;
        std::map<char, int> n;
        for (int i = 0; i < 4000; ++i) for (char ch : s.issue(false, T0, &rnd)) n[ch]++;
        int lo = 1 << 30, hi = 0; for (auto& kv : n) { lo = std::min(lo, kv.second); hi = std::max(hi, kv.second); }
        ok(n.size() == 31 && lo > 800 && hi < 1300, "all 31 symbols appear, none hugely favoured (32000 draws)");
        // badRnd returns only values at the top of the range two times in three; the code still comes out, unbiased.
        seq = 0;
        int got = 0;
        uint64_t (*mix)() = []() -> uint64_t { return (seq % 3 == 2) ? (seq++, g_rng()) : badRnd(); };
        std::string c = s.issue(false, T0, mix); got = (int)c.size();
        ok(got == 8, "values at the top of the range are thrown away, not folded in");
    }
    {   // 7. THE LOCK-OUT GUARD: a remote save may never change "Remote access to server settings".
        for (const char* body : {R"({"remoteAccess":false})", R"({"remote":false,"radios":[]})", R"({"remoteSetup":true})"}) {
            std::string patch, err; bool restart = false;
            const bool okSave = vibe::androidsetup::settingsPatch(body, patch, restart, err);
            ok(!okSave && err.find("on the device itself") != std::string::npos, "a remote save touching remote access is refused, with the reason");
        }
        for (const char* body : {R"({"factoryReset":true})", R"({"resetAll":1})"}) {
            std::string patch, err; bool restart = false;
            ok(!vibe::androidsetup::settingsPatch(body, patch, restart, err) && err.find("device itself") != std::string::npos,
               "RESET ALL SETTINGS is refused from the remote page");
        }
        std::string patch, err; bool restart = false;
        ok(vibe::androidsetup::settingsPatch(R"({"radios":[]})", patch, restart, err), "an ordinary save still goes through");
    }
    std::printf(fails ? "test-setup-code: %d passed, %d FAILED\n" : "test-setup-code: all %d passed\n", passes, fails);
    return fails ? 1 : 0;
}
