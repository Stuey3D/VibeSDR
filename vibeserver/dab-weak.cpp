// dab-weak.cpp — THE WEAK-SIGNAL REGRESSION CHECK for the DAB chain (V12, 2026-11-04).
//
// ★★★ WHY. Our DAB is praised for weak multiplexes and DX, and that is the thing every change to the shared chain (sync,
//     FIC, the decoder, the front end) can quietly cost. This runs a REAL captured multiplex through the REAL DabService
//     — the same object the server runs — with white noise added at a chosen level, and reports how far the receiver
//     got and how fast: lock, the ensemble label, the services, and the chosen service's audio. Stepping the noise up
//     until audio fails gives the lowest SNR that still decodes; doing it before and after a change is the gate.
//
//   dab-weak <clip.vbu8> [options]
//     --sid HEX        the service to follow (its super frames / AUs are the audio measure); default the clip's own
//     --skip S         seconds of the clip to skip first (an AGC still climbing)               default 0
//     --secs S         seconds to feed                                                         default 30
//     --shift HZ       mix the clip by HZ first (a capture taken off the block centre)          default 0
//     --noise DB       add white noise DB below the clip's total power (omit = none)
//     --seed N         the noise's seed (runs are reproducible)                                 default 1
//     --wide RATE      ★ the WIDE-RATE path: up-sample the clip to RATE, put the block at --offset, add the same noise
//                       DENSITY across the whole wide band, and feed the service at RATE (its DDC extracts the block)
//     --offset HZ      where the block sits in the wide capture                                 default 2500000
//     --second F.vbu8  ★ two blocks at once: a second clip at --offset2 in the same wide stream, decoded by a SECOND
//                       service on the same samples (the two-listener case)
//     --offset2 HZ     default -1500000
//     --sid2 HEX       the second clip's service
//     --paired 1       wide only: add the noise at the clip's rate BEFORE the up-sampler — the very samples the native run
//                       gets — so native against wide is an A/B of the two front ends on identical noise
//   Prints ONE JSON line per service: lock/label/first-audio times (in CLIP seconds), the final MER, FIB rate, super
//   frames ok of tried, AUs bad of in, RS lost, and the input's own measured in-band SNR.
// ★ The clip is STREAMED (as dab-unitlog): nothing whole-file in memory. ★ Paced by back-pressure (never ahead of the
//   worker), so results do not depend on how busy the machine is.
#include "vibe_dab_service.h"
#include "vibe_dab_channels.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

struct Clip {
    FILE* fp = nullptr; double rate = 0, centre = 0; uint32_t sid = 0;
    bool open(const char* path) {
        fp = std::fopen(path, "rb");
        char magic[8];
        return fp && std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, "VIBEBU81", 8) == 0 && std::fread(&rate, 8, 1, fp) == 1
            && std::fread(&centre, 8, 1, fp) == 1 && std::fread(&sid, 4, 1, fp) == 1;
    }
    void skip(double s) { std::fseek(fp, long(28 + 2 * (long long)(s * rate)), SEEK_SET); }
    /** n complex samples as floats; false at the end. */
    bool read(std::vector<uint8_t>& u, std::vector<float>& f, size_t n) {
        u.resize(2 * n); f.resize(2 * n);
        if (std::fread(u.data(), 1, u.size(), fp) != u.size()) return false;
        for (size_t i = 0; i < u.size(); ++i) f[i] = (float(u[i]) - 127.5f) / 127.5f;
        return true;
    }
};

/** A plain mixer in double (the TEST GENERATOR: exact, not fast). */
struct Mix {
    double ph = 0, step = 0;
    void set(double hz, double rate) { step = 2.0 * M_PI * hz / rate; }
    void run(float* x, size_t n) {
        if (step == 0) return;
        for (size_t i = 0; i < n; ++i) {
            const double c = std::cos(ph), s = std::sin(ph);
            const double re = x[2 * i] * c - x[2 * i + 1] * s, im = x[2 * i] * s + x[2 * i + 1] * c;
            x[2 * i] = float(re); x[2 * i + 1] = float(im);
            ph += step; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        }
    }
};

/** ★ The TEST's up-sampler (2.4 MS/s → the wide rate): a long Kaiser-windowed rational interpolator, far better than any
 *  receiver stage, so whatever the DDC under test loses is the DDC's. Passes ±0.9 MHz, rejects the images. */
