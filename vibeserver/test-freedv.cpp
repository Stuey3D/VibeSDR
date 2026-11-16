// ★★ V12 FreeDV AS A MODE (vibe_freedv.h, docs/v12/FREEDV.md) — end to end on each library's OWN transmitter.
//
// RADE V1 (2026-11-16): FreeDV's rade_c transmitter (the tests' archive librade_vibe_tx.a, VIBE_HAVE_RADE_TX) makes the RADE
// overs — the synthetic voice, then the end-of-over frame carrying a callsign — and every section below runs RADE beside the
// codec2 sub-modes where both are here. RADE adds: the callsign as "last" once the over ends (never as the talker's text), AUTO
// choosing RADE or 700D as the dial changes between them, and no false RADE sync on noise or on a codec2 signal.
//
// A synthetic voice is encoded and modulated by codec2's freedv_tx / freedv_comptx (700D, 700E, 1600), noise is added at a
// chosen SNR (3 kHz noise bandwidth, FreeDV's own convention), and the signal goes through:
//   1. FreeDvListener alone, fed 48 kHz USB audio — each sub-mode chosen by hand, then AUTO finding each one;
//   2. the REAL RxPipeline in USB (channel filter, AGC, resampler) from complex IQ with the station off the dial centre and a
//      small frequency error — the server's own path, demodulator + modem host;
//   3. pure noise for 30 s in AUTO — never a lock, never a sound.
// Checks: sync, the sub-mode AUTO names, the others closed after the lock, speech blocks decoded, audio out (energy), the
// text channel ("VIBE TEST", shown only when it has come twice the same), the report's JSON, bounded queues, and a CPU row.
//
// ★ Exit 3 = NOT RUN when neither libcodec2 nor RADE is here; each half runs when its library is. Formerly: NOT RUN when libcodec2 is not installed (apt install libcodec2-1.2 / brew install codec2). VIBE_FREEDV_DISABLE=1
//   must make the library "not installed" — checked last, in a child process's environment.
#include "vibe_freedv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using vibe::freedv::FreeDvListener;
namespace fdv = vibe::freedv;
using cf = std::complex<float>;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("   %s %s%s%s\n", ok ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m", what,
                detail.empty() ? "" : " — ", detail.c_str());
    (ok ? g_pass : g_fail)++;
}

// ── a voice: a pitch-gliding harmonic series through two moving formants, in syllables ─────────────────────────────────
static std::vector<short> voice(double sec, unsigned seed) {
    const int fs = 8000, n = int(sec * fs);
    std::vector<short> v(static_cast<size_t>(n));
    std::mt19937 rng(seed);
    double ph = 0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / fs;
        const double f0 = 120 + 30 * std::sin(2 * M_PI * 0.7 * t);
        ph += 2 * M_PI * f0 / fs;
        const double f1 = 600 + 250 * std::sin(2 * M_PI * 1.3 * t), f2 = 1700 + 500 * std::sin(2 * M_PI * 0.9 * t + 1);
        double s = 0;
        for (int h = 1; h * f0 < 3500; ++h) {
            const double fh = h * f0;
            const double g = std::exp(-std::pow((fh - f1) / 200, 2)) + 0.6 * std::exp(-std::pow((fh - f2) / 300, 2)) + 0.05;
            s += g * std::sin(h * ph);
        }
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.5 * t));   // syllables with gaps
        v[size_t(i)] = short(std::lrint(std::max(-1.0, std::min(1.0, 0.25 * s * syl)) * 12000));
    }
    (void)rng;
    return v;
}

struct TxText { const char* msg; size_t at; };
static char txChar(void* st) {
    auto* t = static_cast<TxText*>(st);
    const char c = t->msg[t->at++];
    if (!t->msg[t->at]) t->at = 0;
    return c;
}

#if defined(VIBE_HAVE_RADE) && defined(VIBE_HAVE_RADE_TX)
static constexpr bool kRadeTx = true;
/** ★ A RADE V1 over at 8 kHz as its analytic baseband, from rade_c's own transmitter: `sec` of the voice (the same voice as
 *  the codec2 modes, at 16 kHz), then — when `call` is set — the end-of-over frame carrying it. Analytic rms 0.2, like
 *  modulate(). */
static std::vector<cf> radeModulate(double sec, const char* call) {
    std::vector<cf> out;
    vrade_tx* t = vrade_tx_open();
    if (!t) return out;
    const std::vector<short> v8 = voice(sec, 7);
    std::vector<float> f8(v8.size()), sp;
    for (size_t i = 0; i < v8.size(); ++i) f8[i] = float(v8[i]) / 32768.0f;
    vibedsp::RationalResampler up(8000, 16000);
    sp.resize(size_t(up.maxOut(int(f8.size()))));
    sp.resize(size_t(up.process(f8.data(), int(f8.size()), sp.data())));
    std::vector<float> re(size_t(vrade_tx_frame_samples()) * (2 + sp.size() / 1920)), im(re.size());
    const int n = vrade_tx_speech(t, sp.data(), int(sp.size()), re.data(), im.data());
    for (int i = 0; i < n; ++i) out.emplace_back(re[size_t(i)], im[size_t(i)]);
    if (call) {
        std::vector<float> er(static_cast<size_t>(vrade_tx_eoo_samples())), ei(er.size());
        const int ne = vrade_tx_eoo(t, call, er.data(), ei.data());
        for (int i = 0; i < ne; ++i) out.emplace_back(er[size_t(i)], ei[size_t(i)]);
        out.resize(out.size() + 8000, cf(0, 0));   // ★ then a second of the band (noise only): the receiver sees the end-of-over
    }                                              //   frame only once its window has moved past it
    vrade_tx_close(t);
    double p = 0;
    for (size_t i = 0; i < size_t(n); ++i) p += std::norm(out[i]);   // ★ the level of the speech frames (the EOO rides along)
    const float k = float(0.2 / std::sqrt(p / double(std::max(1, n))));
    for (auto& x : out) x *= k;
    return out;
}
#else
static constexpr bool kRadeTx = false;
static std::vector<cf> radeModulate(double, const char*) { return {}; }
#endif

