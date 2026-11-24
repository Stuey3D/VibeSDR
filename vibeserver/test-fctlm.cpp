// test-fctlm — V12 beta90 FUNcube BPSK 1200 telemetry (vibe_bpsk_rx.h + vibe_fctlm.h; docs/v12/SATELLITE-BEST-SERVER.md):
//   §1 the sync vector against Karn's printed sequence; the frame layout
//   §2 the FEC: our encoder → the decoder, clean, with symbol errors, both coder output orders
//   §3 ★ NOISE: no frame ever (random soft symbols through the framer; white noise through the whole receiver)
//   §4 the whole receiver: our transmitter (differential BPSK, raised cosine) with an offset, a Doppler ramp, a clock
//      error, both polarities — and a frame-rate curve against Eb/N0
//   §5 SIMD against scalar: bit-identical symbols
//   §6 the telemetry values (raw where the document gives no calibration) and the sat_tlm JSON
//   §7 cost
//   §8 ★★★ beta92 THE WIDE SEARCH + DRIFT FOLLOW (Stuart 11-23, AO-73 1.2–1.4 kHz above the prediction: 0 frames). The
//      satellite channel modelled end to end: our transmitter at the prediction + an error, the pick's dial
//      (downlink − centre, + the lock's trim, applied 0.3 s late as the pipeline would), a USB demodulator's 0..bw
//      passband — the OLD pick (1200 Hz centre, ±900 Hz hunt, 2.65 kHz USB) against the NEW one (3800 Hz, ±2600 Hz,
//      7.5 kHz), and the trim the locked decoder steers (vibefc::TrimFollow) converging without ringing.
//   §9 ★ noise at the wide range: no frame, and the follow never moves the trim
//   §10 ★★★ THE REAL SIGNAL (2026-11-24): 23.5 s of AO-73 as the Pi 500's decoder heard it, which decoded NOTHING live —
//      the satellite's clock is ~1400 ppm fast (the timing loop now integrates), a 1 is no phase change (the test
//      transmitter's default now), the frame type counts 0–23. Frames, their schedule, sane values, the learnt clock.
// Silent: no audio device. `--quick` shortens §3/§4.
#include "vibe_fctlm_tx.h"
#include "vibe_sat_orbit.h"   // its json:: parser (for the §6 checks)
#include "vibe_sat_engine.h"  // ★ beta92 the pick's numbers (kTlmCentreHz …) and the trim's clamp and slew

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; std::printf("   ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
                              else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool g_quick = false;

using namespace vibefc;

static void s1() {
    std::printf("§1 the sync vector and the frame\n");
    const char* karn = "11111110000111011110010110010010000001000100110001011101011011000";
    std::string s; for (auto b : syncVector()) s += char('0' + b);
    CHECK(s == karn, "x⁷+x³+1 from all ones = Karn's printed 65 bits");
    CHECK(kCoded == 5132 && kSyms == 5200 && kCoded + 65 == 5197, "5132 coded + 65 sync of 5200");
    std::vector<uint8_t> f = tx::sampleFrame(3, 1);
    uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
    bool syncAt80 = true; for (int j = 0; j < 65; ++j) syncAt80 &= ch[80 * j] == syncVector()[(size_t)j];
    CHECK(syncAt80, "the sync bit leads every 80-symbol column");
}

static void s2() {
    std::printf("§2 the FEC: our encoder → the decoder\n");
    std::mt19937 rng(7);
    int ok = 0, bitOk = 0;
    for (int t = 0; t < 24; ++t) {
        std::vector<uint8_t> f = tx::sampleFrame(t, 100 + t);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = ch[i] ? 40 : -40;
        uint8_t out[kDataBytes]; const int e = decodeFrame(soft, out);
        if (e == 0) ++ok;
        if (std::memcmp(out, f.data(), kDataBytes) == 0) ++bitOk;
    }
    CHECK(ok == 24 && bitOk == 24, "24 frame types decode clean, byte-exact (%d / %d)", ok, bitOk);
    // symbol errors: 6 % hard flips (Viterbi territory) + soft noise
    int good = 0;
    for (int k = 0; k < 20; ++k) {
        std::vector<uint8_t> f = tx::sampleFrame(k % 24, k);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms];
        std::normal_distribution<double> g(0, 1);
        for (int i = 0; i < kSyms; ++i) { double v = (ch[i] ? 1.0 : -1.0) + 0.55 * g(rng); soft[i] = int8_t(std::max(-127.0, std::min(127.0, v * 40))); }
        uint8_t out[kDataBytes];
        if (decodeFrame(soft, out) >= 0 && std::memcmp(out, f.data(), kDataBytes) == 0) ++good;
    }
    CHECK(good >= 19, "σ = 0.55 soft noise (raw symbol error ≈ 3.5 %%): %d / 20 decode", good);
    // the other coder output order
    {
        std::vector<uint8_t> f = tx::sampleFrame(5, 55);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        // swap each coded pair in place (through the de-interleaver's map)
        uint8_t sw[kSyms]; std::memcpy(sw, ch, kSyms);
        for (int k = 0; k + 1 < kCoded; k += 2) {
            auto idx = [](int kk) { return (kk % kCols) * kRows + 1 + kk / kCols; };
            std::swap(sw[idx(k)], sw[idx(k + 1)]);
        }
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = sw[i] ? 40 : -40;
        uint8_t out[kDataBytes]; int order = -1;
        CHECK(decodeFrame(soft, out, &order) == 0 && order == 1 && std::memcmp(out, f.data(), kDataBytes) == 0, "the swapped output order is found (order %d)", order);
    }
    // ★ a frame whose RS words are beyond the code is refused, not "repaired"
    {
        std::vector<uint8_t> f = tx::sampleFrame(2, 2);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = ch[i] ? 40 : -40;
        for (int i = 0; i < kSyms; i += 3) soft[i] = int8_t(-soft[i]);
        uint8_t out[kDataBytes];
        CHECK(decodeFrame(soft, out) < 0, "a third of the symbols wrong: refused");
    }
}

