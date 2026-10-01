// ★★★ DO THE ADVANCED RDS MEASUREMENTS DEPEND ON THE LISTENER'S PASSBAND OR THE SAMPLE RATE?
//
// The field report (Onfliner, 2026-09-29): his OWN FM test transmitter on 88.902 MHz, set to a KNOWN
// 3.0 kHz of RDS deviation, read through an Airspy on the web client's Advanced RDS panel:
//
//     ±100 kHz passband @ 3 MS/s    RDS avg 2.7      pilot 6.6
//     ±150 kHz          @ 3 MS/s    RDS 3.0-3.1      pilot 5.9-6.6
//     ±100 kHz          @ 6 MS/s    RDS 2.8          pilot 6.5
//     ±125 kHz          @ 6 MS/s    RDS 2.8-2.9      pilot 5.7-6.3
//
// A transmitter constant must not move when the LISTENER changes their filter or the owner changes
// the radio's sample rate. This test synthesises a broadcast with EXACTLY known pilot, RDS and peak
// deviation, runs it through the real RxPipeline at several capture rates and listener passbands
// (auto bandwidth included), and reads back what the Advanced RDS panel would be sent.
//
// ★ THE SIGNAL. Everything the panel measures is in it, at known levels:
//     · a 19 kHz pilot at 6.75 kHz (9 % — the middle of the 6.0-7.5 kHz spec window)
//     · stereo programme with TREBLE IN L-R: an 11 kHz tone in L only puts L-R energy at 49 kHz,
//       which is where real pop music puts it — and where the RDS guard band was found to sit
//       (see RdsDemod::process: it measured 51 kHz, not the 63 kHz its comment promised)
//     · spec-shaped RDS (IEC 62106 cos shaping, rdsdev_cal's exact pulse) carrying valid 0A
//       groups, normalised to a known ABSOLUTE PEAK — the definition rdsdev_cal and every
//       calibration note in rds.cpp use
//     · optionally ROTATING: the RDS carrier a fraction of a hertz off 3x the pilot, the way an
//       encoder that is not locked to the pilot looks on air (Onfliner's Jazz and Rock FM,
//       2026-09-29: "rotating 2-3 deg/s")
//
// ★ SILENT: no audio device is touched, the audio callback discards everything.
// ★ `--full` runs the whole matrix (the before/after table in the report); the default is the
//   cross-section the suite can afford.
#include "vibedsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::RdsDecoder;
using vibedsp::cf32;

static int failures = 0, checks = 0;
static void ok(bool cond, const std::string& what) {
    checks++;
    if (cond) { std::printf("   ok   %s\n", what.c_str()); return; }
    failures++;
    std::printf("   FAIL %s\n", what.c_str());
}

