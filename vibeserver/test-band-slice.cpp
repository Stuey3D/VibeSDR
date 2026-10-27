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
#include "decoders/jtty_decoder.h"
#include <random>
#include <algorithm>
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
static Resp toneThrough(double fs, int fft, int bins, int centreBin, double offHz, int taper = kBandTaperDiv,
                        std::vector<cf32>* keep = nullptr) {
    vibedsp::Channelizer ch(fft);
    vibedsp::Channelizer::ExtractCtx ctx;
    std::vector<cf32> slice((size_t)bins), out;
    const int n = fft * 24;
    std::vector<cf32> x((size_t)n);
    const double w = 2 * M_PI * offHz / fs;
    for (int i = 0; i < n; ++i) x[(size_t)i] = cf32((float)std::cos(w * i), (float)std::sin(w * i));
    ch.feed(x.data(), n, [&](const cf32* b, int) {
        const int got = ch.extract(b, centreBin, bins, slice.data(), ctx, ch.blockIndex(), taper);
        out.insert(out.end(), slice.begin(), slice.begin() + got);
    });
    if (keep) *keep = out;
    const double rate = fs * bins / fft, binHz = fs / fft, f = offHz - centreBin * binHz;
    // skip the first quarter (start-up), correlate the rest at +f and at −f
    const size_t s0 = out.size() / 4;
    std::complex<double> a(0, 0), m(0, 0);
    // ★ Hann-weighted, so a tone near the slice's centre (its mirror only a few hundred hertz away) does not leak into
    //   the mirror's reading through the correlation's own sidelobes
    double N = 0;
    for (size_t i = s0; i < out.size(); ++i) {
        const double ph = 2 * M_PI * f * (double)i / rate;
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - s0) / (double)(out.size() - s0 - 1));
        a += w * std::complex<double>(out[i]) * std::polar(1.0, -ph);
        m += w * std::complex<double>(out[i]) * std::polar(1.0, ph);
        N += w;
    }
    return { 20 * std::log10(std::abs(a) / N + 1e-12), 20 * std::log10(std::abs(m) / N + 1e-12) };
}


// ── synthetic signals ────────────────────────────────────────────────────────────────────────────────────────────────
static void addFsk(std::vector<cf32>& x, double rate, double offHz, double startSec, const std::vector<uint8_t>& tones, double sym, double amp) {
    const double spacing = 1.0 / sym;
    const size_t s0 = (size_t)(startSec * rate), ns = (size_t)std::llround(sym * rate);
    double ph = 0;
    for (size_t k = 0; k < tones.size(); ++k)
        for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + k * ns + j;
            if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] * spacing) / rate;
            x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
}
static void addJtty(std::vector<cf32>& x, double rate, double f0Off, double startSec, const char* text, double amp) {
    std::vector<uint64_t> frames; jtty::packMessage(text, frames);
    std::vector<uint8_t> tones;
    for (uint64_t f : frames) { uint8_t t[jtty::kFrameSyms]; jtty::encodeFrame(f, t); tones.insert(tones.end(), t, t + jtty::kFrameSyms); }
    addFsk(x, rate, f0Off, startSec, tones, 1.0 / jtty::kBaud, amp);
}
static double ampFor(double snrDb, double sigma, double rate) { return std::sqrt(std::pow(10.0, snrDb / 10.0) * 2 * sigma * sigma / rate * 2500.0); }

/** ★ The server's route, synchronously: a capture at `fs` centred `hw` → the channeliser → the main slice on `dial` and
 *  every extra slice the capture holds → a Band spotter, each block with its capture time. */