static void s3() {
    std::printf("§3 ★ noise: no frame, ever\n");
    std::mt19937 rng(11);
    std::normal_distribution<double> g(0, 1);
    Framer fr; int frames = 0; fr.onFrame = [&](const uint8_t*, int, int) { ++frames; };
    const long n = g_quick ? 1200L * 600 : 1200L * 3600;   // 10 min / 1 h of symbols
    for (long i = 0; i < n; ++i) fr.push(float(g(rng)));
    CHECK(frames == 0, "%ld random soft symbols (%.0f min at 1200 Bd): %d frames (%llu sync matches, all refused by RS)",
          n, n / 1200.0 / 60.0, frames, (unsigned long long)fr.syncs());
    // the whole receiver on white audio noise
    vibebpsk::Rx::Cfg c;
    int f2 = 0;
    Run run(c, false, Run::Out{ [&](const std::string& j) { if (j.find("sat_tlm") != std::string::npos) ++f2; }, nullptr, nullptr, nullptr });
    std::vector<float> a(48000);
    const int secs = g_quick ? 60 : 300;
    for (int s = 0; s < secs; ++s) { for (auto& v : a) v = float(0.1 * g(rng)); run.feedAudio(a.data(), (int)a.size()); }
    CHECK(f2 == 0, "%d s of white noise through the whole receiver: %d frames", secs, f2);
}

struct Result { int good = 0; uint64_t syncs = 0, failed = 0; double afc = 0; };
static Result runChain(const tx::Cfg& tc, int nFrames, bool invertRx = false, bool forceScalar = false, std::vector<float>* symsOut = nullptr, size_t blk = 4800) {
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < nFrames; ++i) fr.push_back(tx::sampleFrame(i % 24, 1000 + (uint32_t)i));
    std::vector<float> audio = tx::modulate(fr, tc);
    vibebpsk::Rx::Cfg c;
    Result r;
    Run run(c, invertRx, Run::Out{ nullptr, nullptr, nullptr, nullptr });
    run.rxMut().forceScalar = forceScalar;
    std::vector<uint8_t> got;
    if (symsOut) { auto& rx = run.rxMut(); auto prev = rx.onSymbol; rx.onSymbol = [prev, symsOut](float s) { symsOut->push_back(s); prev(s); }; }
    for (size_t i = 0; i < audio.size(); i += blk) run.feedAudio(audio.data() + i, (int)std::min<size_t>(blk, audio.size() - i));
    r.good = (int)run.framer().good(); r.syncs = run.framer().syncs(); r.failed = run.framer().failed(); r.afc = run.rx().afcHz();
    return r;
}

