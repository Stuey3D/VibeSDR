// test-dab-faad.cpp — DAB+ audio: FAAD2 in process (vibe_dab_faad.h) against TODAY's FFmpeg child process, on REAL
// off-air access units — unit logs written by dab-unitlog from captured multiplexes, CRC failures and lost super frames
// included, so the comparison is made on exactly what the receiver produced on air. V12, 2026-10-26.
//
// Each decoder's output is put through the SERVICE's timeline exactly as DabService builds it:
//   ffmpeg  — today: only the good units go to `ffmpeg -f aac … -ar 48000` (the server's own arguments); its 1024-sample
//             output is paced at the measured rate; a hole is SPLICED (the neighbours join) and the clock is held by
//             silence once the deficit passes 240 ms (holdTimeline).
//   faad2   — vibe_dab_faad.h: every missing unit CONCEALED (the last good unit decoded again, fading out; silence;
//             the next good unit fading in), so the clock never needs filling.
//   faad2-splice — the same decoder fed the way FFmpeg is (spliced + silence fill): what the decoder alone changes.
//   ref960  — an independent check of the 960 transform: FFmpeg reading the units as LOAS/LATM, whose StreamMuxConfig
//             carries the AudioSpecificConfig with frameLengthFlag = 1. Measurement only — never a server path.
// Reported per service: level, agreement (SNR) with the 960 reference, stereo image, and on damaged logs the largest
// step, clicks, dips and gaps; start-up; CPU and memory. Exit 0 when the checks pass, 1 when one fails, 3 when no logs.
//   run: test-dab-faad [<log.dabu>…]  (none: every log in ~/VibeSDR-test-audio/dab/logs) (VIBE_DAB_FAAD_WAV=<prefix>: also writes <prefix>-<log>-<variant>.wav, 48 kHz 16-bit)
// ★★★ The logs are other people's broadcasts: they live in ~/VibeSDR-test-audio/dab/logs only, never in the repo. With
//     none given (or none found) the test says NOT RUN and exits 3.
#include "vibe_dab_aacdec.h"
#include "vibe_dab_faad.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <dirent.h>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

#if !defined(VIBE_HAVE_FAAD2_DAB)
int main() { std::printf("   NOT RUN — no plain FAAD2 (libfaad_dab) in this build\n"); return 3; }
#else
using namespace std::chrono;

struct Slot { uint32_t sf = 0; int slot = 0, slots = 0; bool ps = false; std::vector<uint8_t> adts; };
struct Log { std::vector<Slot> s; int slots = 0; bool ps = false; std::string name; };

static bool loadLog(const std::string& path, Log& L) {
    FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
    char m[8]; if (std::fread(m, 1, 8, f) != 8 || std::memcmp(m, "VIBEDABU", 8) != 0) { std::fclose(f); return false; }
    uint32_t sf; uint8_t h[4]; uint16_t len;
    while (std::fread(&sf, 4, 1, f) == 1 && std::fread(h, 1, 4, f) == 4 && std::fread(&len, 2, 1, f) == 1) {
        Slot s; s.sf = sf; s.slot = h[0]; s.slots = h[1]; s.ps = h[2] != 0; s.adts.resize(len);
        if (len && std::fread(s.adts.data(), 1, len, f) != len) break;
        L.s.push_back(std::move(s));
    }
    std::fclose(f);
    if (L.s.empty()) return false;
    L.slots = L.s.front().slots; L.ps = L.s.front().ps;
    const size_t sl = path.find_last_of('/'); L.name = path.substr(sl == std::string::npos ? 0 : sl + 1);
    if (L.name.size() > 5) L.name.resize(L.name.size() - 5);
    return true;
}

