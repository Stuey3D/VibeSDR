// VibeServer capacity benchmark — how many users fit on this box?
//
// Drives the REAL vibedsp RxPipeline (waterfall FFT + DDC + demod + resample),
// which is exactly what one connected VibeServer user costs. Feeds synthetic IQ,
// so it needs NO dongle, no root, and no librtlsdr — you can run it on a box
// that's already busy doing something else (e.g. an ADS-B feeder) without
// touching it.
//
// Reports CPU time, so background load doesn't skew the result.
//
//   build:  ./build.sh
//   run:    ./bench

#include "../../android/app/src/main/cpp/vibedsp/vibedsp.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

using namespace vibedsp;

// ── which code path did we actually compile? ────────────────────────────────
// vibedsp/simd_internal.h enables NEON only on __aarch64__. A 32-bit userland
// (armv7l) silently falls back to scalar C — several times slower. This is the
// single most important line of output.
static const char* simdPath() {
#if defined(__aarch64__)
    return "NEON (aarch64) — fast path";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return "SCALAR — CPU has NEON but vibedsp only enables it on __aarch64__ (32-bit OS)";
#elif defined(__x86_64__)
    return "SCALAR (x86-64 host — indicative only, not comparable to a Pi)";
#else
    return "SCALAR";
#endif
}

