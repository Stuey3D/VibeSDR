// test-dab-faad.cpp — DAB+ audio: FAAD2 in process (vibe_dab_faad.h) against TODAY's FFmpeg child process
// (vibe_dab_aacdec.h AacDecoderFfmpeg, exactly as the server runs it), on the SAME access units from the benchmark's
// multiplex (bench-clip/*.vbu8 through the real DabService). The gate Stuart set before FAAD2 may replace FFmpeg:
//   1. clean frames: level, SNR between the two (after the alignment and the speed correction the server applies),
//      and the tones' frequencies against each other;
//   2. SBR/PS services the same, stereo image included;
//   3. damaged units (1 in 10 corrupted): clicks (largest sample step), dips, gaps;
//   4. start-up latency (first PCM after the first unit), CPU and memory per decoder.
// It REPORTS; it does not decide. Exit 0 always when it ran (the numbers are the result), 3 when it could not.
//   build: cmake --build … --target test-dab-faad;  run: test-dab-faad <clip.vbu8> [impaired.vbu8]
#if !defined(VIBE_DAB_FAAD_FILES)
#include "vibe_benchmark_dab.h"
#endif
#include "vibe_dab_aacdec.h"
#include "vibe_dab_faad.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <cmath>
#include <complex>
#include <cstdio>
#include <sys/resource.h>
#include <thread>
#include <vector>

#if !defined(VIBE_HAVE_FAAD2)
int main() { std::printf("   NOT RUN — no FAAD2 in this build\n"); return 3; }
#else
using namespace std::chrono;

struct Units { std::vector<std::vector<uint8_t>> adts; int aot = 0, sfi = 0, ch = 0; };

/** ★ ADTS FILES MODE (-DVIBE_DAB_FAAD_FILES): read units from ADTS files dumped by the clip mode (VIBE_DAB_FAAD_DUMP) —
 *  how the PLAIN FAAD2 build is measured. The server links FAAD2's DRM build, and that build REFUSES DAB+'s fill
 *  elements ("No standard extension payload allowed in DRM", measured 2026-10-26), so DAB+ would need the plain build
 *  beside it — the two export the same symbols. */
static bool loadAdts(const std::string& path, Units& u) {
    FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
    std::vector<uint8_t> d; int c; while ((c = std::fgetc(f)) != EOF) d.push_back(uint8_t(c)); std::fclose(f);
    size_t p = 0;
    while (p + 7 < d.size()) { const size_t len = size_t(((d[p + 3] & 3) << 11) | (d[p + 4] << 3) | (d[p + 5] >> 5)); if (!len || p + len > d.size()) break; u.adts.emplace_back(d.begin() + long(p), d.begin() + long(p + len)); p += len; }
    if (u.adts.empty()) return false;
    const uint8_t* a = u.adts[0].data();
    u.aot = ((a[2] >> 6) & 3) + 1; u.sfi = (a[2] >> 2) & 15; u.ch = ((a[2] & 1) << 2) | (a[3] >> 6);
    return true;
}
#if !defined(VIBE_DAB_FAAD_FILES)
/** Run the real DabService on the clip, service `pick` (0 = the clip's own), collecting ~`secs` of ADTS units. */
static bool collect(const std::string& clipPath, int pick, double secs, Units& u, uint32_t& sidOut) {
    vibe::DabClip clip;
    if (!vibe::loadDabClip(clipPath, clip)) return false;
    vibedab::DabService svc;
    svc.setRfRate(clip.rate); svc.setRfCentre(clip.centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - clip.centre) < 1000.0) { svc.setChannel(i); break; }
    std::mutex m; bool on = false;
    svc.onAdtsForTest = [&](const uint8_t* p, size_t n) { std::lock_guard<std::mutex> lk(m); if (on) u.adts.emplace_back(p, p + n); };
    const size_t blk = size_t(clip.rate / 20) * 2;
    size_t pos = 0; double fed = 0;
    std::vector<float> pcm(48000);
    const auto t0 = steady_clock::now();
    auto feedFor = [&](double s) {
        const double until = fed + s;
        while (fed < until) {
            if (pos + blk > clip.iq.size()) pos = 0;
            svc.feed(clip.iq.data() + pos, blk / 2); pos += blk; fed += 0.05;
            svc.takePcm(pcm.data(), pcm.size() / 2);
            std::this_thread::sleep_until(t0 + microseconds((long long)(fed * 1e6)));
        }
    };
    svc.setService(clip.sid);
    feedFor(8.0);
    uint32_t sid = clip.sid;
    if (pick > 0) {
        auto learn = svc.learnable();
        int k = 0;
        for (auto& r : learn) if (r.sid != clip.sid && ++k == pick) { sid = r.sid; break; }
        svc.setService(sid); feedFor(5.0);
    }
    { std::lock_guard<std::mutex> lk(m); on = true; }
    feedFor(secs);
    { std::lock_guard<std::mutex> lk(m); on = false; }
    sidOut = sid;
    if (u.adts.empty()) return false;
    const uint8_t* a = u.adts[0].data();
    u.aot = ((a[2] >> 6) & 3) + 1; u.sfi = (a[2] >> 2) & 15; u.ch = ((a[2] & 1) << 2) | (a[3] >> 6);
    return true;
}