static void s4() {
    std::printf("§4 the whole receiver: our transmitter → audio → frames\n");
    const int nf = g_quick ? 6 : 12;
    tx::Cfg c; c.ebn0Db = 99;
    Result r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "clean, on 1200 Hz: %d / %d frames", r.good, nf);
    /* ★★ THE SERVER's 20 ms BLOCKS, OFF CENTRE (2026-11-21): a fresh run used to think itself locked until its first
     *  frame — one hunt ran only if the first block was ≥ 53 ms, so 960-sample blocks never found ±150 Hz (the benchmark
     *  row decoded 0). Every frame, from the first. */
    {
        tx::Cfg b; b.offsetHz = 150; b.ebn0Db = 12;
        Result rb = runChain(b, 4, false, false, nullptr, 960);
        CHECK(rb.good == 4, "20 ms blocks, +150 Hz off, Eb/N0 12 dB: %d / 4 frames (the first one too)", rb.good);
        b.offsetHz = -500;
        rb = runChain(b, 4, false, false, nullptr, 960);
        CHECK(rb.good == 4, "20 ms blocks, −500 Hz off: %d / 4 frames", rb.good);
    }
    c.offsetHz = 350; c.rampHzS = 8; c.clockPpm = 60; c.phase = 2.1; c.ebn0Db = 14;
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "+350 Hz off, an 8 Hz/s Doppler ramp, 60 ppm clock, Eb/N0 14 dB: %d / %d (AFC %.0f Hz)", r.good, nf, r.afc);
    c.offsetHz = -500; c.rampHzS = -6; c.invertBits = false;   // ★ the air's is true (a 1 = no phase change)
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "−500 Hz, the other polarity: %d / %d (AFC %.0f Hz)", r.good, nf, r.afc);
    /* ★★★ 11-24 THE SATELLITE'S CLOCK: AO-73 sends ~1201.6 Bd (−1400 ppm here: a shorter symbol). The proportional-only
     *  timing loop held ±800 ppm and slipped all pass (0 syncs on a 12 dB signal). Both signs, and well past it. */
    for (double ppm : {-1400.0, 1400.0, -3000.0}) {
        tx::Cfg k; k.offsetHz = 150; k.clockPpm = ppm; k.ebn0Db = 12; k.seed = 77;
        Result q = runChain(k, nf, false, false, nullptr, 960);
        CHECK(q.good >= nf - 1, "the symbol clock %+.0f ppm (%.1f Bd), 20 ms blocks, Eb/N0 12 dB: %d / %d", ppm, 1200.0 / (1.0 + ppm * 1e-6), q.good, nf);
    }
    c.differential = false; c.offsetHz = 120; c.invertBits = false; c.clockPpm = 60;
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "no differential code (coherent form): %d / %d", r.good, nf);
    std::printf("   frame rate against Eb/N0 (offset +200 Hz, 4 Hz/s ramp, 30 ppm):\n");
    tx::Cfg k; k.offsetHz = 200; k.rampHzS = 4; k.clockPpm = 30;
    int at8 = 0;
    for (double eb : {12.0, 10.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0}) {
        k.ebn0Db = eb; k.seed = (uint32_t)(eb * 10);
        Result q = runChain(k, nf);
        std::printf("     Eb/N0 %4.1f dB: %2d / %d frames (%llu syncs, %llu failed)\n", eb, q.good, nf, (unsigned long long)q.syncs, (unsigned long long)q.failed);
        if (eb == 8.0) at8 = q.good;
    }
    CHECK(at8 >= nf * 3 / 4, "Eb/N0 8 dB: %d / %d frames", at8, nf);
}

static void s5() {
    std::printf("§5 SIMD (kernel %d) against scalar\n", vibebpsk::kernel());
    tx::Cfg c; c.offsetHz = 210; c.ebn0Db = 10;
    std::vector<float> a, b;
    runChain(c, 2, false, false, &a);
    runChain(c, 2, false, true, &b);
    CHECK(a.size() == b.size() && !a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "%zu symbols bit-identical", a.size());
}

