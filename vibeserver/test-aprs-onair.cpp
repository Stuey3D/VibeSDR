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
// ★★ beta92 APRS FROM IQ (the decoder's own tap, RxPipeline::setDecoderTap → Run::iq1200_):
//   --iq-sweep [afc|noafc] [packets]  the IQ receiver's filter width × deviation ±3/4.7/6 kHz × CNR 6–12 dB × TX error 0/±2 kHz
//   --audio-check                     does the listener's audio still add packets the IQ receiver misses?
//   --cf32 FILE RATE                  a decoder dump's channel IQ straight into the decoder (now vs before), + its CPU
//   --cpu                             what the tap costs the listener's pipeline
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
    vibedsp::RxPipeline::Mode mode = vibedsp::RxPipeline::Mode::NFM;   ///< ★ beta92 the LISTENER's mode (the tap ignores it)
};

static bool g_randomData = false;   ///< ★ makeIq sends random bits instead of each frame (the false-decode check)
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
        if (g_randomData) {   // ★ --false: random bits (4× a frame's length) where the frame would be
            std::mt19937 rb(uint32_t(c.seed * 131 + uint64_t(i)));
            for (size_t k = 0; k < f.size() * 8 * 4; ++k) bits.push_back(int(rb() & 1));
        } else vibehdlc::stuffBits(f, bits);
        vibehdlc::flagBits(bits); vibehdlc::flagBits(bits);
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

/** ★ 2026-11-24 (docs/v12/APRS-WEAK.md): EXACT — every frame the run logs (aprs_frame: source and the information field
 *  in hex) must be one the transmitter sent, byte for byte; anything else (a repair that "passed" the FCS on the wrong
 *  bits, a noise frame) counts as WRONG. Before this only a missing "on-air test #" text counted. */
static std::string hexOf(const std::string& s) { static const char* hx = "0123456789abcdef"; std::string o; for (unsigned char c : s) { o += hx[c >> 4]; o += hx[c & 15]; } return o; }
static const std::string& expectedFrame(int i) {   // "from|hex(info)" of packet i
    static std::vector<std::string> v;
    while (int(v.size()) <= i) {
        const int k = int(v.size());
        const std::vector<uint8_t> b = ui(k);
        const std::string info(b.begin() + 30, b.end());
        v.push_back(std::string("G4ABC") + (k % 16 ? "-" + std::to_string(k % 16) : "") + "|" + hexOf(info));
    }
    return v[size_t(i)];
}
static std::string field(const std::string& j, const char* name) {
    const std::string k = std::string("\"") + name + "\":\"";
    const size_t a = j.find(k); if (a == std::string::npos) return "";
    const size_t b = j.find('"', a + k.size()); return j.substr(a + k.size(), b == std::string::npos ? std::string::npos : b - a - k.size());
}

/** The packets (by number) a run's JSON announced. */
struct Heard { std::set<int> got; int wrong = 0; int frames = 0; };
static void noteJson(const std::string& j, Heard& h) {
    if (j.find("\"type\":\"aprs_frame\"") != std::string::npos) {
        ++h.frames;
        const std::string hx = field(j, "hex"), from = field(j, "from");
        const std::string txt = field(j, "text");
        const size_t p = txt.find("on-air test #");
        const int i = p == std::string::npos ? -1 : std::atoi(txt.c_str() + p + 13);
        if (i < 0 || i > 100000 || expectedFrame(i) != from + "|" + hx) {
            ++h.wrong;
            if (std::getenv("APRS_SHOW_WRONG")) std::printf("WRONG FRAME: %s\n   expected %s\n", j.c_str(), i >= 0 && i <= 100000 ? expectedFrame(i).c_str() : "?");
        }
        return;
    }
    if (j.find("\"type\":\"aprs_packet\"") == std::string::npos) return;
    const size_t p = j.find("on-air test #");
    if (p == std::string::npos) { ++h.wrong; return; }
    h.got.insert(std::atoi(j.c_str() + p + 13));
}

/** The server's chain: RxPipeline NFM → audio (+ channel IQ, + ★ beta92 the decoder tap) → Run AUTO — several decoders
 *  on ONE pass of the same IQ:
 *    pipe   AUTO as it runs now: the listener's audio + the decoder's own IQ receiver on the pipeline's DECODER TAP
 *    old    AUTO as it ran before beta92: the audio + the "narrow" path (±5 kHz, no AFC) on the LISTENER's channel IQ
 *    iq     the IQ receiver on the tap alone (no audio) — does the audio path still add anything?
 *    audio  a 1200 run on the listener's audio alone
 *    widths[k]  the IQ receiver alone at the filter half-width `halves[k]` (the --iq-sweep) */
