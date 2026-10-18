// vibe_denormals.h — flush denormals to zero on the calling thread (FTZ + DAZ).
//
// ★★★ WHY (measured 2026-10-18 on the Lenovo, i5-11300H, amd64 and i386 builds alike: a trap counter
//     that unmasks the denormal exception to find WHERE, then 5 interleaved timing rounds off vs on).
//     A recursive filter fed an EXACT zero — or a clean carrier that demodulates to one — decays
//     geometrically through 1e-38 into the denormal range and stays there, and every x86 instruction
//     that touches a denormal takes a microcode assist (~100+ cycles on a modern core; worse on the
//     in-order Atoms the i386 package is for). Where: MultipathMeter and MpxNoiseMeter (vibedsp.h)
//     after a WFM signal drops to digital silence, the audio FirDecimator on a noise-free carrier,
//     and libopus encoding a squelched listener's zeros. vibe-demod, % of a core, off -> on:
//       WFM, clean unmodulated carrier   46.2 -> 2.3  (amd64)   47.0 -> 2.4  (i386)
//       WFM, signal drops to zero         9.8 -> 2.4            10.1 -> 2.6
//       NFM, clean carrier                3.7 -> 0.2             3.8 -> 0.3
//       AM listener + Opus, silence       0.31 -> 0.25 (amd64; Debian's i386 libopus is untouched)
// ★★ AN RTL'S OWN NOISE HIDES IT. With an idle dongle's 1-2 LSB of noise on the input, and on every row
//    of the stock benchmark, nothing moved beyond run-to-run spread — the filters never decay that
//    far, which is why nobody saw it. What breeds them is a source that goes QUIET: a dongle at
//    minimum gain where the noise falls under one LSB, a radio that delivers zeros while it
//    recovers, a squelched listener's Opus encode. On the boxes this matters for that is the
//    difference between a green row and a dropout, and it costs nothing when it does not apply,
//    so it is on for every DSP thread rather than argued case by case.
// ★★ THE AUDIO IS BIT-IDENTICAL. WFM/NFM/AM/SSB on programme, RTL noise, a drop to silence and a
//    clean carrier, decoded on vs off: every sample equal, on amd64 and i386. A denormal is below
//    1.2e-38 (~760 dB under full scale) and vanishes into any normal sum; flushing it changes only
//    how a decay ENDS, and every chain already meets exact zeros (an exact-zero input yields them
//    directly), so a zero is not a new state for any meter or log.
// ★ PER THREAD, NOT PER PROCESS. MXCSR / FPCR / FPSCR are thread state. Linux copies it into each
//   thread created after it is set (vibeserver main() sets it first for that reason), but a thread
//   started earlier, an Android JNI thread or an Apple pthread need not have it, so every thread
//   that runs DSP calls this at its top — via the role functions in vibe_thread.h and RxPipeline's
//   own workers.
// ★ ARM: 32-bit NEON already flushes (that is why armhf never showed it in vector code); FZ
//   brings VFP scalar code into line — on the ARM11 of a Pi Zero/Pi 1 a denormal is trapped to the
//   kernel's VFP support code, the most expensive case of all. AArch64 cores handle denormals in
//   hardware with little or no penalty (not measured here), so there it is consistency — the same
//   output on every server — rather than speed.
#pragma once
#include <cstdint>
#include <cstring>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define VIBE_DENORMALS_X86 1
#endif

namespace vibe {

#if defined(VIBE_DENORMALS_X86)
/** The CPU's MXCSR_MASK (FXSAVE byte 28): which MXCSR bits it accepts. ★★ DAZ (bit 6) is NOT
 *  universal on SSE2 parts — the first Pentium 4 steppings lack it, and setting an unsupported
 *  MXCSR bit is a #GP fault, i.e. the server dies on its first DSP thread. FTZ (bit 15) is in
 *  every SSE CPU. A zero mask means the architectural default 0xFFBF (no DAZ). */
inline uint32_t vibeMxcsrMask_() {
#if defined(__GNUC__) || defined(__clang__)
    static const uint32_t mask = [] {
        alignas(16) unsigned char area[512];
        std::memset(area, 0, sizeof area);
        __asm__ volatile("fxsave %0" : "=m"(*(unsigned char(*)[512])area));
        uint32_t m; std::memcpy(&m, area + 28, sizeof m);
        return m ? m : 0xFFBFu;
    }();
    return mask;
#else
    return 0xFFBFu;   // ★ no inline asm: FTZ only, which is always safe
#endif
}
#endif

/** Flush denormal results to zero, and treat denormal inputs as zero, on THIS thread. Cheap and
 *  idempotent: call it at the top of every thread that runs DSP. */
inline void vibeFlushDenormals() {
#if defined(VIBE_DENORMALS_X86)
    const uint32_t want = 0x8000u | (vibeMxcsrMask_() & 0x0040u);   // FTZ | DAZ where the CPU has it
    _mm_setcsr(_mm_getcsr() | want);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);                                            // FPCR.FZ
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__) && (defined(__GNUC__) || defined(__clang__))
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);                                             // FPSCR.FZ
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}

} // namespace vibe
