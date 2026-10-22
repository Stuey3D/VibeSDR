// test-bg-priority.cpp — BACKGROUND DECODING and the box's decoder SLOTS (Stuart, 2026-10-22): "background decoding gets a
// tier list drag the VFO's into order of priority, then the decoder slots are for real listeners as highest priority so a
// background decode will have to drop off until its free again".
//   · a SINGLE-radio server caps a profile at its decoder slots (refused at the save, in words);
//   · the box's priority list picks who gives way: the lowest-ranked running background VFO on the whole box, a tie to the
//     radio running the most; a dead process counts for nothing;
//   · a listener's claim on a full box gets a slot at once — exactly the lowest VFO pauses, the message it was in the
//     middle of is SAVED (partial), and it resumes when the slot frees, the highest-ranked paused one first;
//   · two radios (two runners, two registries, one slot directory — as two processes see it).
#include "vibe_bgdecode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/bgdec-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}

// ── signals ─────────────────────────────────────────────────────────────────────────────────────────────────────
// NAVTEX (CCIR 476 + SITOR-B FEC), the encoder test-navtex-fec.cpp uses (fldigi's create_fec).
static const char* NLT = "________________" "_______J___F_CK_" "_______W___Y_PQ_" "_____G___MX_V___"
                         "_______A___S_IU_" "___D_RE__N__ ___" "___Z_L___H__\n___" "_OB_T___\r_______";
static const char* NFG = "________________" "_______'___!_:(_" "_______2___6_01_" "_____&___./_;___"
                         "_______-___\x07_87_" "___$_43__,__ ___" "___\"_)___#__\n___" "_9?_5___\r_______";