namespace {

constexpr double kFb = 1187.5;          // RDS bit rate = 19000 / 16
constexpr double kPilotHz = 19000.0;
constexpr double kOffsetHz = 200000.0;  // the station sits 200 kHz off the capture centre

/** IEC 62106 transmit shaping, Ht(f) = cos(pi f / 4 fb) for |f| <= 2 fb — rdsdev_cal's exact
 *  numerical integration, so "unit peak" here means what it means in every note in rds.cpp. */
std::vector<double> shapingTaps(double fs, int halfLenBits) {
    const int half = (int)std::round(halfLenBits * fs / kFb);
    std::vector<double> h(2 * half + 1);
    const int NF = 2000;
    const double fmax = 2.0 * kFb, df = fmax / NF;
    for (int n = -half; n <= half; ++n) {
        const double t = n / fs;
        double acc = 0.0;
        for (int k = 0; k <= NF; ++k) {
            const double f = k * df;
            const double w = (k == 0 || k == NF) ? 0.5 : 1.0;
            acc += w * std::cos(M_PI * f / (4.0 * kFb)) * std::cos(2.0 * M_PI * f * t) * df;
        }
        h[n + half] = 2.0 * acc;
    }
    return h;
}

uint32_t encodeBlock(uint16_t data, int off) {
    const uint16_t cw = RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[off];
    return ((uint32_t)data << 10) | cw;
}

/** The shaped RDS baseband, one bit period = kSpb samples, unit ABSOLUTE peak, and periodic: the
 *  group sequence repeats, so the waveform is built for one period and read modulo it. */
struct RdsWave {
    static constexpr int kSpb = 128;    // samples per bit — 152 kHz, 63x the 2.4 kHz band
    std::vector<double> s;              // one period
    double fs = kFb * kSpb;
    explicit RdsWave(bool shaped) {
        // 0A groups carrying PS "VIBETEST" (PI 0xC0DE), then sixteen 2A groups of RadioText.
        // ★ The RadioText is PSEUDO-RANDOM on purpose. The absolute peak of a shaped biphase wave
        //   depends on the data (a worst-case run of ~8 bits sets it), and rdsdev_cal — whose
        //   definition of "peak deviation" every note in rds.cpp uses — takes it over 20000 random
        //   bits. Four PS groups alone repeat every 416 bits and never reach that worst case.
        std::vector<int> bits;
        auto group = [&](uint16_t b2, uint16_t c, uint16_t d) {
            const uint32_t blk[4] = { encodeBlock(0xC0DE, 0), encodeBlock(b2, 1), encodeBlock(c, 2), encodeBlock(d, 4) };
            for (int b = 0; b < 4; ++b) for (int k = 25; k >= 0; --k) bits.push_back((blk[b] >> k) & 1);
        };
        const char* PS = "VIBETEST";
        for (int addr = 0; addr < 4; ++addr)
            group((uint16_t)(addr & 3), 0x1234,
                  (uint16_t)(((uint8_t)PS[addr * 2] << 8) | (uint8_t)PS[addr * 2 + 1]));
        uint32_t lcg = 12345u;
        auto ch = [&]() { lcg = lcg * 1103515245u + 12345u; return (uint8_t)(0x20 + ((lcg >> 16) % 95)); };
        for (int seg = 0; seg < 16; ++seg) {
            const uint8_t a = ch(), b = ch(), c = ch(), d = ch();
            group((uint16_t)((2u << 12) | (uint16_t)seg), (uint16_t)((a << 8) | b), (uint16_t)((c << 8) | d));
        }
        // Differential encoding. If a period holds an odd number of 1s the differential stream comes
        // back inverted, so the true period is two of them.
        std::vector<int> m; int prev = 0;
        for (int rep = 0; rep < 2; ++rep)
            for (int b : bits) { prev ^= b; m.push_back(prev); }
        if (m[bits.size() - 1] == m.back()) m.resize(bits.size());   // even parity: one period repeats
        const int nb = (int)m.size();
        // ★ Unshaped = rectangular half-bit symbols, as a cheap encoder (or the benchmark's generator)
        //   sends them: its absolute peak IS its amplitude, and it spends ~20 % of its power outside
        //   the ±2.4 kHz band a receiver keeps.
        const std::vector<double> h = shaped ? shapingTaps(fs, 4) : std::vector<double>{ 1.0 };
        std::vector<double> p(kSpb + h.size() - 1, 0.0);      // one shaped biphase symbol
        for (int i = 0; i < kSpb; ++i) {
            const double sym = (i < kSpb / 2) ? 1.0 : -1.0;
            for (size_t k = 0; k < h.size(); ++k) p[i + k] += sym * h[k];
        }
        const long N = (long)nb * kSpb;
        s.assign((size_t)N, 0.0);
        for (int b = 0; b < nb; ++b) {
            const double d = m[b] ? 1.0 : -1.0;
            const long off = (long)b * kSpb;
            for (size_t k = 0; k < p.size(); ++k) s[(size_t)((off + (long)k) % N)] += d * p[k];
        }
        double pk = 0.0;
        for (double v : s) pk = std::max(pk, std::fabs(v));
        for (double& v : s) v /= pk;                          // ★ unit ABSOLUTE peak
    }
    double at(double tBits) const {                          // t in BITS, linear interpolation
        const double x = tBits * kSpb;
        const double N = (double)s.size();
        double f = std::fmod(x, N); if (f < 0) f += N;
        const long i0 = (long)f; const double a = f - (double)i0;
        const double v0 = s[(size_t)i0], v1 = s[(size_t)((i0 + 1) % (long)s.size())];
        return v0 + a * (v1 - v0);
    }
};
const RdsWave& rdsWave(bool shaped) {
    static RdsWave ws(true), wr(false);
    return shaped ? ws : wr;
}

struct Sig {
    double pilotKHz = 6.75;
    double rdsKHz = 3.0;
    double rotHz = 0.0;          // RDS carrier offset from 3x pilot; 0 = locked
    double rdsPhaseDeg = 0.0;    // RDS-to-pilot phase at t=0
    bool   trebleLmr = true;     // L-R energy at 49 kHz
    bool   shaped = true;        // IEC 62106 shaping (false = rectangular biphase)
    double programme = 1.0;      // scale on the audio (0 = pilot and RDS only)
    double noise = 0.0;          // white noise on I and Q, against a carrier of amplitude 0.5
    double echoAmp = 0.0;        // a reflection: this fraction of the signal, echoDelayUs later
    double echoDelayUs = 3.0;    // ~900 m of extra path
};

/** Streams IQ: a broadcast at kOffsetHz, FM-modulated at 75 kHz per unit MPX. */
struct Gen {
    Sig sig; double fs; long n = 0; double ph = 0.0; double mpxPeak = 0.0;
    /** ★★★ THE PEAK AS THE METER NOW DEFINES IT (2026-10-01): per 50 ms window, the level the composite
     *  reaches for 125 µs IN TOTAL — MPXtool's default peak response — and the highest of those. `mpxPeak`
     *  (the instantaneous maximum) was the truth until then, and it is exactly the definition Onfliner's
     *  side-by-side proved wrong: a sum of steady tones lines up for a few samples once in a while, and
     *  an instrument reading that read 1-8 kHz above MPXtool on air. It stays as the CEILING for the
     *  dropped-block check, where "never above the signal's own maximum" is the right question. */
    double mpxPeak125 = 0.0;
    std::vector<float> win;
    void closeWin() {
        const size_t k = (size_t)std::lround(125e-6 * fs);
        if (win.size() > k) {
            std::nth_element(win.begin(), win.begin() + (long)k, win.end(), std::greater<float>());
            mpxPeak125 = std::max(mpxPeak125, (double)win[k]);
        }
        win.clear();
    }
    double offset = kOffsetHz;   // where the station sits in the capture (0 = at DC)
    std::mt19937 rng{11};
    std::vector<cf32> hist; int hi = 0;   // the echo's delay line
    std::normal_distribution<double> gauss{0.0, 1.0};
    Gen(const Sig& s, double rate) : sig(s), fs(rate) {}
    void fill(std::vector<cf32>& out, int cnt) {
        out.resize((size_t)cnt);
        const RdsWave& W = rdsWave(sig.shaped);
        const double aP = sig.pilotKHz / 75.0, aR = sig.rdsKHz / 75.0;
        // The encoder's clock: locked = the pilot's; rotating = scaled so bits AND carrier drift together.
        const double clk = 1.0 + sig.rotHz / 57000.0;
        for (int i = 0; i < cnt; ++i, ++n) {
            const double t = (double)n / fs;
            const double L = sig.programme * 0.50 * std::sin(2 * M_PI * 400.0 * t)
                           + (sig.trebleLmr ? 0.30 * std::sin(2 * M_PI * 11000.0 * t) : 0.0);
            const double R = sig.programme * 0.50 * std::sin(2 * M_PI * 1300.0 * t);
            const double wp = 2 * M_PI * kPilotHz * t;
            const double mpx = 0.9 * (0.5 * (L + R) + 0.5 * (L - R) * std::cos(2 * wp))
                             + aP * std::cos(wp)
                             + aR * W.at(t * kFb * clk) * std::cos(3 * wp * clk + sig.rdsPhaseDeg * M_PI / 180.0);
            if (t > 0.01) {
                mpxPeak = std::max(mpxPeak, std::fabs(mpx));
                win.push_back((float)std::fabs(mpx));
                if (win.size() >= (size_t)(0.05 * fs)) closeWin();
            }
            ph += 2 * M_PI * (offset + 75000.0 * mpx) / fs;
            if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            double re = 0.5 * std::cos(ph), im = 0.5 * std::sin(ph);
            if (sig.echoAmp > 0.0) {
                // y = x + a·e^{j1.1}·x[n−D]: a static reflection, the comb that makes FM's envelope move
                const int D = std::max(1, (int)std::lround(sig.echoDelayUs * 1e-6 * fs));
                if ((int)hist.size() != D) { hist.assign((size_t)D, cf32(0.0f, 0.0f)); hi = 0; }
                const cf32 old = hist[(size_t)hi];
                hist[(size_t)hi] = cf32((float)re, (float)im); hi = (hi + 1) % D;
                const double c = std::cos(1.1) * sig.echoAmp, sn = std::sin(1.1) * sig.echoAmp;
                re += c * old.real() - sn * old.imag();
                im += sn * old.real() + c * old.imag();
            }
            if (sig.noise > 0.0) { re += sig.noise * gauss(rng); im += sig.noise * gauss(rng); }
            out[(size_t)i] = cf32((float)re, (float)im);
        }
    }
};

struct Reading {
    float pilot = 0, rdsAvg = 0, rdsPk = 0, rdsRaw = 0, mpxHold = 0, phase = -1, coh = 0, drift = 0;
    float snr = 0, mp = 0; int snrOk = 0, mpOk = 0, measured = 0;
    int groups = 0; int calls = 0; bool eye = false; unsigned dropped = 0;
};
struct Cap {
    Reading r;
    static void onExt(void* c, const RxPipeline::Callbacks::RdsExt& x) {
        auto* p = (Cap*)c;
        p->r.pilot = x.pilotDevKHz; p->r.rdsAvg = x.rdsDevKHz; p->r.rdsPk = x.rdsDevPeakKHz;
        p->r.rdsRaw = x.rdsDevRawKHz; p->r.mpxHold = x.mpxDevHoldKHz;
        p->r.phase = x.pilotPhaseDeg; p->r.coh = x.pilotPhaseCoherence; p->r.drift = x.pilotPhaseDriftDegPerSec;
        p->r.groups = x.groupTotal; p->r.calls++;
        p->r.eye = x.eyeBand[0] != nullptr && x.eyeW > 0;
#ifdef VIBEDSP_HAS_MPXMEASURE
        p->r.snr = x.mpxSnrDb; p->r.snrOk = x.snrOk; p->r.mp = x.multipath; p->r.mpOk = x.multipathOk;
        p->r.measured = x.measured;
#endif
    }
    static void onAudio(void*, const float*, int, int, int) {}   // ★ silent — nothing leaves
    static void onPs(void*, uint16_t, const char*) {}
};

bool gThreaded = false;     // run the instrument on its worker (the production path)
bool gBlocking = true;      // and make it wait rather than drop (false = the live server's policy)

Reading run(double fs, double bw, double autoBw, const Sig& sig, double seconds, double* truthPeak = nullptr) {
    static std::atomic<bool> wanted{true};
    Cap cap;
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.ctx = &cap; cb.audio = &Cap::onAudio; cb.rdsPs = &Cap::onPs; cb.rdsExt = &Cap::onExt;
    rx.setRdsExtWantedFlag(&wanted);
#ifdef VIBEDSP_HAS_MPXMEASURE
    // ★ Inline by default, so the figures are the SIGNAL's and not a race with a worker thread. The
    //   threaded path — same code, other thread — is checked against it separately.
    rx.setMeasureThread(gThreaded);
    rx.setMeasureBlocking(gBlocking);
#endif
    rx.start(fs, 1024, 10.0, 48000, cb);
    rx.setTune(kOffsetHz, RxPipeline::Mode::WFM, bw);
    rx.setRdsNoiseCorrection(true);             // ★ what the server does while the panel is open
    if (autoBw > 0.0) rx.setAutoBandwidth(autoBw);
    Gen gen(sig, fs);
    std::vector<cf32> buf;
    const int block = (int)std::max(4096.0, fs / 100.0);  // ~10 ms blocks, like a radio
    const long total = (long)(fs * seconds);
    for (long done = 0; done < total; done += block) {
        gen.fill(buf, (int)std::min<long>(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
    }
#ifdef VIBEDSP_HAS_MPXMEASURE
    cap.r.dropped = rx.measureDropped();
#endif
    rx.stop();
    if (truthPeak) *truthPeak = gen.mpxPeak125 * 75.0;   // ★ the 125 µs peak — see Gen::mpxPeak125
    return cap.r;
}

/** ★★ THE SHARED-DIAL PATH: the capture through the Channelizer (the server's one forward FFT),
 *  one listener's channel extracted exactly as feedOneClient() does, into that listener's own
 *  pipeline at the channel rate. The channel is sized like chanBinsFor(): the smallest power of two
 *  of bins whose rate reaches `need`. */
Reading runChannelised(double fs, int fftSize, double need, double bw, const Sig& sig, double seconds,
                       double* chanRateOut = nullptr) {
    static std::atomic<bool> wanted{true};
    int b = 64;
    while (b < fftSize && fs * b / fftSize < need) b <<= 1;
    const double chanRate = fs * b / fftSize, binHz = fs / fftSize;
    const int centreBin = (int)std::lround(kOffsetHz / binHz);
    if (chanRateOut) *chanRateOut = chanRate;
    Cap cap;
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.ctx = &cap; cb.audio = &Cap::onAudio; cb.rdsPs = &Cap::onPs; cb.rdsExt = &Cap::onExt;
    rx.setRdsExtWantedFlag(&wanted);
#ifdef VIBEDSP_HAS_MPXMEASURE
    rx.setMeasureThread(false);
#endif
    rx.start(chanRate, 1024, 10.0, 48000, cb);
    rx.setTune(kOffsetHz - centreBin * binHz, RxPipeline::Mode::WFM, bw);   // the residual, as the shim does
    rx.setRdsNoiseCorrection(true);
    vibedsp::Channelizer ch(fftSize);
    vibedsp::Channelizer::ExtractCtx ctx;
    std::vector<cf32> slice((size_t)b), buf;
    Gen gen(sig, fs);
    const int block = (int)std::max(4096.0, fs / 100.0);
    const long total = (long)(fs * seconds);
    for (long done = 0; done < total; done += block) {
        gen.fill(buf, (int)std::min<long>(block, total - done));
        ch.feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int) {
            const int got = ch.extract(bins, centreBin, b, slice.data(), ctx, ch.blockIndex());
            if (got > 0) rx.feed(slice.data(), got);
        });
    }
    rx.stop();
    return cap.r;
}

/** ★★★ A STREAM WITH HOLES IN IT — what a loaded server hands the DSP. Every `every` seconds a
 *  stretch of `holeLen` samples is generated (the transmitter carries on) and NOT fed: the two sides
 *  of the hole abut, exactly as they do when a radio library drops a USB buffer or a listener's
 *  thread drops a block. With `tell`, the feeder says so first (RxPipeline::noteInputGap), as the
 *  server now does. Reports the Advanced RDS figures at the end and the LISTENER's block error rate
 *  (the panel's ERRORS) averaged from `warm` seconds on, plus how often it read -1 after first sync. */
struct HoleRun {
    Reading r;
    double berAvg = -1.0; double berNegFrac = 0.0; int berN = 0;
    std::string ps; unsigned gaps = 0; int holes = 0;
};
struct BerCap : Cap {
    long berSum = 0; int berN = 0, berNeg = 0; bool synced = false; bool counting = false;
    std::string ps;
    static void onBer(void* c, int pct) {
        auto* p = (BerCap*)c;
        if (pct >= 0) p->synced = true;
        if (!p->counting || !p->synced) return;
        if (pct < 0) { p->berNeg++; return; }
        p->berSum += pct; p->berN++;
    }
    static void onPs(void* c, uint16_t, const char* ps8) { ((BerCap*)c)->ps.assign(ps8, strnlen(ps8, 8)); }
};
HoleRun runHoles(double fs, const Sig& sig, double seconds, double every, int holeLen, bool tell,
                 double warm = 2.0) {
    static std::atomic<bool> wanted{true};
    BerCap cap;
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.ctx = &cap; cb.audio = &Cap::onAudio; cb.rdsPs = &BerCap::onPs; cb.rdsExt = &Cap::onExt;
    cb.rdsBer = &BerCap::onBer;
    rx.setRdsExtWantedFlag(&wanted);
    rx.setMeasureThread(false);
    rx.start(fs, 1024, 10.0, 48000, cb);
    rx.setTune(kOffsetHz, RxPipeline::Mode::WFM, 200000.0);
    rx.setRdsNoiseCorrection(true);
    Gen gen(sig, fs);
    std::vector<cf32> buf, hole;
    const int block = (int)std::max(4096.0, fs / 100.0);
    const long total = (long)(fs * seconds);
    double nextHole = every > 0.0 ? 1.0 : 1e9;         // the first after the decoder has locked
    HoleRun out;
    for (long done = 0; done < total; ) {
        const double t = (double)done / fs;
        cap.counting = t >= warm;
        if (t >= nextHole) {
            gen.fill(hole, holeLen);                   // time passes on air…
            done += holeLen;                           // …and none of it reaches the receiver
            if (tell) rx.noteInputGap();
            out.holes++;
            nextHole += every;
            continue;
        }
        gen.fill(buf, (int)std::min<long>(block, total - done));
        rx.feed(buf.data(), (int)buf.size());
        done += (long)buf.size();
    }
    out.gaps = rx.inputGaps();
    rx.stop();
    out.r = cap.r;
    out.berN = cap.berN;
    out.berAvg = cap.berN ? (double)cap.berSum / cap.berN : -1.0;
    out.berNegFrac = (cap.berN + cap.berNeg) ? (double)cap.berNeg / (cap.berN + cap.berNeg) : 0.0;
    out.ps = cap.ps;
    return out;
}

/** ★★ THE SAME SUBCARRIER, DEMODULATED IDEALLY — no FM, no channel, no discriminator: the RDS
 *  term of the multiplex straight into an RdsDemod at 192 kS/s with perfect 57 kHz references and
 *  bit clock (test_rds_dsp's direct method). What the pipeline reads divided by this IS the transfer
 *  of everything between the aerial and the RDS demodulator — the quantity kRdsChainGain undoes.
 *  Averaged after the first 2 s, like the panel: the demod's own envelope mean is ~40 ms long and
 *  follows the data pattern. */
float idealRaw(const Sig& sig, double seconds, float* peakOut = nullptr) {
    const double R = 192000.0;
    vibedsp::RdsDemod d;
    d.configure(R, RdsDecoder::Callbacks{});
    d.setNoiseCorrection(true);
    const RdsWave& W = rdsWave(sig.shaped);
    const int blk = 1920;
    std::vector<float> mpx(blk), r57(blk), r57q(blk), clk(blk);
    const double aR = sig.rdsKHz / 75.0;
    long n = 0;
    double acc = 0.0; long cnt = 0;
    for (int b = 0; b < (int)(seconds * R / blk); ++b) {
        for (int i = 0; i < blk; ++i, ++n) {
            const double t = n / R, w3 = 3.0 * 2.0 * M_PI * kPilotHz * t;
            mpx[i] = (float)(aR * W.at(t * kFb) * std::cos(w3));
            r57[i] = (float)std::cos(w3); r57q[i] = (float)std::sin(w3);
            clk[i] = (float)std::fmod(2.0 * M_PI * kFb * t, 2.0 * M_PI);
        }
        d.setPilotRef(0.09f);
        d.process(mpx.data(), r57.data(), r57q.data(), clk.data(), blk);
        int af[32]; d.mergedAf(af, 32);
        if (n > (long)(2.0 * R) && d.rdsDeviationRawKHz() > 0.0f) { acc += d.rdsDeviationRawKHz(); ++cnt; }
    }
    if (peakOut) *peakOut = d.rdsDeviationPeakKHz();
    return cnt ? (float)(acc / cnt) : -1.0f;
}

struct Cfg { double fs; double bw; double autoBw; const char* label; };

std::string fmt(float v) { char b[32]; std::snprintf(b, sizeof b, "%6.2f", v); return b; }
bool near(double a, double b, double frac) { return std::fabs(a - b) <= frac * std::fabs(b); }

double cpuNow() { return (double)std::clock() / CLOCKS_PER_SEC; }

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool full = mode == "--full";
    const double secs = std::getenv("VIBE_TEST_SECS") ? std::atof(std::getenv("VIBE_TEST_SECS")) : 6.5;
#ifdef VIBEDSP_HAS_MPXMEASURE
    const bool after = true;
#else
    const bool after = false;
#endif

#ifdef VIBEDSP_HAS_MPXMEASURE
    // ── The instrument's filters, characterised from their own taps at every capture rate ──
    if (mode.empty() || full) {
        std::printf("\nThe measurement path's filters, from their taps\n");
        std::printf("   %-7s %-44s %9s %9s %9s  %7s %7s %7s %7s\n", "MS/s", "plan", "ripple dB", "@175k dB",
                    "stop dB", "19k", "57k", "63k", "80k");
        for (double r : { 768000.0, 912000.0, 1024000.0, 2048000.0, 2400000.0, 2500000.0, 3000000.0,
                          6000000.0, 8000000.0, 10000000.0 }) {
            vibedsp::MpxMeasure m;
            m.configure(r);
            m.build();
            double lo = 1e9, hi = -1e9, stop = -1e9;
            for (double f = 0.0; f <= vibedsp::MpxMeasure::kPassHz; f += 1000.0) {
                const double g = 20.0 * std::log10(m.channelGain(f));
                lo = std::min(lo, g); hi = std::max(hi, g);
            }
            // From the stop edge out to 600 kHz — the next two channels each side.
            for (double f = vibedsp::MpxMeasure::kStopHz; f <= std::min(600000.0, r / 2); f += 1000.0)
                stop = std::max(stop, 20.0 * std::log10(std::max(1e-12, m.channelGain(f))));
            char plan[64]; std::snprintf(plan, sizeof plan, "-> chan %.3f kS/s", m.chanRate() / 1e3);
            std::printf("   %-7.3f %-44s %9.3f %9.1f %9.1f  %7.4f %7.4f %7.4f %7.4f\n", r / 1e6, plan, hi - lo,
                        20.0 * std::log10(m.channelGain(175000.0)), stop, m.mpxGain(19000.0), m.mpxGain(57000.0),
                        m.mpxGain(63000.0), m.mpxGain(80000.0));
            const bool flat = (hi - lo) < 0.02;
            const bool rej = stop < -70.0;
            const bool mpxFlat = std::fabs(m.mpxGain(19000.0) - 1.0) < 0.002 && std::fabs(m.mpxGain(57000.0) - 1.0) < 0.002;
            const bool fixedRate = std::fabs(m.chanRate() - vibedsp::MpxMeasure::kChanRate) < 1.0;
            char what[160];
            std::snprintf(what, sizeof what, "filters at %.3f MS/s: one 384 kS/s channel, flat to ±150 kHz (<0.02 dB), "
                          ">70 dB from ±200 kHz, multiplex flat at 19/57 kHz (<0.2 %%)", r / 1e6);
            ok(flat && rej && mpxFlat && fixedRate, what);
        }
    }