static double cpuChildren() { rusage r; getrusage(RUSAGE_CHILDREN, &r); return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1e-6; }
static double cpuThread() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static std::string tmpDir() { const char* t = getenv("TMPDIR"); return t ? t : "/tmp"; }
static const char* ffPath() {
    for (const char* c : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" }) if (::access(c, X_OK) == 0) return c;
    return "ffmpeg";
}
static bool runFf(const std::string& fmt, const std::vector<uint8_t>& in, std::vector<float>& out, double& cpu, long& rssKb) {
    const std::string a = tmpDir() + "/tdf-in.bin", b = tmpDir() + "/tdf-out.f32";
    FILE* f = std::fopen(a.c_str(), "wb"); std::fwrite(in.data(), 1, in.size(), f); std::fclose(f);
    const double c0 = cpuChildren();
    const std::string cmd = std::string("'") + ffPath() + "' -hide_banner -loglevel quiet -nostdin -f " + fmt + " -i '" + a +
                            "' -f f32le -ar 48000 -ac 2 -y '" + b + "'";
    const int rc = std::system(cmd.c_str());
    cpu = cpuChildren() - c0;
    rusage r; getrusage(RUSAGE_CHILDREN, &r); rssKb = r.ru_maxrss / 1024;   // macOS: bytes
    out.clear();
    if (FILE* g = std::fopen(b.c_str(), "rb")) { float buf[8192]; size_t n; while ((n = std::fread(buf, 4, 8192, g)) > 0) out.insert(out.end(), buf, buf + n); std::fclose(g); }
    std::remove(a.c_str()); std::remove(b.c_str());
    return rc == 0 && !out.empty();
}

/** ★ LOAS/LATM (ISO/IEC 14496-3 1.7) carrying the AudioSpecificConfig — frameLengthFlag = 1 — for the 960 reference. */
static void latmFrame(const uint8_t* au, size_t n, int aot, int sfi, int ch, std::vector<uint8_t>& o) {
    std::vector<uint8_t> e; uint32_t acc = 0; int nb = 0;
    auto put = [&](uint32_t v, int bits) { for (int i = bits - 1; i >= 0; --i) { acc = (acc << 1) | ((v >> i) & 1); if (++nb == 8) { e.push_back(uint8_t(acc)); acc = 0; nb = 0; } } };
    put(0, 1);                       // useSameStreamMux = 0: the config travels in every frame
    put(0, 1); put(1, 1); put(0, 6); put(0, 4); put(0, 3);   // audioMuxVersion 0, allStreamsSameTimeFraming, 1 subframe/program/layer
    put(uint32_t(aot), 5); put(uint32_t(sfi), 4); put(uint32_t(ch), 4); put(1, 1); put(0, 1); put(0, 1);   // ASC: 960, no core coder, no ext
    put(0, 3); put(0xFF, 8);         // frameLengthType 0, latmBufferFullness
    put(0, 1); put(0, 1);            // otherDataPresent, crcCheckPresent
    size_t left = n; while (left >= 255) { put(255, 8); left -= 255; } put(uint32_t(left), 8);
    for (size_t i = 0; i < n; ++i) put(au[i], 8);
    if (nb) put(0, 8 - nb);
    o.push_back(0x56); o.push_back(uint8_t(0xE0 | ((e.size() >> 8) & 0x1F))); o.push_back(uint8_t(e.size()));
    o.insert(o.end(), e.begin(), e.end());
}

/** The service's timeline: units in order, a hole either SPLICED (+ silence once 240 ms behind) or CONCEALED. */
struct Out { std::vector<float> pcm; double cpu = 0, startMs = 0; long rssKb = 0; int64_t concealed = 0, silenceFrames = 0;
             std::vector<size_t> events; bool inHole = false;   ///< output frame where each run of missing units begins
             void hole(size_t at) { if (!inHole) events.push_back(at); inHole = true; } };

static void holdClock(Out& o, uint64_t owed) {   // DabService::holdTimeline: fill only the excess over a 240 ms lag
    const uint64_t pushed = o.pcm.size() / 2, lag = 48000 * 240 / 1000;
    if (owed > pushed + lag) { const uint64_t need = owed - pushed - lag; o.pcm.insert(o.pcm.end(), size_t(need) * 2, 0.0f); o.silenceFrames += int64_t(need); }
}

/** Splice-style timeline from a stream decoded from the GOOD units only (`perUnit` 48 kHz frames each, in order). */
static Out spliceTimeline(const Log& L, const std::vector<float>& dec, double perUnit) {
    Out o; const uint32_t sf0 = L.s.front().sf; const double U = 5760.0 / L.slots;   // the unit's true length at 48 kHz
    // pace: the server plays perUnit frames as one unit's duration — resample dec by U/perUnit (windowed sinc)
    std::vector<float> paced;
    { const double step = perUnit / U; const int H = 16; const size_t n = dec.size() / 2;
      for (double t = H; t < double(n) - H; t += step) { const int i0 = int(t); double l = 0, r = 0, ws = 0; const double c = std::min(1.0, 1.0 / step);
        for (int k = -H + 1; k <= H; ++k) { const double x = t - double(i0 + k); const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x * c) / (M_PI * x * c); const double w = 0.5 + 0.5 * std::cos(M_PI * x / H); l += dec[2 * size_t(i0 + k)] * s * w; r += dec[2 * size_t(i0 + k) + 1] * s * w; ws += s * w; }
        paced.push_back(float(l / ws)); paced.push_back(float(r / ws)); } }
    size_t good = 0; uint32_t cur = sf0;
    for (const auto& s : L.s) {
        if (cur < s.sf) o.hole(o.pcm.size() / 2);
        while (cur < s.sf) { holdClock(o, uint64_t(cur - sf0 + 1) * 5760); ++cur; }
        if (s.adts.empty()) o.hole(o.pcm.size() / 2);
        else {
            o.inHole = false;
            const size_t a = size_t(std::llround(double(good) * U)), b = size_t(std::llround(double(good + 1) * U));
            if (b * 2 <= paced.size()) o.pcm.insert(o.pcm.end(), paced.begin() + long(a * 2), paced.begin() + long(b * 2));
            ++good;
        }
        if (s.slot == s.slots - 1) { holdClock(o, uint64_t(s.sf - sf0 + 1) * 5760); cur = s.sf + 1; }
    }
    return o;
}

