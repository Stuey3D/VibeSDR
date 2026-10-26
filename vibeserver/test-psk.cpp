// test-psk.cpp — the PSK31/63 decoder and skimmer (decoders/psk_skimmer.h), measured, and its plumbing (vibe_psk_run.h
// through vibe_decoder_host.h, and the gallery's PSK summary in vibe_bgdecode.h).
//
// ★★★ WHAT IS PINNED, on synthetic PSK whose every character is known (tools/psk-bench/psk_synth.h — the published
//     description: Varicode + "00", a zero is a reversal, cosine-shaped reversals; SNR in 2.5 kHz):
//   • the Varicode alphabet's shape (128 distinct codes, 1-0-free-1 each, as many of each length as there can be) and
//     a round trip of every printable character;
//   • the floor's calibration on noise (every carrier threshold is a multiple of it);
//   • CER: PSK31 at 0 / −8 / −12 dB and PSK63 at −5 dB, audio (the listener) and IQ (the skimmer), mean of draws;
//   • several frequency offsets, a 1 Hz/s drift, a transmitter clock 200 ppm off;
//   • A CROWDED PASSBAND: 12 signals in 3 kHz, PSK31 and PSK63, two of them 31.25 Hz apart — each decoded, each CQ spotted
//     at its own frequency, no call invented;
//   • ZERO TEXT ON NOISE — plainly, and with the carrier finder forced open so channels sit on noise all the time (only
//     the text gate stands between them and the list);
//   • ZERO TEXT FROM WHAT IS NOT PSK: a steady carrier, an FSK signal (FT8-like 8 tones, RTTY-like two);
//   • IMD read off an overdriven transmitter's idle;
//   • "psk" and "pskskim" through a DecoderHost: 0x01 text, the signals list, spots at their RF, the summary tap, the
//     skimmer counted and its width; and the PSK summary card the gallery keeps.
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "vibe_decoder_host.h"
#include "vibe_bgdecode.h"
#include "../tools/psk-bench/psk_synth.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace vibe;
using namespace psksynth;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct Run { std::map<int, std::string> text; std::map<int, double> hz; std::vector<psk::Spot> spots; };
static psk::Skimmer::Config cfgFor(bool iq, double rate) {
    psk::Skimmer::Config c; c.rate = rate; c.complexIn = iq;
    if (iq) { c.loHz = -rate * 0.47; c.hiHz = rate * 0.47; c.maxSignals = 64; }
    return c;
}
static Run run(psk::Skimmer& sk, const std::vector<cf>& iq, const std::vector<float>& re) {
    Run r;
    sk.onText = [&](int id, double hz, const std::string& t) { r.text[id] += t; r.hz[id] = hz; };
    sk.onSpot = [&](const psk::Spot& s) { r.spots.push_back(s); };
    for (size_t i = 0; i < std::max(iq.size(), re.size()); i += 1024) {
        if (!iq.empty()) sk.feedIq(iq.data() + i, (int)std::min<size_t>(1024, iq.size() - i));
        else sk.feedReal(re.data() + i, (int)std::min<size_t>(1024, re.size() - i));
    }
    return r;
}
static std::string near(const Run& r, double hz, double tol) {
    std::string t;
    for (auto& kv : r.text) { auto h = r.hz.find(kv.first); if (h != r.hz.end() && std::fabs(h->second - hz) < tol) t += kv.second; }
    return t;
}
static std::string qso() { std::string t; for (const char* q : kQso) t += q; return t; }
static double cerOf(bool iq, double baud, double snr, double hz, uint64_t seed, double drift = 0, double ppm = 0) {
    const double rate = iq ? 48000 : 12000;
    Rng rng(seed);
    Sig s; s.hz = hz; s.baud = baud; s.snrDb = snr; s.text = qso(); s.startSec = 3.0; s.driftHzPerSec = drift; s.clockPpm = ppm;
    std::vector<Sig> v{ s }; std::vector<cf> x; std::vector<float> r;
    synth(v, rate, iq, duration(s) + 3.0, rng, x, r);
    psk::Skimmer sk(cfgFor(iq, rate));
    const Run res = run(sk, x, r);
    return cer(s.text, near(res, hz + drift * duration(s) / 2, 30 + std::fabs(drift) * duration(s)));
}
static double meanCer(bool iq, double baud, double snr, int draws) {
    double s = 0; for (int k = 0; k < draws; ++k) s += cerOf(iq, baud, snr, iq ? 1700 + 311 * k : 1000 + 37 * k, 21 + k);
    return s / draws;
}
static std::string pct(double x) { char b[32]; std::snprintf(b, sizeof b, "%.1f %%", x * 100); return b; }

struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::vector<uint8_t>> bin; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string decoded() {
        std::lock_guard<std::mutex> lk(m); std::string o;
        for (auto& f : bin) if (f.size() >= 13 && f[0] == 0x01) o.append((const char*)f.data() + 13, f.size() - 13);
        return o;
    }
    int count(const std::string& needle) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) n += t.find(needle) != std::string::npos; return n; }
    std::string last(const std::string& needle) { std::lock_guard<std::mutex> lk(m); for (size_t i = txt.size(); i-- > 0;) if (txt[i].find(needle) != std::string::npos) return txt[i]; return ""; }
};
struct SpyTap : DecoderTap {
    std::mutex m; std::vector<std::string> calls; std::vector<double> rf; std::atomic<int> finished{0};
    void cwSpot(const std::string& c, double hz, float, float, const std::string&) override { std::lock_guard<std::mutex> lk(m); calls.push_back(c); rf.push_back(hz); }
    void finish(bool) override { finished++; }
};
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/psk-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string slurpTxt(const std::string& dir) {
    std::string all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string f = e->d_name;
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) { std::ifstream in(dir + "/" + f); std::stringstream ss; ss << in.rdbuf(); all += ss.str() + "\n"; }
        }
        closedir(d);
    }
    return all;
}

int main() {
    std::printf("PSK31/63 [%s]\n", psk::Skimmer::simdName());
    // ── Varicode ──
    {
        std::set<std::string> seen; std::map<size_t, int> byLen; bool shape = true;
        for (int a = 0; a < 128; ++a) {
            const std::string v = psk::varicode(a);
            seen.insert(v); byLen[v.size()]++;
            shape &= !v.empty() && v.front() == '1' && v.back() == '1' && v.find("00") == std::string::npos;
        }
        ok(seen.size() == 128 && shape, "Varicode: 128 distinct codes, each 1…1 with no 00 inside");
        const int want[] = { 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 40 };
        bool fib = true; for (int L = 1; L <= 10; ++L) fib &= byLen[(size_t)L] == want[L];
        ok(fib, "…as many of each length as there can be (1 1 2 3 5 8 13 21 34, then 40 of ten bits)");
        ok(std::string(psk::varicode(' ')) == "1" && std::string(psk::varicode('e')) == "11" && std::string(psk::varicode('t')) == "101",
           "the commonest get the shortest: space 1, e 11, t 101");
        bool rt = true;
        for (int a = 0; a < 128; ++a) { uint32_t v = 0; for (const char* p = psk::varicode(a); *p; ++p) v = v * 2 + (*p == '1'); rt &= psk::varicodeChar(v) == a; }
        ok(rt && psk::varicodeChar(0x3ff) == -1, "every code decodes back to its character; a non-code to nothing");
    }
    // ── the floor ──
    {
        psk::Skimmer sk(cfgFor(true, 48000));
        Rng rng(3); std::vector<cf> x(48000 * 20); const double sg = std::sqrt(0.5);
        for (auto& v : x) v = cf((float)(sg * rng.gauss()), (float)(sg * rng.gauss()));
        run(sk, x, {});
        const double c = sk.noiseCalibration();
        ok(c > 0.85 && c < 1.15, "the floor is calibrated on noise (mean slow average ÷ floor " + std::to_string(c) + ", want ≈ 1)");
    }
    // ── accuracy ──
    {
        const double a0 = meanCer(false, 31.25, 0, 2), a8 = meanCer(false, 31.25, -8, 2), a12 = meanCer(false, 31.25, -12, 3);
        ok(a0 < 0.02, "PSK31 audio 0 dB: CER " + pct(a0) + " (< 2 %)");
        ok(a8 < 0.04, "PSK31 audio −8 dB: CER " + pct(a8) + " (< 4 %)");
        ok(a12 < 0.10, "PSK31 audio −12 dB: CER " + pct(a12) + " (< 10 %)");
        const double i8 = meanCer(true, 31.25, -8, 2), i12 = meanCer(true, 31.25, -12, 3);
        ok(i8 < 0.04, "PSK31 IQ −8 dB: CER " + pct(i8) + " (< 4 %)");
        ok(i12 < 0.10, "PSK31 IQ −12 dB: CER " + pct(i12) + " (< 10 %)");
        const double p5 = meanCer(false, 62.5, -5, 3), q5 = meanCer(true, 62.5, -5, 3);
        ok(p5 < 0.09 && q5 < 0.09, "PSK63 −5 dB: CER audio " + pct(p5) + ", IQ " + pct(q5) + " (< 9 %)");
    }
    // ── offsets, drift, clock ──
    {
        double worst = 0; for (double hz : { -20000.0, -9001.3, -100.0, 0.0, 37.7, 4321.9, 19876.5 }) worst = std::max(worst, cerOf(true, 31.25, -6, hz, 5));
        ok(worst < 0.05, "PSK31 at seven offsets across 48 kHz IQ (centre, edges, odd Hz): worst CER " + pct(worst));
        double wa = 0; for (double hz : { 300.0, 523.4, 1500.0, 2788.8, 3300.0 }) wa = std::max(wa, cerOf(false, 31.25, -6, hz, 6));
        ok(wa < 0.06, "…and at five audio frequencies 300–3300 Hz: worst " + pct(wa));
        const double d1 = cerOf(true, 31.25, -5, 1200, 7, 1.0);
        ok(d1 < 0.04, "a 1 Hz/s drift is followed: CER " + pct(d1));
        const double ck = cerOf(false, 31.25, -6, 1100, 8, 0, 200);
        ok(ck < 0.04, "a transmitter symbol clock 200 ppm off: CER " + pct(ck));
    }
    // ── a crowded passband ──
    {
        Rng rng(42);
        std::vector<Sig> v;
        const char* calls[] = { "g4abc","dl1xyz","f5mgu","oh3uq","ea7xx","i2abc","sp9zz","ok1rr","ha5kk","yo3aa","9a1cd","lz2ef" };
        double hz = 430;
        for (int i = 0; i < 12; ++i) {
            Sig s; s.baud = (i % 4 == 3) ? 62.5 : 31.25; s.hz = hz;
            hz += (i == 4) ? 31.25 : (s.baud > 40 ? 230 : 160);
            s.snrDb = -4 + (i % 3) * 3; if (i == 9) s.snrDb = 12;
            s.text = std::string("cq cq de ") + calls[i] + " " + calls[i] + " " + calls[i] + " pse k\ncq cq de " + calls[i] + " " + calls[i] + " k\n" + kQso[1];
            s.startSec = 2.5 + 0.4 * i;
            v.push_back(s);
        }
        double dur = 0; for (auto& s : v) dur = std::max(dur, s.startSec + duration(s));
        std::vector<cf> x; std::vector<float> r;
        synth(v, 12000, false, dur + 3, rng, x, r);
        psk::Skimmer sk(cfgFor(false, 12000));
        const Run res = run(sk, x, r);
        int good = 0, spotted = 0, wrong = 0; std::string bad;
        for (size_t i = 0; i < v.size(); ++i) {
            const double c = cer(v[i].text, near(res, v[i].hz, 12));
            good += c < 0.08; if (c >= 0.08) bad += " " + std::to_string((int)v[i].hz) + ":" + pct(c);
            if (c >= 0.08 && std::getenv("PSK_SHOW")) std::printf("     sent: %s\n     got:  %s\n", squash(v[i].text).c_str(), squash(near(res, v[i].hz, 12)).c_str());
            std::string up = calls[i]; for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
            for (auto& s : res.spots) if (s.call == up && std::fabs(s.offsetHz - v[i].hz) < 12) { ++spotted; break; }
        }
        for (auto& s : res.spots) {
            bool real = false; for (auto* c : calls) { std::string up = c; for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch); real |= s.call == up; }
            wrong += !real;
        }
        ok(good == 12, "12 signals in 3 kHz (PSK31 + PSK63, one +16 dB above its neighbours): " + std::to_string(good) + "/12 under 8 % CER (a late join costs its first CQ)" + bad);
        const double pa = cer(v[4].text, near(res, v[4].hz, 10)), pb = cer(v[5].text, near(res, v[5].hz, 10));
        ok(pa < 0.06 && pb < 0.06, "★ two PSK31 signals 31.25 Hz apart, both read cleanly: " + pct(pa) + " / " + pct(pb));
        ok(spotted == 12, "each CQ spotted at its own frequency (" + std::to_string(spotted) + "/12)");
        ok(wrong == 0, "…and no call invented (" + std::to_string(wrong) + ")");
    }
    // ── zero text on noise ──
    for (int stress = 0; stress < 2; ++stress) for (int iq = 0; iq < 2; ++iq) {
        const double rate = iq ? 48000 : 12000;
        psk::Skimmer::Config c = cfgFor(iq, rate);
        if (stress) c.candidateDb = 0.4;
        psk::Skimmer sk(c);
        size_t chars = 0, spots = 0;
        sk.onText = [&](int, double, const std::string& t) { chars += t.size(); };
        sk.onSpot = [&](const psk::Spot&) { ++spots; };
        Rng rng(99 + iq + 10 * stress);
        std::vector<cf> x((size_t)rate); std::vector<float> r((size_t)rate); const double sg = std::sqrt(0.5);
        const int secs = stress ? 300 : 600;
        for (int s = 0; s < secs; ++s) {
            for (size_t i = 0; i < (size_t)rate; ++i) { if (iq) x[i] = cf((float)(sg * rng.gauss()), (float)(sg * rng.gauss())); else r[i] = (float)rng.gauss(); }
            if (iq) sk.feedIq(x.data(), (int)x.size()); else sk.feedReal(r.data(), (int)r.size());
        }
        ok(chars == 0 && spots == 0 && sk.signals(true).empty(),
           std::string(stress ? "★ channels FORCED OPEN on " : "") + std::to_string(secs / 60) + " min of noise, " + (iq ? "48 kHz IQ" : "12 kHz audio") + ": "
           + std::to_string(chars) + " characters, " + std::to_string(spots) + " spots" + (stress ? " (" + std::to_string(sk.tentativesDropped()) + " channels tried)" : ""));
        if (stress) ok(sk.tentativesDropped() > 200, "…and the stress really did open channels on noise (" + std::to_string(sk.tentativesDropped()) + ")");
    }
    // ── zero text from what is not PSK ──
    {
        const double rate = 12000;
        Rng rng(5);
        std::vector<float> r((size_t)(rate * 90));
        // a steady carrier; FT8-like 8-FSK (6.25 Hz, 0.16 s a tone); RTTY-like 2-FSK (170 Hz, 45.45 Bd) — strong
        double p1 = 0, p2 = 0, p3 = 0; int tone = 0, bit = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            const double t = (double)i / rate;
            if (i % (size_t)(0.16 * rate) == 0) tone = (int)(rng.uni() * 8);
            if (i % (size_t)(rate / 45.45) == 0) bit = rng.uni() < 0.5;
            p1 += 2 * M_PI * 700 / rate; p2 += 2 * M_PI * (1500 + 6.25 * tone) / rate; p3 += 2 * M_PI * (2400 + (bit ? 170 : 0)) / rate;
            r[i] = (float)(0.3 * std::cos(p1) + 0.3 * std::cos(p2) + 0.3 * std::cos(p3) + rng.gauss() * 0.3);
            (void)t;
        }
        psk::Skimmer sk(cfgFor(false, rate));
        size_t chars = 0; sk.onText = [&](int, double, const std::string& t) { chars += t.size(); };
        for (size_t i = 0; i < r.size(); i += 1024) sk.feedReal(r.data() + i, (int)std::min<size_t>(1024, r.size() - i));
        ok(chars == 0 && sk.signals(true).empty(), "a carrier, an FT8-like and an RTTY-like signal, 90 s, strong: " + std::to_string(chars) + " characters");
    }
    // ── IMD ──
    {
        float got[2] = {};
        for (int k = 0; k < 2; ++k) {
            Rng rng(5);
            Sig s; s.hz = 1200; s.baud = 31.25; s.snrDb = 10; s.cubic = k ? 0.35 : 0.2; s.startSec = 2.5; s.idleBefore = 160; s.text = kQso[0];
            std::vector<Sig> v{ s }; std::vector<cf> x; std::vector<float> r;
            synth(v, 12000, false, duration(s) + 3, rng, x, r);
            psk::Skimmer sk(cfgFor(false, 12000)); run(sk, x, r);
            for (auto& sg : sk.signals(true)) got[k] = sg.imdDb;
        }
        ok(std::fabs(got[0] - cubicImdDb(0.2)) < 2.5 && std::fabs(got[1] - cubicImdDb(0.35)) < 2.5,
           "IMD of an overdriven idle: " + std::to_string((int)std::lround(got[0])) + " / " + std::to_string((int)std::lround(got[1]))
           + " dB (the cubic's own: " + std::to_string((int)std::lround(cubicImdDb(0.2))) + " / " + std::to_string((int)std::lround(cubicImdDb(0.35))) + ")");
    }
    // ── through a DecoderHost: a listener's "psk" on 48 kHz audio ──
    {
        DecoderHost::Env e;
        e.dialHz = [] { return 14070000.0; };
        e.rfOfAudio = [](double a) { return 14070000.0 + a; };   // USB
        auto tap = std::make_shared<SpyTap>();
        e.pskSpotTap = [tap](bool) -> DecoderTapPtr { return tap; };
        DecoderHost h(e, "s1");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("psk", "{\"centre\":1500}") == DecoderHost::Start::Ok && h.kind() == "psk" && h.wantsAudio() && !h.wantsIq(), "\"psk\" starts on audio");
        Rng rng(31);
        // ★ two signals in the passband: the one nearer the middle (1480 Hz) is ONE SIGNAL's
        Sig a; a.hz = 1480; a.snrDb = 0; a.startSec = 2.0; a.text = "cq cq cq de g4abc g4abc g4abc pse k\ncq cq cq de g4abc g4abc g4abc pse k\n";
        Sig b; b.hz = 900; b.snrDb = 4; b.startSec = 2.5; b.text = "qrl? qrl? de dl1xyz dl1xyz k\n qrl? qrl?\n";
        std::vector<Sig> v{ a, b }; std::vector<cf> x; std::vector<float> au;
        synth(v, 48000, false, std::max(duration(a), duration(b)) + 4, rng, x, au, 0.3);
        for (size_t i = 0; i < au.size(); i += 1024) {
            h.feed(au.data() + i, (int)std::min<size_t>(1024, au.size() - i), 1, NAN);
            while (h.queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int w = 0; w < 300 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::string got = p->decoded();
        ok(got.find("de g4abc g4abc") != std::string::npos && got.find("dl1xyz") == std::string::npos,
           "ONE SIGNAL's text (the one in the middle of the passband) arrives as 0x01 frames, and only its (\"" + got.substr(0, 50) + "\")");
        ok(p->count("\"type\":\"psk_signals\"") > 0 && p->count("\"skimmer\":false") > 0, "the signals list arrives, marked not a skimmer");
        const std::string lst = p->last("\"type\":\"psk_signals\"");
        if (std::getenv("PSK_SHOWLIST")) std::printf("%s\n", lst.c_str());
        ok(lst.find("\"baud\":63") == std::string::npos && lst.find("\"hz\":14071480") != std::string::npos && lst.find("\"hz\":1407090") != std::string::npos,
           "both signals listed, at dial + audio, as PSK31");
        ok(p->count("\"type\":\"psk_spot\"") >= 1 && p->count("\"callsign\":\"G4ABC\"") >= 1 && p->count("\"mode\":\"BPSK31\"") >= 1, "G4ABC is spotted on the socket, as BPSK31");
        bool rfOk = false; { std::lock_guard<std::mutex> lk(tap->m); for (size_t i = 0; i < tap->calls.size(); ++i) rfOk |= tap->calls[i] == "G4ABC" && std::fabs(tap->rf[i] - 14071480.0) < 10; }
        ok(rfOk, "…and the summary tap gets it at its RF frequency");
        h.stop();
        ok(tap->finished.load() >= 1 && !h.wantsAudio(), "stopping finishes the summary and stops the audio");
        h.shutdown();
    }
    // ── through a DecoderHost: a skimmer's "pskskim" on IQ ──
    {
        DecoderHost::Env e;
        DecoderHost h(e, "s2");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        const int before = DecoderHost::skimmersRunning().load();
        ok(h.start("pskskim", "{\"width\":12000}") == DecoderHost::Start::Ok && h.kind() == "pskskim" && h.wantsIq() && !h.wantsAudio(), "\"pskskim\" starts on IQ, not audio");
        ok(DecoderHost::skimmersRunning().load() == before + 1 && std::fabs(h.iqWidthHz() - 12000) < 1, "the process counts it, and its width is the one asked for");
        Rng rng(77); std::vector<Sig> v;
        const char* calls[] = { "ea7xx", "ok1rr" }; const double hz[] = { -3100, 2700 };
        for (int i = 0; i < 2; ++i) {
            Sig s; s.hz = hz[i]; s.baud = i ? 62.5 : 31.25; s.snrDb = 4; s.startSec = 2.0;
            for (int r = 0; r < 3; ++r) s.text += std::string("cq cq de ") + calls[i] + " " + calls[i] + " " + calls[i] + " k\n";
            v.push_back(s);
        }
        double secs = 0; for (auto& s : v) secs = std::max(secs, s.startSec + duration(s));
        std::vector<cf> x; std::vector<float> r; synth(v, 18750, true, secs + 3, rng, x, r);
        for (size_t i = 0; i < x.size(); i += 2048) {
            h.feedIq(x.data() + i, (int)std::min<size_t>(2048, x.size() - i), 18750, 14071000.0);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        for (int w = 0; w < 300 && p->count("OK1RR") == 0; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ok(p->count("\"callsign\":\"EA7XX\"") >= 1 && p->count("\"callsign\":\"OK1RR\"") >= 1, "both stations spotted from IQ (an odd slice rate, 18.75 kHz)");
        ok(p->count("\"frequency\":14067900") >= 1 && p->count("\"frequency\":14073700") >= 1, "each at centre + offset");
        ok(p->count("\"mode\":\"BPSK63\"") >= 1, "the PSK63 one is spotted as BPSK63");
        ok(p->decoded().empty(), "a skimmer sends no 0x01 text (its text is in the list)");
        h.stop();
        ok(DecoderHost::skimmersRunning().load() == before && !h.wantsIq(), "stopped: no longer counted, no longer wants IQ");
        h.shutdown();
    }
    // ── the JSON text is escaped, never cut into ──
    ok(PskRun::jsonText("a\"b\\c\nd\x01" "e", 100) == "a\\\"b\\\\c de" && PskRun::jsonText("x\ny", 100, true) == "x\\ny",
       "off-air text is escaped for JSON (quotes, backslashes), control codes dropped, newlines a space in the list");
    // ── the gallery's PSK summary ──
    {
        gallery::Store st; const std::string dir = tmpDir("gal"); st.setDir(dir);
        gallery::Meta m; m.kind = "psk"; m.origin = "background"; m.freqHz = 14071500; m.mode = "usb"; m.decoder = "pskskim"; m.profile = "Profile 1";
        int64_t now = 1761400000;
        {
            bg::Ft8SummaryTap t(st, m, NAN, NAN, [&] { return now; });
            for (int i = 0; i < 4; ++i) { t.cwSpot("DL1ABC", 14070900, 0, 12, "CQ"); now += 20; }
            t.cwSpot("G4ABC", 14071300, 0, 8, "DE"); t.cwSpot("JA1ZZ", 14072100, 0, 3, "CQ");
            now += 70; t.tick();
            t.finish(false);
        }
        const std::string all = slurpTxt(dir);
        ok(all.find("PSK spots summary") != std::string::npos, "a PSK summary card is written");
        ok(all.find("Spots heard:       6 from 3 stations") != std::string::npos, "it counts spots and stations");
        ok(all.find("Most heard:        DL1ABC (4 spots)") != std::string::npos, "the most-heard call");
        ok(all.find("WPM") == std::string::npos && all.find("CW") == std::string::npos && all.find("FT8") == std::string::npos,
           "nothing in it says WPM, CW or FT8");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
