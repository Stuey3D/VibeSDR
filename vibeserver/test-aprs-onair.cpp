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

#include <algorithm>
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
    double nbOffHz = 0;      ///< ★ a STRONG NEIGHBOUR this far from the channel (0 = none): an NFM carrier, always on
    double nbDb = 0;         ///< …its power over the packets' carrier
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
    if (c.nbOffHz != 0) {   // ★ the neighbour: ±2.5 kHz of FM at a 400 Hz tone, nbDb over the packets' carrier, never keyed off
        const float a = float(std::pow(10.0, c.nbDb / 20));
        double ph = 0;
        for (int i = 0; i < n; ++i) {
            ph += 2 * M_PI * (off + c.nbOffHz + 2500 * std::sin(2 * M_PI * 400.0 * i / fs)) / fs;
            iq[size_t(i)] += a * cf(float(std::cos(ph)), float(std::sin(ph)));
        }
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
/** `audioOnly` (out): what the listener's AUDIO alone gave — a 1200 Bd run fed the audio and never the IQ (the decoder as
 *  it was before its narrow path, 2026-11-22). */
static Heard viaPipeline(const Cfg& c, const std::vector<cf>& iq, double fs, double off, Heard* audioOnly = nullptr) {
    Heard h, ha;
    vibeaprs::Run::Opts o; o.json = [&](const std::string& j) { noteJson(j, h); };
    vibeaprs::Run run(0, std::move(o));
    vibeaprs::Run::Opts oa; oa.json = [&](const std::string& j) { noteJson(j, ha); };
    vibeaprs::Run runA(1200, std::move(oa));
    struct Ctx { vibeaprs::Run* r; vibeaprs::Run* a; } ctx{&run, &runA};
    vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
    cb.audio = [](void* x, const float* pcm, int frames, int ch, int) {
        auto* p = static_cast<Ctx*>(x);
        if (ch == 1) { p->r->feedAudio(pcm, frames); p->a->feedAudio(pcm, frames); return; }
        std::vector<float> m; m.resize(size_t(frames)); for (int i = 0; i < frames; ++i) m[size_t(i)] = pcm[2 * i];
        p->r->feedAudio(m.data(), frames); p->a->feedAudio(m.data(), frames);
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
    if (audioOnly) *audioOnly = ha;
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
                  c.gainStep != 1 ? " +gain step" : (c.nbOffHz != 0 ? (" +neighbour " + std::to_string(int(c.nbOffHz / 1000)) + "k " + std::to_string(int(c.nbDb)) + " dB").c_str() : ""));
    return b;
}
struct Row { int pipe = 0, audio = 0, ref = 0, wrong = 0; };
static Row run1(const Cfg& c, bool ref, bool print = true) {
    const double fs = 240000, off = 37000;
    const std::vector<cf> iq = makeIq(c, fs, off);
    Row r;
    Heard ao;
    const Heard a = viaPipeline(c, iq, fs, off, &ao);
    r.pipe = int(a.got.size()); r.audio = int(ao.got.size()); r.wrong = a.wrong + ao.wrong;
    if (ref) { const Heard b = viaReference(iq, fs, off); r.ref = int(b.got.size()); r.wrong += b.wrong; }
    if (print) std::printf("   %-78s  server %2d/%d (audio alone %2d)%s%s\n", desc(c).c_str(), r.pipe, c.packets, r.audio,
                           ref ? (" · reference " + std::to_string(r.ref) + "/" + std::to_string(c.packets)).c_str() : "", r.wrong ? " WRONG FRAMES" : "");
    return r;
}


/** ★ `test-aprs-onair --cu8 FILE RATE [voice|all] [tau_us]`: a REAL capture (rtl_sdr u8 IQ; an rtl_tcp "RTL0" header is
 *  skipped; centred on the channel) through the same chain — up-sampled to 240 kS/s and put 37 kHz off the pipeline's
 *  centre, as makeIq's — and through the reference; every packet each heard is printed (2026-11-23, the shared-dial hunt). */
static int decodeCu8(const char* path, double rate, bool voice, double tau) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::vector<uint8_t> u;
    { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) u.insert(u.end(), b, b + k); }
    std::fclose(f);
    const size_t o = (u.size() >= 12 && std::memcmp(u.data(), "RTL0", 4) == 0) ? 12 : 0;
    std::vector<float> I, Q;
    for (size_t i = o; i + 1 < u.size(); i += 2) { I.push_back((u[i] - 127.5f) / 127.5f); Q.push_back((u[i + 1] - 127.5f) / 127.5f); }
    const double fs = 240000, off = 37000;
    vibedsp::RationalResampler ri(rate, fs), rq(rate, fs);
    std::vector<float> Iu(size_t(ri.maxOut(int(I.size())))), Qu(Iu.size());
    const int n = ri.process(I.data(), int(I.size()), Iu.data());
    rq.process(Q.data(), int(Q.size()), Qu.data());
    std::vector<cf> iq(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) { const double p = 2 * M_PI * off * i / fs; iq[size_t(i)] = cf(Iu[size_t(i)], Qu[size_t(i)]) * cf(float(std::cos(p)), float(std::sin(p))); }
    std::printf("%s: %.1f s at %.0f S/s\n", path, double(I.size()) / rate, rate);
    std::vector<std::string> pipe, aud, ref;
    auto grab = [](std::vector<std::string>& v) { return [&v](const std::string& j) {
        if (j.find("\"type\":\"aprs_packet\"") == std::string::npos) return;
        const size_t a = j.find("\"raw\":\""); if (a == std::string::npos) return;
        const size_t b = j.find("\",\"", a + 7); v.push_back(j.substr(a + 7, b == std::string::npos ? std::string::npos : b - a - 7)); }; };
    {
        vibeaprs::Run::Opts oo; oo.json = grab(pipe); vibeaprs::Run run(0, std::move(oo));
        vibeaprs::Run::Opts oa; oa.json = grab(aud); vibeaprs::Run runA(1200, std::move(oa));
        struct Ctx { vibeaprs::Run* r; vibeaprs::Run* a; } ctx{&run, &runA};
        vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
        cb.audio = [](void* x, const float* pcm, int frames, int ch, int) {
            auto* p = static_cast<Ctx*>(x);
            std::vector<float> m(static_cast<size_t>(frames)); for (int i = 0; i < frames; ++i) m[size_t(i)] = pcm[size_t(i) * size_t(ch)];
            p->r->feedAudio(m.data(), frames); p->a->feedAudio(m.data(), frames);
        };
        cb.iq = [](void* x, const vibedsp::cf32* z, int k, double r) { static_cast<Ctx*>(x)->r->feedIq(reinterpret_cast<const std::complex<float>*>(z), k, r); };
        vibedsp::RxPipeline p; p.setMeasureThread(false);
        p.setDeemphasis(tau); p.setNfmVoice(voice);
        p.start(fs, 1024, 20.0, 48000, cb);
        p.setTune(off, vibedsp::RxPipeline::Mode::NFM, 12500);
        const int blk = int(fs / 50);
        for (size_t i = 0; i + size_t(blk) <= iq.size(); i += size_t(blk)) p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
        p.stop();
        std::printf("── the server's chain, AUTO (%s, de-emphasis %.0f us): %zu packets (a 1200 run on the audio alone: %zu; first from narrow %llu, audio %llu) ──\n",
                    voice ? "VOICE" : "ALL", tau * 1e6, pipe.size(), aud.size(), (unsigned long long)run.framesFromNarrow(), (unsigned long long)run.framesFromAudio());
        for (auto& s : pipe) std::printf("   %s\n", s.c_str());
    }
    {
        vibeaprs::Run::Opts o2; o2.json = grab(ref); vibeaprs::Run run(1200, std::move(o2));
        const std::vector<float> lp = vibedsp::designLowpass(8000.0 / fs, 4000.0 / fs);
        const int K = int(lp.size()), D = int(std::lround(fs / 48000));
        std::vector<cf> ring(static_cast<size_t>(K)); int rp = 0; cf prev(1, 0); std::vector<float> out;
        for (size_t i = 0; i < iq.size(); ++i) {
            const double p = -2 * M_PI * off * double(i) / fs;
            ring[size_t(rp)] = iq[i] * cf(float(std::cos(p)), float(std::sin(p))); rp = (rp + 1) % K;
            if (i % size_t(D)) continue;
            cf y(0, 0); for (int k = 0; k < K; ++k) y += ring[size_t((rp + k) % K)] * lp[size_t(k)];
            out.push_back(std::arg(y * std::conj(prev)) * float(48000.0 / (2 * M_PI * 6250.0))); prev = y;
            if (out.size() == 960) { run.feedAudio(out.data(), int(out.size())); out.clear(); }
        }
        std::printf("── the reference (±8 kHz FIR, ideal discriminator): %zu packets ──\n", ref.size());
        for (auto& s : ref) std::printf("   %s\n", s.c_str());
    }
    return 0;
}