using vibe::sat::json::Value; namespace json = vibe::sat::json;
static void s6() {
    std::printf("§6 the values and the JSON\n");
    std::vector<uint8_t> f = tx::sampleFrame(13, 0x123456, "HELLO FROM THE TEST\x01");
    Decoded d = parse(f.data());
    CHECK(d.satId == 2 && d.frameType == 13 && d.frameName == "FM1", "FUNcube-1, type 13 = FM1 (the air counts 0–23, WO1 = 0)");
    CHECK(frameName(0) == "WO1" && frameName(11) == "WO12" && frameName(12) == "HR1" && frameName(16) == "HR2" && frameName(20) == "HR3"
          && frameName(23) == "FM9" && frameName(24) == "type 24", "the schedule 0–23: WO1–12, HR1, FM1–3, HR2, FM4–6, HR3, FM7–9");
    CHECK(d.seq == 0x123456 && d.mode == "Eclipse", "sequence %lld, mode %s", d.seq, d.mode.c_str());
    CHECK(d.values.size() == 58 && d.values[4].key == "eps.vbat" && d.values[4].v == 1444 && d.values[4].raw, "58 values; battery voltage raw 1444");
    CHECK(d.values[6].key == "eps.reboots" && !d.values[6].raw, "a count is exact, not raw");
    CHECK(d.fitter == "HELLO FROM THE TEST.", "fitter text, whitelisted (%s)", d.fitter.c_str());
    const std::string j = tlmJson(d, f.data(), f.size(), "AO-73", 1795600000.0);
    json::Value v;
    CHECK(json::parseValue(j, v) && v.str("type") == "sat_tlm", "sat_tlm parses");
    const json::Value* dd = v.get("data");
    CHECK(dd && dd->str("sat") == "AO-73" && dd->str("format") == "funcube-rt" && dd->num("seq") == 0x123456 && dd->get("values")
          && dd->get("values")->a.size() == 58 && dd->str("hex").size() == 512, "the contract's fields");
    Decoded w = parse(tx::sampleFrame(2, 9).data());
    CHECK(w.fitter.empty() && w.frameName == "WO3", "a whole-orbit frame carries no fitter text");
    // ★★ 11-24 on air: an empty slot is "..", and FM9 carries binary — neither is a message
    Decoded e = parse(tx::sampleFrame(21, 9, "..").data());
    CHECK(e.frameName == "FM7" && e.fitter.empty(), "an empty fitter slot (\"..\") shows nothing");
    std::vector<uint8_t> bin = tx::sampleFrame(23, 9);
    const uint8_t air[] = {0xff, 0x0e, 0x09, 0xff, 0xff, 0xff, 0x00};   // FM9's payload as AO-73 sent it 11-24
    std::memcpy(&bin[56], air, sizeof air);
    Decoded b9 = parse(bin.data());
    CHECK(b9.frameName == "FM9" && b9.fitter.empty(), "binary in a fitter slot is not shown as text (%s)", b9.fitter.c_str());
}

static void s7() {
    std::printf("§7 cost\n");
    tx::Cfg c; c.ebn0Db = 12; c.offsetHz = 150;
    std::vector<std::vector<uint8_t>> fr; for (int i = 0; i < 4; ++i) fr.push_back(tx::sampleFrame(i + 1, (uint32_t)i));
    std::vector<float> audio = tx::modulate(fr, c);
    vibebpsk::Rx::Cfg rc;
    Run run(rc, false, Run::Out{});
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < audio.size(); i += 4800) run.feedAudio(audio.data() + i, (int)std::min<size_t>(4800, audio.size() - i));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double aud = audio.size() / 48000.0;
    std::printf("   %.1f s of audio in %.3f s: %.3f %% of one core (this machine)\n", aud, sec, 100.0 * sec / aud);
    CHECK(sec / aud < 0.05, "under 5 %% of a core here");
}

