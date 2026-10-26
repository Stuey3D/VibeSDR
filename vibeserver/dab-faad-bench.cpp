// dab-faad-bench.cpp — DAB+ through FAAD2: what each stage COSTS and whether a change kept the AUDIO. V12, 2026-10-26.
// A dev tool beside test-dab-faad (which is the pass/fail gate). Inputs are the real off-air unit logs written by
// dab-unitlog (~/VibeSDR-test-audio/dab/logs — other people's broadcasts, never in the repo).
//
//   dab-faad-bench run [--repeat N] [--out f.f32] <log.dabu>…   AacDecoderFaad as the service drives it (units decoded,
//        holes concealed): CPU split decode / resample / rest, per unit and as % of one core per stream; --out writes
//        the 48 kHz interleaved stereo float output (all logs, in order).
//   dab-faad-bench raw --out f.f32 <log.dabu>…                   FAAD2 ALONE at its own rate (no resampler, no
//        concealment — holes skipped): the bytes to compare a patched FAAD2 against stock.
//   dab-faad-bench cmp <a.f32> <b.f32> [rate]                    max |a−b|, SNR of a against the difference, level,
//        worst third-octave difference (63 Hz – 16 kHz), side/mid of each — interleaved stereo float files.
//   dab-faad-bench resamp <log.dabu>…                            the 32→48 kHz resampler against a long double-precision
//        reference on the decoded audio (SNR over the band), and its measured frequency response.
// ★ Timing is CLOCK_THREAD_CPUTIME_ID inside AacDecoderFaad (the same counters the DAB panel shows).
#include "vibe_dab_aacdec.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(VIBE_HAVE_FAAD2_DAB)
int main() { std::printf("no plain FAAD2 (libfaad_dab) in this build\n"); return 3; }
#else
struct Slot { std::vector<uint8_t> adts; };
static bool loadLog(const char* path, std::vector<Slot>& L) {
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    char m[8]; if (std::fread(m, 1, 8, f) != 8 || std::memcmp(m, "VIBEDABU", 8) != 0) { std::fclose(f); return false; }
    uint32_t sf; uint8_t h[4]; uint16_t len;
    while (std::fread(&sf, 4, 1, f) == 1 && std::fread(h, 1, 4, f) == 4 && std::fread(&len, 2, 1, f) == 1) {
        Slot s; s.adts.resize(len); if (len && std::fread(s.adts.data(), 1, len, f) != len) break; L.push_back(std::move(s));
    }
    std::fclose(f); return !L.empty();
}
static bool readF32(const char* p, std::vector<float>& v) {
    FILE* f = std::fopen(p, "rb"); if (!f) return false; float b[8192]; size_t n;
    while ((n = std::fread(b, 4, 8192, f)) > 0) v.insert(v.end(), b, b + n); std::fclose(f); return true;
}
static void writeF32(const char* p, const std::vector<float>& v) { if (FILE* f = std::fopen(p, "wb")) { std::fwrite(v.data(), 4, v.size(), f); std::fclose(f); } }
static double cpuNow() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return double(t.tv_sec) + double(t.tv_nsec) * 1e-9; }

