// test-digi-spots.cpp — DIGITAL SPOTS, V12 (2026-10-26): the mode registry, the band table, band stepping, the passband
// per band, Band mode's tuner, and a whole synthetic BAND decoded end to end through a DecoderHost — FT8 and FT4 on their
// own dials in one IQ slice, each spot out with its mode and its RF — and JS8 (Normal + Turbo) and FT2 on theirs. Plus the lower sideband (an FT8 signal BELOW the FT8
// dial must not decode as one above it), and pure noise (no spot at all).
//
//   test-digi-spots          the tests
//   test-digi-spots cpu      + the CPU of Band mode vs Single mode on this machine (docs/v12/DIGITAL-SPOTS.md)
#include "vibe_decoder_host.h"
#include "vibe_clock.h"
#include "decoders/digi_bands.h"
#include "decoders/digi_spotter.h"
#include "decoders/digi_tuner.h"
#include "decoders/digimodes/js8_decoder.h"
#include "decoders/digimodes/wspr_decoder.h"
#include "decoders/digi_activity.h"
#include "decoders/digi_governor.h"
#include "decoders/jtty_decoder.h"
#include "decoders/psk_skimmer.h"
#include "../tools/wspr-bench/wspr_synth.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static double cpuSecs() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

// ── signals ──────────────────────────────────────────────────────────────────────────────────────
/** One FT8 (or FT4) transmission's tones for `msg`, or empty. */
static std::vector<uint8_t> tonesFor(const char* msg, bool ft4) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) return {};
    std::vector<uint8_t> t(ft4 ? FT4_NN : FT8_NN);
    if (ft4) ft4_encode(m.payload, t.data()); else ft8_encode(m.payload, t.data());
    return t;
}
/** Add one transmission to a complex slice at `rate`, `offHz` from the slice centre, starting `startSec` in. Continuous
 *  phase FSK — the tone spacing and symbol length of FT8 (6.25 Hz, 0.16 s) or FT4 (20.833 Hz, 0.048 s). */
static void addTx(std::vector<cf32>& x, double rate, double offHz, double startSec, const std::vector<uint8_t>& tones,
                  bool ft4, double amp) {
    const double sym = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD, spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * rate), ns = (size_t)std::llround(sym * rate);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / rate;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}
/** Any FSK frame: `sym` seconds a symbol, tones `1/sym` apart (JS8's submodes, FT2's 24 ms). */
static void addFsk(std::vector<cf32>& x, double rate, double offHz, double startSec, const std::vector<uint8_t>& tones,
                   double sym, double amp) {
    const double spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * rate), ns = (size_t)std::llround(sym * rate);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / rate;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}
/** A JS8 heartbeat's 79 tones in `sub`. */
static std::vector<uint8_t> js8Heartbeat(const char* call, const char* grid, Js8Submode sub) {
    uint8_t b72[72], info[87]; std::vector<uint8_t> t(79);
    js8::packHeartbeat(call, grid, -1, b72); js8Info87(b72, js8::kFlagFirst | js8::kFlagLast, info); js8Tones(info, sub, t.data());
    return t;
}
static void addNoise(std::vector<cf32>& x, double sigma, unsigned seed) {
    std::mt19937 g(seed); std::normal_distribution<float> n(0.0f, (float)sigma);
    for (auto& v : x) v += cf32(n(g), n(g));
}

// ── V12 Band governor helpers (2026-10-27) ──────────────────────────────────────────────────────────────────────────
/** In-place radix-2 FFT (test-only: the server reads its bins off the forward FFT it already has). */
static void fftInPlace(std::vector<cf32>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) { cf32 w(1, 0); for (size_t j = 0; j < len / 2; ++j) { const cf32 u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
    }
}
/** ★ The slice's bins as the server makes them: an N-point FFT whose hop is 3/4 of it (the channeliser's overlap-save
 *  geometry), Hann-windowed in frequency by digi::slicePower — so a test drives the gate exactly as the shim does. */
struct BinMaker {
    int N; std::vector<cf32> hist, tmp; std::vector<float> pw;
    explicit BinMaker(int n = 512) : N(n), hist((size_t)n, cf32(0, 0)), pw((size_t)n) {}
    int hop() const { return N * 3 / 4; }
    const float* make(const cf32* x) {
        std::copy(hist.begin() + hop(), hist.end(), hist.begin());
        std::copy(x, x + hop(), hist.end() - hop());
        tmp = hist; fftInPlace(tmp);
        digi::slicePower(tmp.data(), N, 0, N, pw.data());
        return pw.data();
    }
};
/** Feed a slice through a Band spotter, with the bins (gate on) or without (gate off). */
static void feedBand(digi::Spotter& sp, const std::vector<cf32>& x, double rate, double centre, double utc0, bool bins,
                     bool yieldToWorkers = true) {
    BinMaker bm(512);
    const int hop = bm.hop();
    for (size_t o = 0; o + (size_t)hop <= x.size(); o += (size_t)hop) {
        const float* pw = bins ? bm.make(&x[o]) : nullptr;
        sp.feedIq(&x[o], hop, rate, centre, utc0 + (double)(o + hop) / rate, pw, bins ? bm.N : 0, rate / bm.N);
        if (yieldToWorkers && (o / hop) % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
}
/** A mode that decodes nothing and counts what it is fed (and whether it was told it is loaded), per gate period. */
struct CountShared { std::mutex m; std::map<long long, uint64_t> perPeriod; uint64_t total = 0, light = 0, full = 0; };
static digi::ModeDef countingMode(const char* name, double dialHz, const char* band, double slotSec, int prio, double cost,
                                  double light, std::shared_ptr<CountShared> cs, float lo = 100, float hi = 3500) {
    digi::ModeDef m; m.name = name; m.slotSec = slotSec; m.audioLoHz = lo; m.audioHiHz = hi; m.priority = prio;
    m.costVsFt8 = cost; m.lightCostVsFt8 = light; m.dials = { { band, dialHz } };
    struct E : digi::Engine {
        std::shared_ptr<CountShared> cs; double P;
        void process(const float*, int n, double utc) override {
            std::lock_guard<std::mutex> lk(cs->m);
            cs->perPeriod[(long long)std::floor((utc - 1e-3) / P)] += (uint64_t)n; cs->total += (uint64_t)n;
            if (loaded && loaded()) cs->light += (uint64_t)n; else cs->full += (uint64_t)n;
        }
    };
    const double P = slotSec > 0 ? slotSec : 4.0;
    m.make = [cs, P] { auto e = new E(); e->cs = cs; e->P = P; return std::unique_ptr<digi::Engine>(e); };
    return m;
}
/** A complex FSK burst of FT8's shape (8 tones 6.25 Hz apart, 0.16 s) at `snrDb` in 2500 Hz over noise σ per rail. */
static double ampFor(double snrDb, double sigma, double rate) { return std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / rate * 2500.0); }
/** ★ PSK31 (G3PLX): a ZERO is a phase reversal shaped by a cosine through zero, a ONE no change; Varicode with "00"
 *  between characters; idle (reversals) before and after. Complex baseband at `offHz` from the slice centre. */
static void addPsk31(std::vector<cf32>& x, double rate, double offHz, double startSec, const std::string& text, double amp) {
    std::string bits(64, '0');                                   // ★ two seconds of idle: the receiver locks on reversals
    for (char c : text) { const char* v = psk::varicode((unsigned char)c); if (v) { bits += v; bits += "00"; } }
    bits += std::string(32, '0') + std::string(16, '1');
    const double T = 1.0 / 31.25; const size_t ns = (size_t)std::llround(T * rate), s0 = (size_t)(startSec * rate);
    double cur = 1.0, ph = 0;
    for (size_t b = 0; b < bits.size(); ++b)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + b * ns + j; if (i >= x.size()) return;
            const double env = bits[b] == '0' ? cur * std::cos(M_PI * (double)j / (double)ns) : cur;
            ph += 2 * M_PI * offHz / rate;
            x[i] += cf32((float)(amp * env * std::cos(ph)), (float)(amp * env * std::sin(ph)));
            if (j + 1 == ns && bits[b] == '0') cur = -cur;
        }
}
/** ★ A JTTY transmission (WSJT-X 3.2): `text` packed into frames, 59 tones a frame at 31.25 baud (h = 1, phase-continuous). */
static void addJtty(std::vector<cf32>& x, double rate, double f0Off, double startSec, const char* text, double amp) {
    std::vector<uint64_t> frames; jtty::packMessage(text, frames);
    std::vector<uint8_t> tones;
    for (uint64_t f : frames) { uint8_t t[jtty::kFrameSyms]; jtty::encodeFrame(f, t); tones.insert(tones.end(), t, t + jtty::kFrameSyms); }
    addFsk(x, rate, f0Off, startSec, tones, 1.0 / jtty::kBaud, amp);
}

/** A decoder socket that keeps the spot frames. */
struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t*, size_t) override {}
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::vector<std::string> spots() { std::lock_guard<std::mutex> lk(m); std::vector<std::string> o; for (auto& t : txt) if (t.find("digital_spot") != std::string::npos) o.push_back(t); return o; }
};
static std::string field(const std::string& j, const char* k) {
    const std::string key = std::string("\"") + k + "\":";
    size_t p = j.find(key); if (p == std::string::npos) return "";
    p += key.size();
    if (j[p] == '"') { const size_t e = j.find('"', p + 1); return j.substr(p + 1, e - p - 1); }
    size_t e = p; while (e < j.size() && j[e] != ',' && j[e] != '}') ++e;
    return j.substr(p, e - p);
}

/** A mode with no engine of its own that never decodes — to cost spans with modes not yet built. */
static digi::ModeDef dummyMode(const char* name, float lo, float hi) {
    digi::ModeDef m; m.name = name; m.audioLoHz = lo; m.audioHiHz = hi;
    struct Quiet : digi::Engine { void process(const float*, int, double) override {} };
    m.make = [] { return std::unique_ptr<digi::Engine>(new Quiet()); };
    return m;
}

/** ★ `clip <out.cu8> <rate> <centreHz>`: 15 s of 20 m as an RTL delivers it (unsigned 8-bit IQ), UTC-ANCHORED — clip time
 *  t plays at UTC ≡ t (mod 15) under fake-rtl-tcp --hf-utc — for scripts/test-server-spots.mjs. FT8 at +0.5 s, FT4 at
 *  +0.5 and +8.0 s, one FT8 station on the lower sideband of the FT8 dial, a JS8 Normal heartbeat on the JS8 dial,
 *  an FT2 CQ on the FT2 dial, a JTTY CQ on 14.090 and a PSK31 CQ on 14.070. */
/** ★ V12 (2026-10-27) `clip40`: 40 m — an FT8 CQ on the main slice and a JTTY CQ on 7.090, which is on 40 m's EXTRA slice
 *  (beyond the 48 kHz cluster: digi_bands.h Span::extras), so the server's extra-slice route is driven end to end. */
static int writeClip40(const char* path, double rate, double centre) {
    std::vector<cf32> x((size_t)(rate * 15.0), cf32(0, 0));
    addTx(x, rate, 7074600 - centre, 0.5, tonesFor("CQ G4ABC IO92", false), false, 4.0);
    addJtty(x, rate, 7091500 - centre, 2.0, "CQ G4JTY CQ", 4.0);
    addNoise(x, 5.0, 23);
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].real())));
        u[2 * i + 1] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].imag())));
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    return 0;
}
/** ★ V12 (2026-10-27, v12-band-dc) `clip40dc`: 40 m with SIX FT8 CQs spread across the FT8 sub-band (audio 400…2900 Hz, so
 *  7.0744…7.0769 MHz) — where a dongle's DC lands when the tuner sits 15 kHz above 40 m's Band dial (7.0756): every one is
 *  within 1.3 kHz of it. scripts/test-server-band-dc.mjs. */