static void throughSlices(digi::Spotter& sp, const std::vector<cf32>& cap, double fs, double hw, double dial, double utc0) {
    const int fft = fftSizeForRate(fs);
    const double binHz = fs / fft;
    struct Sl { int bins, cb; double rate, centre; std::vector<cf32> buf; std::vector<float> pw; vibedsp::Channelizer::ExtractCtx ctx; };
    std::vector<Sl> sls;
    { Sl m; m.bins = bandSliceBins(fs, fft, std::min(DecoderHostBandWidth(dial), fs * 0.8)); m.cb = (int)std::lround((dial - hw) / binHz);
      m.rate = fs * m.bins / fft; m.centre = hw + m.cb * binHz; sls.push_back(std::move(m)); }
    for (const ExtraSlice& e : bandExtraSlices(dial)) {
        Sl x; x.bins = bandSliceBins(fs, fft, e.widthHz); x.cb = (int)std::lround((e.centreHz - hw) / binHz);
        x.rate = fs * x.bins / fft; x.centre = hw + x.cb * binHz;
        if (captureHolds(fs, hw, x.centre, x.rate)) sls.push_back(std::move(x));
    }
    vibedsp::Channelizer ch(fft);
    const int chunk = (int)(fs / 20);
    for (size_t o = 0; o < cap.size(); o += (size_t)chunk) {
        const int n = (int)std::min<size_t>((size_t)chunk, cap.size() - o);
        ch.feed(&cap[o], n, [&](const cf32* b, int) {
            const double utc = utc0 + (double)((ch.blockIndex() + 1) * (long long)ch.hop()) / fs;
            for (size_t i = 0; i < sls.size(); ++i) {
                Sl& s = sls[i];
                s.buf.resize((size_t)s.bins); s.pw.resize((size_t)s.bins);
                const int got = ch.extract(b, s.cb, s.bins, s.buf.data(), s.ctx, ch.blockIndex(), kBandTaperDiv);
                digi::slicePower(b, fft, s.cb, s.bins, s.pw.data());
                if (got > 0) sp.feedIq(s.buf.data(), got, s.rate, s.centre, utc, s.pw.data(), s.bins, binHz);
            }
        });
        if ((o / (size_t)chunk) % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // ★ the engines' workers
    }
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
        if (L == 1 && dph == 0) { out.assign(in, in + n); return; }
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

static std::map<std::string, std::set<std::string>> g_lastCalls;   // ★ the last runReal's calls per mode (the regression)
static int runReal(int argc, char** argv) {
    const char* path = argv[2];
    const double utc0 = std::atof(argv[3]), recCentre = std::atof(argv[4]), dial = std::atof(argv[5]);
    int L = 25; double hwOff = 15000; bool direct = false, chan = false, live = false, oldStamp = false, oldSlice = false; double secs = 1e9, stallMs = 0;
    for (int i = 6; i < argc; ++i) {
        if (!std::strncmp(argv[i], "L=", 2)) L = std::atoi(argv[i] + 2);
        if (!std::strncmp(argv[i], "hw=", 3)) hwOff = std::atof(argv[i] + 3);
        if (!std::strncmp(argv[i], "secs=", 5)) secs = std::atof(argv[i] + 5);
        if (!std::strcmp(argv[i], "direct")) direct = true;
        if (!std::strcmp(argv[i], "chan")) chan = true;
        if (!std::strcmp(argv[i], "live")) live = true;
        if (!std::strcmp(argv[i], "oldstamp")) oldStamp = true;
        if (!std::strcmp(argv[i], "oldslice")) oldSlice = true;   // ★ the tap as before 2026-10-27: w/0.92, a quarter's roll-off, no extras                         // ★ real time, stamped as the server stamps
        if (!std::strncmp(argv[i], "stall=", 6)) stallMs = std::atof(argv[i] + 6);   // ★ the tap starved after each slot
    }
    std::vector<cf32> x; uint32_t rate = 0;
    const size_t pl = std::strlen(path);
    if (pl > 4 && !std::strcmp(path + pl - 4, ".cu8")) {
        // ★ a server test's clip (test-digi-spots clip): 8-bit I/Q at fs=<rate>, looped `loops=` times (UTC-anchored at utc0)
        FILE* f = std::fopen(path, "rb"); if (!f) return 3;
        std::vector<uint8_t> u; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) u.insert(u.end(), b, b + k); }
        std::fclose(f);
        rate = 960000; int loops = 4;
        for (int i = 6; i < argc; ++i) { if (!std::strncmp(argv[i], "fs=", 3)) rate = (uint32_t)std::atof(argv[i] + 3); if (!std::strncmp(argv[i], "loops=", 6)) loops = std::atoi(argv[i] + 6); }
        for (int l = 0; l < loops; ++l) for (size_t i = 0; i + 1 < u.size(); i += 2) x.push_back(cf32((u[i] - 127.5f) / 128.0f, (u[i + 1] - 127.5f) / 128.0f));
        L = 1;
    } else if (!loadIq(path, x, rate)) { std::printf("cannot read %s\n", path); return 3; }
    if (!live && x.size() > secs * rate) x.resize((size_t)(secs * rate));
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
        const std::vector<ExtraSlice> extras = bandExtraSlices(dial);
        for (const ExtraSlice& e : extras) std::printf("extra slice %.1f kHz, %.1f kHz wide\n", e.centreHz / 1e3, e.widthHz / 1e3);
        /* ★ `live`: real time, on a clock that reads the recording's own UTC (utc0 + seconds since the start) — the
         *  producer is the DSP thread and stamps each chunk with it when it is captured. `oldstamp`: the stamp taken when
         *  the tap HANDS a block over, as DecoderHost::feedIq did before 2026-10-27. `stall`: the tap thread held off for
         *  that long once a slot (the slot's decodes waking on a small box), its queue filling meanwhile. */
        /* ★ `live` plays the recording on the REAL clock, as a server hears the air: it starts at the point of the recording
         *  whose UTC, moved on by whole two-minute steps (every mode's slots line up), is now — so the engines' wall-clock
         *  pass budgets see a live slot, not a recording hours old. */
        double shift = 0; size_t from = 0;
        if (live) {
            shift = std::floor((vibeUtcNow() - utc0) / 120.0) * 120.0;
            from = (size_t)((vibeUtcNow() - utc0 - shift) * rate);
            if (from + (size_t)(30 * rate) > x.size()) { shift += 120.0; from = 0; }   // too near the end: wait for the next lap
            while (vibeUtcNow() < utc0 + shift + (double)from / rate) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            x.erase(x.begin(), x.begin() + (std::ptrdiff_t)from);
            if (x.size() > secs * rate) x.resize((size_t)(secs * rate));
            std::printf("live from %.1f s into the recording, %.0f s\n", from / (double)rate, x.size() / (double)rate);
        }
        const double base = utc0 + shift + (double)from / rate;
        const double wall0 = vibeUtcNow();
        auto clk = [&] { return base + (vibeUtcNow() - wall0); };
        const auto tp = std::chrono::steady_clock::now();   // ★ the pacing starts after the wait
        double lastStallSlot = -1;
        auto sink = [&](const cf32* s, int k, double r, double c, const float* p, int nb, double bh, double utc, int slice) {
            if (slice == 0) consumed.fetch_add((uint64_t)std::llround(k * fs / r));
            if (stallMs > 0 && slice == 0) {
                const double slot = std::floor(utc / 15.0);
                if (slot != lastStallSlot && utc - slot * 15.0 > 0.3) { lastStallSlot = slot; std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(stallMs * 1000))); }
            }
            sp.feedIq(s, k, r, c, oldStamp ? (live ? clk() : utc) : utc, p, nb, bh);
        };
        if (!chan) {
            tap.start(WideTap::BandSink(sink));
            WideTap::Geometry g; g.sampleRate = fs; g.hwCentreHz = hwCentre; g.wantCentreHz = dial; g.widthHz = width; g.fftSize = fft; g.bins = true;
            g.band = !oldSlice; g.nExtras = oldSlice ? 0 : (int)std::min<size_t>(extras.size(), kMaxExtraSlices);
            for (int k = 0; k < g.nExtras; ++k) g.extras[k] = extras[(size_t)k];
            tap.aim(g);
        }
        uint64_t produced = 0;
        const int chunk = (int)rate / 20;
        std::vector<cf32> xb[kMaxExtraSlices]; vibedsp::Channelizer::ExtractCtx xc[kMaxExtraSlices]; std::vector<float> xp;
        for (size_t o = 0; o < x.size(); o += (size_t)chunk) {
            const int k = (int)std::min<size_t>((size_t)chunk, x.size() - o);
            ip.run(&x[o], k, up);
            produced += up.size();
            const double chunkUtc = live ? clk() : base + (double)produced / fs;
            if (chan) {
                long long inEnd = (long long)produced, inStart = inEnd - (long long)up.size();
                ch.feed(up.data(), (int)up.size(), [&](const cf32* b, int) {
                    const long long endIdx = std::min(inEnd, std::max(inStart, (ch.blockIndex() + 1) * (long long)ch.hop()));
                    const double utc = chunkUtc - (double)(inEnd - endIdx) / fs;
                    const int got = ch.extract(b, cb, bins, sl.data(), ctx, ch.blockIndex(), kBandTaperDiv);
                    digi::slicePower(b, fft, cb, bins, pw.data());
                    if (got > 0) sink(sl.data(), got, srate, scentre, pw.data(), bins, binHz, utc, 0);
                    for (size_t e = 0; e < extras.size() && e < (size_t)kMaxExtraSlices; ++e) {
                        const int eb = bandSliceBins(fs, fft, extras[e].widthHz);
                        const int ecb = (int)std::lround((extras[e].centreHz - hwCentre) / binHz);
                        const double er = fs * eb / fft, ec = hwCentre + ecb * binHz;
                        if (!captureHolds(fs, hwCentre, ec, er)) continue;
                        xb[e].resize((size_t)eb); xp.resize((size_t)eb);
                        const int eg = ch.extract(b, ecb, eb, xb[e].data(), xc[e], ch.blockIndex(), kBandTaperDiv);
                        digi::slicePower(b, fft, ecb, eb, xp.data());
                        if (eg > 0) sink(xb[e].data(), eg, er, ec, xp.data(), eb, binHz, utc, (int)e + 1);
                    }
                });
            } else {
                tap.feed(up.data(), (int)up.size(), chunkUtc);
                if (live) { std::this_thread::sleep_until(tp + std::chrono::microseconds((int64_t)(1e6 * (double)produced / fs))); continue; }
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
    g_lastCalls = calls;
    if (const char* f = getenv("VIBE_CALLS_OUT")) {
        FILE* o = std::fopen(f, "w");
        if (o) { for (auto& kv : calls) for (auto& c : kv.second) std::fprintf(o, "%s %s\n", kv.first.c_str(), c.c_str()); std::fclose(o); }
    }
    return 0;
}

/** `tapcost [rate]`: the WideTap's own CPU per second of capture (noise in, the slice dropped), process CPU minus the
 *  generator's. */
static double cpuNow() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }
static int tapCost(double fs, bool band) {
    const int fft = fftSizeForRate(fs);
    std::vector<cf32> x((size_t)(fs / 10));
    uint32_t r = 1; for (auto& v : x) { r = r * 1664525u + 1013904223u; const float a = (float)(r >> 8) / 16777216.0f - 0.5f; r = r * 1664525u + 1013904223u; v = cf32(a, (float)(r >> 8) / 16777216.0f - 0.5f); }
    std::atomic<uint64_t> got{0};
    WideTap tap;
    tap.start(WideTap::BandSink([&](const cf32*, int k, double, double, const float*, int, double, double, int) { got += (uint64_t)k; }));
    WideTap::Geometry g; g.sampleRate = fs; g.hwCentreHz = 7075600; g.wantCentreHz = 7060600; g.widthHz = 41900; g.fftSize = fft; g.bins = true;
    // ★ `tapcost <rate> band`: as Band runs it on 40 m — the flat sizing and 40 m's extra slice (JTTY)
    if (band) { g.band = true; const auto e = bandExtraSlices(7060600); g.nExtras = (int)e.size(); for (size_t k = 0; k < e.size(); ++k) g.extras[k] = e[k]; }
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
    if (argc > 1 && !std::strcmp(argv[1], "tapcost")) return tapCost(argc > 2 ? std::atof(argv[2]) : 2400000, argc > 3 && !std::strcmp(argv[3], "band"));
    if (argc > 5 && !std::strcmp(argv[1], "real")) return runReal(argc, argv);
    const bool verbose = argc > 1 && !std::strcmp(argv[1], "-v");

    std::printf("── the slice's response at every mode's passband edges, every band, every capture rate (main + extra slices) ──\n");
    {
        const double rates[] = { 2400000, 2048000, 1024000, 960000, 300000 };
        double worst = 0; std::string worstAt; double worstMirror = -999; int placed = 0, out = 0; std::string missing;
        for (double fs : rates) {
            const int fft = fftSizeForRate(fs);
            const double binHz = fs / fft;
            for (const digi::Band& b : digi::bands()) {
                const digi::Span sp = digi::spanOf(b, digi::modes());
                if (!sp.ok) continue;
                const double dial = sp.centreHz;                          // ★ the Band dial: the span's centre
                const double hw = dial + 15000;                           // the capture's centre (an RTL's offset)
                struct Sl { int bins, cb; double rate, centre; };
                std::vector<Sl> sls;
                {
                    const int bins = vibe::bandSliceBins(fs, fft, std::min(DecoderHostBandWidth(dial), fs * 0.8));
                    const int cb = (int)std::lround((dial - hw) / binHz);
                    sls.push_back({ bins, cb, fs * bins / fft, hw + cb * binHz });
                }
                for (const ExtraSlice& e : bandExtraSlices(dial)) {
                    const int eb = bandSliceBins(fs, fft, e.widthHz), ecb = (int)std::lround((e.centreHz - hw) / binHz);
                    const double er = fs * eb / fft, ec = hw + ecb * binHz;
                    if (captureHolds(fs, hw, ec, er)) sls.push_back({ eb, ecb, er, ec });
                }
                for (const digi::ModeDef& m : digi::modes()) {
                    if (m.sampleRate != 12000) continue;
                    for (double d : digi::dialsOf(m, b.id)) {
                        int best = -1; double bm = -1e300;
                        for (size_t i = 0; i < sls.size(); ++i) {
                            const double g = digi::DigiTuner::margin(d, m.audioLoHz, m.audioHiHz, sls[i].rate, sls[i].centre);
                            if (g >= 0 && g > bm) { bm = g; best = (int)i; }
                        }
                        if (best < 0) {
                            ++out;
                            // ★ only allowed where the capture cannot hold the dial at all
                            if (std::fabs(d + 0.5 * (m.audioLoHz + m.audioHiHz) - hw) + 12000 < fs * 0.45) { missing += std::string(" ") + b.id + " " + m.name; }
                            continue;
                        }
                        ++placed;
                        for (double rf : { d + m.audioLoHz, d + m.audioHiHz }) {
                            const Resp r = toneThrough(fs, fft, sls[(size_t)best].bins, sls[(size_t)best].cb, rf - hw);
                            if (verbose) std::printf("    %4.0fk %-6s %-5s %.1f Hz (slice %d): %+6.2f dB (mirror %+6.1f)\n", fs / 1e3, b.id, m.name.c_str(), rf, best, r.db, r.mirrorDb);
                            if (r.db < worst) { worst = r.db; char t[160]; std::snprintf(t, sizeof t, "%s %s %.0f Hz at %.0f S/s", b.id, m.name.c_str(), rf, fs); worstAt = t; }
                            worstMirror = std::max(worstMirror, r.mirrorDb);
                        }
                    }
                }
            }
        }
        char t[300];
        std::snprintf(t, sizeof t, "every mode's dial on every band is on a slice (%d placed; %d beyond a narrow capture, shown \"out\")%s%s",
                      placed, out, missing.empty() ? "" : " — MISSING:", missing.c_str());
        ok(missing.empty(), t);
        std::snprintf(t, sizeof t, "every mode's passband edge within 0.5 dB of a tone at the slice's centre (worst %.2f dB: %s)", worst, worstAt.c_str());
        ok(worst > -0.5, t);
        std::snprintf(t, sizeof t, "nothing at the mirror frequency — the slice is the right way up (worst %.1f dB)", worstMirror);
        ok(worstMirror < -60, t);
    }

    std::printf("── the narrow roll-off keeps the block-rate spurs down (the 2026-08-02 picket fence) ──\n");
    {
        const double fs = 2400000; const int fft = fftSizeForRate(fs); const int bins = vibe::bandSliceBins(fs, fft, 42000);
        double sp[3] = { 0, 0, 0 };
        const int tapers[3] = { 4, 8, 16 };
        for (int t = 0; t < 3; ++t) {
            std::vector<cf32> y;
            toneThrough(fs, fft, bins, 0, 12345.6, tapers[t], &y);
            // Hann-windowed 8192-point spectrum of the slice; the largest bin more than 40 bins from the tone, re the tone
            const size_t N = 8192, o = y.size() / 4;
            std::vector<cf32> a(N);
            for (size_t i = 0; i < N; ++i) a[i] = y[o + i] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / (N - 1)));
            std::vector<double> mag(N);
            vibedsp::ComplexFFT F((int)N, false);
            std::vector<cf32> A(N);
            F.forward(a.data(), A.data());
            size_t pk = 0; double pv = 0;
            for (size_t k = 0; k < N; ++k) { mag[k] = std::norm(A[k]); if (mag[k] > pv) { pv = mag[k]; pk = k; } }
            double spur = 0;
            for (size_t k = 0; k < N; ++k) { const long d = std::labs((long)k - (long)pk); const long dd = std::min<long>(d, (long)N - d); if (dd > 40) spur = std::max(spur, mag[k]); }
            sp[t] = 10 * std::log10(spur / pv + 1e-30);
        }
        char t[200]; std::snprintf(t, sizeof t, "largest spur re a tone: %.1f dB with a quarter's roll-off, %.1f an eighth's, %.1f a sixteenth's", sp[0], sp[1], sp[2]);
        ok(sp[kBandTaperDiv == 8 ? 1 : 2] < -90, t);
    }

    std::printf("── JTTY decodes in Band on every band it has a dial (main or extra slice), and every other mode keeps its place ──\n");
    {
        const double fs = 960000;
        for (const digi::Band& b : digi::bands()) {
            const digi::ModeDef* jm = digi::modeByName("JTTY");
            if (!jm) break;
            const std::vector<double> ds = digi::dialsOf(*jm, b.id);
            if (ds.empty()) continue;
            const digi::Span sp = digi::spanOf(b, digi::modes());
            const double dial = sp.centreHz, hw = dial + 15000, jd = ds[0];
            const double secs = 14, sigma = 0.01;
            std::vector<cf32> cap((size_t)(secs * fs), cf32(0, 0));
            const double rf = jd + 1500;
            addJtty(cap, fs, rf - hw, 2.0, "CQ G4JTY CQ", ampFor(-6, sigma, fs));
            { std::mt19937 g(11); std::normal_distribution<float> nd(0.0f, (float)sigma); for (auto& v : cap) v += cf32(nd(g), nd(g)); }
            std::vector<std::string> got; std::mutex gm;
            digi::Spotter spt(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& s, double f) {
                char t[96]; std::snprintf(t, sizeof t, "%s %s %.0f", s.mode.c_str(), s.call.c_str(), f);
                std::lock_guard<std::mutex> lk(gm); got.push_back(t); }, nullptr, nullptr);
            throughSlices(spt, cap, fs, hw, dial, 1800000000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            bool hit = false; std::string all;
            { std::lock_guard<std::mutex> lk(gm);
              for (auto& g : got) { all += " [" + g + "]"; char m[16], c[16]; double f = 0;
                if (std::sscanf(g.c_str(), "%15s %15s %lf", m, c, &f) == 3 && !std::strcmp(m, "JTTY") && !std::strcmp(c, "G4JTY") && std::fabs(f - rf) < 20) hit = true; } }
            // every mode the band carries is running (its own slice or the main one)
            std::set<std::string> running; for (auto& m : spt.status()) running.insert(m.mode);
            std::string lost; for (const std::string& m : sp.allModes()) if (!running.count(m)) lost += " " + m;
            char t[400];
            std::snprintf(t, sizeof t, "%s: JTTY G4JTY on %.3f MHz decoded in Band (dial %.4f, %s slice)%s%s", b.id, jd / 1e6, dial / 1e6,
                          std::find(sp.modes.begin(), sp.modes.end(), std::string("JTTY")) != sp.modes.end() ? "main" : "its own", hit ? "" : " — got:", hit ? "" : all.c_str());
            ok(hit, t);
            ok(lost.empty(), std::string(b.id) + ": every mode on the band runs (" + std::to_string(running.size()) + ")" + (lost.empty() ? "" : " — NOT RUNNING:" + lost));
        }
    }

    std::printf("── a +10 kHz tone comes out at +10 kHz (orientation) ──\n");
    {
        const double fs = 2400000; const int fft = fftSizeForRate(fs); const int bins = vibe::bandSliceBins(fs, fft, 42000);
        const Resp up = toneThrough(fs, fft, bins, 0, 10000), dn = toneThrough(fs, fft, bins, 0, -10000);
        char t[200]; std::snprintf(t, sizeof t, "+10 kHz: %.2f dB at +10, %.1f dB at −10; −10 kHz: %.2f dB at −10", up.db, up.mirrorDb, dn.db);
        ok(up.db > -0.5 && up.mirrorDb < -60 && dn.db > -0.5, t);
    }

    std::printf("── the real 40 m recording: Band through the server's slice route vs the same IQ straight in ──\n");
    {
        std::string dir = getenv("VIBE_TEST_AUDIO") ? getenv("VIBE_TEST_AUDIO") : std::string(getenv("HOME") ? getenv("HOME") : "") + "/VibeSDR-test-audio";
        const std::string wav = dir + "/bandall/band_40m_7066600_iq96_20261026T1738.wav";
        if (FILE* f = std::fopen(wav.c_str(), "rb")) {
            std::fclose(f);
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            auto run = [&](std::vector<std::string> extra) {
                std::vector<std::string> a = { "x", "real", wav, "1793036293.537", "7066600", "7060600", "secs=90" };
                a.insert(a.end(), extra.begin(), extra.end());
                std::vector<char*> av; for (auto& x : a) av.push_back(&x[0]);
                runReal((int)av.size(), av.data());
                return g_lastCalls;
            };
            const auto direct = run({ "direct" });
            const auto tap = run({});
            const auto chan = run({ "chan" });
            const size_t d = direct.count("FT8") ? direct.at("FT8").size() : 0;
            const size_t t = tap.count("FT8") ? tap.at("FT8").size() : 0, c = chan.count("FT8") ? chan.at("FT8").size() : 0;
            char m[220];
            std::snprintf(m, sizeof m, "FT8 calls in 90 s: straight in %zu, one-pipeline tap %zu, range-locked extract %zu — the slice within 10 %%", d, t, c);
            ok(d > 40 && t >= d * 0.9 && c >= d * 0.9, m);
            const size_t d4 = direct.count("FT4") ? direct.at("FT4").size() : 0, t4 = tap.count("FT4") ? tap.at("FT4").size() : 0;
            std::snprintf(m, sizeof m, "FT4 calls: straight in %zu, tap %zu", d4, t4);
            ok(t4 + 2 >= d4 * 0.9, m);
        } else std::printf("  (not run — %s is not here)\n", wav.c_str());
    }

    std::printf("%s\n", fails ? "SOME TESTS FAILED" : "all passed");
    return fails ? 1 : 0;
}