static int cmdRun(int argc, char** argv) {
    int repeat = 1; const char* out = nullptr; std::vector<const char*> logs;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else logs.push_back(argv[i]);
    }
    std::vector<float> all; double tDec = 0, tRes = 0, tTot = 0, secs = 0; long units = 0;
    for (const char* lp : logs) {
        std::vector<Slot> L; if (!loadLog(lp, L)) { std::printf("cannot read %s\n", lp); return 2; }
        double d = 0, r = 0, t = 0, s = 0; long u = 0;
        for (int rep = 0; rep < repeat; ++rep) {
            vibedab::AacDecoderFaad f; vibedab::AacPcm p; p.interleaved.reserve(L.size() * 6000);
            for (auto& x : L) { if (x.adts.empty()) f.conceal(1, p); else f.decode(x.adts.data(), x.adts.size(), p); }
            d += f.cpuDecodeSeconds(); r += f.cpuResampleSeconds(); t += f.cpuSeconds(); s += double(p.interleaved.size() / 2) / 48000.0; u += long(L.size());
            if (rep == 0 && out) all.insert(all.end(), p.interleaved.begin(), p.interleaved.end());
        }
        std::printf("%-28s %5.1f s x%d: decode %6.1f us/unit  resample %5.1f  rest %5.1f  = %.3f %% of a core (decode %.3f, resample %.3f)\n",
                    lp + (std::strrchr(lp, '/') ? std::strrchr(lp, '/') - lp + 1 : 0), s / repeat, repeat, 1e6 * d / double(u), 1e6 * r / double(u),
                    1e6 * (t - d - r) / double(u), 100 * t / s, 100 * d / s, 100 * r / s);
        tDec += d; tRes += r; tTot += t; secs += s; units += u;
    }
    std::printf("ALL %.1f s of programme: decode %.1f us/unit, resample %.1f, rest %.1f — %.3f %% of a core (decode %.3f %%, resample %.3f %%, rest %.3f %%)\n",
                secs, 1e6 * tDec / double(units), 1e6 * tRes / double(units), 1e6 * (tTot - tDec - tRes) / double(units),
                100 * tTot / secs, 100 * tDec / secs, 100 * tRes / secs, 100 * (tTot - tDec - tRes) / secs);
    if (out) writeF32(out, all);
    return 0;
}

extern "C" {
NeAACDecHandle vibedab_NeAACDecOpen(void);
NeAACDecConfigurationPtr vibedab_NeAACDecGetCurrentConfiguration(NeAACDecHandle);
unsigned char vibedab_NeAACDecSetConfiguration(NeAACDecHandle, NeAACDecConfigurationPtr);
char vibedab_NeAACDecInit2(NeAACDecHandle, unsigned char*, unsigned long, unsigned long*, unsigned char*);
void* vibedab_NeAACDecDecode(NeAACDecHandle, NeAACDecFrameInfo*, unsigned char*, unsigned long);
void vibedab_NeAACDecClose(NeAACDecHandle);
}
static int cmdRaw(int argc, char** argv) {
    const char* out = nullptr; std::vector<const char*> logs; int repeat = 1;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else logs.push_back(argv[i]);
    }
    std::vector<float> all; double cpu = 0, secs = 0; long units = 0;
    for (const char* lp : logs) {
        std::vector<Slot> L; if (!loadLog(lp, L)) return 2;
        for (int rep = 0; rep < repeat; ++rep) {
            NeAACDecHandle h = nullptr; int rate = 0;
            for (auto& x : L) {
                if (x.adts.size() < 9) continue;
                const uint8_t* a = x.adts.data();
                if (!h) {
                    const int profile = ((a[2] >> 6) & 3) + 1, sfi = (a[2] >> 2) & 15, ch = ((a[2] & 1) << 2) | (a[3] >> 6);
                    h = vibedab_NeAACDecOpen(); NeAACDecConfigurationPtr c = vibedab_NeAACDecGetCurrentConfiguration(h);
                    c->outputFormat = FAAD_FMT_FLOAT; c->downMatrix = 0; vibedab_NeAACDecSetConfiguration(h, c);
                    const uint16_t asc = uint16_t((profile << 11) | (sfi << 7) | (ch << 3) | (1 << 2));
                    unsigned char ab[2] = { uint8_t(asc >> 8), uint8_t(asc) }; unsigned long sr = 0; unsigned char chs = 0;
                    vibedab_NeAACDecInit2(h, ab, 2, &sr, &chs);
                }
                const size_t hdr = (a[1] & 1) ? 7 : 9;
                NeAACDecFrameInfo fi{};
                const double c0 = cpuNow();
                float* pcm = static_cast<float*>(vibedab_NeAACDecDecode(h, &fi, const_cast<uint8_t*>(a + hdr), (unsigned long)(x.adts.size() - hdr)));
                cpu += cpuNow() - c0; ++units;
                if (!pcm || fi.error || !fi.samples) continue;
                rate = int(fi.samplerate);
                const size_t fr = fi.samples / fi.channels;
                secs += double(fr) / double(rate);
                if (rep == 0) for (size_t i = 0; i < fr; ++i) { all.push_back(pcm[i * fi.channels]); all.push_back(pcm[i * fi.channels + (fi.channels > 1)]); }
            }
            if (h) vibedab_NeAACDecClose(h);
            if (rep == 0) std::printf("%s: %d Hz\n", lp, rate);
        }
    }
    std::printf("FAAD2 alone: %.1f us/unit, %.3f %% of a core (%.1f s of programme)\n", 1e6 * cpu / double(units), 100 * cpu / secs, secs);
    if (out) writeF32(out, all);
    return 0;
}