/** The modem signal at 8 kHz as its analytic baseband (comptx), with the tx's text channel carrying "VIBE TEST\r".
 *  RADE: its own transmitter, no end-of-over (an over cut off mid-way, as the codec2 modes' are) unless `radeCall`. */
static std::vector<cf> modulate(int sub, double sec, const char* radeCall = nullptr) {
    if (sub == fdv::kRade) return radeModulate(sec, radeCall);
    const fdv::Api& a = fdv::api();
    fdv::FdvOpaque* f = a.open(fdv::codec2Mode(sub));
    TxText tt{"VIBE TEST\r", 0};
    a.setTxt(f, nullptr, &txChar, &tt);
    const int ns = a.nSpeech(f), nm = a.nNomModem(f);
    const std::vector<short> sp = voice(sec, 7);
    std::vector<cf> out;
    std::vector<cf> mod(static_cast<size_t>(nm));
    std::vector<short> blk(static_cast<size_t>(ns));
    for (size_t i = 0; i + size_t(ns) <= sp.size(); i += size_t(ns)) {
        std::copy(sp.begin() + long(i), sp.begin() + long(i) + ns, blk.begin());
        a.comptx(f, mod.data(), blk.data());
        out.insert(out.end(), mod.begin(), mod.end());
    }
    a.close(f);
    double p = 0;
    for (auto& x : out) p += std::norm(x);
    const float k = float(0.2 / std::sqrt(p / double(out.size())));   // analytic rms 0.2
    for (auto& x : out) x *= k;
    return out;
}

static double realPower(const std::vector<cf>& x) {
    double p = 0;
    for (auto& v : x) p += double(v.real()) * v.real();
    return p / double(x.size());
}

/** 48 kHz USB audio: Re(analytic) upsampled, with real noise for `snrDb` in 3 kHz. */
static std::vector<float> usbAudio(const std::vector<cf>& m8, double snrDb, unsigned seed) {
    std::vector<float> re(m8.size());
    for (size_t i = 0; i < m8.size(); ++i) re[i] = m8[i].real();
    const double ps = realPower(m8);
    // real noise at 8 kHz spreads over 4 kHz: σ²·3/4 falls in 3 kHz
    const double sigma = std::sqrt(ps / (0.75 * std::pow(10.0, snrDb / 10)));
    std::mt19937 rng(seed); std::normal_distribution<float> nd(0.f, float(sigma));
    for (auto& v : re) v += nd(rng);
    vibedsp::RationalResampler up(8000, 48000);
    std::vector<float> o(size_t(up.maxOut(int(re.size()))));
    o.resize(size_t(up.process(re.data(), int(re.size()), o.data())));
    return o;
}

struct Run {
    int locked = 0;
    uint64_t speech = 0;
    double outRms = 0;
    double nonZeroFrac = 0;
    int open = 0;
    std::string status;
    float snr = NAN;
    uint64_t dropped = 0;
    double cpuPerSec = 0;
    int wins[fdv::kSubCount] = {0, 0, 0, 0, 0};
    unsigned seen = 0;          // ★ every sub-mode that was in sync at some point (bit = Sub): a RADE over ENDS out of sync
    std::string syncStatus;     // the last report while in sync
    std::string last;           // RADE's last end-of-over callsign
};

/** Feed `x` (48 kHz) in 20 ms blocks as the audio callback does: feed, then take the same number of frames out. */
static Run play(FreeDvListener& L, const std::vector<float>& x) {
    Run r;
    const int blk = 960;
    std::vector<float> out(static_cast<size_t>(blk));
    double e = 0; size_t nz = 0, tot = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + size_t(blk) <= x.size(); i += size_t(blk)) {
        L.feedAudio(x.data() + i, blk, 1, 48000.0);
        while (L.queuedFrames() > 24000) std::this_thread::sleep_for(std::chrono::microseconds(200));
        L.takePcm(out.data(), blk);
        for (float v : out) { e += double(v) * v; if (v != 0.0f) ++nz; }
        tot += size_t(blk);
        if (const int k = L.lockedSub()) { r.seen |= 1u << k; r.syncStatus = L.statusJson(nullptr); }
    }
    while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.cpuPerSec = wall / (double(x.size()) / 48000.0);
    r.locked = L.lockedSub(); r.speech = L.speechBlocks(); r.open = L.modemsOpen();
    r.outRms = std::sqrt(e / double(std::max<size_t>(1, tot)));
    r.nonZeroFrac = double(nz) / double(std::max<size_t>(1, tot));
    r.status = L.statusJson(nullptr); r.snr = L.snrDb(); r.dropped = L.droppedBlocks();
    for (int k = 0; k < fdv::kSubCount; ++k) r.wins[k] = L.winsOf(k);
    r.last = L.lastCallsign();
    return r;
}

static std::string desc(const Run& r) {
    char b[400];
    std::snprintf(b, sizeof b, "locked=%s seen=%x speech=%llu rms=%.3f audible=%.0f%% open=%d snr=%.1f  %s",
                  fdv::subName(r.locked), r.seen, (unsigned long long)r.speech, r.outRms, 100 * r.nonZeroFrac, r.open, r.snr, r.status.c_str());
    return b;
}

