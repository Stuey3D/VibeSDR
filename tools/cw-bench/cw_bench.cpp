// cw_bench — the CW skimmer measured: accuracy on synthetic Morse, false signals on noise, real recordings, and CPU.
//
//   cw_bench accuracy                 CER over WPM × SNR (one signal, real 12 kHz audio and 48 kHz IQ)
//   cw_bench conditions               QSB, drift, key clicks, Farnsworth, a ragged fist
//   cw_bench multi                    many signals 50–300 Hz apart in one segment (clicks on), per-signal CER
//   cw_bench noise [minutes]          pure noise: false signals and false spots
//   cw_bench bias                     the floor's bias on noise (kFloorBias in cw_skimmer.cpp)
//   cw_bench cpu [seconds]            % of one core for a busy segment at 24/48/96 kHz (+ the listener's 12 kHz)
//   cw_bench wav <iq.wav> [centreHz] [width]   a real recording (stereo int16 I/Q) → spots and signals
//
// ★ SNR is quoted the way receivers are compared: carrier (key down) over the noise in 2.5 kHz. ★ Silent: no audio out.
// Build: see scripts/test-cw.sh (it builds this twice — SIMD and VIBE_FORCE_SCALAR — for the speed-up).
#include "decoders/cw_skimmer.h"
#include "vibedsp/vibe_denormals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using cf = std::complex<float>;
using namespace vibe::cw;

// ── deterministic noise ──
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double u = uni(), v = uni(); if (u < 1e-300) u = 1e-300; return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v); }
};

// ── Morse keying ──
struct Keying {
    double wpm = 20, farnsworth = 0;   // farnsworth: the character speed stays wpm, the gaps are stretched to this overall speed
    double riseMs = 4;                 // raised-cosine edges; 0 = hard keying (key clicks)
    double jitter = 0;                 // a human fist: each element's length varies by this fraction (σ)
    double weight = 1.0;               // mark/space weighting: 1.1 = marks 10 % long
};
/** The key's envelope (0..1), sampled at `rate`, for `text` — then `tailSec` of silence. */
std::vector<float> keyEnvelope(const std::string& text, const Keying& k, double rate, Rng& rng, double tailSec = 0.5) {
    const double unit = 1.2 / k.wpm;
    double gapUnit = unit;
    if (k.farnsworth > 0 && k.farnsworth < k.wpm) {
        // ARRL-style: the extra time is spread over the character and word gaps (19 of 50 units of PARIS)
        const double ta = (60.0 * k.wpm - 37.2 * k.farnsworth) / (k.farnsworth * k.wpm);
        gapUnit = ta / 19.0 * 1.0 / 1.0;   // seconds per gap unit
    }
    std::vector<std::pair<double, bool>> seg;   // (seconds, keyed)
    auto add = [&](double sec, bool on) { if (sec > 0) seg.push_back({ sec, on }); };
    auto jit = [&](double x) { return k.jitter > 0 ? std::max(0.3 * x, x * (1.0 + k.jitter * rng.gauss())) : x; };
    add(0.3, false);
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ') { add(jit((k.farnsworth > 0 ? gapUnit : unit) * 4), false); continue; }   // 3 already after the char → 7
        std::string code;
        if (c == '<') { const size_t e = text.find('>', i); code = morseCode(text.substr(i, e - i + 1)); i = e; }
        else code = morseCode(std::string(1, c));
        if (code.empty()) continue;
        for (size_t e = 0; e < code.size(); ++e) {
            const double m = (code[e] == '-' ? 3 : 1) * unit;
            add(jit(m * k.weight), true);
            if (e + 1 < code.size()) add(jit(unit * (2.0 - k.weight)), false);
        }
        add(jit((k.farnsworth > 0 ? gapUnit : unit) * 3), false);
    }
    add(tailSec, false);
    double total = 0; for (auto& s : seg) total += s.first;
    std::vector<float> env((size_t)(total * rate) + 1, 0.0f);
    double t = 0;
    for (auto& s : seg) {
        if (s.second) {
            const size_t a = (size_t)(t * rate), b = std::min(env.size(), (size_t)((t + s.first) * rate));
            const size_t r = (size_t)(k.riseMs * 1e-3 * rate);
            for (size_t i = a; i < b; ++i) {
                double g = 1.0;
                if (r > 0) {
                    if (i - a < r) g = 0.5 - 0.5 * std::cos(M_PI * (double)(i - a) / r);
                    if (b - i < r) g = std::min(g, 0.5 - 0.5 * std::cos(M_PI * (double)(b - i) / r));
                }
                env[i] = (float)g;
            }
        }
        t += s.first;
    }
    return env;
}