static const struct { const char* msg; double rf; } kClip40Dc[] = {
    { "CQ G4AAA IO92", 7074400 }, { "CQ DL1AAB JO31", 7074900 }, { "CQ K1AAC FN42", 7075400 },
    { "CQ F5AAD JN18", 7075900 }, { "CQ EA4AAE IN80", 7076400 }, { "CQ JA1AAF PM95", 7076900 } };
static int writeClip40Dc(const char* path, double rate, double centre, double amp) {
    std::vector<cf32> x((size_t)(rate * 15.0), cf32(0, 0));
    for (const auto& t : kClip40Dc) addTx(x, rate, t.rf - centre, 0.5, tonesFor(t.msg, false), false, amp);
    addNoise(x, 5.0, 29);
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].real())));
        u[2 * i + 1] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].imag())));
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    return 0;
}
static int writeClip(const char* path, double rate, double centre) {
    std::vector<cf32> x((size_t)(rate * 15.0), cf32(0, 0));
    struct Tx { const char* msg; bool ft4; double rf, start; };
    const Tx txs[] = { { "CQ G4ABC IO92", false, 14074600, 0.5 }, { "CQ DL1XYZ JO31", false, 14075450, 0.5 }, { "CQ K1ABC FN42", false, 14076300, 0.5 },
                       { "CQ JA1QRS PM95", true, 14080800, 0.5 }, { "CQ EA4TUV IN80", true, 14081700, 8.0 },
                       { "CQ ZL3LSB RE66", false, 14072800, 0.5 } };
    for (const Tx& t : txs) addTx(x, rate, t.rf - centre, t.start, tonesFor(t.msg, t.ft4), t.ft4, 4.0);
    // ★ JS8 Normal on its 14.078 dial (+0.5 s) and FT2 on its 14.084 dial (+0.1 s of the 3.75 s slot at 3.75)
    addFsk(x, rate, 14079000 - centre, 0.5, js8Heartbeat("G4JSN", "IO91", Js8Submode::Normal), 0.16, 4.0);
    addFsk(x, rate, 14084900 - centre, 3.85, tonesFor("CQ IU8AAA JN70", true), 0.024, 4.0);
    // ★ V12 2026-10-27: a JTTY CQ on the 14.090 dial (tone 0 at +1500 Hz, from +2 s) and a PSK31 CQ on 14.070 (+1200 Hz)
    addJtty(x, rate, 14091500 - centre, 2.0, "CQ G4JTY CQ", 4.0);
    addPsk31(x, rate, 14072400 - centre, 0.5, "CQ CQ DE G4PSK G4PSK K\n", 4.0);
    addNoise(x, 5.0, 21);
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].real())));
        u[2 * i + 1] = (uint8_t)std::max(0.0, std::min(255.0, std::round(127.5 + x[i].imag())));
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    std::printf("wrote %s: 15 s at %.0f Hz centred on %.0f\n", path, rate, centre);
    return 0;
}

/** ★ `real <iq.wav> <start_utc> <centreHz> [swap]`: a REAL recording — a stereo int16 I/Q WAV (ubersdr-rec … iq48), its first
 *  sample's UTC — through Band mode, faster than real time, stamped by the recording's own clock. Prints every spot, then
 *  the per-mode counts (compared by hand against the receiver's own decodes for the same minutes). */
