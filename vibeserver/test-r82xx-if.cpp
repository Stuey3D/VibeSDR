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
#include "vibe_r82xx_if.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
