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
    int wins[4] = {0, 0, 0, 0};
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
    for (int k = 0; k < 4; ++k) r.wins[k] = L.winsOf(k);
    return r;
}

/** ★ The bench clip for scripts/test-server-freedv.mjs: unsigned 8-bit IQ (rtl_tcp's format) at `rate`, `sec` long, centred
 *  on the fake dongle's --hf-centre, with three stations — 700D at +10 kHz, 700E at +20 kHz, 1600 at −10 kHz — at `snr` dB in
 *  3 kHz each. The clip loops in the fake dongle, so each station's modem relocks once a loop (as a real station's overs do). */
static int writeClip(const char* path, int rate, double sec, double snrDb, bool lsb) {
    const fdv::Api& a = fdv::api();
    if (!a.anySub() || !a.comptx) { std::printf("NOT RUN — %s\n", a.anySub() ? "no freedv_comptx" : a.why.c_str()); return 3; }
    if (rate % 8000) { std::printf("the rate must be a multiple of 8 kHz\n"); return 2; }
    const int n = int(sec * rate);
    std::vector<cf> iq(static_cast<size_t>(n));
    struct St { int sub; double off; } st[] = {{fdv::k700D, 10000}, {fdv::k700E, 20000}, {fdv::k1600, -10000}};
    double ps = 0;
    for (auto t : st) {
        if (!a.sub[t.sub]) continue;
        const std::vector<cf> m8 = modulate(t.sub, sec + 1);   // (the same voice and text for each)
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
    std::printf("wrote %s: %d S/s, %.0f s, 700D +10 kHz, 700E +20 kHz, 1600 −10 kHz, %s, %.0f dB in 3 kHz\n", path, rate, sec, lsb ? "LSB" : "USB", snrDb);
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
        uint64_t v = 0; const std::string j = L.statusJson(&v);
        if (v != ver) { ver = v; std::printf("  %7.1f s  %s\n", double(i) / rate, j.c_str()); }
    }
    std::printf("speech blocks %llu, audible %.0f %% of the time\n", (unsigned long long)L.speechBlocks(), 100.0 * double(nz) / double(std::max<size_t>(1, x.size())));
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "wav") return playWav(argv[2], argc > 3 ? std::max(0, fdv::subFromName(argv[3])) : fdv::kAuto);
    if (argc > 1 && std::string(argv[1]) == "--probe") {   // the child for the VIBE_FREEDV_DISABLE check
        std::printf("%d\n", fdv::available() ? 1 : 0);
        return 0;
    }
    // test-freedv clip OUT.cu8 [rate=288000] [sec=20] [snr=12] [usb|lsb]
    if (argc > 2 && std::string(argv[1]) == "clip")
        return writeClip(argv[2], argc > 3 ? std::atoi(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 20, argc > 5 ? std::atof(argv[5]) : 12,
                         argc > 6 && std::string(argv[6]) == "lsb");
    const fdv::Api& a = fdv::api();
    if (!a.anySub()) { std::printf("NOT RUN — %s\n", a.why.c_str()); return 3; }
    std::printf("libcodec2 %s (FreeDV API version %d), sub-modes %s\n", a.path.c_str(), a.version, a.subsJson().c_str());
    if (!a.comptx) { std::printf("NOT RUN — this libcodec2 has no freedv_comptx (the test transmitter)\n"); return 3; }

    const int subs[] = {fdv::k700D, fdv::k700E, fdv::k1600};

    std::printf("\n── 0. the sideband rule (AUTO = the FreeDV application's; USB / LSB by hand) ──\n");
    check(fdv::lowerSideband(7.177e6, fdv::kSbAuto) && fdv::lowerSideband(3.643e6, fdv::kSbAuto), "AUTO: 40 m and 80 m LSB");
    check(!fdv::lowerSideband(14.236e6, fdv::kSbAuto) && !fdv::lowerSideband(10e6, fdv::kSbAuto), "AUTO: from 10 MHz up USB");
    check(!fdv::lowerSideband(5.3685e6, fdv::kSbAuto) && !fdv::lowerSideband(5.4035e6, fdv::kSbAuto), "AUTO: 60 m USB");
    check(!fdv::lowerSideband(7.177e6, fdv::kSbUsb) && fdv::lowerSideband(14.236e6, fdv::kSbLsb), "USB / LSB by hand win over the convention");
    check(fdv::sbFromName("LSB") == fdv::kSbLsb && fdv::sbFromName("auto") == fdv::kSbAuto && fdv::sbFromName("dsb") == -1, "sideband names");

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

    /* ★★ 2b. AUTO NEVER CHOOSES THE WRONG SUB-MODE (2026-11-16: test-server-freedv failed ~1 run in 4 on the 1600 station —
     *  AUTO in "sync" at 2 dB on something else, then silence). Each sub-mode alone, many times: a different noise lead-in
     *  (so the modems see the signal start at a different phase), the clip's own loop (the signal twice, end to start — the
     *  fake dongle's wrap), 8 and 12 dB. Every win must be the sub-mode on air. */
    std::printf("\n── 2b. AUTO on one sub-mode, many starts and the loop wrap: never a wrong win ──\n");
    {
        int runs = 0, wrong = 0, missed = 0;
        std::string why;
        for (int s : subs) {
            if (!a.sub[s]) continue;
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
                for (int o : subs) if (o != s) other += L.winsOf(o);
                if (other) { ++wrong; char b[160]; std::snprintf(b, sizeof b, " [%s k=%d %.0f dB: wins 700D %d 700E %d 1600 %d]", fdv::subName(s), k, snr,
                                                           L.winsOf(fdv::k700D), L.winsOf(fdv::k700E), L.winsOf(fdv::k1600)); why += b; }
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
            if (!a.sub[s]) continue;
            for (int k = 0; k < 8; ++k) {
                const Run r = throughPipeline(s, fdv::kAuto, (k & 1) ? 8 : 12, 6000 + 137 * k, -40 + 11 * k, 12);
                ++runs;
                int other = 0; for (int o : subs) if (o != s) other += r.wins[o];
                if (other) { ++wrong; char b[120]; std::snprintf(b, sizeof b, " [%s k=%d: wins %d/%d/%d]", fdv::subName(s), k, r.wins[1], r.wins[2], r.wins[3]); why += b; }
                if (r.locked != s) { ++missed; char b[80]; std::snprintf(b, sizeof b, " [%s k=%d ends %s]", fdv::subName(s), k, fdv::subName(r.locked)); why += b; }
            }
        }
        std::printf("   %d runs, %d with a wrong win, %d not in sync on the right one at the end%s\n", runs, wrong, missed, why.c_str());
        check(wrong == 0 && missed == 0, "through the USB chain: only ever the sub-mode on air", why);
    }

    /* ★★ 2c. A WINNER THAT DID NOT LAST searches again at once, not after 8 s: 700D for 2.5 s (AUTO wins it about 1.3 s
     *  in), then the same dial carries 1600. AUTO must be decoding 1600 within 4 s of the change. */
    std::printf("\n── 2c. a short-lived win: the full search again at once ──\n");
    if (a.sub[fdv::k700D] && a.sub[fdv::k1600]) {
        std::vector<float> d = usbAudio(modulate(fdv::k700D, 3), 12, 31), f = usbAudio(modulate(fdv::k1600, 12), 12, 32);
        d.resize(size_t(48000 * 2.5));
        std::vector<float> x = d; x.insert(x.end(), f.begin(), f.begin() + 48000 * 4);
        FreeDvListener L(fdv::kAuto);
        const Run r = play(L, x);
        std::printf("   %s  wins 700D %d 1600 %d, probation losses %d\n", desc(r).c_str(), L.winsOf(fdv::k700D), L.winsOf(fdv::k1600), L.probationLosses());
        check(L.winsOf(fdv::k700D) >= 1 && L.probationLosses() >= 1, "the 700D win that did not last went back to the search");
        check(r.locked == fdv::k1600, "…and AUTO is on 1600 within 4 s of the change (not 8 s + a search)");
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
