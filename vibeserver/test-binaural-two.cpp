// ★★★ BINAURAL BETWEEN TWO SIGNALS (V12, 2026-10-18) — vibedsp/binaural.h, the whole controller, with TWO signals in the
//     passband: one BELOW the dial, one ABOVE.
//
// Stuart on iOS 483: "the binaural is acting odd when in between 2 signals rather than putting the one below us in the left
// ear and the one above us in the right it is blending them both and swirling them about between the ears. This is on CW
// too." His spec (binaural.h): "if say you are halfway between 2 signals you get one in the left and one in the right" ·
// "a crowded band gives you stereo separation of the signals". Higher RF to the right in every sideband.
//
// Each signal's PLACE is measured in the output, per 200 ms window while that signal is sounding, in a band where it alone
// has energy: its level difference (R over L, dB — + = right) and, below ~900 Hz where a phase difference cannot wrap,
// its time difference (µs, + = right). Per signal: the mean, the robust sd (1.4826 × MAD), the share of windows on the
// WRONG side (|ILD| > 0.5 dB the wrong way) and the reversals per minute (swings of ≥ 1.5 dB each way).
//
// CASES, each for USB, LSB and CW: (a) both at once, (b) taking turns (2-5 s overs, a QSO), (c) one 15 dB stronger.
//   SSB synthetic — transmitter-limited voices (300-2700 Hz before the shift), carriers ±600 and ±1000 Hz from the dial;
//                   once with the estimator alone, once with the waterfall cue following whoever talks (what the app's
//                   SsbCoarse does: "the voice overlapping the passband most").
//   CW synthetic  — two keyed carriers at the note ±300 Hz in a 1 kHz filter, and ±150 Hz in the default 400 Hz one; the
//                   app's CW peak tracker (binauralOffset.ts BinauralTracker, ported below) feeding setOffset.
//   REAL (VIBE_TEST_AUDIO=~/VibeSDR-test-audio):
//     • Stuart's own CW recording on 483 (binaural/cw2sig_483.wav — tapped BEFORE binaural, so L == R is the clean input):
//       7.0152 MHz CW-U, tones 387 Hz (strongest) and 715 Hz (−7 dB) either side of a ~600 Hz note, noise 0-1200 Hz;
//     • two off-air VOLMET broadcasts (ssb/offsets) — one recorded with the dial 500 / 1000 Hz ABOVE its carrier (so it
//       lies below the dial), the other with the dial that far BELOW its own: mixed, both at once and in turns. As LSB the
//       same audio is the mirror case (a USB clip's audio shifted down is an LSB station ABOVE the dial).
// PASS = the lower signal sits LEFT and the upper RIGHT, steadily, in every case: mean |ILD| ≥ 1.5 dB the right way, no more
// than 3 % of windows the wrong way, ≤ 6 reversals a minute; and the ITD, where measured, never the wrong way on average.
//
// ★ SILENT: everything is generated or read and measured in memory, never played.
#include "vibedsp/binaural.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace vibedsp;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// ── signals ──────────────────────────────────────────────────────────────────────────────────────────────────────

/* Band noise (a windowed-sinc band-pass on white noise): the receiver's own noise through its filter. */
static std::vector<float> bandNoise(double rate, size_t n, double f1, double f2, double amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, 1.0f);
    const int taps = 255;
    std::vector<double> hk(taps);
    for (int i = 0; i < taps; ++i) {
        const double m = i - (taps - 1) / 2.0;
        auto sinc = [&](double fc) { return m == 0 ? 2 * fc / rate : std::sin(2 * M_PI * fc / rate * m) / (M_PI * m); };
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        hk[i] = (sinc(f2) - sinc(f1)) * w;
    }
    std::vector<float> wn(n + taps), x(n);
    for (auto& v : wn) v = g(rng);
    for (size_t i = 0; i < n; ++i) { double a = 0; for (int k = 0; k < taps; ++k) a += hk[k] * wn[i + k]; x[i] = (float)(amp * a); }
    return x;
}