    // ── What the instrument COSTS: the whole path alone, inline, per second of signal ──
    if (mode == "--cpu" || full) {
        std::printf("\nCPU of the measurement path alone (this machine, one core, %% of real time)\n");
        for (double r : { 2048000.0, 2400000.0, 3000000.0, 6000000.0, 8000000.0 }) {
            Sig s; Gen g(s, r); g.offset = 0.0;
            std::vector<cf32> iq;
            g.fill(iq, (int)r);                       // one second of signal, generated up front
            vibedsp::MpxMeasure m;
            m.setThreaded(false);
            m.configure(r);
            const int blk = (int)(r / 100.0);
            const int reps = 6;
            const double c0 = cpuNow();
            for (int k = 0; k < reps; ++k)
                for (int o = 0; o + blk <= (int)iq.size(); o += blk) m.feed(iq.data() + o, blk, 1);
            const double used = cpuNow() - c0;
            // …and, for scale, the listener's whole WFM pipeline on the same second of signal, with the
            // panel closed (nothing of the instrument runs) and open (inline, so it is counted here).
            double pipe[2] = { 0.0, 0.0 };
            for (int open = 0; open < 2; ++open) {
                static std::atomic<bool> flag{false};
                flag.store(open != 0);
                Cap cap; RxPipeline rx; RxPipeline::Callbacks cb{};
                cb.ctx = &cap; cb.audio = &Cap::onAudio; cb.rdsPs = &Cap::onPs; cb.rdsExt = &Cap::onExt;
                rx.setRdsExtWantedFlag(&flag);
                rx.setMeasureThread(false);
                rx.start(r, 1024, 10.0, 48000, cb);
                rx.setTune(0.0, RxPipeline::Mode::WFM, 200000.0);
                const double c1 = cpuNow();
                for (int k = 0; k < reps; ++k)
                    for (int o = 0; o + blk <= (int)iq.size(); o += blk) rx.feed(iq.data() + o, blk);
                pipe[open] = (cpuNow() - c1) / reps;
                rx.stop();
            }
            std::printf("   %-6.3f MS/s   instrument alone %5.1f %% of a core   listener WFM %5.1f %% -> %5.1f %% with the panel open\n",
                        r / 1e6, 100.0 * used / reps, 100.0 * pipe[0], 100.0 * pipe[1]);
        }
        if (mode == "--cpu") return 0;
    }

