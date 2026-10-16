// vibe_thread_layout.h — THE RECEIVER'S THREAD LAYOUT, decided in one place.
//
// ★★★ ONE THREAD LAYOUT ON EVERY SERVER (2026-10-16, Stuart: "the multithreaded setup needs to be the default for
//     all servers … no point having different thread layouts for different setups"). The split — spectrum FFT on
//     vibe-spec, the demodulator + Opus on vibe-demod (RxPipeline, VIBE_DSP_THREADS=1), DAB's MSC on vibe-dab-msc
//     (dabSplitMsc, VIBE_DAB_SPLIT=1) — had grown up per platform: 32-bit ARM (vibeserver main.cpp, 2026-09-19),
//     Lite (its MainApplication), small aarch64 (2026-10-16, the Portable). Everything else ran the whole receiver
//     on ONE vibe-dsp thread. Measured on the Portable (Pi 3A+, 104.2, Advanced RDS): one thread 97-100 % dspCpu and
//     blocks 41-49 ms late per 32 ms; split, 41 % and 18-25 ms, with no warnings.
// ★ Splitting forces nothing onto separate cores — the scheduler may still run the threads on one. What it buys
//   everywhere is that the spectrum DROPS a frame rather than delay the audio, and that the stages overlap on idle
//   cores at lower clocks instead of one core flat out. A single-core box keeps one thread.
// ★ An owner's own VIBE_DSP_THREADS / VIBE_DAB_SPLIT (e.g. =0 in the service file) always wins: overwrite = 0, and
//   RxPipeline / dabSplitMsc only turn ON for '1'.
// ★ Called by local_sdr_shim.cpp at library load (so every host of the shared core — Linux vibeserver, the Mac
//   VibeServer, the Android app, Lite, iOS once its prebuilt lib is rebuilt — gets it before any pipeline starts)
//   and by the standalone tools (vibeserver-bench, dab-switch), which may not link the shim's object at all, so
//   that a bench measures the layout that is served.
#pragma once
#if !defined(_WIN32)
#include <cstdlib>
#include <thread>
#include <unistd.h>

namespace vibe {
/** True if the machine has two or more cores. ★★ CONFIGURED, NOT ONLINE: hardware_concurrency() is
 *  _SC_NPROCESSORS_ONLN on libc++ and libstdc++ alike, and an old Android phone (mpdecision — the Moto G1 class
 *  Lite runs on) hot-unplugs cores while idle, so at a boot auto-start it can read 1 on a quad core — and that
 *  phone would lose the split Lite used to give it unconditionally. _SC_NPROCESSORS_CONF counts what is fitted. */
inline bool vibeMultiCore() {
    const long conf = sysconf(_SC_NPROCESSORS_CONF);
    return conf >= 2 || std::thread::hardware_concurrency() >= 2;
}
/** Seed the split into the environment unless the owner already chose. Returns true (for a static initialiser). */
inline bool vibeApplyDefaultThreadLayout() {
    if (vibeMultiCore()) {
        setenv("VIBE_DSP_THREADS", "1", 0);
        setenv("VIBE_DAB_SPLIT",   "1", 0);
    }
    return true;
}
} // namespace vibe
#endif