/* A TRANSMITTED SSB voice as the receiver hears it: harmonics of a gliding pitch, only those inside the transmitter's
 * 300-2700 Hz (a real voice is band-limited BEFORE it is mistuned — test-binaural's voice() is not, so two of those would
 * both fill the whole passband), moved by `shiftHz` (+ = the audio HIGH), kept inside the receiver's 50-2700 Hz, spoken in
 * syllables. `on(t)` gates it (overs). No noise: the mix adds the receiver's once. */
template <class On>
static std::vector<float> txVoice(double rate, size_t n, double shiftHz, double f0c, unsigned seed, double amp, On on) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 6.2831853f);
    std::vector<float> x(n);
    float th[60];
    for (auto& t : th) t = u(rng);
    const double sylHz = 2.6 + 0.8 * (seed % 5) / 4.0;
    double ph = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / rate;
        const double sc = f0c / 135.0;
        const double f0 = f0c + sc * 35 * std::sin(2 * M_PI * 0.23 * t + seed) + sc * 12 * std::sin(2 * M_PI * 1.7 * t + 1.0);
        ph += 2 * M_PI * f0 / rate;
        if (!on(t)) continue;
        const double syl = std::max(0.0, std::sin(2 * M_PI * sylHz * t));
        double s = 0;
        for (int k = 1; k < 60; ++k) {
            const double v = k * f0;
            if (v < 300 || v > 2700) continue;          // the transmitter's filter
            const double a = v + shiftHz;
            if (a < 50 || a > 2700) continue;           // the receiver's
            s += std::cos(k * ph + 2 * M_PI * shiftHz * t + th[k]) / std::sqrt((double)k);
        }
        x[i] = (float)(amp * s * syl);
    }
    return x;
}

/* A keyed CW carrier: elements of 60-180 ms with 60 ms spaces (random), soft 5 ms edges. `on(t)` gates it (overs). */
template <class On>
static std::vector<float> cwKeyed(double rate, size_t n, double hz, double amp, unsigned seed, On on) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> el(1, 3);
    std::vector<float> key(n, 0.0f);
    size_t i = 0;
    while (i < n) {
        const size_t mark = (size_t)(0.06 * el(rng) * rate), space = (size_t)(0.06 * rate * (rng() % 4 == 0 ? 3 : 1));
        for (size_t k = 0; k < mark && i + k < n; ++k) key[i + k] = 1;
        i += mark + space;
    }
    // soft edges: a 5 ms moving average
    const int e = (int)(0.005 * rate);
    std::vector<float> x(n);
    double acc = 0;
    for (size_t k = 0; k < n; ++k) {
        acc += key[k]; if (k >= (size_t)e) acc -= key[k - e];
        const double g = on(k / rate) ? acc / e : 0.0;
        x[k] = (float)(amp * g * std::sin(2 * M_PI * hz * k / rate));
    }
    return x;
}

// Overs: who is talking at time t. Alternating A, B, A, …, 2-5 s each, 0.6 s gaps. Deterministic per seed.
struct Overs {
    std::vector<double> start, end; std::vector<int> who;
    Overs(double secs, unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> len(2.0, 5.0);
        double t = 0.3; int w = 0;
        while (t < secs) { const double l = len(rng); start.push_back(t); end.push_back(t + l); who.push_back(w); t += l + 0.6; w ^= 1; }
    }
    bool on(int w, double t) const {
        for (size_t i = 0; i < start.size(); ++i) if (who[i] == w && t >= start[i] && t < end[i]) return true;
        return false;
    }
};

// ── the measurement ──────────────────────────────────────────────────────────────────────────────────────────────

struct Place { double mean = 0, sd = 0, wrong = 0, revPerMin = 0, itdUs = 0; int windows = 0; bool itd = false; };

static double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/* One signal's place in the output: band [f1, f2] where it alone has energy; `comp` = that signal alone (the input
 * component), used only to find the windows where it sounds; D = the chain's latency (samples). `want` = −1 left, +1 right. */