static Out runFfmpegToday(const Log& L) {
    std::vector<uint8_t> in; size_t good = 0;
    for (const auto& s : L.s) if (!s.adts.empty()) { in.insert(in.end(), s.adts.begin(), s.adts.end()); ++good; }
    std::vector<float> dec; double cpu = 0; long rss = 0;
    if (!good || !runFf("aac", in, dec, cpu, rss)) return {};
    std::printf("   ffmpeg today: %.1f frames at 48 kHz per unit — paced to %.1f\n", double(dec.size() / 2) / double(good), 5760.0 / L.slots);
    Out o = spliceTimeline(L, dec, double(dec.size() / 2) / double(good));
    o.cpu = cpu; o.rssKb = rss;
    // start-up through the live pipe exactly as the server runs it
    vibedab::AacDecoderFfmpeg ff; vibedab::AacPcm p; const auto t0 = steady_clock::now();
    for (const auto& s : L.s) { if (s.adts.empty()) continue; ff.decode(s.adts.data(), s.adts.size(), p); std::this_thread::sleep_for(milliseconds(20)); ff.drain(p); if (!p.interleaved.empty()) break; }
    if (!p.interleaved.empty()) o.startMs = duration<double, std::milli>(steady_clock::now() - t0).count();
    ff.close();
    return o;
}

static Out runRef960(const Log& L) {
    std::vector<uint8_t> in; size_t good = 0;
    for (const auto& s : L.s) if (!s.adts.empty()) {
        const uint8_t* a = s.adts.data(); const size_t hdr = (a[1] & 1) ? 7 : 9;
        latmFrame(a + hdr, s.adts.size() - hdr, ((a[2] >> 6) & 3) + 1, (a[2] >> 2) & 15, ((a[2] & 1) << 2) | (a[3] >> 6), in); ++good;
    }
    std::vector<float> dec; double cpu = 0; long rss = 0;
    if (!good || !runFf("loas", in, dec, cpu, rss)) return {};
    // ★ a TRUE 960 decoder needs no pacing: perUnit should come out at U exactly — reported, then used as measured
    std::printf("   ref960: %.1f frames at 48 kHz per unit (a true 960 transform gives %.1f)\n", double(dec.size() / 2) / double(good), 5760.0 / L.slots);
    return spliceTimeline(L, dec, double(dec.size() / 2) / double(good));
}

static Out runFaad(const Log& L, bool conceal, int corruptEvery = 0) {
    Out o; vibedab::AacDecoderFaad f; vibedab::AacPcm p;
    if (const char* e = getenv("VIBE_DAB_FADE_MS")) f.setFadeMs(std::atoi(e));
    rusage r0; getrusage(RUSAGE_SELF, &r0);
    const double c0 = cpuThread(); const auto t0 = steady_clock::now(); bool first = true;
    const uint32_t sf0 = L.s.front().sf; int64_t unitsOut = -1, idxSeen = 0; uint32_t cur = sf0;
    for (const auto& s : L.s) {
        const int64_t idx = int64_t(s.sf - sf0) * L.slots + s.slot;
        std::vector<uint8_t> a = s.adts;
        if (corruptEvery && !a.empty() && (++idxSeen % corruptEvery) == 0) for (size_t k = 9; k < a.size(); k += 3) a[k] ^= 0x5A;   // ★ synthetic: bypasses the CRC
        if (conceal) {
            // ★ as DabService does it: a hole AHEAD of the clock is concealed; a slot behind it (a super frame the clock
            //   placed twice — a firecode that passed at the wrong phase) is not concealed twice
            if (unitsOut >= 0 && unitsOut < idx) { o.hole(p.interleaved.size() / 2); f.conceal(int(idx - unitsOut), p); unitsOut = idx; }
            if (a.empty()) { if (unitsOut >= 0 && idx >= unitsOut) { o.hole(p.interleaved.size() / 2); f.conceal(1, p); unitsOut = idx + 1; } continue; }
            o.inHole = false;
            f.decode(a.data(), a.size(), p); unitsOut = std::max(unitsOut, idx) + 1;
        } else {
            if (cur < s.sf || a.empty()) o.hole(p.interleaved.size() / 2);
            while (cur < s.sf) { o.pcm.swap(p.interleaved); holdClock(o, uint64_t(cur - sf0 + 1) * 5760); o.pcm.swap(p.interleaved); ++cur; }
            if (!a.empty()) { o.inHole = false; f.decode(a.data(), a.size(), p); }
            if (s.slot == s.slots - 1) { o.pcm.swap(p.interleaved); holdClock(o, uint64_t(s.sf - sf0 + 1) * 5760); o.pcm.swap(p.interleaved); cur = s.sf + 1; }
        }
        if (first && !p.interleaved.empty()) { o.startMs = duration<double, std::milli>(steady_clock::now() - t0).count(); first = false; }
    }
    o.cpu = cpuThread() - c0;
    rusage r1; getrusage(RUSAGE_SELF, &r1); o.rssKb = (r1.ru_maxrss - r0.ru_maxrss) / 1024;
    const int64_t sil = o.silenceFrames; o.pcm = std::move(p.interleaved); o.silenceFrames = sil;
    o.concealed = f.concealed();
    return o;
}