// ── the server's path: IQ → RxPipeline (USB) → FreeDvListener ───────────────────────────────────────────────────────
static Run throughPipeline(int sub, int listenSub, double snrDb, double offHz, double errHz, double sec) {
    const std::vector<cf> m8 = modulate(sub, sec);
    // analytic 8 kHz → 48 kHz (I and Q each), then placed at dial offset + error
    std::vector<float> I(m8.size()), Q(m8.size());
    for (size_t i = 0; i < m8.size(); ++i) { I[i] = m8[i].real(); Q[i] = m8[i].imag(); }
    vibedsp::RationalResampler ui(8000, 48000), uq(8000, 48000);
    std::vector<float> I4(size_t(ui.maxOut(int(I.size())))), Q4(I4.size());
    const int n = ui.process(I.data(), int(I.size()), I4.data());
    uq.process(Q.data(), int(Q.size()), Q4.data());
    const double ps = realPower(m8);
    // complex noise at 48 kHz: the USB demodulator keeps 3 kHz of one side, and the real part halves it — σc²·3/96
    const double sc = std::sqrt(ps * 32.0 / std::pow(10.0, snrDb / 10));
    std::mt19937 rng(11); std::normal_distribution<float> nd(0.f, float(sc / std::sqrt(2.0)));
    std::vector<cf> iq(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2 * M_PI * (offHz + errHz) * i / 48000.0;
        iq[size_t(i)] = cf(I4[size_t(i)], Q4[size_t(i)]) * cf(float(std::cos(ph)), float(std::sin(ph))) + cf(nd(rng), nd(rng));
    }
    FreeDvListener L(listenSub);
    struct Ctx { FreeDvListener* L; std::vector<float> out; double e = 0; size_t nz = 0, tot = 0; unsigned seen = 0; } ctx{&L, {}, 0, 0, 0, 0};
    vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
    cb.audio = [](void* c, const float* pcm, int frames, int ch, int rate) {
        auto* x = static_cast<Ctx*>(c);
        x->L->feedAudio(pcm, frames, ch, double(rate));
        while (x->L->queuedFrames() > 24000) std::this_thread::sleep_for(std::chrono::microseconds(200));
        x->out.resize(size_t(frames));
        x->L->takePcm(x->out.data(), frames);
        for (float v : x->out) { x->e += double(v) * v; if (v != 0.0f) ++x->nz; }
        x->tot += size_t(frames);
    };
    vibedsp::RxPipeline p; p.setMeasureThread(false);
    p.start(48000.0, 1024, 20.0, 48000, cb);
    p.setTune(offHz, vibedsp::RxPipeline::Mode::SSB_USB, 2700);
    const int blk = 4800;
    for (int i = 0; i + blk <= n; i += blk) {
        p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
        if (const int k = L.lockedSub()) ctx.seen |= 1u << k;
    }
    while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Run r;
    r.locked = L.lockedSub(); r.speech = L.speechBlocks(); r.open = L.modemsOpen();
    r.outRms = std::sqrt(ctx.e / double(std::max<size_t>(1, ctx.tot)));
    r.nonZeroFrac = double(ctx.nz) / double(std::max<size_t>(1, ctx.tot));
    r.status = L.statusJson(nullptr); r.snr = L.snrDb(); r.dropped = L.droppedBlocks();
    for (int k = 0; k < fdv::kSubCount; ++k) r.wins[k] = L.winsOf(k);
    r.seen = ctx.seen | (r.locked ? 1u << r.locked : 0u);
    r.last = L.lastCallsign();
    return r;
}

/** ★ The bench clip for scripts/test-server-freedv.mjs: unsigned 8-bit IQ (rtl_tcp's format) at `rate`, `sec` long, centred
 *  on the fake dongle's --hf-centre, with three stations — 700D at +10 kHz, 700E at +20 kHz, 1600 at −10 kHz — at `snr` dB in
 *  3 kHz each. The clip loops in the fake dongle, so each station's modem relocks once a loop (as a real station's overs do). */
/*  ★ RADE V1 (2026-11-16) at +30 kHz where rade_c's transmitter is here: an over of 70 % of the clip, its end-of-over
 *  frame carrying "G4ABC", then noise to the loop — so a receiver syncs, decodes speech, reports "last":"G4ABC" once the
 *  over ends, and syncs again on the next loop. A clip with RADE alone (no libcodec2) is written too. */
