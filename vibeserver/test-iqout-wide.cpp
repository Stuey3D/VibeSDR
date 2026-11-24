// test-iqout-wide — RAW IQ OUT's own wide tap (docs/v12/IQOUT-WIDE.md), through the real code:
//   RxPipeline::setRawIqRate (direct mode: IqOutDecimator off the baseband, on the DSP thread),
//   Channelizer::extract + IqOutDecimator (per-listener mode: a slice of its own off the shared FFT, as iqTapClient),
//   and IqOutResampler (the writer thread's sharp filter + exact rate, as iqConvert).
//
// For every rate the server offers (48 / 96 / 192 / 250 kHz) on 2.4 and 2.048 MS/s captures, USB and NFM listeners:
//   1. in-band tones (±0.10 … +0.445 × rate) come out at the right frequency and level (± 0.1 dB of what went in);
//   2. out-of-band tones (beyond ±0.55 × rate) are ≥ 60 dB down wherever they could land;
//   3. the sample count is EXACTLY the rate — the output over the second 10 s of a 20 s run is rate × 10 (± 1);
//   4. the listener's audio is bit-identical with IQ out on and off (and switched on and off mid-stream).
// Then the cost: CPU per second of signal, old way (the channel raised to the rate, the passband out) against new.
//
// ★ Silent: no audio leaves this process.
#include "vibedsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using vibedsp::cf32;
using vibedsp::RxPipeline;
using M = RxPipeline::Mode;

static int failures = 0;
static void ok(bool cond, const std::string& what) {
    if (!cond) { std::printf("   FAIL %s\n", what.c_str()); failures++; }
}

