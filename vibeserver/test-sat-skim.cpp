// test-sat-skim.cpp — ★★★ beta97 THE LIVE SATELLITE DIGITAL-MODE SKIMMER (decoders/sat_skim.h) behind the transponder
// finder's ONE Doppler mixer (vibe_xpdr_run.h Opts::turned) — everything below the host:
//   1. the windows: they cover the span (+ the edges), overlap by 300 Hz, are centred, and are bounded
//   2. ★★★ Stuart's JO-97 FT4 clip (vibeserver/testdata-sat/jo97-ft4-2026-11-27T205458Z-12k.wav) made into the slice's IQ
//      with a SIMULATED PASS DOPPLER on it (the finder's mixer turns it out again), fed on today's slot grid: BOTH CQs —
//      "CQ DL6UMR JO71" (pass 1) and "CQ PF7X JO22" (pass 2, alignment-sensitive) — decode through the skimmer, as FT4, at
//      the dial + audio offset in the satellite frame, each with the tag it was cut under; nothing doubled
//   3. a SYNTHETIC PASSBAND: several FT8 and FT4 stations at different offsets — one in the overlap of two windows, one at
//      the very edge — with a pass's Doppler curve at 435 / 145 MHz, for an inverting bird and a non-inverting one (both
//      come down USB) and an LSB downlink (the mirror path): every station decoded exactly once, at its own frequency
//   4. THE CATCH-UP QUEUE: far more audio than one worker can decode, a small cap — the queue never holds more than its
//      cap, the OLDEST slots go (logged, in order), the newest are decoded, and everything enqueued is accounted for
//   5. the benchmark's default (live / catch-up) and the RAM-scaled cap
// Silent (no audio device). Exit 3 = the clip is missing.
#include "decoders/sat_skim.h"
#include "vibe_xpdr_run.h"
#include "vibe_clock.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
using cd = std::complex<double>;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }

// ── a plain radix-2 FFT (double) for the clip's analytic upsampling ────────────────────────────────────────────────────
static void fft(std::vector<cd>& a, bool inv) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / (double)len * (inv ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
static bool readWav16(const char* path, std::vector<float>& out, int& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    size_t p = 12; int ch = 0, bits = 0; rate = 0;
    while (p + 8 <= b.size()) {
        const uint32_t sz = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = b[p + 10] | b[p + 11] << 8; rate = (int)(b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | (uint32_t)b[p + 15] << 24); bits = b[p + 22] | b[p + 23] << 8; }
        else if (!std::memcmp(&b[p], "data", 4)) {
            if (ch != 1 || bits != 16) return false;
            const size_t end = std::min(b.size(), p + 8 + (size_t)sz);
            for (size_t i = p + 8; i + 1 < end; i += 2) out.push_back((float)(int16_t)(b[i] | b[i + 1] << 8) / 32768.0f);
            return rate > 0;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

/** The simulated pass: Doppler (Hz at `fc`) at time t (s from the start) — an S-curve through zero at `tca`, `maxHz` at
 *  the horizons, steepest (≈ maxHz·k/2 Hz/s) at TCA — what a LEO pass looks like. */
struct PassDoppler {
    double maxHz, tca, k;
    double at(double t) const { return -maxHz * std::tanh(k * (t - tca)); }
};
/** Rotate `x` (fs, starting at t0 s) by the pass's Doppler — every signal moved by the same Hz (the differential across a
 *  passband is ≤ 3 Hz, see the header: within the decoders' tolerance, and the finder turns out the centre's figure). */
static void applyDoppler(std::vector<cf32>& x, double fs, const PassDoppler& d, double t0) {
    double ph = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        ph += 2 * M_PI * d.at(t0 + (double)i / fs) / fs;
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
    }
}

struct Got { std::string mode, call, text; double rf; satskim::Tag tag; double slot; int snr = 0; float dt = 0; };
struct Sink {
    std::mutex m;
    std::vector<Got> got;
    void add(const digi::Spot& s, double rf, const satskim::Tag& t) { std::lock_guard<std::mutex> lk(m); got.push_back({ s.mode, s.call, s.text, rf, t, s.slotUtc, s.snr, s.dt }); }
    int count(const std::string& text) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& g : got) if (g.text == text) ++n; return n; }
    const Got* find(const std::string& text) { std::lock_guard<std::mutex> lk(m); for (auto& g : got) if (g.text == text) return &g; return nullptr; }
};

