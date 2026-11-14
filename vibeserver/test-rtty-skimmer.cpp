// test-rtty-skimmer.cpp — the RTTY skimmer (decoders/rtty_skimmer.h), measured on synthetic IQ whose every character is
// known: ITA2 with LTRS/FIGS, a start bit (space), five data bits least significant first, 1.5 or 1 stop bits (mark),
// phase-continuous FSK; noise is complex Gaussian, SNR given in 2.5 kHz.
//
// ★★★ WHAT IS PINNED:
//   • A WINDOW OF MIXED SETTINGS — 45.45 baud / 170 Hz normal, 45.45 / 170 reverse, 50 / 450, 75 / 850 — every signal
//     found at its centre with its shift, its OWN settings found, its text decoded and its callsign spotted;
//   • listed ("waiting for parameter detection") BEFORE its settings are found, and the settings written into the text;
//   • ZERO from what is not RTTY: two CW carriers 170 Hz apart, two PSK31 carriers 170 Hz apart, two steady carriers,
//     none listed and none with text; and NOISE with the alternation test forced open (every pair listed) — the text
//     gate alone keeps it silent;
//   • the queue run by 1 worker and by 4 (threads calling workOnce) settles all four alike;
//   • the cost: ms per second of radio for 24 and 96 kHz windows with 0 / 10 / 30 signals, searching and settled.
// ★ Silent: nothing is played. Deterministic signals: fixed seeds.
#include "decoders/rtty_skimmer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf = std::complex<float>;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct RSig { double hz = 0, shift = 170, baud = 45.45; bool rev = false, oneStop = false; double snrDb = 15; std::string text; double startSec = 1, endSec = 1e9; };
struct Carrier { double hz = 0; int kind = 0; double snrDb = 15; };   // kind 0 steady, 1 CW keyed, 2 PSK31

/** The bits of a message: mark idle, then each character's frame (start, 5 data LSB first, stop); half-bit units so
 *  1.5 stop bits are whole. LTRS first, FIGS/LTRS as the characters need. */
static std::vector<uint8_t> halfBits(const RSig& s) {
    std::vector<uint8_t> h;
    auto frame = [&](int code) {
        h.push_back(0); h.push_back(0);
        for (int b = 0; b < 5; ++b) { const uint8_t v = (code >> b) & 1; h.push_back(v); h.push_back(v); }
        for (int k = 0; k < (s.oneStop ? 2 : 3); ++k) h.push_back(1);
    };
    for (int k = 0; k < 20; ++k) h.push_back(1);   // idle mark
    bool figs = false; frame(31);                  // LTRS
    for (char c : s.text) {
        bool f = figs;
        const int code = rtty::ita2Code(c, f);
        if (code < 0) continue;
        if (f != figs) { frame(f ? 27 : 31); figs = f; }
        frame(code);
    }
    for (int k = 0; k < 20; ++k) h.push_back(1);
    return h;
}

static std::vector<cf> synth(const std::vector<RSig>& sigs, const std::vector<Carrier>& cars, double rate, double secs, uint32_t seed, bool noise = true) {
    const size_t n = (size_t)(rate * secs);
    std::vector<cf> x(n, cf(0, 0));
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.0f, std::sqrt(0.5f));
    if (noise) for (auto& v : x) v = cf(g(rng), g(rng));
    for (const RSig& s : sigs) {
        const double A = std::sqrt(std::pow(10.0, s.snrDb / 10) * 2500.0 / rate);
        const auto hb = halfBits(s);
        double ph = 0;
        const size_t a = (size_t)(s.startSec * rate);
        const double hbSec = 1.0 / (2 * s.baud);
        double t = 0; int rep = 0;
        const size_t e = (size_t)std::min<double>((double)n, s.endSec * rate);
        for (size_t i = a; i < e; ++i) {
            const int hi = (int)(t / hbSec);
            if (hi >= (int)hb.size()) { if (++rep > 1000) break; t -= hb.size() * hbSec; continue; }   // the message again
            const bool mark = hb[(size_t)hi] != 0;
            const double f = s.hz + ((mark != s.rev) ? s.shift / 2 : -s.shift / 2);
            ph += 2 * M_PI * f / rate; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            x[i] += cf((float)(A * std::cos(ph)), (float)(A * std::sin(ph)));
            t += 1.0 / rate;
        }
    }
    std::uniform_real_distribution<double> u(0, 1);
    for (const Carrier& c : cars) {
        const double A = std::sqrt(std::pow(10.0, c.snrDb / 10) * 2500.0 / rate);
        double ph = u(rng) * 6; bool on = true; double next = 0.06, sym = 0; int sign = 1; double prevSign = 1;
        for (size_t i = 0; i < n; ++i) {
            const double t = i / rate;
            double amp = 1;
            if (c.kind == 1) {   // CW: random dots/dashes/gaps at ≈ 20 wpm
                if (t >= next) { on = !on; next = t + 0.06 * (on ? (u(rng) < 0.5 ? 1 : 3) : (u(rng) < 0.6 ? 1 : 3)); }
                amp = on ? 1 : 0;
            } else if (c.kind == 2) {   // PSK31: a reversal (cosine through zero) on a random half of the symbols
                const double ts = t * 31.25;
                if (ts >= sym + 1) { sym = std::floor(ts); prevSign = sign; if (u(rng) < 0.5) sign = -sign; }
                const double fr = ts - sym;
                amp = prevSign == sign ? sign : sign * -std::cos(M_PI * fr);
            }
            ph += 2 * M_PI * c.hz / rate;
            x[i] += cf((float)(A * amp * std::cos(ph)), (float)(A * amp * std::sin(ph)));
        }
    }
    return x;
}