// ── §8 / §9 ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** The pick's listening setup (old or new). */
struct Pick { double centre, range, bw; const char* name; };
static const Pick kOld{1200, 900, 2650, "OLD 1200 Hz / ±900 Hz / 2.65 kHz USB"};
static const Pick kNew{vibe::sat::kTlmCentreHz, vibe::sat::kTlmAfcHz, vibe::sat::kTlmAudioHiHz - vibe::sat::kTlmAudioLoHz, "NEW 3800 Hz / ±2600 Hz / 7.5 kHz USB"};
/** A USB demodulator with a 0..bw passband: x (real, the "RF" at 48 kHz) × e^{−jφ_LO}, then only 0..bw kept (a complex
 *  band-pass: shift by −bw/2, low-pass at bw/2, shift back) — what the server's SsbDemod does. The LO's frequency is
 *  set per block (the dial + the trim): phase-continuous. */
struct UsbChannel {
    double rate = 48000, bw = 2650, ph = 0, ph2 = 0;
    std::vector<float> h; vibebpsk::Ring r;
    explicit UsbChannel(double bwHz) : bw(bwHz) { h = vibebpsk::lowpass(128, bw / 2, rate); r.init(h.size()); }
    void run(const float* x, float* y, int n, double loHz) {
        const double w = 2 * M_PI * loHz / rate, w2 = 2 * M_PI * (bw / 2) / rate;
        for (int i = 0; i < n; ++i) {
            // down by the LO and by bw/2 (the passband's middle to DC), low-pass, back up by bw/2, the real part
            const double a = ph + ph2;
            r.push(float(x[i] * std::cos(a)), float(-x[i] * std::sin(a)));
            const float zr = vibebpsk::dot(r.wr(), h.data(), h.size()), zi = vibebpsk::dot(r.wi(), h.data(), h.size());
            y[i] = 2.0f * float(zr * std::cos(ph2) - zi * std::sin(ph2));
            ph += w; ph2 += w2;
            if (ph > M_PI) ph -= 2 * M_PI;
            if (ph2 > M_PI) ph2 -= 2 * M_PI;
        }
    }
};
/** The satellite engine's trim, as Engine::tick moves a telemetry pick's (kDecTrimSlewHzS, clamped to kTrimClampHz). */
struct TrimModel {
    double applied = 0, target = 0;
    void tick(double dt) { const double st = vibe::sat::Engine::kDecTrimSlewHzS * dt; applied += std::max(-st, std::min(st, target - applied)); }
    void ask(double t) { target = std::max(-vibe::sat::Engine::kTrimClampHz, std::min(vibe::sat::Engine::kTrimClampHz, t)); }
};
struct PassResult {
    int good = 0, moves = 0, reversals = 0; double afc = 0, trim = 0, errEnd = 0, maxOver = 0, lastMv = 0; std::string log;
    std::vector<double> offs;   // the signal against the channel's centre, once a second
};
static constexpr double kPredHz = 12000;   // the prediction, as an "RF" tone in the 48 kHz test signal
/** One pass: frames sent at kPredHz + errHz (+ driftHzS·t), heard through the pick `pk`. `follow`: the locked decoder
 *  steers the trim (the new pick's behaviour); otherwise the channel stays on the prediction. */
