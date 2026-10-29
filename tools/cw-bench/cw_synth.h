// cw_synth.h — synthetic Morse for the CW skimmer's bench (cw_bench.cpp) and tests (vibeserver/test-cw.cpp): keying per
// ITU-R M.1677 timing (PARIS: a dot is 1.2/WPM s), shaped or hard edges, a fist's jitter, weighting, Farnsworth; tones
// in noise at an SNR quoted in 2.5 kHz; QSB and drift; and the character error rate of a decode.
#pragma once
#include "decoders/cw_skimmer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace cwsynth {
using cf = std::complex<float>;
using vibe::cw::morseCode;

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
    double jitter = 0;                 // a human fist: each element's length varies by this fraction (σ) — all of them
    double weight = 1.0;               // mark/space weighting: 1.1 = marks 10 % long (the gap inside a character pays)
    // ★★ HUMAN FISTS, each on its own (2026-10-25: "compensate for variable WPM for real human operators")
    double wpmEnd = 0;                 // > 0: the speed RAMPS linearly from wpm to this across the text (an operator speeding up)
    double dashRatio = 3.0;            // dash : dot (2.5–4 among real fists)
    double elemGap = 1.0, charGap = 3.0, wordGap = 7.0;   // spacings, in dots
    double dotJitter = -1, dashJitter = -1, gapJitter = -1;   // σ per kind (−1 = `jitter`); a bug: dots ≈ 0, dashes and gaps big
};
/** The key's envelope (0..1), sampled at `rate`, for `text` — then `tailSec` of silence. */
inline std::vector<float> keyEnvelope(const std::string& text, const Keying& k, double rate, Rng& rng, double tailSec = 0.5) {
    std::vector<std::pair<double, bool>> seg;   // (seconds, keyed)
    auto add = [&](double sec, bool on) { if (sec > 0) seg.push_back({ sec, on }); };
    const double jd = k.dotJitter >= 0 ? k.dotJitter : k.jitter, jh = k.dashJitter >= 0 ? k.dashJitter : k.jitter,
                 jg = k.gapJitter >= 0 ? k.gapJitter : k.jitter;
    auto jit = [&](double x, double j) { return j > 0 ? std::max(0.3 * x, x * (1.0 + j * rng.gauss())) : x; };
    add(0.3, false);
    for (size_t i = 0; i < text.size(); ++i) {
        // ★ The speed at this point of the text (a ramp), and Farnsworth's stretched gaps
        const double w = k.wpmEnd > 0 ? k.wpm + (k.wpmEnd - k.wpm) * (double)i / std::max<size_t>(1, text.size() - 1) : k.wpm;
        const double unit = 1.2 / w;
        double gapUnit = unit;
        if (k.farnsworth > 0 && k.farnsworth < w) {
            // ARRL-style: the extra time is spread over the character and word gaps (19 of 50 units of PARIS)
            const double ta = (60.0 * w - 37.2 * k.farnsworth) / (k.farnsworth * w);
            gapUnit = ta / 19.0;   // seconds per gap unit
        }
        const char c = text[i];
        if (c == ' ') { add(jit(gapUnit * (k.wordGap - k.charGap), jg), false); continue; }   // the char gap is already there
        std::string code;
        if (c == '<') { const size_t e = text.find('>', i); code = morseCode(text.substr(i, e - i + 1)); i = e; }
        else code = morseCode(std::string(1, c));
        if (code.empty()) continue;
        for (size_t e = 0; e < code.size(); ++e) {
            const bool dash = code[e] == '-';
            const double m = (dash ? k.dashRatio : 1.0) * unit;
            add(jit(m * k.weight, dash ? jh : jd), true);
            if (e + 1 < code.size()) add(jit(unit * k.elemGap * (2.0 - k.weight), jg), false);
        }
        add(jit(gapUnit * k.charGap, jg), false);
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
inline void synth(std::vector<Tone>& tones, double rate, bool complexOut, double seconds, Rng& rng,
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


// ── human fists (shared by cw_bench and vibeserver/test-cw.cpp) ──
/** ★★ HUMAN FISTS (Stuart, 2026-10-25: "we also need to compensate for variable WPM for real human operators"): a speed
 *  ramp inside an over, a step between overs, a bug key, a straight key's jitter, odd weighting and spacing, Farnsworth.
 *  Each case is a QSO's worth of overs (≈ 60 characters each, 1.5 s apart), 48 kHz IQ. */
struct FistCase { const char* name; std::vector<Keying> overs; };
/** A QSO's four overs, in turn. */
static const std::string kOvers[4] = { "CQ CQ DE G4ABC G4ABC K  ", "G4ABC DE DL1XYZ GM UR RST 579 579 NAME HANS QTH BERLIN HW? BK  ",
                                       "DL1XYZ DE G4ABC R TNX FER CALL UR 599 OP JOHN WX FINE TEMP 12C  ", "G4ABC DE DL1XYZ FB JOHN TNX QSO 73 ES GL SK  " };
inline std::vector<FistCase> fistCases() {
    auto K = [](double w) { Keying k; k.wpm = w; return k; };
    auto rep = [](const Keying& k, int n = 8) { return std::vector<Keying>((size_t)n, k); };
    std::vector<FistCase> v;
    v.push_back({ "clean 20 WPM (reference)", rep(K(20)) });
    { Keying k = K(15); k.wpmEnd = 25; v.push_back({ "ramp 15→25 WPM in each over", rep(k) }); }
    { Keying k = K(28); k.wpmEnd = 16; v.push_back({ "ramp 28→16 WPM in each over", rep(k) }); }
    { std::vector<Keying> o; for (double w : { 15, 28, 18, 32, 14, 26, 20, 35 }) o.push_back(K(w));
      v.push_back({ "step between overs 15/28/18/32/14/26/20/35", o }); }
    { Keying k = K(24); k.dotJitter = 0.02; k.dashJitter = 0.25; k.gapJitter = 0.25; k.dashRatio = 3.6;
      v.push_back({ "bug key (dots exact, dashes/gaps ±25 %)", rep(k) }); }
    for (double j : { 0.10, 0.15, 0.20, 0.30 }) { Keying k = K(18); k.jitter = j; k.riseMs = 3;
      static char nm[4][48]; const int i = j < 0.12 ? 0 : j < 0.17 ? 1 : j < 0.25 ? 2 : 3;
      std::snprintf(nm[i], sizeof nm[i], "straight key, σ %.0f %% per element", j * 100); v.push_back({ nm[i], rep(k) }); }
    { Keying k = K(22); k.dashRatio = 2.5; v.push_back({ "dash:dot 2.5:1", rep(k) }); }
    { Keying k = K(22); k.dashRatio = 4.0; v.push_back({ "dash:dot 4:1", rep(k) }); }
    { Keying k = K(22); k.weight = 1.3; v.push_back({ "heavy keying (marks +30 %)", rep(k) }); }
    { Keying k = K(22); k.weight = 0.75; v.push_back({ "light keying (marks −25 %)", rep(k) }); }
    { Keying k = K(22); k.elemGap = 1.5; v.push_back({ "element gaps 1.5 dots", rep(k) }); }
    { Keying k = K(22); k.elemGap = 0.7; v.push_back({ "element gaps 0.7 dot", rep(k) }); }
    { Keying k = K(22); k.charGap = 5; k.wordGap = 10; k.gapJitter = 0.25; v.push_back({ "long, uneven char/word gaps", rep(k) }); }
    { Keying k = K(25); k.farnsworth = 12; v.push_back({ "Farnsworth 25/12", rep(k) }); }
    { Keying k = K(15); k.wpmEnd = 25; k.jitter = 0.15; k.dashRatio = 3.4; k.weight = 1.1;
      v.push_back({ "all at once: ramp, σ 15 %, 3.4:1, heavy", rep(k) }); }
    return v;
}
/** One fist case's whole QSO (48 kHz), each over followed by 1.5 s of silence; `sent` gets the text. */
inline std::vector<float> fistEnvelope(const FistCase& fc, Rng& rng, std::string& sent) {
    std::vector<float> env;
    for (size_t i = 0; i < fc.overs.size(); ++i) {
        auto e = keyEnvelope(kOvers[i % 4], fc.overs[i], 48000, rng, 1.5);
        env.insert(env.end(), e.begin(), e.end());
        sent += kOvers[i % 4];
    }
    return env;
}

// ── scoring: semi-global edit distance (the decoder may join late and stop early) ──
inline std::string squash(const std::string& s) {
    std::string o; bool sp = true;
    for (char c : s) { if (c == ' ' || c == '\n') { if (!sp) o += ' '; sp = true; } else { o += c; sp = false; } }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
/** Character errors of `got` against the best-matching span of `ref`, and that span's length. Spaces count. */
inline void cer(const std::string& refIn, const std::string& gotIn, int& errs, int& refLen) {
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

/** ★★ WORDS SPLIT AND MERGED (V12, 2026-10-29): over the best alignment of `got` with `ref` (as cer), the word spaces
 *  the decoder ADDED (a word broken: "E E BT") and the ones it LOST (words run together: "OPISRICHARD"). */
inline void spaceErrors(const std::string& refIn, const std::string& gotIn, int& split, int& merged, int& refSpaces) {
    const std::string ref = squash(refIn), got = squash(gotIn);
    const size_t R = ref.size(), G = got.size();
    split = merged = 0; refSpaces = 0;
    if (!G || !R) return;
    // full DP with backtrace; free start/end in ref (semi-global)
    std::vector<std::vector<int>> D(G + 1, std::vector<int>(R + 1, 0));
    for (size_t i = 1; i <= G; ++i) D[i][0] = (int)i;
    for (size_t i = 1; i <= G; ++i)
        for (size_t j = 1; j <= R; ++j)
            D[i][j] = std::min({ D[i - 1][j - 1] + (got[i - 1] == ref[j - 1] ? 0 : 1), D[i - 1][j] + 1, D[i][j - 1] + 1 });
    size_t j = 0; for (size_t k = 1; k <= R; ++k) if (D[G][k] < D[G][j]) j = k;
    size_t i = G; const size_t jEnd = j;
    while (i > 0 && j > 0) {
        if (D[i][j] == D[i - 1][j - 1] + (got[i - 1] == ref[j - 1] ? 0 : 1)) {
            if (got[i - 1] != ref[j - 1]) { if (got[i - 1] == ' ') ++split; else if (ref[j - 1] == ' ') ++merged; }
            if (ref[j - 1] == ' ') ++refSpaces;
            --i; --j;
        } else if (D[i][j] == D[i - 1][j] + 1) { if (got[i - 1] == ' ') ++split; --i; }
        else { if (ref[j - 1] == ' ') { ++merged; ++refSpaces; } --j; }
    }
    (void)jEnd;
}

} // namespace cwsynth
