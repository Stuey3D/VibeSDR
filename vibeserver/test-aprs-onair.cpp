// test-aprs-onair.cpp — APRS AS THE SERVER HEARS IT OFF AIR (docs/v12/APRS-ON-AIR-2.md, 2026-11-22).
//
// test-aprs feeds the modem clean AUDIO with mild white noise between packets. Off air it is different: a tracker KEYS UP,
// sends its preamble and frame on an FM carrier, and drops; between packets the discriminator has no carrier and puts out
// full-scale FM hiss. This drives the server's own chain: Bell 202 → FM (±3 kHz, the TX's pre-emphasis or none) → a
// 240 kS/s capture with complex white noise at a given CNR → vibedsp::RxPipeline NFM (the channel filter, discriminator,
// de-emphasis, Voice or ALL, the resampler) → vibeaprs::Run in AUTO (1200 on the audio + 9600 on the channel IQ, FX.25,
// IL2P), exactly what a listener's DecoderHost runs. A minimal reference chain (a FIR to ±8 kHz and an ideal
// discriminator) on the SAME IQ says how much the chain itself costs.
//   --sweep   the full table (slow); default: the regression rows with their thresholds.
#include "vibe_aprs_run.h"
#include "vibedsp/vibedsp.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using cf = std::complex<float>;
static int g_pass = 0, g_fail = 0;
static void check(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) ++g_pass; else ++g_fail; }

static void addr(std::vector<uint8_t>& b, const std::string& call, int ssid, bool last) {
    for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < int(call.size()) ? call[size_t(i)] : ' ') << 1));
    b.push_back(uint8_t(0x60 | (ssid << 1) | (last ? 1 : 0)));
}
static std::vector<uint8_t> ui(int i) {
    std::vector<uint8_t> b; addr(b, "APRS", 0, false); addr(b, "G4ABC", i % 16, false); addr(b, "WIDE1", 1, false); addr(b, "WIDE2", 1, true);
    b.push_back(3); b.push_back(0xF0);
    char info[120]; std::snprintf(info, sizeof info, "!%02d%02d.%02dN/00%d%02d.%02dW>on-air test #%03d# a typical tracker comment", 50 + i % 5, i % 60, (i * 7) % 100, i % 9, (i * 3) % 60, i % 100, i);
    b.insert(b.end(), info, info + std::strlen(info));
    return b;
}

struct Cfg {
    double cnrDb = 12;       ///< carrier to noise in 12.5 kHz
    double devHz = 3000;     ///< the louder tone's peak deviation
    double tiltDb = 0;       ///< the TX's 2200 Hz against its 1200 Hz (a mic input's pre-emphasis ≈ +5)
    int flags = 30;          ///< TXDELAY in flags (30 = 200 ms)
    double errHz = 0;        ///< the transmitter off the dial
    bool voice = false;      ///< the listener's NFM Voice (300 Hz – 3 kHz) instead of ALL
    double tau = 50e-6;      ///< the pipeline's de-emphasis (its default 50 µs; 0 = off)
    double bw = 12500;       ///< the listener's NFM filter
    double gainStep = 1;     ///< ×amplitude half-way through every packet (an RTL AGC / gain step)
    int packets = 24;
    uint64_t seed = 1;
};