static Place measure(const std::vector<float>& L, const std::vector<float>& R, const std::vector<float>& comp, double rate,
                     double f1, double f2, int D, double fromSec, int want) {
    const int W = (int)(0.2 * rate);
    int N = 1; while (N < W) N <<= 1;
    RealFFT fft(N);
    std::vector<float> buf(N);
    std::vector<cf32> sL(N / 2 + 1), sR(N / 2 + 1), sC(N / 2 + 1);
    const double binHz = rate / N;
    const int k1 = std::max(1, (int)std::ceil(f1 / binHz)), k2 = std::min(N / 2, (int)std::floor(f2 / binHz));
    struct Win { double ild, pc, itd; bool itdOk; };
    std::vector<Win> ws;
    for (size_t a = (size_t)(fromSec * rate); a + W + D <= L.size() && a + W <= comp.size(); a += W) {
        auto spec = [&](const std::vector<float>& x, size_t off, std::vector<cf32>& out) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < W; ++i) buf[i] = x[off + i] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / W));
            fft.forward(buf.data(), out.data());
        };
        spec(L, a + D, sL); spec(R, a + D, sR); spec(comp, a, sC);
        double pl = 0, pr = 0, pc = 0, num = 0, den = 0;
        for (int k = k1; k <= k2; ++k) {
            pl += std::norm(sL[k]); pr += std::norm(sR[k]); pc += std::norm(sC[k]);
            const double f = k * binHz;
            if (f < 900) {
                const std::complex<double> c = std::complex<double>(sL[k]) * std::conj(std::complex<double>(sR[k]));
                const double ph = std::arg(c), w = std::abs(c);
                num += w * (-ph / (2 * M_PI * f)); den += w;     // L later than R by τ → L·R* turns by −ωτ → τ > 0 = sounds RIGHT
            }
        }
        ws.push_back({10 * std::log10((pr + 1e-30) / (pl + 1e-30)), pc, den > 0 ? num / den : 0, den > 0});
    }
    Place p;
    if (ws.empty()) return p;
    std::vector<double> pcs;
    for (auto& w : ws) pcs.push_back(w.pc);
    std::sort(pcs.begin(), pcs.end());
    const double thr = 0.1 * pcs[pcs.size() * 9 / 10];          // sounding: within 10 dB of its loud windows
    std::vector<double> ild; double itdS = 0; int itdN = 0, wrong = 0;
    for (auto& w : ws) {
        if (!(w.pc >= thr) || w.pc <= 0) continue;
        ild.push_back(w.ild);
        if (w.ild * want < -0.5) ++wrong;
        if (w.itdOk) { itdS += w.itd; ++itdN; }
    }
    p.windows = (int)ild.size();
    if (ild.empty()) return p;
    double s = 0; for (double v : ild) s += v;
    p.mean = s / ild.size();
    const double med = medianOf(ild);
    std::vector<double> dev; for (double v : ild) dev.push_back(std::fabs(v - med));
    p.sd = 1.4826 * medianOf(dev);
    p.wrong = (double)wrong / ild.size();
    // reversals of the ILD series, swings ≥ 1.5 dB each way
    double ext = ild[0]; int dir = 0, revs = 0;
    for (double v : ild) {
        if (dir == 0) { if (v - ext >= 1.5) { dir = 1; ext = v; } else if (ext - v >= 1.5) { dir = -1; ext = v; } }
        else if (dir > 0) { if (v > ext) ext = v; else if (ext - v >= 1.5) { ++revs; dir = -1; ext = v; } }
        else { if (v < ext) ext = v; else if (v - ext >= 1.5) { ++revs; dir = 1; ext = v; } }
    }
    p.revPerMin = revs / (ild.size() * 0.2 / 60.0);
    p.itd = itdN > 0 && f1 < 900; p.itdUs = itdN ? 1e6 * itdS / itdN : 0;
    return p;
}

static bool placedRight(const Place& p, int want) {
    return p.windows >= 5 && p.mean * want >= 1.5 && p.wrong <= 0.03 && p.revPerMin <= 6.0 && (!p.itd || p.itdUs * want >= 0);
}