    // ── The multipath meter's noise floor on THIS path — the numbers behind kMpSnr / kMpDepth ──
    if (mode == "--multipath") {
        std::printf("\nMultipath depth with NO echo, against the S/N the path reports (3 MS/s)\n");
        for (double sigma : { 0.0, 0.02, 0.05, 0.08, 0.12, 0.18, 0.25, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0, 3.0, 5.0 }) {
            Sig s; s.noise = sigma;
            Gen g(s, 3000000.0); g.offset = 0.0;
            vibedsp::MpxMeasure m;
            m.setThreaded(false);
            m.configure(3000000.0);
            std::vector<cf32> iq;
            double snr = 0, depth = 0; int n = 0;
            for (int b = 0; b < 800; ++b) {           // 8 s in 10 ms blocks, averaged over the last 4
                g.fill(iq, 30000);
                m.feed(iq.data(), 30000, 1);
                if (b >= 400) {
                    vibedsp::MpxMeasure::Out o; int ew = 0; unsigned seq = 0;
                    m.snapshot(o, nullptr, ew, nullptr, seq);
                    snr += o.snrDb; depth += o.multipathRaw; ++n;
                }
            }
            std::printf("   noise %.3f   S/N %6.1f dB   raw depth %.4f\n", sigma, snr / n, depth / n);
        }
        return 0;
    }
#endif