// ── measures ─────────────────────────────────────────────────────────────────────────────────────────────────────
static double rmsDb(const std::vector<float>& x, size_t a = 0, size_t b = SIZE_MAX) {
    b = std::min(b, x.size()); double e = 0; for (size_t i = a; i < b; ++i) e += double(x[i]) * x[i];
    return 10 * std::log10(e / double(std::max<size_t>(1, b - a)) + 1e-20);
}
static int bestLag(const std::vector<float>& a, const std::vector<float>& b, int maxLag) {
    const size_t N = std::min<size_t>(std::min(a.size(), b.size()) / 2, 48000 * 8);
    double best = -1e30; int bl = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0;
        for (size_t i = size_t(maxLag) + 48000; i + size_t(maxLag) < N; i += 2) { const size_t j = size_t(long(i) + lag); s += (a[2 * i] + a[2 * i + 1]) * double(b[2 * j] + b[2 * j + 1]); }
        if (s > best) { best = s; bl = lag; }
    }
    return bl;
}
/** SNR of `a` against reference `r` (aligned, gain-matched), over the whole overlap after the first second. */
static double snrAgainst(const std::vector<float>& a, const std::vector<float>& r, int& lagOut, double& gainDb) {
    const int lag = bestLag(a, r, 4800); lagOut = lag;
    double ar = 0, rr = 0, aa = 0;
    const size_t n = std::min(a.size() / 2, r.size() / 2);
    for (size_t i = 48000; i + 48000 < n; ++i) { const long j = long(i) + lag; if (j < 0 || size_t(j) >= r.size() / 2) continue;
        for (int c = 0; c < 2; ++c) { const double x = a[2 * i + size_t(c)], y = r[2 * size_t(j) + size_t(c)]; ar += x * y; rr += y * y; aa += x * x; } }
    const double g = ar / (rr + 1e-30); gainDb = 20 * std::log10(std::fabs(g) + 1e-30);
    double e = 0; for (size_t i = 48000; i + 48000 < n; ++i) { const long j = long(i) + lag; if (j < 0 || size_t(j) >= r.size() / 2) continue;
        for (int c = 0; c < 2; ++c) { const double d = a[2 * i + size_t(c)] - g * r[2 * size_t(j) + size_t(c)]; e += d * d; } }
    return 10 * std::log10(aa / (e + 1e-30));
}
/** ★ SPECTRAL agreement — the right yardstick for HE-AAC. SBR fills its high band partly with NOISE and PS synthesises
 *  the side signal, so two correct decoders need not agree sample for sample; what must agree is the spectrum. Long-term
 *  third-octave levels (63 Hz – 16 kHz), worst band difference; and the short-time log-spectral distance (24 bands, 21 ms
 *  frames, the median and 95th percentile) after alignment. */