static void show(const char* who, const Place& p) {
    std::printf("        %-6s ILD %+6.2f dB (sd %.2f) · wrong side %4.1f %% · %4.1f rev/min · ITD %s%+5.0f µs · %d windows\n", who,
                p.mean, p.sd, 100 * p.wrong, p.revPerMin, p.itd ? "" : "(n/a) ", p.itd ? p.itdUs : 0.0, p.windows);
}

// ── the app's CW peak tracker (binauralOffset.ts BinauralTracker.onFrame, 'peak' kind) ─────────────────────────────────
/* ★ Ported faithfully so the controller gets what the app sends: per 100 ms spectrum frame the strongest bin in the
 *   passband, ≥ 10 dB over the passband's median; a jump of > max(3 bins, 60 Hz) believed only after 10 frames there;
 *   smoothed 0.5; held 1.5 s; pushed when it changes by ≥ 1 Hz. The spectrum here is the audio's own (10 Hz bins), the
 *   offset against `noteHz` — the passband centre the app measures against. */
struct CwTracker {
    double peakAbs = NAN, peakCand = NAN, smooth = NAN, lastOk = -1e9, sentHz = NAN; int candN = 0; bool sentOk = false;
    void frame(BinauralTuning& bt, const float* x, int n, double rate, double noteHz, double lo, double hi, double now) {
        int N = 1; while (N < n) N <<= 1;
        RealFFT fft(N);
        std::vector<float> b(N, 0.0f); std::vector<cf32> s(N / 2 + 1);
        for (int i = 0; i < n; ++i) b[i] = x[i] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / n));
        fft.forward(b.data(), s.data());
        const double binHz = rate / N;
        const int a = std::max(1, (int)std::ceil(lo / binHz)), z = std::min(N / 2 - 1, (int)std::floor(hi / binHz));
        std::vector<double> d;
        int pk = a;
        for (int k = a; k <= z; ++k) { d.push_back(10 * std::log10(std::norm(s[k]) + 1e-20)); if (std::norm(s[k]) > std::norm(s[pk])) pk = k; }
        std::vector<double> srt(d); std::sort(srt.begin(), srt.end());
        const double med = srt[srt.size() / 2];
        bool ok = d[pk - a] - med >= 10;
        double hz = 0;
        if (ok) {
            const double y0 = 10 * std::log10(std::norm(s[pk - 1]) + 1e-20), y1 = d[pk - a], y2 = 10 * std::log10(std::norm(s[pk + 1]) + 1e-20);
            const double den = y0 - 2 * y1 + y2;
            double dd = den < 0 ? 0.5 * (y0 - y2) / den : 0;
            dd = std::max(-0.5, std::min(0.5, dd));
            const double abs = (pk + dd) * binHz;
            const double jump = std::max(3 * binHz, 60.0);
            if (std::isnan(peakAbs) || std::fabs(abs - peakAbs) <= jump) { peakAbs = abs; candN = 0; }
            else {
                if (!std::isnan(peakCand) && std::fabs(abs - peakCand) <= jump) ++candN; else { peakCand = abs; candN = 1; }
                if (candN >= 10) { peakAbs = abs; candN = 0; smooth = NAN; } else ok = false;
            }
            hz = abs - noteHz;
        }
        if (ok) { smooth = std::isnan(smooth) ? hz : smooth + 0.5 * (hz - smooth); lastOk = now; }
        const bool valid = !std::isnan(smooth) && now - lastOk <= 1.5;
        const double out = valid ? smooth : 0;
        if (valid != sentOk || (valid && !(std::fabs(out - sentHz) < 1))) { bt.setOffset((float)out, valid); sentOk = valid; sentHz = out; }
    }
};

// ── running the controller ───────────────────────────────────────────────────────────────────────────────────────

/* Mono in → L, R out through one BinauralTuning, in 20 ms packets, the SSB worker pumped every 40 ms. `perFrame(t)` runs
 * every 100 ms before the packet (the app's spectrum callback: the CW tracker, the waterfall cue). */