static PassResult pass(const Pick& pk, double errHz, double driftHzS, int nFrames, bool follow, double ebn0 = 14, uint32_t seed = 5) {
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < nFrames; ++i) fr.push_back(tx::sampleFrame(i % 24, 2000 + (uint32_t)i));
    tx::Cfg tc; tc.carrierHz = kPredHz; tc.offsetHz = errHz; tc.rampHzS = driftHzS; tc.ebn0Db = ebn0; tc.seed = seed;
    const std::vector<float> rf = tx::modulate(fr, tc);
    UsbChannel ch(pk.bw);
    TrimModel tm;
    vibebpsk::Rx::Cfg rc; rc.centreHz = pk.centre; rc.afcRangeHz = pk.range;
    PassResult res;
    Run::Out o{};
    if (follow) {
        o.trimNow = [&](double& a, double& t) { a = tm.applied; t = tm.target; return true; };
        o.trimTo = [&](double t) {
            const double was = tm.target;
            tm.ask(t);
            const double mv = tm.target - was;
            if (res.lastMv != 0 && mv != 0 && (mv > 0) != (res.lastMv > 0)) ++res.reversals;
            if (mv != 0) res.lastMv = mv;
            ++res.moves;
        };
        o.log = [&](const std::string& l) { if (res.log.empty()) res.log = l; };
        o.satName = [] { return std::string("AO-73"); };
    }
    Run run(rc, false, std::move(o));
    const int blk = 960;   // the server's 20 ms blocks
    const int lagBlk = 15; // ★ the trim reaches the audio 0.3 s after the engine applies it (the pipeline)
    std::deque<double> hist(lagBlk + 1, 0.0);
    std::vector<float> a(blk);
    for (size_t i = 0; i < rf.size(); i += blk) {
        const int n = (int)std::min<size_t>(blk, rf.size() - i);
        tm.tick(n / 48000.0);
        hist.push_back(tm.applied); hist.pop_front();
        const double lo = kPredHz - pk.centre + hist.front();   // the dial (downlink − centre) + the trim (late)
        ch.run(rf.data() + i, a.data(), n, lo);
        const uint64_t before = run.rx().samples() / 48000;
        run.feedAudio(a.data(), n);
        if (run.rx().samples() / 48000 != before) {
            const double t = (double)(i + n) / 48000.0;
            const double truth = errHz + driftHzS * t;          // the signal against the prediction
            res.offs.push_back(truth - hist.front());           // …against the channel's centre
            if (follow) res.maxOver = std::max(res.maxOver, (errHz >= 0 ? 1 : -1) * (hist.front() - truth));
        }
    }
    res.good = (int)run.framer().good(); res.afc = run.rx().afcHz(); res.trim = tm.applied;
    const double tEnd = rf.size() / 48000.0;
    res.errEnd = errHz + driftHzS * tEnd - tm.applied;
    return res;
}

static void s8() {
    std::printf("§8 ★★★ the wide search and the drift follow (beta92 — AO-73 1.3 kHz above the prediction)\n");
    const int nf = g_quick ? 12 : 24;
    struct Case { double err, drift; const char* what; };
    const Case cases[] = { {1300, 0, "+1.3 kHz (Stuart's 11-23 pass)"}, {-2000, 0, "−2.0 kHz"} };
    for (const Case& c : cases) {
        PassResult o = pass(kOld, c.err, c.drift, nf, false);
        CHECK(o.good == 0, "%s on the %s pick: %d / %d frames", c.what, kOld.name, o.good, nf);
        PassResult w = pass(kNew, c.err, c.drift, nf, false);
        CHECK(w.good >= nf - 2, "%s on the %s pick, no follow: %d / %d frames (AFC %+.0f Hz)", c.what, kNew.name, w.good, nf, w.afc);
        PassResult f = pass(kNew, c.err, c.drift, nf, true);
        CHECK(f.good >= nf - 2, "%s, following: %d / %d frames", c.what, f.good, nf);
        CHECK(std::fabs(f.trim - c.err) < 120 && std::fabs(f.errEnd) < 120,
              "  the trim converged: %+.0f Hz (the error %+.0f) — the signal ends %+.0f Hz from the centre (decoder AFC %+.0f)",
              f.trim, c.err, f.errEnd, f.afc);
        CHECK(f.moves >= 1 && f.moves <= 3 && f.reversals == 0 && f.maxOver < 120,
              "  no ringing: %d move(s), %d reversal(s), overshoot %.0f Hz", f.moves, f.reversals, f.maxOver);
        if (!f.log.empty()) std::printf("     log: %s\n", f.log.c_str());
    }
    {   // ★ the wide hunt costs no weak-signal margin: the old pick ON its centre against the new one 1.3 kHz out, following
        std::printf("     weak signals (Eb/N0): the old pick with the signal ON its centre | the new one 1.3 kHz out, following\n");
        int newAt6 = 0;
        for (double eb : {8.0, 6.0, 5.0, 4.0}) {
            PassResult a0 = pass(kOld, 0, 0, nf, false, eb, (uint32_t)(eb * 7));
            PassResult b0 = pass(kNew, 1300, 0, nf, true, eb, (uint32_t)(eb * 7));
            std::printf("     Eb/N0 %3.1f dB: %2d / %d | %2d / %d\n", eb, a0.good, nf, b0.good, nf);
            if (eb == 6.0) newAt6 = b0.good;
        }
        CHECK(newAt6 >= nf * 3 / 4, "Eb/N0 6 dB, 1.3 kHz out, wide + following: %d / %d frames", newAt6, nf);
    }
    {   // the passband alone was not the fix: the old ±900 Hz hunt in a wide passband still cannot reach +1.3 kHz
        PassResult n0 = pass(Pick{1200, 900, 7500, "1200 / ±900 / 7.5 kHz"}, 1300, 0, nf, false);
        CHECK(n0.good == 0, "+1.3 kHz, the passband widened but the old ±900 Hz hunt: %d / %d (the range was the other half)", n0.good, nf);
    }
    {   // ★ a slow drift: 300 Hz a minute from +1.3 kHz — the follow keeps it near the centre
        const int nd = g_quick ? 30 : 48;   // 2.5 / 4 minutes
        PassResult d = pass(kNew, 1300, 5.0, nd, true);
        double worst = 0;
        for (size_t i = d.offs.size() / 3; i < d.offs.size(); ++i) worst = std::max(worst, std::fabs(d.offs[i]));
        CHECK(d.good >= nd - 3, "+1.3 kHz drifting 300 Hz/min (%.1f min, ends %+.0f Hz off the prediction), following: %d / %d frames",
              nd * 5.0 / 60.0, 1300 + 5.0 * nd * 5.0, d.good, nd);
        CHECK(worst < 300 && std::fabs(d.errEnd) < 250 && d.reversals == 0,
              "  the signal held within %.0f Hz of the centre after the first minute (%d moves, %d reversals, ends %+.0f Hz)",
              worst, d.moves, d.reversals, d.errEnd);
        PassResult nd0 = pass(kNew, 1300, 5.0, nd, false);
        std::printf("     (without the follow: %d / %d frames, the signal ends %+.0f Hz from the centre)\n", nd0.good, nd, nd0.errEnd);
    }
}