struct Multi { Heard pipe, old, iq, audio; std::vector<Heard> widths; double tapRate = 0, afc = 0; };
static Multi viaPipelineAll(const Cfg& c, const std::vector<cf>& iq, double fs, double off, const std::vector<double>& halves = {}, bool afc = true) {
    Multi M; M.widths.resize(halves.size());
    auto mk = [](Heard& h, int baud) { vibeaprs::Run::Opts o; o.json = [&h](const std::string& j) { noteJson(j, h); }; return std::make_unique<vibeaprs::Run>(baud, std::move(o)); };
    auto rNew = mk(M.pipe, 0), rOld = mk(M.old, 0), rIq = mk(M.iq, 0), rAud = mk(M.audio, 1200);
    { vibeaprs::Run::IqTune t; t.halfHz = 5000; t.transHz = 2000; t.afc = false; t.decimate = false; rOld->setIqTune(t); }   // ★ the pre-beta92 narrow path, as it was
    if (const char* e = std::getenv("APRS_IQ_TUNE")) {   // ★ experiments: "afc=0 trans=3000"
        vibeaprs::Run::IqTune t; t.afc = !std::strstr(e, "afc=0");
        if (const char* q = std::strstr(e, "trans=")) t.transHz = std::atof(q + 6);
        rIq->setIqTune(t);
    }
    std::vector<std::unique_ptr<vibeaprs::Run>> rW;
    for (size_t k = 0; k < halves.size(); ++k) { rW.push_back(mk(M.widths[k], 1200)); vibeaprs::Run::IqTune t; t.halfHz = halves[k]; t.afc = afc; rW.back()->setIqTune(t); }
    struct Ctx { vibeaprs::Run *n, *o, *q, *a; std::vector<std::unique_ptr<vibeaprs::Run>>* w; double rate = 0; } ctx{rNew.get(), rOld.get(), rIq.get(), rAud.get(), &rW};
    vibedsp::RxPipeline::Callbacks cb; cb.ctx = &ctx;
    cb.audio = [](void* x, const float* pcm, int frames, int ch, int) {
        auto* p = static_cast<Ctx*>(x);
        std::vector<float> m(static_cast<size_t>(frames)); for (int i = 0; i < frames; ++i) m[size_t(i)] = pcm[size_t(i) * size_t(ch)];
        p->n->feedAudio(m.data(), frames); p->o->feedAudio(m.data(), frames); p->a->feedAudio(m.data(), frames);
    };
    cb.iq = [](void* x, const vibedsp::cf32* z, int k, double rate) { static_cast<Ctx*>(x)->o->feedIq(reinterpret_cast<const std::complex<float>*>(z), k, rate); };
    cb.decIq = [](void* x, const vibedsp::cf32* z, int k, double rate) {
        auto* p = static_cast<Ctx*>(x); p->rate = rate;
        const auto* q = reinterpret_cast<const std::complex<float>*>(z);
        p->n->feedIq(q, k, rate); p->q->feedIq(q, k, rate);
        for (auto& r : *p->w) r->feedIq(q, k, rate);
    };
    vibedsp::RxPipeline p; p.setMeasureThread(false);
    p.setDeemphasis(c.tau);
    p.setNfmVoice(c.voice);
    p.setDecoderTap(vibeaprs::Run::kTapHz);
    p.start(fs, 1024, 20.0, 48000, cb);
    p.setTune(off, c.mode, c.bw);
    const int blk = int(fs / 50);
    for (size_t i = 0; i + size_t(blk) <= iq.size(); i += size_t(blk)) p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
    p.stop();
    M.tapRate = ctx.rate; M.afc = rNew->iqAfcHz();
    return M;
}
/** `audioOnly` (out): what the listener's AUDIO alone gave. */
static Heard viaPipeline(const Cfg& c, const std::vector<cf>& iq, double fs, double off, Heard* audioOnly = nullptr, Heard* old = nullptr) {
    Multi M = viaPipelineAll(c, iq, fs, off);
    if (audioOnly) *audioOnly = M.audio;
    if (old) *old = M.old;
    return M.pipe;
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
struct Row { int pipe = 0, audio = 0, ref = 0, wrong = 0, old = 0, iq = 0; };
static Row run1(const Cfg& c, bool ref, bool print = true) {
    const double fs = 240000, off = 37000;
    const std::vector<cf> iq = makeIq(c, fs, off);
    Row r;
    const Multi M = viaPipelineAll(c, iq, fs, off);
    r.pipe = int(M.pipe.got.size()); r.audio = int(M.audio.got.size()); r.old = int(M.old.got.size()); r.iq = int(M.iq.got.size());
    r.wrong = M.pipe.wrong + M.audio.wrong + M.old.wrong + M.iq.wrong;
    if (ref) { const Heard b = viaReference(iq, fs, off); r.ref = int(b.got.size()); r.wrong += b.wrong; }
    if (print) std::printf("   %-78s  server %2d/%d (IQ alone %2d, before beta92 %2d, audio alone %2d)%s%s\n", desc(c).c_str(), r.pipe, c.packets, r.iq, r.old, r.audio,
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


/** ★ `test-aprs-onair --write-clip FILE RATE CNR [packets [nbOffHz nbDb [devHz errHz tiltDb]]]`: the same keyed, pre-emphasised Bell 202 FM in noise (CNR in
 *  12.5 kHz) as u8 IQ at RATE, centred on the channel — for a REAL server fed by fake-rtl-tcp (--hf FILE --hf-centre
 *  144800000): scripts/test-server-aprs-shared.mjs. The packets are "on-air test #NNN" (noteJson). */
static int writeClip(const char* path, double rate, double cnr, int packets, double nbOff, double nbDb, double dev = 3000, double err = 0, double tilt = 5) {
    Cfg c; c.cnrDb = cnr; c.tiltDb = tilt; c.packets = packets; c.seed = 7; c.nbOffHz = nbOff; c.nbDb = nbDb; c.devHz = dev; c.errHz = err;
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

/** ★★ beta92 `test-aprs-onair --cf32 FILE RATE`: a decoder dump's CHANNEL IQ (VIBE_DEC_AUDIO_DUMP .cf32, float I,Q, the
 *  dial at DC) straight into the decoder — the IQ receiver as it runs now (three filters + AFC) against the narrow path it
 *  replaced (±5 kHz, no AFC). A dump made before beta92 is ALREADY cut to the listener's passband, so it cannot show what
 *  the wide tap adds; it shows the decoder on the same samples, and that it makes nothing up. Prints every packet. */
static int decodeCf32(const char* path, double rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::vector<cf> iq;
    { std::vector<cf> b(16384); size_t k; while ((k = std::fread(b.data(), sizeof(cf), b.size(), f)) > 0) iq.insert(iq.end(), b.begin(), b.begin() + long(k)); }
    std::fclose(f);
    std::printf("%s: %.1f s at %.1f S/s\n", path, double(iq.size()) / rate, rate);
    auto rawOf = [](const std::string& j, std::vector<std::string>& v) {
        if (j.find("\"type\":\"aprs_packet\"") == std::string::npos) return;
        const size_t a = j.find("\"raw\":\""); if (a == std::string::npos) return;
        const size_t b = j.find("\",\"", a + 7); v.push_back(j.substr(a + 7, b == std::string::npos ? std::string::npos : b - a - 7)); };
    std::vector<std::string> now, old; size_t frNow = 0;
    vibeaprs::Run::Opts o1; o1.json = [&](const std::string& j) { rawOf(j, now); if (j.find("\"type\":\"aprs_frame\"") != std::string::npos) ++frNow; };
    vibeaprs::Run rNow(0, std::move(o1));
    vibeaprs::Run::Opts o2; o2.json = [&](const std::string& j) { rawOf(j, old); }; vibeaprs::Run rOld(0, std::move(o2));
    { vibeaprs::Run::IqTune t; t.halfHz = 5000; t.afc = false; t.decimate = false; rOld.setIqTune(t); }
    const size_t blk = size_t(rate / 50);
    // ★ the CPU of each (one core of this machine; the dump's real time is iq.size() / rate)
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + blk <= iq.size(); i += blk) rNow.feedIq(iq.data() + i, int(blk), rate);
    const auto t1 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + blk <= iq.size(); i += blk) rOld.feedIq(iq.data() + i, int(blk), rate);
    const auto t2 = std::chrono::steady_clock::now();
    const double sec = double(iq.size()) / rate;
    std::printf("CPU (AUTO: 1200 IQ receiver + 9600): now %.3f %% of a core, before beta92 %.3f %%\n",
                100 * std::chrono::duration<double>(t1 - t0).count() / sec, 100 * std::chrono::duration<double>(t2 - t1).count() / sec);
    {   // ★ and 1200 alone (the 9600 receiver out of it)
        vibeaprs::Run::Opts o3; o3.json = [](const std::string&) {}; vibeaprs::Run r12(1200, std::move(o3));
        vibeaprs::Run::Opts o4; o4.json = [](const std::string&) {}; vibeaprs::Run r12o(1200, std::move(o4));
        { vibeaprs::Run::IqTune t; t.halfHz = 5000; t.afc = false; t.decimate = false; r12o.setIqTune(t); }
        const auto a = std::chrono::steady_clock::now();
        for (size_t i = 0; i + blk <= iq.size(); i += blk) r12.feedIq(iq.data() + i, int(blk), rate);
        const auto b = std::chrono::steady_clock::now();
        for (size_t i = 0; i + blk <= iq.size(); i += blk) r12o.feedIq(iq.data() + i, int(blk), rate);
        const auto c = std::chrono::steady_clock::now();
        std::printf("CPU (1200 IQ receiver alone): now %.3f %%, before beta92 (one ±5 kHz path) %.3f %%\n",
                    100 * std::chrono::duration<double>(b - a).count() / sec, 100 * std::chrono::duration<double>(c - b).count() / sec);
    }
    std::printf("── now (±5 / ±6.5 / ±8 kHz + AFC): %zu packets, %zu frames in all; from each filter first %llu / %llu / %llu ──\n", now.size(), frNow,
                (unsigned long long)rNow.framesFromIqPath(0), (unsigned long long)rNow.framesFromIqPath(1), (unsigned long long)rNow.framesFromIqPath(2));
    for (auto& x : now) std::printf("   %s\n", x.c_str());
    std::printf("── before beta92 (the narrow path, ±5 kHz, no AFC): %zu packets ──\n", old.size());
    for (auto& x : old) std::printf("   %s\n", x.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--cf32") == 0) return decodeCf32(argv[2], std::atof(argv[3]));
    if (argc >= 5 && std::strcmp(argv[1], "--write-clip") == 0) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]), argc >= 6 ? std::atoi(argv[5]) : 24,
                                                                                         argc >= 7 ? std::atof(argv[6]) : 0, argc >= 8 ? std::atof(argv[7]) : 0,
                                                                                         // ★ beta92 [dev err tilt]: an over-deviated tracker off 144.800
                                                                                         argc >= 9 ? std::atof(argv[8]) : 3000, argc >= 10 ? std::atof(argv[9]) : 0,
                                                                                         argc >= 11 ? std::atof(argv[10]) : 5);
    if (argc >= 4 && std::strcmp(argv[1], "--cu8") == 0)
        return decodeCu8(argv[2], std::atof(argv[3]), argc >= 5 && std::strcmp(argv[4], "voice") == 0, argc >= 6 ? std::atof(argv[5]) * 1e-6 : 50e-6);
    const bool sweep = argc > 1 && std::strcmp(argv[1], "--sweep") == 0;
    if (argc > 1 && std::strcmp(argv[1], "--neighbour") == 0) {   // ★ the 11-23 hunt: a strong signal 60 kHz up
        for (double db : { 0.0, 20.0, 30.0, 40.0 }) { Cfg c; c.cnrDb = 15; c.tiltDb = 5; c.nbOffHz = db > 0 ? 60000 : 0; c.nbDb = db; run1(c, true); }
        // ★ the Lenovo dump's bursts (11-23): 5–8 dB CNR in 12.5 kHz, flat, OVER-deviated (±4.5–5 kHz, ~14 kHz occupied)
        for (double dev : { 3000.0, 4700.0 }) for (double cnr : { 6.0, 8.0, 10.0 }) { Cfg c; c.cnrDb = cnr; c.devHz = dev; c.tiltDb = 0; c.packets = 24; c.seed = 11; run1(c, true); }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--cpu") == 0) {
        // ★★ beta92 what the decoder tap costs the PIPELINE (2.4 MS/s, NFM 12.5 kHz, 30 s of noise): the listener's chain
        //    with and without setDecoderTap, one core of this machine
        const double fs = 2400000; std::vector<cf> z(size_t(fs / 50)); std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : z) v = cf(nd(rng), nd(rng));
        for (int tap = 0; tap < 2; ++tap) {
            vibedsp::RxPipeline::Callbacks cb; cb.audio = [](void*, const float*, int, int, int) {};
            cb.decIq = [](void*, const vibedsp::cf32*, int, double) {};
            vibedsp::RxPipeline p; p.setMeasureThread(false); if (tap) p.setDecoderTap(vibeaprs::Run::kTapHz);
            p.start(fs, 1024, 20.0, 48000, cb); p.setTune(37000, vibedsp::RxPipeline::Mode::NFM, 12500);
            p.feed(reinterpret_cast<const vibedsp::cf32*>(z.data()), int(z.size()));
            const auto a = std::chrono::steady_clock::now();
            for (int k = 0; k < 30 * 50; ++k) p.feed(reinterpret_cast<const vibedsp::cf32*>(z.data()), int(z.size()));
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
            p.stop();
            std::printf("pipeline NFM 12.5 kHz at 2.4 MS/s, decoder tap %s: %.3f %% of a core\n", tap ? "ON " : "off", 100 * s / 30);
        }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--curve") == 0) {
        // ★★ 2026-11-24 THE WEAK-SIGNAL CURVE (docs/v12/APRS-WEAK.md): packets decoded against CNR (in 12.5 kHz) for flat,
        //    pre-emphasised (+5 dB) and de-emphasised (−5 dB) transmitters at three deviation / tuning shapes, through the
        //    server's chain — `both` = AUTO as a listener runs it (audio + IQ), `iq` = the IQ receiver alone, `aud` = a 1200
        //    run on the listener's audio alone (NFM 12.5 kHz ALL, 50 µs). The 50 % point of each by interpolation.
        //    argv: [packets a row] [first CNR] [last CNR] [shapes: a subset of "abc"]
        const int P = argc > 2 ? std::atoi(argv[2]) : 40;
        const double lo = argc > 3 ? std::atof(argv[3]) : 3, hi = argc > 4 ? std::atof(argv[4]) : 12;
        const std::string pick = argc > 5 ? argv[5] : "abc";
        struct Shape { char id; double dev, err; } shapes[] = { { 'a', 3000, 0 }, { 'b', 4500, 1000 }, { 'c', 6000, -2000 }, { 'd', 6000, 0 }, { 'e', 3000, -2000 }, { 'f', 4500, 0 } };   // d–f: by hand
        int totB = 0, totI = 0, totA = 0, totW = 0, totF = 0;
        std::printf("%d packets a row; columns: both (audio + IQ) / IQ alone / audio alone; wrong frames\n", P);
        for (const Shape& s : shapes) {
            if (pick.find(s.id) == std::string::npos) continue;
            for (double tilt : { 0.0, 5.0, -5.0 }) {
                if (const char* e = std::getenv("APRS_CURVE_TILT")) if (std::atof(e) != tilt) continue;   // one TX shape only
                std::vector<double> xs; std::vector<int> yb, yi, ya;
                for (double cnr = lo; cnr <= hi + 1e-9; cnr += 1) {
                    Cfg c; c.cnrDb = cnr; c.devHz = s.dev; c.errHz = s.err; c.tiltDb = tilt; c.packets = P;
                    c.seed = uint64_t(1000 + cnr * 13 + tilt * 3 + s.dev / 100);
                    const double fs = 240000, off = 37000;
                    const std::vector<cf> iq = makeIq(c, fs, off);
                    const Multi M = viaPipelineAll(c, iq, fs, off);
                    const int w = M.pipe.wrong + M.iq.wrong + M.audio.wrong;
                    xs.push_back(cnr); yb.push_back(int(M.pipe.got.size())); yi.push_back(int(M.iq.got.size())); ya.push_back(int(M.audio.got.size()));
                    totB += yb.back(); totI += yi.back(); totA += ya.back(); totW += w; totF += M.pipe.frames;
                    std::printf("  dev %.1fk err %+5.0f TX %-8s CNR %4.1f   both %3d  iq %3d  aud %3d  wrong %d\n", s.dev / 1000, s.err,
                                tilt > 0 ? "pre-emph" : tilt < 0 ? "de-emph" : "flat", cnr, yb.back(), yi.back(), ya.back(), w);
                    std::fflush(stdout);
                }
                auto half = [&](const std::vector<int>& y) {
                    for (size_t k = 0; k < y.size(); ++k) if (y[k] * 2 >= P) {
                        if (k == 0) return xs[0];
                        const double a = y[k - 1], b = y[k];
                        return xs[k - 1] + (P / 2.0 - a) / std::max(1e-9, b - a);
                    }
                    return 99.0;
                };
                std::printf("  50%% POINT dev %.1fk err %+5.0f TX %-8s  both %.2f dB  iq %.2f dB  aud %.2f dB\n", s.dev / 1000, s.err,
                            tilt > 0 ? "pre-emph" : tilt < 0 ? "de-emph" : "flat", half(yb), half(yi), half(ya));
            }
        }
        std::printf("TOTAL both %d  iq %d  aud %d  wrong %d (frames logged %d)\n", totB, totI, totA, totW, totF);
        return totW ? 1 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--listener-wav") == 0) {
        // ★★ 2026-11-24 what a LISTENER's REC sounds like at a CNR: the synthetic keyed packets (flat TX, ±3 kHz, 8 packets)
        //    through the pipeline's NFM 12.5 kHz ALL 50 µs, written as a 48 kHz 16-bit WAV — to place a real REC on the
        //    curve by its hiss quieting and tone-to-hiss ratio (docs/v12/APRS-WEAK.md).
        Cfg c; c.cnrDb = std::atof(argv[3]); c.tiltDb = argc > 4 ? std::atof(argv[4]) : 0; c.packets = 8; c.seed = 3;
        const double fs = 240000, off = 37000;
        const std::vector<cf> iq = makeIq(c, fs, off);
        static std::vector<int16_t> pcmOut; pcmOut.clear();
        vibedsp::RxPipeline::Callbacks cb;
        cb.audio = [](void*, const float* pcm, int frames, int ch, int) { for (int i = 0; i < frames; ++i) pcmOut.push_back(int16_t(std::clamp(pcm[size_t(i) * size_t(ch)] * 16000.f, -32767.f, 32767.f))); };
        vibedsp::RxPipeline p; p.setMeasureThread(false); p.setDeemphasis(50e-6);
        p.start(fs, 1024, 20.0, 48000, cb); p.setTune(off, vibedsp::RxPipeline::Mode::NFM, 12500);
        const int blk = int(fs / 50);
        for (size_t i = 0; i + size_t(blk) <= iq.size(); i += size_t(blk)) p.feed(reinterpret_cast<const vibedsp::cf32*>(iq.data() + i), blk);
        p.stop();
        FILE* f = std::fopen(argv[2], "wb"); if (!f) return 1;
        const uint32_t n = uint32_t(pcmOut.size() * 2);
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); w32(36 + n); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32(48000); w32(96000); w16(2); w16(16);
        std::fwrite("data", 1, 4, f); w32(n); std::fwrite(pcmOut.data(), 2, pcmOut.size(), f); std::fclose(f);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--wav") == 0) {
        // ★★ 2026-11-24 a REAL recording of the decoder's audio (a client's REC of NFM — the XCover HF+ file): a 1200 run on
        //    it (16-bit or float WAV, first channel, 48 kHz), every packet printed. docs/v12/APRS-WEAK.md.
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        std::vector<uint8_t> b; { uint8_t t[65536]; size_t k; while ((k = std::fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + k); }
        std::fclose(f);
        auto u32 = [&](size_t p) { return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24; };
        auto u16 = [&](size_t p) { return uint16_t(b[p] | b[p + 1] << 8); };
        size_t p = 12, at = 0, len = 0; int fmt = 0, ch = 1, bits = 0, rate = 0;
        while (p + 8 <= b.size()) {
            const uint32_t l = u32(p + 4);
            if (!std::memcmp(&b[p], "fmt ", 4)) { fmt = u16(p + 8); ch = u16(p + 10); rate = int(u32(p + 12)); bits = u16(p + 22); }
            if (!std::memcmp(&b[p], "data", 4)) { at = p + 8; len = std::min<size_t>(l, b.size() - at); break; }
            p += 8 + l + (l & 1);
        }
        std::vector<float> a;
        if (fmt == 1 && bits == 16) for (size_t i = 0; i + size_t(2 * ch) <= len; i += size_t(2 * ch)) a.push_back(int16_t(u16(at + i)) / 32768.f);
        else if (fmt == 3 && bits == 32) for (size_t i = 0; i + size_t(4 * ch) <= len; i += size_t(4 * ch)) { float v; std::memcpy(&v, &b[at + i], 4); a.push_back(v); }
        if (rate != 48000 || a.empty()) { std::fprintf(stderr, "need a 48 kHz 16-bit or float WAV (got %d Hz, fmt %d, %d bits)\n", rate, fmt, bits); return 1; }
        int pk = 0, fr = 0;
        vibeaprs::Run::Opts o; double tNow = 0;
        o.json = [&](const std::string& j) {
            if (j.find("\"type\":\"aprs_frame\"") != std::string::npos) ++fr;
            if (j.find("\"type\":\"aprs_packet\"") == std::string::npos) return;
            ++pk; std::printf("  %6.1f s  %s\n", tNow, field(j, "raw").c_str());
        };
        vibeaprs::Run run(argc > 3 ? std::atoi(argv[3]) : 1200, std::move(o));
        for (size_t i = 0; i < a.size(); i += 960) { tNow = double(i) / 48000; run.feedAudio(a.data() + i, int(std::min<size_t>(960, a.size() - i))); }
        std::printf("%s: %.1f s — %d packets, %d frames (%llu repaired)\n", argv[2], a.size() / 48000.0, pk, fr, (unsigned long long)run.repaired());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--cpu-paths") == 0) {
        // ★★ 2026-11-24: what each 1200 Bd path costs, one core of this machine — the listener's audio path (one AfskDemod
        //    at 48 kHz) and the IQ receiver at a 40 kS/s tap (three discriminator filters + AFC, then + the FM tone bank).
        const double secs = 60;
        std::mt19937 rng(9); std::normal_distribution<float> nd(0.f, 0.3f);
        {
            vibeaprs::Run::Opts o; o.json = [](const std::string&) {}; vibeaprs::Run r(1200, std::move(o));
            std::vector<float> a(960); for (auto& v : a) v = nd(rng);
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < int(secs * 50); ++k) r.feedAudio(a.data(), int(a.size()));
            std::printf("audio path (AfskDemod at 48 kHz): %.3f %% of a core\n", 100 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / secs);
        }
        for (int mf = 0; mf < 2; ++mf) {
            vibeaprs::Run::Opts o; o.json = [](const std::string&) {}; vibeaprs::Run r(1200, std::move(o));
            setenv("APRS_MF", mf ? "1" : "0", 1);
            const double rate = 40000; std::vector<cf> b(size_t(rate / 50)); for (auto& z : b) z = cf(nd(rng), nd(rng));
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < int(secs * 50); ++k) r.feedIq(b.data(), int(b.size()), rate);
            std::printf("IQ receiver at 40 kS/s, tone bank %s: %.3f %% of a core\n", mf ? "ON " : "off", 100 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / secs);
        }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--false") == 0) {
        // ★★ 2026-11-24 FALSE DECODES (docs/v12/APRS-WEAK.md): zero is the only pass. (1) band noise into AUTO's IQ receiver
        //    and a 1200 run's audio (a discriminator of the same noise); (2) RANDOM DATA — Bell 202 AFSK of random bits (no
        //    frames, but flags and frame-sized runs between them by chance) keyed on FM at 4–20 dB, through the server's
        //    chain; every frame either run logs is counted. argv: [minutes of noise] [random-data packets a CNR]
        const double mins = argc > 2 ? std::atof(argv[2]) : 20;
        const int RP = argc > 3 ? std::atoi(argv[3]) : 40;
        int frames = 0;
        {
            vibeaprs::Run::Opts o; o.json = [&](const std::string& j) { if (j.find("\"type\":\"aprs_frame\"") != std::string::npos) ++frames; };
            vibeaprs::Run run(0, std::move(o));
            vibeaprs::Run::Opts o2; o2.json = [&](const std::string& j) { if (j.find("\"type\":\"aprs_frame\"") != std::string::npos) ++frames; };
            vibeaprs::Run runA(1200, std::move(o2));
            const double rate = 40000; std::mt19937 rng(777); std::normal_distribution<float> nd(0.f, 0.3f);
            std::vector<cf> b(size_t(rate / 50)); std::vector<float> a; cf prev(1, 0);
            for (long k = 0; k < long(mins * 60 * 50); ++k) {
                for (auto& z : b) z = cf(nd(rng), nd(rng));
                run.feedIq(b.data(), int(b.size()), rate);
                // the same noise's discriminator, ×1.2 to 48 kHz by holding (it is noise; the shape does not matter)
                a.clear();
                for (size_t i = 0; i < b.size(); ++i) { const float v = std::arg(b[i] * std::conj(prev)) / float(M_PI); prev = b[i]; a.push_back(v); if (i % 5 == 4) a.push_back(v); }
                runA.feedAudio(a.data(), int(a.size()));
            }
            std::printf("noise: %.0f minutes into AUTO's IQ receiver and a 1200 run's audio: %d frames\n", mins, frames);
        }
        int rnd = 0, rndWrong = 0;
        for (double cnr : { 4.0, 8.0, 12.0, 20.0 }) {
            // random bits: replace the generator's frames with random levels of the same length, flags kept out
            Cfg c; c.cnrDb = cnr; c.packets = RP; c.tiltDb = 0; c.seed = uint64_t(cnr * 31 + 5);
            const double fs = 240000, off = 37000;
            g_randomData = true;
            const std::vector<cf> iq = makeIq(c, fs, off);
            g_randomData = false;
            const Multi M = viaPipelineAll(c, iq, fs, off);
            const int f = M.pipe.frames + M.iq.frames + M.audio.frames;
            rnd += f; rndWrong += M.pipe.wrong + M.iq.wrong + M.audio.wrong;
            std::printf("random data, CNR %4.1f dB, %d bursts of ~2.5 s of random bits: %d frames\n", cnr, RP, f);
        }
        std::printf("FALSE TOTAL noise %d, random data %d\n", frames, rnd);
        return frames || rnd ? 1 : 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--audio-check") == 0) {
        // ★★ beta92: does the listener's AUDIO path still add packets the IQ receiver misses? Pre-emphasised and flat TXs,
        //    VOICE and ALL, at the threshold. `server` = audio + IQ, `IQ alone` = the IQ receiver by itself.
        int both = 0, iqo = 0;
        for (double tilt : { 5.0, 0.0, -5.0 }) for (bool voice : { false, true }) for (double cnr : { 7.0, 8.0, 9.0 }) {
            Cfg c; c.cnrDb = cnr; c.tiltDb = tilt; c.voice = voice; c.packets = 48; c.seed = uint64_t(300 + cnr * 3 + tilt + (voice ? 50 : 0));
            const Row r = run1(c, false); both += r.pipe; iqo += r.iq;
        }
        std::printf("total: audio + IQ %d, IQ alone %d\n", both, iqo);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--iq-sweep") == 0) {
        // ★★ beta92: the IQ receiver's filter half-width against deviation, CNR (in 12.5 kHz) and the TX's error — every
        //    width on the SAME IQ (one pass), flat TX (worst for the 2200 tone), the listener on NFM 12.5 kHz ALL.
        //    argv[2] = "noafc" for the AFC off. Columns: widths, then old / new AUTO / IQ alone / audio alone.
        const bool afc = !(argc > 2 && std::strcmp(argv[2], "noafc") == 0);
        const std::vector<double> halves = { 5000, 6000, 6500, 7000, 7500, 8000, 9000, 10000 };
        const int P = argc > 3 ? std::atoi(argv[3]) : 32;
        std::printf("AFC %s, %d packets a row\n%-26s", afc ? "on" : "off", P, "dev  CNR  err");
        for (double h : halves) std::printf(" ±%-4.1fk", h / 1000);
        std::printf("   old  new  iq  aud\n");
        std::vector<int> tot(halves.size(), 0); int un[4] = {0, 0, 0, 0};
        for (double err : { 0.0, 2000.0, -2000.0 })
            for (double dev : { 3000.0, 4700.0, 6000.0 })
                for (double cnr : { 6.0, 8.0, 10.0, 12.0 }) {
                    Cfg c; c.cnrDb = cnr; c.devHz = dev; c.tiltDb = 0; c.errHz = err; c.packets = P; c.seed = uint64_t(dev + cnr * 7 + err);
                    const double fs = 240000, off = 37000;
                    const std::vector<cf> iq = makeIq(c, fs, off);
                    const Multi M = viaPipelineAll(c, iq, fs, off, halves, afc);
                    std::printf("%4.1fk %4.0f %+5.0f %10s", dev / 1000, cnr, err, "");
                    int wr = M.pipe.wrong + M.old.wrong + M.iq.wrong;
                    for (size_t k = 0; k < halves.size(); ++k) { std::printf(" %6d", int(M.widths[k].got.size())); tot[k] += int(M.widths[k].got.size()); wr += M.widths[k].wrong; }
                    // unions: two or three widths run side by side (k indexes into halves)
                    auto uni = [&](std::initializer_list<int> ks) { std::set<int> u; for (int k : ks) u.insert(M.widths[size_t(k)].got.begin(), M.widths[size_t(k)].got.end()); return int(u.size()); };
                    const int u1 = uni({0, 4}), u2 = uni({0, 5}), u3 = uni({0, 2, 5}), u4 = uni({1, 5});
                    un[0] += u1; un[1] += u2; un[2] += u3; un[3] += u4;
                    std::printf("   %3d  %3d %3d %3d | 5+7.5 %2d 5+8 %2d 5+6.5+8 %2d 6+8 %2d%s\n", int(M.old.got.size()), int(M.pipe.got.size()), int(M.iq.got.size()), int(M.audio.got.size()),
                                u1, u2, u3, u4, wr ? "  WRONG" : "");
                    std::fflush(stdout);
                }
        std::printf("%-26s", "total");
        for (int t : tot) std::printf(" %6d", t);
        std::printf("   unions 5+7.5 %d, 5+8 %d, 5+6.5+8 %d, 6+8 %d\n", un[0], un[1], un[2], un[3]);
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
        std::printf("── ★★ beta92 APRS FROM IQ: the decoder's own tap, filters and AFC — whatever the listener has set ──\n");
        t = Cfg(); t.cnrDb = 10; t.devHz = 6000; t.errHz = 1500; t.tiltDb = 0; t.seed = 92; r = run1(t, false);
        check(r.pipe >= 22 && r.old <= r.pipe - 12 && r.wrong == 0,
              "an over-deviated tracker (±6 kHz) 1.5 kHz off 144.800 at 10 dB, the listener on 12.5 kHz: " + std::to_string(r.pipe) + "/24 (before beta92 " + std::to_string(r.old) + ")");
        t = Cfg(); t.cnrDb = 9; t.devHz = 3000; t.errHz = -2000; t.tiltDb = 5; t.seed = 93; r = run1(t, false);
        check(r.pipe >= 22 && r.pipe >= r.old && r.wrong == 0, "a normal tracker 2 kHz low at 9 dB: " + std::to_string(r.pipe) + "/24 (before " + std::to_string(r.old) + ")");
        t = Cfg(); t.cnrDb = 10; t.devHz = 4700; t.tiltDb = 0; t.bw = 6000; t.seed = 94; r = run1(t, false);
        check(r.pipe >= 22 && r.old <= r.pipe - 8, "the listener's NFM cut to 6 kHz, a ±4.7 kHz tracker at 10 dB: " + std::to_string(r.pipe) + "/24 (before " + std::to_string(r.old) + ")");
        t = Cfg(); t.cnrDb = 10; t.devHz = 4700; t.tiltDb = 0; t.mode = vibedsp::RxPipeline::Mode::SSB_USB; t.bw = 2800; t.seed = 95; r = run1(t, false);
        check(r.pipe >= 22, "the listener on USB 2.8 kHz (no FM in their chain at all): " + std::to_string(r.pipe) + "/24");
        t = Cfg(); t.cnrDb = 10; t.devHz = 4700; t.tiltDb = 0; t.voice = true; t.tau = 0; t.seed = 96; r = run1(t, false);
        check(r.pipe >= 22, "VOICE, de-emphasis off: " + std::to_string(r.pipe) + "/24");
        {   // ★ the tap's rate and width do not follow the listener: it holds ±12 kHz at ≥ 2.75 × that for every mode / width
            const double fs = 240000, off = 37000;
            for (auto m : { vibedsp::RxPipeline::Mode::NFM, vibedsp::RxPipeline::Mode::SSB_USB, vibedsp::RxPipeline::Mode::AM }) {
                vibedsp::RxPipeline::Callbacks cb; double got = 0; cb.ctx = &got;
                cb.audio = [](void*, const float*, int, int, int) {};
                cb.decIq = [](void* x, const vibedsp::cf32*, int, double rate) { *static_cast<double*>(x) = rate; };
                vibedsp::RxPipeline p; p.setMeasureThread(false); p.setDecoderTap(vibeaprs::Run::kTapHz);
                p.start(fs, 1024, 20.0, 48000, cb); p.setTune(off, m, m == vibedsp::RxPipeline::Mode::NFM ? 6000 : 2800);
                std::vector<cf> z(size_t(fs / 50));
                for (int k = 0; k < 10; ++k) p.feed(reinterpret_cast<const vibedsp::cf32*>(z.data()), int(z.size()));
                p.stop();
                check(got >= 2.75 * vibeaprs::Run::kTapHz, "the decoder tap with the listener on mode " + std::to_string(int(m)) + ": " + std::to_string(int(got)) + " S/s");
            }
        }
        std::printf("── ★★★ 2026-11-24 BELOW THE FM THRESHOLD: the IQ tone bank (docs/v12/APRS-WEAK.md) ──\n");
        t = Cfg(); t.cnrDb = 5; t.tiltDb = 0; t.packets = 48; t.seed = 1124; r = run1(t, false);
        check(r.iq >= 30 && r.wrong == 0, "CNR 5 dB, a flat ±3 kHz tracker: the IQ receiver ≥ 30 of 48 (" + std::to_string(r.iq) + "; the discriminators alone gave 0 here)");
        t.tiltDb = 5; t.cnrDb = 4.5; t.seed = 1125; r = run1(t, false);
        check(r.iq >= 20 && r.wrong == 0, "CNR 4.5 dB, pre-emphasised: ≥ 20 of 48 (" + std::to_string(r.iq) + ")");
        {   // ★ RANDOM DATA keyed on FM (flags, then random bits where a frame would be): no frame at all
            int f = 0, wr = 0;
            for (double cnr : { 6.0, 12.0, 20.0 }) {
                Cfg c; c.cnrDb = cnr; c.packets = 10; c.seed = uint64_t(cnr * 31 + 5);
                g_randomData = true; const std::vector<cf> iq = makeIq(c, 240000, 37000); g_randomData = false;
                const Multi M = viaPipelineAll(c, iq, 240000, 37000);
                f += M.pipe.frames + M.iq.frames + M.audio.frames; wr += M.pipe.wrong + M.iq.wrong + M.audio.wrong;
            }
            check(f == 0 && wr == 0, "random data keyed on FM at 6 / 12 / 20 dB (30 bursts of ~2.5 s): " + std::to_string(f) + " frames");
        }
        {   // ★★ THE NOISE CHECK: 20 minutes of band noise straight into AUTO's IQ receiver (and 9600) — no frame at all
            vibeaprs::Run::Opts o; int frames = 0; o.json = [&](const std::string& j) { if (j.find("\"type\":\"aprs_frame\"") != std::string::npos || j.find("\"type\":\"aprs_packet\"") != std::string::npos) ++frames; };
            vibeaprs::Run run(0, std::move(o));
            const double rate = 40540.541; std::mt19937 rng(4242); std::normal_distribution<float> nd(0.f, 0.3f);
            std::vector<cf> b(size_t(rate / 50)); double afcMax = 0;
            for (long k = 0; k < long(20 * 60 * 50); ++k) {
                for (auto& z : b) z = cf(nd(rng), nd(rng));
                run.feedIq(b.data(), int(b.size()), rate);
                afcMax = std::max(afcMax, std::fabs(run.iqAfcHz()));
            }
            check(frames == 0, "20 minutes of noise into the IQ receiver + 9600: " + std::to_string(frames) + " frames");
            check(afcMax < 500, "…and the AFC stayed home in it (at most " + std::to_string(int(afcMax)) + " Hz off; it moves on a carrier only)");
        }
    }
    std::printf("   (%.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::printf("\ntest-aprs-onair: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