struct Run {
    std::map<int, std::string> text; std::map<int, double> hz; std::vector<rtty::Spot> spots;
};
static rtty::Skimmer::Config cfgFor(double rate, int workers = 0) {
    rtty::Skimmer::Config c; c.rate = rate; c.loHz = -rate * 0.47; c.hiHz = rate * 0.47; c.workers = workers; c.maxSignals = 64;
    return c;
}
/** Feed in 1024-sample blocks; `workers` threads call workOnce() while it runs (the server's shape). */
static Run feed(rtty::Skimmer& sk, const std::vector<cf>& x, int workers) {
    Run r;
    sk.onText = [&](int id, double hz, const std::string& t) { r.text[id] += t; r.hz[id] = hz; };
    sk.onSpot = [&](const rtty::Spot& s) { r.spots.push_back(s); };
    std::mutex m; std::condition_variable cv; bool stop = false; int wake = 0;
    sk.onWork = [&] { { std::lock_guard<std::mutex> lk(m); ++wake; } cv.notify_one(); };
    std::vector<std::thread> th;
    for (int w = 0; w < workers; ++w) th.emplace_back([&] {
        for (;;) {
            { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return stop || wake > 0; }); if (stop) return; if (wake > 0) --wake; }
            while (sk.workOnce()) {}
        }
    });
    for (size_t i = 0; i < x.size(); i += 1024) {
        sk.feedIq(x.data() + i, (int)std::min<size_t>(1024, x.size() - i));
        // ★ a feeder faster than real time: give the workers a moment when jobs are waiting, as the radio's pace would
        if (workers > 0 && sk.queuedJobs() > 0 && (i / 1024) % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (workers > 0) {
        while (sk.queuedJobs() > 0 || sk.runningJobs() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    { std::lock_guard<std::mutex> lk(m); stop = true; } cv.notify_all();
    for (auto& t : th) t.join();
    return r;
}
static const rtty::SignalView* nearest(const std::vector<rtty::SignalView>& v, double hz, double tol) {
    const rtty::SignalView* b = nullptr;
    for (auto& s : v) if (std::fabs(s.offsetHz - hz) < tol && (!b || std::fabs(s.offsetHz - hz) < std::fabs(b->offsetHz - hz))) b = &s;
    return b;
}
static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

static std::vector<RSig> mixedFour() {
    std::vector<RSig> v(4);
    v[0].hz = -9000; v[0].shift = 170; v[0].baud = 45.45; v[0].rev = false; v[0].startSec = 1.0;
    v[0].text = "CQ CQ CQ DE G4ABC G4ABC G4ABC PSE K\nCQ TEST G4ABC G4ABC TEST\n";
    v[1].hz = -3000; v[1].shift = 170; v[1].baud = 45.45; v[1].rev = true; v[1].startSec = 1.5;
    v[1].text = "CQ TEST DL1XYZ DL1XYZ TEST\nDL1XYZ 599 001 001 TU\n";
    v[2].hz = 3000; v[2].shift = 450; v[2].baud = 50; v[2].rev = false; v[2].startSec = 2.0;
    v[2].text = "ZCZC FREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ DE DDK2 DDK2\n";
    v[3].hz = 10000; v[3].shift = 850; v[3].baud = 75; v[3].rev = false; v[3].startSec = 1.2;
    v[3].text = "CQ CQ DE PA3XYZ PA3XYZ PSE K\n02A 04B 06A 08B 12X 17B 22X 26Y\n";
    return v;
}

static void mixedWindow(int workers, bool full) {
    const double rate = 48000;
    auto sigs = mixedFour();
    std::vector<Carrier> cars = {
        { -15000, 1, 18 }, { -14830, 1, 18 },     // two CW carriers 170 Hz apart
        { 15000, 2, 18 }, { 15170, 2, 18 },       // two PSK31 carriers 170 Hz apart
        { 18000, 0, 18 }, { 18170, 0, 18 },       // two steady carriers
    };
    const auto x = synth(sigs, cars, rate, 40, 7);
    rtty::Skimmer sk(cfgFor(rate, workers));
    const Run r = feed(sk, x, workers);
    // one more quiet hop so any result posted at the very end is applied
    { std::vector<cf> z((size_t)sk.fftSize(), cf(0, 0)); sk.feedIq(z.data(), (int)z.size()); }
    const auto v = sk.signals(true);
    char b[200];
    std::printf("  — %d worker%s: %d listed, %d set, %llu pairs rejected, %llu not RTTY, %llu jobs, %llu chars held back\n",
                workers, workers == 1 ? "" : "s", sk.listed(), sk.settled(), (unsigned long long)sk.candidatesRejected(),
                (unsigned long long)sk.notRtty(), (unsigned long long)sk.jobsRun(), (unsigned long long)sk.charsSuppressed());
    int setRight = 0;
    for (const RSig& s : sigs) {
        const rtty::SignalView* f = nearest(v, s.hz, 40);
        std::snprintf(b, sizeof b, "%+.0f Hz (%.2f baud / %.0f Hz / %s)", s.hz, s.baud, s.shift, s.rev ? "Reverse" : "Normal");
        if (!f) { ok(false, std::string(b) + ": found"); continue; }
        const bool right = f->set && std::fabs(f->baud - s.baud) < 1 && f->shiftHz == s.shift && f->reverse == s.rev;
        setRight += right;
        if (!full) continue;
        char m[160]; std::snprintf(m, sizeof m, ": found at %+.1f Hz, shift %.0f; set to [%s]", f->offsetHz, f->shiftHz, f->settings.c_str());
        ok(std::fabs(f->offsetHz - s.hz) < 12 && f->shiftHz == s.shift, std::string(b) + m);
        ok(right, std::string(b) + ": its own settings");
        // the text: every word of the message heard, the settings line inside it
        const std::string t = upper(f->text);
        const std::string want = s.text.substr(0, s.text.find('\n'));
        ok(t.find(want) != std::string::npos, std::string(b) + ": text \"" + want + "\" decoded");
        const std::string line = "[" + f->settings + "]";
        ok(f->text.find(line) != std::string::npos, std::string(b) + ": the settings are written into the text (" + line + ")");
        double ls = -1, ss = -1; sk.timesOf(f->id, ls, ss);
        std::snprintf(m, sizeof m, ": listed at %.2f s, set at %.2f s", ls, ss);
        ok(ls >= 0 && ss > ls, std::string(b) + m + " — listed BEFORE its settings were found");
    }
    if (!full) { ok(setRight == 4, std::to_string(workers) + " worker(s): all four signals set with their own settings (" + std::to_string(setRight) + "/4)"); return; }
    // callsigns
    auto spotted = [&](const std::string& c) { for (auto& s : r.spots) if (s.call == c) return true; return false; };
    ok(spotted("G4ABC") && spotted("DL1XYZ") && spotted("PA3XYZ"), "callsigns spotted from CQ / DE / TEST: G4ABC, DL1XYZ, PA3XYZ");
    bool invented = false; for (auto& s : r.spots) invented |= s.call != "G4ABC" && s.call != "DL1XYZ" && s.call != "PA3XYZ" && s.call != "DDK2";
    ok(!invented, "no callsign invented");
    // nothing from what is not RTTY
    int fromCarriers = 0; size_t charsCarriers = 0;
    for (auto& s : v) for (const Carrier& c : cars) if (std::fabs(s.offsetHz - c.hz) < 200) { ++fromCarriers; charsCarriers += s.chars; break; }
    ok(fromCarriers == 0 && charsCarriers == 0, "CW pair, PSK31 pair, steady pair (each 170 Hz apart): " + std::to_string(fromCarriers) + " listed, "
       + std::to_string(charsCarriers) + " characters");
    ok((int)v.size() == 4, "exactly the four RTTY signals listed (" + std::to_string(v.size()) + ")");
}

static void noiseForcedOpen() {
    const double rate = 24000;
    const auto x = synth({}, {}, rate, 60, 11);
    auto c = cfgFor(rate); c.alternation = -2; c.candidateDb = 1.5;
    rtty::Skimmer sk(c);
    const Run r = feed(sk, x, 0);
    size_t shown = 0; for (auto& s : sk.signals(true)) shown += s.chars;
    char b[200];
    std::snprintf(b, sizeof b, "NOISE, 60 s, the alternation test forced open: %llu jobs, %llu set, %llu not RTTY — %zu characters shown (%llu held back)",
                  (unsigned long long)sk.jobsRun(), (unsigned long long)(sk.jobsRun() - sk.notRtty()), (unsigned long long)sk.notRtty(), shown,
                  (unsigned long long)sk.charsSuppressed());
    ok(shown == 0, b);
    // and plainly: nothing listed on noise
    rtty::Skimmer sk2(cfgFor(rate));
    feed(sk2, x, 0);
    ok(sk2.signals(true).empty(), "NOISE, 60 s, as configured: nothing listed (" + std::to_string(sk2.candidatesRejected()) + " pairs tested and rejected)");
}

/** ★ A station that changes its settings: 45/170 normal for 20 s, then 50/170 reverse — searched again and re-set. */
static void changeOfSettings() {
    const double rate = 24000;
    RSig a; a.hz = 2000; a.shift = 170; a.baud = 45.45; a.startSec = 1; a.endSec = 20; a.text = "CQ CQ DE G4ABC G4ABC PSE K\n";
    RSig b = a; b.baud = 50; b.rev = true; b.startSec = 20; b.endSec = 1e9; b.text = "QRZ DE G4ABC G4ABC NOW 50 BAUD REVERSE K\n";
    const auto x = synth({ a, b }, {}, rate, 50, 21);
    rtty::Skimmer sk(cfgFor(rate));
    feed(sk, x, 0);
    const auto v = sk.signals(false);
    const rtty::SignalView* f = nearest(v, 2000, 30);
    ok(f && f->set && f->baud == 50 && f->reverse && sk.requeues() >= 1 && f->text.find("[45 baud / 170 Hz / Normal]") != std::string::npos
       && f->text.find("[50 baud / 170 Hz / Reverse]") != std::string::npos && upper(f->text).find("NOW 50 BAUD REVERSE") != std::string::npos,
       "a station that changes 45/170 normal → 50/170 reverse: searched again (" + std::to_string(sk.requeues()) + " requeue) and both settings in its text"
       + (f ? " — now [" + f->settings + "]" : std::string(" — not found")));
}

/** ★ Weaker signals: the mixed window at lower SNRs (in 2.5 kHz) — how many are found and set right. */
static void weaker() {
    for (double snr : { 6.0, 0.0 }) {
        auto sigs = mixedFour(); for (auto& s : sigs) s.snrDb = snr;
        const auto x = synth(sigs, {}, 48000, 40, 9);
        rtty::Skimmer sk(cfgFor(48000));
        feed(sk, x, 0);
        const auto v = sk.signals(false);
        int found = 0, right = 0;
        for (auto& s : sigs) { const rtty::SignalView* f = nearest(v, s.hz, 40); found += f != nullptr; right += f && f->set && std::fabs(f->baud - s.baud) < 1 && f->shiftHz == s.shift && f->reverse == s.rev; }
        std::printf("  — the mixed window at %+.0f dB SNR (2.5 kHz): %d of 4 listed, %d set right, %zu listed in all\n", snr, found, right, v.size());
        if (snr >= 6) ok(right == 4 && v.size() == 4, "the mixed window at +6 dB SNR: all four found and set right");
        else ok(v.size() <= 4, "the mixed window at 0 dB SNR: nothing invented (" + std::to_string(right) + " of 4 set right)");
    }
}

static void textGate() {
    ok(rtty::readsAsText("CQ CQ CQ DE G4ABC G4ABC G4ABC PSE K"), "text gate: a CQ reads as text");
    ok(rtty::readsAsText("ZCZC FREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ"), "text gate: a weather bulletin reads as text");
    ok(rtty::readsAsText("TEST DL1XYZ 599 001 TU G4ABC 599 002"), "text gate: a contest exchange reads as text");
    ok(!rtty::readsAsText("EXQKVTHZOBPLRNWJUYDMCSGAFI EOTRUWQYP"), "text gate: random letters do not");
    ok(!rtty::readsAsText("-'!:(5\")#&?-'!:3(/ -;8!:)\"#&?-' "), "text gate: a spray of figures does not");
    ok(!rtty::readsAsText("RYRYRYRYRYRYRYRYRYRYRYRYRY"), "text gate: RYRY does not (not words)");
}

static void bench() {
    std::printf("  — the cost (%s): ms of one core per second of radio\n", rtty::Skimmer::simdName());
    for (double rate : { 24000.0, 96000.0 }) {
        for (int nsig : { 0, 10, 30 }) {
            std::vector<RSig> v;
            const double span = rate * 0.84;
            for (int k = 0; k < nsig; ++k) {
                RSig s; s.hz = -span / 2 + (k + 0.5) * span / std::max(1, nsig);
                s.shift = (k % 3 == 2) ? 450 : 170; s.baud = (k % 4 == 3) ? 50 : 45.45; s.rev = k % 2; s.snrDb = 15; s.startSec = 0.5 + 0.1 * k;
                s.text = "CQ CQ DE G" + std::to_string(k % 10) + "ABC G" + std::to_string(k % 10) + "ABC PSE K\n";
                v.push_back(s);
            }
            const double secs = 24;
            const auto x = synth(v, {}, rate, secs, 5);
            rtty::Skimmer sk(cfgFor(rate));
            const size_t half = (size_t)(rate * 12);
            auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < half; i += 1024) sk.feedIq(x.data() + i, (int)std::min<size_t>(1024, half - i));
            auto t1 = std::chrono::steady_clock::now();
            for (size_t i = half; i < x.size(); i += 1024) sk.feedIq(x.data() + i, (int)std::min<size_t>(1024, x.size() - i));
            auto t2 = std::chrono::steady_clock::now();
            const double a = std::chrono::duration<double, std::milli>(t1 - t0).count() / 12.0;
            const double b = std::chrono::duration<double, std::milli>(t2 - t1).count() / (secs - 12);
            std::printf("     %2.0f kHz, %2d signals: first 12 s (finding + searching) %6.2f ms/s (%.2f %% of a core) · next 12 s (settled: %d of %d) %6.2f ms/s (%.2f %%) · N %d, M %d, channel %.1f Hz\n",
                        rate / 1000, nsig, a, a / 10, sk.settled(), nsig, b, b / 10, sk.fftSize(), sk.channelSize(), sk.channelRate());
            if (std::getenv("RS_DEBUG")) for (auto& q : v) std::printf("       TRUE %+8.1f Hz %.0f Hz %.2f %s\n", q.hz, q.shift, q.baud, q.rev ? "R" : "N");
            if (std::getenv("RS_DEBUG")) for (auto& s : sk.signals(false)) {
                const rtty::SignalView* t = nullptr; double bd = 1e9;
                for (auto& q : v) if (std::fabs(q.hz - s.offsetHz) < bd) { bd = std::fabs(q.hz - s.offsetHz); (void)t; }
                std::printf("       %+8.1f Hz %.0f Hz [%s] nearest true %.1f Hz off: %s\n", s.offsetHz, s.shiftHz, s.settings.c_str(), bd, s.text.substr(0, 40).c_str());
            }
            if (nsig > 0) {
                const auto got = sk.signals(false);
                int right = 0;
                for (auto& q : v) { const rtty::SignalView* f = nearest(got, q.hz, 15); right += f && f->set && std::fabs(f->baud - q.baud) < 1 && f->shiftHz == q.shift && f->reverse == q.rev; }
                ok(right == nsig && (int)got.size() == nsig, "bench window " + std::to_string((int)(rate / 1000)) + " kHz, " + std::to_string(nsig)
                   + " signals: " + std::to_string(right) + " found with their own settings, " + std::to_string((int)got.size()) + " listed");
            }
        }
    }
}

int main(int argc, char** argv) {
    const bool benchOnly = argc > 1 && std::string(argv[1]) == "bench";
    std::printf("RTTY skimmer [%s]\n", rtty::Skimmer::simdName());
    if (!benchOnly) {
        {
            rtty::Skimmer sk(cfgFor(31250));
            char b[160]; std::snprintf(b, sizeof b, "an odd input rate (31.25 kHz): channel %.2f Hz (N %d, M %d) — within 0.2 %% of 3000", sk.channelRate(), sk.fftSize(), sk.channelSize());
            ok(std::fabs(sk.channelRate() - 3000) < 6, b);
        }
        textGate();
        mixedWindow(0, true);
        mixedWindow(1, false);
        mixedWindow(4, false);
        noiseForcedOpen();
        changeOfSettings();
        weaker();
    }
    bench();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