/** The capture: 240 kS/s, the station 37 kHz above its centre, keyed only for each packet (and 30 ms of tail). */
static std::vector<cf> makeIq(const Cfg& c, double fs, double off) {
    std::vector<float> aud;      // 48 kHz modulating audio, normalised to the louder tone = 1
    std::vector<uint8_t> key;    // carrier on?
    const double sg = std::pow(10.0, c.tiltDb / 20), mg = c.tiltDb > 0 ? 1 / sg : 1.0, sp = c.tiltDb > 0 ? 1.0 : sg;
    double ph = 0;
    for (int i = 0; i < c.packets; ++i) {
        const size_t gap = 48000 + size_t((i * 7919) % 24000);    // 1.0–1.5 s of no carrier
        aud.insert(aud.end(), gap, 0.f); key.insert(key.end(), gap, 0);
        std::vector<uint8_t> f = ui(i); vibehdlc::appendFcs(f);
        std::vector<int> bits; for (int k = 0; k < c.flags; ++k) vibehdlc::flagBits(bits);
        vibehdlc::stuffBits(f, bits); vibehdlc::flagBits(bits); vibehdlc::flagBits(bits);
        std::vector<int> lv; int level = 1; vibehdlc::nrzi(bits, lv, level);
        double t = 0;
        for (size_t k = 0; k < lv.size(); ++k)
            for (; t < (k + 1) * 40.0; t += 1) { ph += 2 * M_PI * (lv[k] ? 1200 : 2200) / 48000.0; aud.push_back(float((lv[k] ? mg : sp) * std::sin(ph))); key.push_back(1); }
        aud.insert(aud.end(), 1440, 0.f); key.insert(key.end(), 1440, 1);   // the tail: carrier, no tone
    }
    aud.insert(aud.end(), 48000, 0.f); key.insert(key.end(), 48000, 0);
    // FM at 48 kHz (the deviation is tiny against 240 k, so an analytic 48 kHz baseband up-sampled is exact enough)
    std::vector<float> I(aud.size()), Q(aud.size());
    double phi = 0;
    for (size_t i = 0; i < aud.size(); ++i) { phi += 2 * M_PI * c.devHz * aud[i] / 48000.0; I[i] = key[i] ? float(std::cos(phi)) : 0.f; Q[i] = key[i] ? float(std::sin(phi)) : 0.f; }
    vibedsp::RationalResampler ri(48000, fs), rq(48000, fs);
    std::vector<float> Iu(size_t(ri.maxOut(int(I.size())))), Qu(Iu.size());
    const int n = ri.process(I.data(), int(I.size()), Iu.data());
    rq.process(Q.data(), int(Q.size()), Qu.data());
    std::vector<cf> iq{}; iq.resize(size_t(n));
    // noise: carrier power 1 (where keyed); complex noise σ² over fs, so in 12.5 kHz it is σ²·12500/fs
    const double sigma2 = 1.0 / std::pow(10.0, c.cnrDb / 10) * fs / 12500.0;
    std::mt19937 rng(uint32_t(c.seed * 977 + 13)); std::normal_distribution<float> nd(0.f, float(std::sqrt(sigma2 / 2)));
    // a gain step: ×gainStep on the whole capture (signal AND noise, as a tuner's gain is) from each packet's middle to the next gap
    bool stepped = false; size_t onRun = 0;
    for (int i = 0; i < n; ++i) {
        const size_t a = std::min(size_t(double(i) * 48000.0 / fs), key.size() - 1);
        if (key[a]) { ++onRun; if (!stepped && onRun > size_t(0.35 * fs)) stepped = true; } else { onRun = 0; stepped = false; }
        const double p = 2 * M_PI * (off + c.errHz) * i / fs;
        const float g = (c.gainStep != 1 && stepped) ? float(c.gainStep) : 1.f;
        iq[size_t(i)] = (cf(Iu[size_t(i)], Qu[size_t(i)]) * cf(float(std::cos(p)), float(std::sin(p))) + cf(nd(rng), nd(rng))) * g;
    }
    return iq;
}

/** The packets (by number) a run's JSON announced. */
struct Heard { std::set<int> got; int wrong = 0; };
static void noteJson(const std::string& j, Heard& h) {
    if (j.find("\"type\":\"aprs_packet\"") == std::string::npos) return;
    const size_t p = j.find("on-air test #");
    if (p == std::string::npos) { ++h.wrong; return; }
    h.got.insert(std::atoi(j.c_str() + p + 13));
}

/** The server's chain: RxPipeline NFM → audio (+ channel IQ) → Run AUTO. */
static Heard viaPipeline(const Cfg& c, const std::vector<cf>& iq, double fs, double off) {
    Heard h;
    vibeaprs::Run::Opts o; o.json = [&](const std::string& j) { noteJson(j, h); };
    vibeaprs::Run run(0, std::move(o));
    struct Ctx { vibeaprs::Run* r; } ctx{&run};
    vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
    cb.audio = [](void* x, const float* pcm, int frames, int ch, int) {
        auto* p = static_cast<Ctx*>(x);
        if (ch == 1) { p->r->feedAudio(pcm, frames); return; }
        std::vector<float> m; m.resize(size_t(frames)); for (int i = 0; i < frames; ++i) m[size_t(i)] = pcm[2 * i];
        p->r->feedAudio(m.data(), frames);
    };
    cb.iq = [](void* x, const vibedsp::cf32* z, int k, double rate) { static_cast<Ctx*>(x)->r->feedIq(reinterpret_cast<const std::complex<float>*>(z), k, rate); };
    vibedsp::RxPipeline p; p.setMeasureThread(false);
    p.setDeemphasis(c.tau);
    p.setNfmVoice(c.voice);
    p.start(fs, 1024, 20.0, 48000, cb);
    p.setTune(off, vibedsp::RxPipeline::Mode::NFM, c.bw);
    const int blk = int(fs / 50);
    for (size_t i = 0; i + size_t(blk) <= iq.size(); i += size_t(blk)) p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
    p.stop();
    return h;
}

/** The reference: mix to DC, one FIR to ±8 kHz, ÷5 to 48 kS/s, an ideal discriminator — nothing else. */
static Heard viaReference(const std::vector<cf>& iq, double fs, double off) {
    Heard h;
    vibeaprs::Run::Opts o; o.json = [&](const std::string& j) { noteJson(j, h); };
    vibeaprs::Run run(1200, std::move(o));
    const std::vector<float> lp = vibedsp::designLowpass(8000.0 / fs, 4000.0 / fs);
    const int K = int(lp.size()), D = int(std::lround(fs / 48000));
    std::vector<cf> ring; ring.resize(size_t(K)); int rp = 0;
    cf prev(1, 0);
    std::vector<float> out; out.reserve(4096);
    for (size_t i = 0; i < iq.size(); ++i) {
        const double p = -2 * M_PI * off * double(i) / fs;
        ring[size_t(rp)] = iq[i] * cf(float(std::cos(p)), float(std::sin(p))); rp = (rp + 1) % K;
        if (i % size_t(D)) continue;
        cf y(0, 0);
        for (int k = 0; k < K; ++k) y += ring[size_t((rp + k) % K)] * lp[size_t(k)];
        const float a = std::arg(y * std::conj(prev)); prev = y;
        out.push_back(a * float(48000.0 / (2 * M_PI * 6250.0)));   // full scale at ±6.25 kHz, as the pipeline's NFM 12.5 k
        if (out.size() == 960) { run.feedAudio(out.data(), int(out.size())); out.clear(); }
    }
    if (!out.empty()) run.feedAudio(out.data(), int(out.size()));
    return h;
}

