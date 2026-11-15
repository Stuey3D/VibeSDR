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
#include "vibe_decoder_host.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        const int code = rttysk::ita2Code(c, f);
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
    std::map<int, std::string> text; std::map<int, double> hz; std::vector<rttysk::Spot> spots;
};
static rttysk::Skimmer::Config cfgFor(double rate, int workers = 0) {
    rttysk::Skimmer::Config c; c.rate = rate; c.loHz = -rate * 0.47; c.hiHz = rate * 0.47; c.workers = workers; c.maxSignals = 64;
    return c;
}
/** Feed in 1024-sample blocks; `workers` threads call workOnce() while it runs (the server's shape). */
static Run feed(rttysk::Skimmer& sk, const std::vector<cf>& x, int workers) {
    Run r;
    sk.onText = [&](int id, double hz, const std::string& t) { r.text[id] += t; r.hz[id] = hz; };
    sk.onSpot = [&](const rttysk::Spot& s) { r.spots.push_back(s); };
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
static const rttysk::SignalView* nearest(const std::vector<rttysk::SignalView>& v, double hz, double tol) {
    const rttysk::SignalView* b = nullptr;
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
    rttysk::Skimmer sk(cfgFor(rate, workers));
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
        const rttysk::SignalView* f = nearest(v, s.hz, 40);
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
    rttysk::Skimmer sk(c);
    const Run r = feed(sk, x, 0);
    size_t shown = 0; for (auto& s : sk.signals(true)) shown += s.chars;
    char b[200];
    std::snprintf(b, sizeof b, "NOISE, 60 s, the alternation test forced open: %llu jobs, %llu set, %llu not RTTY — %zu characters shown (%llu held back)",
                  (unsigned long long)sk.jobsRun(), (unsigned long long)(sk.jobsRun() - sk.notRtty()), (unsigned long long)sk.notRtty(), shown,
                  (unsigned long long)sk.charsSuppressed());
    ok(shown == 0, b);
    // and plainly: nothing listed on noise
    rttysk::Skimmer sk2(cfgFor(rate));
    feed(sk2, x, 0);
    ok(sk2.signals(true).empty(), "NOISE, 60 s, as configured: nothing listed (" + std::to_string(sk2.candidatesRejected()) + " pairs tested and rejected)");
}

/** ★ A station that changes its settings: 45/170 normal for 20 s, then 50/170 reverse — searched again and re-set. */
static void changeOfSettings() {
    const double rate = 24000;
    RSig a; a.hz = 2000; a.shift = 170; a.baud = 45.45; a.startSec = 1; a.endSec = 20; a.text = "CQ CQ DE G4ABC G4ABC PSE K\n";
    RSig b = a; b.baud = 50; b.rev = true; b.startSec = 20; b.endSec = 1e9; b.text = "QRZ DE G4ABC G4ABC NOW 50 BAUD REVERSE K\n";
    const auto x = synth({ a, b }, {}, rate, 50, 21);
    rttysk::Skimmer sk(cfgFor(rate));
    feed(sk, x, 0);
    const auto v = sk.signals(false);
    const rttysk::SignalView* f = nearest(v, 2000, 30);
    ok(f && f->set && f->baud == 50 && f->reverse && sk.requeues() >= 1 && f->text.find("[45 baud / 170 Hz / Normal]") != std::string::npos
       && f->text.find("[50 baud / 170 Hz / Reverse]") != std::string::npos && upper(f->text).find("NOW 50 BAUD REVERSE") != std::string::npos,
       "a station that changes 45/170 normal → 50/170 reverse: searched again (" + std::to_string(sk.requeues()) + " requeue) and both settings in its text"
       + (f ? " — now [" + f->settings + "]" : std::string(" — not found")));
}

/** ★★ ONE ROW PER STATION (Stuart, 2026-11-14): a station that pauses between overs keeps its row; a call heard again
 *  after it moved joins its row; and two DIFFERENT stations in turn on (nearly) the same frequency stay two rows. */
/** ★★ THE EARLY FIRST TRY (Stuart, 2026-11-15: 45/170 is what everybody uses): a clean 45/170 station is set within
 *  a second of being listed (it was ≈ 2.5 s — the full search waited for 4 s of channel); a 50 baud one is NOT taken
 *  for 45.45 by the early try, and is still set right by the full search. The 40 m capture: median 2.5 → 0.5 s. */
static void earlyFirstTry() {
    const double rate = 24000;
    for (int k = 0; k < 2; ++k) {
        RSig a; a.hz = 2000; a.startSec = 1;
        a.text = "CQ CQ TEST G4ABC G4ABC TEST QRZ? 599 001 001 TU G4ABC TEST\n";
        if (k == 1) { a.baud = 50; a.rev = true; }
        const auto x = synth({ a }, {}, rate, 50, 20);
        rttysk::Skimmer sk(cfgFor(rate)); feed(sk, x, 0);
        const auto v = sk.signals(true);
        const rttysk::SignalView* f = v.empty() ? nullptr : &v[0];
        double ls = -1, ss = -1; if (f) sk.timesOf(f->id, ls, ss);
        char m[160];
        if (k == 0) {
            std::snprintf(m, sizeof m, "early try: a clean 45/170 station set %.2f s after it was listed (≤ 1 s), %s", ss - ls, f ? f->settings.c_str() : "none");
            ok(f && f->set && std::fabs(f->baud - 45.45) < 0.1 && !f->reverse && ls >= 0 && ss - ls <= 1.0, m);
        } else {
            std::snprintf(m, sizeof m, "early try: a 50 baud reverse station is not taken for 45.45 — set %s (%llu early miss)", f ? f->settings.c_str() : "none",
                          (unsigned long long)sk.earlyMisses());
            ok(f && f->set && std::fabs(f->baud - 50) < 0.1 && f->reverse, m);
        }
    }
}

static void oneRowPerStation() {
    const double rate = 24000;
    auto rowsWith = [](const std::vector<rttysk::SignalView>& v, const char* call) {
        int n = 0; for (auto& s : v) n += s.chars > 0 && upper(s.text).find(call) != std::string::npos; return n; };
    {   // a pause of 15 s, back 10 Hz up
        RSig a; a.hz = 2000; a.startSec = 1; a.endSec = 16; a.text = "CQ TEST G4ABC G4ABC TEST\n";
        RSig b = a; b.hz = 2010; b.startSec = 31; b.endSec = 1e9;
        const auto x = synth({ a, b }, {}, rate, 50, 31);
        rttysk::Skimmer sk(cfgFor(rate)); feed(sk, x, 0);
        const auto v = sk.signals(true);
        // ★ searched ONCE: an early 45.45 try that did not set is not a search (it waited for the full one)
        const uint64_t searched = sk.jobsRun() - sk.earlyMisses();
        ok(rowsWith(v, "G4ABC") == 1 && sk.rowsReused() >= 1 && searched == 1,
           "a station back after a 15 s pause (+10 Hz): ONE row (" + std::to_string(rowsWith(v, "G4ABC")) + "), taken back "
           + std::to_string(sk.rowsReused()) + "×, searched " + std::to_string(searched) + "× (not again)");
    }
    {   // the same call 400 Hz away after a pause: the callsign joins the rows
        RSig a; a.hz = 2000; a.startSec = 1; a.endSec = 16; a.text = "CQ TEST G4ABC G4ABC TEST\n";
        RSig b = a; b.hz = 2400; b.startSec = 31; b.endSec = 1e9; b.text = "CQ CQ DE G4ABC G4ABC PSE K\n";
        const auto x = synth({ a, b }, {}, rate, 55, 32);
        rttysk::Skimmer sk(cfgFor(rate)); feed(sk, x, 0);
        const auto v = sk.signals(true);
        int n = 0; const rttysk::SignalView* r = nullptr; for (auto& s : v) if (s.call == "G4ABC" || (s.chars && upper(s.text).find("G4ABC") != std::string::npos)) { ++n; r = &s; }
        ok(n == 1 && sk.rowsMerged() >= 1 && r && upper(r->text).find("CQ TEST G4ABC") != std::string::npos && upper(r->text).find("CQ CQ DE G4ABC") != std::string::npos,
           "the same call back 400 Hz away: ONE row with both overs (" + std::to_string(n) + " rows, " + std::to_string(sk.rowsMerged()) + " merge)");
    }
    {   // a DIFFERENT station 15 Hz from where the first one was, after it stopped
        RSig a; a.hz = 2000; a.startSec = 1; a.endSec = 16; a.text = "CQ TEST G4ABC G4ABC TEST\n";
        RSig b = a; b.hz = 2015; b.startSec = 31; b.endSec = 1e9; b.text = "CQ TEST DL1XYZ DL1XYZ TEST\n";
        const auto x = synth({ a, b }, {}, rate, 55, 33);
        rttysk::Skimmer sk(cfgFor(rate)); feed(sk, x, 0);
        const auto v = sk.signals(true);
        const rttysk::SignalView *ra = nullptr, *rb = nullptr;
        for (auto& s : v) { if (s.call == "G4ABC") ra = &s; if (s.call == "DL1XYZ") rb = &s; }
        ok(ra && rb && ra->id != rb->id && upper(ra->text).find("DL1XYZ") == std::string::npos && upper(rb->text).find("G4ABC") == std::string::npos,
           "a DIFFERENT station 15 Hz away after the first stopped: TWO rows, neither with the other's text (split "
           + std::to_string(sk.rowsSplit()) + "×)");
    }
}

/** ★ Weaker signals: the mixed window at lower SNRs (in 2.5 kHz) — how many are found and set right. */
static void weaker() {
    for (double snr : { 6.0, 0.0 }) {
        auto sigs = mixedFour(); for (auto& s : sigs) s.snrDb = snr;
        const auto x = synth(sigs, {}, 48000, 40, 9);
        rttysk::Skimmer sk(cfgFor(48000));
        feed(sk, x, 0);
        const auto v = sk.signals(false);
        int found = 0, right = 0;
        for (auto& s : sigs) { const rttysk::SignalView* f = nearest(v, s.hz, 40); found += f != nullptr; right += f && f->set && std::fabs(f->baud - s.baud) < 1 && f->shiftHz == s.shift && f->reverse == s.rev; }
        std::printf("  — the mixed window at %+.0f dB SNR (2.5 kHz): %d of 4 listed, %d set right, %zu listed in all\n", snr, found, right, v.size());
        if (snr >= 6) ok(right == 4 && v.size() == 4, "the mixed window at +6 dB SNR: all four found and set right");
        else ok(v.size() <= 4, "the mixed window at 0 dB SNR: nothing invented (" + std::to_string(right) + " of 4 set right)");
    }
}

// ── "rttyskim" through a DecoderHost: the wire the clients read ──
struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::vector<uint8_t>> bin; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string last(const std::string& needle) { std::lock_guard<std::mutex> lk(m); for (size_t i = txt.size(); i-- > 0;) if (txt[i].find(needle) != std::string::npos) return txt[i]; return ""; }
    int count(const std::string& needle) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) n += t.find(needle) != std::string::npos; return n; }
    bool state(uint8_t s) { std::lock_guard<std::mutex> lk(m); for (auto& b : bin) if (b.size() == 2 && b[0] == 0x03 && b[1] == s) return true; return false; }
};
struct SpyTap : DecoderTap {
    std::mutex m; std::vector<std::string> calls; std::vector<double> rf; std::atomic<int> finished{0};
    void cwSpot(const std::string& c, double hz, float, float, const std::string&) override { std::lock_guard<std::mutex> lk(m); calls.push_back(c); rf.push_back(hz); }
    void finish(bool) override { finished++; }
};
static void hostPath() {
    const double rate = 48000, centre = 14080000;
    auto sigs = mixedFour();
    const auto x = synth(sigs, {}, rate, 30, 13);
    auto peer = std::make_shared<Peer>();
    auto tap = std::make_shared<SpyTap>();
    DecoderHost::Env env;
    env.dialHz = [centre] { return centre; };
    env.rttySkimSpotTap = [tap] { return tap; };
    env.rttySkimWorkers = [] { return 2; };
    const int before = DecoderHost::skimmersRunning().load();
    {
        DecoderHost h(env, "t");
        h.addPeer(peer);
        ok(h.start("rttyskim", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"rttyskim\",\"width\":48000}") == DecoderHost::Start::Ok,
           "host: \"rttyskim\" starts");
        ok(h.kind() == "rttyskim" && h.wantsIq() && std::fabs(h.iqWidthHz() - 48000) < 1 && DecoderHost::skimmersRunning().load() == before + 1,
           "host: it is the RTTY skimmer, wants IQ, 48 kHz, counted as a running skimmer");
        const size_t blk = 960;
        for (size_t i = 0; i < x.size(); i += blk) {
            while (h.queuedIq() > 0.5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            h.feedIq(x.data() + i, (int)std::min(blk, x.size() - i), rate, centre);
        }
        for (int i = 0; i < 4000 && h.queuedIq() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // ★ the list goes out at most once a stream second: one more second of input
        for (size_t i = 0; i < (size_t)rate * 2; i += blk) h.feedIq(x.data() + i, (int)blk, rate, centre);
        for (int i = 0; i < 4000 && h.queuedIq() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string list = peer->last("\"rtty_signals\"");
        bool allRf = true;
        for (auto& s : sigs) {
            bool f = false;
            for (size_t p = list.find("\"hz\":"); p != std::string::npos; p = list.find("\"hz\":", p + 5))
                f |= std::fabs(std::atof(list.c_str() + p + 5) - (centre + s.hz)) < 3;
            allRf &= f;
        }
        ok(allRf, "host: rtty_signals lists all four at their RF (centre + offset)");
        ok(list.find("\"settings\":\"45 baud / 170 Hz / Normal\"") != std::string::npos && list.find("\"settings\":\"45 baud / 170 Hz / Reverse\"") != std::string::npos
           && list.find("\"settings\":\"50 baud / 450 Hz / Normal\"") != std::string::npos && list.find("\"settings\":\"75 baud / 850 Hz / Normal\"") != std::string::npos,
           "host: …each with its settings");
        ok(list.find("\"set\":true") != std::string::npos && peer->count("[45 baud / 170 Hz / Reverse] ") > 0, "host: the settings line went out in the row's text (and scrolls away with it)");
        ok(peer->count("\"set\":false") > 0, "host: a list went out with signals still waiting for their parameters (\"set\":false) first");
        ok(peer->state(1) && peer->state(3), "host: state 1 (listed, none set) then 3 (decoding)");
        ok(peer->count("\"rtty_spot\"") > 0 && peer->last("\"rtty_spot\"").find("\"mode\":\"RTTY") != std::string::npos, "host: rtty_spot sent");
        bool tapOk = false; { std::lock_guard<std::mutex> lk(tap->m); for (size_t i = 0; i < tap->calls.size(); ++i) tapOk |= tap->calls[i] == "G4ABC" && std::fabs(tap->rf[i] - (centre - 9000)) < 20; }
        ok(tapOk, "host: the gallery's summary tap hears G4ABC at its RF");
        ok(list.find("\"workers\":2") != std::string::npos, "host: the owner's worker count (2) is used");
        h.stop();
        ok(DecoderHost::skimmersRunning().load() == before && !h.wantsIq(), "host: stopped — no longer counted, no IQ wanted");
    }
    ok(tap->finished.load() >= 1, "host: the summary tap is finished when it stops");
}

static void textGate() {
    ok(rttysk::readsAsText("CQ CQ CQ DE G4ABC G4ABC G4ABC PSE K"), "text gate: a CQ reads as text");
    ok(rttysk::readsAsText("ZCZC FREQUENCIES 4583 KHZ 7646 KHZ 10100.8 KHZ"), "text gate: a weather bulletin reads as text");
    ok(rttysk::readsAsText("TEST DL1XYZ 599 001 TU G4ABC 599 002"), "text gate: a contest exchange reads as text");
    ok(!rttysk::readsAsText("EXQKVTHZOBPLRNWJUYDMCSGAFI EOTRUWQYP"), "text gate: random letters do not");
    ok(!rttysk::readsAsText("-'!:(5\")#&?-'!:3(/ -;8!:)\"#&?-' "), "text gate: a spray of figures does not");
    ok(!rttysk::readsAsText("RYRYRYRYRYRYRYRYRYRYRYRYRY"), "text gate: RYRY does not (not words)");
}

static void bench() {
    std::printf("  — the cost (%s): ms of one core per second of radio\n", rttysk::Skimmer::simdName());
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
            rttysk::Skimmer sk(cfgFor(rate));
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
                const rttysk::SignalView* t = nullptr; double bd = 1e9;
                for (auto& q : v) if (std::fabs(q.hz - s.offsetHz) < bd) { bd = std::fabs(q.hz - s.offsetHz); (void)t; }
                std::printf("       %+8.1f Hz %.0f Hz [%s] nearest true %.1f Hz off: %s\n", s.offsetHz, s.shiftHz, s.settings.c_str(), bd, s.text.substr(0, 40).c_str());
            }
            if (nsig > 0) {
                const auto got = sk.signals(false);
                int right = 0;
                for (auto& q : v) { const rttysk::SignalView* f = nearest(got, q.hz, 15); right += f && f->set && std::fabs(f->baud - q.baud) < 1 && f->shiftHz == q.shift && f->reverse == q.rev; }
                ok(right == nsig && (int)got.size() == nsig, "bench window " + std::to_string((int)(rate / 1000)) + " kHz, " + std::to_string(nsig)
                   + " signals: " + std::to_string(right) + " found with their own settings, " + std::to_string((int)got.size()) + " listed");
            }
        }
    }
}