#endif
struct Dec { std::vector<float> pcm; int rate = 0; double cpu = 0, startMs = 0; long rssKb = 0; int64_t bad = 0; };

static double cpuNow() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static Dec runFaad(const Units& u, int corruptEvery) {
    Dec d; vibedab::DabFaad f;
    rusage r0; getrusage(RUSAGE_SELF, &r0);
    const double c0 = cpuNow(); const auto t0 = steady_clock::now(); bool first = true;
    for (size_t i = 0; i < u.adts.size(); ++i) {
        std::vector<uint8_t> a = u.adts[i];
        if (corruptEvery && (i + 1) % corruptEvery == 0) for (size_t k = 9; k < a.size(); k += 3) a[k] ^= 0x5A;
        f.decode(a.data(), a.size(), d.pcm);
        if (first && !d.pcm.empty()) { d.startMs = duration<double, std::milli>(steady_clock::now() - t0).count(); first = false; }
    }
    d.cpu = cpuNow() - c0; d.rate = f.rateHz(); d.bad = f.bad();
    rusage r1; getrusage(RUSAGE_SELF, &r1); d.rssKb = (r1.ru_maxrss - r0.ru_maxrss) / 1024;   // macOS: bytes
    return d;
}

static const char* ffPath() {   // ★ the same homes vibe_dab_aacdec.h looks in
    for (const char* c : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" }) if (::access(c, X_OK) == 0) return c;
    return "ffmpeg";
}
/** FFmpeg with the server's own arguments (vibe_dab_aacdec.h: -f aac … -f f32le -ar 48000 -ac 2) on the same units, run
 *  over a FILE so that every unit is decoded — the start-up latency is measured separately through the server's own live
 *  pipe class (AacDecoderFfmpeg), whose non-blocking writes DROP a unit when the pipe is full (see the report). */
static Dec runFfmpeg(const Units& u, int corruptEvery) {
    Dec d;
    const std::string dir = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    const std::string in = dir + "/tdf-in.aac", out = dir + "/tdf-out.f32";
    FILE* f = std::fopen(in.c_str(), "wb");
    for (size_t i = 0; i < u.adts.size(); ++i) {
        std::vector<uint8_t> a = u.adts[i];
        if (corruptEvery && (i + 1) % corruptEvery == 0) for (size_t k = 9; k < a.size(); k += 3) a[k] ^= 0x5A;
        std::fwrite(a.data(), 1, a.size(), f);
    }
    std::fclose(f);
    rusage c0; getrusage(RUSAGE_CHILDREN, &c0);
    const std::string cmd = std::string("'") + ffPath() + "' -hide_banner -loglevel quiet -nostdin -f aac -i '" + in +
                            "' -f f32le -ar 48000 -ac 2 -y '" + out + "'";
    if (std::system(cmd.c_str()) != 0) return d;
    rusage c1; getrusage(RUSAGE_CHILDREN, &c1);
    d.cpu = (c1.ru_utime.tv_sec - c0.ru_utime.tv_sec) + (c1.ru_utime.tv_usec - c0.ru_utime.tv_usec) * 1e-6
          + (c1.ru_stime.tv_sec - c0.ru_stime.tv_sec) + (c1.ru_stime.tv_usec - c0.ru_stime.tv_usec) * 1e-6;
    d.rssKb = c1.ru_maxrss / 1024;
    FILE* g = std::fopen(out.c_str(), "rb");
    float buf[8192]; size_t n;
    while ((n = std::fread(buf, 4, 8192, g)) > 0) d.pcm.insert(d.pcm.end(), buf, buf + n);
    std::fclose(g); std::remove(in.c_str()); std::remove(out.c_str());
    d.rate = 48000;
    // start-up latency through the live pipe, as the server runs it
    vibedab::AacDecoderFfmpeg ff; vibedab::AacPcm o;
    const auto t0 = steady_clock::now();
    for (size_t i = 0; i < u.adts.size() && o.interleaved.empty(); ++i) {
        ff.decode(u.adts[i].data(), u.adts[i].size(), o);
        std::this_thread::sleep_for(milliseconds(20)); ff.drain(o);
    }
    if (!o.interleaved.empty()) d.startMs = duration<double, std::milli>(steady_clock::now() - t0).count();
    ff.close();
    return d;
}

/** The server's pacing: FFmpeg decodes each 960-sample unit as 1024 and the service plays the result at the MEASURED rate
 *  (samples returned per unit over the unit's real duration). Do the same here — windowed-sinc, so the resampler is not
 *  what is being compared. */
static std::vector<float> resample(const std::vector<float>& st, double ratio /* out/in */) {
    std::vector<float> o;
    const size_t n = st.size() / 2; const double step = 1.0 / ratio; const int H = 16;
    for (double t = H; t < double(n) - H; t += step) {
        const int i0 = int(t); double l = 0, r = 0, ws = 0;
        for (int k = -H + 1; k <= H; ++k) {
            const double x = t - double(i0 + k);
            const double c = std::min(1.0, ratio);
            const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x * c) / (M_PI * x * c);
            const double w = 0.5 + 0.5 * std::cos(M_PI * x / H);
            l += st[2 * size_t(i0 + k)] * s * w; r += st[2 * size_t(i0 + k) + 1] * s * w; ws += s * w;
        }
        o.push_back(float(l / ws)); o.push_back(float(r / ws));
    }
    return o;
}

