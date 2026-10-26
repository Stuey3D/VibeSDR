// test-band-slice.cpp — BAND SPOTS' IQ SLICE, the server's own route (V12, 2026-10-27).
//
// test-digi-spots feeds the spotter a recording's IQ directly. The server does not: it cuts the band out of the radio's
// whole capture with the channeliser (vibedsp::Channelizer, a slice of the forward FFT — vibe_wide_tap.h on a one-pipeline
// radio, the per-listener extract on a range-locked one), and only then hands it to digi::Spotter. This test is that route.
//
//   test-band-slice                                   the tests (tones through the slice, every band, every rate)
//   test-band-slice real <wav> <utc> <recCentreHz> <dialHz> [L=25] [hw=<offset Hz>] [direct] [chan]
//        a 96 kHz I/Q recording interpolated ×L to a radio's capture (25 → 2.4 MS/s) and played through the real WideTap
//        (or `chan`: the range-locked extract; `direct`: no channeliser, as test-digi-spots) into a Band spotter.
#include "vibe_wide_tap.h"
#include "vibe_clock.h"
#include "vibe_band_slice.h"
#include "decoders/digi_bands.h"
#include "decoders/digi_spotter.h"
#include "decoders/digi_tuner.h"
#include "decoders/digi_activity.h"
#include <atomic>
#include <sys/resource.h>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

/** The server's own FFT size for a capture rate (local_sdr_shim.cpp fftSizeForRate — copied: the shim is not linkable). */
static int fftSizeForRate(double rate) { double want = rate / 75.0; int s = 4096; while (s < (int)want && s < 32768) s *= 2; return s; }

/** One tone at `offHz` from the capture's centre through the slice: its level (dB re the input) where it should be, and
 *  the level at its MIRROR (a spectrally inverted slice would put it there). */
struct Resp { double db, mirrorDb; };
static Resp toneThrough(double fs, int fft, int bins, int centreBin, double offHz) {
    vibedsp::Channelizer ch(fft);
    vibedsp::Channelizer::ExtractCtx ctx;
    std::vector<cf32> slice((size_t)bins), out;
    const int n = fft * 24;
    std::vector<cf32> x((size_t)n);
    const double w = 2 * M_PI * offHz / fs;
    for (int i = 0; i < n; ++i) x[(size_t)i] = cf32((float)std::cos(w * i), (float)std::sin(w * i));
    ch.feed(x.data(), n, [&](const cf32* b, int) {
        const int got = ch.extract(b, centreBin, bins, slice.data(), ctx, ch.blockIndex());
        out.insert(out.end(), slice.begin(), slice.begin() + got);
    });
    const double rate = fs * bins / fft, binHz = fs / fft, f = offHz - centreBin * binHz;
    // skip the first quarter (start-up), correlate the rest at +f and at −f
    const size_t s0 = out.size() / 4;
    std::complex<double> a(0, 0), m(0, 0);
    for (size_t i = s0; i < out.size(); ++i) {
        const double ph = 2 * M_PI * f * (double)i / rate;
        a += std::complex<double>(out[i]) * std::polar(1.0, -ph);
        m += std::complex<double>(out[i]) * std::polar(1.0, ph);
    }
    const double N = (double)(out.size() - s0);
    return { 20 * std::log10(std::abs(a) / N + 1e-12), 20 * std::log10(std::abs(m) / N + 1e-12) };
}

// ── real recordings ───────────────────────────────────────────────────────────────────────────────────────────────────
static bool loadIq(const char* path, std::vector<cf32>& x, uint32_t& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> raw; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) raw.insert(raw.end(), b, b + k); }
    std::fclose(f);
    uint16_t chans = 0; size_t data = 0, dlen = 0; rate = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        const uint32_t len = raw[p + 4] | raw[p + 5] << 8 | raw[p + 6] << 16 | (uint32_t)raw[p + 7] << 24;
        if (!std::memcmp(&raw[p], "fmt ", 4)) { chans = raw[p + 10] | raw[p + 11] << 8; rate = raw[p + 12] | raw[p + 13] << 8 | raw[p + 14] << 16 | (uint32_t)raw[p + 15] << 24; }
        if (!std::memcmp(&raw[p], "data", 4)) { data = p + 8; dlen = std::min<size_t>(len, raw.size() - data); break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 2 || !rate || !data) return false;
    x.resize(dlen / 4);
    for (size_t i = 0; i < x.size(); ++i) {
        const int16_t a = (int16_t)(raw[data + 4 * i] | raw[data + 4 * i + 1] << 8), b = (int16_t)(raw[data + 4 * i + 2] | raw[data + 4 * i + 3] << 8);
        x[i] = cf32(a / 32768.0f, b / 32768.0f);
    }
    return true;
}