static void spectrumThirds(const std::vector<float>& x, int ch, double rate, std::vector<double>& bands) {
    const size_t N = 8192; std::vector<double> acc(N / 2 + 1, 0.0); size_t frames = x.size() / 2, nwin = 0;
    std::vector<std::complex<double>> b(N);
    for (size_t s = 0; s + N <= frames; s += N / 2, ++nwin) {
        for (size_t i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(N)); b[i] = x[2 * (s + i) + size_t(ch)] * w; }
        // radix-2 FFT
        for (size_t i = 1, j = 0; i < N; ++i) { size_t bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(b[i], b[j]); }
        for (size_t len = 2; len <= N; len <<= 1) { const double ang = -2 * M_PI / double(len); const std::complex<double> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < N; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; ++j) { auto u = b[i + j], v = b[i + j + len / 2] * w; b[i + j] = u + v; b[i + j + len / 2] = u - v; w *= wl; } } }
        for (size_t k = 0; k <= N / 2; ++k) acc[k] += std::norm(b[k]);
    }
    bands.clear();
    for (double fc = 63; fc <= 16001; fc *= std::pow(2.0, 1.0 / 3)) {
        const double lo = fc / std::pow(2.0, 1.0 / 6), hi = fc * std::pow(2.0, 1.0 / 6); double e = 1e-30;
        for (size_t k = 0; k <= N / 2; ++k) { const double f = double(k) * rate / double(N); if (f >= lo && f < hi) e += acc[k]; }
        bands.push_back(10 * std::log10(e / double(std::max<size_t>(1, nwin))));
    }
}
static int cmdCmp(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<float> a, b; if (!readF32(argv[0], a) || !readF32(argv[1], b)) { std::printf("cannot read\n"); return 2; }
    const double rate = argc > 2 ? std::atof(argv[2]) : 48000.0;
    /* ★ different filters have different delays: b is shifted to the lag (±64 frames) that matches a best */
    {
        const size_t m = std::min(a.size(), b.size()) / 2; int bl = 0; double be = 1e300;
        for (int lag = -64; lag <= 64; ++lag) { double e = 0; for (size_t k = 200000; k < 400000 && k + 64 < m; ++k) { const double d = double(a[2 * k]) - b[2 * size_t((long long)k + lag)]; e += d * d; } if (e < be) { be = e; bl = lag; } }
        if (bl > 0) b.erase(b.begin(), b.begin() + 2 * bl); else if (bl < 0) a.erase(a.begin(), a.begin() - 2 * bl);
        if (bl) std::printf("(aligned: b leads by %d frames)\n", bl);
    }
    const size_t n = std::min(a.size(), b.size());
    double ea = 0, ed = 0, eb = 0, mx = 0; size_t ndiff = 0;
    double mid = 0, side = 0, midb = 0, sideb = 0;
    for (size_t i = 0; i < n; ++i) { const double d = double(a[i]) - b[i]; ea += double(a[i]) * a[i]; eb += double(b[i]) * b[i]; ed += d * d; mx = std::max(mx, std::fabs(d)); ndiff += a[i] != b[i]; }
    for (size_t i = 0; i + 1 < n; i += 2) { const double m = 0.5 * (a[i] + a[i + 1]), s = 0.5 * (a[i] - a[i + 1]); mid += m * m; side += s * s;
                                            const double m2 = 0.5 * (b[i] + b[i + 1]), s2 = 0.5 * (b[i] - b[i + 1]); midb += m2 * m2; sideb += s2 * s2; }
    std::vector<double> ba, bb, bl, br; std::vector<float> a2(a.begin(), a.begin() + long(n)), b2(b.begin(), b.begin() + long(n));
    spectrumThirds(a2, 0, rate, ba); spectrumThirds(b2, 0, rate, bb); spectrumThirds(a2, 1, rate, bl); spectrumThirds(b2, 1, rate, br);
    double worst = 0; for (size_t k = 0; k < ba.size(); ++k) { worst = std::max(worst, std::fabs(ba[k] - bb[k])); worst = std::max(worst, std::fabs(bl[k] - br[k])); }
    std::printf("samples %zu vs %zu; %zu differ; max |diff| %.3g; SNR %.1f dB; level a %.3f dB b %.3f dB (diff %.4f dB); worst third-octave diff %.4f dB; side/mid a %.3f dB b %.3f dB\n",
                a.size(), b.size(), ndiff, mx, ed > 0 ? 10 * std::log10(ea / ed) : 999.0, 10 * std::log10(ea / double(n) + 1e-30), 10 * std::log10(eb / double(n) + 1e-30),
                10 * std::log10((ea + 1e-30) / (eb + 1e-30)), worst, 10 * std::log10(side / mid + 1e-30), 10 * std::log10(sideb / midb + 1e-30));
    return 0;
}