    if (mode == "--diag") {
        // Which ingredient moves which reading — one variable at a time.
        struct D { const char* name; Sig s; };
        Sig base; Sig rect = base; rect.shaped = false; Sig mono = base; mono.trebleLmr = false;
        Sig rectMono = rect; rectMono.trebleLmr = false;
        Sig quiet = base; quiet.programme = 0.0; quiet.trebleLmr = false;
        Sig noisy = base; noisy.noise = 0.03;
        {
            float pk = 0.0f;
            const float ir = idealRaw(base, secs, &pk);
            std::printf("   IDEAL demod of the same subcarrier: raw %.3f  peak %.3f  (truth %.2f)\n", ir, pk, base.rdsKHz);
        }
        for (const D& d : { D{ "shaped, treble L-R", base }, D{ "shaped, no treble", mono },
                            D{ "rectangular, treble", rect }, D{ "rectangular, no treble", rectMono },
                            D{ "no programme", quiet }, D{ "shaped, noisy", noisy } })
            for (double bw : { 200000.0, 300000.0 }) {
                const Reading r = run(3000000.0, bw, 0.0, d.s, secs);
                std::printf("   %-24s ±%3.0fk: pilot %.2f  avg %.2f  peak %.2f  raw %.2f  S/N %.1f\n", d.name, bw / 2e3,
                            r.pilot, r.rdsAvg, r.rdsPk, r.rdsRaw, r.snr);
            }
        return 0;
    }

    std::printf("\nAdvanced RDS measurements against a signal of KNOWN deviation (%s)\n",
                after ? "the fixed measurement path" : "the LISTENER's path — before");
    std::printf("   truth: pilot 6.75 kHz; RDS 3.00 kHz absolute peak, IEC-shaped; the station %.0f kHz off centre\n",
                kOffsetHz / 1e3);

    const double rates[] = { 2048000.0, 2400000.0, 3000000.0, 6000000.0, 8000000.0 };
    struct Pb { double bw; double autoBw; const char* label; };
    const Pb pbs[] = { { 150000.0, 0.0, "±75" }, { 200000.0, 0.0, "±100" }, { 250000.0, 0.0, "±125" },
                       { 300000.0, 0.0, "±150" }, { 200000.0, 134000.0, "auto 134k" } };
    std::vector<Cfg> cfgs;
    if (full) {
        for (double r : rates) for (const Pb& p : pbs) cfgs.push_back({ r, p.bw, p.autoBw, p.label });
    } else {
        // ★ The cross-section: both rate extremes and the middle, the two passbands Onfliner compared,
        //   and auto bandwidth — enough that a regression in either axis fails the suite.
        for (double r : { 2400000.0, 3000000.0, 8000000.0 })
            for (int k : { 1, 3, 4 }) cfgs.push_back({ r, pbs[k].bw, pbs[k].autoBw, pbs[k].label });
    }

