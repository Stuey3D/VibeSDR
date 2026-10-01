// test-r82xx-if.cpp — the R820T's IF, and the +1.945 MHz a torn update of it puts on the whole dial.
//
// ★★★ Kiko, 2026-09-30 (Moto G 2014, Android 5.1, RTL-SDR Blog V3, Lite 11 b6): every station a
//     CONSTANT +1.950 MHz from the dial. This pins the arithmetic that fingerprint points at
//     (vibe_r82xx_if.h) and replays, step by step, the interleaving of librtlsdr's two IF writers —
//     the IF filter (r820t_set_bw, on the hardware-writer thread) and rtlsdr_set_direct_sampling(0)
//     (r82xx_init, sent by Android on every start from the Kotlin thread) — that leaves the tuner's
//     int_freq and the RTL2832's IF register disagreeing. The fix serialises the second behind the
//     first's lock (devMtx) and stops sending the no-op re-init at all; the last case below is that
//     order, and it must read 0.
// ★ int32_t throughout, deliberately: librtlsdr's IF arithmetic is 32-bit on every ABI, and so is
//   armeabi-v7a's `long` — nothing here may depend on a 64-bit host to come out right.
// ★★ ROUTE #2 (second half of this file): the hardware writer's AUTOMATIC direct-sampling crossover.
//    Going back to the tuner re-runs r82xx_init, which wipes the IF filter and the gain; the automatic
//    route used to put the filter back only when a width had been asked for, and never the gain. Both
//    routes now call vibertl::restoreTunerAfterReinit (vibe_rtl_tuner_restore.h) — and so does this
//    test, against a model of osmocom rtl-sdr 2.0.3's direct-sampling, filter, rate and gain calls.
#include "vibe_r82xx_if.h"
#include "vibe_rtl_tuner_restore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vibertl;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

/** librtlsdr's state for one dongle, reduced to what decides where the dial lands. */
struct FakeRtl {
    int32_t intFreq = kR82xxInitIfHz;   // tuner: priv->int_freq
    int32_t demodIf = kR82xxInitIfHz;   // RTL2832: the IF register
    uint32_t freq = 0;                  // dev->freq
    uint32_t lo = 0;                    // what the PLL was last programmed to
    void setCenter(uint32_t f) { lo = f + (uint32_t)intFreq; freq = f; }       // r82xx_set_freq
    /** Where a station heard at the DDC's 0 Hz really is, minus the dial. */
    int32_t dialError() const { return (int32_t)(lo - (uint32_t)demodIf) - (int32_t)freq; }
};

/** r820t_set_bw(), in its three steps. */
struct SetBw {
    FakeRtl& d; int32_t bw; int32_t r = 0;
    void a() { r = r82xxIntFreqForBw(bw); d.intFreq = r; }   // r82xx_set_bandwidth
    void b() { d.demodIf = r; }                                // rtlsdr_set_if_freq(dev, r)
    void c() { d.setCenter(d.freq); }                          // rtlsdr_set_center_freq(dev, dev->freq)
};
/** rtlsdr_set_direct_sampling(dev, 0), in its three steps. */
struct DsOff {
    FakeRtl& d;
    void a() { d.intFreq = kR82xxInitIfHz; }   // tuner->init → r82xx_init
    void b() { d.demodIf = kR82xxInitIfHz; }   // rtlsdr_set_if_freq(dev, R82XX_IF_FREQ)
    void c() { d.setCenter(d.freq); }          // rtlsdr_set_center_freq(dev, dev->freq)
};

// ═══════════════════════════ ROUTE #2 — the direct-sampling switch back to the tuner ═══════════════════════════

/** osmocom rtl-sdr 2.0.3 for one R820T dongle: the calls the server makes around direct sampling, each as
 *  librtlsdr performs it (src/librtlsdr.c, src/tuner_r82xx.c). Every call is logged, so a case can assert
 *  that NOTHING was sent. Also the Ops the shared restore function drives. */
struct Dongle {
    static constexpr int kInitFilter = -1;   // the filter r82xx_init calibrates (its 6 MHz default)
    int ds = 0;                  // dev->direct_sampling
    uint32_t rate = 2048000;     // dev->rate
    uint32_t devBw = 0;          // dev->bw — librtlsdr's BELIEF about the filter; init does NOT reset it
    uint32_t freq = 0;           // dev->freq
    int32_t intFreq = kR82xxInitIfHz, demodIf = kR82xxInitIfHz;
    uint32_t lo = 0;
    int filter = kInitFilter;    // what the tuner's filter really is (Hz), or init's default
    bool gainManual = false;     // r82xx_init_array: LNA + mixer on the tuner's own AGC
    int gain = -1;               // the manual gain written, tenths of a dB
    std::vector<std::string> calls;