static int writeClip(const char* path, int rate, double sec, double snrDb, bool lsb) {
    const fdv::Api& a = fdv::api();
    const bool c2 = a.ok && a.comptx, rade = kRadeTx && a.sub[fdv::kRade];
    if (!c2 && !rade) { std::printf("NOT RUN — %s\n", a.anySub() ? "no freedv_comptx" : a.why.c_str()); return 3; }
    if (rate % 8000) { std::printf("the rate must be a multiple of 8 kHz\n"); return 2; }
    const int n = int(sec * rate);
    std::vector<cf> iq(static_cast<size_t>(n));
    struct St { int sub; double off; } st[] = {{fdv::k700D, 10000}, {fdv::k700E, 20000}, {fdv::k1600, -10000}, {fdv::kRade, 30000}};
    double ps = 0;
    for (auto t : st) {
        if (t.sub == fdv::kRade ? !rade : (!c2 || !a.sub[t.sub])) continue;
        std::vector<cf> m8 = t.sub == fdv::kRade ? modulate(t.sub, sec * 0.7, "G4ABC") : modulate(t.sub, sec + 1);   // (the same voice and text for each)
        if (t.sub == fdv::kRade) {
            const double pr = realPower(std::vector<cf>(m8.begin(), m8.begin() + long(std::min<size_t>(m8.size(), size_t(sec * 0.6 * 8000)))));
            /* ★ 6 dB above the codec2 stations: the end-of-over callsign is ONE frame of LDPC-coded QPSK and needs ~9 dB to
             *  come through (measured 2026-11-16: every time at 8.7 dB, never at 6.5) — at the clip's 12 dB, RADE's own SNR
             *  estimate reads ~8 dB through the range-locked radio's channel and the server test's "last" check failed. */
            const double want = (ps == 0 ? pr : ps) * 4.0;
            for (auto& x : m8) x *= float(std::sqrt(want / pr));
            if (ps == 0) ps = pr;
            m8.resize(size_t((sec + 1) * 8000), cf(0, 0));             // noise only to the end of the loop
        } else ps = realPower(m8);
        ps = realPower(m8);
        std::vector<float> I(m8.size()), Q(m8.size());
        for (size_t i = 0; i < m8.size(); ++i) { I[i] = m8[i].real(); Q[i] = m8[i].imag(); }
        vibedsp::RationalResampler ui(8000, rate), uq(8000, rate);
        std::vector<float> Iu(static_cast<size_t>(ui.maxOut(int(I.size())))), Qu(Iu.size());
        const int nu = ui.process(I.data(), int(I.size()), Iu.data());
        uq.process(Q.data(), int(Q.size()), Qu.data());
        for (int i = 0; i < n && i < nu; ++i) {
            const double ph = 2 * M_PI * t.off * i / rate;
            // ★ lower sideband: the station's spectrum mirrored below its dial (the conjugate of the analytic signal)
            const cf v = lsb ? cf(Iu[size_t(i)], -Qu[size_t(i)]) : cf(Iu[size_t(i)], Qu[size_t(i)]);
            iq[size_t(i)] += v * cf(float(std::cos(ph)), float(std::sin(ph)));
        }
    }
    // complex noise over `rate`: a USB demodulator keeps 3 kHz of one side and the real part halves it — σc²·3000/(2·rate)
    const double sc = std::sqrt(ps * 2.0 * rate / 3000.0 / std::pow(10.0, snrDb / 10));
    std::mt19937 rng(21); std::normal_distribution<float> nd(0.f, float(sc / std::sqrt(2.0)));
    double peak = 0;
    for (auto& x : iq) { x += cf(nd(rng), nd(rng)); peak = std::max(peak, double(std::max(std::fabs(x.real()), std::fabs(x.imag())))); }
    const double k = 100.0 / peak;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::vector<uint8_t> u(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        u[size_t(2 * i)] = uint8_t(std::lrint(127.5 + k * iq[size_t(i)].real()));
        u[size_t(2 * i + 1)] = uint8_t(std::lrint(127.5 + k * iq[size_t(i)].imag()));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    std::printf("wrote %s: %d S/s, %.0f s,%s%s %s, %.0f dB in 3 kHz\n", path, rate, sec, c2 ? " 700D +10 kHz, 700E +20 kHz, 1600 −10 kHz," : "",
                rade ? " RADE +30 kHz (over, then G4ABC's end of over)," : "", lsb ? "LSB" : "USB", snrDb);
    return 0;
}

/** ★ Off-air audio (a 16-bit PCM WAV of a USB/LSB demodulation — tools/ubersdr-rec) through the receiver, as the audio
 *  callback feeds it, printing every report that changes: `test-freedv wav FILE [auto|700D|700E|1600]`. A tool, not a
 *  pass/fail — what it prints is what a listener's VTS would have shown. */
static int playWav(const char* path, int sub) {
    if (!fdv::available()) { std::printf("NOT RUN — %s\n", fdv::api().why.c_str()); return 3; }
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    std::vector<uint8_t> b; { uint8_t t[65536]; size_t n; while ((n = std::fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n); }
    std::fclose(f);
    // the RIFF chunks: fmt (rate, channels, 16-bit) and data
    int rate = 0, ch = 1; size_t at = 12, dataAt = 0, dataLen = 0;
    while (at + 8 <= b.size()) {
        const uint32_t len = uint32_t(b[at + 4]) | uint32_t(b[at + 5]) << 8 | uint32_t(b[at + 6]) << 16 | uint32_t(b[at + 7]) << 24;
        if (!std::memcmp(&b[at], "fmt ", 4)) { ch = b[at + 10] | b[at + 11] << 8; rate = int(uint32_t(b[at + 12]) | uint32_t(b[at + 13]) << 8 | uint32_t(b[at + 14]) << 16); }
        if (!std::memcmp(&b[at], "data", 4)) { dataAt = at + 8; dataLen = std::min<size_t>(len, b.size() - dataAt); break; }
        at += 8 + len + (len & 1);
    }
    if (!rate || !dataAt) { std::printf("not a PCM WAV\n"); return 2; }
    const size_t frames = dataLen / 2 / size_t(ch);
    std::vector<float> x(frames);
    for (size_t i = 0; i < frames; ++i) x[i] = float(int16_t(b[dataAt + 2 * i * size_t(ch)] | b[dataAt + 2 * i * size_t(ch) + 1] << 8)) / 32768.0f;
    std::printf("%s: %d Hz, %d ch, %.1f s\n", path, rate, ch, double(frames) / rate);
    FreeDvListener L(sub);
    const int blk = rate / 50;
    std::vector<float> out(static_cast<size_t>(blk));
    uint64_t ver = 0; double e = 0; size_t nz = 0;
    for (size_t i = 0; i + size_t(blk) <= x.size(); i += size_t(blk)) {
        L.feedAudio(x.data() + i, blk, 1, double(rate));
        while (L.queuedFrames() > size_t(rate) / 2) std::this_thread::sleep_for(std::chrono::microseconds(200));
        L.takePcm(out.data(), blk);
        for (float v : out) { e += double(v) * v; if (v != 0.0f) ++nz; }
        uint64_t v = 0; const std::string j = L.statusJson(&v);   // ★ RADE's callsigns show as "last" once each over ends
        if (v != ver) { ver = v; std::printf("  %7.1f s  %s\n", double(i) / rate, j.c_str()); }
    }
    std::printf("speech blocks %llu, audible %.0f %% of the time\n", (unsigned long long)L.speechBlocks(), 100.0 * double(nz) / double(std::max<size_t>(1, x.size())));
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "wav") return playWav(argv[2], argc > 3 ? std::max(0, fdv::subFromName(argv[3])) : fdv::kAuto);
    if (argc > 1 && std::string(argv[1]) == "--probe") {   // the child for the VIBE_FREEDV_DISABLE / VIBE_RADE_DISABLE checks
        std::printf("%d %s\n", fdv::available() ? 1 : 0, fdv::api().subsJson().c_str());
        return 0;
    }
    // test-freedv clip OUT.cu8 [rate=288000] [sec=20] [snr=12] [usb|lsb]
    if (argc > 2 && std::string(argv[1]) == "clip")
        return writeClip(argv[2], argc > 3 ? std::atoi(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 20, argc > 5 ? std::atof(argv[5]) : 12,
                         argc > 6 && std::string(argv[6]) == "lsb");
    const fdv::Api& a = fdv::api();
    if (!a.anySub()) { std::printf("NOT RUN — %s\n", a.why.c_str()); return 3; }
    const bool c2 = a.ok && a.comptx;                         // ★ the codec2 half (700D, 700E, 1600) can be tested
    const bool rade = kRadeTx && a.sub[fdv::kRade];          // ★ the RADE half: the receiver AND the tests' transmitter
    std::printf("libcodec2: %s; RADE V1: %s; sub-modes %s\n", a.ok ? (a.path + " (FreeDV API " + std::to_string(a.version) + ")").c_str() : a.why.c_str(),
                a.sub[fdv::kRade] ? (kRadeTx ? "compiled in, with the test transmitter" : "compiled in, NO test transmitter") : "not built in",
                a.subsJson().c_str());
    if (!c2 && !rade) { std::printf("NOT RUN — %s\n", a.ok ? "this libcodec2 has no freedv_comptx (the test transmitter)" : "RADE without its test transmitter"); return 3; }
    if (a.ok && !a.comptx) std::printf("   (the codec2 half is NOT RUN: no freedv_comptx)\n");
    if (!rade) std::printf("   (the RADE half is NOT RUN: %s)\n", a.sub[fdv::kRade] ? "no test transmitter (librade_vibe_tx.a)" : "not built in");

    std::vector<int> subs;   // ★ every sub-mode this run can transmit and receive: RADE first, as AUTO opens them
    if (rade) subs.push_back(fdv::kRade);
    if (c2) for (int s : {int(fdv::k700D), int(fdv::k700E), int(fdv::k1600)}) if (a.sub[s]) subs.push_back(s);
    int openAll = 0; for (int s = 1; s < fdv::kSubCount; ++s) openAll += fdv::runs(s) ? 1 : 0;
    const auto name = [](int s) { return std::string(fdv::subName(s)); };
    const auto bit = [](int s) { return 1u << s; };

    std::printf("\n── 0. the sideband rule (AUTO = the FreeDV application's; USB / LSB by hand) and the names ──\n");
    check(fdv::lowerSideband(7.177e6, fdv::kSbAuto) && fdv::lowerSideband(3.643e6, fdv::kSbAuto), "AUTO: 40 m and 80 m LSB");
    check(!fdv::lowerSideband(14.236e6, fdv::kSbAuto) && !fdv::lowerSideband(10e6, fdv::kSbAuto), "AUTO: from 10 MHz up USB");
    check(!fdv::lowerSideband(5.3685e6, fdv::kSbAuto) && !fdv::lowerSideband(5.4035e6, fdv::kSbAuto), "AUTO: 60 m USB");
    check(!fdv::lowerSideband(7.177e6, fdv::kSbUsb) && fdv::lowerSideband(14.236e6, fdv::kSbLsb), "USB / LSB by hand win over the convention");
    check(fdv::sbFromName("LSB") == fdv::kSbLsb && fdv::sbFromName("auto") == fdv::kSbAuto && fdv::sbFromName("dsb") == -1, "sideband names");
    check(fdv::subFromName("rade") == fdv::kRade && fdv::subFromName("RADEV1") == fdv::kRade && std::string(fdv::subName(fdv::kRade)) == "RADE"
          && fdv::subFromName("RADEV2") == -1, "RADE's name (V1 only: RADEV2 is none of ours)");
    if (a.sub[fdv::kRade]) check(a.subsJson().rfind("[\"RADE\"", 0) == 0, "freedvSubs lists RADE first", a.subsJson());

    std::printf("\n── 1. each sub-mode chosen by hand, 10 dB, 20 s ──\n");
    for (int s : subs) {
        FreeDvListener L(s);
        const Run r = play(L, usbAudio(modulate(s, 20, s == fdv::kRade ? "G4ABC" : nullptr), 10, 1));
        std::printf("   %s: %s\n", name(s).c_str(), desc(r).c_str());
        // ★ a RADE over ENDS with its end-of-over frame, out of sync: judged on what happened during it
        const std::string& st = s == fdv::kRade ? r.syncStatus : r.status;
        check(s == fdv::kRade ? (r.seen & bit(s)) != 0 : r.locked == s, (name(s) + " in sync").c_str());
        check(r.speech > 50, (name(s) + " speech decoded").c_str(), std::to_string(r.speech) + " blocks");
        check(r.outRms > 0.01 && r.nonZeroFrac > 0.5, (name(s) + " audio comes out").c_str());
        check(st.find("\"state\":\"sync\"") != std::string::npos && st.find("\"mode\":\"" + name(s) + "\"") != std::string::npos,
              (name(s) + " report names the mode").c_str(), st);
        if (s == fdv::kRade) {
            check(r.last == "G4ABC" && r.status.find("\"last\":\"G4ABC\"") != std::string::npos, "RADE end of over: the callsign, as \"last\"", r.status);
            check(st.find("\"text\"") == std::string::npos, "…and never as the talker's text while in sync", st);
            check(r.status.find("\"state\":\"search\"") != std::string::npos, "the over ended: searching again", r.status);
        } else {
            check(r.status.find("\"text\":\"VIBE TEST\"") != std::string::npos, (name(s) + " text channel").c_str());
            const size_t at = r.status.find("\"snr\":"); const int shown = at == std::string::npos ? -99 : std::atoi(r.status.c_str() + at + 6);
            check(std::fabs(shown - r.snr) <= 1.5f, (name(s) + " report's SNR follows the modem").c_str(), std::to_string(shown));
        }
        if (s == fdv::kRade) {
            const size_t at = st.find("\"snr\":"); const int shown = at == std::string::npos ? -99 : std::atoi(st.c_str() + at + 6);
            check(shown >= 6 && shown <= 14, "RADE report's SNR near the 10 dB sent", std::to_string(shown));
        }
        check(r.dropped == 0, (name(s) + " nothing dropped").c_str());
        std::printf("   CPU: %.1f %% of real time on this machine (manual %s)\n", 100 * r.cpuPerSec, name(s).c_str());
    }

    std::printf("\n── 2. AUTO finds each, 10 dB, 20 s ──\n");
    for (int s : subs) {
        FreeDvListener L(fdv::kAuto);
        const Run r = play(L, usbAudio(modulate(s, 20), 10, 2));
        std::printf("   on air %s: %s\n", name(s).c_str(), desc(r).c_str());
        check(r.locked == s, ("AUTO names " + name(s)).c_str());
        check(r.open == 1, "the other sub-modes closed after the lock");
        check(r.status.find("\"sub\":\"auto\"") != std::string::npos, "the report says AUTO");
        check(r.speech > 40 && r.outRms > 0.01, "audio after AUTO's lock");
        std::printf("   CPU: %.1f %% of real time (AUTO, searching then one modem)\n", 100 * r.cpuPerSec);
    }

    /* ★★ 2b. AUTO NEVER CHOOSES THE WRONG SUB-MODE (2026-11-16: test-server-freedv failed ~1 run in 4 on the 1600 station —
     *  AUTO in "sync" at 2 dB on something else, then silence). Each sub-mode alone, many times: a different noise lead-in
     *  (so the modems see the signal start at a different phase), the clip's own loop (the signal twice, end to start — the
     *  fake dongle's wrap), 8 and 12 dB. Every win must be the sub-mode on air — RADE included, both ways: no RADE win on a
     *  codec2 signal, and no codec2 win on RADE. */
    std::printf("\n── 2b. AUTO on one sub-mode, many starts and the loop wrap: never a wrong win ──\n");
    {
        int runs = 0, wrong = 0, missed = 0;
        std::string why;
        for (int s : subs) {
            const std::vector<cf> m8 = modulate(s, 20);
            for (int k = 0; k < 16; ++k) {
                const double snr = (k & 1) ? 8 : 12;
                std::vector<float> one = usbAudio(m8, snr, 100 + unsigned(k));
                std::vector<float> x;
                // a noise lead-in of 0…5.6 s at the same noise level as the signal's
                { const double ps = realPower(m8); const double sig = std::sqrt(ps / (0.75 * std::pow(10.0, snr / 10)));
                  std::mt19937 rng(500 + unsigned(k)); std::normal_distribution<float> nd(0.f, float(sig));
                  std::vector<float> n8(size_t(k * 0.37 * 8000)); for (auto& v : n8) v = nd(rng);
                  vibedsp::RationalResampler up(8000, 48000); std::vector<float> o(size_t(up.maxOut(int(n8.size())) + 1));
                  o.resize(size_t(up.process(n8.data(), int(n8.size()), o.data()))); x = o; }
                x.insert(x.end(), one.begin(), one.end());
                x.insert(x.end(), one.begin(), one.end());   // ★ the loop: end straight into start, as the fake dongle replays
                FreeDvListener L(fdv::kAuto);
                const Run r = play(L, x);
                ++runs;
                int other = 0;
                for (int o = 1; o < fdv::kSubCount; ++o) if (o != s) other += L.winsOf(o);
                if (other) { ++wrong; char b[200]; std::snprintf(b, sizeof b, " [%s k=%d %.0f dB: wins RADE %d 700D %d 700E %d 1600 %d]", fdv::subName(s), k, snr,
                                                           L.winsOf(fdv::kRade), L.winsOf(fdv::k700D), L.winsOf(fdv::k700E), L.winsOf(fdv::k1600)); why += b; }
                if (r.locked != s) { ++missed; char b[120]; std::snprintf(b, sizeof b, " [%s k=%d: ends %s]", fdv::subName(s), k, fdv::subName(r.locked)); why += b; }
            }
        }
        std::printf("   %d runs, %d with a wrong win, %d not in sync on the right sub-mode at the end%s\n", runs, wrong, missed, why.c_str());
        check(wrong == 0, "AUTO never wins on a sub-mode that is not on air", std::to_string(wrong) + " of " + std::to_string(runs));
        check(missed == 0, "…and every run ends in sync on the one that is", std::to_string(missed) + " of " + std::to_string(runs));
    }

    /* ★★ 2b′. …and through the REAL USB chain (the server's path, where the false win was seen: 700E holding "sync" on the
     *  1600 station at ~1 dB, every frame failing its FEC): each sub-mode, 8 runs with different mistunings and noise. */
    std::printf("\n── 2b′. the same through the RxPipeline USB chain ──\n");
    {
        int runs = 0, wrong = 0, missed = 0; std::string why;
        for (int s : subs) {
            for (int k = 0; k < 8; ++k) {
                const Run r = throughPipeline(s, fdv::kAuto, (k & 1) ? 8 : 12, 6000 + 137 * k, -40 + 11 * k, 12);
                ++runs;
                int other = 0; for (int o = 1; o < fdv::kSubCount; ++o) if (o != s) other += r.wins[o];
                if (other) { ++wrong; char b[140]; std::snprintf(b, sizeof b, " [%s k=%d: wins %d/%d/%d/%d]", fdv::subName(s), k, r.wins[1], r.wins[2], r.wins[3], r.wins[4]); why += b; }
                if (r.locked != s) { ++missed; char b[80]; std::snprintf(b, sizeof b, " [%s k=%d ends %s]", fdv::subName(s), k, fdv::subName(r.locked)); why += b; }
            }
        }
        std::printf("   %d runs, %d with a wrong win, %d not in sync on the right one at the end%s\n", runs, wrong, missed, why.c_str());
        check(wrong == 0 && missed == 0, "through the USB chain: only ever the sub-mode on air", why);
    }

    /* ★★ 2c. A WINNER THAT DID NOT LAST searches again at once, not after 8 s: 700D for 2.5 s (AUTO wins it about 1.3 s
     *  in), then the same dial carries 1600. AUTO must be decoding 1600 within 4 s of the change. */
    std::printf("\n── 2c. a short-lived win: the full search again at once ──\n");
    if (c2 && a.sub[fdv::k700D] && a.sub[fdv::k1600]) {
        std::vector<float> d = usbAudio(modulate(fdv::k700D, 3), 12, 31), f = usbAudio(modulate(fdv::k1600, 12), 12, 32);
        d.resize(size_t(48000 * 2.5));
        std::vector<float> x = d; x.insert(x.end(), f.begin(), f.begin() + 48000 * 4);
        FreeDvListener L(fdv::kAuto);
        const Run r = play(L, x);
        std::printf("   %s  wins 700D %d 1600 %d, probation losses %d\n", desc(r).c_str(), L.winsOf(fdv::k700D), L.winsOf(fdv::k1600), L.probationLosses());
        check(L.winsOf(fdv::k700D) >= 1 && L.probationLosses() >= 1, "the 700D win that did not last went back to the search");
        check(r.locked == fdv::k1600, "…and AUTO is on 1600 within 4 s of the change (not 8 s + a search)");
    }

    /* ★★★ 2d. AUTO CHOOSING RADE OR 700D as the dial changes between them — what a listener on a shared FreeDV calling
     *  frequency meets: a RADE over (ending with its end-of-over frame), a second of noise, then a 700D station; and the
     *  other way round. Each must be won in turn, with speech from each, and the RADE callsign kept as "last". */
    std::printf("\n── 2d. AUTO: RADE then 700D, and 700D then RADE, on one dial ──\n");
    if (rade && c2 && a.sub[fdv::k700D]) {
        const double snr = 10;
        const std::vector<float> R = usbAudio(modulate(fdv::kRade, 6, "M0RAD"), snr, 41), D = usbAudio(modulate(fdv::k700D, 16), snr, 42);
        std::vector<float> gap(48000, 0.f);
        { std::mt19937 rng(43); const double sig = std::sqrt(realPower(modulate(fdv::k700D, 2)) / (0.75 * std::pow(10.0, snr / 10)));
          std::normal_distribution<float> nd(0.f, float(sig)); std::vector<float> n8(8000); for (auto& v : n8) v = nd(rng);
          vibedsp::RationalResampler up(8000, 48000); gap.resize(size_t(up.maxOut(8000))); gap.resize(size_t(up.process(n8.data(), 8000, gap.data()))); }
        {
            std::vector<float> x = R; x.insert(x.end(), gap.begin(), gap.end()); x.insert(x.end(), D.begin(), D.end());
            FreeDvListener L(fdv::kAuto);
            const Run r = play(L, x);
            std::printf("   RADE → 700D: %s  wins RADE %d 700D %d\n", desc(r).c_str(), r.wins[fdv::kRade], r.wins[fdv::k700D]);
            check(r.wins[fdv::kRade] >= 1 && (r.seen & bit(fdv::kRade)), "AUTO won the RADE over first");
            check(r.wins[fdv::k700D] >= 1 && r.locked == fdv::k700D, "…then 700D once RADE had gone", desc(r));
            check(r.last == "M0RAD", "…keeping RADE's callsign as \"last\"", r.last);
        }
        {
            std::vector<float> D2(D.begin(), D.begin() + 48000 * 6), x = D2;
            const std::vector<float> R2 = usbAudio(modulate(fdv::kRade, 16, "M0RAD"), snr, 44);
            x.insert(x.end(), gap.begin(), gap.end()); x.insert(x.end(), R2.begin(), R2.end());
            FreeDvListener L(fdv::kAuto);
            const Run r = play(L, x);
            std::printf("   700D → RADE: %s  wins RADE %d 700D %d\n", desc(r).c_str(), r.wins[fdv::kRade], r.wins[fdv::k700D]);
            check(r.wins[fdv::k700D] >= 1 && r.wins[fdv::kRade] >= 1 && (r.seen & bit(fdv::kRade)), "AUTO won 700D, then RADE", desc(r));
        }
    } else std::printf("   not run — needs both halves\n");

    std::printf("\n── 3. weak signals, manual ──\n");
    {
        struct W { int s; double snr; } ws[] = {{fdv::kRade, 2}, {fdv::k700D, 4}, {fdv::k700E, 5}, {fdv::k1600, 5}};
        for (auto w : ws) {
            if (std::find(subs.begin(), subs.end(), w.s) == subs.end()) continue;
            FreeDvListener L(w.s);
            const Run r = play(L, usbAudio(modulate(w.s, 20), w.snr, 3));
            std::printf("   %s at %.0f dB: %s\n", fdv::subName(w.s), w.snr, desc(r).c_str());
            check(r.locked == w.s && r.speech > 30, (name(w.s) + " holds sync at " + std::to_string(int(w.snr)) + " dB").c_str());
        }
    }

    std::printf("\n── 4. pure noise, AUTO, 30 s ──\n");
    {
        FreeDvListener L(fdv::kAuto);
        std::vector<float> nz(48000 * 30);
        std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : nz) v = nd(rng);
        const Run r = play(L, nz);
        std::printf("   %s\n", desc(r).c_str());
        check(r.locked == fdv::kAuto && r.seen == 0 && r.status.find("\"state\":\"search\"") != std::string::npos, "never a lock on noise");
        check(r.outRms == 0.0, "silent on noise");
        check(r.open == openAll, "all sub-modes still searching", std::to_string(r.open) + " of " + std::to_string(openAll));
        check(r.status.find("\"last\"") == std::string::npos, "no callsign out of noise");
        std::printf("   CPU: %.1f %% of real time (AUTO searching, %d modems)\n", 100 * r.cpuPerSec, openAll);
    }
    if (a.sub[fdv::kRade]) {
        FreeDvListener L(fdv::kRade);
        std::vector<float> nz(48000 * 60);
        std::mt19937 rng(6); std::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : nz) v = nd(rng);
        const Run r = play(L, nz);
        std::printf("   RADE alone, 60 s of noise: %s\n", desc(r).c_str());
        check(r.seen == 0 && r.speech == 0 && r.outRms == 0.0, "RADE: no false sync, no sound, in 60 s of noise");
        std::printf("   CPU: %.2f %% of real time (RADE searching)\n", 100 * r.cpuPerSec);
    }

    std::printf("\n── 5. the server's path: IQ → RxPipeline USB → modem, off the dial centre, +25 Hz error ──\n");
    for (int s : subs) {
        const Run r = throughPipeline(s, fdv::kAuto, 8, 6000, 25, 20);
        std::printf("   %s: %s\n", name(s).c_str(), desc(r).c_str());
        check(r.locked == s && r.speech > 40 && r.outRms > 0.01, (name(s) + " through the real USB chain, AUTO").c_str());
    }

    std::printf("\n── 6. switching sub-mode while running ──\n");
    if (c2) {
        FreeDvListener L(fdv::k1600);
        Run r = play(L, usbAudio(modulate(fdv::k700D, 8), 10, 4));
        check(r.locked == fdv::kAuto, "1600 chosen, 700D on air: no lock", desc(r));
        L.setSub(fdv::k700D);
        r = play(L, usbAudio(modulate(fdv::k700D, 12), 10, 6));
        check(r.locked == fdv::k700D && r.status.find("\"sub\":\"700D\"") != std::string::npos, "switched to 700D: in sync", desc(r));
    }
    if (rade) {
        FreeDvListener L(c2 ? fdv::k700D : fdv::kAuto);
        if (c2) { Run r = play(L, usbAudio(modulate(fdv::kRade, 8), 10, 7)); check(r.seen == 0, "700D chosen, RADE on air: no lock", desc(r)); }
        L.setSub(fdv::kRade);
        Run r = play(L, usbAudio(modulate(fdv::kRade, 12), 10, 8));
        check(r.locked == fdv::kRade && r.status.find("\"sub\":\"RADE\"") != std::string::npos, "switched to RADE: in sync", desc(r));
    }

    std::printf("\n── 7. no library = no mode ──\n");
    {
        const auto probe = [&](const char* env) {
            std::string cmd = std::string(env) + " '" + argv[0] + "' --probe";
            FILE* p = popen(cmd.c_str(), "r");
            char buf[128] = {0};
            if (p) { if (!std::fgets(buf, sizeof buf, p)) buf[0] = 0; pclose(p); }
            return std::string(buf);
        };
        const std::string all = probe("VIBE_FREEDV_DISABLE=1");
        check(all.rfind("0", 0) == 0, "VIBE_FREEDV_DISABLE=1 → not available, RADE included", all);
        if (a.sub[fdv::kRade]) {
            const std::string noRade = probe("VIBE_RADE_DISABLE=1");
            check(noRade.find("RADE") == std::string::npos && noRade.rfind(a.ok ? "1" : "0", 0) == 0,
                  "VIBE_RADE_DISABLE=1 → RADE gone, codec2's sub-modes as before", noRade);
            const std::string noC2 = probe("VIBE_CODEC2_DISABLE=1");
            check(noC2.rfind("1 [\"RADE\"]", 0) == 0, "without libcodec2 the mode is still offered, with RADE alone", noC2);
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
