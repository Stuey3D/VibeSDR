// test-decoder-hosts.cpp — PER-LISTENER DECODERS and the box-wide DECODER LIMIT (vibe_decoder_host.h).
//
// ★★★ THE BUG THIS PINS (Pi 500, 2026-09-30, RSP on a locked range): the server ran ONE decoder per
//     RADIO. Listener B starting WEFAX replaced listener A's RTTY, every decoder socket on the radio got
//     WEFAX, and FT8 made 0 decodes in nine minutes on three VFOs because it was fed the WEFAX
//     listener's audio. So this drives three listeners on one multi-VFO radio, each running a
//     DIFFERENT decoder on its OWN audio — RTTY, WEFAX, FT8 — with REAL decoders and REAL signals, and
//     checks each socket receives its own decoder's output and nothing else.
//  ★★ Then the limit: the N+1th decoder is REFUSED with the listener-facing words, a slot frees the
//     moment a decoder stops or its listener goes, and the slots hold ACROSS PROCESSES (every radio is
//     its own process on Linux) and are released by the kernel when a process dies.
//  ★ And the shared dial is unchanged: one host, every decoder socket a mirror of it, one slot.
#include "vibe_decoder_host.h"
#include "vibe_clock.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

/** A decoder socket that remembers everything it was sent. */
struct TestPeer : DecoderPeer {
    std::mutex m;
    std::vector<std::vector<uint8_t>> bin;
    std::vector<std::string> txt;
    std::atomic<bool> isOpen{true};
    bool open() const override { return isOpen.load(); }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    /** All RTTY/time text this socket received (0x01 | u64 ts | u32 len | utf-8). */
    std::string decodedText() {
        std::lock_guard<std::mutex> lk(m); std::string out;
        for (auto& f : bin) if (f.size() >= 13 && f[0] == 0x01) {
            const uint32_t len = (uint32_t(f[9]) << 24) | (uint32_t(f[10]) << 16) | (uint32_t(f[11]) << 8) | f[12];
            if (13 + len == f.size()) out.append((const char*)f.data() + 13, len);
        }
        return out;
    }
    /** WEFAX lines of the given width (0x01 | u32 line | u32 width | px). */
    int wefaxLines(uint32_t width) {
        std::lock_guard<std::mutex> lk(m); int n = 0;
        for (auto& f : bin) if (f.size() == 9 + width && f[0] == 0x01) {
            const uint32_t w = (uint32_t(f[5]) << 24) | (uint32_t(f[6]) << 16) | (uint32_t(f[7]) << 8) | f[8];
            if (w == width) ++n;
        }
        return n;
    }
    int spots(const char* call) {
        std::lock_guard<std::mutex> lk(m); int n = 0;
        for (auto& t : txt) if (t.find("digital_spot") != std::string::npos && t.find(call) != std::string::npos) ++n;
        return n;
    }
    size_t anySpots() { std::lock_guard<std::mutex> lk(m); size_t n = 0; for (auto& t : txt) if (t.find("digital_spot") != std::string::npos) ++n; return n; }
    size_t binCount() { std::lock_guard<std::mutex> lk(m); return bin.size(); }
};