/** ×L polyphase interpolator (Blackman-windowed sinc, 16 taps a phase), then a shift by `shiftHz` at the output rate. */
struct Interp {
    int L, K = 16; std::vector<float> h; std::vector<cf32> hist; double ph = 0, dph;
    Interp(int l, double inRate, double shiftHz) : L(l), hist((size_t)16, cf32(0, 0)) {
        const int n = L * K; h.resize((size_t)n);
        const double fc = 0.47 / L;   // cycles per output sample: 0.47 of the input rate
        for (int k = 0; k < n; ++k) {
            const double m = k - (n - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * k / (n - 1)) + 0.08 * std::cos(4 * M_PI * k / (n - 1));
            h[(size_t)k] = (float)(s * w * L);
        }
        dph = 2 * M_PI * shiftHz / (inRate * L);
    }
    void run(const cf32* in, int n, std::vector<cf32>& out) {
        out.resize((size_t)n * L);
        for (int i = 0; i < n; ++i) {
            std::memmove(hist.data() + 1, hist.data(), (size_t)(K - 1) * sizeof(cf32));
            hist[0] = in[i];
            for (int p = 0; p < L; ++p) {
                float re = 0, im = 0;
                for (int k = 0; k < K; ++k) { const float c = h[(size_t)(k * L + p)]; re += c * hist[(size_t)k].real(); im += c * hist[(size_t)k].imag(); }
                const cf32 r((float)std::cos(ph), (float)std::sin(ph));
                out[(size_t)i * L + p] = cf32(re, im) * r;
                ph += dph; if (ph > M_PI) ph -= 2 * M_PI;
            }
        }
    }
};

