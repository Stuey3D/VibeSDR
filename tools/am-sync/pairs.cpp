// pairs — which stations carry the SAME programme? (helpers shared with am_replay.cpp)
// pairs <secs> <wav:centre:station>…   — SAM on each, 8 kHz, max |correlation| over ±300 ms for every pair.
// (orig header follows)
// am_replay — REAL recordings through the real RxPipeline in every AM-family mode, with an objective score.
//
//   am_replay <iqA.wav> <centreA Hz> <stationA Hz> <iqB.wav> <centreB Hz> <stationB Hz> <seconds> <outdir> [modes…]
//
// Station A is the one judged. Station B is the SAME PROGRAMME on another frequency, recorded at the same moment
// (two UberSDR iq192 captures started together, or one capture holding both) — an independent path with independent
// fading and noise. ★★ That is the reference: magnitude-squared coherence γ²(f) between A's demodulated audio and
// B's (SAM) audio, averaged over 300-3500 Hz, is a per-window S/N figure (γ²/(1−γ²)) that needs no clean original,
// is blind to level, AGC setpoint and filter phase, and drops with distortion, noise and interference alike. The
// SAME reference scores every mode of A, so the modes are compared like for like.
// Also printed per window: A's carrier depth (carrier vs its own median, from a 25 Hz low-pass of the channel) so
// the scores can be split into "carrier fading" and "not".
// Modes: am, am+a, sam, sam+a, sau, sal, ecssu, ecssl (+a = auto bandwidth on; default for the others is off).
// WAVs: <outdir>/<tag>_<mode>.wav, 48 kHz mono 16-bit, every file RMS-matched (−20 dBFS) so an A/B is not won by
// loudness. ★ SILENT: nothing is played.
#include "vibedsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace vibedsp;
using M = RxPipeline::Mode;

static std::vector<cf32> readIq(const char* path, double secs, int& rate) {
    FILE* f = std::fopen(path, "rb"); if (!f) { std::perror(path); std::exit(1); }
    unsigned char h[44]; if (std::fread(h, 1, 44, f) != 44) std::exit(1);
    rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
    const size_t n = (size_t)(secs * rate);
    std::vector<short> b(2 * n);
    const size_t got = std::fread(b.data(), 4, n, f);
    std::fclose(f);
    std::vector<cf32> z(got);
    for (size_t i = 0; i < got; ++i) z[i] = cf32(b[2 * i] / 32768.f, b[2 * i + 1] / 32768.f);
    return z;
}
struct Sink { std::vector<float> mono; std::vector<float> st; };
static void audioCb(void* c, const float* pcm, int frames, int ch, int) {
    auto* s = (Sink*)c;
    for (int i = 0; i < frames; ++i) { s->mono.push_back(ch == 2 ? 0.5f * (pcm[2 * i] + pcm[2 * i + 1]) : pcm[i]);
                                       if (ch == 2) { s->st.push_back(pcm[2 * i]); s->st.push_back(pcm[2 * i + 1]); } }
}
struct ModeSpec { const char* id; M mode; bool autoBw; double bw; };
static const ModeSpec kModes[] = {
    { "am", M::AM, false, 10000 }, { "am+a", M::AM, true, 10000 }, { "sam", M::SAM, false, 10000 }, { "sam+a", M::SAM, true, 10000 },
    { "sau", M::SAM_U, false, 10000 }, { "sal", M::SAM_L, false, 10000 }, { "sau+a", M::SAM_U, true, 10000 }, { "sal+a", M::SAM_L, true, 10000 },
    { "ecssu", M::ECSS_U, false, 5000 }, { "ecssl", M::ECSS_L, false, 5000 }, { "amst", M::AM_STEREO, false, 10000 } };