struct Tone {
    double hz = 700;          // offset (IQ) or audio frequency
    double snrDb = 10;        // carrier over noise in 2.5 kHz
    double driftHzPerSec = 0;
    double qsbDb = 0, qsbHz = 0.2;   // a fade: depth (dB, peak-to-trough) and rate
    double startSec = 0;
    std::vector<float> env;
};
/** Mix tones + noise. Noise power is 1 in the full band (IQ: complex, rate Hz wide; real: rate/2 wide). */
void synth(std::vector<Tone>& tones, double rate, bool complexOut, double seconds, Rng& rng,
           std::vector<cf>& iq, std::vector<float>& re, double noiseOn = 1.0) {
    const size_t n = (size_t)(seconds * rate);
    if (complexOut) iq.assign(n, cf(0, 0)); else re.assign(n, 0.0f);
    const double bw = complexOut ? rate : rate / 2;
    for (auto& t : tones) {
        // carrier power so that P / (noise in 2500 Hz) = snr; noise density = 1/bw
        const double pc = std::pow(10.0, t.snrDb / 10.0) * 2500.0 / bw;
        const double amp = complexOut ? std::sqrt(pc) : std::sqrt(2 * pc);
        double ph = rng.uni() * 2 * M_PI;
        const size_t s0 = (size_t)(t.startSec * rate);
        for (size_t i = 0; i < t.env.size() && s0 + i < n; ++i) {
            const double tt = (double)i / rate;
            const double f = t.hz + t.driftHzPerSec * tt;
            ph += 2 * M_PI * f / rate;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
            double a = amp * t.env[i];
            if (t.qsbDb > 0) a *= std::pow(10.0, -t.qsbDb / 20.0 * (0.5 - 0.5 * std::cos(2 * M_PI * t.qsbHz * tt)));
            if (a == 0) continue;
            if (complexOut) iq[s0 + i] += cf((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
            else re[s0 + i] += (float)(a * std::cos(ph));
        }
    }
    const double sg = complexOut ? std::sqrt(0.5) : 1.0;
    if (noiseOn > 0)
        for (size_t i = 0; i < n; ++i) {
            if (complexOut) iq[i] += cf((float)(sg * rng.gauss() * noiseOn), (float)(sg * rng.gauss() * noiseOn));
            else re[i] += (float)(rng.gauss() * noiseOn);
        }
}

// ── scoring: semi-global edit distance (the decoder may join late and stop early) ──
static std::string squash(const std::string& s) {
    std::string o; bool sp = true;
    for (char c : s) { if (c == ' ' || c == '\n') { if (!sp) o += ' '; sp = true; } else { o += c; sp = false; } }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
/** Character errors of `got` against the best-matching span of `ref`, and that span's length. Spaces count. */
static void cer(const std::string& refIn, const std::string& gotIn, int& errs, int& refLen) {
    const std::string ref = squash(refIn), got = squash(gotIn);
    const size_t R = ref.size(), G = got.size();
    if (G == 0) { errs = (int)R; refLen = (int)R; return; }
    // D[i][j]: got[0..i) against ref ending at j, free start in ref
    std::vector<int> prev(R + 1, 0), cur(R + 1);
    std::vector<int> startPrev(R + 1), startCur(R + 1);
    for (size_t j = 0; j <= R; ++j) startPrev[j] = (int)j;
    for (size_t i = 1; i <= G; ++i) {
        cur[0] = (int)i; startCur[0] = 0;
        for (size_t j = 1; j <= R; ++j) {
            int best = prev[j - 1] + (got[i - 1] == ref[j - 1] ? 0 : 1); int st = startPrev[j - 1];
            if (prev[j] + 1 < best) { best = prev[j] + 1; st = startPrev[j]; }
            if (cur[j - 1] + 1 < best) { best = cur[j - 1] + 1; st = startCur[j - 1]; }
            cur[j] = best; startCur[j] = st;
        }
        std::swap(prev, cur); std::swap(startPrev, startCur);
    }
    int bj = 0; for (size_t j = 1; j <= R; ++j) if (prev[j] < prev[bj]) bj = (int)j;
    errs = prev[bj]; refLen = std::max(1, bj - startPrev[bj]);
}

static std::string kText =
    "CQ CQ DE G4ABC G4ABC K  THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789  "
    "UR RST 579 579 NAME JOHN QTH LONDON  TNX FER CALL  CQ TEST DL1XYZ DL1XYZ  PSE QSL VIA BURO  73 ES GL ";

struct RunResult { std::map<int, std::string> text; std::map<int, double> hz; std::vector<Spot> spots; Skimmer* sk = nullptr; double cpuSec = 0; };

static double threadCpu() {
    timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9;
}
RunResult run(Skimmer& sk, const std::vector<cf>& iq, const std::vector<float>& re, int block = 512) {
    RunResult r; r.sk = &sk;
    sk.onText = [&](int id, double hz, const std::string& t) { r.text[id] += t; r.hz[id] = hz; };
    sk.onSpot = [&](const Spot& s) { r.spots.push_back(s); };
    const double c0 = threadCpu();
    if (!iq.empty()) for (size_t i = 0; i < iq.size(); i += block) sk.feedIq(iq.data() + i, (int)std::min<size_t>(block, iq.size() - i));
    else for (size_t i = 0; i < re.size(); i += block) sk.feedReal(re.data() + i, (int)std::min<size_t>(block, re.size() - i));
    r.cpuSec = threadCpu() - c0;
    return r;
}
/** The text decoded nearest `hz` (within 40 Hz): the signal there that decoded the most. A second signal on the same
 *  tone (one that went and was found again) is counted separately by duplicatesNear. */
std::string textNear(const RunResult& r, double hz) {
    std::string t;
    for (auto& kv : r.text) { auto h = r.hz.find(kv.first); if (h != r.hz.end() && std::fabs(h->second - hz) < 40 && kv.second.size() > t.size()) t = kv.second; }
    return t;
}
int duplicatesNear(const RunResult& r, double hz) {
    int n = 0;
    for (auto& kv : r.text) { auto h = r.hz.find(kv.first); if (h != r.hz.end() && std::fabs(h->second - hz) < 40) ++n; }
    return std::max(0, n - 1);
}

// ── modes ──
static int accuracy(bool complexIn) {
    const double rate = complexIn ? 48000 : 12000;
    std::printf("\n== accuracy: one signal, %s, 70 s of text; CER = character errors / characters sent (spaces count) ==\n",
                complexIn ? "IQ 48 kHz (skimmer)" : "audio 12 kHz (listener)");
    const double wpms[] = { 12, 20, 30, 40 };
    const double snrs[] = { 10, 0, -5, -8, -10, -12, -14 };
    std::printf("SNR dB(2.5k) ");
    for (double w : wpms) std::printf("  %2.0f WPM      ", w);
    std::printf("\n");
    for (double snr : snrs) {
        std::printf("   %+4.0f     ", snr);
        for (double w : wpms) {
            Rng rng((uint64_t)(w * 100 + snr + 1000 + complexIn));
            Keying k; k.wpm = w;
            std::string sent; while (sent.size() < (size_t)(w * 5 * 1.1)) sent += kText;   // ≈ 70 s of text
            Tone t; t.hz = complexIn ? 3130 : 700; t.snrDb = snr; t.env = keyEnvelope(sent, k, rate, rng);
            std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
            synth(ts, rate, complexIn, t.env.size() / rate, rng, iq, re);
            Skimmer::Config c; c.rate = rate; c.complexIn = complexIn;
            if (complexIn) { c.loHz = -20000; c.hiHz = 20000; } else { c.loHz = 150; c.hiHz = 3500; }
            Skimmer sk(c);
            auto r = run(sk, iq, re);
            int e, n; cer(sent, textNear(r, t.hz), e, n);
            float wpmEst = 0; for (auto& v : sk.signals()) if (std::fabs(v.offsetHz - t.hz) < 40) { wpmEst = v.wpm; break; }
            const int dup = duplicatesNear(r, t.hz);
            std::printf(" %5.1f%% (%3.0f)%s", 100.0 * e / std::max(1, (int)squash(sent).size()), wpmEst, dup ? "+" : " ");
        }
        std::printf("\n");
    }
    std::printf("(in brackets: the WPM the decoder measured; + = the signal was lost and found again at least once)\n");
    return 0;
}

static int conditions() {
    std::printf("\n== conditions: 20 and 30 WPM, IQ 48 kHz, SNR 0 dB unless stated ==\n");
    struct Case { const char* name; double wpm, snr; Keying k; double qsb, drift; };
    std::vector<Case> cs;
    auto base = [](double w) { Keying k; k.wpm = w; return k; };
    for (double w : { 20.0, 30.0 }) {
        cs.push_back({ "clean", w, 0, base(w), 0, 0 });
        cs.push_back({ "QSB 15 dB / 5 s", w, 5, base(w), 15, 0 });
        cs.push_back({ "QSB 20 dB / 3 s", w, 5, base(w), 20, 0 });
        cs.push_back({ "drift +2 Hz/s", w, 0, base(w), 0, 2 });
        Keying hard = base(w); hard.riseMs = 0; cs.push_back({ "hard keying (clicks)", w, 0, hard, 0, 0 });
        Keying fist = base(w); fist.jitter = 0.12; cs.push_back({ "ragged fist (12 %)", w, 0, fist, 0, 0 });
        Keying heavy = base(w); heavy.weight = 1.25; cs.push_back({ "heavy weighting 1.25", w, 0, heavy, 0, 0 });
        Keying fw = base(w); fw.farnsworth = w * 0.6; cs.push_back({ "Farnsworth 60 %", w, 0, fw, 0, 0 });
    }
    for (auto& c : cs) {
        Rng rng(77 + (uint64_t)c.wpm);
        std::string sent; while (sent.size() < (size_t)(c.wpm * 5 * 1.1)) sent += kText;
        Tone t; t.hz = 2000; t.snrDb = c.snr; t.qsbDb = c.qsb; t.qsbHz = c.qsb >= 20 ? 1 / 3.0 : 0.2; t.driftHzPerSec = c.drift;
        t.env = keyEnvelope(sent, c.k, 48000, rng);
        std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
        synth(ts, 48000, true, t.env.size() / 48000.0, rng, iq, re);
        Skimmer::Config cfg; cfg.rate = 48000; cfg.complexIn = true; cfg.loHz = -20000; cfg.hiHz = 20000;
        Skimmer sk(cfg);
        auto r = run(sk, iq, re);
        // drift moves the tone: take every signal's text
        std::string got; for (auto& kv : r.text) got += kv.second;
        int e, n; cer(sent, c.drift ? got : textNear(r, t.hz), e, n);
        std::printf("  %2.0f WPM  %-22s SNR %+3.0f  CER %5.1f%%\n", c.wpm, c.name, c.snr, 100.0 * e / std::max(1, (int)squash(sent).size()));
    }
    return 0;
}

static int multi() {
    std::printf("\n== multi: 12 signals in 48 kHz IQ, 60–300 Hz apart, mixed speeds/SNR, half hard-keyed ==\n");
    Rng rng(4242);
    const char* calls[] = { "G4ABC","DL1XYZ","EA3QP","K1ABC","JA1ZZ","OH2BH","F5IN","PA3EWP","SP9X","OK1RR","VE3XN","I2XYZ" };
    std::vector<Tone> ts; std::vector<std::string> sent;
    double hz = -9000;
    for (int i = 0; i < 12; ++i) {
        const double gap = 60 + 240 * rng.uni();
        hz += gap;
        Keying k; k.wpm = 16 + 4 * (i % 7); k.riseMs = (i % 2) ? 0 : 4;
        std::string s; for (int r = 0; r < 6; ++r) s += std::string("CQ TEST ") + calls[i] + " " + calls[i] + " TEST  ";
        Tone t; t.hz = hz; t.snrDb = -6 + 3 * (i % 6); t.startSec = rng.uni() * 3;
        // ★ one very strong signal, to cast sidelobes/clicks over its neighbours
        if (i == 5) t.snrDb = 30;
        t.env = keyEnvelope(s, k, 48000, rng);
        ts.push_back(t); sent.push_back(s);
        std::printf("  sig %2d  %+8.1f Hz  %2.0f WPM  SNR %+3.0f  %s\n", i, hz, k.wpm, t.snrDb, k.riseMs == 0 ? "hard" : "shaped");
    }
    double secs = 0; for (auto& t : ts) secs = std::max(secs, t.startSec + t.env.size() / 48000.0);
    std::vector<cf> iq; std::vector<float> re;
    synth(ts, 48000, true, secs, rng, iq, re);
    Skimmer::Config cfg; cfg.rate = 48000; cfg.complexIn = true; cfg.loHz = -20000; cfg.hiHz = 20000;
    Skimmer sk(cfg);
    auto r = run(sk, iq, re);
    int found = 0, spotted = 0;
    for (size_t i = 0; i < ts.size(); ++i) {
        const std::string got = textNear(r, ts[i].hz);
        int e, n; cer(sent[i], got, e, n);
        bool sp = false; for (auto& s : r.spots) if (s.call == calls[i]) sp = true;
        found += !got.empty(); spotted += sp;
        std::printf("  sig %2d  CER %5.1f%%  spot %s\n", (int)i, 100.0 * e / std::max(1, (int)squash(sent[i]).size()), sp ? "yes" : "NO");
    }
    // ghosts: decoded text not near any tone
    int ghostText = 0;
    for (auto& kv : r.text) {
        bool near = false; for (auto& t : ts) if (std::fabs(r.hz[kv.first] - t.hz) < 40) near = true;
        if (!near) { ++ghostText; std::printf("  ✗ ghost at %+.0f Hz: \"%.60s\"\n", r.hz[kv.first], kv.second.c_str()); }
    }
    int wrong = 0; for (auto& s : r.spots) { bool ok = false; for (auto c : calls) ok |= s.call == c; if (!ok) { ++wrong; std::printf("  ✗ false spot %s\n", s.call.c_str()); } }
    std::printf("  decoded %d/12, spotted %d/12, ghost signals %d, false spots %d (ghosts dropped by the test: %llu)\n",
                found, spotted, ghostText, wrong, (unsigned long long)sk.ghostsDropped());
    return 0;
}

static int noise(double minutes) {
    std::printf("\n== noise: %.0f minutes of pure noise, 96 kHz IQ and 12 kHz audio ==\n", minutes);
    for (int pass = 0; pass < 2; ++pass) {
        const bool cplx = pass == 0;
        const double rate = cplx ? 96000 : 12000;
        Skimmer::Config cfg; cfg.rate = rate; cfg.complexIn = cplx;
        if (cplx) { cfg.loHz = -45000; cfg.hiHz = 45000; } else { cfg.loHz = 150; cfg.hiHz = 3500; }
        Skimmer sk(cfg);
        Rng rng(99 + pass);
        int sigs = 0, spots = 0; size_t text = 0;
        std::vector<cf> iq; std::vector<float> re;
        std::map<int, int> seen;
        for (double t = 0; t < minutes * 60; t += 10) {
            std::vector<Tone> none;
            synth(none, rate, cplx, 10, rng, iq, re);
            auto r = run(sk, iq, re);
            for (auto& kv : r.text) { text += kv.second.size(); seen[kv.first] = 1; }
            for (auto& s : r.spots) { ++spots; std::printf("  ✗ false spot %s\n", s.call.c_str()); }
            if (std::getenv("CW_CAL") && (int)t % 120 == 0) std::printf("    t=%4.0f s calibration %.3f tentatives dropped %llu tracked %d\n", t, sk.noiseCalibration(), (unsigned long long)sk.tentativesDropped(), sk.tracked());
        }
        for (auto& v : sk.signals(true)) { seen[v.id] = 1; std::printf("  ✗ false signal %+.0f Hz %.0f WPM %+.1f dB \"%.50s\"\n", v.offsetHz, v.wpm, v.snrDb, v.text.c_str()); }
        sigs = (int)seen.size();
        std::printf("  %s: %llu hops; %d false signals confirmed, %d false spots, %zu characters of noise text, %llu tentatives dropped\n",
                    cplx ? "96 kHz IQ" : "12 kHz audio", (unsigned long long)sk.hops(), sigs, spots, text, (unsigned long long)sk.tentativesDropped());
    }
    return 0;
}

static int bias() {
    for (int pass = 0; pass < 2; ++pass) {
        const bool cplx = pass == 0; const double rate = cplx ? 48000 : 12000;
        Skimmer::Config cfg; cfg.rate = rate; cfg.complexIn = cplx;
        if (cplx) { cfg.loHz = -20000; cfg.hiHz = 20000; } else { cfg.loHz = 150; cfg.hiHz = 3500; }
        Skimmer sk(cfg);
        Rng rng(5);
        std::vector<Tone> none; std::vector<cf> iq; std::vector<float> re;
        synth(none, rate, cplx, 60, rng, iq, re);
        run(sk, iq, re);
        std::printf("bias %s: long average / noise estimate on pure noise = %.3f (%+.2f dB; 1.0 = kFloorBias right)\n",
                    cplx ? "IQ 48 kHz" : "audio 12 kHz", sk.noiseCalibration(), 10 * std::log10(sk.noiseCalibration()));
    }
    return 0;
}

static int cpu(double seconds) {
    std::printf("\n== cpu (%s kernels): %.0f s of a busy segment (one signal every ~400 Hz, all keyed), one thread ==\n",
                Skimmer::simdName(), seconds);
    struct W { const char* name; double rate; bool cplx; double lo, hi; };
    const W ws[] = { { "listener 12 kHz audio", 12000, false, 150, 3500 },
                     { "skimmer 24 kHz IQ", 24000, true, -11000, 11000 },
                     { "skimmer 48 kHz IQ", 48000, true, -22000, 22000 },
                     { "skimmer 96 kHz IQ", 96000, true, -45000, 45000 },
                     { "skimmer 192 kHz IQ", 192000, true, -90000, 90000 } };
    for (auto& w : ws) {
        Rng rng(1);
        std::vector<Tone> ts;
        for (double hz = w.lo + 200; hz < w.hi - 200; hz += 400) {
            Keying k; k.wpm = 18 + 10 * rng.uni();
            std::string s; while (s.size() < seconds * k.wpm / 1.2 / 8) s += "CQ TEST G4ABC G4ABC  ";
            Tone t; t.hz = hz; t.snrDb = 5 + 20 * rng.uni(); t.env = keyEnvelope(s, k, w.rate, rng);
            ts.push_back(t);
        }
        std::vector<cf> iq; std::vector<float> re;
        synth(ts, w.rate, w.cplx, seconds, rng, iq, re);
        Skimmer::Config cfg; cfg.rate = w.rate; cfg.complexIn = w.cplx; cfg.loHz = w.lo; cfg.hiHz = w.hi;
        cfg.maxSignals = 400;
        Skimmer sk(cfg);
        auto r = run(sk, iq, re);
        const double pct = 100.0 * r.cpuSec / seconds;
        const double samples = w.rate * seconds;
        std::printf("  %-24s FFT %5d  bins %5d  tones %3zu  confirmed %3d  %6.3f %% of one core  (%.1f ns/sample)\n",
                    w.name, sk.fftSize(), sk.binsSearched(), ts.size(), sk.confirmed(), pct, 1e9 * r.cpuSec / samples);
    }
    return 0;
}

static int wav(const char* path, double centreHz, double width) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 1; }
    // minimal RIFF: find "fmt " and "data"
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12) return 1;
    int rate = 0, ch = 0, bits = 0; long dataLen = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) return 1;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len); if (std::fread(b.data(), 1, len, f) != len) return 1;
            ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; bits = b[14] | b[15] << 8;
        } else if (!std::memcmp(id, "data", 4)) { dataLen = len; break; }
        else std::fseek(f, len, SEEK_CUR);
    }
    if (ch != 2 || bits != 16) { std::fprintf(stderr, "need stereo int16 I/Q (got %d ch %d bit)\n", ch, bits); return 1; }
    (void)dataLen;
    Skimmer::Config cfg; cfg.rate = rate; cfg.complexIn = true; cfg.loHz = -width / 2; cfg.hiHz = width / 2; cfg.maxSignals = 160;
    Skimmer sk(cfg);
    std::printf("%s: %d Hz IQ, FFT %d (%.2f Hz bins, hop %.2f ms), searching %.0f kHz (%d bins), %s kernels\n", path, rate, sk.fftSize(),
                sk.binHzActual(), sk.hopSec() * 1e3, width / 1e3, sk.binsSearched(), Skimmer::simdName());
    std::vector<Spot> spots;
    sk.onSpot = [&](const Spot& s) {
        spots.push_back(s);
        std::printf("  SPOT %-10s %10.2f kHz  %2.0f WPM  %+3.0f dB  %s\n", s.call.c_str(), (centreHz + s.offsetHz) / 1e3, s.wpm, s.snrDb, s.how.c_str());
    };
    std::vector<int16_t> raw(2 * 8192); std::vector<cf> iq(8192);
    double cpu = 0; long total = 0;
    for (;;) {
        const size_t got = std::fread(raw.data(), 4, 8192, f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) iq[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
        const double c0 = threadCpu();
        sk.feedIq(iq.data(), (int)got);
        cpu += threadCpu() - c0; total += (long)got;
    }
    std::fclose(f);
    const double secs = (double)total / rate;
    std::printf("\n%.0f s decoded in %.2f s of CPU = %.3f %% of one core; %zu spots; %d signals active at the end; %llu not-CW dropped (%llu broadband)\n",
                secs, cpu, 100.0 * cpu / secs, spots.size(), sk.confirmed(), (unsigned long long)sk.ghostsDropped(), (unsigned long long)sk.broadbandDropped());
    std::printf("\nSignals heard (strongest active first, then gone):\n");
    for (auto& v : sk.signals()) {
        std::string t = v.text; for (auto& c : t) if (c == '\n') c = ' ';
        if (t.size() > 70) t = "…" + t.substr(t.size() - 70);
        std::printf("  %10.2f kHz %-9s %2.0f WPM %+4.0f dB %-6s %s\n", (centreHz + v.offsetHz) / 1e3, v.call.c_str(), v.wpm, v.snrDb,
                    qualityWord(v.quality), t.c_str());
    }
    return 0;
}

