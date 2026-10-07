// ★★★ EVERY CHAIN DELIVERS EXACTLY 48 000 AUDIO SAMPLES PER SECOND OF CAPTURE (2026-10-07).
//
// The fault: the demod chain's audio rate is sampleRate / decimation, rarely a whole number —
// 3 MS/s / 296 = 10135.135 Hz (the Lenovo RSP1A in LSB), a per-listener 31.25 kHz channel / 3 =
// 10416.667 Hz (the Pi 500 RSP1B in USB). The resampler was built from llround() of it and then
// capped at L <= 256 by a rational approximation, which picked 251/53: an output rate 31.9 ppm SLOW.
// Measured on the live Lenovo, −31.7 ppm against the Mac's clock, every listener's stream running
// ~1.9 ms a minute short until the playout buffer ran dry and stepped. The code said the error was
// "far below anything audible, well inside what the jitter buffer absorbs" — true of the pitch,
// false of the stream: a jitter buffer cannot absorb a RATE error, it drains at it.
//
// ★ THREE RULES:
//   1. THE RESAMPLER COUNTS EXACTLY. Fed any exact input rate (as the pipeline computes it), in odd
//      block sizes, its output count tracks in·L/M to within two samples over ten minutes of audio
//      — no drift at all, whatever L is, including L far past the 512-branch table.
//   2. QUANTISING THE PHASE COSTS LITTLE. Where the exact L exceeds the table, a 1 kHz tone through
//      10135.135 -> 48000 stays a clean 1 kHz tone (residual after a sine fit better than −55 dB).
//      And where L fits, the exact constructor is BIT-IDENTICAL to the integer one.
//   3. THE WHOLE CHAIN, EVERY MODE, BOTH HOST PATHS. RxPipeline at the shared capture rates
//      (RSP 2/3/6/8/10 MS/s, RTL 2.048/2.4, HF+ 912 k) and at the per-listener channel rates the shim
//      cuts (sampleRate·bins/fftSize): the lag of audio behind 48 000/s, averaged over 5 s, moves by
//      at most 1.5 samples in 15 s (~2 ppm) — against 31-95 ppm (23-70 samples) from the old resampler.
// ★ SILENT: audio is counted, never played. No device is touched.
#include "vibedsp.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::RationalResampler;
using vibedsp::cf32;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// ── 1 + 2: the resampler alone ────────────────────────────────────────────────────────────────
static void countExact(double inRate, const char* label) {
    RationalResampler rs(inRate, 48000, RationalResampler::ExactRate{});
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> blk(1, 4097);
    std::vector<float> in(4097, 0.25f), out(rs.maxOut(4097));
    long long nin = 0, nout = 0;
    const long long total = (long long)(inRate * 600.0);       // ten minutes of input
    double worst = 0.0;
    while (nin < total) {
        const int n = blk(rng);
        nout += rs.process(in.data(), n, out.data());
        nin += n;
        const double expect = (double)nin * (double)rs.L() / (double)rs.M();
        worst = std::max(worst, std::fabs((double)nout - expect));
    }
    const double ppm = ((double)nout / ((double)nin * 48000.0 / inRate) - 1.0) * 1e6;
    char b[256];
    std::snprintf(b, sizeof b, "%-34s in %.6f Hz: L/M %lld/%lld, %d branches, worst lag %.1f samples, %+.4f ppm",
                  label, inRate, rs.L(), rs.M(), rs.branches(), worst, ppm);
    check(worst <= 2.0 + rs.maxOut(1) && std::fabs(ppm) < 0.05, b);
}