namespace {
double threadCpu() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

struct Tone { double hz; double amp; };   // hz relative to the DIAL

/** `secs` of capture at `fs`: the dial at `dialOff` from the centre, a sum of complex tones around it, and a little noise. */
std::vector<cf32> makeCapture(double fs, double secs, double dialOff, const std::vector<Tone>& tones) {
    const size_t n = (size_t)(fs * secs);
    std::vector<cf32> x(n, cf32(0, 0));
    for (const Tone& t : tones) {
        const double w = 2.0 * M_PI * (dialOff + t.hz) / fs;
        // ★ a recursive phasor in double, renormalised — exact enough over 20 s and fast
        double cr = t.amp, ci = 0.0;
        const double rr = std::cos(w), ri = std::sin(w);
        for (size_t i = 0; i < n; ++i) {
            x[i] += cf32((float)cr, (float)ci);
            const double nr = cr * rr - ci * ri, ni = cr * ri + ci * rr;
            cr = nr; ci = ni;
            if ((i & 4095) == 4095) { const double m = t.amp / std::hypot(cr, ci); cr *= m; ci *= m; }
        }
    }
    uint32_t s = 12345;
    for (size_t i = 0; i < n; ++i) {   // ≈ -100 dBFS/Hz-ish floor: keeps the numbers honest, far below the 60 dB checks
        s = s * 1664525u + 1013904223u; const float a = ((s >> 9) / 8388608.0f - 0.5f) * 2e-5f;
        s = s * 1664525u + 1013904223u; const float b = ((s >> 9) / 8388608.0f - 0.5f) * 2e-5f;
        x[i] += cf32(a, b);
    }
    return x;
}

/** Amplitude of the complex tone at `hz` in `x` (rate `fs`), Blackman-Harris window — sidelobes < -92 dB. */
double toneAmp(const std::vector<float>& xi, const std::vector<float>& xq, size_t from, size_t n, double fs, double hz) {
    double sr = 0, si = 0, sw = 0;
    const double w = -2.0 * M_PI * hz / fs;
    for (size_t k = 0; k < n; ++k) {
        const double t = 2.0 * M_PI * (double)k / (double)(n - 1);
        const double win = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        const double c = std::cos(w * (double)(from + k)), s = std::sin(w * (double)(from + k));
        const double a = xi[from + k], b = xq[from + k];
        sr += win * (a * c - b * s); si += win * (a * s + b * c); sw += win;
    }
    return std::hypot(sr, si) / sw;
}
double dB(double a) { return 20.0 * std::log10(std::max(a, 1e-15)); }

/** Where an input tone at `hz` (relative to the dial) lands in an output at rate R. */
double aliasOf(double hz, double R) { double a = std::fmod(hz, R); if (a > R / 2) a -= R; if (a < -R / 2) a += R; return a; }

// ── the collector: what the tap hands the shim, run through the writer's IqOutResampler as iqConvert does ─────────
struct Collect {
    vibedsp::IqOutResampler rs;
    int R = 0;
    std::vector<float> oi, oq, bi, bq;      // all output so far; the last block
    long long tapSamples = 0; double tapRate = 0;
    double fsIn = 0, dialOff = 0;           // ★ where a tone really is after the CAPTURE wraps it (a narrow radio)
    double writerCpu = 0;
    std::vector<size_t> marks;              // output count at each mark()
    void push(const cf32* x, int n, double rate) {
        tapSamples += n; tapRate = rate;
        const double c0 = threadCpu();
        rs.configure(rate, R);
        const int m = rs.process(x, n, bi, bq);
        writerCpu += threadCpu() - c0;
        oi.insert(oi.end(), bi.begin(), bi.begin() + m); oq.insert(oq.end(), bq.begin(), bq.begin() + m);
    }
};
void rawCb(void* ctx, const cf32* x, int n, double rate) { static_cast<Collect*>(ctx)->push(x, n, rate); }

struct AudioCap { std::vector<float> pcm; };
void audioCb(void* ctx, const float* pcm, int frames, int ch, int) {
    auto* a = static_cast<AudioCap*>(ctx);
    a->pcm.insert(a->pcm.end(), pcm, pcm + (size_t)frames * ch);
}
struct Both { Collect* c; AudioCap* a; };
void bothAudio(void* ctx, const float* pcm, int frames, int ch, int r) { audioCb(static_cast<Both*>(ctx)->a, pcm, frames, ch, r); }
void bothRaw(void* ctx, const cf32* x, int n, double rate) { rawCb(static_cast<Both*>(ctx)->c, x, n, rate); }
void bothOldIq(void* ctx, const cf32* x, int n, double rate) { rawCb(static_cast<Both*>(ctx)->c, x, n, rate); }

const std::vector<Tone> kIn  = { { 0.400, 0.05 }, { -0.300, 0.05 }, { 0.445, 0.05 }, { -0.100, 0.05 } };   // × R
const std::vector<Tone> kOut = { { 0.620, 0.05 }, { -0.750, 0.05 }, { 1.300, 0.05 }, { -0.580, 0.05 } };   // × R

std::vector<Tone> tonesFor(double R) {
    std::vector<Tone> t;
    for (auto v : kIn)  t.push_back({ v.hz * R, v.amp });
    for (auto v : kOut) t.push_back({ v.hz * R, v.amp });
    return t;
}

/** Checks 1–2 on a collected stream: the last `secs` seconds (the first second skipped as settling). */
void checkTones(const Collect& c, double R, const std::string& tag) {
    const size_t n = (size_t)(R * 2.0);
    if (c.oi.size() < n + (size_t)R) { ok(false, tag + ": too little output (" + std::to_string(c.oi.size()) + ")"); return; }
    const size_t from = c.oi.size() - n;
    double worstIn = 0, worstOut = -999; std::string wi, wo;
    for (auto v : kIn) {
        const double a = toneAmp(c.oi, c.oq, from, n, R, v.hz * R);
        const double err = dB(a) - dB(v.amp);
        if (std::fabs(err) > std::fabs(worstIn)) { worstIn = err; char b[64]; std::snprintf(b, sizeof b, "%+.3fR", v.hz); wi = b; }
    }
    for (auto v : kOut) {
        double abs = std::fmod(c.dialOff + v.hz * R, c.fsIn); if (abs > c.fsIn / 2) abs -= c.fsIn; if (abs < -c.fsIn / 2) abs += c.fsIn;
        const double at = aliasOf(abs - c.dialOff, R);
        const double a = toneAmp(c.oi, c.oq, from, n, R, at);
        const double rel = dB(a) - dB(v.amp);
        if (rel > worstOut) { worstOut = rel; char b[64]; std::snprintf(b, sizeof b, "%+.2fR→%+.2fR", v.hz, at / R); wo = b; }
    }
    std::printf("   %-34s in-band worst %+6.3f dB (%s)   out-of-band worst %6.1f dB (%s)\n", tag.c_str(), worstIn, wi.c_str(), worstOut, wo.c_str());
    ok(std::fabs(worstIn) <= 0.1, tag + ": in-band tone off by " + std::to_string(worstIn) + " dB at " + wi);
    ok(worstOut <= -60.0, tag + ": out-of-band tone only " + std::to_string(worstOut) + " dB down (" + wo + ")");
}

// ── direct mode: RxPipeline::setRawIqRate ─────────────────────────────────────────────────────────────────────────
void directCase(double fs, M mode, double bw, const char* mname, int R) {
    const double dialOff = 0.15 * fs;
    const auto x = makeCapture(fs, 20.0, dialOff, tonesFor(R));
    Collect c; c.R = R; c.fsIn = fs; c.dialOff = dialOff;
    AudioCap a;
    Both both{ &c, &a };
    RxPipeline rx;
    RxPipeline::Callbacks cb{};
    cb.ctx = &both; cb.audio = &bothAudio; cb.rawIq = &bothRaw;
    rx.start(fs, 1024, 10.0, 48000, cb);
    rx.setTune(dialOff, mode, bw);
    rx.setRawIqRate(R);
    const int blk = (int)(fs / 100.0);
    size_t at10 = 0;
    for (size_t o = 0; o + (size_t)blk <= x.size(); o += (size_t)blk) {
        rx.feed(x.data() + o, blk);
        if (o + (size_t)blk == (size_t)(fs * 10.0)) at10 = c.oi.size();
    }
    const size_t at20 = c.oi.size();
    char tag[96]; std::snprintf(tag, sizeof tag, "direct %.3f MS/s %s %d", fs / 1e6, mname, R);
    checkTones(c, R, tag);
    // 3. continuity: the second 10 s is exactly R × 10 samples (the filters' delay is in the first 10 s)
    const long long second = (long long)at20 - (long long)at10;
    ok(std::llabs(second - (long long)R * 10) <= 1, std::string(tag) + ": second 10 s held " + std::to_string(second) + " samples, want " + std::to_string(R * 10));
    std::printf("   %-34s tap %s → %.1f Hz, writer %d taps/output; 10 s → %lld samples (want %d)\n", "", rx.rawIqTapPlan().c_str(),
                rx.rawIqTapRate(), c.rs.tapsPerOutput(), second, R * 10);
    rx.stop();
}

// ── per-listener mode: Channelizer slice + IqOutDecimator (as iqTapClient) ──────────────────────────────────────────
void perListenerCase(double fs, int fftSize, int R) {
    const double binHz = fs / fftSize;
    const double dialOff = 0.2 * fs + 0.37 * binHz;          // ★ off a whole bin, so the residual shift is exercised
    const auto x = makeCapture(fs, 20.0, dialOff, tonesFor(R));
    vibedsp::Channelizer ch(fftSize);
    vibedsp::Channelizer::ExtractCtx ctx;
    // the shim's iqSliceBinsFor
    int b = 64; while (b < fftSize && fs * b / fftSize < 1.2 * R) b <<= 1; b = std::min(b, fftSize);
    const double sr = fs * b / fftSize;
    const int centreBin = (int)std::lround(dialOff / binHz);
    vibedsp::IqOutDecimator dec; dec.configure(sr, R);
    dec.setShiftHz(dialOff - centreBin * binHz);
    std::vector<cf32> slice((size_t)b), out;
    Collect c; c.R = R; c.fsIn = fs; c.dialOff = dialOff;
    const int blk = (int)(fs / 100.0);
    size_t at10 = 0;
    long long consumed = 0;                                   // input samples the channelizer has turned into blocks
    double tapCpu = 0;                                        // the listener thread's share: extract + decimate (+ the copy out)
    for (size_t o = 0; o + (size_t)blk <= x.size(); o += (size_t)blk) {
        ch.feed(x.data() + o, blk, [&](const cf32* bins, int) {
            const double t0 = threadCpu();
            struct Acc { double& a; double t0; ~Acc() { a += threadCpu() - t0; } } acc{ tapCpu, t0 };
            const int got = ch.extract(bins, centreBin, b, slice.data(), ctx, ch.blockIndex(), 16);   // ★ the shim's kIqTaperDiv
            consumed += ch.hop();
            if (got <= 0) return;
            out.resize((size_t)dec.maxOut(got));
            const int m = dec.process(slice.data(), got, out.data());
            if (m > 0) c.push(out.data(), m, dec.outRate());
        });
        if (o + (size_t)blk == (size_t)(fs * 10.0)) at10 = c.oi.size();
    }
    const size_t at20 = c.oi.size();
    char tag[96]; std::snprintf(tag, sizeof tag, "per-listener %.3f MS/s/%d %d", fs / 1e6, fftSize, R);
    checkTones(c, R, tag);
    // ★ the channelizer delivers whole blocks, so a 10 s mark falls mid-block (one block of slack there); the RATE is checked
    //   over the whole run instead: every input sample the channelizer consumed is R/fs output samples, less a constant
    //   filter delay (a few ms) — any drift or lost block over 20 s would show far beyond that
    const long long second = (long long)at20 - (long long)at10;
    const double hop = (double)(fftSize - fftSize / 4) * (double)b / fftSize * R / sr;
    ok(std::fabs((double)second - R * 10.0) <= hop + 2, std::string(tag) + ": second 10 s held " + std::to_string(second));
    const double expect = (double)consumed * R / fs, short_ = expect - (double)at20;
    ok(short_ >= 0.0 && short_ <= 0.01 * R, std::string(tag) + ": " + std::to_string(at20) + " samples for " + std::to_string(expect) + " expected");
    std::printf("   %-34s slice %d bins = %.0f Hz → %s → %.1f Hz, writer %d taps; 20 s → %zu samples, %.1f short of rate × input (filter delay %.2f ms)\n", "", b, sr,
                dec.plan().c_str(), dec.outRate(), c.rs.tapsPerOutput(), at20, short_, short_ / R * 1e3);
    std::printf("   %-34s listener thread %.2f %% of a Mac core (incl. the writer's %.2f %%, run inline here)\n", "",
                tapCpu / 20.0 * 100.0, c.writerCpu / 20.0 * 100.0);
}

// ── 4. the listener's audio, IQ out on vs off ───────────────────────────────────────────────────────────────────────
void identicalCase(double fs, M mode, double bw, const char* mname) {
    const double dialOff = 0.1 * fs;
    std::vector<Tone> t = { { 700.0, 0.2 }, { 1900.0, 0.1 }, { 40000.0, 0.3 }, { -90000.0, 0.3 } };
    const auto x = makeCapture(fs, 4.0, dialOff, t);
    auto run = [&](int pattern) {
        Collect c; c.R = 250000;
        AudioCap a;
        Both both{ &c, &a };
        RxPipeline rx;
        RxPipeline::Callbacks cb{};
        cb.ctx = &both; cb.audio = &bothAudio; cb.rawIq = &bothRaw;
        rx.start(fs, 1024, 10.0, 48000, cb);
        rx.setTune(dialOff, mode, bw);
        const int blk = (int)(fs / 100.0);
        int k = 0;
        for (size_t o = 0; o + (size_t)blk <= x.size(); o += (size_t)blk, ++k) {
            if (pattern == 1 && k == 0) rx.setRawIqRate(250000);
            if (pattern == 2) rx.setRawIqRate((k / 37) % 2 ? 192000 : ((k / 53) % 2 ? 0 : 48000));   // on, off, other rates
            rx.feed(x.data() + o, blk);
        }
        rx.stop();
        return a.pcm;
    };
    const auto off = run(0), on = run(1), toggled = run(2);
    const bool same1 = off.size() == on.size() && std::memcmp(off.data(), on.data(), off.size() * sizeof(float)) == 0;
    const bool same2 = off.size() == toggled.size() && std::memcmp(off.data(), toggled.data(), off.size() * sizeof(float)) == 0;
    std::printf("   %s @ %.3f MS/s: %zu audio samples — IQ out on %s, toggled %s\n", mname, fs / 1e6, off.size(),
                same1 ? "bit-identical" : "DIFFERENT", same2 ? "bit-identical" : "DIFFERENT");
    ok(!off.empty(), std::string(mname) + ": no audio at all");
    ok(same1, std::string(mname) + ": the listener's audio changed with IQ out on");
    ok(same2, std::string(mname) + ": the listener's audio changed with IQ out switched on and off");
}

// ── the cost: before (the channel raised to the rate, the passband out) and after ──────────────────────────────────
void costCase(double fs, M mode, double bw, const char* mname, int R) {
    const double dialOff = 0.15 * fs;
    const auto x = makeCapture(fs, 4.0, dialOff, tonesFor(R));
    const int blk = (int)(fs / 100.0);
    auto time = [&](int how, Collect& c) {   // 0 = IQ off, 1 = old (setIqMinRate + channel IQ), 2 = new (setRawIqRate)
        AudioCap a; Both both{ &c, &a };
        RxPipeline rx;
        RxPipeline::Callbacks cb{};
        cb.ctx = &both; cb.audio = &bothAudio;
        if (how == 1) cb.iq = &bothOldIq;
        if (how == 2) cb.rawIq = &bothRaw;
        rx.start(fs, 1024, 10.0, 48000, cb);
        rx.setTune(dialOff, mode, bw);
        if (how == 1) rx.setIqMinRate(R);
        if (how == 2) rx.setRawIqRate(R);
        for (size_t o = 0; o + (size_t)blk <= x.size() / 4; o += (size_t)blk) rx.feed(x.data() + o, blk);   // settle
        double best = 1e9, bestW = 0;
        for (int r = 0; r < 3; ++r) {
            const double w0 = c.writerCpu, c0 = threadCpu();
            for (size_t o = 0; o + (size_t)blk <= x.size(); o += (size_t)blk) rx.feed(x.data() + o, blk);
            const double w = c.writerCpu - w0, t = threadCpu() - c0 - w;   // the DSP side alone
            if (t < best) { best = t; bestW = w; }
        }
        rx.stop();
        const double secs = (double)x.size() / fs;
        return std::make_pair(best / secs * 100.0, bestW / secs * 100.0);
    };
    Collect c0; c0.R = R; Collect c1; c1.R = R; Collect c2; c2.R = R;
    const auto off = time(0, c0), old = time(1, c1), neu = time(2, c2);
    // what the OLD stream carried: the tone at +0.40 R against the one at -0.10 R
    const size_t n = (size_t)R;
    auto rel = [&](const Collect& c, double f) { const size_t from = c.oi.size() - n; return dB(toneAmp(c.oi, c.oq, from, n, R, f * R)) - dB(0.05); };
    std::printf("   %-4s %3d kHz @ %.3f MS/s   DSP thread %% of a Mac core: off %.2f | old %.2f | new %.2f   writer: old %.2f | new %.2f"
                "   tone +0.40R: old %+.1f dB, new %+.2f dB\n",
                mname, R / 1000, fs / 1e6, off.first, old.first, neu.first, old.second, neu.second, rel(c1, 0.40), rel(c2, 0.40));
    ok(neu.first < old.first, std::string(mname) + ": the new tap costs the DSP thread more than the old way");
}
}  // namespace