static const uint8_t NLTRS = 0x5A, NFIGS = 0x36, ALPHA = 0x0F, REP = 0x66, CHAR32 = 0x6A;
static int nfind(const char* t, char c) { for (int i = 0; i < 128; i++) if (t[i] == c && c != '_') return i; return -1; }
static std::vector<uint8_t> navEncode(const std::string& text) {
    std::vector<uint8_t> v{NLTRS}; bool fig = false;
    for (char c : text) {
        const int l = nfind(NLT, c), f = nfind(NFG, c);
        if (c == ' ' || c == '\r' || c == '\n') v.push_back((uint8_t)l);
        else if (l >= 0 && !fig) v.push_back((uint8_t)l);
        else if (f >= 0 && fig) v.push_back((uint8_t)f);
        else if (f >= 0) { v.push_back(NFIGS); v.push_back((uint8_t)f); fig = true; }
        else { v.push_back(NLTRS); v.push_back((uint8_t)l); fig = false; }
    }
    return v;
}
/** The bit stream of a whole NAVTEX transmission: phasing, the message with FEC, phasing after. +1 = mark. */
static std::vector<int> navBits(const std::string& text) {
    const auto c = navEncode(text);
    std::vector<uint8_t> s;
    for (int i = 0; i < 40; i++) { s.push_back(REP); s.push_back(ALPHA); }
    for (size_t i = 0; i < c.size(); i++) { s.push_back(c[i]); s.push_back(i >= 2 ? c[i - 2] : ALPHA); }
    s.push_back(CHAR32); s.push_back(c[c.size() - 2]); s.push_back(CHAR32); s.push_back(c.back());
    for (int i = 0; i < 40; i++) { s.push_back(REP); s.push_back(ALPHA); }
    std::vector<int> b;
    for (uint8_t w : s) for (int i = 0; i < 7; i++) b.push_back((w >> i & 1) ? 1 : -1);
    return b;
}
// ── 8. listeners first, background in its own order (Stuart, 2026-10-22) ────────────────────────────────────────
static int countRunning(const std::string& dir, const std::string& id) {
    int n = 0; for (auto& e : bg::BgRegistry::all(dir)) if (e.id == id && e.running) ++n; return n;
}
static bool runningRank(const std::string& dir, const std::string& id, int rank) {
    for (auto& e : bg::BgRegistry::all(dir)) if (e.id == id && e.rank == rank) return e.running; return false;
}
static bool waitFor(const std::function<bool()>& f, int ms) { for (int t = 0; t < ms; t += 50) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } return f(); }
static void priority() {
    std::printf("── a single-radio server: a profile is capped at the decoder slots ──\n");
    bg::Config c; c.on = true; bg::Profile p; p.centreHz = 500000;
    for (int i = 0; i < 9; i++) p.vfos.push_back(bg::Vfo{ 490000.0 + i * 3000, "usb", "navtex" });
    c.profiles = { p };
    std::string err;
    ok(!bg::validate(c, 768000, err, 8) && err.find("Profile 1 has 9 VFOs") != std::string::npos && err.find("can run 8 decoders at once") != std::string::npos,
       "★ 9 VFOs on a single-radio server with 8 slots: refused, in words: " + err);
    ok(bg::validate(c, 768000, err, 0), "...and allowed on a multi-radio server (background takes what is free)");

    std::printf("── the box's priority list: who gives way ──\n");
    {
        const std::string dir = tmpDir("reg");
        bg::BgRegistry a, b; a.open(dir, "A"); b.open(dir, "B");
        a.publish({ {1, true}, {2, true} });
        b.publish({ {1, true}, {2, true}, {3, false} });
        ok(bg::BgRegistry::victim(dir) == "A" || bg::BgRegistry::victim(dir) == "B", "a tie on rank (2 and 2), equal counts: one of them");
        b.publish({ {1, true}, {2, true}, {3, true} });
        int r = 0;
        ok(bg::BgRegistry::victim(dir, &r) == "B" && r == 3, "★ the lowest-ranked running VFO box-wide: B's rank 3");
        a.publish({ {1, true}, {2, true} }); b.publish({ {1, true}, {2, true}, {3, false} });
        b.publish({ {1, true}, {2, true}, {3, false}, {4, false} });
        a.publish({ {1, true}, {2, false} });
        // tie on rank 2? A's lowest running is 1, B's is 2 → B
        ok(bg::BgRegistry::victim(dir) == "B", "lowest running rank wins over a paused one");
        a.publish({ {2, true} }); b.publish({ {1, true}, {2, true} });
        ok(bg::BgRegistry::victim(dir) == "B", "★ a tie on rank 2: the radio running MORE background decoders gives way (B, 2 vs 1)");
        ok(bg::BgRegistry::bestPaused(dir) == 0, "nobody paused");
        b.close();
        ok(bg::BgRegistry::victim(dir) == "A", "a radio whose process has gone counts for nothing");
    }

    std::printf("── a listener on another radio: the lowest VFO pauses, its message is kept, it comes back ──\n");
    {
        const std::string dir = tmpDir("prio");
        gallery::Store s; s.setDir(tmpDir("prio-gal"));
        FileSlots slotsA(dir, [] { return 3; }), lis(dir, [] { return 3; });
        bg::BgRegistry regA; regA.open(dir, "A");
        const double fs = 250000, centre = 500000;
        bg::BgRunner r;
        bg::BgRunner::Params p;
        p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slotsA; p.store = &s; p.registry = &regA;
        p.profile.centreHz = centre;
        p.profile.vfos = { bg::Vfo{489500, "usb", "navtex"}, bg::Vfo{518500, "lsb", "navtex"}, bg::Vfo{517500, "usb", "navtex"} };
        std::vector<std::string> logs; std::mutex lm;
        p.log = [&](const std::string& t) { std::lock_guard<std::mutex> lk(lm); logs.push_back(t); };
        auto logged = [&](const std::string& k) { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) if (l.find(k) != std::string::npos) return true; return false; };
        r.start(p);
        ok(slotsA.inUse() == 3 && countRunning(dir, "A") == 3, "3 VFOs take all 3 slots");
        // half a NAVTEX message, heard by VFOs 2 and 3
        const auto bits = navBits("ZCZC GB98\r\nGALE WARNING STORM AUSTEN WIND SOUTHWEST SEVERE GALE NINE LATER\r\nNNNN\r\n");
        const double spb = fs / 100.0; std::vector<vibedsp::cf32> iq; double ph = 0, acc = 0; size_t made = 0;
        auto emit = [&](double rf, double samples) {
            acc += samples; const size_t k = (size_t)std::lround(acc);
            for (; made < k; made++) {
                ph += 2 * M_PI * (rf - centre) / fs; if (ph > M_PI) ph -= 2 * M_PI;
                iq.push_back(vibedsp::cf32((float)(0.1 * std::cos(ph)), (float)(0.1 * std::sin(ph))));
                if (iq.size() == 16384) { r.feedIq(iq.data(), (int)iq.size()); iq.clear(); std::this_thread::sleep_for(std::chrono::microseconds(300)); }
            }
        };
        emit(518000 + 85, fs);
        for (size_t i = 0; i < bits.size() * 3 / 4; i++) emit(518000 + (bits[i] > 0 ? 85 : -85), spb);
        if (!iq.empty()) { r.feedIq(iq.data(), (int)iq.size()); iq.clear(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        ok(lis.claim() < 0, "the box is full: a listener's plain claim is refused");
        const auto t0 = std::chrono::steady_clock::now();
        const bool room = bg::BgRegistry::makeRoomForListener(lis, dir);
        const int tok = room ? lis.claim() : -1;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(tok >= 0, "★ ...but asking background to make room gets the listener a slot (in " + std::to_string((int)ms) + " ms)");
        ok(ms < 1500, "...without a user-visible wait");
        ok(runningRank(dir, "A", 1) && runningRank(dir, "A", 2) && !runningRank(dir, "A", 3), "★ exactly the LOWEST-ranked VFO (3) paused; 1 and 2 run on");
        ok(logged("VFO 3 (517.5 kHz NAVTEX) paused \xe2\x80\x94 a listener needed a decoder slot"), "logged: VFO 3 (517.5 kHz NAVTEX) paused — a listener needed a decoder slot");
        const std::string list = s.listJson("navtex", 10);
        ok(list.find("\"hz\":517500") != std::string::npos && list.find("\"partial\":true") != std::string::npos,
           "★ the message VFO 3 was in the middle of is SAVED, marked partial: " + list.substr(0, 160));
        ok(list.find("\"hz\":518500") == std::string::npos, "...VFO 2, still running, has not cut its message short");
        lis.release(tok);
        ok(waitFor([&] { return runningRank(dir, "A", 3); }, 5000), "★ the listener's decoder stops: VFO 3 resumes within 5 s");
        ok(logged("VFO 3 (517.5 kHz NAVTEX) resumed"), "logged: …resumed");
        r.stop();
        ok(slotsA.inUse() == 0, "stopped: every slot back");
    }

    std::printf("── two radios: the lowest rank on the BOX gives way, the highest paused comes back first ──\n");
    {
        const std::string dir = tmpDir("prio2");
        FileSlots sa(dir, [] { return 5; }), sb(dir, [] { return 5; }), lis(dir, [] { return 5; });
        bg::BgRegistry ra, rb; ra.open(dir, "A"); rb.open(dir, "B");
        auto params = [&](FileSlots* sl, bg::BgRegistry* rg, int n) {
            bg::BgRunner::Params p; p.sampleRate = 250000; p.centreHz = 500000; p.fftSize = 8192; p.slots = sl; p.registry = rg;
            p.profile.centreHz = 500000;
            for (int i = 0; i < n; i++) p.profile.vfos.push_back(bg::Vfo{ 490000.0 + i * 5000, "usb", "navtex" });
            return p;
        };
        bg::BgRunner A, B;
        A.start(params(&sa, &ra, 2));
        B.start(params(&sb, &rb, 3));
        ok(sa.inUse() == 5 && countRunning(dir, "A") == 2 && countRunning(dir, "B") == 3, "A runs 2, B runs 3: the box's 5 slots are full");
        int t1 = -1, t2 = -1;
        if (bg::BgRegistry::makeRoomForListener(lis, dir)) t1 = lis.claim();
        ok(t1 >= 0 && !runningRank(dir, "B", 3) && countRunning(dir, "A") == 2, "★ a listener: B's rank 3 — the lowest on the box — pauses; A untouched");
        if (bg::BgRegistry::makeRoomForListener(lis, dir)) t2 = lis.claim();
        ok(t2 >= 0 && countRunning(dir, "A") + countRunning(dir, "B") == 3, "a second listener: one more rank-2 VFO pauses (a tie: either radio)");
        lis.release(t1);
        ok(waitFor([&] { return countRunning(dir, "A") + countRunning(dir, "B") == 4; }, 5000), "a slot frees: one VFO resumes");
        ok(bg::BgRegistry::bestPaused(dir) == 3, "★ ...the rank-2 one first — what still waits is rank 3");
        lis.release(t2);
        ok(waitFor([&] { return countRunning(dir, "A") + countRunning(dir, "B") == 5; }, 5000), "the last slot frees: all 5 run again");
        A.stop(); B.stop();
        ok(lis.inUse() == 0, "stopped: every slot back");
    }
}

// ── in range while a listener is on; a locked radio's user slots ────────────────────────────────────────────────
static void inRange() {
    std::printf("── the radio moves under background decoding: re-cut, not rebuilt; out of the window pauses ──\n");
    const std::string dir = tmpDir("win");
    FileSlots sl(dir, [] { return 8; });
    bg::BgRegistry reg; reg.open(dir, "W");
    std::vector<std::string> logs; std::mutex lm;
    auto logged = [&](const std::string& k) { std::lock_guard<std::mutex> lk(lm); for (auto& l : logs) if (l.find(k) != std::string::npos) return true; return false; };
    std::atomic<int> budget{ 1 << 20 };
    bg::BgRunner r;
    bg::BgRunner::Params p; p.sampleRate = 250000; p.centreHz = 500000; p.fftSize = 8192; p.slots = &sl; p.registry = &reg;
    p.profile.centreHz = 500000;
    p.profile.vfos = { bg::Vfo{489500, "usb", "navtex"}, bg::Vfo{517500, "usb", "navtex"}, bg::Vfo{610000, "usb", "navtex"} };
    p.log = [&](const std::string& t) { std::lock_guard<std::mutex> lk(lm); logs.push_back(t); };
    p.channelBudget = [&] { return budget.load(); };
    p.listenerOn = true;
    r.start(p);
    ok(r.inWindowCount() == 2 && r.decodersRunning() == 2, "centre 500 kHz at 250 kS/s: 489.5 and 517.5 in the window and running; 610 is not");
    ok(logged("(2 inside the radio's window) \xe2\x80\x94 with a listener on"), "...and the log says so");
    r.retarget(650000, 0, true);
    ok(r.inWindowCount() == 1 && r.decodersRunning() == 0, "★ the listener's dial moves the radio to 650 kHz: both leave the window and pause (610 is now in)");
    ok(logged("left the radio's window"), "logged: …paused — left the radio's window");
    r.slotTickForTest();
    ok(waitFor([&] { return r.decodersRunning() == 1; }, 4000), "...and 610, now in the window, starts");
    r.retarget(500000, 0, true);
    ok(r.inWindowCount() == 2, "back to 500 kHz: 489.5 and 517.5 are in the window again");
    ok(waitFor([&] { r.slotTickForTest(); return r.decodersRunning() == 2; }, 6000), "★ ...and resume (one per tick)");
    std::printf("── a locked radio's user slots: background takes only the FREE ones ──\n");
    budget.store(1);
    r.slotTickForTest();
    ok(r.decodersRunning() == 1 && logged("a listener took this radio's last user slot"), "★ budget 1: the lowest VFO gives up its channel, at once");
    budget.store(2);
    ok(waitFor([&] { r.slotTickForTest(); return r.decodersRunning() == 2; }, 6000), "a listener leaves (budget 2): it comes back");
    r.stop();
    ok(sl.inUse() == 0, "stopped: every slot back");
}

int main() {
    inRange();
    priority();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