/** ★ The synthetic HF clip the server/web tests replay (scripts/test-server-rtty-skim.mjs, test-web-rtty-skim.mjs): unsigned
 *  8-bit IQ at 960 kHz centred on 14.080 MHz, the four mixed stations of mixedFour() at their offsets, +15 dB, repeating.
 *  Written as it is made — 40 s at 960 kHz is 38 M samples, never held. */
static int writeClip(const char* out, double rate, double seconds) {
    auto sigs = mixedFour();
    struct Live { std::vector<uint8_t> hb; double hbSec, A, ph = 0, t = 0; const RSig* s; };
    std::vector<Live> L;
    for (const RSig& s : sigs) { Live l; l.hb = halfBits(s); l.hbSec = 1.0 / (2 * s.baud); l.A = std::sqrt(std::pow(10.0, s.snrDb / 10) * 2500.0 / rate); l.s = &s; L.push_back(l); }
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    std::mt19937 rng(77); std::normal_distribution<float> g(0.0f, std::sqrt(0.5f));
    const size_t n = (size_t)(seconds * rate), blk = 65536;
    std::vector<uint8_t> buf(blk * 2);
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    for (size_t i0 = 0; i0 < n; i0 += blk) {
        const size_t k = std::min(blk, n - i0);
        for (size_t j = 0; j < k; ++j) {
            const double t = (double)(i0 + j) / rate;
            double re = g(rng), im = g(rng);
            for (auto& l : L) {
                if (t < l.s->startSec) continue;
                size_t h = (size_t)(l.t / l.hbSec);
                if (h >= l.hb.size()) { l.t -= l.hb.size() * l.hbSec; h = 0; }
                const bool mark = l.hb[h] != 0;
                l.ph += 2 * M_PI * (l.s->hz + ((mark != l.s->rev) ? l.s->shift / 2 : -l.s->shift / 2)) / rate;
                if (l.ph > M_PI) l.ph -= 2 * M_PI; else if (l.ph < -M_PI) l.ph += 2 * M_PI;
                re += l.A * std::cos(l.ph); im += l.A * std::sin(l.ph);
                l.t += 1.0 / rate;
            }
            buf[2 * j] = q(re); buf[2 * j + 1] = q(im);
        }
        std::fwrite(buf.data(), 1, 2 * k, f);
    }
    std::fclose(f);
    std::printf("%s: %.0f s at %.0f Hz, %zu RTTY stations\n", out, seconds, rate, L.size());
    return 0;
}