static void fftR(std::vector<std::complex<double>>& x) {
    const size_t N = x.size();
    for (size_t i = 1, j = 0; i < N; ++i) { size_t bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(x[i], x[j]); }
    for (size_t len = 2; len <= N; len <<= 1) { const auto w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < N; i += len) { std::complex<double> wn(1); for (size_t k = 0; k < len / 2; ++k) { auto u = x[i + k], v = x[i + k + len / 2] * wn; x[i + k] = u + v; x[i + k + len / 2] = u - v; wn *= w; } } }
}
static std::vector<std::vector<double>> bandFrames(const std::vector<float>& st, long lag, std::vector<double>& longTerm) {
    const size_t N = 1024; std::vector<std::vector<double>> fr; longTerm.assign(28, 0.0);
    std::vector<std::complex<double>> x(N);
    const size_t n = st.size() / 2;
    for (size_t at = 48000; at + N + 48000 < n; at += N) {
        const long s0 = long(at) + lag; if (s0 < 0 || size_t(s0) + N > n) continue;
        for (size_t i = 0; i < N; ++i) x[i] = (st[2 * (size_t(s0) + i)] + st[2 * (size_t(s0) + i) + 1]) * (0.5 - 0.5 * std::cos(2 * M_PI * double(i) / N));
        fftR(x);
        std::vector<double> b(28, 1e-12);
        for (size_t k = 1; k < N / 2; ++k) { const double f = double(k) * 48000.0 / N; const int bi = int(std::floor(3 * std::log2(f / 50.0))); if (bi >= 0 && bi < 28) b[size_t(bi)] += std::norm(x[k]); }
        for (int i = 0; i < 28; ++i) longTerm[size_t(i)] += b[size_t(i)];
        fr.push_back(b);
    }
    return fr;
}
static void spectralAgreement(const std::vector<float>& a, const std::vector<float>& r, double& worstBandDb, double& lsdMed, double& lsd95) {
    int lag = bestLag(a, r, 4800);
    std::vector<double> la, lr; auto fa = bandFrames(a, 0, la), fr = bandFrames(r, lag, lr);
    worstBandDb = 0; for (int i = 0; i < 25; ++i) worstBandDb = std::max(worstBandDb, std::fabs(10 * std::log10((la[size_t(i)] + 1e-9) / (lr[size_t(i)] + 1e-9))));   // to 16 kHz
    std::vector<double> d; const size_t m = std::min(fa.size(), fr.size());
    for (size_t k = 0; k < m; ++k) { double s = 0; int c = 0; for (int i = 0; i < 25; ++i) { if (fr[k][size_t(i)] < 1e-6 && fa[k][size_t(i)] < 1e-6) continue; const double q = 10 * std::log10((fa[k][size_t(i)] + 1e-9) / (fr[k][size_t(i)] + 1e-9)); s += q * q; ++c; } if (c) d.push_back(std::sqrt(s / c)); }
    if (d.empty()) { lsdMed = lsd95 = 0; return; }
    std::sort(d.begin(), d.end()); lsdMed = d[d.size() / 2]; lsd95 = d[size_t(0.95 * double(d.size() - 1))];
}
static void stereo(const std::vector<float>& st, double& corr, double& sideDb) {
    double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
    for (size_t i = 0; i + 1 < st.size(); i += 2) { const double l = st[i], r = st[i + 1]; ll += l * l; rr += r * r; lr += l * r; mm += (l + r) * (l + r); ss += (l - r) * (l - r); }
    corr = lr / std::sqrt(ll * rr + 1e-30); sideDb = 10 * std::log10((ss + 1e-30) / (mm + 1e-30));
}
/** ★ What a listener hears as damage, without a clean copy to compare against (real errors have none):
 *   step  — the largest sample-to-sample jump (99.999th percentile), against the same measure of the clean stretch;
 *   clicks — 1 ms windows whose second-difference energy stands 20 dB above the median of the 200 ms around them;
 *   dips  — 10 ms windows 20 dB under the median of the 500 ms around them, where that median is programme;
 *   gaps  — runs of digital silence of 10 ms or more. */
struct Damage { double step = 0; int clicks = 0, dips = 0, gaps = 0; double gapSec = 0; };
static Damage damage(const std::vector<float>& p) {
    Damage d; const size_t n = p.size() / 2;
    { std::vector<float> s; for (size_t i = 1; i < n; ++i) s.push_back(std::max(std::fabs(p[2 * i] - p[2 * i - 2]), std::fabs(p[2 * i + 1] - p[2 * i - 1])));
      if (!s.empty()) { std::sort(s.begin(), s.end()); d.step = s[size_t(0.99999 * double(s.size() - 1))]; } }
    const size_t W1 = 48; std::vector<double> hf(n / W1, 0.0);
    for (size_t w = 0; w < hf.size(); ++w) for (size_t k = 0; k < W1; ++k) { const size_t i = w * W1 + k; if (i < 2) continue;
        for (int c = 0; c < 2; ++c) { const double x = p[2 * i + size_t(c)] - 2 * p[2 * (i - 1) + size_t(c)] + p[2 * (i - 2) + size_t(c)]; hf[w] += x * x; } }
    for (size_t w = 100; w + 100 < hf.size(); ++w) { std::vector<double> nb(hf.begin() + long(w) - 100, hf.begin() + long(w) + 100);
        std::nth_element(nb.begin(), nb.begin() + 100, nb.end()); if (hf[w] > 100.0 * nb[100] + 1e-9) { ++d.clicks; w += 10; } }
    const size_t W = 480; std::vector<double> en(n / W, 0.0);
    for (size_t w = 0; w < en.size(); ++w) for (size_t k = 0; k < W; ++k) { const size_t i = w * W + k; en[w] += double(p[2 * i]) * p[2 * i] + double(p[2 * i + 1]) * p[2 * i + 1]; }
    for (size_t w = 25; w + 25 < en.size(); ++w) { std::vector<double> nb(en.begin() + long(w) - 25, en.begin() + long(w) + 25);
        std::nth_element(nb.begin(), nb.begin() + 25, nb.end()); if (nb[25] > 1e-5 * W && en[w] < 0.01 * nb[25]) ++d.dips; }
    size_t run = 0; for (size_t i = 0; i < n; ++i) { if (p[2 * i] == 0.0f && p[2 * i + 1] == 0.0f) ++run; else { if (run >= 480) { ++d.gaps; d.gapSec += double(run) / 48000.0; } run = 0; } }
    if (run >= 480) { ++d.gaps; d.gapSec += double(run) / 48000.0; }
    return d;
}
/** ★★ AT EACH HOLE — where a run of missing units begins in that decoder's output. click: the largest second
 *  difference within ±4 ms of it, against the RMS second difference of the 200 ms before (dB); "audible" above 15 dB.
 *  dip: the quietest 10 ms in the 200 ms after it, against the 200 ms before (dB, 0 = no dip). Only holes with
 *  programme before them count (a hole after silence measures nothing). */
