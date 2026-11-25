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
//   §11 ★★★ beta94 FUNcube-1's UNITS: known counts → AMSAT-UK's published equations; a board's data-valid flag off → hidden
//   §12 ★★★ beta94 JY1Sat: the extended id, its own layout (data budget 2F), the 11-25 JO-97 pass rebuilt from its printout
//   §10 ★★★ THE REAL SIGNAL (2026-11-24): 23.5 s of AO-73 as the Pi 500's decoder heard it, which decoded NOTHING live —
//      the satellite's clock is ~1400 ppm fast (the timing loop now integrates), a 1 is no phase change (the test
//      transmitter's default now), the frame type counts 0–23. Frames, their schedule, sane values, the learnt clock.
//      ★★ beta93: the same air moved as the 1500 Hz pick hears it (dial shifted in the test) — the frames hold.
// Silent: no audio device. `--quick` shortens §3/§4.
#include "vibe_fctlm_tx.h"
#include "vibe_sat_orbit.h"   // its json:: parser (for the §6 checks)
#include "vibe_sat_engine.h"  // ★ beta92 the pick's numbers (kTlmCentreHz …) and the trim's clamp and slew

#include <chrono>
#include <cmath>
#include <map>
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
    CHECK(d.values.size() == 58 && d.values[4].key == "eps.vbat" && std::fabs(d.values[4].v - 1.444) < 1e-9 && d.values[4].unit == "V"
          && !d.values[4].raw, "58 values; battery voltage 1.444 V (the EPS sends millivolts: 1444)");
    CHECK(d.values[6].key == "eps.reboots" && !d.values[6].raw && d.values[6].unit.empty(), "a count is exact, no unit, not raw");
    CHECK(d.layout == "FUNcube-1" && d.absent.empty(), "FUNcube-1's layout; every board's data valid");
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
static const Pick kNew{vibe::sat::kTlmCentreHz, vibe::sat::kTlmAfcHz, vibe::sat::kTlmAudioHiHz - vibe::sat::kTlmAudioLoHz, "NEW 1500 Hz / +2600 −900 Hz / 4.9 kHz USB"};
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
    /* ★★ beta93 the carrier at 1500 Hz (Stuart: 3800 "sounds thin and high-pitched"): below the centre the hunt now reaches
     *  a 300 Hz carrier (−1200) — −2.0 kHz (beta92's case) would be below 0 Hz in USB audio. −0.8 kHz: a 700 Hz carrier,
     *  its mirror at −700 Hz, never taken (the trim converges to −800, not to the mirror). The 11-24 pass was −0.6. */
    const Case cases[] = { {1300, 0, "+1.3 kHz (Stuart's 11-23 pass)"}, {-800, 0, "−0.8 kHz (11-24 was −0.6)"} };
    for (const Case& c : cases) {
        PassResult o = pass(kOld, c.err, c.drift, nf, false);
        PassResult w = pass(kNew, c.err, c.drift, nf, false);
        // ★ beta93: −0.8 kHz is inside the old pick's reach too (a 400 Hz carrier) — only +1.3 kHz was ever out of it
        if (c.err > 0) CHECK(o.good == 0, "%s on the %s pick: %d / %d frames", c.what, kOld.name, o.good, nf);
        else std::printf("     (%s on the %s pick: %d / %d — inside its reach)\n", c.what, kOld.name, o.good, nf);
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
    {   // ★★ beta93 ON the prediction: the carrier at 1500 Hz in the audio, the AFC at ~0 from the start
        PassResult z = pass(kNew, 0, 0, nf, false);
        CHECK(z.good >= nf - 1 && std::fabs(z.afc) < 40, "on the prediction, the NEW pick: %d / %d frames, the carrier at %.0f Hz audio (AFC %+.0f)",
              z.good, nf, kNew.centre + z.afc, z.afc);
        PassResult b = pass(kNew, -1000, 0, nf, false);
        CHECK(b.good >= nf - 2, "−1.0 kHz (a 500 Hz carrier, low in the audio): %d / %d frames (AFC %+.0f)", b.good, nf, b.afc);
        vibebpsk::Rx::Cfg rc; rc.centreHz = kNew.centre; rc.afcRangeHz = kNew.range;
        vibebpsk::Rx rx(rc);
        CHECK(rx.afcLoHz() == -1200 && rx.afcHiHz() == 2600, "the NEW pick's search window: %+.0f … %+.0f Hz (a carrier from 300 to 4100 Hz)", rx.afcLoHz(), rx.afcHiHz());
        vibebpsk::Rx::Cfg hc; vibebpsk::Rx hand(hc);
        CHECK(hand.afcLoHz() == -900 && hand.afcHiHz() == 900, "by hand (centre 1200, ±900): %+.0f … %+.0f Hz — as before", hand.afcLoHz(), hand.afcHiHz());
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
        /* ★ beta93: with the 11-24 receiver this case decodes SOME frames — the hunt, capped at +900, takes the squared
         *  signal's side line (2Δf − baud) and the loop sits half a baud (600 Hz) off the carrier, where the differential
         *  decode still reads every bit inverted. Real frames (RS-checked), at the wrong AFC — so the satellite pick's own
         *  window (centre 1500, +2600) is still what makes +1.3 kHz work properly; printed, not pinned. */
        std::printf("     +1.3 kHz, the passband widened but the old ±900 Hz hunt: %d / %d (AFC %+.0f — a half-baud lock, not the carrier)\n", n0.good, nf, n0.afc);
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

/** Real USB audio heard with the dial `shiftHz` HIGHER: only [lo, hi] Hz kept (a complex band-pass — the analytic
 *  signal of that band), moved down by shiftHz, the real part. What a USB demodulator gives on the moved dial. */
static std::vector<float> shiftDown(const std::vector<float>& x, double rate, double shiftHz, double lo, double hi) {
    const int n = 256;
    const std::vector<float> h = vibebpsk::lowpass(n, (hi - lo) / 2, rate);
    const double fc = (lo + hi) / 2, m = (n - 1) / 2.0;
    std::vector<double> hr((size_t)n), hq((size_t)n);
    for (int i = 0; i < n; ++i) { const double ang = 2 * M_PI * fc / rate * (i - m); hr[(size_t)i] = h[(size_t)i] * std::cos(ang); hq[(size_t)i] = h[(size_t)i] * std::sin(ang); }
    std::vector<float> y(x.size(), 0.f);
    const double w = 2 * M_PI * shiftHz / rate;
    for (size_t k = 0; k < x.size(); ++k) {
        double zr = 0, zi = 0;
        for (int i = 0; i < n; ++i) {
            const long j = (long)k - i + n / 2;
            if (j < 0 || j >= (long)x.size()) continue;
            zr += x[(size_t)j] * hr[(size_t)i]; zi += x[(size_t)j] * hq[(size_t)i];
        }
        const double ph = std::fmod(w * (double)k, 2 * M_PI);
        y[k] = float(2.0 * (zr * std::cos(ph) + zi * std::sin(ph)));
    }
    return y;
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
    // sane values: the battery in volts, the reboot count steady across the frames, sunlit
    bool sane = !got.empty();
    for (auto& d : got) {
        sane = sane && d.values.size() == 58 && d.values[4].key == "eps.vbat" && d.values[4].v > 7.0 && d.values[4].v < 9.0;
        sane = sane && d.values[6].key == "eps.reboots" && d.values[6].v == got[0].values[6].v && d.mode == "Sunlight";
    }
    CHECK(sane, "battery %.3f V, reboot count %.0f in every frame, sunlit",
          got.empty() ? 0.0 : got[0].values[4].v, got.empty() ? 0.0 : got[0].values[6].v);
    /* ★★ beta94 THE UNITS ON THE AIR: every frame of the real pass, in AMSAT-UK's published units, inside what a healthy
     * sunlit FUNcube-1 reads (the handbook: 6.8–8.2 V battery, 300 mW beacon in sunlight; the EPS's own 22 °C). */
    bool units = !got.empty();
    std::string why;
    auto in = [&](const Decoded& d, const char* k, const char* unit, double lo, double hi) {
        const vibefc::Value* v = d.find(k);
        const bool ok = v && v->valid && v->unit == unit && !v->raw && v->v >= lo && v->v <= hi;
        if (!ok && why.empty()) why = std::string(k) + "=" + (v ? std::to_string(v->v) + " " + v->unit : "missing");
        return ok;
    };
    for (auto& d : got) {
        units = units && d.layout == "FUNcube-1" && d.absent.empty();
        units = units && in(d, "eps.vbat", "V", 7.5, 8.3) && in(d, "eps.pv1", "V", 3.0, 5.5) && in(d, "eps.ipv", "mA", 100, 500);
        units = units && in(d, "eps.isys", "mA", 100, 400) && in(d, "eps.tbat", "\xc2\xb0" "C", 0, 40);
        for (const char* k : {"bob.tpxp", "bob.tpxm", "bob.tpyp", "bob.tpym"}) units = units && in(d, k, "\xc2\xb0" "C", 0, 45);
        units = units && in(d, "bob.v33", "V", 3.2, 3.4) && in(d, "bob.v50", "V", 4.8, 5.1) && in(d, "bob.i33", "mA", 50, 300);
        units = units && in(d, "rf.temp", "\xc2\xb0" "C", 10, 40) && in(d, "pa.temp", "\xc2\xb0" "C", 20, 50);
        units = units && in(d, "pa.fwd", "mW", 250, 350) && in(d, "pa.i", "mA", 60, 160);
        units = units && in(d, "ant.t0", "\xc2\xb0" "C", 10, 40) && in(d, "ant.t1", "\xc2\xb0" "C", 10, 40);
        const vibefc::Value* rc = d.find("eps.reset");
        units = units && rc && rc->text == "Watchdog reset";
        const vibefc::Value* rs = d.find("rf.rssi");
        units = units && rs && rs->raw && !rs->note.empty();   // the one kind left raw: the sheet says "use raw values"
    }
    const vibefc::Value* b0 = got.empty() ? nullptr : got[0].find("eps.vbat");
    const vibefc::Value* f0 = got.empty() ? nullptr : got[0].find("pa.fwd");
    CHECK(units, "%zu real frames in their units: battery %.3f V, PA forward %.0f mW (the 300 mW sunlit beacon), panels, buses "
                 "3.28 / 4.96 V, temperatures 14–37 \xc2\xb0" "C; RSSI raw with its reason%s%s", got.size(), b0 ? b0->v : 0.0,
          f0 ? f0->v : 0.0, why.empty() ? "" : " — first out: ", why.c_str());
    const double baud = run.rx().baudHz();
    CHECK(baud > 1201.0 && baud < 1202.5, "the timing loop learnt the satellite's clock: %.2f Bd (1200 nominal)", baud);
    /* ★★ beta93 THE SAME AIR, AS THE NEW PICK HEARS IT. The recording was made with the dial 3800 Hz below the downlink;
     * the pick now sits 1500 below, i.e. 2300 Hz higher — the same as moving every audio frequency 2300 Hz down (only
     * what was above the new 100 Hz edge kept). Then the carrier lands where THAT pass's error puts it (~880 Hz: the
     * bird was ~0.6 kHz below the prediction), and, moved 1700 Hz, where a pass ON the prediction would (~1480 Hz). */
    struct Sh { double hz; const char* what; double afcLo, afcHi; };
    for (const Sh& sh : {Sh{2300, "as the 1500 Hz pick would have heard this pass (carrier ~880 Hz)", -700, -500},
                         Sh{1700, "the carrier moved to ~1480 Hz (a pass ON the prediction)", -100, 100}}) {
        const std::vector<float> b = shiftDown(a, 48000, sh.hz, sh.hz + 100, 7400);
        std::vector<Decoded> g2;
        Run::Out o2{ nullptr, nullptr, nullptr, nullptr };
        o2.record = [&](const Decoded& d, const TlmCtx&) { g2.push_back(d); };
        Run r2(c, false, std::move(o2));
        double afcAtFirst = NAN;
        for (size_t i = 0; i + 960 <= b.size(); i += 960) { r2.feedAudio(&b[i], 960); if (std::isnan(afcAtFirst) && !g2.empty()) afcAtFirst = r2.rx().afcHz(); }
        std::string n2; for (auto& d : g2) n2 += d.frameName + " ";
        CHECK(g2.size() >= 4 && g2[0].seq == 1023471 && g2.back().frameName == "WO1" && afcAtFirst > sh.afcLo && afcAtFirst < sh.afcHi,
              "%s: %zu frames (%s), AFC %+.0f Hz at the first", sh.what, g2.size(), n2.c_str(), afcAtFirst);
    }
}

// ── §11 / §12 ★★★ beta94 UNITS AND JY1SAT ──────────────────────────────────────────────────────────────────────────
/** A frame written field by field (MSB first) from a layout list: each (key, width) takes `vals[key]` or 0. */
struct FieldW { const char* k; int w; };
static std::vector<uint8_t> writeFrame(uint8_t b0, int b1, const std::vector<FieldW>& lay, const std::map<std::string, uint32_t>& vals) {
    std::vector<uint8_t> d(256, 0);
    size_t pos = 0;
    auto put = [&](uint32_t v, int w) { for (int i = w - 1; i >= 0; --i, ++pos) if ((v >> i) & 1u) d[pos >> 3] |= uint8_t(0x80 >> (pos & 7)); };
    put(b0, 8);
    if (b1 >= 0) put((uint32_t)b1, 8);
    for (const FieldW& f : lay) { auto it = vals.find(f.k); put(it == vals.end() ? 0u : it->second, f.w); }
    return d;
}
/** The software block, both layouts (48 bits): seq, DTMF ×3, the seven data-valid flags, eclipse, safe, three flags. */
static const std::vector<FieldW> kSw = {{"seq", 24}, {"dtmfn", 6}, {"dtmflast", 5}, {"dtmfok", 1}, {"vasib", 1}, {"veps", 1},
    {"vpa", 1}, {"vrf", 1}, {"v5", 1}, {"vantb", 1}, {"vanta", 1}, {"eclipse", 1}, {"safe", 1}, {"habf", 1}, {"sabf", 1}, {"dep", 1}};
/** FUNcube-1's real-time block — the Downlink Data document's order and widths (440 bits). */
static std::vector<FieldW> fc1Layout() {
    std::vector<FieldW> l = {{"pv1", 16}, {"pv2", 16}, {"pv3", 16}, {"ipv", 16}, {"vbat", 16}, {"isys", 16}, {"reboots", 16}, {"swerr", 16},
        {"tb1", 8}, {"tb2", 8}, {"tb3", 8}, {"tbat", 8}, {"lu5", 8}, {"lu33", 8}, {"reset", 8}, {"ppt", 8},
        {"sunx", 10}, {"suny", 10}, {"sunz", 10}, {"tpxp", 10}, {"tpxm", 10}, {"tpyp", 10}, {"tpym", 10}, {"v33", 10}, {"i33", 10}, {"v50", 10},
        {"dop", 8}, {"rssi", 8}, {"rft", 8}, {"irx", 8}, {"itx33", 8}, {"itx50", 8}, {"rev", 8}, {"fwd", 8}, {"pat", 8}, {"pai", 8},
        {"at0", 8}, {"at1", 8}, {"d0", 1}, {"d1", 1}, {"d2", 1}, {"d3", 1}};
    l.insert(l.end(), kSw.begin(), kSw.end());
    return l;
}
/** JY1Sat's real-time block — its "data budget with tlm equations 2F" sheet, row by row (432 bits). */
static std::vector<FieldW> jy1Layout() {
    std::vector<FieldW> l = {{"pv1", 14}, {"pv2", 14}, {"pv3", 14}, {"vbat", 14}, {"ip1", 10}, {"ip2", 10}, {"ip3", 10}, {"ipv", 10},
        {"isys", 10}, {"reboots", 8}, {"tb1", 8}, {"tb2", 8}, {"tb3", 8}, {"tbat", 8}, {"lu5", 8}, {"i5", 8}, {"reset", 4}, {"ppt", 4},
        {"imode", 2}, {"ierr", 3}, {"icfg", 1}, {"itemp", 8},
        {"sxp", 10}, {"sxm", 10}, {"syp", 10}, {"sym", 10}, {"szp", 10}, {"szm", 10}, {"v33", 10}, {"iup", 20}, {"v50", 10},
        {"dop", 8}, {"rssi", 8}, {"rft", 8}, {"irx", 8}, {"itx33", 8}, {"itx50", 8}, {"rev", 8}, {"fwd", 8}, {"pat", 8}, {"pai", 8},
        {"at0", 8}, {"at1", 8}, {"d0", 1}, {"d1", 1}, {"d2", 1}, {"d3", 1}};
    l.insert(l.end(), kSw.begin(), kSw.end());
    return l;
}
static bool near(const Decoded& d, const char* k, double want, double tol, const char* unit) {
    const vibefc::Value* v = d.find(k);
    const bool ok = v && v->valid && !v->raw && v->unit == unit && std::fabs(v->v - want) <= tol;
    if (!ok) std::printf("      (%s: %s)\n", k, v ? (std::to_string(v->v) + " " + v->unit + (v->valid ? "" : " invalid") + (v->raw ? " raw" : "")).c_str() : "missing");
    return ok;
}
static const char* kDeg = "\xc2\xb0" "C";

static void s11() {
    std::printf("§11 ★★★ beta94 FUNcube-1's units: known counts → AMSAT-UK's published equations (telemetry_equations_b1)\n");
    {
        size_t bits = 8; for (auto& f : fc1Layout()) bits += (size_t)f.w;
        size_t jb = 16; for (auto& f : jy1Layout()) jb += (size_t)f.w;
        CHECK(bits == 448 && jb == 448, "both layouts end at bit 448 (FUNcube-1 8 + 440, JY1Sat 16 + 432; payload from byte 56)");
    }
    std::map<std::string, uint32_t> v = {{"pv1", 3963}, {"ipv", 332}, {"vbat", 7876}, {"isys", 215}, {"reboots", 1475}, {"tbat", 0xF6},
        {"tb1", 22}, {"reset", 3}, {"ppt", 1}, {"sunx", 100}, {"suny", 4}, {"sunz", 990}, {"tpxp", 669}, {"tpxm", 660}, {"tpyp", 696},
        {"tpym", 654}, {"v33", 819}, {"i33", 143}, {"v50", 826}, {"dop", 157}, {"rssi", 181}, {"rft", 198}, {"irx", 41}, {"itx33", 51},
        {"itx50", 24}, {"rev", 80}, {"fwd", 208}, {"pat", 126}, {"pai", 188}, {"at0", 142}, {"at1", 166}, {"d0", 1}, {"d1", 1}, {"d2", 1},
        {"d3", 1}, {"seq", 1023471}, {"vasib", 1}, {"veps", 1}, {"vpa", 1}, {"vrf", 1}, {"v5", 1}, {"vantb", 1}, {"vanta", 1}};
    std::vector<uint8_t> f = writeFrame(0x80 | 21, -1, fc1Layout(), v);
    Decoded d = parse(f.data());
    CHECK(d.satId == 2 && d.layout == "FUNcube-1" && d.frameName == "FM7" && d.seq == 1023471 && d.absent.empty(), "FUNcube-1, FM7, seq 1023471");
    CHECK(near(d, "eps.vbat", 7.876, 1e-9, "V") && near(d, "eps.pv1", 3.963, 1e-9, "V") && near(d, "eps.ipv", 332, 0, "mA")
          && near(d, "eps.isys", 215, 0, "mA"), "EPS \"real values are transmitted\": 7876 mV = 7.876 V, 3963 mV = 3.963 V, 332 / 215 mA");
    CHECK(near(d, "eps.tbat", -10, 0, kDeg) && near(d, "eps.tboost1", 22, 0, kDeg), "EPS temperatures are signed bytes: 0xF6 = −10 °C, 22 = 22 °C");
    CHECK(d.find("eps.reset")->text == "Watchdog reset" && d.find("eps.ppt")->text == "MPPT", "reset cause 3 = watchdog, PPT mode 1 = MPPT (the sheet's words)");
    CHECK(near(d, "bob.tpxp", -0.2073 * 669 + 158.239, 1e-9, kDeg) && near(d, "bob.tpxm", -0.2083 * 660 + 159.227, 1e-9, kDeg)
          && near(d, "bob.tpyp", -0.2076 * 696 + 158.656, 1e-9, kDeg) && near(d, "bob.tpym", -0.2087 * 654 + 159.045, 1e-9, kDeg),
          "panel temps, each face its own line: X+ 669 = 19.6 °C, X− 660 = 21.7, Y+ 696 = 14.2, Y− 654 = 22.6");
    CHECK(near(d, "bob.v33", 3.276, 1e-9, "V") && near(d, "bob.v50", 4.956, 1e-9, "V") && near(d, "bob.i33", 143, 0, "mA"),
          "buses: 819 counts = 3.276 V (mV ÷ 4), 826 = 4.956 V (mV ÷ 6), 3.3 V current 1 mA a count");
    CHECK(near(d, "bob.sunx", 1096, 0.5, "lux") && near(d, "bob.suny", 0, 0, "lux") && near(d, "bob.sunz", 26303, 0.5, "lux")
          && !d.find("bob.sunz")->note.empty(), "sun sensors (log-lux table): 100 = 10^3.04 = 1096 lux, 4 = noise (0), 990 = the 984 limit");
    CHECK(near(d, "rf.temp", -0.857 * 198 + 193.672, 1e-9, kDeg) && near(d, "rf.irx", 0.636 * 41, 1e-9, "mA")
          && near(d, "rf.itx33", 0.636 * 51, 1e-9, "mA") && near(d, "rf.itx50", 1.272 * 24, 1e-9, "mA"),
          "RF: 198 = 24.0 °C, receive 41 = 26.1 mA, transmit 3.3 V 51 = 32.4 mA, 5.0 V 24 = 30.5 mA (× 1.272)");
    CHECK(near(d, "pa.fwd", 302.62, 0.01, "mW") && near(d, "pa.rev", 0.005 * std::pow(80.0, 2.0629), 1e-9, "mW")
          && near(d, "pa.temp", 37.125, 1e-9, kDeg) && near(d, "pa.i", 0.5496 * 188 + 2.5435, 1e-9, "mA"),
          "PA: forward 208 = 302.6 mW (0.005 × n^2.0629), reverse 80 = 42.2 mW, temp 126 = 37.1 °C (the table), current 188 = 105.9 mA");
    CHECK(near(d, "ant.t0", 24.39, 0.02, kDeg) && near(d, "ant.t1", -4.43, 0.05, kDeg) && !d.find("ant.t0")->note.empty(),
          "antenna temps (the AntS curve, count as 0–3.3 V, said in its note): 142 = 24.4 °C, 166 = −4.4 °C");
    const vibefc::Value* dop = d.find("rf.doppler"); const vibefc::Value* rssi = d.find("rf.rssi");
    CHECK(dop && dop->raw && dop->v == 157 && !dop->note.empty() && rssi && rssi->raw && rssi->v == 181 && !rssi->note.empty(),
          "Doppler and RSSI stay raw — the sheet: \"use raw values\" — each with its own note");
    int raws = 0; for (auto& x : d.values) raws += x.raw;
    CHECK(raws == 2, "%d channels raw (FUNcube-1 publishes a conversion for every other analogue channel)", raws);
    // the JSON: units, words, notes
    const std::string j = tlmJson(d, f.data(), f.size(), "AO-73", 1795600000.0);
    json::Value jv;
    CHECK(json::parseValue(j, jv) && jv.get("data") && jv.get("data")->str("layout") == "FUNcube-1" && jv.get("data")->get("absent")
          && jv.get("data")->get("absent")->a.empty(), "sat_tlm: layout FUNcube-1, absent []");
    CHECK(j.find("\"k\":\"eps.vbat\",\"name\":\"Battery voltage\",\"v\":7.876,\"unit\":\"V\"}") != std::string::npos
          && j.find("\"unit\":\"\xc2\xb0" "C\"") != std::string::npos && j.find("\"text\":\"Watchdog reset\"") != std::string::npos
          && j.find("\"k\":\"rf.rssi\",\"name\":\"Receiver RSSI\",\"v\":181,\"raw\":true,\"note\":") != std::string::npos,
          "the JSON carries 7.876 V, \"°C\", the reset cause's words and RSSI raw with its note");

    // ★★ a frame whose EPS says its data is NOT valid: no EPS values shown, said so
    v["veps"] = 0; v["vbat"] = 0; v["pv1"] = 0;
    std::vector<uint8_t> f2 = writeFrame(0x80, -1, fc1Layout(), v);
    Decoded e = parse(f2.data());
    const vibefc::Value* vb = e.find("eps.vbat");
    CHECK(e.absent.size() == 1 && e.absent[0] == "EPS" && vb && !vb->valid && !e.find("eps.pv1")->valid && e.find("bob.v33")->valid,
          "EPS data valid 0: the EPS values marked invalid, absent [EPS]; the ASIB's still valid");
    const std::string j2 = tlmJson(e, f2.data(), f2.size(), "AO-73", 1795600000.0);
    CHECK(j2.find("eps.vbat") == std::string::npos && j2.find("eps.reboots") == std::string::npos && j2.find("\"absent\":[\"EPS\"]") != std::string::npos
          && j2.find("bob.v33") != std::string::npos, "the JSON leaves every EPS value out and says \"absent\":[\"EPS\"]");
    CHECK(batteryText(e) == "no EPS data" && batteryText(d) == "battery 7.876 V", "the log line: \"battery 7.876 V\" / \"no EPS data\"");
    v["veps"] = 1; v["vanta"] = 0; v["vantb"] = 0; v["vpa"] = 0;
    Decoded g = parse(writeFrame(0x80, -1, fc1Layout(), v).data());
    CHECK(g.absent.size() == 2 && !g.find("ant.t0")->valid && !g.find("pa.fwd")->valid && g.find("eps.vbat")->valid,
          "PA and both ANTS buses not valid: those hidden, absent [PA, ANTS]; one ANTS bus valid is enough to show them");
}

static void s12() {
    std::printf("§12 ★★★ beta94 JY1Sat (JO-97): the extended id, its OWN layout (data budget 2F), the 11-25 pass\n");
    // synthetic: every JY1Sat field a known count
    std::map<std::string, uint32_t> v = {{"pv1", 4321}, {"pv2", 16383}, {"vbat", 8050}, {"ip1", 120}, {"ip2", 7}, {"ip3", 1023}, {"ipv", 250},
        {"isys", 180}, {"reboots", 42}, {"tb1", 5}, {"tbat", 0xFB}, {"lu5", 2}, {"i5", 33}, {"reset", 1}, {"ppt", 2},
        {"imode", 2}, {"ierr", 5}, {"icfg", 1}, {"itemp", 21}, {"sxp", 100}, {"sym", 1001}, {"v33", 825}, {"iup", 720}, {"v50", 833},
        {"dop", 160}, {"rssi", 200}, {"rft", 220}, {"irx", 50}, {"itx33", 60}, {"itx50", 70}, {"rev", 20}, {"fwd", 150}, {"pat", 203},
        {"pai", 100}, {"at0", 150}, {"at1", 150}, {"d0", 1}, {"d1", 1}, {"d2", 1}, {"d3", 1}, {"seq", 640952},
        {"vasib", 1}, {"veps", 1}, {"vpa", 1}, {"vrf", 1}, {"v5", 1}, {"vantb", 1}, {"vanta", 1}, {"eclipse", 1}};
    std::vector<uint8_t> f = writeFrame(0xC0 | 13, 0x10, jy1Layout(), v);
    Decoded d = parse(f.data());
    CHECK(d.satId == 3 && d.extId == kExtIdJy1Sat && d.layout == "JY1Sat" && d.frameName == "FM1" && d.seq == 640952 && d.mode == "Eclipse"
          && satIdText(d) == "JY1Sat (extended id 19)", "extended header 0xCD 0x10 = id 19 (JY1Sat), FM1, seq 640952, eclipse");
    CHECK(near(d, "eps.pv1", 4.321, 1e-9, "V") && near(d, "eps.pv2", 16.383, 1e-9, "V") && near(d, "eps.vbat", 8.050, 1e-9, "V")
          && near(d, "eps.ipv1", 120, 0, "mA") && near(d, "eps.ipv3", 1023, 0, "mA") && near(d, "eps.ipv", 250, 0, "mA") && near(d, "eps.isys", 180, 0, "mA"),
          "EPS: 14-bit millivolts (8050 = 8.050 V, 16383 the top), 10-bit milliamps, the three photo currents of its own");
    CHECK(d.find("eps.reboots")->v == 42 && near(d, "eps.tbat", -5, 0, kDeg) && near(d, "eps.i5sw1", 33, 0, "mA")
          && d.find("eps.reset")->text == "External reset" && d.find("eps.ppt")->text == "Fixed software PPT",
          "reboots 42 (8 bits), battery −5 °C, 5 V switch 33 mA, reset 1 = external, PPT 2 = fixed software (4 bits each)");
    CHECK(d.find("imtq.mode")->text == "Detumbling" && d.find("imtq.err")->raw && near(d, "imtq.temp", 21, 0, kDeg)
          && near(d, "imtq.up", 1.0, 1e-9, "h"), "iMTQ: mode 2 = detumbling, error code raw (\"not translated\"), MCU 21 °C, uptime 720 × 5 s = 1.0 h");
    CHECK(near(d, "asib.sunxp", 1096, 0.5, "lux") && near(d, "asib.sunym", 26303, 0.5, "lux") && near(d, "asib.sunxm", 0, 0, "lux")
          && near(d, "bob.v33", 3.3, 1e-9, "V") && near(d, "bob.v50", 4.998, 1e-9, "V"),
          "six sun sensors in lux, the 3.3 V bus 825 = 3.300 V and the 5 V bus 833 = 4.998 V — ASIB's own order (uptime between)");
    {   const std::string jj = tlmJson(d, f.data(), f.size(), "JO-97", 1795600000.0);
        json::Value jv; const bool okj = json::parseValue(jj, jv) && jv.get("data") && jv.get("data")->get("values");
        CHECK(d.values.size() == 63 && okj && jv.get("data")->get("values")->a.size() == 63 && jj.find("sw.depwait") != std::string::npos,
              "all 63 JY1Sat values reach the JSON, the last flag included");
    }
    CHECK(near(d, "rf.temp", -0.98 * 220 + 234.58, 1e-9, kDeg) && near(d, "rf.itx50", 0.636 * 70, 1e-9, "mA"),
          "RF: JY1Sat's own temperature line (−0.98 n + 234.58: 220 = 19.0 °C) and 0.636 mA a count on the 5 V transmitter");
    CHECK(near(d, "pa.temp", 10, 1e-9, kDeg) && near(d, "pa.fwd", 0.005 * std::pow(150.0, 2.0629), 1e-9, "mW"), "PA: \"Treat as FC1\" — 203 = 10 °C, forward 150 = 155 mW");
    v["itemp"] = 85;
    Decoded i85 = parse(writeFrame(0xC0 | 13, 0x10, jy1Layout(), v).data());
    CHECK(!i85.find("imtq.temp")->valid && i85.find("imtq.mode")->valid, "iMTQ MCU temperature 85 = invalid (the sheet) — hidden alone");
    // an extended id we do not know: no values guessed — unless the pick says JO-97
    Decoded unk = parse(writeFrame(0xC0 | 13, 0x24, jy1Layout(), v).data());
    Decoded hint = parse(writeFrame(0xC0 | 13, 0x24, jy1Layout(), v).data(), "JO-97");
    CHECK(unk.layout.empty() && unk.values.empty() && unk.seq < 0 && hint.layout == "JY1Sat" && hint.seq == 640952,
          "an unknown extended id: no values (no layout guessed); on the JO-97 pick: JY1Sat's");

    /* ★★★ THE 11-25 PASS (Stuart, the Pi 500, 09:53 UTC, 3 good frames). We have its values as the FUNcube-1 reading
     * printed them, not its bytes: the frame is rebuilt here from those printed values (fields not printed = 0; both ANTS
     * flags taken as 1), then read the right way. Read as FUNcube-1 it said "battery 0", "panel temp X− 92" (= 140 °C),
     * "photo voltage 1 4096" — the header's second byte. */
    std::map<std::string, uint32_t> air = {{"pv1", 4096}, {"reset", 27}, {"ppt", 85}, {"sunx", 1001}, {"suny", 67}, {"sunz", 403},
        {"tpxp", 993}, {"tpxm", 92}, {"tpyp", 475}, {"tpym", 511}, {"v33", 68}, {"i33", 273}, {"v50", 0}, {"dop", 187}, {"rssi", 255},
        {"rft", 241}, {"irx", 64}, {"itx33", 125}, {"itx50", 55}, {"rev", 57}, {"fwd", 95}, {"pat", 202}, {"pai", 255}, {"at0", 166},
        {"at1", 166}, {"d0", 1}, {"d1", 1}, {"d2", 1}, {"d3", 1}, {"seq", 640952}, {"dtmfn", 11}, {"vasib", 1}, {"veps", 0}, {"vpa", 1},
        {"vrf", 1}, {"v5", 0}, {"vantb", 1}, {"vanta", 1}, {"eclipse", 1}, {"safe", 0}};
    std::vector<uint8_t> fa = writeFrame(0xC0 | 5, -1, fc1Layout(), air);
    CHECK(fa[1] == 0x10 && fa[2] == 0x00, "rebuilt: its second byte is 0x10 (FUNcube-1's \"photo voltage 1 = 4096\")");
    Decoded a = parse(fa.data(), "JO-97");
    const std::string ja = tlmJson(a, fa.data(), fa.size(), "JO-97", 1795600000.0);
    CHECK(a.layout == "JY1Sat" && a.extId == kExtIdJy1Sat && a.seq == 640952 && a.mode == "Eclipse", "JY1Sat's layout, id 19, seq 640952, eclipse");
    CHECK(a.absent.size() == 2 && a.absent[0] == "EPS" && a.absent[1] == "iMTQ" && !a.find("eps.vbat")->valid
          && ja.find("eps.vbat") == std::string::npos && ja.find("\"absent\":[\"EPS\",\"iMTQ\"]") != std::string::npos,
          "EPS and iMTQ not valid: no battery shown at all (was \"battery 0 raw\"), said as absent");
    CHECK(a.find("imtq.temp") && a.find("imtq.temp")->v == 85 && !a.find("imtq.temp")->valid, "the iMTQ MCU temperature reads 85 — the sheet's own \"invalid\" (FUNcube-1's \"PPT mode 85\")");
    CHECK(near(a, "asib.sunxp", 26303, 0.5, "lux") && near(a, "asib.sunyp", std::pow(10.0, 3.58 + (403 - 400) / 67.0 * 0.11), 0.5, "lux")
          && near(a, "asib.sunym", 26303, 0.5, "lux") && near(a, "asib.sunzp", std::pow(10.0, 2.889 + 36.0 / 44.0 * 0.151), 0.5, "lux"),
          "what read as swinging \"panel temps\" are SUN SENSORS (X+ and Y− at the limit, Y+ 403, Z+ 92) — a spinning bird");
    CHECK(a.find("imtq.up") && !a.find("imtq.up")->valid && std::fabs(a.find("imtq.up")->v - 69905 * 5 / 3600.0) < 1e-9, "the iMTQ uptime (FUNcube-1's \"3.3 V bus\" voltage + current pair: 97 h) hidden with the iMTQ");
    CHECK(near(a, "rf.temp", -0.98 * 241 + 234.58, 1e-9, kDeg) && near(a, "rf.irx", 0.636 * 64, 1e-9, "mA") && near(a, "pa.fwd", 60.09, 0.01, "mW")
          && near(a, "pa.temp", 12.6 - 6.0 / 7.0 * 2.6, 1e-9, kDeg) && near(a, "ant.t0", -4.43, 0.05, kDeg),
          "the radio: RF −1.6 °C, receive 40.7 mA, PA forward 60 mW (the low-power beacon), PA 10.4 °C, antennas −4.4 °C (eclipse)");
    CHECK(a.find("rf.rssi")->raw && a.find("rf.doppler")->raw, "Doppler and RSSI raw — JY1Sat's sheet: \"no conversion required\"");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--quick") g_quick = true;
    s1(); s2(); s3(); s4(); s5(); s6(); s7(); s8(); s9(); s10(); s11(); s12();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