/** The finder's Run with the skimmer on its `turned` stream, the pass's Doppler as the model; the slice is fed in 20 ms blocks
 *  stamped from `t0`. Returns once the skimmer's queue is worked off. */
static void runThroughFinder(const std::vector<cf32>& iq, double fs, double centre, double spanLo, double spanHi, bool lsb,
                             const PassDoppler& dop, double t0, satskim::Skimmer& sk) {
    vibexpdr::Run::Opts o;
    o.widthHz = fs * 0.9; o.centreHz = centre; o.spanLoHz = spanLo; o.spanHiHz = spanHi; o.usb = !lsb;
    double tNow = 0;
    o.dopplerHz = [&](double hz) { return dop.at(tNow) * hz / centre; };
    o.turned = [&](const cf32* x, int n, double rate, double utc, bool locked) { if (locked) sk.feed(x, n, rate, utc); };
    vibexpdr::Run run(std::move(o));
    const int blk = (int)(fs / 50);
    for (size_t off = 0; off < iq.size(); off += (size_t)blk) {
        const int n = (int)std::min<size_t>((size_t)blk, iq.size() - off);
        tNow = ((double)off + 0.5 * n) / fs;   // ★ the model's figure at the block's middle
        run.feedIq(iq.data() + off, n, fs, centre, t0 + (double)(off + n) / fs);
    }
    sk.waitIdle(120);
}

// ── FT8 / FT4 synthesis (continuous-phase FSK, as test-bg-band) ─────────────────────────────────────────────────────────
static std::vector<uint8_t> tonesOf(const char* msg, bool ft4) {
    ftx_message_t m; ftx_message_init(&m);
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) return {};
    std::vector<uint8_t> t(ft4 ? FT4_NN : FT8_NN);
    if (ft4) ft4_encode(m.payload, t.data()); else ft8_encode(m.payload, t.data());
    return t;
}
/** One transmission at `offHz` from the slice centre (its tone 0), starting `startSec` into `x`; `lsb` = sent so that it
 *  comes down LSB (tones descending). Amplitude from `snrDb` in 2500 Hz over complex noise of `sigma` per rail. */
static void addFtx(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::vector<uint8_t>& tones, bool ft4,
                   double snrDb, double sigma, bool lsb) {
    const double sym = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD, spacing = 1.0 / sym;
    const double amp = std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / fs * 2500.0);
    const size_t s0 = (size_t)(startSec * fs), ns = (size_t)std::llround(sym * fs);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + (lsb ? -1.0 : 1.0) * tones[k] * spacing) / fs;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}

struct Station { const char* msg; bool ft4; double satHz; double snr; int slot; };