static int runReal(int argc, char** argv) {
    const char* path = argv[2];
    const double utc0 = std::atof(argv[3]), recCentre = std::atof(argv[4]), dial = std::atof(argv[5]);
    int L = 25; double hwOff = 15000; bool direct = false, chan = false, live = false; double secs = 1e9, stallMs = 0;
    for (int i = 6; i < argc; ++i) {
        if (!std::strncmp(argv[i], "L=", 2)) L = std::atoi(argv[i] + 2);
        if (!std::strncmp(argv[i], "hw=", 3)) hwOff = std::atof(argv[i] + 3);
        if (!std::strncmp(argv[i], "secs=", 5)) secs = std::atof(argv[i] + 5);
        if (!std::strcmp(argv[i], "direct")) direct = true;
        if (!std::strcmp(argv[i], "chan")) chan = true;
        if (!std::strcmp(argv[i], "live")) live = true;                         // ★ real time, stamped as the server stamps
        if (!std::strncmp(argv[i], "stall=", 6)) stallMs = std::atof(argv[i] + 6);   // ★ the tap starved after each slot
    }
    std::vector<cf32> x; uint32_t rate = 0;
    if (!loadIq(path, x, rate)) { std::printf("cannot read %s\n", path); return 3; }
    if (x.size() > secs * rate) x.resize((size_t)(secs * rate));
    std::map<std::string, std::set<std::string>> calls; std::map<std::string, int> per; std::mutex gm;
    digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& s, double) {
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; calls[s.mode].insert(s.call); }, nullptr, nullptr);
    const double fs = (double)rate * L, hwCentre = dial + hwOff;
    const int fft = fftSizeForRate(fs);
    const double width = std::min(DecoderHostBandWidth(dial), fs * 0.8);
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<uint64_t> consumed{0};   // capture samples the slice has delivered
    if (direct) {
        // the recording itself, re-centred on nothing: what test-digi-spots does
        for (size_t o = 0; o + 384 <= x.size(); o += 384) sp.feedIq(&x[o], 384, rate, recCentre, utc0 + (o + 384.0) / rate);
    } else {
        Interp ip(L, rate, recCentre - hwCentre);
        std::vector<cf32> up;
        const double binHz = fs / fft;
        const int bins = vibe::bandSliceBins(fs, fft, width);
        const int cb = (int)std::lround((dial - hwCentre) / binHz);
        const double srate = fs * bins / fft, scentre = hwCentre + cb * binHz;
        std::printf("capture %.0f S/s, FFT %d, slice %d bins = %.0f S/s centred %.1f Hz (width asked %.0f Hz)\n", fs, fft, bins, srate, scentre, width);
        WideTap tap;
        vibedsp::Channelizer ch(fft); vibedsp::Channelizer::ExtractCtx ctx; std::vector<cf32> sl((size_t)bins); std::vector<float> pw((size_t)bins);
        /* ★ `live`: the server's stamp — DecoderHost::feedIq takes vibeUtcNow() when the tap hands the block over —
         *  on a clock shifted by whole two-minute steps so the recording's slots still line up. `stall`: the tap thread
         *  held off for that long once a slot (the slot's decodes waking on a small box), its queue filling meanwhile. */
        const double shift = live ? std::ceil((vibeUtcNow() - utc0 + 60.0) / 120.0) * 120.0 : 0.0;
        double lastStallSlot = -1;
        auto sink = [&](const cf32* s, int k, double r, double c, const float* p, int nb, double bh) {
            const uint64_t cap = consumed.fetch_add((uint64_t)k * fft / bins) + (uint64_t)k * fft / bins;
            const double tcap = utc0 + (double)cap / fs;
            if (stallMs > 0) {
                const double slot = std::floor(tcap / 15.0);
                if (slot != lastStallSlot && tcap - slot * 15.0 > 0.3) { lastStallSlot = slot; std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(stallMs * 1000))); }
            }
            sp.feedIq(s, k, r, c, live ? vibeUtcNow() : tcap, p, nb, bh);
        };
        if (!chan) {
            tap.start(WideTap::BinSink(sink));
            WideTap::Geometry g; g.sampleRate = fs; g.hwCentreHz = hwCentre; g.wantCentreHz = dial; g.widthHz = width; g.fftSize = fft; g.bins = true;
            tap.aim(g);
        }
        uint64_t produced = 0;
        const int chunk = (int)rate / 20;
        for (size_t o = 0; o < x.size(); o += (size_t)chunk) {
            const int k = (int)std::min<size_t>((size_t)chunk, x.size() - o);
            ip.run(&x[o], k, up);
            produced += up.size();
            if (chan) {
                ch.feed(up.data(), (int)up.size(), [&](const cf32* b, int) {
                    const int got = ch.extract(b, cb, bins, sl.data(), ctx, ch.blockIndex());
                    digi::slicePower(b, fft, cb, bins, pw.data());
                    if (got > 0) sink(sl.data(), got, srate, scentre, pw.data(), bins, binHz);
                });
            } else {
                tap.feed(up.data(), (int)up.size());
                if (live) { std::this_thread::sleep_until(t0 + std::chrono::microseconds((int64_t)(1e6 * (double)produced / fs))); continue; }
                // ★ back-pressure: the tap drops what is more than half a second behind, as on a server that cannot keep up
                while (produced > consumed.load() + (uint64_t)(fs * 0.2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!chan) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); std::printf("tap dropped %llu blocks\n", (unsigned long long)tap.droppedBlocks()); tap.stop(); }
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::printf("%s: %s, %.0f s in %.1f s wall; engines %s; slots decoded %llu\n", path, direct ? "direct" : chan ? "range-locked extract" : "WideTap",
                x.size() / (double)rate, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), sp.describe().c_str(),
                (unsigned long long)sp.slotsDecoded());
    for (auto& kv : per) std::printf("%s: %d spots, %zu calls\n", kv.first.c_str(), kv.second, calls[kv.first].size());
    for (auto& m : sp.status())
        std::printf("  status %-5s %-6s dials %d  periods: live %llu idle %llu shed %llu\n", m.mode.c_str(), digi::stateWord(m.state), m.dials,
                    (unsigned long long)m.live, (unsigned long long)m.idle, (unsigned long long)m.shed);
    if (const char* f = getenv("VIBE_CALLS_OUT")) {
        FILE* o = std::fopen(f, "w");
        if (o) { for (auto& kv : calls) for (auto& c : kv.second) std::fprintf(o, "%s %s\n", kv.first.c_str(), c.c_str()); std::fclose(o); }
    }
    return 0;
}

/** `tapcost [rate]`: the WideTap's own CPU per second of capture (noise in, the slice dropped), process CPU minus the
 *  generator's. */