/** ★ A REAL CAPTURE, replayed (opt-in; the recording is never in the repo): `test-rtty-skimmer wav <file.wav> <centreHz>`
 *  — stereo int16 I (left) / Q (right) at the file's rate, streamed through the engine in 1024-sample blocks with its
 *  jobs inline. Prints what was listed, the settings found, the text and calls, and the CPU per second of radio. Exit 3
 *  (not run) when the file is absent. Nothing here is a pass/fail: it reports what the engine made of the air. */
static int replayWav(const char* path, double centre) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("   not run — %s is not here\n", path); return 3; }
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::printf("not a WAV\n"); std::fclose(f); return 1; }
    uint32_t rate = 0; uint16_t ch = 0, bits = 0; long dataLen = -1;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) { std::vector<uint8_t> b(len); if (std::fread(b.data(), 1, len, f) != len) break; ch = (uint16_t)(b[2] | b[3] << 8); std::memcpy(&rate, &b[4], 4); bits = (uint16_t)(b[14] | b[15] << 8); }
        else if (!std::memcmp(id, "data", 4)) { dataLen = (long)len; break; }
        else std::fseek(f, (long)len + (len & 1), SEEK_CUR);
    }
    if (ch != 2 || bits != 16 || rate == 0 || dataLen < 0) { std::printf("want stereo int16 I/Q (got %u ch, %u bits, %u Hz)\n", ch, bits, rate); std::fclose(f); return 1; }
    rttysk::Skimmer::Config c; c.rate = rate; c.loHz = -0.47 * (double)rate; c.hiHz = 0.47 * (double)rate; c.maxSignals = 64; c.workers = 0;   // ★ (double): -rate on a uint32 wraps
    rttysk::Skimmer sk(c);
    std::vector<rttysk::Spot> spots;
    sk.onSpot = [&](const rttysk::Spot& s) { spots.push_back(s); };
    std::vector<int16_t> raw(2048); std::vector<cf> iq(1024);
    double cpu = 0; size_t total = 0; double nextReport = 30;
    std::printf("replay %s: %u Hz, centre %.3f kHz, N %d (%.2f Hz bins), channel %.1f Hz, %s\n", path, rate, centre / 1e3,
                sk.fftSize(), sk.binHz(), sk.channelRate(), rttysk::Skimmer::simdName());
    for (;;) {
        const size_t got = std::fread(raw.data(), sizeof(int16_t), raw.size(), f) / 2;
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
        const auto t0 = std::chrono::steady_clock::now();
        sk.feedIq(iq.data(), (int)got);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += got;
        if (sk.nowSec() >= nextReport) {
            std::printf("  t=%3.0f s: %d listed, %d set, %llu pairs rejected (not alternating), %llu not RTTY, %llu jobs\n", sk.nowSec(), sk.listed(), sk.settled(),
                        (unsigned long long)sk.candidatesRejected(), (unsigned long long)sk.notRtty(), (unsigned long long)sk.jobsRun());
            nextReport += 30;
        }
    }
    std::fclose(f);
    const double secs = (double)total / rate;
    std::printf("  %.1f s of radio, %.2f ms of one core per second of radio (%.3f %%), jobs inline\n", secs, 1000 * cpu / secs, 100 * cpu / secs);
    std::printf("  hops %llu, channels open at the end %d\n", (unsigned long long)sk.hops(), sk.tracked());
    std::printf("  totals: %llu pairs rejected, %llu not RTTY, %llu jobs, %llu requeues, %llu characters held back by the text gate\n",
                (unsigned long long)sk.candidatesRejected(), (unsigned long long)sk.notRtty(), (unsigned long long)sk.jobsRun(),
                (unsigned long long)sk.requeues(), (unsigned long long)sk.charsSuppressed());
    std::printf("  one row per station: %llu rows taken back after a pause, %llu joined by callsign, %llu split off (another station)\n",
                (unsigned long long)sk.rowsReused(), (unsigned long long)sk.rowsMerged(), (unsigned long long)sk.rowsSplit());
    {   // ★ rows per call: a station with more than one row is a duplicate
        std::map<std::string, int> per; for (const auto& s : sk.signals(true)) if (!s.call.empty()) per[s.call]++;
        std::string dup; for (auto& kv : per) if (kv.second > 1) dup += " " + kv.first + "×" + std::to_string(kv.second);
        std::printf("  calls with more than one row:%s\n", dup.empty() ? " none" : dup.c_str());
    }
    const auto v = sk.signals(true);
    std::printf("  signals listed (active, then gone): %zu\n", v.size());
    for (const auto& s : v) {
        double ls = -1, ss = -1; sk.timesOf(s.id, ls, ss);
        std::string t = s.text; for (auto& ch2 : t) if (ch2 == '\n') ch2 = '|';
        if (t.size() > 220) t = "…" + t.substr(t.size() - 220);
        std::printf("   #%d %9.2f kHz shift %3.0f %s %-28s snr %4.1f dB %-6s listed %.1f s set %.1f s chars %u call %s\n      \"%s\"\n",
                    s.id, (centre + s.offsetHz) / 1e3, s.shiftHz, s.active ? "active" : "gone  ", s.set ? s.settings.c_str() : "(waiting / not set)",
                    s.snrDb, rttysk::qualityWord(s.quality), ls, ss, s.chars, s.call.empty() ? "-" : s.call.c_str(), t.c_str());
    }
    std::printf("  spots: %zu\n", spots.size());
    for (auto& s : spots) std::printf("   %s %.2f kHz %s %s\n", s.call.c_str(), (centre + s.offsetHz) / 1e3, s.how.c_str(), s.mode.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "wav") return replayWav(argv[2], argc > 3 ? std::atof(argv[3]) : 0);
    if (argc > 2 && std::string(argv[1]) == "clip") return writeClip(argv[2], 960000, argc > 3 ? std::atof(argv[3]) : 40);
    const bool benchOnly = argc > 1 && std::string(argv[1]) == "bench";
    std::printf("RTTY skimmer [%s]\n", rttysk::Skimmer::simdName());
    if (!benchOnly) {
        {
            rttysk::Skimmer sk(cfgFor(31250));
            char b[160]; std::snprintf(b, sizeof b, "an odd input rate (31.25 kHz): channel %.2f Hz (N %d, M %d) — within 0.2 %% of 3000", sk.channelRate(), sk.fftSize(), sk.channelSize());
            ok(std::fabs(sk.channelRate() - 3000) < 6, b);
        }
        textGate();
        mixedWindow(0, true);
        mixedWindow(1, false);
        mixedWindow(4, false);
        noiseForcedOpen();
        changeOfSettings();
        earlyFirstTry();
        oneRowPerStation();
        weaker();
        hostPath();
    }
    bench();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