struct Upsampler {
    int L = 1, M = 1, T = 0; std::vector<float> h; std::vector<float> hist; long long phaseAcc = 0;
    static double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
    void design(double inRate, double outRate) {
        // rational L/M with L*inRate == M*outRate
        long long a = (long long)std::llround(outRate), b = (long long)std::llround(inRate);
        long long g = std::gcd(a, b); L = int(a / g); M = int(b / g);
        T = 24;
        const int n = L * T;
        h.assign(size_t(n), 0.0f);
        const double fs = inRate * L;                         // prototype rate
        const double fc = 1.05e6 / fs;                        // cycles/sample (pass ±0.9 MHz, stop by 1.2)
        const double beta = 10.0;
        for (int i = 0; i < n; ++i) {
            const double t = i - (n - 1) * 0.5;
            const double sinc = std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
            const double r = 2.0 * i / (n - 1) - 1.0;
            const double w = bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
            h[size_t(i)] = float(sinc * w * L);
        }
        hist.assign(size_t(2 * T), 0.0f);
    }
    /** in: n complex at inRate; appends output at outRate. */
    void run(const float* in, size_t n, std::vector<float>& out) {
        for (size_t i = 0; i < n; ++i) {
            // shift history (T complex), newest last
            std::memmove(hist.data(), hist.data() + 2, sizeof(float) * size_t(2 * T - 2));
            hist[size_t(2 * T - 2)] = in[2 * i]; hist[size_t(2 * T - 1)] = in[2 * i + 1];
            // emit every output whose position falls in this input step: output k at input position k*M/L
            while (phaseAcc < L) {
                const int p = int(phaseAcc);
                double re = 0, im = 0;
                for (int t = 0; t < T; ++t) {
                    const float c = h[size_t(p + t * L)];
                    re += c * hist[size_t(2 * (T - 1 - t))]; im += c * hist[size_t(2 * (T - 1 - t) + 1)];
                }
                out.push_back(float(re)); out.push_back(float(im));
                phaseAcc += M;
            }
            phaseAcc -= L;
        }
    }
};

/** The in-band SNR of a 2.4 MS/s block of IQ: power in ±700 kHz over the noise density measured at 0.85-1.1 MHz. */
double inbandSnrDb(const std::vector<float>& x, double rate) {
    const int N = 4096; if (x.size() < size_t(2 * N * 8)) return NAN;
    std::vector<double> psd(N, 0.0);
    std::vector<std::complex<double>> a(N);
    const int frames = int(x.size() / 2 / N);
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N); a[size_t(i)] = { x[size_t(2 * (f * N + i))] * w, x[size_t(2 * (f * N + i) + 1)] * w }; }
        // radix-2 FFT
        for (int i = 1, j = 0; i < N; ++i) { int bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[size_t(i)], a[size_t(j)]); }
        for (int len = 2; len <= N; len <<= 1) {
            const std::complex<double> wl = std::polar(1.0, -2 * M_PI / len);
            for (int i = 0; i < N; i += len) { std::complex<double> w = 1; for (int j = 0; j < len / 2; ++j) { const auto u = a[size_t(i + j)], v = a[size_t(i + j + len / 2)] * w; a[size_t(i + j)] = u + v; a[size_t(i + j + len / 2)] = u - v; w *= wl; } }
        }
        for (int i = 0; i < N; ++i) psd[size_t(i)] += std::norm(a[size_t(i)]);
    }
    double sIn = 0, nIn = 0; int cIn = 0, cN = 0;
    for (int i = 0; i < N; ++i) {
        const double f = (i < N / 2 ? i : i - N) * rate / N;
        if (std::fabs(f) < 700e3 && std::fabs(f) > 5e3) { sIn += psd[size_t(i)]; ++cIn; }
        if (std::fabs(f) > 850e3 && std::fabs(f) < 1100e3) { nIn += psd[size_t(i)]; ++cN; }
    }
    if (!cN || !cIn) return NAN;
    const double nd = nIn / cN, total = sIn / cIn;
    return 10 * std::log10(std::max(1e-12, (total - nd) / nd));
}