/** One synthetic pass: `sts` on a slice of `fs` around `centre`, a pass Doppler, skimmed; every station once at its frequency. */
static void synthCase(const char* name, double fs, double centre, double lo, double hi, bool lsb, double dopMax,
                      const std::vector<Station>& sts) {
    std::printf("── %s ──\n", name);
    const double seconds = 47.0;
    const double now = vibeUtcNow();
    const double t0 = std::ceil(now / 15.0) * 15.0 - 1.0;   // ★ today's slot grid: the slice starts 1 s before a slot
    std::vector<cf32> iq((size_t)(seconds * fs));
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.0f, 1.0f);
    const double sigma = 1.0;
    for (auto& v : iq) v = cf32(g(rng), g(rng));
    for (const Station& s : sts) {
        const auto t = tonesOf(s.msg, s.ft4);
        if (t.empty()) { ok(false, std::string("encode ") + s.msg); continue; }
        const double slotSec = s.ft4 ? 7.5 : 15.0;
        addFtx(iq, fs, s.satHz - centre, 1.0 + s.slot * slotSec + 0.5, t, s.ft4, s.snr, sigma, lsb);
    }
    PassDoppler pd{ dopMax, seconds / 2, 0.08 };
    applyDoppler(iq, fs, pd, 0.0);
    Sink sink;
    satskim::Skimmer::Config c;
    c.centreHz = centre; c.loHz = lo; c.hiHz = hi; c.lsb = lsb; c.workers = 4;
    c.emit = [&](const digi::Spot& s, double rf, const satskim::Tag& t) { sink.add(s, rf, t); };
    c.tag = [] { satskim::Tag t; t.via = "TEST-1"; t.norad = 99999; t.passAos = 1.0; return t; };
    satskim::Skimmer sk(c);
    std::printf("     %d windows, %.1f s of %.0f kS/s, Doppler ±%.0f Hz (steepest %.0f Hz/s)\n", (int)sk.dials().size(), seconds, fs / 1000,
                dopMax, dopMax * pd.k);
    runThroughFinder(iq, fs, centre, lo, hi, lsb, pd, t0, sk);
    int all = 0, once = 0, placed = 0;
    for (const Station& s : sts) {
        // ★ the frequency of an FTx spot is its tone 0: USB = above the carrier, an LSB downlink's tone 0 the highest
        const std::string text = s.msg;
        const int n = sink.count(text);
        const Got* gg = sink.find(text);
        if (n >= 1) ++all;
        if (n == 1) ++once;
        if (gg && std::fabs(gg->rf - s.satHz) < 12.0 && gg->mode == (s.ft4 ? "FT4" : "FT8") && gg->tag.via == "TEST-1") ++placed;
        if (n != 1 || !gg || std::fabs(gg->rf - s.satHz) >= 12.0)
            std::printf("     %-24s %s %.0f Hz: decoded %d× %s\n", s.msg, s.ft4 ? "FT4" : "FT8", s.satHz, n, gg ? ("at " + std::to_string((long long)gg->rf)).c_str() : "");
    }
    const auto st = sk.stats();
    std::printf("     %d / %zu decoded; %llu duplicates dropped; %llu tasks, %.1f ms a task (mean), %.0f ms CPU a slot\n", all, sts.size(),
                (unsigned long long)st.dupes, (unsigned long long)st.tasksDecoded, st.meanTaskMs, st.cpuMsPerSlot);
    ok(all == (int)sts.size(), "every station decoded (" + std::to_string(all) + "/" + std::to_string(sts.size()) + ")");
    ok(once == (int)sts.size(), "each exactly once (the overlap's second copy dropped)");
    ok(placed == (int)sts.size(), "each at its own satellite-frame frequency (±12 Hz), the right mode, the tag it was cut under");
}