static void s9() {
    std::printf("§9 ★ noise at the wide range: no frame, the trim never moves\n");
    std::mt19937 rng(23);
    std::normal_distribution<double> g(0, 1);
    vibebpsk::Rx::Cfg c; c.centreHz = vibe::sat::kTlmCentreHz; c.afcRangeHz = vibe::sat::kTlmAfcHz;
    int frames = 0, asks = 0;
    Run::Out o{ [&](const std::string& j) { if (j.find("sat_tlm") != std::string::npos) ++frames; }, nullptr, nullptr, nullptr };
    o.trimNow = [](double& a, double& t) { a = 0; t = 0; return true; };
    o.trimTo = [&](double) { ++asks; };
    Run run(c, false, std::move(o));
    const int secs = g_quick ? 120 : 600;
    UsbChannel ch(kNew.bw);
    std::vector<float> w(960), a(960);
    for (int s = 0; s < secs * 50; ++s) {
        for (auto& v : w) v = float(0.1 * g(rng));
        ch.run(w.data(), a.data(), 960, 9000);
        run.feedAudio(a.data(), 960);
    }
    CHECK(frames == 0 && asks == 0, "%d s of white noise through the 7.5 kHz passband and the ±2600 Hz hunt: %d frames, %d trim requests (%llu syncs refused)",
          secs, frames, asks, (unsigned long long)run.framer().syncs());
    vibebpsk::Rx rx(c);
    CHECK(rx.afcRangeHz() == vibe::sat::kTlmAfcHz, "the wide receiver's range: %.0f Hz", rx.afcRangeHz());
}