// ── signals ──────────────────────────────────────────────────────────────────────────────────────
/** RTTY, 45.45 baud ITA2 5N1.5, mark/space where FskDecoder puts them for cf 1000 / shift 170. */
static std::vector<float> rtty(const std::string& text, double seconds) {
    const char32_t L[32] = { 0,U'E',U'\n',U'A',U' ',U'S',U'I',U'U',U'\r',U'D',U'R',U'J',U'N',U'F',U'C',U'K',
                             U'T',U'Z',U'L',U'W',U'H',U'Y',U'P',U'Q',U'O',U'B',U'G',0,U'M',U'X',U'V',0 };
    const double fs = 48000, cf = 1000, qv = cf + 4000.0 / cf, mark = qv + 85, space = qv - 85, baud = 45.45;
    std::vector<int> bits;                        // 1 = mark
    auto sendCode = [&](int code) {
        bits.push_back(0);                                         // start
        for (int b = 0; b < 5; b++) bits.push_back((code >> b) & 1);   // LSB first
        bits.push_back(2);                                         // 1.5 stop (2 = a stop and a half)
    };
    for (int i = 0; i < 20; i++) bits.push_back(1);               // idle mark
    sendCode(31);                                                   // LTRS
    for (int rep = 0; rep < 40; ++rep)
        for (char ch : text) {
            int code = -1;
            for (int c = 0; c < 32; c++) if (L[c] == (char32_t)ch) { code = c; break; }
            if (code >= 0) sendCode(code);
        }
    std::vector<float> out((size_t)(fs * seconds));
    double ph = 0; size_t i = 0; const double spb = fs / baud;
    for (int b : bits) {
        const double dur = b == 2 ? 1.5 * spb : spb;
        const double f = (b == 0) ? space : mark;
        for (double t = 0; t < dur && i < out.size(); t += 1.0, ++i) { ph += 2 * M_PI * f / fs; out[i] = 0.4f * (float)std::sin(ph); }
        if (i >= out.size()) break;
    }
    for (; i < out.size(); ++i) { ph += 2 * M_PI * mark / fs; out[i] = 0.4f * (float)std::sin(ph); }
    return out;
}
/** WEFAX: an FM subcarrier on 1900 ± 400 Hz, a slow ramp so there is a picture to draw. */
static std::vector<float> wefax(double seconds) {
    const double fs = 48000;
    std::vector<float> out((size_t)(fs * seconds));
    double ph = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = i / fs, grey = 0.5 + 0.5 * std::sin(2 * M_PI * 3.0 * t);
        ph += 2 * M_PI * (1500.0 + 800.0 * grey) / fs;
        out[i] = 0.4f * (float)std::sin(ph);
    }
    return out;
}
/** One FT8 transmission of `msg` at `f0` Hz, starting 0.5 s into a 15 s slot, plus a little noise. */
static std::vector<float> ft8(const char* msg, double f0) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) { std::printf("  ft8 encode failed\n"); std::exit(2); }
    uint8_t tones[FT8_NN]; ft8_encode(m.payload, tones);
    const double fs = 48000, sym = FT8_SYMBOL_PERIOD;
    std::vector<float> out((size_t)(fs * 15.0));
    std::mt19937 rng(7); std::normal_distribution<float> n(0.0f, 0.01f);
    for (auto& v : out) v = n(rng);
    double ph = 0; const size_t start = (size_t)(0.5 * fs);
    for (int s = 0; s < FT8_NN; ++s)
        for (int k = 0; k < (int)(sym * fs); ++k) {
            const size_t i = start + (size_t)s * (size_t)(sym * fs) + (size_t)k;
            if (i >= out.size()) break;
            ph += 2 * M_PI * (f0 + tones[s] * 6.25) / fs;
            out[i] += 0.3f * (float)std::sin(ph);
        }
    return out;
}
/** Feed a listener's audio faster than real time — but PACED on the host's queue, because the host
 *  (rightly) drops audio it is 4 s behind on, and a test that outruns it measures the drop. */
/** ★ `clock` (optional): the capture UTC of v[0], advanced past v — FT8 cuts its slots by capture time, so audio
 *  fed faster than real time carries the times it WOULD have been heard at. */