static std::string desc(const Cfg& c) {
    char b[200];
    std::snprintf(b, sizeof b, "CNR %4.1f dB dev %.1fk TX %s TXD %d %s de-emph %s bw %.1fk%s%s", c.cnrDb, c.devHz / 1000, c.tiltDb > 0 ? "pre-emph" : c.tiltDb < 0 ? "de-emph" : "flat",
                  c.flags, c.voice ? "VOICE" : "ALL  ", c.tau > 0 ? "50us" : "off ", c.bw / 1000, c.errHz != 0 ? (" off " + std::to_string((int)c.errHz)).c_str() : "",
                  c.gainStep != 1 ? " +gain step" : "");
    return b;
}
struct Row { int pipe = 0, ref = 0, wrong = 0; };
static Row run1(const Cfg& c, bool ref, bool print = true) {
    const double fs = 240000, off = 37000;
    const std::vector<cf> iq = makeIq(c, fs, off);
    Row r;
    const Heard a = viaPipeline(c, iq, fs, off);
    r.pipe = int(a.got.size()); r.wrong = a.wrong;
    if (ref) { const Heard b = viaReference(iq, fs, off); r.ref = int(b.got.size()); r.wrong += b.wrong; }
    if (print) std::printf("   %-78s  server %2d/%d%s\n", desc(c).c_str(), r.pipe, c.packets, ref ? (" · reference " + std::to_string(r.ref) + "/" + std::to_string(c.packets)).c_str() : "");
    return r;
}

int main(int argc, char** argv) {
    const bool sweep = argc > 1 && std::strcmp(argv[1], "--sweep") == 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (sweep) {
        std::printf("── the sweep: the server's chain against the reference, the same IQ ──\n");
        for (double tilt : { 0.0, 5.0 })
            for (double cnr : { 6.0, 8.0, 10.0, 12.0, 15.0 }) {
                Cfg c; c.cnrDb = cnr; c.tiltDb = tilt; c.seed = uint64_t(cnr * 10 + tilt);
                run1(c, true);
                c.voice = true; run1(c, false);
                c.voice = false; c.tau = 0; run1(c, false);
            }
        std::printf("── deviation, TXDELAY, tuning, gain steps at CNR 12 dB ──\n");
        for (double dev : { 2000.0, 4500.0 }) { Cfg c; c.devHz = dev; c.tiltDb = 5; run1(c, true); }
        for (int fl : { 8, 15 }) { Cfg c; c.flags = fl; c.tiltDb = 5; run1(c, true); }
        for (double e : { 1500.0, -2500.0 }) { Cfg c; c.errHz = e; c.tiltDb = 5; run1(c, true); }
        { Cfg c; c.gainStep = 2; c.tiltDb = 5; run1(c, true); }
        { Cfg c; c.gainStep = 0.5; c.tiltDb = 5; run1(c, true); }
        for (double bw : { 10000.0, 16000.0 }) { Cfg c; c.bw = bw; c.tiltDb = 5; c.devHz = 4000; run1(c, true); }
    } else {
        std::printf("── the server's chain off air (synthetic keyed FM in noise, 24 packets a row) ──\n");
        Cfg c; c.cnrDb = 12; c.tiltDb = 5;
        Row r = run1(c, true);
        check(r.pipe >= 22 && r.wrong == 0, "strong-ish (CNR 12 dB, pre-emphasised TX): ≥ 22 of 24, no wrong frames");
        check(r.pipe >= r.ref - 1, "…and the chain costs no more than one packet against the reference (" + std::to_string(r.pipe) + " vs " + std::to_string(r.ref) + ")");
        c.tiltDb = 0; r = run1(c, true);
        check(r.pipe >= 20 && r.pipe >= r.ref - 2, "flat TX at 12 dB: ≥ 20 and within 2 of the reference");
        c.tiltDb = 5; c.gainStep = 2; r = run1(c, false);
        check(r.pipe >= 22, "a 6 dB gain step in every packet's middle costs nothing (" + std::to_string(r.pipe) + ")");
        c.gainStep = 1; c.flags = 15; r = run1(c, false);
        check(r.pipe >= 22, "a short TXDELAY (15 flags, 100 ms) after FM hiss: ≥ 22 (" + std::to_string(r.pipe) + ")");
    }
    std::printf("   (%.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::printf("\ntest-aprs-onair: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