    void centre(uint32_t f) {    // rtlsdr_set_center_freq
        freq = f;
        if (ds) { demodIf = (int32_t)f; lo = 0; } else lo = f + (uint32_t)intFreq;
    }
    int setCenter(uint32_t f) { calls.push_back("centre"); centre(f); return 0; }
    int setTunerBandwidth(uint32_t bw) {          // rtlsdr_set_tuner_bandwidth → r820t_set_bw
        calls.push_back("bw");
        const uint32_t b = bw > 0 ? bw : rate;
        intFreq = r82xxIntFreqForBw((int32_t)b); filter = (int)b;
        demodIf = intFreq; devBw = bw; centre(freq);
        return 0;
    }
    int setTunerGainMode(int manual) { calls.push_back("gainmode"); gainManual = manual != 0; return 0; }
    int setTunerGain(int tenth)      { calls.push_back("gain"); gain = tenth; return 0; }
    int setSampleRate(uint32_t r) {               // rtlsdr_set_sample_rate: re-derives from dev->bw
        calls.push_back("rate"); rate = r;
        const uint32_t b = devBw > 0 ? devBw : rate;
        intFreq = r82xxIntFreqForBw((int32_t)b); filter = (int)b; demodIf = intFreq; centre(freq);
        return 0;
    }
    int setDirectSampling(int on) {               // rtlsdr_set_direct_sampling — NO "already" check
        calls.push_back("ds" + std::to_string(on));
        if (on) { ds = on; }                      // tuner->exit (standby) + ADC routing
        else {                                    // tuner->init = r82xx_init, then the IF register
            intFreq = kR82xxInitIfHz; filter = kInitFilter; gainManual = false; gain = -1;
            demodIf = kR82xxInitIfHz; ds = 0;
        }
        centre(freq);
        return 0;
    }
    int getDirectSampling() const { return ds; }
    int32_t dialError() const { return (int32_t)(lo - (uint32_t)demodIf) - (int32_t)freq; }
};

/** The server's side, mirroring local_sdr_shim.cpp: the open, the hardware writer's tune with its
 *  automatic crossover, a VibeAGC step, and setDirectSampling. The decisions are the SHARED functions. */
struct Server {
    Dongle d;
    int tunerBwHz = 0;           // g_tunerBwHz — 0 = no width asked for
    double sampleRate = 2048000;
    int lastGainTenth = -1, hwGainNow = -1;
    bool autoDs = false; double belowHz = 24e6;
    int dsNow = -1;              // g_dsNow
    bool fixed = true;           // false = the automatic route as it was before this fix
    std::vector<std::string> log;

    void open(uint32_t hz, int gainTenth) {       // start(): rate, filter, tune, manual gain
        d.setSampleRate((uint32_t)sampleRate);
        d.setTunerBandwidth(tunerBwHz > 0 ? (uint32_t)tunerBwHz : (uint32_t)sampleRate);
        d.setCenter(hz);
        d.setTunerGainMode(1);
        lastGainTenth = gainTenth; d.setTunerGain(gainTenth); hwGainNow = gainTenth;
        d.calls.clear();
    }
    void agcStep(int tenth) { d.setTunerGain(tenth); hwGainNow = tenth; }   // queueHwGain → the writer
    void restore(const char* route) {
        const TunerRestorePlan plan = tunerRestorePlan(tunerBwHz, sampleRate, hwGainNow, lastGainTenth);
        restoreTunerAfterReinit(d, plan);
        if (plan.gainTenth >= 0) hwGainNow = plan.gainTenth;
        log.push_back(std::string(route) + " restored " + std::to_string(plan.bwHz) + " " + std::to_string(plan.gainTenth));
    }
    void writerTune(uint32_t hz) {                // startHwWriter's `if (hz)` block
        bool restoreTuner = false;
        if (autoDs) {
            const int want = ((double)hz < belowHz) ? 2 : 0;
            const int libWas = d.getDirectSampling();
            if (want != dsNow && !directSamplingSendNeeded(libWas, want)) dsNow = want;
            if (want != dsNow) {
                if (d.setDirectSampling(want) == 0) {
                    restoreTuner = fixed && switchReinitialisedTuner(libWas, want);
                    dsNow = want;
                }
            }
        }
        d.setCenter(hz);
        if (restoreTuner) restore("auto");
        else if (tunerBwHz > 0) d.setTunerBandwidth((uint32_t)tunerBwHz);
    }
    void setDirectSampling(int mode) {            // LocalSdrShim::setDirectSampling
        const int was = d.getDirectSampling();
        if (!directSamplingSendNeeded(was, mode)) { dsNow = mode; return; }
        d.setDirectSampling(mode);
        if (switchReinitialisedTuner(was, mode)) restore("manual");
        dsNow = mode;
    }
};