double num(const std::string& j, const char* key, double dflt = NAN) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    if (p == std::string::npos) return dflt;
    const char* s = j.c_str() + p + k.size();
    if (*s == 't') return 1; if (*s == 'f') return 0;
    return std::atof(s);
}
std::string str(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = j.find(k);
    if (p == std::string::npos) return "";
    const size_t e = j.find('"', p + k.size());
    return j.substr(p + k.size(), e - p - k.size());
}
int countServices(const std::string& j) {
    const size_t p = j.find("\"services\":[");
    if (p == std::string::npos) return 0;
    int n = 0; for (size_t q = j.find("{\"sid\":", p); q != std::string::npos; q = j.find("{\"sid\":", q + 1)) ++n;
    return n;
}

struct Watch {
    std::string name; double tLock = -1, tLabel = -1, tAudio = -1, tSvc = -1;
    double merSum = 0; int merN = 0; std::string last;
    void poll(vibedab::DabService& s, double t) {
        std::string j; if (!s.jsonTry(j)) return;
        last = j;
        if (tLock < 0 && num(j, "locked") == 1) tLock = t;
        if (tLabel < 0 && !str(j, "label").empty()) tLabel = t;
        if (tSvc < 0 && countServices(j) > 0) tSvc = t;
        if (tAudio < 0 && num(j, "sfOk", 0) + num(j, "mp2Out", 0) > 0) tAudio = t;
        const double m = num(j, "mer"); if (t > 5 && m == m && num(j, "locked") == 1) { merSum += m; ++merN; }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: dab-weak <clip.vbu8> [--sid HEX] [--skip S] [--secs S] [--shift HZ] [--noise DB] [--wide RATE --offset HZ] [--second F --offset2 HZ --sid2 HEX]\n"); return 2; }
    double skip = 0, secs = 30, shift = 0, noiseDb = NAN, wide = 0, offset = 2.5e6, offset2 = -1.5e6, shift2 = 0;
    uint32_t sid = 0, sid2 = 0; unsigned seed = 1; const char* second = nullptr;
    const char* writePath = nullptr;   // ★ --write F: write the composed stream (u8 VIBEBU81) instead of decoding it — the
    bool to2048 = false;               //   fake radio's clips (test-server-dab-wide.mjs); --to2048 1 resamples a 2.4 clip to
    double centreOut = 0;              //   2.048 first (the native path's own converter); --centre-out HZ = the header centre
    bool loop = false;
    bool paired = false;   // ★ --paired 1: the noise goes in at the CLIP's rate before the up-sampler, so the wide path sees
                           //   exactly the in-band noise the native path does (an A/B of the front ends alone)
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i]; const char* v = argv[i + 1];
        if (a == "--sid") sid = uint32_t(std::strtoul(v, nullptr, 16));
        else if (a == "--skip") skip = std::atof(v);
        else if (a == "--secs") secs = std::atof(v);
        else if (a == "--shift") shift = std::atof(v);
        else if (a == "--shift2") shift2 = std::atof(v);
        else if (a == "--noise") noiseDb = std::atof(v);
        else if (a == "--seed") seed = unsigned(std::atoi(v));
        else if (a == "--paired") paired = std::atoi(v) != 0;
        else if (a == "--loop") loop = std::atoi(v) != 0;   // ★ replay a short clip (the 5.76 s bench clips are seamless loops)
        else if (a == "--write") writePath = v;
        else if (a == "--to2048") to2048 = std::atoi(v) != 0;
        else if (a == "--centre-out") centreOut = std::atof(v);
        else if (a == "--wide") wide = std::atof(v);
        else if (a == "--offset") offset = std::atof(v);
        else if (a == "--second") second = v;
        else if (a == "--offset2") offset2 = std::atof(v);
        else if (a == "--sid2") sid2 = uint32_t(std::strtoul(v, nullptr, 16));
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    Clip c1, c2;
    if (!c1.open(argv[1])) { std::printf("not a VIBEBU81 clip: %s\n", argv[1]); return 2; }
    if (second && !c2.open(second)) { std::printf("not a VIBEBU81 clip: %s\n", second); return 2; }
    if (second && !wide) { std::printf("--second needs --wide\n"); return 2; }
    if (!sid) sid = c1.sid; if (second && !sid2) sid2 = c2.sid;
    c1.skip(skip); if (second) c2.skip(skip);
    auto chanFor = [](double centre) { for (int i = 0; i < int(vibedab::kBandIIICount); ++i) if (std::fabs(double(vibedab::kBandIII[i].centreHz) - centre) < 1000.0) return i; return -1; };

