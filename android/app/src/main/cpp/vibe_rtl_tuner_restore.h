// vibe_rtl_tuner_restore.h — what rtlsdr_set_direct_sampling(dev, 0) wipes, and the ONE function that
// puts it back. Used by BOTH routes back to the tuner: the owner's manual switch (setDirectSampling) and
// the hardware writer's automatic HF crossover (autoDs). test-r82xx-if.cpp drives this same code.
//
// ★★★ WHY ONE FUNCTION (2026-09-30, Kiko's +1.950 MHz). 20c5b9f0 taught the MANUAL switch to put the IF
//     filter and the gain back after the tuner's re-init; the AUTOMATIC crossover — the other caller of
//     the same librtlsdr call — was left restoring the filter only when a width had been asked for, and
//     never the gain. ONE RULE, TWO READERS. Both now go through restoreTunerAfterReinit().
//
// ★★ WHAT THE RE-INIT WIPES — read from osmocom rtl-sdr 2.0.3 (the tag sdr-kit/build-librtlsdr.sh and the
//    Linux Dockerfile both build), not assumed. rtlsdr_set_direct_sampling(dev, 0) runs, in order:
//      tuner->init  = r820t_init → r82xx_init → r82xx_set_tv_standard:  every tuner register back to
//                     r82xx_init_array (so the LNA/mixer gain is the TUNER'S OWN AUTO again, and the
//                     manual gain we wrote is gone), int_freq := 3.57 MHz and the filter re-calibrated
//                     for its 6 MHz default — NOT the filter we programmed;
//      rtlsdr_set_if_freq(dev, 3.57 MHz)  — the RTL2832's half of the IF, agreeing with the tuner's;
//      rtlsdr_set_center_freq(dev, dev->freq).
//    ★ It does NOT reset dev->bw — librtlsdr still BELIEVES our filter is on, and re-derives from that
//      stale belief at the next rtlsdr_set_sample_rate. So the tuner runs wide with the whole band in
//      its passband while every readout (and the "tuner write" diagnostic) says otherwise.
//    WHAT SURVIVES, and so is deliberately NOT re-sent (each re-send is another control transfer on a
//    radio that is streaming — see the "ONE WRITE" note in the hardware writer):
//      • ppm — r820t_init re-reads the tuner clock through rtlsdr_get_xtal_freq(), which applies
//        dev->corr, and the RTL2832's sample-clock correction is a demodulator register init never
//        touches;
//      • bias-T — rtlsdr_set_bias_tee is GPIO 0 on the RTL2832, not the tuner;
//      • the dongle's digital AGC — demodulator register 0x19, not the tuner;
//      • the sample rate — demodulator registers.
//    Offset tuning and the tuner IF gain are never used by this server.
//
// ★ The IF filter restored is the EFFECTIVE one — the width the server would program at open: the width
//   asked for (manual, the auto-IF's choice, or DAB's) when there is one, else the capture width. That
//   "else" is the case the automatic crossover used to skip, and it is what the open has done since
//   2026-09-22 ("THE IF FILTER IS PROGRAMMED ON EVERY OPEN"). Writing it re-derives BOTH halves of the IF
//   together (r820t_set_bw: int_freq, then the IF register, then a re-tune), so the dial stays exact.
#pragma once
#include <cmath>
#include <cstdint>

namespace vibertl {

/** What the tuner must be given back after a re-init. */
struct TunerRestorePlan {
    uint32_t bwHz = 0;          // the IF filter written — never 0 (0 would mean "librtlsdr's choice")
    bool     captureWidth = false;   // true when no width was asked for and the capture width stands in
    int      gainTenth = -1;    // the manual gain written, tenths of a dB; -1 = none known (mode only)
};

/** A switch that librtlsdr actually performs — it compares nothing itself, and a re-send of the mode it
 *  is already in is NOT a no-op (0-when-0 re-runs the whole tuner init). `libMode` is
 *  rtlsdr_get_direct_sampling(dev), librtlsdr's own record. */
constexpr bool directSamplingSendNeeded(int libMode, int want) { return libMode != want; }

/** Did that switch re-initialise the tuner? Only a move from direct sampling (1 = I, 2 = Q) back to it:
 *  going INTO direct sampling puts the tuner in standby, which forgets nothing we need until we return. */
constexpr bool switchReinitialisedTuner(int wasMode, int nowMode) { return nowMode == 0 && wasMode > 0; }

/** The plan, from the server's state.
 *  @param wantBwHz      g_tunerBwHz — the width asked for; <= 0 = none yet.
 *  @param sampleRate    the capture rate, the width that stands in for "none" (as the open does).
 *  @param hwGainNow     the gain last WRITTEN to the tuner (VibeAGC's current step, or the slider's).
 *  @param lastGainTenth the owner's / the start's gain — used when nothing has been written yet.
 *  ★ hwGainNow FIRST: under VibeAGC lastGainTenth is only where the loop STARTED, and snapping the tuner
 *    back there would leave the loop believing it is somewhere it is not. */
inline TunerRestorePlan tunerRestorePlan(int wantBwHz, double sampleRate, int hwGainNow, int lastGainTenth) {
    TunerRestorePlan p;
    p.captureWidth = wantBwHz <= 0;
    p.bwHz = p.captureWidth ? (uint32_t)std::lround(sampleRate) : (uint32_t)wantBwHz;
    p.gainTenth = hwGainNow >= 0 ? hwGainNow : lastGainTenth;
    return p;
}

/** ★★★ THE ONE "RESTORE THE TUNER AFTER ITS RE-INIT". Call it under the caller's devMtx, straight after a
 *  switch for which switchReinitialisedTuner() is true, once the dongle is on the frequency it should be
 *  on (the filter write re-tunes to librtlsdr's last centre). The reopen path's order: filter, manual
 *  gain mode, gain.
 *  Ops: int setTunerBandwidth(uint32_t) · int setTunerGainMode(int) · int setTunerGain(int) — thin
 *  wrappers over librtlsdr in the shim, a model of it in the test. Returns the first nonzero rc, or 0. */
template <class Ops>
inline int restoreTunerAfterReinit(Ops& o, const TunerRestorePlan& p) {
    int rc = o.setTunerBandwidth(p.bwHz);
    // ★ Manual mode ALWAYS — "AUTO" on a dongle is VibeAGC, which writes gains and trusts that the
    //   tuner never left manual mode. r82xx_init just took it out.
    const int rm = o.setTunerGainMode(1);
    if (!rc) rc = rm;
    if (p.gainTenth >= 0) { const int rg = o.setTunerGain(p.gainTenth); if (!rc) rc = rg; }
    return rc;
}

}  // namespace vibertl