static bool tunerAsOpened(const Dongle& d, int filterHz, int gainTenth) {
    return d.dialError() == 0 && d.filter == filterHz && d.intFreq == r82xxIntFreqForBw(filterHz)
        && d.demodIf == d.intFreq && d.gainManual && d.gain == gainTenth;
}

static void directSamplingRoutes() {
    std::printf("── route #2: the AUTOMATIC crossover back to the tuner (autoDs, below 24 MHz) ──\n");
    {   // ★ The case this fix is for: no width ever asked for, HF then back to VHF.
        Server s; s.autoDs = true; s.open(100000000, 297);
        s.writerTune(7100000);
        ok(s.d.ds == 2, "HF: the crossover puts the dongle on the Q branch");
        s.writerTune(107700000);
        ok(s.d.ds == 0 && s.dsNow == 0, "VHF: back on the tuner");
        ok(s.d.dialError() == 0, "no width set, HF → VHF: on frequency (no +1.945 MHz)");
        ok(s.d.filter == 2048000 && s.d.intFreq == 1625000 && s.d.demodIf == 1625000,
           "the filter is the capture width again (2.048 MHz → IF 1.625 MHz, both halves), as the open set it");
        ok(s.d.gainManual && s.d.gain == 297, "the gain is back: manual mode, 29.7 dB");
        ok(!s.log.empty() && s.log.back() == "auto restored 2048000 297",
           "one 'direct sampling off (auto): tuner restored' line, with the filter and the gain");
        s.writerTune(93700000);
        ok(s.d.dialError() == 0 && s.d.filter == 2048000 && s.d.gain == 297 && s.log.size() == 1,
           "the next VHF tune changes nothing and restores nothing");
    }
    {   // ★ The same moves through the automatic route AS IT WAS — the regression this test guards.
        Server s; s.autoDs = true; s.fixed = false; s.open(100000000, 297);
        s.writerTune(7100000); s.writerTune(107700000);
        ok(s.d.filter == Dongle::kInitFilter, "before: the tuner kept r82xx_init's own filter (6 MHz), not ours");
        ok(!s.d.gainManual && s.d.gain < 0, "before: and its own gain loop — the manual gain was gone");
        ok(s.d.devBw == 2048000, "before: while librtlsdr still believed our filter was on (dev->bw)");
        // ★ Honest about the dial: a SERIAL re-init resets int_freq and the IF register together, so this
        //   route alone reads 0 in 2.0.3. The +1.945 MHz needs the TORN interleaving above (route #1).
        ok(s.d.dialError() == 0, "before: serial, the dial itself was right — it was the filter and gain");
    }
    {   // ★ VibeAGC moved the gain: the restore puts back where the TUNER was, not where the loop started.
        Server s; s.autoDs = true; s.open(100000000, 297);
        s.agcStep(364);
        s.writerTune(7100000); s.writerTune(98500000);
        ok(s.d.gainManual && s.d.gain == 364, "under VibeAGC: the loop's current 36.4 dB, not its 29.7 dB start");
    }
    {   // A width was asked for (the auto IF's 1.2 MHz): that is what comes back.
        Server s; s.autoDs = true; s.tunerBwHz = 1200000; s.open(100000000, 297);
        s.writerTune(7100000); s.writerTune(107700000);
        ok(tunerAsOpened(s.d, 1200000, 297), "a width set (1.2 MHz): restored with the gain, on frequency");
        ok(s.log.back() == "auto restored 1200000 297", "…through the same function");
    }
    {   // Later rate change: librtlsdr re-derives from dev->bw, which now matches the tuner again.
        Server s; s.autoDs = true; s.open(100000000, 297);
        s.writerTune(7100000); s.writerTune(107700000);
        s.sampleRate = 2400000; s.d.setSampleRate(2400000);
        ok(s.d.dialError() == 0 && s.d.demodIf == s.d.intFreq, "a later rate change keeps the two IF halves agreeing");
    }

    std::printf("── route #1: the MANUAL switch (setDirectSampling) — unchanged, same function ──\n");
    {
        Server s; s.open(100000000, 297);
        s.setDirectSampling(2);
        ok(s.d.ds == 2 && s.log.empty(), "on: nothing to restore going INTO direct sampling");
        s.d.freq = 107700000; s.d.centre(107700000);   // the owner tunes to FM while still on it
        s.setDirectSampling(0);
        ok(tunerAsOpened(s.d, 2048000, 297), "off: filter (capture width) and 29.7 dB back, on frequency");
        ok(s.log.size() == 1 && s.log.back() == "manual restored 2048000 297", "…one restore, the shared one");
    }

    std::printf("── the no-op path still sends NOTHING ──\n");
    {
        Server s; s.open(100000000, 297);
        s.setDirectSampling(0);
        ok(s.d.calls.empty() && s.log.empty(), "directSampling 0 on a dongle already on its tuner: no call at all");
        ok(s.dsNow == 0, "…and the state records librtlsdr's answer");
    }
    {
        Server s; s.autoDs = true; s.open(100000000, 297);
        s.writerTune(107700000);
        ok(s.d.calls.size() == 1 && s.d.calls[0] == "centre",
           "auto, first tune above the crossover (g_dsNow -1): the tune only — no ds0, no re-init");
        ok(s.dsNow == 0 && s.log.empty(), "…recorded as 0, nothing restored");
    }
    {   // First tune BELOW the crossover is a real switch into direct sampling; nothing to restore.
        Server s; s.autoDs = true; s.open(100000000, 297);
        s.writerTune(7100000);
        ok(s.d.calls.size() == 2 && s.d.calls[0] == "ds2" && s.log.empty(), "auto, first tune on HF: ds2 then the tune");
    }
    {   // Plan arithmetic.
        const TunerRestorePlan a = tunerRestorePlan(0, 2048000.0, -1, 297);
        ok(a.bwHz == 2048000 && a.captureWidth && a.gainTenth == 297, "plan: no width → the capture width; no write yet → the start gain");
        const TunerRestorePlan b = tunerRestorePlan(-1, 2400000.0, -1, -1);
        ok(b.bwHz == 2400000 && b.gainTenth == -1, "plan: nothing known → capture width, manual mode only");
        ok(!switchReinitialisedTuner(0, 0) && !switchReinitialisedTuner(0, 2) && switchReinitialisedTuner(2, 0)
           && switchReinitialisedTuner(1, 0) && !switchReinitialisedTuner(-1, 0),
           "only a move from direct sampling (1 or 2) back to 0 re-initialised the tuner");
    }
}