struct EventStats { int n = 0, audible = 0; double clickMed = 0, clickWorst = -99, dipMed = 0, dipWorst = 0; };
static EventStats atHoles(const std::vector<float>& p, const std::vector<size_t>& ev) {
    EventStats st; std::vector<double> clk, dip; const size_t n = p.size() / 2;
    auto d2 = [&](size_t i) { double m = 0; for (int c = 0; c < 2; ++c) m = std::max(m, std::fabs(double(p[2 * i + size_t(c)]) - 2 * p[2 * (i - 1) + size_t(c)] + p[2 * (i - 2) + size_t(c)])); return m; };
    for (size_t e : ev) {
        if (e < 9700 || e + 9700 >= n) continue;
        double ref = 0, before = 0; for (size_t i = e - 9600; i < e - 200; ++i) { const double v = d2(i); ref += v * v; before += double(p[2 * i]) * p[2 * i] + double(p[2 * i + 1]) * p[2 * i + 1]; }
        ref = std::sqrt(ref / 9400); before /= 9400;
        if (before < 1e-7) continue;
        double pk = 0; for (size_t i = e - 192; i < e + 192; ++i) pk = std::max(pk, d2(i));
        double lo = 1e30; for (size_t w = e; w + 480 < e + 9600; w += 240) { double en = 0; for (size_t i = w; i < w + 480; ++i) en += double(p[2 * i]) * p[2 * i] + double(p[2 * i + 1]) * p[2 * i + 1]; lo = std::min(lo, en / 480); }
        const double c = 20 * std::log10(pk / (ref + 1e-12) + 1e-12), d = std::min(0.0, 10 * std::log10((lo + 1e-12) / before));
        clk.push_back(c); dip.push_back(d); if (c > 15) ++st.audible;
        if (getenv("VIBE_DAB_EVENTS") && c > 12) std::printf("      event at %.3f s: click %+.1f dB, dip %.1f dB\n", double(e) / 48000.0, c, d);
    }
    st.n = int(clk.size());
    if (!clk.empty()) { std::sort(clk.begin(), clk.end()); std::sort(dip.begin(), dip.end());
        st.clickMed = clk[clk.size() / 2]; st.clickWorst = clk.back(); st.dipMed = dip[dip.size() / 2]; st.dipWorst = dip.front(); }
    return st;
}
/** ★ Hard edges: a sample step of more than 0.02 into or out of a run of digital silence of 10 ms or more — the
 *  sound of audio stopping dead or starting from nothing. */
static int abruptEdges(const std::vector<float>& p) {
    const size_t n = p.size() / 2; int edges = 0; size_t run = 0;
    for (size_t i = 0; i < n; ++i) {
        const bool z = p[2 * i] == 0.0f && p[2 * i + 1] == 0.0f;
        if (z) { if (run == 0 && i > 0 && std::max(std::fabs(p[2 * i - 2]), std::fabs(p[2 * i - 1])) > 0.02f) {
                     size_t k = i; while (k < n && p[2 * k] == 0.0f && p[2 * k + 1] == 0.0f) ++k; if (k - i >= 480) ++edges; }
                 ++run; }
        else { if (run >= 480 && std::max(std::fabs(p[2 * i]), std::fabs(p[2 * i + 1])) > 0.02f) ++edges; run = 0; }
    }
    return edges;
}
static void writeWav(const std::string& path, const std::vector<float>& lr) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const uint32_t n = uint32_t(lr.size()), data = n * 2, rate = 48000, byteRate = rate * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(rate);
    u32(byteRate); u16(4); u16(16); std::fwrite("data", 1, 4, f); u32(data);
    for (float x : lr) { const float c = x > 1.f ? 1.f : (x < -1.f ? -1.f : x); u16(uint16_t(int16_t(std::lrint(c * 32767.f)))); }
    std::fclose(f);
}

static int checks = 0, failed = 0;
static void check(bool ok, const std::string& what) { ++checks; if (!ok) ++failed; std::printf("   %s %s\n", ok ? "ok  " : "FAIL", what.c_str()); }