static int bestLag(const std::vector<float>& a, const std::vector<float>& b, int maxLag) {
    const size_t N = std::min<size_t>(std::min(a.size(), b.size()) / 2, 48000 * 5);
    double best = -1e30; int bl = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0;
        for (size_t i = size_t(maxLag); i + size_t(maxLag) < N; i += 3) {
            const size_t j = size_t(long(i) + lag);
            s += (a[2 * i] + a[2 * i + 1]) * (b[2 * j] + b[2 * j + 1]);
        }
        if (s > best) { best = s; bl = lag; }
    }
    return bl;
}

static double rmsDb(const std::vector<float>& x) { double e = 0; for (float v : x) e += double(v) * v; return 10 * std::log10(e / std::max<size_t>(1, x.size()) + 1e-20); }
static double maxStep(const std::vector<float>& p) {
    std::vector<float> d; for (size_t i = 2; i < p.size(); i += 2) d.push_back(std::fabs(p[i] - p[i - 2]));
    if (d.empty()) return 0; std::sort(d.begin(), d.end()); return d[size_t(0.99999 * double(d.size() - 1))];
}
/** 10 ms windows more than 20 dB under the same window of the clean decode — dips and gaps. */
static int dips(const std::vector<float>& clean, const std::vector<float>& dmg) {
    int n = 0; const size_t W = 960;
    for (size_t i = 0; i + W <= std::min(clean.size(), dmg.size()); i += W) {
        double a = 0, b = 0; for (size_t k = 0; k < W; ++k) { a += clean[i + k] * clean[i + k]; b += dmg[i + k] * dmg[i + k]; }
        if (a > 1e-6 && b < a * 0.01) ++n;
    }
    return n;
}
/** The three strongest tones (Hz), from a 2^16 FFT of the mid channel. */
static std::vector<double> tones(const std::vector<float>& st, int rate) {
    const size_t N = 65536; if (st.size() / 2 < N) return {};
    std::vector<std::complex<double>> x(N);
    for (size_t i = 0; i < N; ++i) x[i] = (st[2 * i] + st[2 * i + 1]) * (0.5 - 0.5 * std::cos(2 * M_PI * i / N));
    // radix-2 FFT
    for (size_t i = 1, j = 0; i < N; ++i) { size_t bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(x[i], x[j]); }
    for (size_t len = 2; len <= N; len <<= 1) { const auto w = std::polar(1.0, -2 * M_PI / double(len));
        for (size_t i = 0; i < N; i += len) { std::complex<double> wn(1); for (size_t k = 0; k < len / 2; ++k) { auto u = x[i + k], v = x[i + k + len / 2] * wn; x[i + k] = u + v; x[i + k + len / 2] = u - v; wn *= w; } } }
    std::vector<std::pair<double, double>> pk;
    for (size_t k = 2; k + 2 < N / 2; ++k) { const double m = std::abs(x[k]); if (m > std::abs(x[k - 1]) && m >= std::abs(x[k + 1]) && m > std::abs(x[k - 2]) && m >= std::abs(x[k + 2])) {
        const double a = std::abs(x[k - 1]), c = std::abs(x[k + 1]); const double d = 0.5 * (a - c) / (a - 2 * m + c);   // parabolic
        pk.push_back({m, (double(k) + d) * rate / double(N)}); } }
    std::sort(pk.rbegin(), pk.rend());
    std::vector<double> f; for (size_t i = 0; i < pk.size() && f.size() < 3; ++i) f.push_back(pk[i].second);
    std::sort(f.begin(), f.end()); return f;
}
static void stereo(const std::vector<float>& st, double& corr, double& sideDb) {
    double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
    for (size_t i = 0; i + 1 < st.size(); i += 2) { const double l = st[i], r = st[i + 1]; ll += l * l; rr += r * r; lr += l * r; mm += (l + r) * (l + r); ss += (l - r) * (l - r); }
    corr = lr / std::sqrt(ll * rr + 1e-30); sideDb = 10 * std::log10((ss + 1e-30) / (mm + 1e-30));
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: test-dab-faad <clip.vbu8> [impaired.vbu8]\n"); return 3; }
    if (!vibedab::AacDecoderFfmpeg().available()) { std::printf("   NOT RUN — no FFmpeg to compare against\n"); return 3; }
#if defined(VIBE_DAB_FAAD_FILES)
    for (int pick = 1; pick < argc; ++pick) {
        Units u; uint32_t sid = uint32_t(pick);
        if (!loadAdts(argv[pick], u)) { std::printf("   %s: no units\n", argv[pick]); continue; }
#else
    for (int pick : {0, 1, 2}) {
        Units u; uint32_t sid = 0;
        if (!collect(argv[1], pick, 12.0, u, sid)) { std::printf("   service %d: no DAB+ units collected\n", pick); continue; }
#endif
        static const int sfr[16] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0};
        std::printf("\n══ service %04X: AOT %d, core %d Hz, %d ch — %zu access units\n", sid, u.aot, sfr[u.sfi], u.ch, u.adts.size());
        if (const char* dd = getenv("VIBE_DAB_FAAD_DUMP")) { FILE* df = std::fopen((std::string(dd) + "-" + std::to_string(pick) + ".aac").c_str(), "wb"); for (auto& a : u.adts) std::fwrite(a.data(), 1, a.size(), df); std::fclose(df); }
        Dec fa = runFaad(u, 0), ff = runFfmpeg(u, 0);
        std::printf("   FAAD2: %lld units failed of %zu\n", (long long)fa.bad, u.adts.size());
        const double secs = double(fa.pcm.size() / 2) / std::max(1, fa.rate);
        // the server's pacing of FFmpeg's output: its samples over the units' true duration → 48 kHz
        const double ffPerSec = double(ff.pcm.size() / 2) / secs;
        std::vector<float> ffp = resample(ff.pcm, 48000.0 / ffPerSec);
        std::vector<float> fap = fa.rate == 48000 ? fa.pcm : resample(fa.pcm, 48000.0 / fa.rate);
        const int lag = bestLag(fap, ffp, 4800);
        std::vector<float> A, B;
        for (size_t i = 0; i < fap.size() / 2; ++i) { const long j = long(i) + lag; if (j < 0 || size_t(j) >= ffp.size() / 2) continue; A.push_back(fap[2 * i]); A.push_back(fap[2 * i + 1]); B.push_back(ffp[2 * size_t(j)]); B.push_back(ffp[2 * size_t(j) + 1]); }
        // skip the first and last half second (priming)
        if (A.size() > 96000 * 2) { A.erase(A.begin(), A.begin() + 48000); B.erase(B.begin(), B.begin() + 48000); A.resize(A.size() - 48000); B.resize(B.size() - 48000); }
        double e = 0, ea = 0, eb = 0, ab = 0; for (size_t i = 0; i < A.size(); ++i) { e += (A[i] - B[i]) * (A[i] - B[i]); ea += A[i] * A[i]; eb += B[i] * B[i]; ab += A[i] * B[i]; }
        const double g = ab / (eb + 1e-30);
        double eg = 0; for (size_t i = 0; i < A.size(); ++i) eg += (A[i] - g * B[i]) * (A[i] - g * B[i]);
        double maxAbs = 0; for (size_t i = 0; i < A.size(); ++i) maxAbs = std::max(maxAbs, double(std::fabs(A[i] - B[i])));
        std::printf("   FAAD2 out %d Hz, FFmpeg %.0f samples/s of programme (paced to 48 kHz as the server does)\n", fa.rate, ffPerSec);
        std::printf("   level: FAAD2 %.2f dB, FFmpeg %.2f dB (difference %.2f dB)\n", rmsDb(A), rmsDb(B), rmsDb(A) - rmsDb(B));
        std::printf("   FAAD2 vs FFmpeg: SNR %.1f dB raw, %.1f dB gain-matched; max |diff| %.4f (lag %d samples)\n",
                    10 * std::log10(ea / (e + 1e-30)), 10 * std::log10(ea / (eg + 1e-30)), maxAbs, lag);
        const auto tf = tones(A, 48000), tg = tones(B, 48000);
        std::printf("   tones: FAAD2"); for (double f : tf) std::printf(" %.2f", f); std::printf(" Hz | FFmpeg"); for (double f : tg) std::printf(" %.2f", f); std::printf(" Hz\n");
        double c1, s1, c2, s2; stereo(A, c1, s1); stereo(B, c2, s2);
        std::printf("   stereo: L/R correlation FAAD2 %.3f / FFmpeg %.3f; side/mid FAAD2 %.1f dB / FFmpeg %.1f dB\n", c1, c2, s1, s2);
        // damaged
        Dec fad = runFaad(u, 10), ffd = runFfmpeg(u, 10);
        std::vector<float> ffdp = resample(ffd.pcm, 48000.0 / ffPerSec);
        std::vector<float> fadp = fad.rate == 48000 ? fad.pcm : resample(fad.pcm, 48000.0 / fad.rate);
        std::printf("   damaged (1 unit in 10): largest step FAAD2 %.4f (clean %.4f) | FFmpeg %.4f (clean %.4f)\n", maxStep(fadp), maxStep(fap), maxStep(ffdp), maxStep(ffp));
        std::printf("                           10 ms dips > 20 dB: FAAD2 %d | FFmpeg %d;  length vs clean: FAAD2 %+.2f s | FFmpeg %+.2f s\n",
                    dips(fap, fadp), dips(ffp, ffdp), (double(fadp.size()) - double(fap.size())) / 96000.0, (double(ffdp.size()) - double(ffp.size())) / 96000.0);
        std::printf("   start-up: first PCM after %.1f ms (FAAD2) vs %.1f ms (FFmpeg)\n", fa.startMs, ff.startMs);
        std::printf("   CPU for %.1f s: FAAD2 %.3f s (%.2f %%), FFmpeg %.3f s (%.2f %%);  memory: FAAD2 +%ld KB in process, FFmpeg child peak RSS %ld KB\n",
                    secs, fa.cpu, 100 * fa.cpu / secs, ff.cpu, 100 * ff.cpu / secs, fa.rssKb, ff.rssKb);
    }
    return 0;
}
#endif