/** One case, shown: what was sent, every signal's text, the signals list. */
static int one(double wpm, double snr, bool cplx, double hz) {
    const double rate = cplx ? 48000 : 12000;
    Rng rng(7);
    Keying k; k.wpm = wpm;
    std::string sent; while (sent.size() < (size_t)(wpm * 5 * 0.5)) sent += kText;
    Tone t; t.hz = hz; t.snrDb = snr; t.env = keyEnvelope(sent, k, rate, rng);
    std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
    synth(ts, rate, cplx, t.env.size() / rate, rng, iq, re);
    Skimmer::Config c; c.rate = rate; c.complexIn = cplx;
    if (cplx) { c.loHz = -20000; c.hiHz = 20000; } else { c.loHz = 150; c.hiHz = 3500; }
    Skimmer sk(c);
    std::printf("FFT %d, bin %.2f Hz, hop %.2f ms, bins %d\n", sk.fftSize(), sk.binHzActual(), sk.hopSec() * 1e3, sk.binsSearched());
    auto r = run(sk, iq, re);
    std::printf("SENT: %s\n", squash(sent).c_str());
    for (auto& kv : r.text) std::printf("  id %d @ %+.1f Hz: %s\n", kv.first, r.hz[kv.first], squash(kv.second).c_str());
    for (auto& v : sk.signals()) std::printf("  sig %d %+.1f Hz %.1f WPM %+.1f dB q %.2f %s active %d chars %u\n", v.id, v.offsetHz, v.wpm, v.snrDb, v.quality, v.call.c_str(), v.active, v.chars);
    std::printf("  tracked %d, tentatives dropped %llu, ghosts %llu\n", sk.tracked(), (unsigned long long)sk.tentativesDropped(), (unsigned long long)sk.ghostsDropped());
    for (auto& s : r.spots) std::printf("  SPOT %s %s\n", s.call.c_str(), s.how.c_str());
    return 0;
}

int main(int argc, char** argv) {
    vibe::vibeFlushDenormals();
    const std::string m = argc > 1 ? argv[1] : "all";
    if (m == "accuracy") { accuracy(false); return accuracy(true); }
    if (m == "conditions") return conditions();
    if (m == "one" && argc > 4) return one(std::atof(argv[2]), std::atof(argv[3]), std::string(argv[4]) == "iq", argc > 5 ? std::atof(argv[5]) : 700);
    if (m == "multi") return multi();
    if (m == "noise") return noise(argc > 2 ? std::atof(argv[2]) : 10);
    if (m == "bias") return bias();
    if (m == "cpu") return cpu(argc > 2 ? std::atof(argv[2]) : 60);
    if (m == "wav" && argc > 2) return wav(argv[2], argc > 3 ? std::atof(argv[3]) : 0, argc > 4 ? std::atof(argv[4]) : 96000);
    std::fprintf(stderr, "usage: cw_bench accuracy|conditions|multi|noise [min]|bias|cpu [s]|wav <iq.wav> [centreHz] [width]\n");
    return 2;
}