static int runReal(const char* path, double startUtc, double centre, bool swap, bool live, bool gate = false, double budget = INFINITY,
                   double maxSec = 1e9) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0) { std::printf("not a WAV\n"); return 1; }
    uint32_t rate = 0; uint16_t chans = 0; size_t data = 0, dlen = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 2 || !rate || !data) { std::printf("need stereo I/Q (got %u ch, %u Hz)\n", chans, rate); return 1; }
    const size_t n = std::min(dlen / 4, (size_t)(maxSec * rate));
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; ++i) {
        const int16_t a = (int16_t)(raw[data + 4 * i] | raw[data + 4 * i + 1] << 8), b = (int16_t)(raw[data + 4 * i + 2] | raw[data + 4 * i + 3] << 8);
        x[i] = swap ? cf32(b / 32768.0f, a / 32768.0f) : cf32(a / 32768.0f, b / 32768.0f);
    }
    std::map<std::string, int> per; std::mutex gm; std::vector<std::string> lines; std::map<std::string, std::set<std::string>> uniq;
    digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& s, double rf) {
        char b[160]; std::snprintf(b, sizeof b, "%s %-10s %6s %+4d %.0f", s.mode.c_str(), s.call.c_str(), s.grid.c_str(), s.snr, rf);
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; lines.push_back(b); uniq[s.mode].insert(s.call); }, nullptr, nullptr);
    sp.setBudgetCap(budget);
    /* ★ `live`: fed in REAL TIME, its clock shifted by whole minutes so the slots still line up — the FT8 pass gate
     *  judges its time budget against the wall clock, so a recording fed minutes after it was made, or faster than real
     *  time, gets ONE pass (what a loaded server pays); fed live it gets what a live listener's would. */
    // ★ whole TWO-minute steps: WSPR's slots are the even minutes
    const double shift = live ? std::ceil((vibeUtcNow() - startUtc + 60.0) / 120.0) * 120.0 : 0.0;   // (+60: time to make the bins)
    /* ★ `gate`: the slice's bins with each block, as the server sends them (an FFT whose hop is 3/4 of it, ≈ 190 Hz bins).
     *  Made BEFORE the clock starts — the server reads them off the FFT it already has, so they are not Band's cost —
     *  and the blocks are the same 384 samples gated or not, so the two runs differ only by the gate. */
    BinMaker bm(512);
    const int hop = bm.hop();
    std::vector<float> bins;
    if (gate) { bins.reserve((n / hop + 1) * (size_t)bm.N); for (size_t o = 0; o + (size_t)hop <= n; o += (size_t)hop) { const float* p = bm.make(&x[o]); bins.insert(bins.end(), p, p + bm.N); } }
    const auto w0 = std::chrono::steady_clock::now();
    const double c0 = cpuSecs();
    for (size_t o = 0, b = 0; o + (size_t)hop <= n; o += (size_t)hop, ++b) {
        const int k = hop;
        if (live) std::this_thread::sleep_until(w0 + std::chrono::microseconds((int64_t)(1e6 * (double)(o + k) / rate)));
        const float* pw = gate ? &bins[b * (size_t)bm.N] : nullptr;
        sp.feedIq(&x[o], k, rate, centre, startUtc + shift + (double)(o + k) / rate, pw, gate ? bm.N : 0, rate / bm.N);
        if (!live && b % 64 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::printf("%s: %.0f s, engines %s, slots decoded %llu, skipped %llu, CPU %.2f s (%.3f %% of the recording's length)\n", path, n / (double)rate,
                sp.describe().c_str(), (unsigned long long)sp.slotsDecoded(), (unsigned long long)sp.slotsSkipped(), cpuSecs() - c0, 100.0 * (cpuSecs() - c0) / (n / (double)rate));
    if (!getenv("VIBE_QUIET")) for (auto& l : lines) std::printf("  %s\n", l.c_str());
    for (auto& kv : per) std::printf("%s: %d spots, %zu calls\n", kv.first.c_str(), kv.second, uniq[kv.first].size());
    for (auto& m : sp.status())
        std::printf("  status %-5s %-6s dials %d  periods: live %llu idle %llu shed %llu\n", m.mode.c_str(), digi::stateWord(m.state), m.dials,
                    (unsigned long long)m.live, (unsigned long long)m.idle, (unsigned long long)m.shed);
    if (const char* f = getenv("VIBE_CALLS_OUT")) {   // ★ every mode's calls, for comparing two runs
        FILE* o = std::fopen(f, "w");
        if (o) { for (auto& kv : uniq) for (auto& c : kv.second) std::fprintf(o, "%s %s\n", kv.first.c_str(), c.c_str()); std::fclose(o); }
    }
    return 0;
}

/** ★ `realusb <usb.wav> <start_utc> <dialHz> [live]`: the SAME minutes as USB audio (ubersdr-rec … usb, 12 kHz mono) through
 *  SINGLE mode — the A/B for Band's tuner: identical engines, only the way the audio was made differs. */
static int runRealUsb(const char* path, double startUtc, double dial, bool live) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 3; }
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    size_t data = 0, dlen = 0; uint32_t rate = 0; uint16_t chans = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 1 || rate != 12000 || !data) { std::printf("need 12 kHz mono\n"); return 1; }
    const size_t n = dlen / 2;
    std::vector<float> a48(n * 4);
    for (size_t i = 0; i < n; ++i) { const float v = (int16_t)(raw[data + 2 * i] | raw[data + 2 * i + 1] << 8) / 32768.0f; for (int k = 0; k < 4; ++k) a48[4 * i + k] = v; }
    std::map<std::string, int> per; std::mutex gm; std::vector<std::string> lines;
    digi::Spotter sp(digi::Spotter::Scope::Single, digi::modes(), [&](const digi::Spot& s, double rf) {
        char b[160]; std::snprintf(b, sizeof b, "%s %-10s %6s %+4d %.0f", s.mode.c_str(), s.call.c_str(), s.grid.c_str(), s.snr, rf);
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; lines.push_back(b); }, [dial] { return dial; }, nullptr);
    const double shift = live ? std::ceil((vibeUtcNow() - startUtc) / 60.0) * 60.0 : 0.0;
    const auto w0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o < a48.size(); o += 3840) {
        const int k = (int)std::min<size_t>(3840, a48.size() - o);
        if (live) std::this_thread::sleep_until(w0 + std::chrono::microseconds((int64_t)(1e6 * (double)(o + k) / 48000.0)));
        sp.feedAudio48k(&a48[o], k, startUtc + shift + (double)(o + k) / 48000.0);
        if (!live && (o / 3840) % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::printf("%s: Single, slots decoded %llu\n", path, (unsigned long long)sp.slotsDecoded());
    for (auto& l : lines) std::printf("  %s\n", l.c_str());
    for (auto& kv : per) std::printf("%s: %d spots\n", kv.first.c_str(), kv.second);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 4 && std::strcmp(argv[1], "realusb") == 0) return runRealUsb(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc > 5 && !std::strcmp(argv[5], "live"));
    if (argc > 4 && std::strcmp(argv[1], "clip") == 0) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    if (argc > 4 && std::strcmp(argv[1], "clip40") == 0) return writeClip40(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    if (argc > 4 && std::strcmp(argv[1], "clip40dc") == 0) return writeClip40Dc(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc > 5 ? std::atof(argv[5]) : 4.0);
    if (argc > 4 && std::strcmp(argv[1], "real") == 0) {
        bool swap = false, live = false, gate = false; double budget = INFINITY, maxSec = 1e9;
        for (int i = 5; i < argc; ++i) {
            if (!std::strcmp(argv[i], "swap")) swap = true;
            if (!std::strcmp(argv[i], "live")) live = true;
            if (!std::strcmp(argv[i], "gate")) gate = true;                       // ★ V12: the activity gate (bins with the IQ)
            if (!std::strncmp(argv[i], "budget=", 7)) budget = std::atof(argv[i] + 7);   // ★ a budget in FT8 units
            if (!std::strncmp(argv[i], "secs=", 5)) maxSec = std::atof(argv[i] + 5);
        }
        return runReal(argv[2], std::atof(argv[3]), std::atof(argv[4]), swap, live, gate, budget, maxSec);
    }
    const bool cpu = argc > 1 && std::strcmp(argv[1], "cpu") == 0;
    std::printf("── the registry ──\n");
    {
        const auto& ms = digi::modes();
        ok(ms.size() >= 4 && ms[0].name == "FT8" && ms[1].name == "FT4", "FT8 and FT4 are registered, FT8 first");
        const digi::ModeDef* js8 = digi::modeByName("JS8"); const digi::ModeDef* ft2 = digi::modeByName("FT2");
        ok(js8 && ft2 && js8->slotSec == 15.0 && ft2->slotSec == 3.75 && js8->single && ft2->single && js8->dials.empty() && ft2->dials.empty(),
           "JS8 (15 s; its engine cuts all four submodes) and FT2 (3.75 s) registered — Single and Band, dials from the table");
        ok(digi::modeByName("ft4") == &ms[1] && digi::modeByName("nope") == nullptr, "modeByName is case-insensitive and null for unknown");
        ok(ms[0].slotSec == 15.0 && ms[1].slotSec == 7.5 && ms[0].sampleRate == 12000, "FT8 15 s / FT4 7.5 s slots, 12 kHz audio");
        const digi::ModeDef* w = digi::modeByName("WSPR");
        ok(w && w->slotSec == 120.0 && w->sampleRate == 12000 && w->audioLoHz <= 1380 && w->audioHiHz >= 1620 && w->singleDialTolHz > 0,
           "WSPR is registered (digimodes/wspr.h): 120 s slots, 12 kHz, its 200 Hz sub-band, Single only at its dials");
        bool made = true;
        for (const auto& m : ms) { auto e = m.make(); made = made && e != nullptr; }
        ok(made, "every registered mode makes an engine");
    }

    std::printf("── Single mode runs only the modes AT THE DIAL (the cheap button) ──\n");
    {
        const auto& ms = digi::modes();
        auto at = [&](double hz) { std::string o; for (auto& n : digi::Spotter::singleModesAt(ms, hz)) o += (o.empty() ? "" : " ") + n; return o; };
        ok(at(14074000) == "FT8", "14.074 → FT8 only (" + at(14074000) + ")");
        ok(at(14078000) == "JS8", "14.078 → JS8 only (" + at(14078000) + ")");
        ok(at(14080000) == "FT4", "14.080 → FT4 only (" + at(14080000) + ")");
        ok(at(14084000) == "FT2", "14.084 → FT2 only (" + at(14084000) + ")");
        ok(at(3578000) == "FT2 JS8", "3.578 (FT2's and JS8's shared dial) → both (" + at(3578000) + ")");
        ok(at(7074300) == "FT8", "7.0743, a little off FT8's dial → still FT8 (" + at(7074300) + ")");
        // ★ V12 wave 2 (2026-10-31): FST4W shares WSPR's dials and its ±150 Hz rule, so both run there.
        ok(at(14095600) == "WSPR FST4W" && at(14095700) == "WSPR FST4W", "14.0956 (and 100 Hz off it) → WSPR and FST4W, their ±150 Hz (" + at(14095600) + ")");
        ok(at(14096000) == "FT8 FT4", "14.0960, 400 Hz off WSPR's dial and on no other → the fallback (" + at(14096000) + ")");
        ok(at(12345000) == "FT8 FT4", "12.345, on no table entry → FT8 + FT4, what Single always ran (" + at(12345000) + ")");
        ok(at(NAN) == "FT8 FT4", "no dial known → FT8 + FT4");
        // ★ retunes: the set follows the dial; a fine retune keeps the engine it has
        double dial = 14074000;
        digi::Spotter sp(digi::Spotter::Scope::Single, ms, nullptr, [&] { return dial; }, nullptr);
        std::vector<float> blk(960, 0.0f);
        const double t0 = vibeUtcNow();
        sp.feedAudio48k(blk.data(), 960, t0);
        ok(sp.describe() == "FT8" && sp.engines() == 1, "Single at 14.074: one engine, FT8 (" + sp.describe() + ")");
        const uint64_t m0 = sp.made();
        dial = 14074250; sp.feedAudio48k(blk.data(), 960, t0 + 0.02);
        ok(sp.describe() == "FT8" && sp.made() == m0, "a 250 Hz retune keeps the same FT8 engine (no restart)");
        dial = 14078000; sp.feedAudio48k(blk.data(), 960, t0 + 0.04);
        ok(sp.describe() == "JS8" && sp.engines() == 1, "retuned to 14.078: FT8 stopped, JS8 started (" + sp.describe() + ")");
        dial = 3578000; sp.feedAudio48k(blk.data(), 960, t0 + 0.06);
        ok(sp.describe() == "JS8, FT2" || sp.describe() == "FT2, JS8", "3.578: JS8 kept, FT2 added (" + sp.describe() + ")");
        dial = 12345000; sp.feedAudio48k(blk.data(), 960, t0 + 0.08);
        ok(sp.engines() == 2 && sp.describe().find("FT8") != std::string::npos && sp.describe().find("FT4") != std::string::npos,
           "off the table: FT8 + FT4 (" + sp.describe() + ")");
    }

    std::printf("── the band table ──\n");
    {
        bool inside = true; std::string bad;
        for (const auto& d : digi::publishedDials()) {
            const digi::Band* b = digi::bandById(d.band);
            const bool narrow = std::string(d.mode) == "WSPR" || std::string(d.mode) == "FST4W";   // ★ 1400–1600 Hz above the dial
            // ★ a registered mode's own window (FST4: 250–1550 Hz, so 2200 m's 136.0 stays inside); else the old assumption
            const digi::ModeDef* md = digi::modeByName(d.mode);
            const double top = md && !narrow ? md->audioHiHz : (narrow ? 1600 : 3000);
            if (!b || d.hz < b->loHz || d.hz + top > b->hiHz + 1) { inside = false; bad += std::string(d.mode) + "@" + d.band + " "; }
        }
        ok(inside, "every published dial (and its passband above it) lies inside its band " + bad);
        const auto& bs = digi::bands();
        ok(bs.size() == 16 && std::string(bs.front().id) == "2200m" && std::string(bs[11].id) == "10m" && std::string(bs[12].id) == "6m"
           && std::string(bs.back().id) == "70cm", "step order 2200 m … 10 m, then 6 m, 4 m, 2 m, 70 cm");
        ok(std::string(digi::bandAt(14074000)->id) == "20m" && digi::bandAt(14500000) == nullptr, "bandAt names the band a frequency is in");
        // ★ The spans with today's registry: FT8 + FT4 + FT2 + JS8 + WSPR (WSPR at 14.0956 is the top of 20 m's cluster).
        const auto& ms = digi::modes();
        const digi::Span s20 = digi::spanOf(*digi::bandById("20m"), ms);
        // ★ V12 2026-10-27: + JTTY (14.090) inside and PSK (14.070) at the bottom — 20 m now opens at the PSK segment.
        ok(s20.ok && near(s20.loHz, 14070000 + 150 - 250, 60) && near(s20.hiHz, 14095600 + 1650 + 250, 60) && s20.modes.size() == 8,
           "20 m with FT8+FT4+FT2+JS8+WSPR+JTTY+PSK+FST4W (on WSPR's dial): " + std::to_string(s20.loHz) + "–" + std::to_string(s20.hiHz) + " (" + std::to_string((int)s20.widthHz()) + " Hz)");
        const digi::Span s2200 = digi::spanOf(*digi::bandById("2200m"), ms);
        // ★ V12 wave 2: FST4W on WSPR's 136.0 and FST4 (136.0 + 250…1800 Hz, inside the band's 137.8 top) join it.
        ok(s2200.ok && s2200.modes.size() == 3 && s2200.modes[0] == "WSPR", "2200 m: WSPR, FST4W and FST4 now they are registered");
        // ★ The spans the brief worked out, with the modes being built now registered (dummies standing in).
        std::vector<digi::ModeDef> all = ms;
        // (a mode already registered is not doubled — WSPR, JS8 and FT2 are real now)
        for (auto d : { dummyMode("WSPR", 1400, 1600), dummyMode("JS8", 100, 3500), dummyMode("FT2", 100, 3500) })
            if (!digi::modeByName(d.name)) all.push_back(d);
        std::printf("    passband per band, FT8+FT4+FT2+JS8+WSPR registered:\n");
        std::map<std::string, double> w;
        for (const auto& b : digi::bands()) {
            const digi::Span s = digi::spanOf(b, all);
            if (!s.ok) continue;
            w[b.id] = s.widthHz();
            std::string md; for (auto& m : s.modes) md += m + " ";
            if (!s.outside.empty()) { md += "(outside: "; for (auto& m : s.outside) md += m + " "; md += ")"; }
            std::printf("      %-6s %10.4f–%10.4f MHz  %5.1f kHz  centre %.4f  %s\n", b.id, s.loHz / 1e6, s.hiHz / 1e6, s.widthHz() / 1e3, s.centreHz / 1e6, md.c_str());
        }
        ok(near(w["20m"], 27600, 400), "20 m spans PSK 14.070 → WSPR 14.0956+ (≈ 28 kHz): " + std::to_string((int)w["20m"]));
        ok(near(w["40m"], 42500, 1000), "40 m spans WSPR 7.0386 → JS8 7.078 (≈ 43 kHz): " + std::to_string((int)w["40m"]));
        ok(near(w["80m"], 14200, 600), "80 m ≈ 14 kHz (WSPR 3.5686 → PSK 3.580): " + std::to_string((int)w["80m"]));
        ok(w["30m"] > 10000 && w["30m"] < 19000, "30 m ≈ 11–18 kHz (JS8 10.130 → FT2 10.144): " + std::to_string((int)w["30m"]));
        {
            const digi::Span s15 = digi::spanOf(*digi::bandById("15m"), all), s2 = digi::spanOf(*digi::bandById("2m"), all);
            ok(std::find(s15.outside.begin(), s15.outside.end(), "FT4") != s15.outside.end() && s15.widthHz() <= digi::kMaxSpanHz,
               "15 m: FT4 (21.140) is 66 kHz from FT8 — outside the cluster, named, never a 74 kHz slice");
            ok(std::find(s2.outside.begin(), s2.outside.end(), "WSPR") != s2.outside.end()
               && std::find(s2.modes.begin(), s2.modes.end(), "FT4") != s2.modes.end(), "2 m: FT8 + FT4 together, WSPR (144.4905) outside");
        }
        ok(w.count("2200m") && w["2200m"] < 2500, "2200 m: WSPR + FST4W + FST4, still a narrow passband (" + std::to_string((int)w["2200m"]) + ")");
        double widest = 0; for (auto& kv : w) widest = std::max(widest, kv.second);
        ok(widest <= 48000, "no band's passband is wider than 48 kHz (" + std::to_string((int)widest) + ")");
    }

    std::printf("── band stepping ──\n");
    {
        const auto& ms = digi::modes();
        auto everywhere = [](double, double) { return true; };
        const auto spans = digi::reachableSpans(ms, everywhere);
        std::string ids; for (auto& s : spans) ids += std::string(s.band->id) + " ";
        ok(ids == "2200m 630m 160m 80m 60m 40m 30m 20m 17m 15m 12m 10m 6m 4m 2m 70cm ", "FT8/FT4/WSPR bands in step order: " + ids);
        auto at = [&](int i) { return i < 0 ? std::string("-") : std::string(spans[(size_t)i].band->id); };
        ok(at(digi::stepSpan(spans, 14074000, +1)) == "17m" && at(digi::stepSpan(spans, 14074000, -1)) == "30m", "20 m: up → 17 m, down → 30 m");
        ok(at(digi::stepSpan(spans, 28074000, +1)) == "6m", "10 m → 6 m (HF, then VHF)");
        ok(at(digi::stepSpan(spans, 432174000, +1)) == "70cm" && at(digi::stepSpan(spans, 136000, -1)) == "2200m", "clamped at both ends, never wrapped");
        ok(at(digi::stepSpan(spans, 1840000, -1)) == "630m", "160 m down → 630 m (WSPR's LF bands)");
        ok(at(digi::stepSpan(spans, 12000000, +1)) == "20m" && at(digi::stepSpan(spans, 12000000, -1)) == "30m", "between bands: the nearest one that way");
        ok(at(digi::stepSpan(spans, 0, +1)) == "2200m" && at(digi::stepSpan(spans, 9e9, -1)) == "70cm", "from nowhere: the first / the last");
        auto hfPlus = [](double lo, double hi) { return lo >= 0 && hi <= 260e6; };
        const auto hp = digi::reachableSpans(ms, hfPlus);
        ok(!hp.empty() && std::string(hp.back().band->id) == "2m", "an HF+ (≤ 260 MHz) reaches up to 2 m, never 70 cm");
        auto noHf = [](double lo, double) { return lo >= 24e6; };
        const auto rtl = digi::reachableSpans(ms, noHf);
        ok(!rtl.empty() && std::string(rtl.front().band->id) == "12m", "an RTL without direct sampling (≥ 24 MHz) starts at 12 m");
        auto none = [](double, double) { return false; };
        ok(digi::reachableSpans(ms, none).empty() && digi::stepSpan({}, 1, 1) == -1, "a radio that reaches no band: no spans (no Band button)");
    }

    std::printf("── the passband per band (DecoderHost::bandWidthFor) ──\n");
    {
        for (const char* id : { "160m", "80m", "40m", "20m", "10m", "6m", "2m" }) {
            const digi::Span s = digi::spanOf(*digi::bandById(id), digi::modes());
            const double w = DecoderHost::bandWidthFor(s.centreHz);
            ok(near(w, std::max(4000.0, s.widthHz()), 1.0), std::string(id) + ": the slice asked for at the band's centre is its span, " + std::to_string((int)w) + " Hz");
        }
        const digi::Span s = digi::spanOf(*digi::bandById("20m"), digi::modes());
        ok(DecoderHost::bandWidthFor(s.centreHz + 2000) > s.widthHz() + 3900, "a dial typed 2 kHz off-centre asks for a slice wide enough to hold the whole span");
        ok(DecoderHost::bandWidthFor(12.5e6) == 24000.0, "off any band: 24 kHz");
    }

    std::printf("── JTTY and PSK in the registry (V12, 2026-10-27) ──\n");
    {
        const digi::ModeDef* j = digi::modeByName("JTTY"); const digi::ModeDef* p = digi::modeByName("PSK");
        ok(j && j->slotSec == 0 && j->sampleRate == 12000 && j->audioLoHz <= 150 && j->audioHiHz >= 3394 && j->single,
           "JTTY registered: free-running (no slots), 12 kHz, its whole 150–3300 Hz search (+ the top tone), Single and Band");
        ok(p && p->slotSec == 0 && p->audioLoHz <= 200 && p->audioHiHz >= 3600, "PSK registered (the family; spots say PSK31/PSK63): free-running, 200–3600 Hz");
        auto dials = [](const digi::ModeDef* m) { std::string o; for (const auto& b : digi::bands()) for (double d : digi::dialsOf(*m, b.id)) { char t[24]; std::snprintf(t, sizeof t, "%.3f ", d / 1e6); o += t; } return o; };
        ok(dials(j) == "7.090 14.090 21.090 28.090 50.160 144.160 ", "JTTY's dials: 7.090 14.090 21.090 (PSKReporter) + 28.090 50.160 144.160 (" + dials(j) + ")");
        ok(dials(p) == "3.580 7.040 10.142 14.070 18.100 21.070 24.920 28.120 ", "PSK's dials: the PSK31 calling frequencies (" + dials(p) + ")");
        // ★ none of JTTY's dials sits inside another mode's window (the announced 3.575 / 10.140 / 18.100 … were left out)
        bool clear = true; std::string clash;
        for (const auto& b : digi::bands())
            for (double d : digi::dialsOf(*j, b.id))
                for (const auto& m : digi::modes()) {
                    if (&m == j || m.name == "PSK") continue;
                    for (double e : digi::dialsOf(m, b.id))
                        if (d + j->audioLoHz < e + m.audioHiHz && e + m.audioLoHz < d + j->audioHiHz) { clear = false; clash += m.name + "@" + b.id + " "; }
                }
        ok(clear, "no JTTY dial shares a window with another mode (FT8/FT4 energy would hold its gate open all day) " + clash);
        std::string pr; for (const auto& m : digi::modes()) pr += m.name + ":" + std::to_string(m.priority) + " ";
        ok(digi::modeByName("FT8")->priority < digi::modeByName("FT4")->priority && digi::modeByName("FT4")->priority < digi::modeByName("WSPR")->priority
           && digi::modeByName("WSPR")->priority < digi::modeByName("JS8")->priority && digi::modeByName("JS8")->priority < digi::modeByName("FT2")->priority
           && digi::modeByName("FT2")->priority < j->priority && j->priority < p->priority && p->priority < 100,
           "the governor's order FT8 > FT4 > WSPR > JS8 > FT2 > JTTY > PSK > anything new (" + pr + ")");
        const auto& ms = digi::modes();
        auto at = [&](double hz) { std::string o; for (auto& n : digi::Spotter::singleModesAt(ms, hz)) o += (o.empty() ? "" : " ") + n; return o; };
        ok(at(14090000) == "JTTY", "Single at 14.090 → JTTY (" + at(14090000) + ")");
        ok(at(14070000) == "PSK" && at(14071000) == "PSK", "Single at 14.070 / 14.071 → PSK (" + at(14070000) + ")");
        ok(at(7090000) == "JTTY" && at(21090000) == "JTTY", "Single at 7.090 and 21.090 → JTTY");
        ok(at(14074000) == "FT8" && at(14078000) == "JS8", "FT8's and JS8's dials still run only their own");
        // ★ 40 m keeps WSPR: JTTY at 7.090 would stretch the cluster past 48 kHz — the lower priority stays outside
        const digi::Span s40 = digi::spanOf(*digi::bandById("40m"), ms);
        ok(std::find(s40.modes.begin(), s40.modes.end(), "WSPR") != s40.modes.end() && std::find(s40.outside.begin(), s40.outside.end(), "JTTY") != s40.outside.end(),
           "40 m: WSPR stays in the span, JTTY (7.090) is outside it — a new mode never pushes an established one out");
        const digi::Span s15 = digi::spanOf(*digi::bandById("15m"), ms), s10 = digi::spanOf(*digi::bandById("10m"), ms);
        ok(std::find(s15.modes.begin(), s15.modes.end(), "JTTY") != s15.modes.end() && std::find(s15.modes.begin(), s15.modes.end(), "PSK") != s15.modes.end(),
           "15 m: JTTY (21.090) and PSK (21.070) inside, " + std::to_string((int)s15.widthHz()) + " Hz");
        ok(std::find(s10.outside.begin(), s10.outside.end(), "PSK") != s10.outside.end() && std::find(s10.modes.begin(), s10.modes.end(), "JTTY") != s10.modes.end(),
           "10 m: JTTY (28.090) inside, PSK (28.120) outside the 48 kHz cluster");
    }

    std::printf("── the governor: idle, depth before mode, priority, the owner ──\n");
    {
        using digi::GovIn; using digi::RunState; using digi::Override;
        auto in = [](int pr, double c, double l, bool w, Override ov = Override::Auto, bool committed = false) { GovIn g; g.priority = pr; g.cost = c; g.light = l; g.wants = w; g.ov = ov; g.committed = committed; return g; };
        auto str = [](const std::vector<RunState>& v) { std::string o; for (auto s : v) o += std::string(digi::stateWord(s)) + " "; return o; };
        // FT8 1/.6, FT4 1/.6, WSPR 5/2, JS8 1.8/.6, FT2 .9/.6, JTTY 12/12, PSK .6/.6 — all wanting
        std::vector<GovIn> band = { in(10,1,.6,true), in(20,1,.6,true), in(30,5,2,true), in(40,1.8,.6,true), in(50,.9,.6,true), in(60,12,12,true), in(70,.6,.6,true) };
        auto all = digi::govern(band, INFINITY, false);
        ok(str(all) == "active active active active active active active ", "no budget: everything at full depth (" + str(all) + ")");
        auto ld = digi::govern(band, INFINITY, true);
        ok(str(ld) == "light light light light light light light ", "the server loaded: everything light, nothing shed (" + str(ld) + ")");
        // light sum = .6+.6+2+.6+.6+12+.6 = 17; full sum 22.3
        auto b20 = digi::govern(band, 20, false);
        // ★ light sum 17 → 3 left: FT8 +.4, FT4 +.4, WSPR +3 does not fit (stays light), JS8 +1.2, FT2 +.3; JTTY and PSK
        //   have no lighter setting (their "light" IS full)
        ok(str(b20) == "active active light active active active active ",
           "20 units: all decode, light first — full depth given back in priority order, greedily, while it fits (" + str(b20) + ")");
        auto b8 = digi::govern(band, 8, false);
        ok(str(b8) == "active active light active active shed active ",
           "8 units: depth goes first, then JTTY (12) is shed — and PSK (0.6), below it, still fits (" + str(b8) + ")");
        auto b3 = digi::govern(band, 3, false);
        ok(str(b3) == "light light shed light light shed active ", "3 units: WSPR's light 2 does not fit after FT8+FT4 — shed; the cheap ones run (" + str(b3) + ")");
        std::vector<GovIn> quiet = band; quiet[2].wants = false; quiet[5].wants = false; quiet[6].wants = false;
        auto q = digi::govern(quiet, 4, false);
        ok(str(q) == "active active idle light active idle idle ", "empty windows are IDLE, not shed, and leave the budget to the rest (" + str(q) + ")");
        std::vector<GovIn> own = band; own[5].ov = Override::Always; own[6].ov = Override::Off; own[2].wants = false;
        auto o = digi::govern(own, 8, false);
        ok(o[5] != RunState::Shed && o[5] != RunState::Idle && o[6] == RunState::Off && o[0] != RunState::Shed && o[1] != RunState::Shed,
           "the owner's ALWAYS runs over budget (JTTY) without starving the modes above it, OFF never runs (PSK) — " + str(o));
        std::vector<GovIn> inv = { in(60,12,12,true,Override::Auto,true), in(10,1,.6,true) };
        auto iv = digi::govern(inv, 5, false);
        ok(iv[1] != RunState::Shed && iv[0] == RunState::Shed, "a low-priority mode already decoding never keeps a higher one out (JTTY told to stop, FT8 runs)");
    }

    std::printf("── the budget, live ──\n");
    {
        digi::BandBudget b; b.setCap(20);
        ok(b.units() == 20, "the benchmark's cap is the budget to start with");
        b.tick(1000, 0, 10); b.tick(1016, 0, 10);
        ok(b.units() == 20, "calm: stays at the cap");
        b.tick(1032, 3, 12);
        ok(near(b.units(), 9.0, 1e-9), "slots skipped: down to 75 % of what was decoding (12 → 9): " + std::to_string(b.units()));
        b.tick(1048, 3, 9); b.tick(1064, 3, 9);
        ok(b.units() > 10.0 && b.units() < 11.0, "calm periods give 10 % back each: " + std::to_string(b.units()));
        for (int i = 0; i < 20; ++i) b.tick(1080 + 16.0 * i, 3, 9);
        ok(b.units() == 20, "…up to the benchmark's cap, never past it");
        digi::BandBudget u;
        u.tick(0, 0, 30); u.tick(16, 0, 30);
        ok(!std::isfinite(u.units()), "no benchmark: unlimited until a skip says otherwise");
        u.tick(32, 1, 30);
        ok(near(u.units(), 22.5, 1e-9), "…then 75 % of what was in use: " + std::to_string(u.units()));
        ok(near(digi::budgetUnitsFromBench(7.0, 17.0), 170.0, 1e-6) && !std::isfinite(digi::budgetUnitsFromBench(NAN, 17)),
           "the benchmark's Band row → units: 70 % ÷ (row % ÷ its light units)");
    }

    std::printf("── the activity gate: an empty window is never decoded, an occupied one is ──\n");
    {
        const double rate = 96000, centre = 14080000, sigma = 0.02;
        const double slot0 = std::floor(vibeUtcNow() / 60.0) * 60.0 + 60.0;   // a minute boundary ahead
        auto csA = std::make_shared<CountShared>(), csB = std::make_shared<CountShared>(), csC = std::make_shared<CountShared>();
        // A: 14.074 (FT8's place), busy · B: 14.083, empty for two slots then one weak station · C: 14.088, never anything
        std::vector<digi::ModeDef> ms = { countingMode("AAA", 14074000, "20m", 15, 10, 1, .6, csA),
                                          countingMode("BBB", 14083000, "20m", 15, 20, 1, .6, csB),
                                          countingMode("CCC", 14088000, "20m", 15, 30, 1, .6, csC) };
        std::vector<cf32> x((size_t)(rate * 45.2), cf32(0, 0));
        const auto t8 = tonesFor("CQ G4ABC IO92", false);
        for (int s = 0; s < 3; ++s) for (int k = 0; k < 4; ++k) addTx(x, rate, 14074000 + 500 + 700 * k - centre, 15.0 * s + 0.5, t8, false, ampFor(-8, sigma, rate));
        addTx(x, rate, 14083000 + 1500 - centre, 30.5, tonesFor("CQ K1ABC FN42", false), false, ampFor(-16, sigma, rate));   // ★ a lone −16 dB FT8
        addNoise(x, sigma, 99);
        digi::Spotter sp(digi::Spotter::Scope::Band, ms, nullptr, nullptr, nullptr);
        feedBand(sp, x, rate, centre, slot0, true, false);
        auto per = [](std::shared_ptr<CountShared> c, double slot0, int s) { std::lock_guard<std::mutex> lk(c->m); auto it = c->perPeriod.find((long long)std::floor(slot0 / 15.0) + s); return it == c->perPeriod.end() ? 0.0 : it->second / 12000.0; };
        std::printf("    seconds fed per slot   A: %.1f %.1f %.1f   B: %.1f %.1f %.1f   C: %.1f %.1f %.1f\n",
                    per(csA, slot0, 0), per(csA, slot0, 1), per(csA, slot0, 2), per(csB, slot0, 0), per(csB, slot0, 1), per(csB, slot0, 2),
                    per(csC, slot0, 0), per(csC, slot0, 1), per(csC, slot0, 2));
        ok(per(csA, slot0, 0) > 14 && per(csA, slot0, 1) > 14.9 && per(csA, slot0, 2) > 14.9, "A (busy): every slot fed whole — the first opens within a second");
        ok(per(csB, slot0, 0) < 0.01 && per(csB, slot0, 1) < 1.05,
           "B (empty for two slots): nothing fed but the 1 s lead-in before the slot that lit — its decodes never happen");
        ok(per(csB, slot0, 2) > 14.9, "B, its third slot with one −16 dB station: the WHOLE slot fed (held, then flushed when it lit up)");
        ok(per(csC, slot0, 0) + per(csC, slot0, 1) + per(csC, slot0, 2) < 0.01, "C (never anything): nothing fed");
        std::string st; for (auto& m : sp.status()) st += m.mode + "=" + digi::stateWord(m.state) + "(idle " + std::to_string(m.idle) + ") ";
        ok(sp.periodsIdle() >= 4 && sp.periodsShed() == 0, "idle periods counted (" + std::to_string(sp.periodsIdle()) + "), none shed: " + st);
        // ★ The same slice WITHOUT bins: no gate — everything fed, as Band always was.
        auto cA = std::make_shared<CountShared>(), cB = std::make_shared<CountShared>(), cC = std::make_shared<CountShared>();
        std::vector<digi::ModeDef> m2 = { countingMode("AAA", 14074000, "20m", 15, 10, 1, .6, cA), countingMode("BBB", 14083000, "20m", 15, 20, 1, .6, cB),
                                          countingMode("CCC", 14088000, "20m", 15, 30, 1, .6, cC) };
        digi::Spotter s2(digi::Spotter::Scope::Band, m2, nullptr, nullptr, nullptr);
        feedBand(s2, x, rate, centre, slot0, false, false);
        ok(cC->total / 12000.0 > 44, "no bins (a test, the benchmark): no gate — every window fed (" + std::to_string(cC->total / 12000.0) + " s)");
    }

    std::printf("── the gate on noise and at the threshold ──\n");
    {
        const double rate = 96000, centre = 14080000, sigma = 0.02;
        // ★ false openings: 12 windows of 3.4 kHz across 80 kHz of pure noise, 4 s periods (JTTY/PSK's), 10 minutes
        std::vector<std::shared_ptr<CountShared>> cs; std::vector<digi::ModeDef> ms;
        for (int i = 0; i < 12; ++i) { cs.push_back(std::make_shared<CountShared>()); char nm[8]; std::snprintf(nm, sizeof nm, "N%02d", i);
            ms.push_back(countingMode(nm, 14042000 + 6400.0 * i, "20m", 0, 10 + i, 1, 1, cs.back())); }
        digi::Spotter sp(digi::Spotter::Scope::Band, ms, nullptr, nullptr, nullptr);
        const double t0 = std::floor(vibeUtcNow() / 60.0) * 60.0 + 60.0;
        const double chunk = 30.0; std::mt19937 g(5); std::normal_distribution<float> nd(0.0f, (float)sigma);
        for (int c = 0; c < 20; ++c) {
            std::vector<cf32> x((size_t)(rate * chunk)); for (auto& v : x) v = cf32(nd(g), nd(g));
            feedBand(sp, x, rate, centre, t0 + c * chunk, true, false);
        }
        uint64_t idle = 0, live = 0; for (auto& m : sp.status()) { idle += m.idle; live += m.live; }
        const double falseRate = (double)live / std::max<uint64_t>(1, idle + live);
        std::printf("    pure noise, 10 min, 12 windows × 4 s periods: %llu idle, %llu opened (%.2f %%)\n", (unsigned long long)idle, (unsigned long long)live, 100 * falseRate);
        ok(falseRate < 0.02, "pure noise opens fewer than 2 % of windows (a false opening only costs the decode Band always paid)");
        // ★ the threshold: one FT8 station alone in an empty window, by SNR (2500 Hz), over one slot — does the gate open?
        std::printf("    one FT8 station alone in its window, one 15 s slot (20 trials each):\n");
        for (double snr : { -14.0, -17.0, -19.0, -21.0, -23.0 }) {
            int opened = 0;
            for (int tr = 0; tr < 20; ++tr) {
                auto c = std::make_shared<CountShared>();
                std::vector<digi::ModeDef> one = { countingMode("FTX", 14078000, "20m", 15, 10, 1, .6, c) };
                digi::Spotter s1(digi::Spotter::Scope::Band, one, nullptr, nullptr, nullptr);
                std::vector<cf32> x((size_t)(rate * 15.2), cf32(0, 0));
                addTx(x, rate, 14078000 + 400 + 130 * tr - centre, 0.5, tonesFor("CQ G4ABC IO92", false), false, ampFor(snr, sigma, rate));
                addNoise(x, sigma, 1000 + tr);
                const double s0 = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
                feedBand(s1, x, rate, centre, s0, true, false);
                std::vector<cf32> tail((size_t)(rate * 0.2)); addNoise(tail, sigma, 7);
                feedBand(s1, tail, rate, centre, s0 + 15.2, true, false);   // ★ into the next slot: the first is judged
                if (c->total > 12000 * 5) ++opened;
            }
            std::printf("      %+5.0f dB: opened %2d / 20\n", snr, opened);
            if (snr == -14.0) ok(opened == 20, "a −14 dB FT8 station alone always opens its window");
            if (snr == -17.0) ok(opened >= 18, "−17 dB: opens ≥ 18 / 20 (" + std::to_string(opened) + ")");
        }
    }

    std::printf("── the owner's word, and the budget, through the spotter ──\n");
    {
        const double rate = 96000, centre = 14080000, sigma = 0.02, slot0 = std::floor(vibeUtcNow() / 60.0) * 60.0 + 60.0;
        std::vector<cf32> x((size_t)(rate * 30.2), cf32(0, 0));   // ★ busy in A only
        for (int s = 0; s < 2; ++s) addTx(x, rate, 14074000 + 900 - centre, 15.0 * s + 0.5, tonesFor("CQ G4ABC IO92", false), false, ampFor(-8, sigma, rate));
        addNoise(x, sigma, 3);
        {
            auto a = std::make_shared<CountShared>(), b = std::make_shared<CountShared>();
            std::vector<digi::ModeDef> ms = { countingMode("AAA", 14074000, "20m", 15, 10, 1, .6, a), countingMode("BBB", 14083000, "20m", 15, 20, 1, .6, b) };
            digi::Spotter sp(digi::Spotter::Scope::Band, ms, nullptr, nullptr, nullptr);
            sp.setOverrides([](const std::string& m) { return m == "AAA" ? digi::Override::Off : m == "BBB" ? digi::Override::Always : digi::Override::Auto; });
            feedBand(sp, x, rate, centre, slot0, true, false);
            std::string st; for (auto& m : sp.status()) st += m.mode + "=" + digi::stateWord(m.state) + " ";
            ok(a->total == 0 && b->total / 12000.0 > 29.5, "the owner's OFF: busy A never fed · ALWAYS: empty B fed whole, no gate (" + st + ")");
        }
        {
            // ★ ungated (no bins: all three "occupied"), a budget of 1.7 units: A light (0.6) + B light (0.6) fit, C (prio 30)
            //   is shed, and the 0.5 left takes A back to full depth (+0.4) — B stays light
            auto a = std::make_shared<CountShared>(), b = std::make_shared<CountShared>(), c = std::make_shared<CountShared>();
            std::vector<digi::ModeDef> ms = { countingMode("AAA", 14074000, "20m", 15, 10, 1, .6, a), countingMode("BBB", 14083000, "20m", 15, 20, 1, .6, b),
                                              countingMode("CCC", 14088000, "20m", 15, 30, 1, .6, c) };
            digi::Spotter sp(digi::Spotter::Scope::Band, ms, nullptr, nullptr, nullptr);
            sp.setBudgetCap(1.7);
            feedBand(sp, x, rate, centre, slot0, false, false);
            std::string st; for (auto& m : sp.status()) st += m.mode + "=" + digi::stateWord(m.state) + "(shed " + std::to_string(m.shed) + ") ";
            ok(c->total == 0 && sp.periodsShed() >= 2, "over budget: the LOWEST priority (C) is shed, and counted (" + st + ")");
            ok(a->full > 0 && a->light == 0 && b->light > 0 && b->full == 0,
               "depth before mode: with 1.7 units A runs FULL (1.0) and B LIGHT (0.6), C shed");
            // ★ a budget of 3: everything, all full
            auto a2 = std::make_shared<CountShared>(), b2 = std::make_shared<CountShared>(), c2 = std::make_shared<CountShared>();
            std::vector<digi::ModeDef> m3 = { countingMode("AAA", 14074000, "20m", 15, 10, 1, .6, a2), countingMode("BBB", 14083000, "20m", 15, 20, 1, .6, b2),
                                              countingMode("CCC", 14088000, "20m", 15, 30, 1, .6, c2) };
            digi::Spotter s3(digi::Spotter::Scope::Band, m3, nullptr, nullptr, nullptr);
            s3.setBudgetCap(3.0);
            feedBand(s3, x, rate, centre, slot0, false, false);
            ok(c2->full > 0 && a2->full > 0 && b2->full > 0 && s3.periodsShed() == 0, "within budget: every mode at full depth, nothing shed");
        }
    }

    std::printf("── JTTY and PSK31 through Band mode (the real engines) ──\n");
    {
        const digi::Span s = digi::spanOf(*digi::bandById("20m"), digi::modes());
        const double rate = 96000, sigma = 0.02, t0 = std::floor(vibeUtcNow() / 60.0) * 60.0 + 60.0;
        std::vector<cf32> x((size_t)(rate * 40), cf32(0, 0));
        addJtty(x, rate, 14091500 - s.centreHz, 3.0, "CQ G4JTY CQ IO91", ampFor(-8, sigma, rate));
        addJtty(x, rate, 14092300 - s.centreHz, 9.0, "DL1ABC", ampFor(-8, sigma, rate));
        addPsk31(x, rate, 14071200 - s.centreHz, 1.0, "CQ CQ CQ DE G4PSK G4PSK G4PSK PSE K\nCQ CQ DE G4PSK G4PSK K\n", ampFor(-3, sigma, rate));
        addNoise(x, sigma, 11);
        std::vector<std::string> got; std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& z, double rf) {
            char b[200]; std::snprintf(b, sizeof b, "%s %s %.0f %s %s", z.mode.c_str(), z.call.c_str(), rf, z.how.c_str(), z.grid.c_str());
            std::lock_guard<std::mutex> lk(gm); got.push_back(b); }, nullptr, nullptr);
        feedBand(sp, x, rate, s.centreHz, t0, true, true);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto has = [&](const char* mode, const char* call, double rf, double tol, const char* how) {
            std::lock_guard<std::mutex> lk(gm);
            for (auto& g : got) { char m[16], c[16], h[8] = ""; double f = 0;
                if (std::sscanf(g.c_str(), "%15s %15s %lf %7s", m, c, &f, h) >= 3 && !std::strcmp(m, mode) && !std::strcmp(c, call) && near(f, rf, tol) && !std::strcmp(h, how)) return true; }
            return false;
        };
        { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) std::printf("      %s\n", g.c_str()); }
        ok(has("JTTY", "G4JTY", 14091500, 3, "CQ"), "JTTY G4JTY spotted calling CQ, mode JTTY, at its RF (tone 0) from 14.090's tuner");
        ok(has("JTTY", "DL1ABC", 14092300, 3, "QSO"), "JTTY DL1ABC (a call atom, not CQ) spotted as QSO");
        { bool g = false; std::lock_guard<std::mutex> lk(gm); for (auto& l : got) if (l.find("G4JTY") != std::string::npos && l.find(" IO91") != std::string::npos) g = true;
          ok(g, "…and G4JTY's spot comes again with its own grid (the GRID4 frame after its CQ) — a JTTY caller lands on the map"); }
        ok(has("PSK31", "G4PSK", 14071200, 4, "CQ"), "PSK31 G4PSK spotted calling CQ, mode PSK31, at its RF from 14.070's tuner");
        std::string st; for (auto& m : sp.status()) st += m.mode + "=" + digi::stateWord(m.state) + " ";
        bool otherIdle = true; for (auto& m : sp.status()) if (m.mode == "WSPR" && m.live > 0) otherIdle = false;
        ok(otherIdle, "WSPR's empty window was never opened (" + st + ")");
        bool junk = false; { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) if (g.find("G4JTY") == std::string::npos && g.find("DL1ABC") == std::string::npos && g.find("G4PSK") == std::string::npos) junk = true; }
        ok(!junk, "nothing else spotted");
    }

    std::printf("── the tuner ──\n");
    {
        // A tone 1000 Hz above a dial 7 kHz below the slice centre, its mirror 1000 Hz BELOW the dial, and one 5 kHz up.
        const double rate = 75000, centre = 14078800, dial = 14074000;
        auto tone = [&](double rfHz) {
            std::vector<cf32> x((size_t)(rate * 4));
            for (size_t i = 0; i < x.size(); ++i) { const double p = 2 * M_PI * (rfHz - centre) * i / rate; x[i] = cf32((float)std::cos(p), (float)std::sin(p)); }
            digi::DigiTuner t(dial, 100, 3500, 12000);
            std::vector<float> out;
            for (size_t o = 0; o < x.size(); o += 1500) t.feed(&x[o], (int)std::min<size_t>(1500, x.size() - o), rate, centre, out);
            return std::make_pair(out, t.gain());
        };
        auto powAt = [](const std::vector<float>& a, double hz) {   // single-bin DFT over the last 2 s
            double re = 0, im = 0; const size_t n0 = a.size() - 24000;
            for (size_t i = n0; i < a.size(); ++i) { const double p = 2 * M_PI * hz * (double)i / 12000.0; re += a[i] * std::cos(p); im += a[i] * std::sin(p); }
            return (re * re + im * im) / (24000.0 * 24000.0);
        };
        auto [in, g1] = tone(dial + 1000);
        const double pIn = powAt(in, 1000);
        // ★ The level control applies its gain to every input alike only once settled — compare raw powers ÷ gain².
        auto [img, g2] = tone(dial - 1000);
        auto [up, g3] = tone(dial + 5000);
        const double pImg = powAt(img, 1000) / ((double)g2 * g2) * ((double)g1 * g1);
        const double pUp = (powAt(up, 1000) + powAt(up, 5000) + powAt(up, 7000)) / ((double)g3 * g3) * ((double)g1 * g1);
        ok(in.size() >= 47900 && in.size() <= 48100, "4 s of a 75 kHz slice → 4 s of 12 kHz audio (" + std::to_string(in.size()) + ")");
        ok(pIn > 1e-4, "a tone 1000 Hz above the dial is heard at 1000 Hz");
        ok(10 * std::log10(pIn / std::max(pImg, 1e-30)) > 50, "the lower sideband (1000 Hz BELOW the dial) is rejected > 50 dB: "
           + std::to_string((int)(10 * std::log10(pIn / std::max(pImg, 1e-30)))) + " dB");
        ok(10 * std::log10(pIn / std::max(pUp, 1e-30)) > 50, "a tone 5 kHz above the dial (outside 100–3500) is rejected > 50 dB");
        ok(digi::DigiTuner::fits(dial, 100, 3500, rate, centre) && !digi::DigiTuner::fits(dial - 40000, 100, 3500, rate, centre),
           "fits(): a dial inside the slice fits, one 40 kHz away does not");
        digi::DigiTuner t(dial, 100, 3500); std::vector<float> o; std::vector<cf32> blk(1000, cf32(0, 0));
        t.feed(blk.data(), 1000, rate, centre, o);
        std::printf("    taps: stage 1 %d at 75 kHz, stage 2 %d at 12 kHz\n", t.stage1Taps(), t.stage2Taps());
    }

    std::printf("── a whole band, end to end (DecoderHost, Band) ──\n");
    const digi::Span s20 = digi::spanOf(*digi::bandById("20m"), digi::modes());
    const double rate = 2400000.0 * 512 / 16384;   // ★ 75 kHz — what a 2.4 MS/s radio's slice is for 20 m's width
    {
        DecoderHost::Env env;
        double dialNow = s20.centreHz;
        env.dialHz = [&] { return dialNow; };
        DecoderHost h(env, "band-test");
        auto peer = std::make_shared<Peer>();
        h.addPeer(peer);
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Ok && h.spotsScope() == "band", "Band spots start");
        ok(h.wantsIq() && !h.wantsAudio(), "Band mode wants the IQ slice, not the listener's audio");
        ok(near(h.iqWidthHz(), s20.widthHz(), 1), "…and asks for 20 m's span: " + std::to_string((int)h.iqWidthHz()) + " Hz");
        ok(DecoderHost::skimmersRunning().load() == 1, "the shim's one atomic gate counts it (no slice cut on a radio with none)");

        // ★ Two slots of FT8 (15 s) carry four FT8 stations and, in the same 30 s, four FT4 slots carry FT4 stations.
        const double secs = 31.0, slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;   // ★ a whole-minute-half boundary ahead
        std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
        struct Tx { const char* msg; bool ft4; double rf; double start; };
        const double ft8Dial = 14074000, ft4Dial = 14080000;
        const Tx txs[] = {
            { "CQ G4ABC IO92", false, ft8Dial + 600,  0.5 },
            { "CQ DL1XYZ JO31", false, ft8Dial + 1450, 0.5 },
            { "CQ K1ABC FN42", false, ft8Dial + 2300, 15.5 },
            { "CQ VK2DEF QF56", false, ft8Dial + 3000, 15.5 },
            { "CQ JA1QRS PM95", true, ft4Dial + 800,  0.5 },
            { "CQ EA4TUV IN80", true, ft4Dial + 1700, 8.0 },
            { "CQ OH2WXY KP20", true, ft4Dial + 2500, 15.5 },
            // ★ The LOWER SIDEBAND of the FT8 dial: 1200 Hz below it. A USB receiver must never decode this at +1200.
            { "CQ ZL3LSB RE66", false, ft8Dial - 1200, 0.5 },
        };
        for (const Tx& t : txs) addTx(x, rate, t.rf - s20.centreHz, t.start, tonesFor(t.msg, t.ft4), t.ft4, 0.02);
        // ★ JS8 on 14.078: a Normal heartbeat (15 s slot, starts +0.5 s) and a Turbo one (6 s slot 1, starts +0.1 s);
        //   FT2 on 14.084: two stations in two 3.75 s slots (start +0.1 s).
        const double js8Dial = 14078000, ft2Dial = 14084000;
        struct Fx { const char* call; const char* mode; double rf; std::vector<uint8_t> tones; double start, sym; };
        const std::vector<Fx> fx = {
            { "G4JSN", "JS8", js8Dial + 1000, js8Heartbeat("G4JSN", "IO91", Js8Submode::Normal), 0.5, 0.16 },
            { "W1JST", "JS8", js8Dial + 2200, js8Heartbeat("W1JST", "FN31", Js8Submode::Turbo), 6.1, 0.05 },
            { "IU8AAA", "FT2", ft2Dial + 900, tonesFor("CQ IU8AAA JN70", true), 3.85, 0.024 },
            { "SP6BBB", "FT2", ft2Dial + 2100, tonesFor("CQ SP6BBB JO70", true), 7.6, 0.024 },
        };
        for (const Fx& f : fx) addFsk(x, rate, f.rf - s20.centreHz, f.start, f.tones, f.sym, 0.02);
        addNoise(x, 0.02, 7);
        const int blk = 4096;
        const double c0 = cpuSecs();
        // ★ The host's own feedIq stamps vibeUtcNow(); to run faster than real time the Spotter is driven directly with
        //   the capture times, exactly as the host's decode thread does (spotter_->feedIq(…, b.utc)).
        std::vector<std::string> got;
        std::map<std::string, double> slotOf;   // ★ each call's slot start, seconds after slot0
        std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(),
            [&](const digi::Spot& s, double rf) {
                char b[160]; std::snprintf(b, sizeof b, "%s %s %.0f %s", s.mode.c_str(), s.call.c_str(), rf, s.grid.c_str());
                std::lock_guard<std::mutex> lk(gm); got.push_back(b); slotOf[s.call] = s.slotUtc - slot0;
            }, [&] { return dialNow; }, nullptr);
        for (size_t o = 0; o < x.size(); o += blk) {
            const int n = (int)std::min<size_t>(blk, x.size() - o);
            sp.feedIq(&x[o], n, rate, s20.centreHz, slot0 + (double)(o + n) / rate);
            if ((o / blk) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // ★ let the slot workers run
        }
        const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // ★ FT8 2 + FT4 4 + FT2 8 + JS8 (Turbo 5, Fast 3, Normal 2, Slow 1) slots in the 31 s
        while (sp.slotsDecoded() < 25 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 30)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        (void)c0;
        std::printf("    engines %zu (%s), slots decoded %llu, skipped %llu\n", sp.engines(), sp.describe().c_str(),
                    (unsigned long long)sp.slotsDecoded(), (unsigned long long)sp.slotsSkipped());
        auto has = [&](const std::string& mode, const std::string& call, double rf, double tol = 8) {
            std::lock_guard<std::mutex> lk(gm);
            for (auto& g : got) {
                char m[16], c[16]; double f = 0;
                if (std::sscanf(g.c_str(), "%15s %15s %lf", m, c, &f) == 3 && mode == m && call == c && near(f, rf, tol)) return true;
            }
            return false;
        };
        // ★ RF to within a quarter of the mode's tone step: JS8 Turbo 20 Hz tones, FT2 41.7 Hz
        for (const Fx& f : fx)
            ok(has(f.mode, f.call, f.rf, 0.26 / f.sym), std::string(f.mode) + " " + f.call + " spotted with its mode, at its RF (its own dial in the slice)");
        for (const Tx& t : txs) {
            if (std::string(t.msg).find("LSB") != std::string::npos) continue;
            char call[16]; std::sscanf(t.msg, "CQ %15s", call);
            ok(has(t.ft4 ? "FT4" : "FT8", call, t.rf), std::string(t.ft4 ? "FT4 " : "FT8 ") + call + " spotted with its mode, at its RF");
        }
        {
            // ★ THE SPOT'S TIME IS ITS SLOT'S START (M7EIW): FT8 0 and 15 s, FT4 0 / 7.5 / 15, JS8 Normal 0 and Turbo 6,
            //   FT2 3.75 and 7.5 — what WSJT-X's time column shows, and what the host stamps the spot with.
            std::lock_guard<std::mutex> lk(gm);
            const std::pair<const char*, double> want[] = { {"G4ABC", 0}, {"DL1XYZ", 0}, {"K1ABC", 15}, {"VK2DEF", 15}, {"JA1QRS", 0},
                {"EA4TUV", 7.5}, {"OH2WXY", 15}, {"G4JSN", 0}, {"W1JST", 6}, {"IU8AAA", 3.75}, {"SP6BBB", 7.5} };
            std::string bad;
            for (auto& w : want) { auto it = slotOf.find(w.first); if (it == slotOf.end() || std::fabs(it->second - w.second) > 1e-6) bad += std::string(" ") + w.first + "@" + (it == slotOf.end() ? std::string("?") : std::to_string(it->second)); }
            ok(bad.empty(), "every spot carries its slot's START (FT8 :00/:15, FT4 :07.5, JS8 Turbo :06, FT2 :03.75 …)" + bad);
        }
        bool lsb = false;
        { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) if (g.find("ZL3LSB") != std::string::npos) lsb = true; }
        ok(!lsb, "a station 1200 Hz BELOW the FT8 dial (the lower sideband) is not decoded");
        { std::lock_guard<std::mutex> lk(gm); std::printf("    spots: %zu\n", got.size()); for (auto& g : got) std::printf("      %s\n", g.c_str()); }

        // ★ The host's spot frame (emitSpot: mode, call, RF) is checked through its Single path below.
        h.stopSpots();
        ok(!h.wantsIq() && DecoderHost::skimmersRunning().load() == 0, "stopping Band spots releases the IQ slice and the gate");
    }

    /* ★★ WSPR THROUGH BAND MODE (2026-10-27): one two-minute slot of 20 m in a 75 kHz slice — a WSPR station on the WSPR
     *  dial's sub-band beside FT8 traffic — through the Spotter and the real WSPR engine (digimodes/wspr.h): spotted as
     *  "WSPR" at its RF, with an SNR on the WSPRnet scale (white noise: the definition) within 2 dB. Generated a block at a
     *  time (two minutes at 75 kHz would be 74 MB). */
    std::printf("── WSPR in Band mode (one two-minute slot of 20 m) ──\n");
    {
        const double secs = 124.0, slot = std::ceil(vibeUtcNow() / 120.0) * 120.0 + 120.0, t0 = slot - 3.0;
        const double wDial = 14095600, wRf = wDial + 1523.0, ft8Rf = 14074000 + 1200;
        const double sigma = 0.02, snr = -20.0;
        // complex noise σ per rail → N0 = 2σ²/rate; a complex tone of amplitude A has power A²
        const double A = std::sqrt(std::pow(10.0, snr / 10.0) * 2 * sigma * sigma / rate * 2500.0);
        uint8_t tones[wspr::kSymbols];
        wspr::Message m; m.type = 1; m.call = "G4ABC"; m.grid = "IO92"; m.dbm = 37;
        wspr::encode(m, tones);
        const std::vector<uint8_t> ft8 = tonesFor("CQ M0FTE IO91", false);
        ok(ft8.size() == FT8_NN, "the FT8 neighbour encodes");
        std::mt19937 g(4242); std::normal_distribution<float> nd(0.0f, (float)sigma);
        std::vector<std::string> got; std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(),
            [&](const digi::Spot& s, double rf) {
                char b[200]; std::snprintf(b, sizeof b, "%s %s %.1f %s %d", s.mode.c_str(), s.call.c_str(), rf, s.grid.c_str(), s.snr);
                std::lock_guard<std::mutex> lk(gm); got.push_back(b);
            }, nullptr, nullptr);
        const double wStart = (slot + wspr::kStartSec - t0) * rate, sps = rate * 8192.0 / 12000.0;
        double wph = 0, fph = 0;
        const int blk = 4096;
        std::vector<cf32> x((size_t)blk);
        for (size_t o = 0; o < (size_t)(secs * rate); o += blk) {
            for (int i = 0; i < blk; ++i) {
                const double n = (double)(o + (size_t)i);
                cf32 v(nd(g), nd(g));
                const double k = (n - wStart) / sps;
                if (k >= 0 && k < wspr::kSymbols) {
                    const double f = wRf - 1.5 * wspr::kBaud + tones[(int)k] * wspr::kBaud - s20.centreHz;
                    wph += 2 * M_PI * f / rate;
                    v += cf32((float)(A * std::cos(wph)), (float)(A * std::sin(wph)));
                }
                const double ft = std::fmod(t0 + n / rate - 0.5, 15.0);   // FT8: every 15 s slot, from +0.5 s
                if (ft < 79 * FT8_SYMBOL_PERIOD) {
                    const int s = (int)(ft / FT8_SYMBOL_PERIOD);
                    fph += 2 * M_PI * (ft8Rf + ft8[(size_t)s] / FT8_SYMBOL_PERIOD - s20.centreHz) / rate;
                    v += cf32((float)(0.02 * std::cos(fph)), (float)(0.02 * std::sin(fph)));
                }
                x[(size_t)i] = v;
            }
            sp.feedIq(x.data(), blk, rate, s20.centreHz, t0 + (double)(o + blk) / rate);
            if ((o / blk) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double w0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto wsprSpot = [&] { std::lock_guard<std::mutex> lk(gm); for (auto& s : got) if (s.rfind("WSPR ", 0) == 0) return s; return std::string(); };
        while (wsprSpot().empty() && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - w0 < 30)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string ws = wsprSpot();
        char md[16] = {}, call[16] = {}, grid[16] = {}; double rf = 0; int sn = -99;
        std::sscanf(ws.c_str(), "%15s %15s %lf %15s %d", md, call, &rf, grid, &sn);
        std::printf("    engines %zu (%s); slots %llu; %s\n", sp.engines(), sp.describe().c_str(), (unsigned long long)sp.slotsDecoded(), ws.empty() ? "no WSPR spot" : ws.c_str());
        ok(std::string(call) == "G4ABC" && std::string(grid) == "IO92" && near(rf, wRf, 1.0),
           "WSPR G4ABC IO92 spotted as \"WSPR\" at its RF (dial 14.0956 + 1523 Hz): " + ws);
        ok(std::abs(sn - snr) <= 2, "…its SNR (WSPRnet scale) within 2 dB of the " + std::to_string((int)snr) + " dB it was sent at: " + std::to_string(sn));
        bool ft8ok = false; { std::lock_guard<std::mutex> lk(gm); for (auto& s : got) if (s.rfind("FT8 M0FTE", 0) == 0) ft8ok = true; }
        ok(ft8ok, "…while FT8 on its own dial in the same slice still decodes");
        ok(sp.slotsSkipped() == 0, "no slot skipped");
    }

    /* ★ WSPR IN SINGLE MODE: fed only while the listener sits on a WSPR dial — on the FT8 dial its 1400–1600 Hz is FT8
     *  traffic, and the engine is not even fed. The same 48 kHz audio both ways. */
    std::printf("── WSPR in Single mode: only at a WSPR dial ──\n");
    {
        const double slot = std::ceil(vibeUtcNow() / 120.0) * 120.0 + 120.0;
        std::vector<float> a((size_t)(48000 * 124), 0.f);
        wsprsynth::addNoise(a, 77, 0.05);
        wsprsynth::Sig s; s.msg = wsprsynth::msg1("DL1XYZ", "JO62", 30); s.snr = -22; s.centreHz = 1477;
        // wspr_synth's buffer starts kLeadSec before the minute; ours 3 s before: place it 1 s in
        std::vector<float> sig((size_t)(48000 * 122), 0.f);
        wsprsynth::addSignal(sig, 48000, s, 0.05, 5);
        for (size_t i = 0; i < sig.size(); ++i) a[i + 48000] += sig[i];
        auto run = [&](double dial) {
            std::vector<std::string> got; std::mutex gm;
            digi::Spotter sp(digi::Spotter::Scope::Single, digi::modes(),
                [&](const digi::Spot& o, double rf) { char b[120]; std::snprintf(b, sizeof b, "%s %s %.0f", o.mode.c_str(), o.call.c_str(), rf);
                    std::lock_guard<std::mutex> lk(gm); got.push_back(b); }, [dial] { return dial; }, nullptr);
            for (size_t o = 0; o < a.size(); o += 4800) {
                sp.feedAudio48k(&a[o], (int)std::min<size_t>(4800, a.size() - o), slot - 3.0 + (double)(o + 4800) / 48000);
                if ((o / 4800) % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const double w0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            for (;;) {
                { std::lock_guard<std::mutex> lk(gm); for (auto& g : got) if (g.rfind("WSPR", 0) == 0) return g; }
                if (std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - w0 > (dial == 14095600 ? 30 : 4)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return std::string();
        };
        const std::string at = run(14095600), off = run(14074000);
        ok(at.rfind("WSPR DL1XYZ 14097077", 0) == 0, "on the 20 m WSPR dial: WSPR DL1XYZ at dial + 1477 Hz: " + at);
        ok(off.empty(), "on the FT8 dial: no WSPR decode (the engine is not fed there)");
    }

    std::printf("── pure noise: no spot ──\n");
    {
        std::vector<cf32> x((size_t)(31.0 * rate), cf32(0, 0));
        addNoise(x, 0.02, 99);
        int n = 0;
        std::mutex gm;
        digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot&, double) { std::lock_guard<std::mutex> lk(gm); ++n; },
                         nullptr, nullptr);
        const double slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;
        for (size_t o = 0; o < x.size(); o += 4096) {
            const int k = (int)std::min<size_t>(4096, x.size() - o);
            sp.feedIq(&x[o], k, rate, s20.centreHz, slot0 + (double)(o + k) / rate);
            if ((o / 4096) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        while (sp.slotsDecoded() < 6 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok(n == 0 && sp.slotsDecoded() >= 6, "31 s of noise on 20 m (FT8, FT4, FT2, JS8 engines): " + std::to_string(sp.slotsDecoded()) + " slots decoded, " + std::to_string(n) + " spots");
    }

    std::printf("── the host: Single still FT8+FT4 on the listener's audio, and the spot frame names its mode ──\n");
    {
        DecoderHost::Env env;
        env.dialHz = [] { return 14074000.0; };
        DecoderHost h(env, "single-test");
        auto peer = std::make_shared<Peer>();
        h.addPeer(peer);
        ok(h.startSpots() == DecoderHost::Start::Ok && h.spotsScope() == "single" && h.wantsAudio() && !h.wantsIq(), "Single spots: the listener's audio, no slice");
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Ok && h.spotsScope() == "band" && h.slotsHeld() == 1,
           "switching to Band replaces the run and keeps the ONE slot");
        ok(h.startSpots(DecoderHost::SpotScope::Band) == DecoderHost::Start::Joined, "asking for Band again joins it");
        h.stopSpots();
        ok(h.slotsHeld() == 0, "stopped: the slot is free");
        // ★ A FT8 band at 48 kHz on the listener's audio, captured in real time terms (stamped feed).
        h.startSpots();
        const double slot0 = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
        std::vector<float> a((size_t)(48000 * 15.5), 0.0f);
        std::mt19937 g(3); std::normal_distribution<float> nz(0.0f, 0.02f);
        auto put = [&](const char* msg, double f0) {
            auto t = tonesFor(msg, false); double ph = 0;
            for (size_t k = 0; k < t.size(); ++k) for (int j = 0; j < 7680; ++j) {
                const size_t i = 24000 + k * 7680 + (size_t)j; if (i >= a.size()) return;
                ph += 2 * M_PI * (f0 + t[k] * 6.25) / 48000.0; a[i] += 0.1f * (float)std::sin(ph);
            }
        };
        put("CQ M0SGL IO91", 1200);
        for (auto& v : a) v += nz(g);
        for (size_t o = 0; o < a.size(); o += 960) {
            const int k = (int)std::min<size_t>(960, a.size() - o);
            h.feed(&a[o], k, 1, slot0 + (double)(o + k) / 48000.0);
            while (h.queued() > 96000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::string spot;
        for (int i = 0; i < 200 && spot.empty(); ++i) { auto v = peer->spots(); if (!v.empty()) spot = v[0]; else std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        ok(field(spot, "mode") == "FT8" && field(spot, "callsign") == "M0SGL" && field(spot, "band") == "20m"
           && near(std::atof(field(spot, "frequency").c_str()), 14075200, 8), "Single: the spot frame names FT8, the call, 20m, dial + audio: " + spot);
        h.stopSpots();
    }

    std::printf("── Single mode at a JS8 dial and at an FT2 dial: the listener's audio, the spot frame names the mode ──\n");
    for (int which = 0; which < 2; ++which) {
        const bool isJs8 = which == 0;
        const double dial = isJs8 ? 7078000.0 : 14084000.0;
        DecoderHost::Env env;
        env.dialHz = [dial] { return dial; };
        DecoderHost h(env, isJs8 ? "single-js8" : "single-ft2");
        auto peer = std::make_shared<Peer>();
        h.addPeer(peer);
        h.startSpots();
        const double slot0 = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
        std::vector<float> a((size_t)(48000 * 15.5), 0.0f);
        std::mt19937 g(5); std::normal_distribution<float> nz(0.0f, 0.02f);
        const std::vector<uint8_t> t = isJs8 ? js8Heartbeat("M0JSS", "IO92", Js8Submode::Normal) : tonesFor("CQ IU8SGL JN70", true);
        const double sym = isJs8 ? 0.16 : 0.024, f0 = 1300, start = isJs8 ? 0.5 : 0.1;
        const int ns = (int)std::lround(sym * 48000);
        double ph = 0;
        for (size_t k = 0; k < t.size(); ++k) for (int j = 0; j < ns; ++j) {
            const size_t i = (size_t)(start * 48000) + k * (size_t)ns + (size_t)j; if (i >= a.size()) break;
            ph += 2 * M_PI * (f0 + t[k] / sym) / 48000.0; a[i] += 0.1f * (float)std::sin(ph);
        }
        for (auto& v : a) v += nz(g);
        for (size_t o = 0; o < a.size(); o += 960) {
            const int k = (int)std::min<size_t>(960, a.size() - o);
            h.feed(&a[o], k, 1, slot0 + (double)(o + k) / 48000.0);
            while (h.queued() > 96000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::string spot;
        for (int i = 0; i < 300 && spot.empty(); ++i) {
            for (auto& v : peer->spots()) if (field(v, "mode") == (isJs8 ? "JS8" : "FT2")) { spot = v; break; }
            if (spot.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ok(field(spot, "callsign") == (isJs8 ? "M0JSS" : "IU8SGL") && field(spot, "band") == (isJs8 ? "40m" : "20m")
           && field(spot, "grid") == (isJs8 ? "IO92" : "JN70") && near(std::atof(field(spot, "frequency").c_str()), dial + f0, isJs8 ? 4 : 12),
           std::string("Single at ") + (isJs8 ? "JS8 7.078" : "FT2 14.084") + ": " + spot);
        h.stopSpots();
    }

    if (cpu) {
        std::printf("── CPU: Band vs Single on the same busy 20 m (this machine; process CPU, every thread) ──\n");
        // ★ The same traffic both ways: 20 FT8 stations in two slots, and 10 FT4 stations. Single hears the FT8 dial's
        //   passband (the FT4 stations are 6 kHz up, out of it); Band hears both dials. Each figure = CPU ÷ 31 s of audio.
        const double secs = 31.0, slot0 = std::floor(vibeUtcNow() / 30.0) * 30.0 + 30.0;
        std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
        char msg[32];
        for (int k = 0; k < 20; ++k) { std::snprintf(msg, sizeof msg, "CQ G4A%c%c IO9%d", 'A' + k / 26, 'A' + k % 26, k % 10);
            addTx(x, rate, 14074000 + 300 + 140 * k - s20.centreHz, k % 2 ? 15.5 : 0.5, tonesFor(msg, false), false, 0.01); }
        for (int k = 0; k < 10; ++k) { std::snprintf(msg, sizeof msg, "CQ DL4B%c JO3%d", 'A' + k, k % 10);
            addTx(x, rate, 14080000 + 300 + 280 * k - s20.centreHz, 0.5 + 7.5 * (k % 4), tonesFor(msg, true), true, 0.01); }
        addNoise(x, 0.02, 11);
        auto run = [&](digi::Spotter::Scope scope, int& spots) {
            std::mutex gm; spots = 0;
            digi::Spotter sp(scope, digi::modes(), [&](const digi::Spot&, double) { std::lock_guard<std::mutex> lk(gm); ++spots; },
                             [] { return 14074000.0; }, nullptr);
            // ★ Single's input: the FT8 dial's USB audio at 48 kHz — made by a tuner (not counted) and repeated ×4.
            std::vector<float> a48;
            if (scope == digi::Spotter::Scope::Single) {
                digi::DigiTuner t(14074000, 100, 3500); std::vector<float> a12;
                for (size_t o = 0; o < x.size(); o += 4096) t.feed(&x[o], (int)std::min<size_t>(4096, x.size() - o), rate, s20.centreHz, a12);
                a48.resize(a12.size() * 4); for (size_t i = 0; i < a48.size(); ++i) a48[i] = a12[i / 4];
            }
            const double c0 = cpuSecs();
            if (scope == digi::Spotter::Scope::Single) {
                for (size_t o = 0; o < a48.size(); o += 3840) {
                    const int k = (int)std::min<size_t>(3840, a48.size() - o);
                    sp.feedAudio48k(&a48[o], k, slot0 + (double)(o + k) / 48000.0);
                    if ((o / 3840) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else {
                for (size_t o = 0; o < x.size(); o += 4096) {
                    const int k = (int)std::min<size_t>(4096, x.size() - o);
                    sp.feedIq(&x[o], k, rate, s20.centreHz, slot0 + (double)(o + k) / rate);
                    if ((o / 4096) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            while (sp.slotsDecoded() < 6 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0 < 30)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return 100.0 * (cpuSecs() - c0) / secs;
        };
        int nS = 0, nB = 0;
        const double single = run(digi::Spotter::Scope::Single, nS);
        const double band = run(digi::Spotter::Scope::Band, nB);
        std::vector<float> out;
        digi::DigiTuner t8(14074000, 100, 3500), t4(14080000, 100, 3500);
        const double c1 = cpuSecs();
        for (size_t o = 0; o < x.size(); o += 4096) {
            const int k = (int)std::min<size_t>(4096, x.size() - o);
            out.clear(); t8.feed(&x[o], k, rate, s20.centreHz, out); t4.feed(&x[o], k, rate, s20.centreHz, out);
        }
        const double tuners = 100.0 * (cpuSecs() - c1) / secs;
        std::printf("    Single (FT8+FT4 engines on the 14.074 passband): %.3f %% of one core, %d spots\n", single, nS);
        std::printf("    Band 20 m (FT8 on 14.074, FT4 on 14.080, from a 75 kHz slice): %.3f %% of one core, %d spots\n", band, nB);
        std::printf("    of which the two tuners: %.3f %% (%.3f %% per dial)\n", tuners, tuners / 2);
    }

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
