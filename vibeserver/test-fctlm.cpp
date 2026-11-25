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
//      ★★ beta93: the same air moved as the 1500 Hz pick hears it (dial shifted in the test) — the frames hold.
//   §11 ★★★ beta94 (JO-97 0.9 kHz low, 11-25: the 1500 Hz pick's carrier at ~550 Hz, half below the dial, 0 good): frames
//      −2.0 … +2.0 kHz from the prediction through the 3000 Hz pick's whole path at Eb/N0 8 dB — every one decodes.
//   §12 ★★★ beta94 a NUDGE and the TRIM through TCA (55 Hz/s, the model 0.5 s late): nudge-then-trim is one correction,
//      the right way; trim-then-nudge (a 500 Hz step while locked) no longer leaves the loop on its half-baud false lock.
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
struct Pick { double centre, range, lo, hi; const char* name; };
static const Pick kOld{1200, 900, 0, 2650, "OLD 1200 Hz / ±900 Hz / 2.65 kHz USB"};
/** ★★★ beta94 the beta93 pick, kept to show what it did (JO-97 0.9 kHz low, 11-25: its carrier at ~550 Hz, 0 good). */
static const Pick kB93{1500, 2600, 100, 5000, "beta93 1500 Hz / +2600 −1200 Hz / 100–5000 Hz USB"};
/** ★★★ beta94 the pick's AUDIO (what the listener hears) with the decoder on it at ±2600 — the audio path a hand-attached
 *  decoder takes; a satellite pick's decoder reads IQ (§13, ±kTlmAfcHz). */
static const Pick kNew{vibe::sat::kTlmCentreHz, 2600, vibe::sat::kTlmAudioLoHz, vibe::sat::kTlmAudioHiHz, "NEW 3000 Hz / ±2600 Hz / 100–6500 Hz USB"};
/** A USB demodulator with a lo..hi passband: x (real, the "RF" at 48 kHz) × e^{−jφ_LO}, then only lo..hi kept (a complex
 *  band-pass: shift by −(lo+hi)/2, low-pass at (hi−lo)/2, shift back) — what the server's SsbDemod does. The LO's
 *  frequency is set per block (the dial + the trim): phase-continuous. ★★★ beta94 the low edge is modelled (it was
 *  0..bw): a telemetry pick's 100 Hz is part of what a low carrier loses. */