int main() {
    // ── 1. the windows ──────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("── 1. the windows ──\n");
    {
        const auto d = satskim::windowDials(145855000, 145875000);
        bool cover = !d.empty() && d.front() + satskim::kWinAudioLoHz <= 145855000 - satskim::kEdgeHz + 1
                  && d.back() + satskim::kWinAudioHiHz >= 145875000 + satskim::kEdgeHz - 1;
        bool overlap = true;
        for (size_t i = 1; i < d.size(); ++i) overlap = overlap && (d[i - 1] + satskim::kWinAudioHiHz) - (d[i] + satskim::kWinAudioLoHz) >= 299;
        ok(d.size() == 9, "JO-97's 20 kHz passband: 9 windows (" + std::to_string(d.size()) + ")");
        ok(cover && overlap, "they cover it and 1.4 kHz either side, neighbours overlapping by 300 Hz");
        const double mid = 0.5 * (d.front() + satskim::kWinAudioLoHz + d.back() + satskim::kWinAudioHiHz);
        ok(std::fabs(mid - 145865000) < 1, "centred on the passband");
        ok(satskim::windowDials(435610000, 435670000).size() == 25 && satskim::windowDials(435800000, 435900000).size() == 41,
           "RS-44's 60 kHz: 25 windows · FO-29's 100 kHz: 41");
        ok(satskim::windowDials(0, 10e6).size() == (size_t)satskim::kMaxWindows, "bounded at 64 windows whatever is asked");
    }

    // ── 2. Stuart's clip ────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("── 2. Stuart's JO-97 FT4, through the finder's mixer and the skimmer ──\n");
    {
        const char* clip = "vibeserver/testdata-sat/jo97-ft4-2026-11-27T205458Z-12k.wav";
        std::vector<float> a12; int rate = 0;
        if (!readWav16(clip, a12, rate) || rate != 12000) { std::printf("  not run — %s missing\n", clip); return 3; }
        // ★ the slice JO-97's pick asks for: centre 145.8575 (its span with the telemetry), skimmed over the transponder
        const double centre = 145857500.0, dial = 145857000.0, fs = 48000.0;
        // ★ analytic upsampling 12 → 48 kHz in the frequency domain: only the audio's positive frequencies (the USB the
        //   recording was made in), so no image lands anywhere in the slice; then the dial's place in the slice
        const size_t N = 262144, M = 4 * N;   // 21.8 s at 12 kHz
        std::vector<cd> X(N);
        for (size_t i = 0; i < N; ++i) X[i] = i < a12.size() ? cd(a12[i], 0) : cd(0);
        fft(X, false);
        std::vector<cd> Y(M, cd(0));
        for (size_t k = 1; k < N / 2; ++k) Y[k] = 8.0 * X[k];
        fft(Y, true);
        std::vector<cf32> iq(a12.size() * 4);
        const double shift = dial - centre;
        double rms = 0;
        for (float v : a12) rms += (double)v * v;
        rms = std::sqrt(rms / a12.size());
        std::mt19937 rng(3);
        std::normal_distribution<float> g(0.0f, 1.0f);
        // ★ a little noise across the whole slice (20 dB under the recording's own density — PF7X is a marginal pass-2 decode) — the windows outside the
        //   audio are not silent, as they never are on air
        const float nz = (float)(rms * std::sqrt(fs / 3000.0 / 2.0) * 0.1);
        for (size_t i = 0; i < iq.size(); ++i) {
            const cd y = Y[i] / (double)M;
            const double ph = 2 * M_PI * shift * (double)i / fs;
            const cd v = y * cd(std::cos(ph), std::sin(ph));
            iq[i] = cf32((float)v.real() + nz * g(rng), (float)v.imag() + nz * g(rng));
        }
        PassDoppler pd{ 3200.0, std::getenv("SKIM_TCA") ? std::atof(std::getenv("SKIM_TCA")) : 9.0, 0.06 };   // ★ 145 MHz: ±3.2 kHz, ~190 Hz/s at its steepest — far more than the real pass
        applyDoppler(iq, fs, pd, 0.0);
        const double tRec = 1795812898.0;
        const double t0 = tRec + std::ceil((vibeUtcNow() - tRec) / 15.0) * 15.0;   // ★ on today's slot grid (test-sat-ft4)
        Sink sink;
        satskim::Skimmer::Config c;
        c.centreHz = centre; c.loHz = 145855000; c.hiHz = 145875000; c.workers = std::getenv("SKIM_W") ? std::atoi(std::getenv("SKIM_W")) : 2;
        c.emit = [&](const digi::Spot& s, double rf, const satskim::Tag& t) { sink.add(s, rf, t); };
        c.tag = [] { satskim::Tag t; t.via = "JO-97"; t.norad = 43803; t.passAos = 1795812600; return t; };
        std::vector<std::string> logs; std::mutex lm;
        c.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); };
        satskim::Skimmer sk(c);
        runThroughFinder(iq, fs, centre, 145840000, 145875000, false, pd, t0, sk);
        const Got* a = sink.find("CQ DL6UMR JO71");
        const Got* b = sink.find("CQ PF7X JO22");
        for (auto& gg : sink.got) std::printf("     %s %-18s %.0f Hz %+d dB dt %.2f via %s %lld\n", gg.mode.c_str(), gg.text.c_str(), gg.rf, gg.snr, gg.dt, gg.tag.via.c_str(), gg.tag.norad);
        ok(a && b, "★ both CQs decode through the skimmer: DL6UMR (pass 1) and PF7X (pass 2)");
        ok(a && b && a->mode == "FT4" && b->mode == "FT4", "…as FT4");
        ok(a && b && std::fabs(a->rf - (dial + 2020.8)) < 15 && std::fabs(b->rf - (dial + 1958.3)) < 15,
           "at the dial + audio offset — the satellite frame (" + std::to_string((long long)(a ? a->rf : 0)) + ", " + std::to_string((long long)(b ? b->rf : 0)) + ")");
        ok(a && b && a->tag.via == "JO-97" && a->tag.norad == 43803 && a->tag.passAos == 1795812600 && b->tag.via == "JO-97",
           "each carries the tag its slot was cut under (via JO-97, NORAD 43803, the pass)");
        ok(sink.count("CQ DL6UMR JO71") == 1 && sink.count("CQ PF7X JO22") == 1, "each once (two windows hear 1958–2021 Hz? the copy is dropped)");
        const auto st = sk.stats();
        ok(st.tasksDropped == 0 && st.slotsDropped == 0, "nothing dropped");
    }

    // ── 3. synthetic passbands ──────────────────────────────────────────────────────────────────────────────────────
    {
        // windows (RS-44 span 435.610–435.670): dial_i = first + i·2500; a station in the overlap of windows 4 and 5
        const auto d = satskim::windowDials(435610000, 435670000);
        const double ovl = d[5] + 210.0;   // ★ inside both window 4 (…+2710) and window 5 (+210)
        std::vector<Station> rs = {
            { "CQ G4ABC IO91", false, 435612340, -12, 0 },
            { "CQ DL1XYZ JO62", true, 435624150, -10, 0 },
            { "CQ F5AAA JN18", false, 435637800, -14, 1 },
            { "CQ JA1QRZ PM95", true, ovl, -9, 1 },
            { "CQ W1AW FN31", false, 435651500, -11, 0 },
            { "CQ VK2DEF QF56", true, 435662200, -12, 3 },
            { "CQ PY2ABC GG66", false, 435669400, -10, 1 },   // ★ the very top of the passband
            { "CQ ZS6XYZ KG33", true, 435615600, -13, 2 },
        };
        synthCase("3a. RS-44 (inverting, USB downlink), 60 kHz at 435 MHz, ±10 kHz Doppler", 96000, 435640000, 435610000, 435670000, false, 10000, rs);

        std::vector<Station> ao = {
            { "CQ G0AAA IO83", false, 29412000, -12, 0 },
            { "CQ EA4BBB IN80", true, 29437500, -10, 1 },
            { "CQ K9CCC EN52", false, 29461000, -13, 1 },
            { "CQ OH2DDD KP20", true, 29488800, -11, 2 },
        };
        synthCase("3b. AO-7 Mode A (NON-inverting, USB downlink), 100 kHz at 29 MHz, ±700 Hz Doppler", 120000, 29450000, 29400000, 29500000, false, 700, ao);

        std::vector<Station> lsb = {
            { "CQ SM5EEE JO89", false, 145868000, -11, 0 },
            { "CQ I2FFF JN45", true, 145861200, -10, 1 },
            { "CQ ON4GGG JO20", false, 145874100, -12, 1 },
        };
        synthCase("3c. an LSB downlink (\"sb\":\"lsb\" — the mirror path), 20 kHz at 145 MHz, ±3.5 kHz Doppler", 48000, 145865000, 145855000, 145875000, true, 3500, lsb);
    }

    // ── 4. the catch-up queue ───────────────────────────────────────────────────────────────────────────────────────
    std::printf("── 4. the catch-up queue: bounded, the oldest dropped and logged ──\n");
    {
        satskim::Skimmer::Config c;
        c.centreHz = 435640000; c.loHz = 435610000; c.hiHz = 435670000;   // 25 windows
        c.mode = satskim::Skimmer::Mode::CatchUp; c.workers = 1;
        c.capBytes = 30 * satskim::Skimmer::kBufBytes;                    // ★ 10.8 MB: about one slot of 25 windows
        std::vector<std::string> logs; std::mutex lm;
        c.log = [&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); logs.push_back(l); };
        satskim::Skimmer sk(c);
        const double fs = 96000;
        std::mt19937 rng(11);
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::vector<cf32> blk((size_t)(fs / 50));
        const double t0 = std::ceil(vibeUtcNow() / 15.0) * 15.0;
        const int secs = 135;   // ★ nine slots, fed far faster than one worker decodes 75 a slot
        size_t peak = 0;
        for (int b = 0; b < secs * 50; ++b) {
            for (auto& v : blk) v = cf32(g(rng), g(rng));
            sk.feed(blk.data(), (int)blk.size(), fs, t0 + (b + 1) * 0.02);
            if (b % 50 == 0) peak = std::max(peak, sk.stats().queuedBytes);
        }
        const auto mid = sk.stats();
        ok(mid.peakBytes <= c.capBytes && peak <= c.capBytes, "the queue never held more than its cap (" + std::to_string(mid.peakBytes / 1048576.0).substr(0, 4) + " of "
           + std::to_string(c.capBytes / 1048576.0).substr(0, 4) + " MB)");
        ok(mid.slotsDropped >= 3, "the oldest slots were dropped (" + std::to_string(mid.slotsDropped) + ")");
        std::vector<std::string> dropped;
        { std::lock_guard<std::mutex> lk(lm);
          for (auto& l : logs) { auto p = l.find("the oldest slot, "); if (p != std::string::npos) dropped.push_back(l.substr(p + 17, 8)); } }
        ok(!dropped.empty() && std::is_sorted(dropped.begin(), dropped.end()), "…logged, oldest first (" + (dropped.empty() ? std::string() : dropped.front() + " … " + dropped.back()) + ")");
        // the newest slot is still queued (never the newest dropped)
        ok(mid.queuedSlots >= 1, "the newest audio is kept, waiting to be worked off");
        sk.finish();
        ok(sk.waitIdle(300), "…and is worked off after the input ends");
        const auto end = sk.stats();
        const uint64_t enq = end.tasksDecoded + end.tasksDropped;
        // nine whole slots (the first starts on its boundary): windows × (1 FT8 + 2 FT4) each
        const uint64_t tps = 3 * sk.dials().size();
        ok(enq >= 8 * tps && enq <= 9 * tps, "every task enqueued is accounted for: " + std::to_string(end.tasksDecoded) + " decoded + "
           + std::to_string(end.tasksDropped) + " dropped");
        std::printf("     one worker: %.1f ms a task (mean) on this machine\n", end.meanTaskMs);
    }

    // ── 5. defaults ─────────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("── 5. the benchmark's default and the cap ──\n");
    {
        using S = satskim::Skimmer;
        ok(S::defaultMode(NAN, 25) == S::Mode::Live && S::defaultMode(INFINITY, 25) == S::Mode::Live, "no benchmark: live");
        ok(S::defaultMode(40, 25) == S::Mode::Live && S::defaultMode(25, 25) == S::Mode::CatchUp, "RS-44's 25 windows need 30 units: 40 → live, 25 → catch-up");
        ok(S::defaultMode(11, 9) == S::Mode::Live && S::defaultMode(8, 9) == S::Mode::CatchUp, "JO-97's 9 windows need 10.8: 11 → live, 8 → catch-up");
        const size_t cap = S::defaultCapBytes();
        ok(cap >= 8u * 1048576 && cap <= 64u * 1048576, "the cap scales with RAM inside 8–64 MB (" + std::to_string(cap / 1048576) + " MB here)");
    }
    std::printf("%d passed, %d failed\n%s\n", passes, fails, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