/** A long double-precision windowed-sinc reference for 32 → 48 kHz: 256 taps a branch, Kaiser β 12, cut-off 0.975. */
static void refUp(const std::vector<float>& in, std::vector<float>& out, int P, double beta, double cut) {
    const int L = 3, M = 2, N = L * P; const double fc = 0.5 * cut / L;
    std::vector<double> h(static_cast<size_t>(N)); double sum = 0;
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 60; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    for (int n = 0; n < N; ++n) { const double x = n - (N - 1) / 2.0; const double s = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
                                  const double r = 2.0 * n / (N - 1) - 1.0; h[size_t(n)] = s * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta); sum += h[size_t(n)]; }
    const size_t frames = in.size() / 2; out.clear();
    // output j at up-sampled position j*M; up-sampled signal u[k] = in[k/L] when k%L==0; y = sum_n h[n] u[jM - n + (N-1)]...
    // aligned like DabUpsampler: output j uses inputs base..base+P-1 with base = (j*M)/L, branch (j*M)%L, taps h[branch + (P-1-m)*L]
    for (long long ph = 0;; ph += M) {
        const size_t base = size_t(ph / L); if (base + size_t(P) > frames + size_t(P - 1)) break;
        const int br = int(ph % L); double l = 0, r = 0;
        for (int m = 0; m < P; ++m) { const long long idx = (long long)base + m - (P - 1); if (idx < 0 || idx >= (long long)frames) continue;
                                      const double t = h[size_t(br + (P - 1 - m) * L)] * L / sum; l += t * in[2 * size_t(idx)]; r += t * in[2 * size_t(idx) + 1]; }
        out.push_back(float(l)); out.push_back(float(r));
    }
}
static int cmdResamp(int argc, char** argv) {
    // decode raw 32 kHz, run DabUpsampler (as shipped) and the reference; compare after aligning the group delays
    std::vector<float> raw;
    for (int i = 0; i < argc; ++i) {
        std::vector<Slot> L; if (!loadLog(argv[i], L)) return 2;
        vibedab::AacDecoderFaad dummy; (void)dummy;
        NeAACDecHandle h = nullptr;
        for (auto& x : L) {
            if (x.adts.size() < 9) continue; const uint8_t* a = x.adts.data();
            if (!h) { const int profile = ((a[2] >> 6) & 3) + 1, sfi = (a[2] >> 2) & 15, ch = ((a[2] & 1) << 2) | (a[3] >> 6);
                      h = vibedab_NeAACDecOpen(); NeAACDecConfigurationPtr c = vibedab_NeAACDecGetCurrentConfiguration(h); c->outputFormat = FAAD_FMT_FLOAT; c->downMatrix = 0; vibedab_NeAACDecSetConfiguration(h, c);
                      const uint16_t asc = uint16_t((profile << 11) | (sfi << 7) | (ch << 3) | (1 << 2)); unsigned char ab[2] = { uint8_t(asc >> 8), uint8_t(asc) }; unsigned long sr = 0; unsigned char chs = 0; vibedab_NeAACDecInit2(h, ab, 2, &sr, &chs); }
            const size_t hdr = (a[1] & 1) ? 7 : 9; NeAACDecFrameInfo fi{};
            float* pcm = static_cast<float*>(vibedab_NeAACDecDecode(h, &fi, const_cast<uint8_t*>(a + hdr), (unsigned long)(x.adts.size() - hdr)));
            if (!pcm || fi.error || !fi.samples || fi.samplerate != 32000) continue;
            const size_t fr = fi.samples / fi.channels;
            for (size_t k = 0; k < fr; ++k) { raw.push_back(pcm[k * fi.channels]); raw.push_back(pcm[k * fi.channels + (fi.channels > 1)]); }
        }
        if (h) vibedab_NeAACDecClose(h);
    }
    if (raw.empty()) { std::printf("no 32 kHz audio in these logs\n"); return 2; }
    vibedab::DabUpsampler up; up.configure(32000, 48000);
    std::vector<float> got; for (size_t s = 0; s < raw.size() / 2; s += 1920) up.process(raw.data() + 2 * s, std::min<size_t>(1920, raw.size() / 2 - s), got);
    std::vector<float> ref; refUp(raw, ref, 256, 12.0, 0.975);
    // align: DabUpsampler delay (P-1)/2 inputs of history vs the reference's (256-1)/2 — find the best lag
    const size_t n = std::min(got.size(), ref.size()) / 2;
    int bestLag = 0; double best = 1e300;
    for (int lag = -400; lag <= 400; ++lag) { double e = 0; for (size_t k = 48000; k < 48000 + 48000 && k + 400 < n; ++k) { const long long j = (long long)k + lag; const double d = double(got[2 * k]) - ref[2 * size_t(j)]; e += d * d; } if (e < best) { best = e; bestLag = lag; } }
    double es = 0, ed = 0;
    for (size_t k = 1000; k + 1000 < n; ++k) { const long long j = (long long)k + bestLag; for (int c = 0; c < 2; ++c) { const double r = ref[2 * size_t(j) + size_t(c)], d = double(got[2 * k + size_t(c)]) - r; es += r * r; ed += d * d; } }
    std::printf("32→48 kHz, %.1f s of real audio: DabUpsampler vs a 256-tap double reference: SNR %.1f dB (lag %d)\n",
                double(n) / 48000.0, 10 * std::log10(es / ed), bestLag);
    // response: a sweep of tones through DabUpsampler
    std::printf("response (dB):");
    for (double f : { 1000.0, 5000.0, 10000.0, 12000.0, 13000.0, 13800.0, 14500.0, 15000.0, 15500.0 }) {
        vibedab::DabUpsampler u; u.configure(32000, 48000);
        std::vector<float> x(2 * 32000), y; for (size_t k = 0; k < 32000; ++k) x[2 * k] = x[2 * k + 1] = float(0.5 * std::sin(2 * M_PI * f * double(k) / 32000.0));
        u.process(x.data(), 32000, y);
        // amplitude at f and at the image 32000-f, in the steady part
        auto amp = [&](double fr) { double re = 0, im = 0; size_t c = 0; for (size_t k = 4800; k < 43200; ++k, ++c) { re += y[2 * k] * std::cos(2 * M_PI * fr * double(k) / 48000.0); im += y[2 * k] * std::sin(2 * M_PI * fr * double(k) / 48000.0); } return 2 * std::sqrt(re * re + im * im) / double(c); };
        std::printf("  %.1fk %+.2f (image %.0f)", f / 1000, 20 * std::log10(amp(f) / 0.5), 20 * std::log10(amp(32000 - f) / 0.5 + 1e-12));
    }
    std::printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: dab-faad-bench run|raw|cmp|resamp …\n"); return 2; }
    const std::string c = argv[1];
    if (c == "run") return cmdRun(argc - 2, argv + 2);
    if (c == "raw") return cmdRaw(argc - 2, argv + 2);
    if (c == "cmp") return cmdCmp(argc - 2, argv + 2);
    if (c == "resamp") return cmdResamp(argc - 2, argv + 2);
    return 2;
}
#endif