// CPU time actually consumed by this process (not wall clock).
static double cpuSeconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
// Synthetic IQ: broadband noise + a strong FM-modulated carrier at an offset.
// The carrier keeps the demodulators doing real work rather than idling on zeros.
//
// ★ `wfmStereo` builds a REAL broadcast MPX — 19 kHz pilot, 38 kHz L-R subcarrier
// and a 57 kHz RDS subcarrier — instead of a bare 1 kHz tone. This matters for the
// benchmark's honesty, not its realism:
//
//   * Without a pilot the stereo PLL never LOCKS. The WFM figure then measures a
//     receiver sitting in mono, which is not what a listener on a broadcast station
//     costs. (The blend also never leaves 0, so the L-R half of the chain is doing
//     arithmetic on a signal that isn't there.)
//   * Without a lock the RDS demodulator NEVER RUNS AT ALL — `pipeline.cpp` gates it
//     on `pll_.locked()`. So the old "WFM (stereo + RDS)" row contained no RDS. That
//     understatement got worse on 2026-07-25, when the 57 kHz reference and bit clock
//     became conditional on an RDS subscriber too.
//
// The RDS subcarrier here carries arbitrary modulation, not valid group data: we are
// measuring what the demodulator COSTS, and it costs the same whether the bits decode.
static void makeIq(std::vector<cf32>& buf, double fs, double offsetHz, bool wfmStereo) {
    unsigned s = 12345;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (float)((int)(s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    };
    double ph = 0.0, mph = 0.0;
    const double dw = 2.0 * M_PI * offsetHz / fs;
    const double mdw = 2.0 * M_PI * 1000.0 / fs;   // 1 kHz modulating tone
    const double dev = 2.0 * M_PI * 75000.0 / fs;  // 75 kHz deviation (WFM)
    for (size_t i = 0; i < buf.size(); ++i) {
        mph += mdw;
        if (wfmStereo) {
            const double t = (double)i / fs;
            const double L = 0.3 * std::cos(2.0 * M_PI * 1000.0 * t);
            const double R = 0.3 * std::cos(2.0 * M_PI * 4000.0 * t);
            // Pilot and subcarriers are SINES, per the FM stereo standard.
            const double mpx = (L + R)
                             + 0.10 * std::sin(2.0 * M_PI * 19000.0 * t)
                             + (L - R) * std::sin(2.0 * M_PI * 38000.0 * t)
                             + 0.05 * std::sin(2.0 * M_PI * 1187.5 * t)
                                    * std::sin(2.0 * M_PI * 57000.0 * t);
            ph += dw + dev * mpx;
        } else {
            ph += dw + dev * std::sin(mph) / (2.0 * M_PI) * 0.01;
        }
        const float n_i = rnd() * 0.05f, n_q = rnd() * 0.05f;
        buf[i] = cf32((float)std::cos(ph) * 0.5f + n_i,
                      (float)std::sin(ph) * 0.5f + n_q);
    }
}

struct Result {
    double coreFrac;   // fraction of ONE core to sustain one user in real time
    double specRows;   // spectrum rows emitted (sanity: pipeline really ran)
    double audioSecs;  // audio produced (sanity)
    bool   stereo;     // did the pilot PLL lock? (WFM only — see makeIq)
};

static Result runOne(double fs, RxPipeline::Mode mode, double bwHz,
                     int fftSize, double fftRate, double seconds, bool rds,
                     bool fmdx = false, bool adv = false) {
    // Callbacks just count — we're measuring DSP cost, not I/O.
    struct Ctx { long rows = 0; long frames = 0; int rate = 48000; } ctx;

    RxPipeline::Callbacks cb;
    cb.ctx = &ctx;
    cb.spectrum = [](void* c, const float*, int) {
        ((Ctx*)c)->rows++;
    };
    cb.audio = [](void* c, const float*, int frames, int, int rate) {
        auto* x = (Ctx*)c;
        x->frames += frames;
        x->rate = rate;
    };
    // ★ RDS costs nothing unless something SUBSCRIBES to it (pipeline.cpp only builds
    // the 57 kHz reference and bit clock when a callback is set). So to measure RDS we
    // have to register for it, exactly as a real client with the decoder open does.
    if (rds) {
        cb.rdsPs   = [](void*, uint16_t, const char*) {};
        cb.rdsText = [](void*, const char*) {};
    }
    // ★ ADVANCED RDS IS A SEPARATE SUBSCRIPTION, and the same trap as above one level
    // down: pipeline.cpp gates the whole ADV payload on `cb_.rdsExt` alone, so a bench
    // that registered only rdsPs/rdsText measured a receiver with the ADV panel CLOSED
    // and called it RDS. What this row adds is real work, not bookkeeping — an MPX FFT
    // (kMpxFft, accumulated across blocks), the constellation, and the assembly of every
    // field the basic decoder throws away.
    if (adv) {
        cb.rdsExt = [](void*, const RxPipeline::Callbacks::RdsExt&) {};
    }

    RxPipeline pipe;
    // ★ BEFORE start(), so the first channel build already includes it — set afterwards,
    // this would measure one rebuild's worth of the plain chain as well.
    // ★★ RENAMED, AND THE MEANING CHANGED WITH IT. This was setRdsMaxPerformance() and it
    // WIDENED THE CHANNEL FILTER; that was measured to cost ~10 dB of RDS SNR and reverted
    // (see the chHalf note in pipeline.cpp). What FM-DX buys now is the guard-band NOISE
    // MEASUREMENT that makes the deviation readout honest on a weak signal — a second
    // filter pair, not a wider chain. The bench had not compiled since the rename, so every
    // FM-DX figure predating this came from a stale binary measuring a design we no longer
    // ship.
    pipe.setRdsNoiseCorrection(fmdx);
    pipe.start(fs, fftSize, fftRate, 48000, cb);
    pipe.setTune(200000.0, mode, bwHz);   // 200 kHz off centre

    const int block = 65536;
    std::vector<cf32> iq(block);
    makeIq(iq, fs, 200000.0, mode == RxPipeline::Mode::WFM);

    const long long total = (long long)(fs * seconds);
    long long done = 0;

    // Warm-up (filter build, first allocations) — not counted.
    pipe.feed(iq.data(), block);

    const double c0 = cpuSeconds();
    while (done < total) {
        pipe.feed(iq.data(), block);
        done += block;
    }
    const double cpu = cpuSeconds() - c0;

    // Read BEFORE stop() tears the pipeline down. If this is false on a WFM row the
    // stereo/RDS work never happened and the figure is an underestimate.
    const bool stereoLocked = (mode != RxPipeline::Mode::WFM) || (pipe.stereoBlend() > 0.5f);

    pipe.stop();

    const double iqSecs = (double)done / fs;
    Result r;
    r.coreFrac = cpu / iqSecs;          // 1.0 == needs a whole core to keep up
    r.specRows = (double)ctx.rows;
    r.audioSecs = (double)ctx.frames / (ctx.rate > 0 ? ctx.rate : 48000);
    r.stereo = stereoLocked;
    return r;
}

int main(int argc, char** argv) {
    double seconds = 8.0;               // seconds of IQ per test
    if (argc > 1) seconds = atof(argv[1]);

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());

    printf("\n");
    printf("VibeServer capacity benchmark\n");
    printf("=============================\n");
    printf("  DSP path : %s\n", simdPath());
    printf("  cores    : %u\n", cores);
    printf("  workload : %.0f s of IQ per test, waterfall 1024 bins @ 20 fps\n", seconds);
    printf("\n");
    printf("  'core%%' = CPU needed to sustain ONE user in real time.\n");
    printf("  100%% = one whole core. Lower is better.\n");
    printf("\n");

    struct Case { const char* name; RxPipeline::Mode mode; double bw; bool rds; bool fmdx; bool adv; };
    const Case cases[] = {
        // WFM appears three times because they are three genuinely different users, and the
        // gaps between them are the two things an operator is deciding about:
        //   plain stereo         — what most listeners cost
        //   + RDS                — the price of a client having the decoder open at all
        //                          (RDS is only built when something subscribes)
        //   FM-DX                — the price of the DX trade on top: a wider channel filter,
        //                          so the whole chain after it runs at 400 kHz instead of 300,
        //                          plus the guard-band noise measurement on the deviation.
        // ★ THIS IS THE ROW TO LOOK AT before enabling FM-DX on a small host. Shipped
        // un-benchmarked on 2026-07-27 with a warning on the switch rather than a number we
        // had made up; this row replaces the guess.
        // ★★ THE "FM-DX" ROWS ARE GONE — that mode was removed, and the flag they set
        //    (setRdsNoiseCorrection) is now tied to the ADVANCED RDS PANEL instead:
        //    local_sdr_shim.cpp does `if (type=="rdsx") { rdsxOn.store(on); setRdsNoiseCorrection(on); }`
        //    and turns it off again with "nobody looking: stop paying for it". So noise correction
        //    and the ADV payload are ONE switch in the shipping app, and the old table measured two
        //    combinations that cannot occur. The middle two rows below are kept only to ATTRIBUTE
        //    the cost between the payload and the noise measurement — neither ships on its own.
        {"WFM (stereo)",             RxPipeline::Mode::WFM,     180000.0, false, false, false},
        {"WFM + RDS",                RxPipeline::Mode::WFM,     180000.0, true,  false, false},
        {"WFM + RDS, noise corr",    RxPipeline::Mode::WFM,     180000.0, true,  true , false},
        {"WFM + ADV payload",        RxPipeline::Mode::WFM,     180000.0, true,  false, true },
        {"WFM + ADV RDS (as shipped)", RxPipeline::Mode::WFM,   180000.0, true,  true , true },
        {"NFM",                  RxPipeline::Mode::NFM,      12500.0, false, false, false},
        {"AM",                   RxPipeline::Mode::AM,       10000.0, false, false, false},
        {"SSB (USB)",            RxPipeline::Mode::SSB_USB,   2800.0, false, false, false},
    };
    // ★ ONE RATE PER RADIO WE ACTUALLY SUPPORT (updated 2026-07-30 — the list predated both the
    //   Airspy HF+ and the SDRplay RSP, so the two newest receivers were the two not measured):
    //     6.0 MSPS  SDRplay RSP, a wide setting — the one that will break a Pi first
    //     2.4 MSPS  RTL-SDR, the fastest it reliably sustains
    //     2.048/1.024 MSPS  RTL-SDR, lower settings
    //     2.0 MSPS  SDRplay RSP MINIMUM — zero-IF floor (sdrplay_source.cpp clamps below this),
    //               so it is the cheapest an RSP can be run and the realistic serving rate
    //     912 kHz   Airspy HF+ Discovery top rate — what people put on a Pi for FM DX
    //
    // ★★★ THE SYNTHETIC MPX DOES NOT SURVIVE EVERY RATE — a BENCH limitation, not a DSP bug.
    //     The pilot locks at 2.4 / 2.048 / 1.024 MSPS and does NOT at 6.0 or at exactly 2.0, so
    //     the WFM rows at those two rates measure a mono receiver and the bench says so on each.
    //     ★ It is the GENERATOR, not the radio: Stuart runs an SDRplay RSP at up to 8 MHz with
    //       stereo and RDS working, and has never seen a problem. Real hardware beats synthetic IQ.
    //     ★ Ruled out: it is not the tune (setTune matches makeIq's 200 kHz offset) and not the
    //       run length (fails at 8 s as well as 1 s). It is rate-specific and unexplained —
    //       2.048 works and 2.0 does not, which is the clue worth pulling on.
    //     ★★ So: TRUST THE NON-WFM ROWS AT EVERY RATE, and read WFM only where the pilot locked.
    //        Fixing makeIq() would make 6.0 and 2.0 usable, and 2.0 is the RSP's realistic serving
    //        rate, so it is worth doing before the capacity numbers are published.
    const double rates[] = {6000000.0, 2400000.0, 2048000.0, 2000000.0, 1024000.0, 912000.0};

    for (double fs : rates) {
        printf("── %.3f MSPS ─────────────────────────────────────────────\n", fs / 1e6);
        printf("   %-22s %8s   %s\n", "mode", "core%", "users that fit (est.)");
        for (const auto& c : cases) {
            const Result r = runOne(fs, c.mode, c.bw, 1024,
                                    getenv("WFPS") ? atof(getenv("WFPS")) : 20.0,
                                    seconds, c.rds, c.fmdx, c.adv);
            const double pct = r.coreFrac * 100.0;
            // Leave one core's worth of headroom for USB capture, the WebSocket
            // server, ADPCM and the OS — do not promise the last core.
            const double budget = (double)cores - 1.0;
            const int users = (r.coreFrac > 0.0)
                ? (int)(budget / r.coreFrac) : 0;
            printf("   %-22s %7.1f%%   %s%d\n", c.name, pct,
                   users >= 1 ? "~" : "", users);

            // Sanity: if the pipeline produced no audio, the number is garbage.
            if (r.audioSecs < 0.5) {
                printf("      !! produced only %.2fs of audio — result suspect\n",
                       r.audioSecs);
            }
            // A WFM row that never locked measured a MONO receiver with no RDS.
            if (!r.stereo) {
                printf("      !! stereo pilot never locked — stereo/RDS work did NOT run,\n");
                printf("         so this figure is an UNDERESTIMATE\n");
            }
        }
        printf("\n");
    }

    printf("Notes\n");
    printf("  * WFM stereo was reworked on 2026-07-25: the 15 kHz audio filters now\n");
    printf("    DECIMATE as they filter (~64 kHz audio rate instead of the full channel\n");
    printf("    rate), the FM discriminator uses a 4-wide NEON atan2, and the pilot PLL\n");
    printf("    is trig-free. Measured 26-41%% cheaper on Apple silicon depending on the\n");
    printf("    sample rate. If you have an older number to compare against, it was\n");
    printf("    taken before that AND with a WFM row that never locked stereo.\n");
    printf("  * ADPCM encode + WebSocket serving are NOT in these figures, but they\n");
    printf("    are small next to the DSP (they run at 48 kHz, the DSP at %.1f MSPS).\n", rates[0] / 1e6);
    printf("  * USB capture costs extra CPU that this test cannot measure (no dongle).\n");
    printf("    On a Pi 3 running dump1090 at 2.4 MSPS that was ~30%% of one core.\n");
    printf("  * 'users that fit' assumes one whole core is reserved for capture,\n");
    printf("    networking and the OS. Treat it as an upper bound, then halve it\n");
    printf("    before you put it in a README.\n");
    if (std::string(simdPath()).find("SCALAR") != std::string::npos) {
        printf("\n");
        printf("  ****  THIS IS THE SLOW PATH.  ****\n");
        printf("  You are NOT getting the NEON DSP. Re-run on a 64-bit (aarch64) OS\n");
        printf("  for the real numbers — expect a substantial speed-up.\n");
    }
    printf("\n");
    return 0;
}