static void toneQuality() {
    const double inRate = 3000000.0 / 296.0;                     // the Lenovo's LSB chain
    RationalResampler rs(inRate, 48000, RationalResampler::ExactRate{});
    const int N = (int)(inRate * 4.0);
    std::vector<float> in(N), out(rs.maxOut(N));
    for (int i = 0; i < N; ++i) in[i] = (float)std::sin(2.0 * M_PI * 1000.0 * i / inRate);
    const int no = rs.process(in.data(), N, out.data());
    // Least-squares fit of a 1 kHz sine (and cosine) over the settled middle, then the residual.
    const int a = no / 4, z = no - no / 4;
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (int i = a; i < z; ++i) {
        const double s = std::sin(2.0 * M_PI * 1000.0 * i / 48000.0), c = std::cos(2.0 * M_PI * 1000.0 * i / 48000.0);
        ss += s * s; cc += c * c; sc += s * c; ys += out[i] * s; yc += out[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;
    double sig = 0, res = 0;
    for (int i = a; i < z; ++i) {
        const double s = std::sin(2.0 * M_PI * 1000.0 * i / 48000.0), c = std::cos(2.0 * M_PI * 1000.0 * i / 48000.0);
        const double fit = A * s + B * c;
        sig += fit * fit; res += (out[i] - fit) * (out[i] - fit);
    }
    const double db = 10.0 * std::log10(res / sig);
    char b[200];
    std::snprintf(b, sizeof b, "1 kHz through 10135.135 -> 48000 (%d of %lld branches): residual %.1f dB, amplitude %.3f",
                  rs.branches(), rs.L(), db, std::sqrt(A * A + B * B));
    check(db < -55.0 && std::fabs(std::sqrt(A * A + B * B) - 1.0) < 0.02, b);
}

static void identicalWhenItFits() {
    RationalResampler ri(44100, 48000), re(44100.0, 48000, RationalResampler::ExactRate{});
    const int N = 44100;
    std::vector<float> in(N), o1(ri.maxOut(N)), o2(re.maxOut(N));
    for (int i = 0; i < N; ++i) in[i] = (float)std::sin(0.01 * i) * 0.5f;
    int n1 = 0, n2 = 0;
    for (int off = 0; off < N; off += 1000) {
        const int n = std::min(1000, N - off);
        n1 += ri.process(in.data() + off, n, o1.data() + n1);
        n2 += re.process(in.data() + off, n, o2.data() + n2);
    }
    bool same = n1 == n2 && ri.L() == re.L() && ri.M() == re.M();
    for (int i = 0; same && i < n1; ++i) same = o1[i] == o2[i];
    check(same, "44100 -> 48000 (L 160 fits): the exact constructor is bit-identical to the integer one");
}

// ── 3: the whole chain ────────────────────────────────────────────────────────────────────────
static long long g_out = 0;
static void onAud(void*, const float*, int n, int, int) { g_out += n; }
static void onSpec(void*, const float*, int) {}

static int fftSizeForRate(double rate) {      // the shim's own rule (local_sdr_shim.cpp)
    double want = rate / 75.0; int s = 4096;
    while (s < (int)want && s < 32768) s *= 2;
    return s;
}
static double chanRateFor(double fs, double bw) {   // chanBinsFor(): a power-of-two slice
    const int fft = fftSizeForRate(fs);
    const double need = std::max(bw * 2.5, 24000.0);
    int b = 64;
    while (b < fft && fs * b / fft < need) b <<= 1;
    return fs * std::min(b, fft) / fft;
}

static void chain(double fs, RxPipeline::Mode mode, double bw, const char* what) {
    RxPipeline pipe; RxPipeline::Callbacks cb{}; cb.spectrum = onSpec; cb.audio = onAud;
    pipe.start(fs, 1024, 5.0, 48000, cb);
    pipe.setTune(fs > 200000.0 ? 10000.0 : 0.0, mode, bw);
    const int blk = 16384;
    std::vector<cf32> iq(blk);
    std::mt19937 r(1); std::normal_distribution<float> nd(0, 0.01f);
    for (auto& v : iq) v = cf32(nd(r), nd(r));
    g_out = 0;
    /* ★ DRIFT, NOT A SNAPSHOT. The chain emits audio in bursts (its FIR stages and the Weaver pair
     *  work in blocks), so "outputs so far" against "inputs so far" jitters by a few samples at any one
     *  instant whatever the rate. So the lag (outputs − inputs × 48000 / fs) is AVERAGED over every
     *  feed in a 5 s window early on and again 15 s later; a correct rate leaves the two equal, the
     *  old −31.9 ppm moved them 23 samples apart. */
    auto window = [&](long long& in, double secs) {
        double sum = 0; long long n = 0;
        const long long stop = in + (long long)(fs * secs);
        while (in < stop) {
            pipe.feed(iq.data(), blk); in += blk;
            sum += (double)g_out - (double)in * 48000.0 / fs; ++n;
        }
        return sum / (double)n;
    };
    long long in = 0;
    window(in, 1.0);                                   // warm up
    const double lagA = window(in, 5.0);
    const long long inA = in;
    window(in, 10.0);
    const double lagB = window(in, 5.0);
    const double drift = lagB - lagA, span = (double)(in - inA) / fs;
    char b[200];
    std::snprintf(b, sizeof b, "%-44s fs %10.3f  chFs %10.4f: drift %+.2f samples over %.0f s (%+.2f ppm)",
                  what, fs, pipe.chainShape().chFs, drift, span, drift / (span * 48000.0) * 1e6);
    check(std::fabs(drift) <= 1.5, b);
}

int main() {
    std::printf("── 1. the resampler counts exactly, whatever L is ──\n");
    countExact(3000000.0 / 296.0,     "Lenovo RSP1A LSB (3 MS/s / 296)");
    countExact(8000000.0 / 774.0,     "RSP 8 MS/s USB direct (/ 774)");
    countExact(31250.0 / 3.0,         "Pi 500 RSP1B per-VFO USB (31.25k / 3)");
    countExact(46875.0 / 4.0,         "3 MS/s per-VFO USB (46.875k / 4)");
    countExact(2048000.0 / 6.0 / 5.0, "RTL 2.048 WFM audio (/ 6 / 5)");
    countExact(4687.5 * 7.0 / 3.0,    "a non-integer channel (4687.5·7/3)");
    countExact(912000.0 / 76.0,       "HF+ 912 k AM");
    countExact(44100.0,               "44.1 k (small L)");

    std::printf("── 2. phase quantisation is clean; an L that fits is unchanged ──\n");
    toneQuality();
    identicalWhenItFits();

    std::printf("── 3. the whole chain, shared and per-listener ──\n");
    using M = RxPipeline::Mode;
    // The two configurations measured on air, first.
    chain(3000000.0, M::SSB_LSB, 2700.0, "Lenovo RSP1A, shared, LSB 2.7k");
    chain(chanRateFor(8000000.0, 2700.0), M::SSB_USB, 2700.0, "Pi 500 RSP1B, per-VFO channel, USB 2.7k");
    const double rates[] = { 2000000.0, 3000000.0, 6000000.0, 8000000.0, 10000000.0, 2048000.0, 2400000.0, 912000.0 };
    struct Md { M m; double bw; const char* n; } modes[] = {
        { M::AM, 10000.0, "AM 10k" }, { M::SSB_USB, 2700.0, "USB 2.7k" }, { M::CW, 500.0, "CW 500" },
        { M::NFM, 12500.0, "NFM 12.5k" }, { M::WFM, 200000.0, "WFM 200k" } };
    for (double fs : rates) for (const auto& md : modes) {
        char w[80];
        std::snprintf(w, sizeof w, "shared %.3f MS/s %s", fs / 1e6, md.n);
        chain(fs, md.m, md.bw, w);
        const double cr = chanRateFor(fs, md.bw);
        if (cr < fs) {
            std::snprintf(w, sizeof w, "per-VFO (%.3f MS/s) %s", fs / 1e6, md.n);
            chain(cr, md.m, md.bw, w);
        }
    }
    std::printf("   %s — %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
