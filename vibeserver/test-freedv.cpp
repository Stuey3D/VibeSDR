// ★★ V12 FreeDV AS A MODE (vibe_freedv.h, docs/v12/FREEDV.md) — end to end on the library's OWN transmitter.
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
// ★ Exit 3 = NOT RUN when libcodec2 is not installed (apt install libcodec2-1.2 / brew install codec2). VIBE_FREEDV_DISABLE=1
//   must make the library "not installed" — checked last, in a child process's environment.
#include "vibe_freedv.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
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

/** The modem signal at 8 kHz as its analytic baseband (comptx), with the tx's text channel carrying "VIBE TEST\r". */
static std::vector<cf> modulate(int sub, double sec) {
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
    }
    while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.cpuPerSec = wall / (double(x.size()) / 48000.0);
    r.locked = L.lockedSub(); r.speech = L.speechBlocks(); r.open = L.modemsOpen();
    r.outRms = std::sqrt(e / double(std::max<size_t>(1, tot)));
    r.nonZeroFrac = double(nz) / double(std::max<size_t>(1, tot));
    r.status = L.statusJson(nullptr); r.snr = L.snrDb(); r.dropped = L.droppedBlocks();
    return r;
}

static std::string desc(const Run& r) {
    char b[400];
    std::snprintf(b, sizeof b, "locked=%s speech=%llu rms=%.3f audible=%.0f%% open=%d snr=%.1f  %s",
                  fdv::subName(r.locked), (unsigned long long)r.speech, r.outRms, 100 * r.nonZeroFrac, r.open, r.snr, r.status.c_str());
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
    struct Ctx { FreeDvListener* L; std::vector<float> out; double e = 0; size_t nz = 0, tot = 0; } ctx{&L, {}, 0, 0, 0};
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
    for (int i = 0; i + blk <= n; i += blk) p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
    while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Run r;
    r.locked = L.lockedSub(); r.speech = L.speechBlocks(); r.open = L.modemsOpen();
    r.outRms = std::sqrt(ctx.e / double(std::max<size_t>(1, ctx.tot)));
    r.nonZeroFrac = double(ctx.nz) / double(std::max<size_t>(1, ctx.tot));
    r.status = L.statusJson(nullptr); r.snr = L.snrDb(); r.dropped = L.droppedBlocks();
    return r;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--probe") {   // the child for the VIBE_FREEDV_DISABLE check
        std::printf("%d\n", fdv::available() ? 1 : 0);
        return 0;
    }
    const fdv::Api& a = fdv::api();
    if (!a.anySub()) { std::printf("NOT RUN — %s\n", a.why.c_str()); return 3; }
    std::printf("libcodec2 %s (FreeDV API version %d), sub-modes %s\n", a.path.c_str(), a.version, a.subsJson().c_str());
    if (!a.comptx) { std::printf("NOT RUN — this libcodec2 has no freedv_comptx (the test transmitter)\n"); return 3; }

    const int subs[] = {fdv::k700D, fdv::k700E, fdv::k1600};

    std::printf("\n── 1. each sub-mode chosen by hand, 10 dB, 20 s ──\n");
    for (int s : subs) {
        if (!a.sub[s]) continue;
        FreeDvListener L(s);
        const Run r = play(L, usbAudio(modulate(s, 20), 10, 1));
        std::printf("   %s: %s\n", fdv::subName(s), desc(r).c_str());
        check(r.locked == s, (std::string(fdv::subName(s)) + " in sync").c_str());
        check(r.speech > 50, (std::string(fdv::subName(s)) + " speech decoded").c_str(), std::to_string(r.speech) + " blocks");
        check(r.outRms > 0.01 && r.nonZeroFrac > 0.5, (std::string(fdv::subName(s)) + " audio comes out").c_str());
        check(r.status.find("\"state\":\"sync\"") != std::string::npos && r.status.find(std::string("\"mode\":\"") + fdv::subName(s) + "\"") != std::string::npos,
              (std::string(fdv::subName(s)) + " report names the mode").c_str(), r.status);
        check(r.status.find("\"text\":\"VIBE TEST\"") != std::string::npos, (std::string(fdv::subName(s)) + " text channel").c_str());
        { const size_t at = r.status.find("\"snr\":"); const int shown = at == std::string::npos ? -99 : std::atoi(r.status.c_str() + at + 6);
          check(std::fabs(shown - r.snr) <= 1.5f, (std::string(fdv::subName(s)) + " report's SNR follows the modem").c_str(), std::to_string(shown)); }
        check(r.dropped == 0, (std::string(fdv::subName(s)) + " nothing dropped").c_str());
        std::printf("   CPU: %.1f %% of real time on this machine (manual %s)\n", 100 * r.cpuPerSec, fdv::subName(s));
    }

    std::printf("\n── 2. AUTO finds each, 10 dB, 20 s ──\n");
    for (int s : subs) {
        if (!a.sub[s]) continue;
        FreeDvListener L(fdv::kAuto);
        const Run r = play(L, usbAudio(modulate(s, 20), 10, 2));
        std::printf("   on air %s: %s\n", fdv::subName(s), desc(r).c_str());
        check(r.locked == s, (std::string("AUTO names ") + fdv::subName(s)).c_str());
        check(r.open == 1, "the other sub-modes closed after the lock");
        check(r.status.find("\"sub\":\"auto\"") != std::string::npos, "the report says AUTO");
        check(r.speech > 40 && r.outRms > 0.01, "audio after AUTO's lock");
        std::printf("   CPU: %.1f %% of real time (AUTO, searching then one modem)\n", 100 * r.cpuPerSec);
    }

    std::printf("\n── 3. weak signals, manual ──\n");
    {
        struct W { int s; double snr; } ws[] = {{fdv::k700D, 4}, {fdv::k700E, 5}, {fdv::k1600, 5}};
        for (auto w : ws) {
            if (!a.sub[w.s]) continue;
            FreeDvListener L(w.s);
            const Run r = play(L, usbAudio(modulate(w.s, 20), w.snr, 3));
            std::printf("   %s at %.0f dB: %s\n", fdv::subName(w.s), w.snr, desc(r).c_str());
            check(r.locked == w.s && r.speech > 30, (std::string(fdv::subName(w.s)) + " holds sync at " + std::to_string(int(w.snr)) + " dB").c_str());
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
        check(r.locked == fdv::kAuto && r.status.find("\"state\":\"search\"") != std::string::npos, "never a lock on noise");
        check(r.outRms == 0.0, "silent on noise");
        check(r.open == 3 || r.open == int(a.sub[1]) + int(a.sub[2]) + int(a.sub[3]), "all sub-modes still searching");
        std::printf("   CPU: %.1f %% of real time (AUTO searching, three modems)\n", 100 * r.cpuPerSec);
    }

    std::printf("\n── 5. the server's path: IQ → RxPipeline USB → modem, off the dial centre, +25 Hz error ──\n");
    for (int s : subs) {
        if (!a.sub[s]) continue;
        const Run r = throughPipeline(s, fdv::kAuto, 8, 6000, 25, 20);
        std::printf("   %s: %s\n", fdv::subName(s), desc(r).c_str());
        check(r.locked == s && r.speech > 40 && r.outRms > 0.01, (std::string(fdv::subName(s)) + " through the real USB chain, AUTO").c_str());
    }

    std::printf("\n── 6. switching sub-mode while running ──\n");
    {
        FreeDvListener L(fdv::k1600);
        Run r = play(L, usbAudio(modulate(fdv::k700D, 8), 10, 4));
        check(r.locked == fdv::kAuto, "1600 chosen, 700D on air: no lock", desc(r));
        L.setSub(fdv::k700D);
        r = play(L, usbAudio(modulate(fdv::k700D, 12), 10, 6));
        check(r.locked == fdv::k700D && r.status.find("\"sub\":\"700D\"") != std::string::npos, "switched to 700D: in sync", desc(r));
    }

    std::printf("\n── 7. no library = no mode ──\n");
    {
        std::string cmd = std::string("VIBE_FREEDV_DISABLE=1 '") + argv[0] + "' --probe";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[16] = {0};
        if (p) { if (!std::fgets(buf, sizeof buf, p)) buf[0] = 0; pclose(p); }
        check(std::string(buf).rfind("0", 0) == 0, "VIBE_FREEDV_DISABLE=1 → not available", buf);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