/** 16-bit PCM mono WAV → float (the fixture's format; nothing else is accepted). */
static bool readWav16(const char* fn, std::vector<float>& out, int& rate) {
    FILE* f = std::fopen(fn, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int ch = 0, bits = 0; rate = 0;
    while (p + 8 <= d.size()) {
        uint32_t sz; std::memcpy(&sz, &d[p + 4], 4);
        if (!std::memcmp(&d[p], "fmt ", 4) && sz >= 16) {
            uint16_t c, b; uint32_t r; std::memcpy(&c, &d[p + 10], 2); std::memcpy(&r, &d[p + 12], 4); std::memcpy(&b, &d[p + 22], 2);
            ch = c; rate = (int)r; bits = b;
        } else if (!std::memcmp(&d[p], "data", 4)) {
            if (ch != 1 || bits != 16) return false;
            const size_t end = std::min(d.size(), p + 8 + (size_t)sz);
            for (size_t q = p + 8; q + 2 <= end; q += 2) { int16_t v; std::memcpy(&v, &d[q], 2); out.push_back(v / 32768.f); }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

static void s10() {
    std::printf("§10 ★★★ THE REAL SIGNAL: AO-73 on air, 2026-11-24 (the Pi 500 V4L's decoder input, 23.5 s)\n");
    /* The decoder's exact input (VIBE_DEC_AUDIO_DUMP) from the pass that decoded nothing live: a satellite pick (centre
     * 3800, ±2600 Hz), 20 ms blocks as the server feeds them, the hunt from cold. Before the fix: 0 syncs, 0 frames.
     * Cut from v4l-dec-1795534491-fctlm-6.wav (our own recording), 16-bit 48 kHz mono, normalised. */
    const char* fn = std::getenv("VIBE_FCTLM_REAL");
    if (!fn) fn = "test/fixtures/funcube/ao73-v4l-2026-11-24-23s.wav";
    std::vector<float> a; int rate = 0;
    const bool ok = readWav16(fn, a, rate);
    CHECK(ok && rate == 48000 && a.size() > 48000 * 20, "the recording reads (%s, %zu samples at %d Hz)", fn, a.size(), rate);
    if (!ok) return;
    vibebpsk::Rx::Cfg c; c.centreHz = vibe::sat::kTlmCentreHz; c.afcRangeHz = vibe::sat::kTlmAfcHz;
    std::vector<Decoded> got; int falseIds = 0;
    Run::Out o{ nullptr, nullptr, nullptr, nullptr };
    o.record = [&](const Decoded& d, const TlmCtx&) { got.push_back(d); if (d.satId != 2) ++falseIds; };
    Run run(c, false, std::move(o));
    for (size_t i = 0; i + 960 <= a.size(); i += 960) run.feedAudio(&a[i], 960);
    std::string names; for (auto& d : got) names += d.frameName + " ";
    CHECK(got.size() >= 4, "%zu frames from 23.5 s (5 s apart, the first after the hunt): %s(%llu syncs, %llu failed)",
          got.size(), names.c_str(), (unsigned long long)run.framer().syncs(), (unsigned long long)run.framer().failed());
    CHECK(falseIds == 0, "every frame says FUNcube-1 (satellite id 2)");
    // what AO-73 sent: FM7, FM8, FM9 of sequence 1023471, then WO1 of 1023472 — the counter steps on type 0
    const bool sched = got.size() >= 4 && got[0].frameName == "FM7" && got[0].seq == 1023471 && got[1].frameName == "FM8"
                       && got[2].frameName == "FM9" && got[3].frameName == "WO1" && got[3].seq == 1023472;
    CHECK(sched, "FM7 · FM8 · FM9 of seq 1023471, then WO1 of seq 1023472 (types 21, 22, 23, 0)");
    // sane values: the battery in its raw range, the reboot count steady across the frames, sunlit
    bool sane = !got.empty();
    for (auto& d : got) {
        sane = sane && d.values.size() == 58 && d.values[4].key == "eps.vbat" && d.values[4].v > 7000 && d.values[4].v < 9000;
        sane = sane && d.values[6].key == "eps.reboots" && d.values[6].v == got[0].values[6].v && d.mode == "Sunlight";
    }
    CHECK(sane, "battery raw %.0f (≈ 7.9 V read as mV), reboot count %.0f in every frame, sunlit",
          got.empty() ? 0.0 : got[0].values[4].v, got.empty() ? 0.0 : got[0].values[6].v);
    const double baud = run.rx().baudHz();
    CHECK(baud > 1201.0 && baud < 1202.5, "the timing loop learnt the satellite's clock: %.2f Bd (1200 nominal)", baud);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--quick") g_quick = true;
    s1(); s2(); s3(); s4(); s5(); s6(); s7(); s8(); s9(); s10();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