struct UsbChannel {
    double rate = 48000, mid = 1325, half = 1325, ph = 0, ph2 = 0;
    std::vector<float> h; vibebpsk::Ring r;
    UsbChannel(double loHz, double hiHz) : mid((loHz + hiHz) / 2), half((hiHz - loHz) / 2) { h = vibebpsk::lowpass(128, half, rate); r.init(h.size()); }
    void run(const float* x, float* y, int n, double loHz) {
        const double w = 2 * M_PI * loHz / rate, w2 = 2 * M_PI * mid / rate;
        for (int i = 0; i < n; ++i) {
            // down by the LO and by the passband's middle (to DC), low-pass, back up by the middle, the real part
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
    int good = 0, moves = 0, reversals = 0; uint64_t relocks = 0; double afc = 0, trim = 0, errEnd = 0, maxOver = 0, lastMv = 0; std::string log;
    std::vector<double> offs;   // the signal against the channel's centre, once a second
    std::vector<double> goodAt; // ★★★ beta94 when each good frame came (s)
};
static constexpr double kPredHz = 12000;   // the prediction, as an "RF" tone in the 48 kHz test signal
/** ★★★ beta94 more of a real pass (Stuart 11-25, JO-97): the listener's NUDGE of the dial (`nudgeHz` at `nudgeAtSec`, the
 *  lock kept — Engine::nudge moves the nominal), and the pass's whole DOPPLER — the signal carries −A·tanh((t − tca)/τ)
 *  (its steepest, A/τ Hz/s, at TCA) and the channel takes out the MODEL's, the same curve `modelLagSec` late (an
 *  element set / clock error: the residual is largest exactly at TCA), as the 25 Hz tracker does. */
struct PassOpts {
    double nudgeHz = 0, nudgeAtSec = 0;
    bool doppler = false; double tcaSec = 60, dopA = 3300, dopTau = 60, modelLagSec = 0.5;
    double dop(double t) const { return doppler ? -dopA * std::tanh((t - tcaSec) / dopTau) : 0.0; }
};
/** One pass: frames sent at kPredHz + errHz (+ driftHzS·t), heard through the pick `pk`. `follow`: the locked decoder
 *  steers the trim (the new pick's behaviour); otherwise the channel stays on the prediction. */
static PassResult pass(const Pick& pk, double errHz, double driftHzS, int nFrames, bool follow, double ebn0 = 14, uint32_t seed = 5,
                       const PassOpts& po = PassOpts{}) {
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < nFrames; ++i) fr.push_back(tx::sampleFrame(i % 24, 2000 + (uint32_t)i));
    tx::Cfg tc; tc.carrierHz = kPredHz; tc.offsetHz = errHz; tc.rampHzS = driftHzS; tc.ebn0Db = ebn0; tc.seed = seed;
    if (po.doppler) tc.hzAt = [po](double t) { return po.dop(t); };
    const std::vector<float> rf = tx::modulate(fr, tc);
    UsbChannel ch(pk.lo, pk.hi);
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
    double tNow = 0;
    o.record = [&](const Decoded&, const TlmCtx&) { res.goodAt.push_back(tNow); };
    Run run(rc, false, std::move(o));
    const int blk = 960;   // the server's 20 ms blocks
    const int lagBlk = 15; // ★ the trim reaches the audio 0.3 s after the engine applies it (the pipeline)
    std::deque<double> hist(lagBlk + 1, 0.0);
    std::vector<float> a(blk);
    for (size_t i = 0; i < rf.size(); i += blk) {
        const int n = (int)std::min<size_t>(blk, rf.size() - i);
        tm.tick(n / 48000.0);
        hist.push_back(tm.applied); hist.pop_front();
        const double t0 = (double)i / 48000.0;
        const double nudge = (po.nudgeHz != 0 && t0 >= po.nudgeAtSec) ? po.nudgeHz : 0.0;
        // the dial (downlink − centre, + a nudge) + the model's Doppler (25 Hz ticks: per 20 ms block) + the trim (late)
        const double lo = kPredHz - pk.centre + nudge + po.dop(t0 - po.modelLagSec) + hist.front();
        ch.run(rf.data() + i, a.data(), n, lo);
        tNow = (double)(i + n) / 48000.0;
        const uint64_t before = run.rx().samples() / 48000;
        run.feedAudio(a.data(), n);
        if (run.rx().samples() / 48000 != before) {
            const double t = (double)(i + n) / 48000.0;
            const double nudge = (po.nudgeHz != 0 && t >= po.nudgeAtSec) ? po.nudgeHz : 0.0;
            const double truth = errHz + driftHzS * t + po.dop(t) - po.dop(t - po.modelLagSec) - nudge;   // against the nominal
            res.offs.push_back(truth - hist.front());           // …against the channel's centre
            if (follow) res.maxOver = std::max(res.maxOver, (errHz >= 0 ? 1 : -1) * (hist.front() - truth));
        }
    }
    res.good = (int)run.framer().good(); res.afc = run.rx().afcHz(); res.relocks = run.rx().relocks(); res.trim = tm.applied;
    const double tEnd = rf.size() / 48000.0;
    res.errEnd = errHz + driftHzS * tEnd - ((po.nudgeHz != 0 && tEnd >= po.nudgeAtSec) ? po.nudgeHz : 0.0) - tm.applied;
    return res;
}

static void s8() {
    std::printf("§8 ★★★ the wide search and the drift follow (beta92 — AO-73 1.3 kHz above the prediction)\n");
    const int nf = g_quick ? 12 : 24;
    struct Case { double err, drift; const char* what; };
    /* ★★ beta93 the carrier at 1500 Hz (Stuart: 3800 "sounds thin and high-pitched"): below the centre the hunt now reaches
     *  a 300 Hz carrier (−1200) — −2.0 kHz (beta92's case) would be below 0 Hz in USB audio. −0.8 kHz: a 700 Hz carrier,
     *  its mirror at −700 Hz, never taken (the trim converges to −800, not to the mirror). The 11-24 pass was −0.6.
     *  ★★★ beta94 the carrier at 3000 Hz (JO-97 −0.9 kHz on 11-25): −0.8 kHz is now a 2.2 kHz carrier; §11 sweeps ±2 kHz. */
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
    {   // ★★ ON the prediction: the carrier at the centre (3000 Hz since beta94) in the audio, the AFC at ~0 from the start
        PassResult z = pass(kNew, 0, 0, nf, false);
        CHECK(z.good >= nf - 1 && std::fabs(z.afc) < 40, "on the prediction, the NEW pick: %d / %d frames, the carrier at %.0f Hz audio (AFC %+.0f)",
              z.good, nf, kNew.centre + z.afc, z.afc);
        PassResult b = pass(kNew, -2000, 0, nf, false);
        CHECK(b.good >= nf - 2, "−2.0 kHz (a 1000 Hz carrier, its lower half still above the dial): %d / %d frames (AFC %+.0f)", b.good, nf, b.afc);
        vibebpsk::Rx::Cfg rc; rc.centreHz = kNew.centre; rc.afcRangeHz = kNew.range;
        vibebpsk::Rx rx(rc);
        CHECK(rx.afcLoHz() == -2600 && rx.afcHiHz() == 2600, "the NEW pick's search window: %+.0f … %+.0f Hz (a carrier from 400 to 5600 Hz)", rx.afcLoHz(), rx.afcHiHz());
        CHECK(kNew.centre - 2000 - 900 >= kNew.lo && kNew.centre + kNew.range + 900 <= kNew.hi,
              "★★★ beta94 a ±2 kHz error keeps the whole signal (carrier ±0.9 kHz) in %.0f–%.0f Hz, and the hunt's top too", kNew.lo, kNew.hi);
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
        PassResult n0 = pass(Pick{1200, 900, 0, 7500, "1200 / ±900 / 7.5 kHz"}, 1300, 0, nf, false);
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
    vibebpsk::Rx::Cfg c; c.centreHz = vibe::sat::kTlmCentreHz; c.afcRangeHz = kNew.range;
    int frames = 0, asks = 0;
    Run::Out o{ [&](const std::string& j) { if (j.find("sat_tlm") != std::string::npos) ++frames; }, nullptr, nullptr, nullptr };
    o.trimNow = [](double& a, double& t) { a = 0; t = 0; return true; };
    o.trimTo = [&](double) { ++asks; };
    Run run(c, false, std::move(o));
    const int secs = g_quick ? 120 : 600;
    UsbChannel ch(kNew.lo, kNew.hi);
    std::vector<float> w(960), a(960);
    for (int s = 0; s < secs * 50; ++s) {
        for (auto& v : w) v = float(0.1 * g(rng));
        ch.run(w.data(), a.data(), 960, 9000);
        run.feedAudio(a.data(), 960);
    }
    CHECK(frames == 0 && asks == 0, "%d s of white noise through the 7.5 kHz passband and the ±2600 Hz hunt: %d frames, %d trim requests (%llu syncs refused)",
          secs, frames, asks, (unsigned long long)run.framer().syncs());
    vibebpsk::Rx rx(c);
    CHECK(rx.afcRangeHz() == kNew.range, "the wide receiver's range: %.0f Hz", rx.afcRangeHz());
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
    vibebpsk::Rx::Cfg c; c.centreHz = vibe::sat::kTlmCentreHz; c.afcRangeHz = kNew.range;
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
    /* ★★ THE SAME AIR, AS THE NEW PICK HEARS IT. The recording was made with the dial 3800 Hz below the downlink; the
     * pick now sits kTlmCentreHz (3000, beta94) below, i.e. 800 Hz higher — the same as moving every audio frequency
     * 800 Hz down (only what was above the 100 Hz edge kept). Then the carrier lands where THAT pass's error puts it
     * (~2.4 kHz: the bird was ~0.6 kHz below the prediction), and, moved 200 Hz, where a pass ON the prediction would.
     * ★★★ beta94 and moved 2200 Hz: the carrier at ~1.0 kHz, as a bird 2.0 kHz low puts it on the new pick — and, for
     * the record, moved 2650 Hz as the beta93 1500 Hz pick heard JO-97 0.9 kHz low on 11-25 (carrier ~550 Hz). */
    struct Sh { double hz; const char* what; double afcLo, afcHi; double centre; bool must; };
    for (const Sh& sh : {Sh{800, "as the 3000 Hz pick would have heard this pass (carrier ~2.4 kHz)", -700, -500, vibe::sat::kTlmCentreHz, true},
                         Sh{200, "the carrier moved to ~3.0 kHz (a pass ON the prediction)", -100, 100, vibe::sat::kTlmCentreHz, true},
                         Sh{2200, "★★★ the carrier moved to ~1.0 kHz (a bird 2.0 kHz low, on the 3000 Hz pick)", -2100, -1900, vibe::sat::kTlmCentreHz, true},
                         Sh{2650, "(before) the beta93 1500 Hz pick, the carrier at ~550 Hz (JO-97 0.9 kHz low, 11-25)", -1050, -850, 1500, false}}) {
        const std::vector<float> b = shiftDown(a, 48000, sh.hz, sh.hz + 100, 7400);
        std::vector<Decoded> g2;
        Run::Out o2{ nullptr, nullptr, nullptr, nullptr };
        o2.record = [&](const Decoded& d, const TlmCtx&) { g2.push_back(d); };
        vibebpsk::Rx::Cfg c2 = c; c2.centreHz = sh.centre;
        Run r2(c2, false, std::move(o2));
        double afcAtFirst = NAN;
        for (size_t i = 0; i + 960 <= b.size(); i += 960) { r2.feedAudio(&b[i], 960); if (std::isnan(afcAtFirst) && !g2.empty()) afcAtFirst = r2.rx().afcHz(); }
        std::string n2; for (auto& d : g2) n2 += d.frameName + " ";
        if (!sh.must) { std::printf("     %s: %zu frames (%s), %llu syncs, AFC %+.0f Hz\n", sh.what, g2.size(), n2.c_str(),
                                    (unsigned long long)r2.framer().syncs(), r2.rx().afcHz()); continue; }
        /* ★★★ beta94 at a ~1.0 kHz carrier the hunt takes one frame longer on this 23 s cut (FM8 is missed; 0.9 and
         *  1.1 kHz give all four) — 3 frames, every one RS-checked, none failed: the signal is whole, not cut. */
        const size_t need = sh.hz >= 2000 ? 3 : 4;
        CHECK(g2.size() >= need && g2.back().frameName == "WO1" && r2.framer().failed() == 0 && afcAtFirst > sh.afcLo && afcAtFirst < sh.afcHi,
              "%s: %zu frames (%s), %llu failed, AFC %+.0f Hz at the first", sh.what, g2.size(), n2.c_str(),
              (unsigned long long)r2.framer().failed(), afcAtFirst);
    }
}

static void s11() {
    std::printf("§11 ★★★ beta94 ±2 kHz FROM THE PREDICTION (JO-97 0.9 kHz low, 11-25: the beta93 pick's carrier at ~550 Hz, 0 good)\n");
    /* Frames at −2.0 … +2.0 kHz from the prediction through the pick's whole path (dial, USB lo..hi, decoder, follow) at
     * Eb/N0 8 dB — a modest pass, not a strong one (at 14 dB the beta93 pick still decoded −0.9). */
    const int nf = g_quick ? 8 : 12;
    const double eb = 8;
    const double errs[] = {-2000, -1500, -900, 0, 900, 1500, 2000};
    std::string row = "     beta93 (1500 Hz, 100–5000):";
    for (double e : errs) {
        PassResult r = pass(kNew, e, 0, nf, true, eb, 11);
        CHECK(r.good >= nf - 1, "%+5.0f Hz (carrier at %4.0f Hz audio) on the NEW pick, following: %d / %d frames (AFC %+.0f, trim %+.0f)",
              e, kNew.centre + e, r.good, nf, r.afc, r.trim);
        if (e == 0) CHECK(std::fabs(r.afc) < 40 && r.moves == 0, "  exactly predicted: the decoder locks ON its centre (AFC %+.0f Hz), the trim never moves", r.afc);
        if (!g_quick) { PassResult b = pass(kB93, e, 0, nf, true, eb, 11); char t[40]; std::snprintf(t, sizeof t, " %+.0f:%d", e, b.good); row += t; }
    }
    if (!g_quick) std::printf("%s (of %d)\n", row.c_str(), nf);
}

static void s12() {
    std::printf("§12 ★★★ beta94 A NUDGE AND THE TRIM, THROUGH TCA (JO-97 11-25: nudged 500 Hz down, 3 frames, trim −518, then nothing)\n");
    /* The bird 950 Hz below the prediction; the listener tunes the dial 500 Hz DOWN (the lock kept, its nominal moved);
     * the locked decoder's follow (vibefc::TrimFollow) trims the rest. All of it through the pass's steepest Doppler —
     * TCA at 60 s, 3.3 kHz × tanh(t/60 s): 55 Hz/s at TCA (an overhead LEO pass at 145 MHz), the model 0.5 s late (a
     * ~28 Hz residual at TCA). Two orders:
     *  (a) Stuart's: the nudge FIRST (1 s in, before any frame), then the trim — measured against the NUDGED channel, so
     *      one correction the right way: the trim ends at err − nudge = −450 Hz, the signal at the decoder's centre.
     *  (b) the trim first, then a nudge at 15 s: a 500 Hz STEP while locked. Before beta94 the Costas loop slipped to its
     *      half-baud false lock (frames still came — the differential decode survives it — but the reported offset was
     *      600 Hz wrong, so the follow left the signal 500 Hz off for the rest of the pass). The lock check
     *      (vibebpsk::Rx, hunt(check)) moves it back; the follow then trims the nudge out again (one reversal: the
     *      listener moved it away from the signal). */
    const int nf = 24;   // 120 s, TCA in the middle
    for (double eb : {14.0, 8.0}) {
        PassOpts pa; pa.nudgeHz = -500; pa.nudgeAtSec = 1; pa.doppler = true; pa.tcaSec = 60;
        PassResult f = pass(kNew, -950, 0, nf, true, eb, 17, pa);
        int afterTca = 0; for (double t : f.goodAt) if (t > pa.tcaSec) ++afterTca;
        CHECK(f.good >= nf - 1 && afterTca >= nf / 2 - 1, "(a) Eb/N0 %.0f, nudge then trim, NEW pick: %d / %d frames, %d after TCA", eb, f.good, nf, afterTca);
        CHECK(std::fabs(f.trim - (-450)) < 60 && std::fabs(f.errEnd) < 60 && f.moves == 1 && f.reversals == 0,
              "  the trim %+.0f Hz (err − nudge = −450): one correction, the right way — the signal ends %+.0f Hz from the centre (%d move)",
              f.trim, f.errEnd, f.moves);
        double worst = 0; for (size_t i = f.offs.size() / 3; i < f.offs.size(); ++i) worst = std::max(worst, std::fabs(f.offs[i]));
        CHECK(worst < 60, "  through TCA (55 Hz/s) the signal stays within %.0f Hz of the centre — the trim and the Doppler never fight", worst);
        if (!f.log.empty()) std::printf("     log: %s\n", f.log.c_str());
        PassResult b = pass(kB93, -950, 0, nf, true, eb, 17, pa);
        int bTca = 0; for (double t : b.goodAt) if (t > pa.tcaSec) ++bTca;
        std::printf("     (beta93 1500 Hz pick, the same: %d / %d frames, %d after TCA; trim %+.0f, ends %+.0f Hz from its centre)\n",
                    b.good, nf, bTca, b.trim, b.errEnd);
        PassOpts pb = pa; pb.nudgeAtSec = 15;
        PassResult g = pass(kNew, -950, 0, nf, true, eb, 17, pb);
        int gTca = 0; for (double t : g.goodAt) if (t > pb.tcaSec) ++gTca;
        CHECK(g.good >= nf - 1 && gTca >= nf / 2 - 1 && g.relocks >= 1, "(b) Eb/N0 %.0f, trim then a nudge: %d / %d frames, %d after TCA, the lock check moved the loop %llu time(s)",
              eb, g.good, nf, gTca, (unsigned long long)g.relocks);
        CHECK(std::fabs(g.trim - (-450)) < 60 && std::fabs(g.errEnd) < 60 && g.reversals <= 1,
              "  it ends at the centre (%+.0f Hz), the trim %+.0f (%d moves, %d reversal)", g.errEnd, g.trim, g.moves, g.reversals);
    }
}

// ── §13 ★★★ beta94 ON IQ ─────────────────────────────────────────────────────────────────────────────────────────
/** One pass on the IQ input, as DecoderHost feeds it: a slice at kIqRate centred `sliceOffHz` from the aim (the shim cuts
 *  it in whole bins near the aim), the aim = the dial + the model's Doppler + the trim (late), the carrier kTlmCentreHz
 *  above the aim plus `errHz` (+ the pass's Doppler, the model's taken out). */
static constexpr double kIqRate = 24576;   // a 24 kHz slice's rate off a 2.4 MS/s capture (not a multiple of 9.6 k)
static PassResult iqPass(double errHz, int nFrames, bool follow, double ebn0, uint32_t seed, const PassOpts& po = PassOpts{},
                         double sliceOffHz = 1234) {
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < nFrames; ++i) fr.push_back(tx::sampleFrame(i % 24, 3000 + (uint32_t)i));
    std::vector<std::complex<float>> iq;
    tx::Cfg tc; tc.rate = kIqRate; tc.carrierHz = vibe::sat::kTlmCentreHz - sliceOffHz; tc.offsetHz = errHz; tc.ebn0Db = ebn0; tc.seed = seed;
    tc.iqOut = &iq;
    if (po.doppler) tc.hzAt = [po](double t) { return po.dop(t); };
    (void)tx::modulate(fr, tc);
    TrimModel tm; PassResult res;
    vibebpsk::Rx::Cfg rc; rc.centreHz = vibe::sat::kTlmCentreHz; rc.afcRangeHz = vibe::sat::kTlmAfcHz;
    Run::Out o{};
    if (follow) {
        o.trimNow = [&](double& a, double& t) { a = tm.applied; t = tm.target; return true; };
        o.trimTo = [&](double t) {
            const double was = tm.target; tm.ask(t); const double mv = tm.target - was;
            if (res.lastMv != 0 && mv != 0 && (mv > 0) != (res.lastMv > 0)) ++res.reversals;
            if (mv != 0) res.lastMv = mv;
            ++res.moves;
        };
    }
    double tNow = 0;
    o.record = [&](const Decoded&, const TlmCtx&) { res.goodAt.push_back(tNow); };
    Run run(rc, false, std::move(o));
    const double slice = 145.9e6;            // the slice's centre (RF)
    const int blk = 512;
    std::deque<double> hist(16, 0.0);
    double ph = 0;                           // the channel's own move (aim − its start) applied to the samples
    std::vector<std::complex<float>> b(blk);
    for (size_t i = 0; i < iq.size(); i += blk) {
        const int n = (int)std::min<size_t>(blk, iq.size() - i);
        tm.tick(n / kIqRate);
        hist.push_back(tm.applied); hist.pop_front();
        const double t0 = (double)i / kIqRate;
        const double nudge = (po.nudgeHz != 0 && t0 >= po.nudgeAtSec) ? po.nudgeHz : 0.0;
        // the slice is FIXED here; the aim (where the channel is) moves with the model, the trim and a nudge
        const double aimMove = nudge + po.dop(t0 - po.modelLagSec) + hist.front();
        const double aim = slice - sliceOffHz + aimMove;
        for (int k = 0; k < n; ++k) b[(size_t)k] = iq[i + (size_t)k];
        (void)ph;
        tNow = (double)(i + n) / kIqRate;
        run.feedIq(b.data(), n, kIqRate, slice, aim, vibe::sat::kTlmCentreHz);
    }
    res.good = (int)run.framer().good(); res.afc = run.rx().afcHz(); res.trim = tm.applied; res.relocks = run.rx().relocks();
    const double tEnd = iq.size() / kIqRate;
    res.errEnd = errHz + (po.dop(tEnd) - po.dop(tEnd - po.modelLagSec)) - ((po.nudgeHz != 0 && tEnd >= po.nudgeAtSec) ? po.nudgeHz : 0.0) - tm.applied;
    return res;
}

static void s13() {
    std::printf("§13 ★★★ beta94 THE DECODER ON IQ (Stuart 11-25: \"or again use iq\") — no USB edge, the hunt ±%.0f Hz\n", vibe::sat::kTlmAfcHz);
    const int nf = g_quick ? 8 : 12;
    for (double e : {-3500.0, -2500.0, -1500.0, -900.0, 0.0, 900.0, 1500.0, 2500.0, 3500.0}) {
        PassResult r = iqPass(e, nf, true, 8, 21);
        CHECK(r.good >= nf - 1, "%+5.0f Hz from the prediction, IQ, Eb/N0 8, following: %d / %d frames (AFC %+.0f, trim %+.0f)", e, r.good, nf, r.afc, r.trim);
        if (e == 0) CHECK(std::fabs(r.afc) < 40 && r.moves == 0, "  exactly predicted: locked ON the prediction (AFC %+.0f Hz), the trim never moves", r.afc);
        if (e == -900) CHECK(std::fabs(r.trim - e) < 120 && std::fabs(r.errEnd) < 120, "  JO-97's −0.9 kHz: the trim %+.0f, the signal ends %+.0f Hz from the prediction", r.trim, r.errEnd);
    }
    {   // the nudge and the trim through TCA, on IQ
        PassOpts pa; pa.nudgeHz = -500; pa.nudgeAtSec = 1; pa.doppler = true; pa.tcaSec = 60;
        PassResult f = iqPass(-950, 24, true, 8, 23, pa);
        int afterTca = 0; for (double t : f.goodAt) if (t > pa.tcaSec) ++afterTca;
        CHECK(f.good >= 22 && afterTca >= 11 && std::fabs(f.trim + 450) < 60 && std::fabs(f.errEnd) < 60 && f.reversals == 0,
              "a nudge −500 then the trim, through TCA (55 Hz/s): %d / 24 frames, %d after TCA, trim %+.0f (−450), ends %+.0f Hz", f.good, afterTca, f.trim, f.errEnd);
        pa.nudgeAtSec = 15;
        PassResult g = iqPass(-950, 24, true, 8, 23, pa);
        CHECK(g.good >= 22 && std::fabs(g.errEnd) < 60, "the trim then a nudge: %d / 24 frames, ends %+.0f Hz (%llu relock)", g.good, g.errEnd, (unsigned long long)g.relocks);
    }
    {   // noise only: no frame, no trim
        std::mt19937 rng(31); std::normal_distribution<double> g(0, 1);
        vibebpsk::Rx::Cfg c; c.centreHz = vibe::sat::kTlmCentreHz; c.afcRangeHz = vibe::sat::kTlmAfcHz;
        int frames = 0, asks = 0;
        Run::Out o{ [&](const std::string& j) { if (j.find("sat_tlm") != std::string::npos) ++frames; }, nullptr, nullptr, nullptr };
        o.trimNow = [](double& a, double& t) { a = 0; t = 0; return true; };
        o.trimTo = [&](double) { ++asks; };
        Run run(c, false, std::move(o));
        const int secs = g_quick ? 120 : 600;
        std::vector<std::complex<float>> w(512);
        for (long k = 0; k < (long)(secs * kIqRate / 512); ++k) {
            for (auto& v : w) v = std::complex<float>(float(0.1 * g(rng)), float(0.1 * g(rng)));
            run.feedIq(w.data(), 512, kIqRate, 145.9e6, 145.9e6 - 1234, vibe::sat::kTlmCentreHz);
        }
        CHECK(frames == 0 && asks == 0 && run.iq(), "%d s of IQ noise, the ±%.0f Hz hunt: %d frames, %d trim requests", secs, vibe::sat::kTlmAfcHz, frames, asks);
    }
    {   // the real AO-73 audio as IQ: its analytic signal (100–7400 Hz) is the slice a USB dial 3800 Hz below would cut
        std::vector<float> a; int rate = 0;
        const char* fn = std::getenv("VIBE_FCTLM_REAL"); if (!fn) fn = "test/fixtures/funcube/ao73-v4l-2026-11-24-23s.wav";
        if (readWav16(fn, a, rate) && rate == 48000) {
            // analytic: the band 100–7400 Hz as complex (a Hilbert band-pass, the same taps shiftDown uses), at 48 kHz
            const int nt = 256; const std::vector<float> h = vibebpsk::lowpass(nt, 3650, 48000);
            std::vector<std::complex<float>> z(a.size());
            for (size_t k = 0; k < a.size(); ++k) {
                double zr = 0, zi = 0;
                for (int i = 0; i < nt; ++i) {
                    const long j = (long)k - i + nt / 2; if (j < 0 || j >= (long)a.size()) continue;
                    const double ang = 2 * M_PI * 3750.0 / 48000 * (i - (nt - 1) / 2.0);
                    zr += a[(size_t)j] * h[(size_t)i] * std::cos(ang); zi += a[(size_t)j] * h[(size_t)i] * std::sin(ang);
                }
                z[k] = std::complex<float>(float(2 * zr), float(2 * zi));
            }
            // the dial was 3800 below the downlink; the slice centred on the dial (aim = slice): the carrier ~3.2 kHz up
            vibebpsk::Rx::Cfg c; c.centreHz = 3800; c.afcRangeHz = vibe::sat::kTlmAfcHz;
            std::vector<Decoded> got; Run::Out o{ nullptr, nullptr, nullptr, nullptr };
            o.record = [&](const Decoded& d, const TlmCtx&) { got.push_back(d); };
            Run run(c, false, std::move(o));
            for (size_t k = 0; k + 960 <= z.size(); k += 960) run.feedIq(&z[k], 960, 48000, 145.9e6, 145.9e6, 3800);
            std::string names; for (auto& d : got) names += d.frameName + " ";
            CHECK(got.size() >= 4 && run.framer().failed() == 0, "the real AO-73 pass as IQ: %zu frames (%s), %llu failed, AFC %+.0f Hz, %llu relocks, %llu syncs",
                  got.size(), names.c_str(), (unsigned long long)run.framer().failed(), run.rx().afcHz(), (unsigned long long)run.rx().relocks(), (unsigned long long)run.framer().syncs());
        }
    }
}

/** ★★★ beta94 `test-fctlm --write-clip FILE RATE CENTRE_HZ TLM_HZ`: 10.0 s of unsigned 8-bit IQ for the fake rtl_tcp (--hf
 *  FILE --hf-centre CENTRE_HZ) — FUNcube BPSK 1200 at TLM_HZ (two frames, WO1 and WO2, seq 777; exactly 12 000 symbols at
 *  1200 Bd and whole carrier cycles, so the loop is seamless), on a little noise. What scripts/test-server-fctlm-iq.mjs
 *  feeds the real server. */
static int writeClip(const char* path, double rate, double centre, double tlmHz) {
    std::vector<std::vector<uint8_t>> fr = { tx::sampleFrame(0, 777), tx::sampleFrame(1, 777) };
    std::vector<std::complex<float>> z;
    tx::Cfg c; c.rate = rate; c.carrierHz = tlmHz - centre; c.amp = 0.05; c.phase = 0; c.iqOut = &z;
    (void)tx::modulate(fr, c, 0.0, 0.0);
    const size_t n = (size_t)std::llround(10.0 * rate);
    if (z.size() < n) return 3;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::mt19937 rng(9); std::normal_distribution<float> nd(0.0f, 0.012f);
    std::vector<uint8_t> out; out.reserve(1 << 20);
    for (size_t i = 0; i < n; ++i) {
        const float re = z[i].real() + nd(rng), im = z[i].imag() + nd(rng);
        out.push_back((uint8_t)std::max(0.0f, std::min(255.0f, std::round(127.5f + 127.0f * re))));
        out.push_back((uint8_t)std::max(0.0f, std::min(255.0f, std::round(127.5f + 127.0f * im))));
        if (out.size() >= (1 << 20)) { std::fwrite(out.data(), 1, out.size(), f); out.clear(); }
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]));
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--quick") g_quick = true;
    s1(); s2(); s3(); s4(); s5(); s6(); s7(); s8(); s9(); s10(); s11(); s12(); s13();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