/** ★ `test-aprs-onair --write-clip FILE RATE CNR [packets]`: the same keyed, pre-emphasised Bell 202 FM in noise (CNR in
 *  12.5 kHz) as u8 IQ at RATE, centred on the channel — for a REAL server fed by fake-rtl-tcp (--hf FILE --hf-centre
 *  144800000): scripts/test-server-aprs-shared.mjs. The packets are "on-air test #NNN" (noteJson). */
static int writeClip(const char* path, double rate, double cnr, int packets, double nbOff, double nbDb) {
    Cfg c; c.cnrDb = cnr; c.tiltDb = 5; c.packets = packets; c.seed = 7; c.nbOffHz = nbOff; c.nbDb = nbDb;
    const std::vector<cf> iq = makeIq(c, rate, 0);
    double pk = 0; for (const cf& z : iq) pk = std::max(pk, double(std::max(std::fabs(z.real()), std::fabs(z.imag()))));
    const double g = 120.0 / (pk > 0 ? pk : 1);
    std::vector<uint8_t> u(iq.size() * 2);
    for (size_t i = 0; i < iq.size(); ++i) {
        u[2 * i] = uint8_t(std::lround(std::clamp(127.5 + iq[i].real() * g, 0.0, 255.0)));
        u[2 * i + 1] = uint8_t(std::lround(std::clamp(127.5 + iq[i].imag() * g, 0.0, 255.0)));
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(u.data(), 1, u.size(), f); std::fclose(f);
    std::fprintf(stderr, "wrote %s: %.1f s at %.0f S/s, %d packets at CNR %.1f dB\n", path, double(iq.size()) / rate, rate, packets, cnr);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--write-clip") == 0) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc >= 6 ? std::atoi(argv[5]) : 24,
                                                                                         argc >= 7 ? std::atof(argv[6]) : 0, argc >= 8 ? std::atof(argv[7]) : 0);
    if (argc >= 4 && std::strcmp(argv[1], "--cu8") == 0)
        return decodeCu8(argv[2], std::atof(argv[3]), argc >= 5 && std::strcmp(argv[4], "voice") == 0, argc >= 6 ? std::atof(argv[5]) * 1e-6 : 50e-6);
    const bool sweep = argc > 1 && std::strcmp(argv[1], "--sweep") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--neighbour") == 0) {   // ★ the 11-23 hunt: a strong signal 60 kHz up
        for (double db : { 0.0, 20.0, 30.0, 40.0 }) { Cfg c; c.cnrDb = 15; c.tiltDb = 5; c.nbOffHz = db > 0 ? 60000 : 0; c.nbDb = db; run1(c, true); }
        // ★ the Lenovo dump's bursts (11-23): 5–8 dB CNR in 12.5 kHz, flat, OVER-deviated (±4.5–5 kHz, ~14 kHz occupied)
        for (double dev : { 3000.0, 4700.0 }) for (double cnr : { 6.0, 8.0, 10.0 }) { Cfg c; c.cnrDb = cnr; c.devHz = dev; c.tiltDb = 0; c.packets = 24; c.seed = 11; run1(c, true); }
        return 0;
    }
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
        std::printf("── at the threshold: the IF filter's width (CNR always in 12.5 kHz) ──\n");
        for (double cnr : { 8.0, 9.0 }) for (double bw : { 9000.0, 10000.0, 12500.0, 16000.0 }) { Cfg c; c.cnrDb = cnr; c.bw = bw; c.tiltDb = 5; c.packets = 48; c.seed = 77 + uint64_t(cnr); run1(c, false); }
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
        std::printf("── at the FM threshold: the decoder's own narrow path (2026-11-22) ──\n");
        Cfg t; t.cnrDb = 8; t.tiltDb = 5; t.packets = 48; t.seed = 85;
        r = run1(t, false);
        check(r.pipe >= r.audio + 6 && r.pipe >= 38 && r.wrong == 0,
              "CNR 8 dB, the listener on 12.5 kHz: the narrow path adds ≥ 6 (" + std::to_string(r.pipe) + " vs audio alone " + std::to_string(r.audio) + ")");
        t.bw = 16000; r = run1(t, false);
        check(r.pipe >= r.audio + 15 && r.pipe >= 38, "…the listener on 16 kHz: the decoder no longer pays for it (" + std::to_string(r.pipe) + " vs " + std::to_string(r.audio) + ")");
        t.bw = 12500; t.voice = true; t.tiltDb = -5; t.cnrDb = 11; r = run1(t, false);
        check(r.pipe >= r.audio && r.pipe >= 44, "a flat TX heard de-emphasised through VOICE at 11 dB (the 11-10 case): " + std::to_string(r.pipe) + " vs " + std::to_string(r.audio));
        t = Cfg(); t.cnrDb = 12; t.devHz = 5000; t.tiltDb = 5; r = run1(t, false);
        check(r.pipe >= 22 && r.pipe >= r.audio, "an over-deviating tracker (±5 kHz) still decodes (" + std::to_string(r.pipe) + ")");
        t = Cfg(); t.cnrDb = -100; t.packets = 40; r = run1(t, false);
        check(r.pipe == 0 && r.wrong == 0, "no carrier at all (FM hiss, ~60 s, both paths): no frames (" + std::to_string(r.pipe) + ", wrong " + std::to_string(r.wrong) + ")");
    }
    std::printf("   (%.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::printf("\ntest-aprs-onair: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