// ═══════════════════════════ THE CHIP'S OWN REGISTERS — where the mixer really is ═══════════════════════════

/** osmocom rtl-sdr 2.0.3's r82xx_set_pll() register encoding for an LO, with its fine-tune divider adjustment
 *  (`adjust` = -1, 0 or +1 steps of the divider index), written into a 30-register image as the chip holds it. */
static void encodePll(uint8_t* hw, uint32_t loHz, uint32_t xtal, int adjust, bool locked) {
    for (int i = 0; i < kR82xxReadRegs; ++i) hw[i] = 0;
    const uint32_t khz = (loHz + 500) / 1000;
    uint32_t mixDiv = 2; int divNum = 0;
    while (mixDiv <= 64) {
        if (khz * mixDiv >= 1770000u && khz * mixDiv < 3540000u) {
            uint32_t b = mixDiv; while (b > 2) { b >>= 1; divNum++; } break;
        }
        mixDiv <<= 1;
    }
    divNum += adjust;
    const uint64_t vco = (uint64_t)loHz * mixDiv;
    const uint64_t vcoDiv = ((uint64_t)xtal + 65536ull * vco) / (2ull * xtal);
    const uint32_t nint = (uint32_t)(vcoDiv / 65536), sdm = (uint32_t)(vcoDiv % 65536);
    const uint32_t ni = (nint - 13) / 4, si = nint - 4 * ni - 13;
    hw[0x10] = (uint8_t)(divNum << 5);
    hw[0x14] = (uint8_t)(ni + (si << 6));
    hw[0x12] = sdm == 0 ? 0x08 : 0x00;
    hw[0x15] = (uint8_t)(sdm & 0xff); hw[0x16] = (uint8_t)(sdm >> 8);
    hw[0x02] = locked ? 0x40 : 0x00;
    hw[0x04] = 0x20;   // fine tune 2, the reference
}