int main() {
    const int rates[] = { 48000, 96000, 192000, 250000 };
    std::printf("RAW IQ OUT — its own wide tap (docs/v12/IQOUT-WIDE.md)\n\n");

    std::printf("── direct mode (RxPipeline::setRawIqRate + IqOutResampler) ──\n");
    for (double fs : { 2400000.0, 2048000.0 })
        for (int R : rates) {
            directCase(fs, M::SSB_USB, 2700, "USB", R);
            if (R == 250000 || R == 48000) directCase(fs, M::NFM, 12500, "NFM", R);
        }
    // an odd capture rate — Airspy HF+ 768 kHz and a wide one (RSP 8 MS/s)
    directCase(768000.0, M::AM, 10000, "AM", 250000);
    directCase(768000.0, M::AM, 10000, "AM", 192000);
    directCase(8000000.0, M::WFM, 200000, "WFM", 250000);

    std::printf("\n── per-listener mode (shared FFT slice + IqOutDecimator + IqOutResampler) ──\n");
    for (int R : rates) perListenerCase(2048000.0, 16384, R);
    perListenerCase(2400000.0, 32768, 250000);
    perListenerCase(8000000.0, 32768, 250000);

    std::printf("\n── the listener's audio with IQ out on / off ──\n");
    identicalCase(2400000.0, M::SSB_USB, 2700, "USB");
    identicalCase(2400000.0, M::NFM, 12500, "NFM");
    identicalCase(2048000.0, M::WFM, 200000, "WFM");

    std::printf("\n── cost (best of 3, thread CPU per second of signal) ──\n");
    costCase(2400000.0, M::SSB_USB, 2700, "USB", 250000);
    costCase(2400000.0, M::NFM, 12500, "NFM", 250000);
    costCase(2400000.0, M::SSB_USB, 2700, "USB", 48000);
    costCase(2048000.0, M::NFM, 12500, "NFM", 192000);

    std::printf("\n── plans (stages on the sample owner's thread; writer taps per output and rail) ──\n");
    for (double fs : { 240000.0, 768000.0, 912000.0, 1024000.0, 2048000.0, 2400000.0, 2560000.0, 3200000.0, 6000000.0, 8000000.0, 10000000.0 })
        for (int R : rates) {
            vibedsp::IqOutDecimator d; d.configure(fs, R);
            vibedsp::IqOutResampler r; r.configure(d.outRate(), R);
            std::printf("   %6.3f MS/s → %3d kHz: %-34s Fd %9.1f  %6.1f M MAC/s  writer %s\n", fs / 1e6, R / 1000, d.plan().c_str(),
                        d.outRate(), d.macsPerSec() / 1e6, r.passThrough() ? "pass" : (std::to_string(r.tapsPerOutput()) + " taps").c_str());
            ok(d.outRate() >= R * (1 - 1e-9) || fs < R, "Fd below the rate");
        }

    if (failures) { std::printf("\n%d FAILURE(S)\n", failures); return 1; }
    std::printf("\nall passed\n");
    return 0;
}