template <class F>
static void runChain(BinauralTuning& bt, const std::vector<float>& x, int rate, std::vector<float>& L, std::vector<float>& R, F perFrame) {
    const int pkt = rate / 50;
    L.assign(x.size(), 0.0f); R.assign(x.size(), 0.0f);
    for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
        const size_t pi = i / pkt;
        if (pi % 5 == 0) perFrame(i);
        std::copy(x.begin() + i, x.begin() + i + pkt, L.begin() + i);
        std::copy(x.begin() + i, x.begin() + i + pkt, R.begin() + i);
        bt.processPlanar(&L[i], &R[i], pkt, rate);
        if (pi % 2 == 1) bt.pumpSsbForTest();
    }
}

static int latencyFor(int rate) { int n = 128; while (n < 4096 && (double)rate / n > BinauralSpread::kBinHz) n <<= 1; return n; }

struct Verdict { int cases = 0, ok = 0; };

/* One two-signal case through the chain, both signals measured and judged. */
static void judge(Verdict& v, const char* label, const std::vector<float>& L, const std::vector<float>& R,
                  const std::vector<float>& lowC, double l1, double l2, const std::vector<float>& highC, double h1, double h2,
                  int rate, double fromSec, bool strict = true) {
    const int D = latencyFor(rate);
    const Place lo = measure(L, R, lowC, rate, l1, l2, D, fromSec, -1), hi = measure(L, R, highC, rate, h1, h2, D, fromSec, +1);
    const bool ok = placedRight(lo, -1) && placedRight(hi, +1);
    std::printf("      %s%s\n", label, ok ? "" : strict ? "   ← NOT PLACED" : "   (reported)");
    show("below", lo); show("above", hi);
    if (strict) { ++v.cases; if (ok) ++v.ok; }
}

// ── real audio ───────────────────────────────────────────────────────────────────────────────────────────────────

// 16-bit PCM WAV (WAVE_FORMAT_EXTENSIBLE too — the format tag is not checked). Mono or the LEFT of stereo, as float.
static bool readWavMono(const std::string& path, std::vector<float>& x, int& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::fclose(f); return false; }
    int bits = 0, ch = 0; rate = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) { std::fclose(f); return false; }
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len);
            if (std::fread(b.data(), 1, len, f) != len) { std::fclose(f); return false; }
            ch = b[2] | (b[3] << 8); rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24)); bits = b[14] | (b[15] << 8);
        } else if (!std::memcmp(id, "data", 4)) {
            if (bits != 16 || ch < 1 || ch > 2) { std::fclose(f); return false; }
            std::vector<int16_t> s(len / 2);
            s.resize(std::fread(s.data(), 2, s.size(), f));
            std::fclose(f);
            x.resize(s.size() / ch);
            for (size_t i = 0; i < x.size(); ++i) x[i] = s[i * ch] / 32768.0f;
            return !x.empty();
        } else {
            std::fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
}

/* A narrow band-pass's output power around `hz` (±bw) per window → which windows a tone sounds in, as a "component" for
 * measure(): the input itself, band-limited (real recordings have no separate components). */
static std::vector<float> toneComponent(const std::vector<float>& x, double rate, double hz, double bw) {
    // a 2nd-order resonator, run forward: enough to say WHEN the tone sounds (measure() only thresholds its power)
    const double r = std::exp(-M_PI * bw / rate), c = 2 * r * std::cos(2 * M_PI * hz / rate);
    std::vector<float> y(x.size());
    double y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { const double v = (1 - r) * x[i] + c * y1 - r * r * y2; y2 = y1; y1 = v; y[i] = (float)v; }
    return y;
}