static void chipReadback() {
    std::printf("\n── the chip's PLL registers, decoded (rtlsdr_get_r82xx_state) ──\n");
    const uint32_t xtal = 28800000;
    uint8_t hw[kR82xxReadRegs];
    const struct { uint32_t tuner; int32_t intf; const char* what; } cases[] = {
        { 93715000, 1625000, "Kiko's 93.7 (+15 kHz DC offset) on the 2.048 MHz filter" },
        { 94515000, 1625000, "MASSA 94.5" },
        { 102315000, 1625000, "102.3, heard while 93.7 was dead" },
        { 107715000, 1625000, "107.7" },
        { 93715000, 3570000, "93.7 on init's IF (B1's state)" },
        { 96515000, 1575000, "the Sony's 96.5 on the 1.4 MHz rung" },
        { 225648000, 1625000, "DAB 12B" },
        { 1090000000, 1625000, "ADS-B" },
    };
    for (const auto& c : cases) {
        const uint32_t lo = c.tuner + (uint32_t)c.intf;
        encodePll(hw, lo, xtal, 0, true);
        const R82xxPll p = r82xxDecodePll(hw, kR82xxReadRegs, xtal);
        const double err = p.loHz - lo;
        ok(p.valid && p.locked && err < 500 && err > -500 && r82xxLoMatches(p.loHz, lo),
           std::string(c.what) + ": decoded LO " + std::to_string(p.loHz / 1e6) + " MHz == programmed");
        ok(p.divider == r82xxExpectedDivider(lo), std::string(c.what) + ": divider /" + std::to_string(p.divider)
           + " is the one the LO needs");
        // ★★ The fine-tune adjustment, either way: the VCO is right and the divider is not, so the mixer sits a
        //    factor of two from the dial — a flat floor, no stations. Must be caught.
        for (int adj : { -1, +1 }) {
            encodePll(hw, lo, xtal, adj, true);
            const R82xxPll q = r82xxDecodePll(hw, kR82xxReadRegs, xtal);
            ok(!r82xxLoMatches(q.loHz, lo) && q.divider != r82xxExpectedDivider(lo),
               std::string(c.what) + ": divider moved " + (adj < 0 ? "down" : "up") + " -> LO "
               + std::to_string(q.loHz / 1e6) + " MHz is reported as WRONG");
        }
    }
    encodePll(hw, 95340000, xtal, 0, false);
    ok(!r82xxDecodePll(hw, kR82xxReadRegs, xtal).locked, "an unlocked PLL reads as unlocked (rc was 0 regardless)");
    ok(!r82xxDecodePll(hw, 0x10, xtal).valid, "a short read (no PLL registers) decodes as not valid, never as a frequency");
    ok(r82xxLoMatches(14000000.0 + 28.8e6 + 1625000.0, 14000000.0 + 1625000.0),
       "an RTL-SDR Blog V4 on HF (librtlsdr upconverts by 28.8 MHz) is not reported as wrong");
    // ★★ The Sony's bridge returns 16 registers: the PLL comes from librtlsdr's copy, and must still decode.
    {
        uint8_t chip[kR82xxReadRegs], shadow[kR82xxReadRegs] = {}, img[kR82xxReadRegs];
        const uint32_t lo = 96115000 + 1575000;
        encodePll(chip, lo, xtal, 0, true);
        for (int r = 5; r < kR82xxReadRegs; ++r) shadow[r - 5] = chip[r];
        uint8_t got[kR82xxReadRegs] = {};
        for (int r = 0; r < 16; ++r) got[r] = chip[r];          // what a 16-byte read returns
        r82xxComposeImage(got, 16, shadow, img);
        const R82xxPll p = r82xxDecodePll(img, kR82xxReadRegs, xtal);
        ok(p.valid && r82xxLoMatches(p.loHz, lo), "16 registers from the chip + librtlsdr's copy: LO decodes right");
        encodePll(chip, lo, xtal, +1, true);                     // librtlsdr WROTE a moved divider
        for (int r = 5; r < kR82xxReadRegs; ++r) shadow[r - 5] = chip[r];
        r82xxComposeImage(got, 16, shadow, img);
        ok(!r82xxLoMatches(r82xxDecodePll(img, kR82xxReadRegs, xtal).loHz, lo),
           "…and a divider librtlsdr moved is still caught from its own copy");
    }
    ok(r82xxExpectedDivider(1625000.0) == 0, "the LO librtlsdr asks for before the first tune (centre 0 + IF) has no divider");
}