static std::vector<float> run(const std::vector<cf32>& z, int rate, double off, const ModeSpec& m, SyncAm::Status* last,
                              std::vector<float>* st = nullptr) {
    Sink s; RxPipeline::Callbacks cb; cb.ctx = &s; cb.audio = audioCb;
    RxPipeline p; p.setMeasureThread(false);
    p.start(rate, 1024, 20.0, 48000, cb);
    p.setAmAutoBandwidth(m.autoBw);
    p.setTune(off, m.mode, m.bw);
    const int blk = rate / 50;
    for (size_t i = 0; i < z.size(); i += blk) p.feed(z.data() + i, (int)std::min<size_t>(blk, z.size() - i));
    if (last) *last = p.amSyncStatus();
    p.stop();
    if (st) *st = s.st;
    return s.mono;
}
static void writeWav(const std::string& path, const std::vector<float>& x, int ch = 1) {
    double e = 0; for (float v : x) e += (double)v * v;
    const double rms = std::sqrt(e / std::max<size_t>(1, x.size()));
    const float g = rms > 0 ? (float)(0.1 / rms) : 1.0f;           // −20 dBFS
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const unsigned n = (unsigned)x.size(), bytes = n * 2, rate = 48000;
    auto u32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16((unsigned short)ch);
    u32(rate); u32(rate * 2 * ch); u16((unsigned short)(2 * ch)); u16(16); std::fwrite("data", 1, 4, f); u32(bytes);
    for (float v : x) { const float c = std::max(-1.f, std::min(1.f, v * g)); const short s = (short)std::lrint(c * 32767.f); std::fwrite(&s, 2, 1, f); }
    std::fclose(f);
}
// 48 kHz → 8 kHz
static std::vector<float> to8k(const std::vector<float>& x) {
    RealFir lp(designLowpass(3800.0 / 48000.0, 400.0 / 48000.0, true), 6);
    std::vector<float> y(x.size() / 6 + 2);
    const int n = lp.process(x.data(), (int)x.size(), y.data());
    y.resize(n); return y;
}
// lag of b relative to a (samples at 8 k), ±maxLag
static int bestLag(const std::vector<float>& a, const std::vector<float>& b, int maxLag) {
    const size_t n = std::min(a.size(), b.size()) - maxLag - 1;
    double best = -1; int bl = 0;
    for (int L = -maxLag; L <= maxLag; ++L) {
        double s = 0, ea = 0, eb = 0;
        for (size_t i = maxLag; i < n; i += 2) { const float x = a[i], y = b[i + L]; s += x * y; ea += x * x; eb += y * y; }
        const double c = std::fabs(s) / std::sqrt(ea * eb + 1e-30);
        if (c > best) { best = c; bl = L; }
    }
    std::fprintf(stderr, "  lag %+d samples @8k (%.1f ms), peak correlation %.3f\n", bl, bl / 8.0, best);
    return bl;
}
// mean γ² over 300-3500 Hz in one window [i0, i0+len) of a vs b shifted by lag
static double coherence(const std::vector<float>& a, const std::vector<float>& b, size_t i0, size_t len, int lag) {
    const int N = 512; static ComplexFFT fft(N); static std::vector<cf32> in(N), A(N), B(N);
    std::vector<cf32> sab(N / 2, cf32(0, 0)); std::vector<double> saa(N / 2, 0), sbb(N / 2, 0);
    for (size_t s = i0; s + N <= i0 + len; s += N / 2) {
        if ((long)s + lag < 0 || s + lag + N > b.size() || s + N > a.size()) break;
        for (int k = 0; k < N; ++k) in[k] = cf32(a[s + k] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * k / N)), 0);
        fft.forward(in.data(), A.data());
        for (int k = 0; k < N; ++k) in[k] = cf32(b[s + lag + k] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * k / N)), 0);
        fft.forward(in.data(), B.data());
        for (int k = 0; k < N / 2; ++k) { sab[k] += A[k] * std::conj(B[k]); saa[k] += std::norm(A[k]); sbb[k] += std::norm(B[k]); }
    }
    double g = 0; int c = 0;
    for (int k = (int)(300.0 / (8000.0 / N)); k <= (int)(3500.0 / (8000.0 / N)); ++k) { g += std::norm(sab[k]) / (saa[k] * sbb[k] + 1e-30); ++c; }
    return g / std::max(1, c);
}
// carrier depth per window: carrier power (25 Hz LPF of the channel at the station) over its median
static std::vector<double> carrierDb(const std::vector<cf32>& z, int rate, double off, double winSec, int nWin, double t0) {
    std::vector<double> out;
    const double a = 1 - std::exp(-2 * M_PI * 25.0 / rate);
    cf32 s1(0, 0), s2(0, 0); double ph = 0; const double w = -2 * M_PI * off / rate;
    std::vector<double> lvl; double acc = 0; size_t cnt = 0; const size_t per = (size_t)(winSec * rate);
    for (size_t i = 0; i < z.size(); ++i) {
        const cf32 v = z[i] * cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w; if (ph < -M_PI) ph += 2 * M_PI;
        s1 += (float)a * (v - s1); s2 += (float)a * (s1 - s2);
        if (i >= (size_t)(t0 * rate)) { acc += std::norm(s2); if (++cnt == per) { lvl.push_back(acc / cnt); acc = 0; cnt = 0; } }
    }
    std::vector<double> srt = lvl; std::sort(srt.begin(), srt.end());
    const double med = srt.empty() ? 1 : srt[srt.size() / 2];
    for (int k = 0; k < nWin && k < (int)lvl.size(); ++k) out.push_back(10 * std::log10(lvl[k] / med + 1e-30));
    return out;
}

int main(int argc, char** argv) {
    const double secs = std::atof(argv[1]);
    std::vector<std::vector<float>> au; std::vector<std::string> names;
    std::map<std::string, std::vector<cf32>> cache; std::map<std::string, int> rates;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i]; const size_t p1 = a.find(':'), p2 = a.find(':', p1 + 1);
        const std::string w = a.substr(0, p1); const double c = std::atof(a.substr(p1 + 1, p2 - p1 - 1).c_str()), st = std::atof(a.substr(p2 + 1).c_str());
        if (!cache.count(w)) { int r; cache[w] = readIq(w.c_str(), secs, r); rates[w] = r; }
        SyncAm::Status s;
        au.push_back(to8k(run(cache[w], rates[w], st - c, kModes[2], &s)));
        char b[64]; std::snprintf(b, sizeof b, "%.0f", st / 1e3); names.push_back(b);
        std::fprintf(stderr, "%s: locked %d off %+.1f\n", b, s.locked, s.offsetHz);
    }
    for (size_t i = 0; i < au.size(); ++i) for (size_t j = i + 1; j < au.size(); ++j) {
        std::fprintf(stderr, "%s x %s:", names[i].c_str(), names[j].c_str());
        bestLag(au[i], au[j], 2400);
    }
}