int main() {
    std::printf("Binaural — between two signals\n");
    Verdict v;
    const int fs = 48000;
    const double secs = 40;
    const size_t n = (size_t)(fs * secs);
    const double from = 3.0;   // judged from 3 s in: the estimator's first answers and the CW note measured

    // ════ SSB, synthetic ════
    for (int lsb = 0; lsb < 2; ++lsb) {
        for (double sep : {600.0, 1000.0}) {
            // Signal BELOW the dial at −sep, ABOVE at +sep (RF). Audio shift: USB +Δ, LSB −Δ.
            const double sLow = lsb ? sep : -sep, sHigh = lsb ? -sep : sep;
            // Each one's own band (audio): where only it has energy (tx 300-2700 + shift, inside 50-2700).
            auto bandOf = [&](double shift) { return std::make_pair(std::max(80.0, 300 + shift), std::min(2650.0, 2700 + shift)); };
            const auto bl = bandOf(sLow), bh = bandOf(sHigh);
            // exclusive parts
            const double lowA = lsb ? std::max(bl.first, bh.second + 80) : bl.first, lowB = lsb ? bl.second : std::min(bl.second, bh.first - 80);
            const double hiA = lsb ? bh.first : std::max(bh.first, bl.second + 80), hiB = lsb ? std::min(bh.second, bl.first - 80) : bh.second;
            for (int cs = 0; cs < 3; ++cs) {
                const Overs ov(secs, 11 + cs);
                auto allOn = [](double) { return true; };
                const double ampLow = 0.06, ampHigh = cs == 2 ? 0.06 * std::pow(10.0, -15 / 20.0) : 0.06;
                std::vector<float> low, high;
                if (cs == 1) {
                    low = txVoice(fs, n, sLow, 110, 21, ampLow, [&](double t) { return ov.on(0, t); });
                    high = txVoice(fs, n, sHigh, 160, 22, ampHigh, [&](double t) { return ov.on(1, t); });
                } else {
                    low = txVoice(fs, n, sLow, 110, 21, ampLow, allOn);
                    high = txVoice(fs, n, sHigh, 160, 22, ampHigh, allOn);
                }
                const auto nz = bandNoise(fs, n, 50, 2700, 0.004, 31);
                std::vector<float> x(n);
                for (size_t i = 0; i < n; ++i) x[i] = low[i] + high[i] + nz[i];
                for (int cue = 0; cue < 2; ++cue) {
                    BinauralTuning bt(false);
                    bt.setEnabled(true);
                    bt.setSource(lsb ? BinauralTuning::SsbLsb : BinauralTuning::SsbUsb, 50, 1000);
                    bt.setSpread(lsb ? BinauralSpread::Lsb : BinauralSpread::Usb, 50, 2700);
                    const double dial = 7100000;
                    bt.setDial(dial);
                    std::vector<float> L, R;
                    runChain(bt, x, fs, L, R, [&](size_t i) {
                        if (!cue) return;
                        // The waterfall cue follows the voice that fills the passband most — the one talking (both at once:
                        // the stronger; equal: it flips every couple of seconds as their averages cross).
                        const double t = (double)i / fs;
                        int who;
                        if (cs == 1) who = ov.on(0, t) ? 0 : ov.on(1, t) ? 1 : -1;
                        else if (cs == 2) who = 0;
                        else who = ((int)(t / 2.3)) % 2;
                        if (who < 0) return;   // a gap: the app holds its last reading
                        bt.setSsbHint(dial + (who == 0 ? -sep : sep), true);
                    });
                    char label[200];
                    std::snprintf(label, sizeof label, "%s carriers ±%.0f Hz, %s, %s:", lsb ? "LSB" : "USB", sep,
                                  cs == 0 ? "(a) both at once" : cs == 1 ? "(b) taking turns" : "(c) the lower 15 dB stronger",
                                  cue ? "waterfall cue following the talker" : "estimator alone");
                    judge(v, label, L, R, low, lowA, lowB, high, hiA, hiB, fs, from);
                }
            }
        }
    }

    // ════ CW, synthetic ════
    for (int narrow = 0; narrow < 2; ++narrow) {
        const double note = 600, off = narrow ? 150 : 300, lo = narrow ? 400 : 100, hi = narrow ? 800 : 1100;
        for (int cs = 0; cs < 3; ++cs) {
            const Overs ov(secs, 41 + cs);
            auto allOn = [](double) { return true; };
            const double aLow = 0.2, aHigh = cs == 2 ? 0.2 * std::pow(10.0, -15 / 20.0) : 0.2;
            std::vector<float> low, high;
            if (cs == 1) {
                low = cwKeyed(fs, n, note - off, aLow, 51, [&](double t) { return ov.on(0, t); });
                high = cwKeyed(fs, n, note + off, aHigh, 52, [&](double t) { return ov.on(1, t); });
            } else {
                low = cwKeyed(fs, n, note - off, aLow, 51, allOn);
                high = cwKeyed(fs, n, note + off, aHigh, 52, allOn);
            }
            const auto nz = bandNoise(fs, n, lo, hi, 0.03, 61);
            std::vector<float> x(n);
            for (size_t i = 0; i < n; ++i) x[i] = low[i] + high[i] + nz[i];
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);   // binauralPlan('cw'): dead max(15, 0.6 bin), full 300
            bt.setSpread(BinauralSpread::CwUpper, 50, 2700);
            bt.setDial(7015000);
            CwTracker tr;
            std::vector<float> L, R;
            runChain(bt, x, fs, L, R, [&](size_t i) {
                if (i < (size_t)(fs / 10)) return;
                tr.frame(bt, &x[i - fs / 10], fs / 10, fs, note, lo, hi, (double)i / fs);
            });
            char label[200];
            std::snprintf(label, sizeof label, "CW note ±%.0f Hz in a %s filter, %s:", off, narrow ? "400 Hz (the default)" : "1 kHz",
                          cs == 0 ? "(a) both at once" : cs == 1 ? "(b) taking turns" : "(c) the lower 15 dB stronger");
            judge(v, label, L, R, low, note - off - 40, note - off + 40, high, note + off - 40, note + off + 40, fs, from);
        }
    }

    // ════ REAL ════
    const char* dirEnv = std::getenv("VIBE_TEST_AUDIO");
    const std::string dir = dirEnv ? dirEnv : "";
    bool realRun = false;
    if (!dir.empty() && exists(dir + "/binaural/cw2sig_483.wav")) {
        realRun = true;
        std::printf(" ── REAL: Stuart's CW recording on 483 (7.0152 MHz CW-U, 387 + 715 Hz either side of the note) ──\n");
        std::vector<float> x; int rate = 0;
        if (readWavMono(dir + "/binaural/cw2sig_483.wav", x, rate) && rate > 0) {
            BinauralTuning bt(false);
            bt.setEnabled(true);
            bt.setSource(BinauralTuning::Given, 15, 300);
            bt.setSpread(BinauralSpread::CwUpper, 50, 2700);
            bt.setDial(7015200);
            CwTracker tr;
            std::vector<float> L, R;
            runChain(bt, x, rate, L, R, [&](size_t i) {
                if (i < (size_t)(rate / 10)) return;
                tr.frame(bt, &x[i - rate / 10], rate / 10, rate, 600, 50, 1150, (double)i / rate);
            });
            float st[12]; bt.stats(st, 12);
            std::printf("      native's measured CW note %.0f Hz\n", st[11]);
            const auto c387 = toneComponent(x, rate, 387, 30), c715 = toneComponent(x, rate, 715, 30);
            judge(v, "real CW, 387 Hz (below) + 715 Hz (above, −7 dB):", L, R, c387, 350, 425, c715, 680, 750, rate, 2.0);
        } else check(false, "read binaural/cw2sig_483.wav");
    }
    if (!dir.empty() && exists(dir + "/ssb/offsets/shannon-5505")) {
        realRun = true;
        std::printf(" ── REAL: two off-air VOLMET broadcasts, one below the dial, one above ──\n");
        // dial OFF above a carrier → the voice is BELOW the dial; its audio is shifted −OFF (USB).
        struct Pair { const char* low; const char* high; double off; };
        const Pair pairs[] = {
            {"shannon-5505/5505000_usb_dial500.wav", "raf-5450/5450000_usb_dial-500.wav", 500},
            {"shannon-5505/5505000_usb_dial1000.wav", "shannon-3413/3413000_usb_dial-1000.wav", 1000},
            {"raf-5450/5450000_usb_dial1000.wav", "shannon-5505/5505000_usb_dial-1000.wav", 1000},
        };
        for (const Pair& p : pairs) {
            std::vector<float> a, b; int ra = 0, rb = 0;
            if (!readWavMono(dir + "/ssb/offsets/" + p.low, a, ra) || !readWavMono(dir + "/ssb/offsets/" + p.high, b, rb) || ra != rb) {
                check(false, "read a VOLMET pair"); continue;
            }
            const size_t m = std::min({a.size(), b.size(), (size_t)(ra * 90)});
            a.resize(m); b.resize(m);
            for (int lsb = 0; lsb < 2; ++lsb) {
                for (int cs = 0; cs < 3; ++cs) {
                    const Overs ov(m / (double)ra, 71 + cs);
                    // As LSB, the same audio is the mirror case: a USB clip whose audio is shifted DOWN (a voice below the
                    // dial on USB) is, heard as LSB, a voice ABOVE the dial. So on LSB `b` is the lower signal.
                    const std::vector<float>& lowSrc = lsb ? b : a;
                    const std::vector<float>& highSrc = lsb ? a : b;
                    std::vector<float> low(m), high(m), x(m);
                    const double gl = 1.0, gh = cs == 2 ? std::pow(10.0, -15 / 20.0) : 1.0;
                    for (size_t i = 0; i < m; ++i) {
                        const double t = i / (double)ra;
                        low[i] = (float)(gl * lowSrc[i] * (cs == 1 ? ov.on(0, t) : 1));
                        high[i] = (float)(gh * highSrc[i] * (cs == 1 ? ov.on(1, t) : 1));
                        x[i] = low[i] + high[i];
                    }
                    BinauralTuning bt(false);
                    bt.setEnabled(true);
                    bt.setSource(lsb ? BinauralTuning::SsbLsb : BinauralTuning::SsbUsb, 50, 1000);
                    bt.setSpread(lsb ? BinauralSpread::Lsb : BinauralSpread::Usb, 50, 2700);
                    bt.setDial(7100000);
                    std::vector<float> L, R;
                    runChain(bt, x, ra, L, R, [](size_t) {});
                    // exclusive bands: on USB the low one's audio is (300..2700)−OFF, the high one's (300..2700)+OFF.
                    const double off = p.off;
                    const double lowTop = 2700 - off, highBot = 300 + off;
                    double l1, l2, h1, h2;
                    if (!lsb) { l1 = 80; l2 = std::min(lowTop, highBot - 100); h1 = std::max(highBot, lowTop + 100); h2 = 2650; }
                    else      { h1 = 80; h2 = std::min(lowTop, highBot - 100); l1 = std::max(highBot, lowTop + 100); l2 = 2650; }
                    char label[240];
                    std::snprintf(label, sizeof label, "%s %s (below) + %s (above), ±%.0f Hz, %s:", lsb ? "LSB" : "USB",
                                  lsb ? p.high : p.low, lsb ? p.low : p.high, off,
                                  cs == 0 ? "(a) both at once" : cs == 1 ? "(b) taking turns" : "(c) the lower 15 dB stronger");
                    judge(v, label, L, R, low, l1, l2, high, h1, h2, ra, 5.0);
                }
            }
        }
    }
    if (!realRun) std::printf(" ── REAL: NOT RUN — set VIBE_TEST_AUDIO to the folder holding binaural/ and ssb/ ──\n");

    std::printf(" ── summary ──\n");
    char what[160];
    std::snprintf(what, sizeof what, "every two-signal case: the lower signal LEFT, the upper RIGHT, steadily (%d of %d placed)", v.ok, v.cases);
    check(v.ok == v.cases, what);
    std::printf(fails ? "\nFAILED: %d\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