int main() {
    std::printf("── r82xx IF (osmocom rtl-sdr 2.0.3's r82xx_set_bandwidth) ──\n");
    ok(r82xxIntFreqForBw(2000000) == 1625000, "a 2.0 MHz filter (the 2000 kHz rung) leaves int_freq 1.625 MHz");
    ok(r82xxIntFreqForBw(2048000) == 1625000, "2.048 MHz (DAB's filter, or the capture width at 2.048 MS/s) too");
    ok(r82xxIntFreqForBw(2400000) == 1815000, "2.4 MHz (the capture width at 2.4 MS/s) leaves 1.815 MHz");
    ok(r82xxIntFreqForBw(2800000) == 3570000, "2.8 MHz rides the 6 MHz setting: 3.57 MHz, same as init");
    ok(r82xxIntFreqForBw(8000000) == 4570000, "8 MHz: 4.57 MHz");
    ok(r82xxIntFreqForBw(350000) == 2125000, "the narrowest rung, 350 kHz: 2.125 MHz");

    std::printf("── the fingerprint ──\n");
    const int32_t err = dialErrorHz(kR82xxInitIfHz, r82xxIntFreqForBw(2000000));
    ok(err == 1945000, "init's 3.570 MHz against the filter's 1.625 MHz = +1.945 MHz");
    ok(std::abs(err - 1950000) <= 25000,
       "which is Kiko's +1.950 MHz (107.7 heard at 109.65) to within an FM step");

    std::printf("── the interleaving ──\n");
    {   // Serial, either order: the halves agree and the dial is exact.
        FakeRtl d; d.setCenter(107700000);
        SetBw bw{d, 2000000}; bw.a(); bw.b(); bw.c();
        DsOff ds{d}; ds.a(); ds.b(); ds.c();
        ok(d.dialError() == 0, "filter then re-init, one after the other: on frequency");
        FakeRtl e; e.setCenter(107700000);
        DsOff ds2{e}; ds2.a(); ds2.b(); ds2.c();
        SetBw bw2{e, 2000000}; bw2.a(); bw2.b(); bw2.c();
        ok(e.dialError() == 0, "re-init then filter, one after the other: on frequency");
    }
    {   // Torn: the filter computes its IF, the re-init lands whole, then the filter writes its (now
        // stale) IF and re-tunes with init's int_freq.
        FakeRtl d; d.setCenter(107700000);
        SetBw bw{d, 2000000}; DsOff ds{d};
        bw.a(); ds.a(); ds.b(); ds.c(); bw.b(); bw.c();
        ok(d.dialError() == 1945000, "torn one way: +1.945 MHz — Kiko's symptom exactly");
        // ★ And it STAYS: every later tune adds the same int_freq against the same IF register, until
        //   something writes the filter again (with the auto filter off, nothing does).
        d.setCenter(93700000);
        ok(d.dialError() == 1945000, "…and a retune to 93.7 carries it along (95.65, as he reported)");
    }
    {   // Torn the other way: the re-init's int_freq is overwritten by the filter mid-sequence.
        FakeRtl d; d.setCenter(107700000);
        SetBw bw{d, 2000000}; DsOff ds{d};
        ds.a(); bw.a(); bw.b(); bw.c(); ds.b(); ds.c();
        ok(d.dialError() == -1945000, "torn the other way: -1.945 MHz — same fault, other sign");
    }
    {   // ★ The fix's order: a no-op direct-sampling request no longer re-initialises anything, and a
        //   real one waits for the writer's filter write to finish (devMtx), so only whole sequences run.
        FakeRtl d; d.setCenter(107700000);
        SetBw bw{d, 2000000}; bw.a(); bw.b(); bw.c();
        ok(d.dialError() == 0, "fixed: the filter runs whole, nothing tears it");
    }
    directSamplingRoutes();
    chipReadback();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