    struct Case { const char* name; Sig sig; bool rotating; };
    Sig locked;                          // 3.0 kHz, in phase with the pilot, treble in L-R
    Sig rot = locked; rot.rotHz = 0.01;  // 3.6 deg/s — an encoder not locked to the pilot
    const std::vector<Case> cases = { { "3.0 kHz RDS, LOCKED to the pilot", locked, false },
                                      { "3.0 kHz RDS, ROTATING 3.6 deg/s against the pilot", rot, true } };

    float idealPk = 0.0f;
    const float ideal = idealRaw(locked, secs, &idealPk);
    std::printf("   the same subcarrier demodulated IDEALLY (no FM, no channel): raw %.3f, peak %.3f kHz\n\n",
                ideal, idealPk);

    std::vector<float> pil, avg, pkv, raw;
    for (const Case& c : cases) {
        std::printf("── %s ──\n", c.name);
        std::printf("   %-6s %-10s %7s %7s %7s %7s %13s %6s %5s %6s %6s\n", "MS/s", "passband", "pilot", "rdsAvg",
                    "rdsPeak", "rdsRaw", "mpxHold(true)", "phase", "coh", "drift", "S/N");
        for (const Cfg& g : cfgs) {
            double tp = 0.0;
            const Reading r = run(g.fs, g.bw, g.autoBw, c.sig, secs, &tp);
            std::printf("   %-6.3f %-10s %s  %s  %s  %s  %6.2f(%5.1f) %6.1f %5.2f %6.2f %6.1f\n", g.fs / 1e6, g.label,
                        fmt(r.pilot).c_str(), fmt(r.rdsAvg).c_str(), fmt(r.rdsPk).c_str(), fmt(r.rdsRaw).c_str(),
                        r.mpxHold, tp, r.phase, r.coh, r.drift, r.snr);
            std::fflush(stdout);
            pil.push_back(r.pilot); avg.push_back(r.rdsAvg); pkv.push_back(r.rdsPk); raw.push_back(r.rdsRaw);
            if (after) {
                char w[200];
                std::snprintf(w, sizeof w, "%.3f MS/s %s: pilot %.2f within 1.5 %% of 6.75, RDS peak %.2f within 3 %% "
                              "of 3.00, raw %.2f and avg %.2f within 2 %% of the ideal demod, MPX peak within 5 %%",
                              g.fs / 1e6, g.label, r.pilot, r.rdsPk, r.rdsRaw, r.rdsAvg);
                ok(near(r.pilot, 6.75, 0.015) && near(r.rdsPk, 3.0, 0.03) && near(r.rdsRaw, ideal, 0.02)
                   && near(r.rdsAvg, ideal, 0.02) && near(r.mpxHold, tp, 0.05), w);
                std::snprintf(w, sizeof w, "%.3f MS/s %s: %s — phase %.1f deg, coherence %.2f, drift %.2f deg/s",
                              g.fs / 1e6, g.label, c.rotating ? "rotation SEEN (drift 2.5-5 deg/s)"
                                                               : "locked encoder reads locked (< 5 deg, steady, no drift)",
                              r.phase, r.coh, r.drift);
                ok(c.rotating ? (r.drift > 2.5f && r.drift < 5.0f)
                              : (r.phase >= 0.0f && r.phase < 5.0f && r.coh > 0.9f && r.drift < 0.5f), w);
            }
        }
    }

    auto spread = [](const std::vector<float>& v) {
        float lo = 1e9f, hi = -1e9f, sum = 0.0f;
        for (float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); sum += x; }
        return std::make_tuple(lo, hi, (hi - lo) / std::max(1e-6f, sum / (float)v.size()));
    };
    std::printf("\n   SPREAD over every configuration, locked and rotating:\n");
    for (auto [name, v] : { std::make_pair("pilot  ", &pil), std::make_pair("rdsAvg ", &avg),
                            std::make_pair("rdsPeak", &pkv), std::make_pair("rdsRaw ", &raw) }) {
        const auto [lo, hi, rel] = spread(*v);
        std::printf("     %s %5.2f .. %5.2f kHz   (%.1f %%)\n", name, lo, hi, 100.0f * rel);
        if (after) ok(rel < 0.02f, std::string(name) + " spread under 2 % — the passband and the rate no longer move it");
    }

    // ★ Onfliner's other two transmitter settings, 1.2 and 5.1 kHz: a pure scale, not a curve.
    std::printf("\n── level linearity @ 3 MS/s ±100 (the default passband) ──\n");
    for (double lvl : { 1.2, 5.1 }) {
        Sig s = locked; s.rdsKHz = lvl;
        float ipk = 0.0f;
        const float id = idealRaw(s, secs, &ipk);
        const Reading r = run(3000000.0, 200000.0, 0.0, s, secs);
        std::printf("   RDS %.1f kHz: avg %.2f  peak %.2f  raw %.2f   (ideal demod: peak %.2f raw %.2f)\n", lvl, r.rdsAvg,
                    r.rdsPk, r.rdsRaw, ipk, id);
        if (after) {
            char w[160];
            /* ★ 3 % or 0.06 kHz, whichever is larger. Two fixed offsets ride on a small level, both
             *  measured: the peak histogram's 0.022 kHz bins (the upper edge is reported — the ideal
             *  demod reads 1.21 for 1.20), and the stereo treble at 49 kHz leaking ~53 dB down through
             *  the RDS demod's own ±2.4 kHz filter (+0.02-0.04 kHz; it is not there with no programme).
             *  That filter is the DECODER's and is left alone — see the note on RdsDemod::configure. */
            std::snprintf(w, sizeof w, "%.1f kHz: peak within 3 %% (or 0.06 kHz) of the set level, raw within 2 %% of the ideal demod", lvl);
            ok(std::fabs(r.rdsPk - lvl) <= std::max(0.03 * lvl, 0.06) && near(r.rdsRaw, id, 0.02), w);
        }
    }