static double cpuNow() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }
static int tapCost(double fs) {
    const int fft = fftSizeForRate(fs);
    std::vector<cf32> x((size_t)(fs / 10));
    uint32_t r = 1; for (auto& v : x) { r = r * 1664525u + 1013904223u; const float a = (float)(r >> 8) / 16777216.0f - 0.5f; r = r * 1664525u + 1013904223u; v = cf32(a, (float)(r >> 8) / 16777216.0f - 0.5f); }
    std::atomic<uint64_t> got{0};
    WideTap tap;
    tap.start(WideTap::BinSink([&](const cf32*, int k, double, double, const float*, int, double) { got += (uint64_t)k; }));
    WideTap::Geometry g; g.sampleRate = fs; g.hwCentreHz = 7075600; g.wantCentreHz = 7060600; g.widthHz = 41900; g.fftSize = fft; g.bins = true;
    tap.aim(g);
    const int secs = 30;
    const double c0 = cpuNow(); const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < secs * 10; ++i) {
        tap.feed(x.data(), (int)x.size());
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(25 * (i + 1)));   // 4× real time: never behind on a Mac
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double c = cpuNow() - c0;
    std::printf("WideTap at %.0f S/s (FFT %d): %.2f %% of a core (%.3f s CPU for %d s of capture), dropped %llu, slice samples %llu\n",
                fs, fft, 100.0 * c / secs, c, secs, (unsigned long long)tap.droppedBlocks(), (unsigned long long)got.load());
    tap.stop();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "tapcost")) return tapCost(argc > 2 ? std::atof(argv[2]) : 2400000);
    if (argc > 5 && !std::strcmp(argv[1], "real")) return runReal(argc, argv);
    const bool verbose = argc > 1 && !std::strcmp(argv[1], "-v");

    std::printf("── the slice's response at every mode's passband edges, every band, every capture rate ──\n");
    {
        const double rates[] = { 2400000, 2048000, 1024000, 960000, 300000 };
        double worst = 0; std::string worstAt; double worstMirror = -999;
        for (double fs : rates) {
            const int fft = fftSizeForRate(fs);
            const double binHz = fs / fft;
            for (const digi::Band& b : digi::bands()) {
                const digi::Span sp = digi::spanOf(b, digi::modes());
                if (!sp.ok) continue;
                const double dial = 0.5 * (sp.loHz + sp.hiHz);           // ★ the Band dial: the span's centre
                const double width = std::min(DecoderHostBandWidth(dial), fs * 0.8);
                const int bins = vibe::bandSliceBins(fs, fft, width);
                const double hw = dial + 15000;                           // the capture's centre (an RTL's offset)
                const int cb = (int)std::lround((dial - hw) / binHz);
                const double srate = fs * bins / fft, scentre = hw + cb * binHz;
                for (const digi::ModeDef& m : digi::modes()) {
                    if (m.sampleRate != 12000) continue;
                    for (double d : digi::dialsOf(m, b.id)) {
                        if (!digi::DigiTuner::fits(d, m.audioLoHz, m.audioHiHz, srate, scentre)) continue;
                        for (double rf : { d + m.audioLoHz, d + m.audioHiHz }) {
                            const Resp r = toneThrough(fs, fft, bins, cb, rf - hw);
                            if (verbose) std::printf("    %4.0fk %-6s %-5s %.1f Hz: %+6.2f dB (mirror %+6.1f)\n", fs / 1e3, b.id, m.name.c_str(), rf, r.db, r.mirrorDb);
                            if (r.db < worst) { worst = r.db; char t[160]; std::snprintf(t, sizeof t, "%s %s %.0f Hz at %.0f S/s", b.id, m.name.c_str(), rf, fs); worstAt = t; }
                            worstMirror = std::max(worstMirror, r.mirrorDb);
                        }
                    }
                }
            }
        }
        char t[256];
        std::snprintf(t, sizeof t, "every mode's passband edge within 0.5 dB of a tone at the slice's centre (worst %.2f dB: %s)", worst, worstAt.c_str());
        ok(worst > -0.5, t);
        std::snprintf(t, sizeof t, "nothing at the mirror frequency — the slice is the right way up (worst %.1f dB)", worstMirror);
        ok(worstMirror < -60, t);
    }

    std::printf("── a +10 kHz tone comes out at +10 kHz (orientation) ──\n");
    {
        const double fs = 2400000; const int fft = fftSizeForRate(fs); const int bins = vibe::bandSliceBins(fs, fft, 42000);
        const Resp up = toneThrough(fs, fft, bins, 0, 10000), dn = toneThrough(fs, fft, bins, 0, -10000);
        char t[200]; std::snprintf(t, sizeof t, "+10 kHz: %.2f dB at +10, %.1f dB at −10; −10 kHz: %.2f dB at −10", up.db, up.mirrorDb, dn.db);
        ok(up.db > -0.5 && up.mirrorDb < -60 && dn.db > -0.5, t);
    }

    std::printf("%s\n", fails ? "SOME TESTS FAILED" : "all passed");
    return fails ? 1 : 0;
}