int main(int argc, char** argv) {
    /* ★ With no arguments (scripts/run-tests.sh): every log in ~/VibeSDR-test-audio/dab/logs — the real recordings live
     *  only on Stuart's Mac — and NOT RUN (3) where there are none. */
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) paths.push_back(argv[i]);
    if (paths.empty()) {
        const char* home = getenv("HOME"); const std::string dir = std::string(home ? home : "") + "/VibeSDR-test-audio/dab/logs";
        if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) { const std::string n = e->d_name; if (n.size() > 5 && n.substr(n.size() - 5) == ".dabu") paths.push_back(dir + "/" + n); } closedir(d); }
        std::sort(paths.begin(), paths.end());
    }
    if (paths.empty()) { std::printf("   NOT RUN — no unit logs (dab-unitlog makes them from a capture; they stay in ~/VibeSDR-test-audio/dab/logs)\n"); return 3; }
    if (!vibedab::AacDecoderFfmpeg().available()) { std::printf("   NOT RUN — no FFmpeg to compare against\n"); return 3; }
    int ran = 0;
    for (const std::string& path : paths) {
        Log L; if (!loadLog(path, L)) { std::printf("   %s: not a unit log, skipped\n", path.c_str()); continue; }
        ++ran;
        size_t bad = 0;
        { std::map<std::pair<uint32_t, int>, bool> good; for (auto& s : L.s) { bool& g = good[{s.sf, s.slot}]; g = g || !s.adts.empty(); }
          for (auto& kv : good) bad += !kv.second; }
        const uint32_t sfs = L.s.back().sf - L.s.front().sf + 1;
        size_t present = 0; { std::map<std::pair<uint32_t, int>, int> seen; for (auto& s : L.s) seen[{s.sf, s.slot}] = 1; present = seen.size(); }
        const size_t due = size_t(sfs) * size_t(L.slots), missing = due - present + bad;   // ★ a slot logged twice counts once
        const double secs = sfs * 0.12;
        std::printf("\n══ %s: %d units per super frame%s, %.1f s — %zu units due, %zu missing (%zu CRC, %zu in lost super frames)\n",
                    L.name.c_str(), L.slots, L.ps ? ", parametric stereo" : "", secs, due, missing, bad, missing - bad);
        Out ff = runFfmpegToday(L), ref = runRef960(L), fa = runFaad(L, true), fs = runFaad(L, false);
        if (ff.pcm.empty() || fa.pcm.empty()) { std::printf("   (a decoder produced nothing)\n"); continue; }
        const double U = 5760.0 / L.slots;
        std::printf("   timeline: due %.0f frames; ffmpeg %+.0f ms (silence %.2f s) | faad2 %+.0f ms (%lld concealed) | faad2-splice %+.0f ms\n",
                    secs * 48000, (double(ff.pcm.size() / 2) - secs * 48000) / 48.0, ff.silenceFrames / 48000.0,
                    (double(fa.pcm.size() / 2) - secs * 48000) / 48.0, (long long)fa.concealed, (double(fs.pcm.size() / 2) - secs * 48000) / 48.0);
        (void)U;
        std::printf("   level (RMS): ffmpeg %.2f dB | faad2 %.2f dB | ref960 %.2f dB\n", rmsDb(ff.pcm), rmsDb(fa.pcm), ref.pcm.empty() ? 0.0 : rmsDb(ref.pcm));
        if (!ref.pcm.empty() && missing * 50 <= due) {
            int l1, l2, l3; double g1, g2, g3;
            // ★ faad2-splice: the SAME timeline as the reference (holes spliced), so this compares the decoders and nothing else
            const double s1 = snrAgainst(fs.pcm, ref.pcm, l1, g1), s2 = snrAgainst(ff.pcm, ref.pcm, l2, g2), s3 = snrAgainst(ff.pcm, fs.pcm, l3, g3);
            std::printf("   waveform (gain-matched SNR): faad2 vs ref960 %.1f dB (gain %+.2f dB) | ffmpeg-today vs ref960 %.1f dB (gain %+.2f dB) | ffmpeg-today vs faad2 %.1f dB\n",
                        s1, g1, s2, g2, s3);
            double w1, m1, p1, w2, m2, p2; spectralAgreement(fs.pcm, ref.pcm, w1, m1, p1); spectralAgreement(ff.pcm, ref.pcm, w2, m2, p2);
            std::printf("   spectrum vs ref960: faad2 worst third-octave %.2f dB, short-time distance median %.2f / p95 %.2f dB | ffmpeg-today %.2f dB, %.2f / %.2f dB\n",
                        w1, m1, p1, w2, m2, p2);
            (void)l1; (void)l2; (void)l3; (void)g3;
        }
        double c1, sd1, c2, sd2, c3 = 0, sd3 = 0; stereo(fs.pcm, c1, sd1); stereo(ff.pcm, c2, sd2); if (!ref.pcm.empty()) stereo(ref.pcm, c3, sd3);
        std::printf("   stereo: L/R correlation faad2 %.3f / ffmpeg %.3f / ref960 %.3f; side/mid faad2 %.2f dB / ffmpeg %.2f dB / ref960 %.2f dB\n", c1, c2, c3, sd1, sd2, sd3);
        // ★★ THE GATE, as far as numbers can hold it (Stuart listens to the rest): FAAD2 must be the true 960 decode in
        //    level, spectrum and stereo image where the log is clean, never stop or start dead where it is not, and cost less.
        if (!ref.pcm.empty() && missing * 50 <= due) {
            double w1, m1, p1; spectralAgreement(fs.pcm, ref.pcm, w1, m1, p1);
            double c3 = 0, sd3 = 0, c1b = 0, sd1b = 0; stereo(ref.pcm, c3, sd3); stereo(fs.pcm, c1b, sd1b);
            check(std::fabs(rmsDb(fs.pcm) - rmsDb(ref.pcm)) <= 0.15, "level within 0.15 dB of the 960 reference");
            check(w1 <= 1.5, "long-term spectrum within 1.5 dB of the 960 reference in every third octave to 16 kHz");
            check(std::fabs(sd1b - sd3) <= 1.0, "stereo image (side/mid) within 1 dB of the 960 reference");
        }
        { const int ea = abruptEdges(fa.pcm), ef = abruptEdges(ff.pcm);
          std::printf("   hard edges into/out of silence: ffmpeg %d | faad2 %d\n", ef, ea);
          check(ea == 0, "FAAD2 never stops or starts dead (no hard edge into or out of silence)"); }
        check(fa.cpu < ff.cpu, "FAAD2 costs less CPU than the FFmpeg child");
        const Damage dff = damage(ff.pcm), dfa = damage(fa.pcm), dfs = damage(fs.pcm);
        { const EventStats a = atHoles(ff.pcm, ff.events), b = atHoles(fa.pcm, fa.events), c = atHoles(fs.pcm, fs.events);
          auto pr = [](const char* nm, const EventStats& e) { std::printf("   at holes %-13s %3d holes: click median %+5.1f dB worst %+5.1f dB, %d over 15 dB; dip median %5.1f dB worst %5.1f dB\n", nm, e.n, e.clickMed, e.clickWorst, e.audible, e.dipMed, e.dipWorst); };
          if (missing) { pr("ffmpeg", a); pr("faad2", b); pr("faad2-splice", c); } }
        std::printf("   damage: step   ffmpeg %.4f | faad2 %.4f | faad2-splice %.4f\n", dff.step, dfa.step, dfs.step);
        std::printf("           clicks ffmpeg %d | faad2 %d | faad2-splice %d\n", dff.clicks, dfa.clicks, dfs.clicks);
        std::printf("           dips   ffmpeg %d | faad2 %d | faad2-splice %d\n", dff.dips, dfa.dips, dfs.dips);
        std::printf("           gaps   ffmpeg %d (%.2f s) | faad2 %d (%.2f s) | faad2-splice %d (%.2f s)\n", dff.gaps, dff.gapSec, dfa.gaps, dfa.gapSec, dfs.gaps, dfs.gapSec);
        if (getenv("VIBE_DAB_FAAD_SYNTH")) {   // ★ 1 unit in 10 corrupted AFTER the CRC — harsher than air (the CRC removes these)
            const Out fx = runFaad(L, true, 10); const Damage dx = damage(fx.pcm);
            std::printf("   synthetic 1-in-10 corrupted past the CRC: faad2 step %.4f, clicks %d, dips %d\n", dx.step, dx.clicks, dx.dips);
        }
        std::printf("   start-up: first PCM after %.1f ms (faad2) vs %.1f ms (ffmpeg live pipe)\n", fa.startMs, ff.startMs);
        std::printf("   CPU for %.1f s: faad2 %.3f s (%.2f %% of a core), ffmpeg %.3f s (%.2f %%); memory: faad2 +%ld KB in process, ffmpeg child peak RSS %ld KB\n",
                    secs, fa.cpu, 100 * fa.cpu / secs, ff.cpu, 100 * ff.cpu / secs, fa.rssKb, ff.rssKb);
        if (const char* wp = getenv("VIBE_DAB_FAAD_WAV")) {
            const std::string b = std::string(wp) + "-" + L.name + "-";
            writeWav(b + "ffmpeg-today.wav", ff.pcm); writeWav(b + "faad2.wav", fa.pcm); writeWav(b + "faad2-splice.wav", fs.pcm);
            if (!ref.pcm.empty()) writeWav(b + "ref960.wav", ref.pcm);
        }
    }
    if (!ran) { std::printf("   NOT RUN — no readable unit logs\n"); return 3; }
    std::printf("\ntest-dab-faad: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
#endif