#ifdef VIBEDSP_HAS_MPXMEASURE
    // ★★★ HOLES IN THE STREAM (2026-09-30). The Pi 500 under full load: the Airspy HF+ listener's
    //     ERRORS read ~25 % against ~1.5 % unloaded on a clean 30 dB signal, because the radio
    //     library dropped USB buffers and the decoder, not knowing, stayed "synced" on a grid the hole
    //     had shifted and counted ~25 blocks of its own confusion as link errors per hole. A hole the
    //     server KNOWS about is now signalled; this measures both sides of that and holds the fix to:
    //       · the listener's block error rate with signalled holes ≈ the hole-free rate, and never
    //         "-1, no RDS" in between;
    //       · every Advanced RDS figure with signalled holes ≈ the hole-free figure — a hole may make a
    //         figure late, never wrong.
    //     The holes are the two sizes the server makes at the HF+'s 912 kS/s: one USB buffer (2048
    //     samples, 2.2 ms) and one listener hand-off block (12288 samples, 13.5 ms), one a second.
    {
        std::printf("\n── holes in the stream (912 kS/s, one a second) ──\n");
        const double fs = 912000.0, hs = std::max(secs, 8.0);
        const HoleRun clean = runHoles(fs, locked, hs, 0.0, 0, false);
        std::printf("   no holes          : ERRORS %5.1f %%  (-1 %4.1f %% of reads)  PS \"%s\"  pilot %.2f raw %.2f peak %.2f "
                    "phase %.1f coh %.2f S/N %.1f mp %.3f MPX %.1f\n",
                    clean.berAvg, 100 * clean.berNegFrac, clean.ps.c_str(), clean.r.pilot, clean.r.rdsRaw, clean.r.rdsPk,
                    clean.r.phase, clean.r.coh, clean.r.snr, clean.r.mp, clean.r.mpxHold);
        for (int len : { 2048, 12288 }) {
            const HoleRun blind = runHoles(fs, locked, hs, 1.0, len, false);
            const HoleRun told  = runHoles(fs, locked, hs, 1.0, len, true);
            for (const HoleRun* h : { &blind, &told })
                std::printf("   %5d-sample holes, %-6s: ERRORS %5.1f %%  (-1 %4.1f %% of reads)  PS \"%s\"  pilot %.2f raw %.2f "
                            "peak %.2f phase %.1f coh %.2f S/N %.1f mp %.3f MPX %.1f  [%d holes, %u told]\n",
                            len, h == &blind ? "unsaid" : "told", h->berAvg, 100 * h->berNegFrac, h->ps.c_str(),
                            h->r.pilot, h->r.rdsRaw, h->r.rdsPk, h->r.phase, h->r.coh, h->r.snr, h->r.mp,
                            h->r.mpxHold, h->holes, h->gaps);
            char w[200];
            std::snprintf(w, sizeof w, "%d-sample holes: an UNSIGNALLED hole costs the decoder real errors (the fault "
                          "being fixed: %.1f %% vs %.1f %% clean)", len, blind.berAvg, clean.berAvg);
            ok(blind.berAvg > clean.berAvg + 5.0, w);
            std::snprintf(w, sizeof w, "%d-sample holes, signalled: ERRORS within 2 points of the hole-free run "
                          "(%.1f vs %.1f %%), PS intact, never -1 after sync", len, told.berAvg, clean.berAvg);
            ok(told.berAvg >= 0.0 && told.berAvg <= clean.berAvg + 2.0 && told.ps == "VIBETEST"
               && told.berNegFrac == 0.0 && told.gaps == (unsigned)told.holes, w);
            std::snprintf(w, sizeof w, "%d-sample holes, signalled: every Advanced RDS figure equals the hole-free "
                          "run (pilot/raw 1.5 %%, peak 3 %%, phase 2 deg, coh 0.03, S/N 1 dB, multipath 0.01, MPX 3 %%)", len);
            ok(told.r.measured && near(told.r.pilot, clean.r.pilot, 0.015) && near(told.r.rdsRaw, clean.r.rdsRaw, 0.015)
               && near(told.r.rdsAvg, clean.r.rdsAvg, 0.015) && near(told.r.rdsPk, clean.r.rdsPk, 0.03)
               && std::fabs(told.r.phase - clean.r.phase) <= 2.0f && std::fabs(told.r.coh - clean.r.coh) <= 0.03f
               && std::fabs(told.r.snr - clean.r.snr) <= 1.0f && std::fabs(told.r.mp - clean.r.mp) <= 0.01f
               && near(told.r.mpxHold, clean.r.mpxHold, 0.03), w);
        }
    }

    // ★ The production path: the same instrument on its own `vibe-mpx` thread. Blocking, so it sees
    //   every sample and must agree with the inline figures.
    {
        std::printf("\n── on the worker thread (the production path) ──\n");
        gThreaded = true;
        const Reading t = run(3000000.0, 200000.0, 0.0, locked, secs);
        gThreaded = false;
        const Reading i = run(3000000.0, 200000.0, 0.0, locked, secs);
        std::printf("   threaded: pilot %.3f raw %.3f peak %.3f   inline: pilot %.3f raw %.3f peak %.3f\n",
                    t.pilot, t.rdsRaw, t.rdsPk, i.pilot, i.rdsRaw, i.rdsPk);
        ok(near(t.pilot, i.pilot, 0.01) && near(t.rdsRaw, i.rdsRaw, 0.01) && near(t.rdsPk, i.rdsPk, 0.02),
           "the worker thread reads what the inline path reads (1 %)");
    }
    // ★★ AND WHEN IT CANNOT KEEP UP IT DROPS — THE LIVE POLICY. Every hole is a phase step in the
    //    IQ, a discriminator spike a peak meter would publish as the station's deviation. The feeder
    //    is paced at ~2x the instrument's own speed (measured first, inline), so it has to drop a
    //    large share of the blocks; what must survive is that nothing READS WRONG. ★ At that rate
    //    most deviation windows straddle a hole and are thrown away — no MPX peak at all is an
    //    acceptable answer; a WRONG one is not.
    {
        std::printf("\n── dropping (fed faster than it can measure — the live server's policy) ──\n");
        const double fs = 2048000.0;
        Sig s; Gen g(s, fs); g.offset = 0.0;
        std::vector<cf32> iq;
        g.fill(iq, (int)(fs * 9.0));                         // generated up front: the feeder is only a copy
        const double truePk = g.mpxPeak * 75.0;
        const int blk = (int)(fs / 100.0);
        double perBlock = 0.0;
        {
            vibedsp::MpxMeasure m; m.setThreaded(false); m.configure(fs);
            const double c0 = cpuNow();
            for (int o = 0; o + blk <= (int)(fs * 1.0); o += blk) m.feed(iq.data() + o, blk, 1);
            perBlock = (cpuNow() - c0) / 100.0;
        }
        vibedsp::MpxMeasure m; m.setThreaded(true); m.setBlocking(false); m.configure(fs);
        for (int o = 0; o + blk <= (int)iq.size(); o += blk) {
            m.feed(iq.data() + o, blk, 1);
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((long)(perBlock * 0.5e6));
            while (std::chrono::steady_clock::now() < until) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        vibedsp::MpxMeasure::Out o; int ew = 0; unsigned seq = 0;
        m.snapshot(o, nullptr, ew, nullptr, seq);
        const unsigned dropped = m.dropped();
        m.stop();
        std::printf("   %u of %d blocks dropped: pilot %.2f  RDS peak %.2f  MPX peak %.1f (true %.1f)\n",
                    dropped, (int)(iq.size() / blk), o.pilotKHz, o.rdsPeakKHz, o.mpxDevHoldKHz, truePk);
        ok(dropped > 0, "the test really did make it drop");
        ok(o.valid && near(o.pilotKHz, 6.75, 0.03), "pilot still measured, within 3 %");
        ok(o.mpxDevHoldKHz <= truePk * 1.05,
           "no dropped block published as a deviation peak (MPX peak <= truth + 5 %, or none)");
    }
    // ★★★ AN SDRplay FEEDS ~1000-SAMPLE BLOCKS, IN BURSTS — AND THAT MUST NOT LOOK LIKE FALLING BEHIND.
    //     The Lenovo's RSP1A (2026-10-01): ~2900 callbacks a second of ~1030 samples, the DSP thread
    //     running ~90 of them back to back ~30 times a second. With the queue counted in BLOCKS that
    //     was 2 ms of slack, ~700 drops a second, a hold after every one — and an Advanced RDS panel
    //     that never published a figure on an idle i5, while every RTL (16k-sample blocks) was fine.
    //     Fed here exactly so, in real time, on the production (dropping) path: nothing may drop.
    {
        std::printf("\n── an SDRplay's small blocks, in the DSP thread's bursts (real time) ──\n");
        const double fs = 3000000.0;
        Sig s; Gen g(s, fs); g.offset = 0.0;
        std::vector<cf32> iq;
        g.fill(iq, (int)(fs * 4.0));
        const int blk = 1030, burst = 90;
        vibedsp::MpxMeasure m; m.setThreaded(true); m.setBlocking(false); m.configure(fs);
        const auto t0 = std::chrono::steady_clock::now();
        int nb = 0;
        for (int o = 0; o + blk <= (int)iq.size(); o += blk) {
            m.feed(iq.data() + o, blk, 1);
            if (++nb % burst == 0)
                std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)((o + blk) / fs * 1e6)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        vibedsp::MpxMeasure::Out o; int ew = 0; unsigned seq = 0;
        m.snapshot(o, nullptr, ew, nullptr, seq);
        const unsigned dropped = m.dropped(), holes = m.gapsSeen();
        m.stop();
        std::printf("   %u of %d blocks dropped, %u holes: pilot %.2f  RDS raw %.2f\n",
                    dropped, nb, holes, o.pilotKHz, o.rdsRawKHz);
        ok(dropped == 0 && holes == 0, "small bursty blocks are taken whole — no drops, no holes");
        ok(o.valid && near(o.pilotKHz, 6.75, 0.03) && o.rdsRawKHz > 0.0f, "and the panel publishes its figures");
    }
    // ★★ THE SHARED DIAL. Each listener's pipeline is fed a channel cut from the shared FFT, and
    //    extract() rolls that channel's outer quarter off at each edge — so it is flat only to
    //    ±chanRate/4. Sized by the passband (chanBinsFor: 2.5x), ±100 kHz gets 512 kHz at 2.048 MS/s
    //    and 500 kHz at 8 (flat to ±125), ±75 kHz gets 375 kHz at 3 (flat to ±94). Measured here
    //    against a channel floored at 600 kHz, to decide whether the shim should floor it: it did
    //    not (see chanBinsFor's caller) — the difference is nil at ±100 and ~1 % at ±75 with 80 kHz
    //    of deviation (VIBE_TEST_HOT=1.5), and a floor would cost every WFM listener CPU with the
    //    panel shut. The as-sized channel is what is asserted.
    {
        std::printf("\n── shared dial: a listener's channel cut from the shared FFT ──\n");
        if (std::getenv("VIBE_TEST_HOT")) { locked.programme = std::atof(std::getenv("VIBE_TEST_HOT")); }
        const Reading direct = run(3000000.0, 200000.0, 0.0, locked, secs);
        struct Sd { double fs; int fft; double bw; };
        for (const Sd& d : { Sd{ 2048000.0, 16384, 200000.0 }, Sd{ 8000000.0, 32768, 200000.0 },
                             Sd{ 3000000.0, 16384, 150000.0 } }) {
            const double fs = d.fs; const int fft = d.fft;
            double oldRate = 0, newRate = 0;
            const Reading o = runChannelised(fs, fft, d.bw * 2.5, d.bw, locked, secs, &oldRate);
            const Reading n = runChannelised(fs, fft, 4.0 * 150000.0, d.bw, locked, secs, &newRate);
            std::printf("   %.3f MS/s ±%.0fk  channel %4.0f kHz (as the server sizes it): pilot %.2f raw %.2f peak %.2f MPX %.1f\n",
                        fs / 1e6, d.bw / 2e3, oldRate / 1e3, o.pilot, o.rdsRaw, o.rdsPk, o.mpxHold);
            std::printf("   %.3f MS/s ±%.0fk  channel %4.0f kHz (floored, for scale): pilot %.2f raw %.2f peak %.2f MPX %.1f\n",
                        fs / 1e6, d.bw / 2e3, newRate / 1e3, n.pilot, n.rdsRaw, n.rdsPk, n.mpxHold);
            char w[160];
            std::snprintf(w, sizeof w, "%.3f MS/s ±%.0fk shared dial: the channel as the server sizes it reads what the "
                          "direct path reads (pilot 1 %%, raw 1.5 %%, peak 2 %%)", fs / 1e6, d.bw / 2e3);
            ok(near(o.pilot, direct.pilot, 0.01) && near(o.rdsRaw, direct.rdsRaw, 0.015)
               && near(o.rdsPk, direct.rdsPk, 0.02) && newRate >= 600000.0, w);
        }
    }

    // ★★★ NOISE IS NOT MULTIPATH — the central assertion of test-multipath-meter, re-made on THIS
    //     path with its own noise table (kMpSnr/kMpDepth): a noisy signal with no reflection must not
    //     read as one, and a real reflection must.
    {
        std::printf("\n── multipath on the measurement path ──\n");
        Sig noisy; noisy.noise = 0.4;                 // ~24 dB MPX S/N, no echo
        Sig echo; echo.echoAmp = 0.5;                  // a -6 dB reflection, 3 us late, clean
        const Reading n = run(3000000.0, 200000.0, 0.0, noisy, secs);
        const Reading e = run(3000000.0, 200000.0, 0.0, echo, secs);
        std::printf("   noise only: S/N %.1f dB, multipath %.1f %% (%s)   -6 dB echo: multipath %.1f %% (%s)\n",
                    n.snr, 100.0f * n.mp, n.mpOk ? "valid" : "not measurable", 100.0f * e.mp, e.mpOk ? "valid" : "not measurable");
        ok(n.mp < 0.03f, "noise with no reflection does not read as multipath (< 3 %)");
        ok(e.mpOk && e.mp > 0.05f, "a -6 dB reflection reads as multipath (> 5 %)");
    }
#endif

    std::printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