static void feedAll(DecoderRouter& r, const std::string& s, const std::vector<float>& v, double* clock = nullptr) {
    auto h = r.find(s);
    for (size_t o = 0; o < v.size(); o += 960) {
        while (h && h->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int n = (int)std::min<size_t>(960, v.size() - o);
        r.feedSession(s, v.data() + o, n, 1, clock ? *clock + (double)(o + (size_t)n) / 48000.0 : NAN);
    }
    if (clock) *clock += (double)v.size() / 48000.0;
}
template <class F> static bool waitFor(F f, double secs) {
    for (int i = 0; i < (int)(secs * 50); ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}

int main() {
    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vibe-decslots-" + std::to_string(getpid());
    std::string mk = "mkdir -p '" + dir + "'"; if (std::system(mk.c_str()) != 0) return 2;
    std::atomic<int> maxN{3};
    FileSlots slots(dir, [&] { return maxN.load(); });
    std::map<std::string, double> dial = { { "A", 14080000 }, { "B", 8040000 }, { "C", 14074000 }, { "D", 7040000 } };
    DecoderRouter router([&](const std::string& s) {
        DecoderHost::Env e; e.slots = &slots;
        e.dialHz = [&dial, s] { return dial.count(s) ? dial[s] : 0.0; };
        return e; });

    std::printf("── three listeners, one multi-VFO radio, three different decoders ──\n");
    auto pA = std::make_shared<TestPeer>(), pB = std::make_shared<TestPeer>(), pC = std::make_shared<TestPeer>();
    auto hA = router.hostFor("A", true), hB = router.hostFor("B", true), hC = router.hostFor("C", true);
    ok(hA != hB && hB != hC && hA != hC, "each listener on a per-VFO radio gets their OWN host");
    hA->addPeer(pA); hB->addPeer(pB); hC->addPeer(pC);
    ok(hA->start("fsk", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"center_frequency\":1000,\"shift\":170,\"baud_rate\":45.45,\"framing\":\"5N1.5\",\"encoding\":\"ITA2\"}") == DecoderHost::Start::Ok, "A starts RTTY");
    const std::string wefaxMsg = "{\"type\":\"audio_extension_attach\",\"extension_name\":\"wefax\",\"lpm\":120,\"image_width\":1809,\"carrier\":1900,\"deviation\":400,\"use_phasing\":false}";
    ok(hB->start("wefax", wefaxMsg) == DecoderHost::Start::Ok, "B starts WEFAX — and A's RTTY is NOT replaced");
    ok(hA->name() == "fsk", "A is still running RTTY after B started WEFAX");
    ok(hC->startSpots() == DecoderHost::Start::Ok, "C starts FT8/FT4 spots");
    ok(slots.inUse() == 3, "three slots in use (" + std::to_string(slots.inUse()) + ")");

    std::printf("── the limit ──\n");
    auto pD = std::make_shared<TestPeer>();
    auto hD = router.hostFor("D", true); hD->addPeer(pD);
    ok(hD->start("fsk", "{}") == DecoderHost::Start::Refused, "a 4th decoder on a 3-slot box is REFUSED");
    ok(hD->startSpots() == DecoderHost::Start::Refused, "...and so are a 4th listener's spots");
    ok(decoderLimitMessage(3) == "All 3 decoder slots on this server are in use \xe2\x80\x94 try again shortly.", "the refusal says how many slots and what to do");
    ok(decoderLimitMessage(1).find("1 decoder slot on this server is in use") != std::string::npos, "...and reads right for one slot");
    ok(hA->name() == "fsk" && hB->name() == "wefax" && hC->spotsOn(), "a refusal disturbs nobody already decoding");

    std::printf("── each listener's audio, each listener's output ──\n");
    // ★ FT8 cuts UTC slots by CAPTURE time (2026-10-05): C's audio is stamped as if heard from a slot boundary
    //   on, then fed faster than real time.
    double cClock = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
    feedAll(router, "A", rtty("RYRY THE QUICK BROWN FOX ", 8.0));
    feedAll(router, "B", wefax(4.0));
    feedAll(router, "C", ft8("CQ G4ABC IO92", 1200.0), &cClock);
    feedAll(router, "C", std::vector<float>(48000 * 1, 0.0f), &cClock);     // a little of the next slot
    ok(waitFor([&] { return pA->decodedText().find("QUICK") != std::string::npos; }, 20), "A's socket decodes A's RTTY (\"" + pA->decodedText().substr(0, 40) + "\")");
    ok(waitFor([&] { return pB->wefaxLines(1809) >= 3; }, 20), "B's socket draws B's WEFAX lines (" + std::to_string(pB->wefaxLines(1809)) + ")");
    ok(waitFor([&] { return pC->spots("G4ABC") >= 1; }, 30), "C's socket gets C's FT8 decode of G4ABC");
    {
        double f = 0; { std::lock_guard<std::mutex> lk(pC->m);
          for (auto& t : pC->txt) { auto at = t.find("\"frequency\":"); if (at != std::string::npos) { f = std::atof(t.c_str() + at + 12); break; } } }
        ok(std::fabs(f - 14075200.0) < 10.0, "...at C's OWN dial + audio offset (" + std::to_string((long long)f) + " Hz, want 14075200)");
    }
    ok(pA->wefaxLines(1809) == 0 && pA->anySpots() == 0, "A received no WEFAX and no spots");
    ok(pB->decodedText().empty() && pB->anySpots() == 0, "B received no RTTY text and no spots");
    ok(pC->binCount() == 0 && pC->decodedText().empty(), "C received no RTTY and no WEFAX");
    ok(pD->binCount() == 0 && pD->anySpots() == 0, "the refused listener received nothing at all");

    std::printf("── slots free at once ──\n");
    hC->stopSpots();
    ok(hD->start("fsk", "{}") == DecoderHost::Start::Ok, "C closing FT8 frees a slot and D's RTTY starts at once");
    ok(slots.inUse() == 3, "still three in use");
    auto pE = std::make_shared<TestPeer>();
    auto hE = router.hostFor("E", true); hE->addPeer(pE);
    ok(hE->startSpots() == DecoderHost::Start::Refused, "full again: E is refused");
    router.drop("A");
    ok(hE->startSpots() == DecoderHost::Start::Ok, "listener A LEAVING frees its slot for E");
    ok(hB->start("sstv", "{}") == DecoderHost::Start::Ok && slots.inUse() == 3, "switching B from WEFAX to SSTV keeps its one slot");
    maxN.store(2);
    ok(slots.inUse() == 3 && hB->name() == "sstv", "lowering the limit stops nobody who is already decoding");
    hB->stop();
    ok(hB->start("fsk", "{}") == DecoderHost::Start::Refused, "...but a freed slot above the new limit is not handed out again");
    maxN.store(3);

    std::printf("── orphans ──\n");
    router.reapOrphans([](const std::string& s) { return s != "E"; }, 100.0, 20.0);
    ok(hE->spotsOn(), "a listener gone for a moment keeps their decoder (a reconnect is not a departure)");
    router.reapOrphans([](const std::string& s) { return s != "E"; }, 121.0, 20.0);
    ok(!hE->spotsOn(), "a listener gone past the grace keeps no slot");

    std::printf("── shared dial: one host, everybody a mirror ──\n");
    router.shutdownAll();
    auto s1 = router.hostFor("X", false), s2 = router.hostFor("Y", false);
    ok(s1 && s1 == s2, "on a shared dial every session gets the ONE host");
    auto m1 = std::make_shared<TestPeer>(), m2 = std::make_shared<TestPeer>();
    ok(!s1->addPeer(m1) && s1->addPeer(m2), "the second decoder socket is a mirror");
    ok(s1->start("wefax", wefaxMsg) == DecoderHost::Start::Ok, "WEFAX starts on the shared dial");
    { auto w = wefax(3.0); for (size_t o = 0; o < w.size(); o += 960) {
        while (s1->queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        router.feedShared(w.data() + o, 960, 1); } }
    ok(waitFor([&] { return m1->wefaxLines(1809) >= 2 && m2->wefaxLines(1809) >= 2; }, 20), "both mirrors see the same lines");
    auto m3 = std::make_shared<TestPeer>(); s1->addPeer(m3);
    ok(s1->start("wefax", "{\"extension_name\":\"wefax\",\"image_width\":1809,\"lpm\":120,\"carrier\":1900,\"deviation\":400,\"use_phasing\":false,\"type\":\"audio_extension_attach\"}", m3) == DecoderHost::Start::Joined,
       "a listener opening the SAME decoder joins it (field order does not matter) rather than restarting the chart");
    ok(waitFor([&] { return m3->wefaxLines(1809) >= 2; }, 5), "...and is caught up on the picture so far (" + std::to_string(m3->wefaxLines(1809)) + " lines)");
    ok(slots.inUse() == 1, "a shared decoder is ONE slot however many watch it");
    ok(s1->start("fsk", "{}") == DecoderHost::Start::Ok && s1->name() == "fsk" && slots.inUse() == 1,
       "on a shared dial a different decoder replaces it, on the same slot — unchanged behaviour");

    std::printf("── drop, never wait ──\n");
    {
        auto big = std::vector<float>(48000 * 12, 0.1f);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t o = 0; o < big.size(); o += 48000) router.feedShared(big.data() + o, 48000, 1);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(ms < 200.0, "handing 12 s of audio to a busy decoder returns at once (" + std::to_string((int)ms) + " ms)");
    }
    router.shutdownAll();
    ok(slots.inUse() == 0, "shutting the radio down releases every slot");

    std::printf("── across processes (one radio = one process on Linux) ──\n");
    {
        maxN.store(2);
        const int held = slots.claim();
        int toChild[2], toParent[2];
        if (pipe(toChild) != 0 || pipe(toParent) != 0) return 2;
        const pid_t pid = fork();
        if (pid == 0) {
            FileSlots mine(dir, [] { return 2; });
            const int a = mine.claim(), b = mine.claim();
            char r = (a >= 0 && b < 0) ? 'y' : 'n';
            (void)!write(toParent[1], &r, 1);
            char go; (void)!read(toChild[0], &go, 1);
            _exit(0);                                  // ★ no release: the KERNEL must free it
        }
        char r = 0; (void)!read(toParent[0], &r, 1);
        ok(r == 'y', "another process gets the one slot left, then is refused");
        ok(slots.inUse() == 2 && slots.claim() < 0, "this process sees the box full");
        char go = 1; (void)!write(toChild[1], &go, 1);
        int st = 0; waitpid(pid, &st, 0);
        const int again = slots.claim();
        ok(again >= 0, "the other process EXITING (no release) frees its slot");
        slots.release(again); slots.release(held);
    }
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