    const double rate = c1.rate;
    const double feedRate = wide > 0 ? wide : rate;
    std::vector<std::unique_ptr<vibedab::DabService>> svcs;
    std::vector<Watch> watch;
    auto addSvc = [&](const Clip& c, uint32_t s, double off, const std::string& nm) {
        auto p = std::make_unique<vibedab::DabService>();
        const int ch = chanFor(c.centre);
        p->setRfRate(feedRate);
        // ★ The radio's centre: the block sits `off` above it in a wide capture.
        p->setRfCentre(c.centre - (wide > 0 ? off : 0.0));
        if (ch >= 0) p->setChannel(ch);
        if (s) p->setService(s);
        svcs.push_back(std::move(p)); Watch w; w.name = nm; watch.push_back(w);
    };
    addSvc(c1, sid, offset, argv[1]);
    if (second) addSvc(c2, sid2, offset2, second);

    // ★ Noise: power relative to the clip's TOTAL power at its own rate, measured on its first second (after the skip).
    std::vector<uint8_t> u; std::vector<float> f1, f2;
    double pClip = 0;
    { const long pos = std::ftell(c1.fp); c1.read(u, f1, size_t(rate)); for (size_t i = 0; i < f1.size(); ++i) pClip += double(f1[i]) * f1[i]; pClip /= double(f1.size() / 2); std::fseek(c1.fp, pos, SEEK_SET); }
    const bool addNoise = noiseDb == noiseDb;
    const double pNoiseClip = addNoise ? pClip * std::pow(10.0, -noiseDb / 10.0) : 0.0;
    const double density = pNoiseClip / rate;                       // the same density on every path
    const double sigma = std::sqrt(density * feedRate / 2.0);       // per component, at the feed rate
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.0f, 1.0f);

    Mix m1, m2, w1, w2; m1.set(shift, rate); m2.set(shift2, rate);
    Upsampler up1, up2;
    if (wide > 0) { up1.design(rate, wide); w1.set(offset, wide); if (second) { up2.design(rate, wide); w2.set(offset2, wide); } }

    // ★ 50 ms of the clip — or, at 2.048 MS/s, half a DAB frame (98304), so a 60-frame bench clip loops on a frame edge
    const size_t blk = std::fabs(rate - 2048000.0) < 1.0 ? size_t(98304) : size_t(rate / 20);
    std::vector<float> o1, o2, feedBuf, snrProbe;
    double fed = 0, nextPoll = 0;
    const auto tw0 = steady_clock::now();
    FILE* wfp = nullptr; vibedab::Resample24to2048 wrs; std::vector<float> wbuf; std::vector<uint8_t> wu8;
    if (writePath) {
        wfp = std::fopen(writePath, "wb");
        if (!wfp) { std::printf("cannot write %s\n", writePath); return 2; }
        const double r = to2048 ? 2048000.0 : feedRate, cen = centreOut > 0 ? centreOut : c1.centre - (wide > 0 ? offset : 0.0);
        std::fwrite("VIBEBU81", 1, 8, wfp); std::fwrite(&r, 8, 1, wfp); std::fwrite(&cen, 8, 1, wfp); std::fwrite(&sid, 4, 1, wfp);
    }
    while (fed < secs) {
        if (!c1.read(u, f1, blk)) { if (!loop) break; c1.skip(0); if (!c1.read(u, f1, blk)) break; }
        m1.run(f1.data(), blk);
        const double sigClip = std::sqrt(density * rate / 2.0);
        if (addNoise && paired && wide > 0) for (float& x : f1) x += float(sigClip) * g(rng);
        if (second) { std::vector<uint8_t> u2; if (!c2.read(u2, f2, blk)) break; m2.run(f2.data(), blk); }
        if (wide > 0) {
            o1.clear(); up1.run(f1.data(), blk, o1); w1.run(o1.data(), o1.size() / 2);
            if (second) { o2.clear(); up2.run(f2.data(), blk, o2); w2.run(o2.data(), o2.size() / 2);
                          const size_t n = std::min(o1.size(), o2.size()); for (size_t i = 0; i < n; ++i) o1[i] += o2[i]; o1.resize(n); }
            feedBuf.swap(o1);
        } else feedBuf.assign(f1.begin(), f1.end());
        if (addNoise && !(paired && wide > 0)) for (float& x : feedBuf) x += sigma * g(rng);
        if (fed < 1.0 && !wide) snrProbe.insert(snrProbe.end(), feedBuf.begin(), feedBuf.end());
        if (wfp) {
            const std::vector<float>* src = &feedBuf;
            if (to2048) { wbuf.clear(); wrs.process(feedBuf.data(), feedBuf.size() / 2, wbuf); src = &wbuf; }
            wu8.resize(src->size());
            for (size_t i = 0; i < src->size(); ++i) { const float v = (*src)[i] * 127.5f + 127.5f; wu8[i] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : std::lrint(v)); }
            std::fwrite(wu8.data(), 1, wu8.size(), wfp);
            fed += double(blk) / rate;
            continue;
        }
        for (auto& s : svcs) s->feed(feedBuf.data(), feedBuf.size() / 2);
        fed += double(blk) / rate;
        for (int w = 0; w < 400000; ++w) {   // ★ back-pressure: never ahead of any worker
            bool busy = false; for (auto& s : svcs) if (s->backlogForTest() > 0) busy = true;
            if (!busy) break;
            std::this_thread::sleep_for(microseconds(100));
        }
        if (fed >= nextPoll) { for (size_t k = 0; k < svcs.size(); ++k) watch[k].poll(*svcs[k], fed); nextPoll = fed + 0.25; }
    }
    if (wfp) { std::fclose(wfp); for (auto& s : svcs) s->stopWorker(); std::printf("{\"wrote\":\"%s\",\"secs\":%.2f}\n", writePath, fed); return 0; }
    std::this_thread::sleep_for(milliseconds(300));
    const double snrIn = snrProbe.empty() ? NAN : inbandSnrDb(snrProbe, rate);
    for (size_t k = 0; k < svcs.size(); ++k) {
        std::string j; for (int i = 0; i < 100 && !svcs[k]->jsonTry(j); ++i) std::this_thread::sleep_for(milliseconds(10));
        if (j.empty()) j = watch[k].last;
        const Watch& w = watch[k];
        std::printf("{\"clip\":\"%s\",\"path\":\"%s\",\"feedRate\":%.0f,\"noiseDb\":%s,\"snrIn\":%.2f,\"secs\":%.2f,"
                    "\"tLock\":%.2f,\"tLabel\":%.2f,\"tSvcs\":%.2f,\"tAudio\":%.2f,\"label\":\"%s\",\"services\":%d,"
                    "\"mer\":%.2f,\"merAvg\":%.2f,\"mscBer\":%.4f,\"fibRate\":%.3f,\"sfOk\":%.0f,\"sfTried\":%.0f,\"auIn\":%.0f,\"auBad\":%.0f,"
                    "\"rsFixed\":%.0f,\"rsLost\":%.0f,\"mp2In\":%.0f,\"mp2Out\":%.0f,\"mp2Bad\":%.0f,\"frames\":%.0f,\"reacq\":%.0f,\"offsetHz\":%.0f,\"wallS\":%.1f}\n",
                    w.name.substr(w.name.find_last_of('/') + 1).c_str(), wide > 0 ? "wide" : "native", feedRate,
                    addNoise ? std::to_string(noiseDb).c_str() : "null", snrIn, fed,
                    w.tLock, w.tLabel, w.tSvc, w.tAudio, str(j, "label").c_str(), countServices(j),
                    num(j, "mer"), w.merN ? w.merSum / w.merN : NAN, num(j, "mscBer"), num(j, "fibRate"), num(j, "sfOk"), num(j, "sfTried"),
                    num(j, "auIn"), num(j, "auBad"), num(j, "rsFixed"), num(j, "rsLost"), num(j, "mp2In"), num(j, "mp2Out"), num(j, "mp2Bad"),
                    num(j, "frames"), num(j, "reacquires"), num(j, "offsetHz"),
                    duration<double>(steady_clock::now() - tw0).count());
    }
    for (auto& s : svcs) s->stopWorker();
    return 0;
}
