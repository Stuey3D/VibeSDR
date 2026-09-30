// VibeSDR — the server benchmark: what THIS box can carry (Stuart, 2026-09-19).
//
// ★★★ WHY. A low-end box that cannot run WFM stereo should say so before a listener finds out as dropouts.
//     Lite runs this at first setup and switches red features OFF by default (the owner may override); the full
//     versions offer it at the top of setup as advice. Design: memory lite_first_setup_benchmark.md.
// ★★★ THE REAL CHAIN ON SYNTHETIC SIGNALS — RxPipeline exactly as the server builds it (its own thread options,
//     which 32-bit ARM switches on through VIBE_DSP_THREADS), fed a generated station. bench_wfm did the same and
//     matched the live Pi 2 within a few per cent.
// ★★★ SCORED BY THE HOTTEST THREAD against ONE core, never the average: the admin page's 4-core average once read
//     25 % with the audio already dead. Each thread's CPU is read from /proc/self/task; the worst one, over the
//     signal's own duration, is "% of a core in real time". Green < 70, amber 70-85, red > 85.
// ★ A recommended sample rate is never below 1.024 MS/s (RTLs struggle below ~1 MHz — Stuart); lower rates stay
//   a manual choice only.
// ★ Every future decoder adds its row here (the rule is Stuart's): an update re-runs it, so a new decoder starts
//   switched off on a box it would overload.
#pragma once
#include "vibedsp/vibedsp.h"
#if defined(VIBE_HAVE_OPUS)
#include "opus_audio_encoder.h"   // ★ a listener's audio is encoded per listener — see runListener
#endif
// ★ OUTSIDE the Opus guard: the snail's all-core calibration runs as the benchmark's first step on
//   every build. It had been put inside it, so the iOS lib (built without Opus) failed with
//   "undeclared identifier vibehealth" — found by the first build_ios.sh since (2026-09-28).
#include "vibe_health.h"
#include "vibe_benchmark_decoders.h"   // ★ every decoder timed, and how many at once (B6)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace vibe {

/** ★★★ WHERE IT HAS GOT TO, live (Stuart, 2026-09-20: "us humans need to see things working"). Two minutes of a
 *  blank screen is indistinguishable from a hang, and the one thing an owner must not do is power-cycle the box
 *  in the middle of it. runBenchmark keeps this up to date as it goes; a host reads it from another thread —
 *  the HTTP server is still answering while the measurement runs, because only the RADIO is stopped.
 *  ★ Steps, not seconds: each scenario takes about as long as the next, so a step count is an honest bar. A
 *    remaining-time estimate would be a guess dressed up as a fact. */
struct BenchProgress {
    std::atomic<bool> running{false};
    std::atomic<int>  step{0}, steps{0};
    std::mutex        m;
    std::string       label;
};
inline BenchProgress& benchProgress() { static BenchProgress p; return p; }

/** Move to the next step and name it. */
inline void benchStep(const std::string& label) {
    auto& p = benchProgress();
    p.step.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(p.m);
    p.label = label;
}
/** {"running":bool,"step":n,"steps":n,"label":"..."} — what a progress bar needs and nothing more. */
inline std::string benchProgressJson() {
    auto& p = benchProgress();
    std::string label;
    { std::lock_guard<std::mutex> lk(p.m); label = p.label; }
    std::string esc;
    for (char c : label) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
    return std::string("{\"running\":") + (p.running.load() ? "true" : "false")
         + ",\"step\":" + std::to_string(p.step.load())
         + ",\"steps\":" + std::to_string(p.steps.load())
         + ",\"label\":\"" + esc + "\"}";
}

namespace benchdetail {
using vibedsp::cf32;
using vibedsp::RxPipeline;

/** CPU seconds per thread of this process, by thread id. Linux / Android: /proc/self/task/<tid>/stat. */
inline std::map<long, double> threadCpu() {
    std::map<long, double> out;
#if defined(__linux__) || defined(__ANDROID__)
    const double hz = (double)sysconf(_SC_CLK_TCK);
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[96]; snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            if (FILE* f = fopen(path, "r")) {
                char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
                // ★ Fields after the ")" of the comm, which may itself hold spaces: utime is field 14, stime 15.
                const char* p = strrchr(buf, ')');
                unsigned long ut = 0, st = 0;
                if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2)
                    out[atol(e->d_name)] = (ut + st) / hz;
            }
        }
        closedir(d);
    }
#endif
    return out;
}
inline std::string threadName(long tid) {
#if defined(__linux__) || defined(__ANDROID__)
    char path[96]; snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
    if (FILE* f = fopen(path, "r")) { char b[64] = {0}; if (fgets(b, sizeof b, f)) { fclose(f); std::string s(b); while (!s.empty() && s.back() == '\n') s.pop_back(); return s; } fclose(f); }
#endif
    return "thread";
}
inline double threadSelfCpu() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/** A broadcast-FM station that DECODES: stereo, a 19 kHz pilot and REAL RDS — group 0A with proper checkwords,
 *  differentially encoded, biphase on 57 kHz locked to the pilot (the encoder from vibedsp/test/test_rds_dsp.cpp).
 *  ★★ It has to decode: the Advanced RDS scopes (the eye) only run once RDS locks, so a noise-like subcarrier made
 *     that row read ~free on the Pi 2, where live it costs ~16 % of a core. */
inline std::vector<cf32> fmStation(double fs, double seconds, double offset) {
    using vibedsp::RdsDecoder;
    auto enc = [](uint16_t data, int off) {
        const uint16_t cw = RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[off];
        return ((uint32_t)data << 10) | cw;
    };
    std::vector<int> bits;
    const char* PS = "VIBESDR ";
    for (int addr = 0; addr < 4; ++addr) {
        const uint32_t blk[4] = { enc(0xC0DE, 0), enc((uint16_t)(addr & 3), 1), enc(0x1234, 2),
                                  enc((uint16_t)(((uint8_t)PS[addr * 2] << 8) | (uint8_t)PS[addr * 2 + 1]), 4) };
        for (int b = 0; b < 4; ++b) for (int k = 25; k >= 0; --k) bits.push_back((blk[b] >> k) & 1);
    }
    std::vector<int> m(bits.size()); int prev = 0;
    for (size_t k = 0; k < bits.size(); ++k) { m[k] = prev ^ bits[k]; prev = m[k]; }
    const int n = (int)(fs * seconds);
    std::vector<cf32> iq(n);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        const double L = 0.25 * std::cos(2 * M_PI * 1000.0 * t), R = 0.25 * std::cos(2 * M_PI * 4000.0 * t);
        const double pilot = 0.1 * std::cos(2 * M_PI * 19000.0 * t);
        const double stereo = (L - R) * std::cos(2 * M_PI * 38000.0 * t);
        const long kb = (long)std::floor(t * 1187.5);
        const double phInBit = (t * 1187.5 - kb) * 2 * M_PI;
        const double manch = ((phInBit < M_PI) ? 1.0 : -1.0) * (m[(size_t)kb % m.size()] ? 1.0 : -1.0);
        const double rds = 0.05 * manch * std::cos(2 * M_PI * 57000.0 * t);
        const double mpx = (L + R) + pilot + stereo + rds;
        ph += 2 * M_PI * (offset + 75000.0 * mpx) / fs;
        if (ph > 2 * M_PI) ph -= 2 * M_PI; else if (ph < -2 * M_PI) ph += 2 * M_PI;
        iq[i] = cf32((float)(0.5 * std::cos(ph)), (float)(0.5 * std::sin(ph)));
    }
    return iq;
}
/** A narrowband voice-ish signal (two tones, AM-modulated) near `offset` — for NFM / AM / SSB. */
inline std::vector<cf32> nbSignal(double fs, double seconds, double offset) {
    const int n = (int)(fs * seconds);
    std::vector<cf32> iq(n);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        const double a = 0.3 * (1.0 + 0.5 * std::sin(2 * M_PI * 700.0 * t) + 0.3 * std::sin(2 * M_PI * 1700.0 * t));
        ph += 2 * M_PI * offset / fs; if (ph > 2 * M_PI) ph -= 2 * M_PI;
        iq[i] = cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
    }
    return iq;
}

struct Result { std::string id, label; double rate = 0, pct = 0; std::string hottest;
                std::vector<std::pair<std::string, double>> threads; };   // every thread's own % — see runOne

static void noAudio(void*, const float*, int, int, int) {}
static void noSpec(void*, const float*, int) {}
static void noPs(void*, uint16_t, const char*) {}
static void noText(void*, const char*) {}
static void noExt(void*, const vibedsp::RxPipeline::Callbacks::RdsExt&) {}

/** Run one scenario: `seconds` of signal through a fresh RxPipeline, fed as fast as it will take it; the hottest
 *  thread's CPU over the signal's duration is the score. */
inline Result runOne(const std::string& id, const std::string& label, double fs, RxPipeline::Mode mode, double bw,
                     const std::vector<cf32>& iq, double seconds, bool rdsExt) {
    Result r; r.id = id; r.label = label; r.rate = fs;
    static std::atomic<bool> rdsOff{false}, rdsOn{true};
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.audio = noAudio; cb.spectrum = noSpec;
    // ★★ THE SERVER'S RDS CALLBACKS, ALWAYS. The pipeline skips RDS decoding entirely unless the host registers one
    //    (pipeline.cpp: wantRds needs cb.rdsPs/rdsText/rdsPi/rdsSig/rdsExt) and the Advanced RDS scopes also need
    //    rdsExt. A server always wires them, so without these every WFM row read LOW (no RDS decoder at all).
    cb.rdsPs = noPs; cb.rdsText = noText; cb.rdsExt = noExt;
    pipe.setRdsExtWantedFlag(rdsExt ? &rdsOn : &rdsOff);
    /* ★★ THE INSTRUMENT MUST WAIT HERE, NOT DROP. Advanced RDS is measured on its own thread
     *  (MpxMeasure, "vibe-mpx"), which on a live server drops blocks rather than delay audio. Fed as
     *  fast as the pipeline will take it — this whole function — a dropping instrument would score a
     *  fraction of its real cost and grade a Pi 2 green for a feature it cannot carry. */
    pipe.setMeasureBlocking(true);
    const auto before = threadCpu();
    const double self0 = threadSelfCpu();
    pipe.start(fs, 4096, 10.0, 48000, cb);
    pipe.setTune(200000.0, mode, bw);
    // Warm up: the first blocks build the chain and fault the buffers in; the CEQ and AGC settle.
    const int blk = 65536, total = (int)iq.size();
    for (int o = 0; o < std::min(total, (int)fs / 2); o += blk) pipe.feed(iq.data() + o, std::min(blk, total - o));
    const auto mid = threadCpu();
    const double selfMid = threadSelfCpu();
    double fed = 0;
    while (fed < seconds * fs) {
        for (int o = 0; o < total; o += blk) pipe.feed(iq.data() + o, std::min(blk, total - o));
        fed += total;
    }
    // ★★ READ THE WORKERS BEFORE stop(): stop() joins them, and a thread that has exited has taken its
    //    /proc/self/task entry — and its CPU figure — with it. What is still queued (a few blocks) is not counted,
    //    a small under-read against several seconds of signal.
    const auto after = threadCpu();
    const double selfEnd = threadSelfCpu();
    std::map<long, std::string> names;              // ★ named NOW, while the threads still exist
    for (const auto& kv : after) if (!before.count(kv.first)) names[kv.first] = threadName(kv.first);
    pipe.stop();
    const double sigSecs = fed / fs;
    // The feeding thread (vibe-dsp's role) and every worker the pipeline started.
    double best = (selfEnd - selfMid) / sigSecs; r.hottest = "vibe-dsp";
    r.threads.push_back({ "vibe-dsp", 100.0 * best });
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;       // a thread that existed before this scenario is not ours
        auto m = mid.find(kv.first);
        const double used = kv.second - (m != mid.end() ? m->second : 0.0);
        // ★ EVERY thread's own figure, not only the worst: Advanced RDS lands on vibe-demod, and on a box where
        //   that is not yet the hottest the cost would otherwise be invisible until it is.
        r.threads.push_back({ names[kv.first], 100.0 * used / sigSecs });
        if (used / sigSecs > best) { best = used / sigSecs; r.hottest = names[kv.first]; }
    }
    (void)self0;
    r.pct = 100.0 * best;
    return r;
}
#if defined(VIBE_HAVE_OPUS)
struct ListenerAudio { OpusAudioEncoder enc; std::vector<int16_t> i16; std::vector<std::vector<uint8_t>> pkts; long n = 0; };
#endif

/** One locked-range listener: the channel cut from the shared FFT, then that listener's demodulator — the two
 *  things feedOneClient does on the listener's own thread. Scored as the SUM of its threads: they are all its
 *  own, and on a real server they land on whichever core is free. */
inline Result runListener(const std::string& id, const std::string& label, double fs, int fftSize, int chanBins,
                          double chanRate, RxPipeline::Mode mode, double bw, const std::vector<cf32>& iq,
                          double seconds) {
    Result r; r.id = id; r.label = label; r.rate = chanRate;
    /* ★★★ BEFORE THE PIPELINE IS BUILT. Taken after start(), its worker threads are already in `before` and are
     *  skipped as "not ours" — which silently dropped the demodulator AND the Opus encode from every locked-range
     *  row (they run on vibe-demod), leaving NFM at 3.8 % against 6.2 % live. */
    const auto before = threadCpu();
    static std::atomic<bool> rdsOff{false};
    vibedsp::Channelizer chan(fftSize);
    RxPipeline pipe;
    /* ★ NO SPECTRUM CALLBACK. A locked-range listener's own pipeline draws a waterfall only when the owner
     *  turned the private zoom view on (viewRx, config zoomSpectrum) — registering one here charged every
     *  listener a 1024-point FFT ten times a second that most never run: NFM read 14.3 % against 6.2 % live. */
    RxPipeline::Callbacks cb;
    cb.rdsPs = noPs; cb.rdsText = noText; cb.rdsExt = noExt;
    pipe.setRdsExtWantedFlag(&rdsOff);
    /* ★★ AND THE OPUS ENCODE, because every listener pays it and it is not small: the server encodes each
     *  listener's audio on that listener's own demod thread (see the note on setDemodThread). Without it this
     *  row read 3.9 % for an NFM listener the live Pi 2 costs 6.2 %. Where there is no libopus (the Android
     *  build) the server sends PCM and there is nothing to add. */
#if defined(VIBE_HAVE_OPUS)
    ListenerAudio la;
    cb.ctx = &la;
    const bool noEnc = std::getenv("VIBE_BENCH_NOENC") != nullptr;   // ★ temporary A/B: what does the encode cost?
    if (noEnc) cb.audio = noAudio; else
    cb.audio = [](void* ctx, const float* pcm, int frames, int ch, int) {
        auto* a = (ListenerAudio*)ctx;
        if (!a || frames <= 0) return;
        a->i16.resize((size_t)frames * ch);
        for (int i = 0; i < frames * ch; ++i) {
            const float v = pcm[i] * 32767.0f;
            a->i16[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        a->pkts.clear();
        a->enc.encode(a->i16.data(), frames, ch, a->pkts);
        a->n += (long)a->pkts.size();
    };
#else
    cb.audio = noAudio;
#endif
    pipe.start(chanRate, 1024, 10.0, 48000, cb);
    pipe.setTune(0.0, mode, bw);
    std::vector<cf32> slice((size_t)chanBins);
    vibedsp::Channelizer::ExtractCtx ectx;
    const int centreBin = fftSize / 4;                  // a channel off centre, as a listener's would be
    /* ★★★ TIME THE LISTENER'S OWN WORK, NOT THE SHARED FFT. chan.feed() runs the forward transform on THIS
     *  thread, and charging that to a listener read 76 % for an NFM listener the live server costs ~6 %. The
     *  server times exactly this span too, per listener, with the same clock (feedClientChannels ->
     *  CLOCK_THREAD_CPUTIME_ID around feedOneClient), and for the same reason. */
    double ownCpu = 0;
    auto round_ = [&](const cf32* p, int n) {
        chan.feed(p, n, [&](const cf32* bins, int nb) {
            (void)nb;
            const double t0 = threadSelfCpu();
            const int got = chan.extract(bins, centreBin, chanBins, slice.data(), ectx, chan.blockIndex());
            if (got > 0) pipe.feed(slice.data(), got);
            ownCpu += threadSelfCpu() - t0;
        });
    };
    const int blk = 65536, total = (int)iq.size();
    for (int o = 0; o < std::min(total, (int)fs / 2); o += blk) round_(iq.data() + o, std::min(blk, total - o));
    const auto mid = threadCpu();
    const double selfMid = threadSelfCpu();
    ownCpu = 0;                                            // ★ the warm-up's work is not the measurement's
    double fed = 0;
    while (fed < seconds * fs) {
        for (int o = 0; o < total; o += blk) round_(iq.data() + o, std::min(blk, total - o));
        fed += total;
    }
    const auto after = threadCpu();
    const double selfEnd = threadSelfCpu();
    std::map<long, std::string> names;
    for (const auto& kv : after) if (!before.count(kv.first)) names[kv.first] = threadName(kv.first);
    pipe.stop();
    const double sigSecs = fed / fs;
    (void)selfMid; (void)selfEnd;
    double sum = 100.0 * ownCpu / sigSecs;                // the extract and this listener's feed, alone
    r.threads.push_back({ "extract+demod", sum });
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;
        auto m = mid.find(kv.first);
        const double pct = 100.0 * (kv.second - (m != mid.end() ? m->second : 0.0)) / sigSecs;
        r.threads.push_back({ names[kv.first], pct });
        sum += pct;
    }
#if defined(VIBE_HAVE_OPUS)
    if (std::getenv("VIBE_BENCH_DEBUG")) std::fprintf(stderr, "[%s] opus packets %ld\n", id.c_str(), la.n);
#endif
    r.pct = sum; r.hottest = "per listener";
    return r;
}

inline const char* grade(double pct) { return pct < 0 ? "none" : pct < 70 ? "green" : pct <= 85 ? "amber" : "red"; }
} // namespace benchdetail

/** ★★ UPLINK, in kB/s — the other half of "how many listeners" (Stuart, 2026-09-19: a box in deepest Brazil may have
 *  CPU to spare and no bandwidth). ~4 MB posted to the directory's /api/speedtest, which reads and discards it; curl
 *  times it. -1 if it could not be measured. Desktop/Linux only — Android measures in Kotlin and passes it in. */
inline double measureUplinkKBps() {
#if defined(__ANDROID__)
    return -1;
#else
    FILE* p = popen("head -c 4194304 /dev/urandom | curl -s --max-time 60 -X POST --data-binary @- "
                    "-H 'Content-Type: application/octet-stream' -o /dev/null -w '%{speed_upload} %{http_code}' "
                    "https://vibeserver.vibesdr.net/api/speedtest 2>/dev/null", "r");
    if (!p) return -1;
    char b[128] = {0}; const bool got = fgets(b, sizeof b, p) != nullptr; pclose(p);
    double bps = 0; int code = 0;
    if (!got || sscanf(b, "%lf %d", &bps, &code) != 2 || code != 200 || bps <= 0) return -1;
    return bps / 1024.0;
#endif
}

/** Run the benchmark. `progress(done, of, label)` is told as each scenario starts. Returns the result as JSON:
 *  {"v":1,"at":<epoch>,"rows":[{id,label,rate,pct,grade,thread}],"recommendRate":<Hz>,"lockedUsers":{nfm,am,ssb}}. */
inline std::string runBenchmark(const std::function<void(int, int, const std::string&)>& progress = nullptr,
                                double secondsPerScenario = 4.0, double uplinkKBps = -2,
                                const std::function<std::vector<benchdetail::Result>()>& moreRows = nullptr) {
    // ★ moreRows: rows a host adds that need more than vibedsp — DAB (vibe_benchmark_dab.h: runDabRows), which
    //   pulls in the DAB service and its audio decoder. A row graded "none" (pct -1) could not be measured.
    // -2 = measure it here (desktop/Linux); an Android host passes its own figure, or -1 for "could not".
    /* ★ The step count is fixed before anything runs so the bar never jumps backwards: the network probe, the
     *  scenarios, the four locked-range listeners, and the two DAB rows when a host supplies them. */
    {
        auto& p = benchProgress();
        p.running.store(true);
        p.step.store(0);
        p.steps.store(1 + 1 + 7 + 4 + 6 + (moreRows ? 2 : 0));
        { std::lock_guard<std::mutex> lk(p.m); p.label = "starting"; }
    }
    /* ★★★ FIRST, WHILE THE MACHINE IS COOLEST: the all-core clock calibration the throttle snail is
     *  judged against (vibe_health.h, calibrateAllCore). Stuart, 2026-09-27: "On initial benchmark run
     *  a quick CPU stress test on all cores" — and "Only ever needs to be done on initial setup", so a
     *  machine that already holds a calibration skips it. Three seconds: enough to reach boost, not
     *  enough to heat-soak the chip into the very throttling it measures. */
    benchStep("CPU clock calibration");
    double allCoreMHz = -1;
    if (vibehealth::detail::kBoostClocks && !vibehealth::detail::calibrated()) {   // ★ x86 only — see kBoostClocks
        (void)vibehealth::detail::corePeaks();          // ★ load any stored calibration first
        if (!vibehealth::detail::calibrated()) allCoreMHz = vibehealth::detail::calibrateAllCore(3.0);
    }
    benchStep("network uplink");
    if (uplinkKBps == -2) { if (progress) progress(0, 1, "network uplink"); uplinkKBps = measureUplinkKBps(); }
    using namespace benchdetail;
    using M = RxPipeline::Mode;
    struct Sc { const char* id; const char* label; double fs; M mode; double bw; bool fm; bool rds; };
    // ★ Name the pipeline's worker threads, so the hottest one can be reported — unless the host already does.
    if (!RxPipeline::workerInit()) RxPipeline::workerInit() = [](const char* n) {
#if defined(__linux__) || defined(__ANDROID__)
        pthread_setname_np(pthread_self(), n);
#else
        (void)n;
#endif
    };
    const Sc sc[] = {
        { "wfm_2400",  "WFM stereo @ 2.4 MS/s",            2400000, M::WFM,     200000, true,  false },
        { "wfm_2048",  "WFM stereo @ 2.048 MS/s",          2048000, M::WFM,     200000, true,  false },
        { "wfm_1024",  "WFM stereo @ 1.024 MS/s",          1024000, M::WFM,     200000, true,  false },
        { "rdsx_2048", "WFM + Advanced RDS @ 2.048 MS/s",  2048000, M::WFM,     200000, true,  true  },
        { "nfm_2048",  "NFM @ 2.048 MS/s",                 2048000, M::NFM,      12500, false, false },
        { "am_2048",   "AM @ 2.048 MS/s",                  2048000, M::AM,       10000, false, false },
        { "ssb_2048",  "SSB @ 2.048 MS/s",                 2048000, M::SSB_USB,   2700, false, false },
    };
    const int N = (int)(sizeof sc / sizeof sc[0]);
    std::vector<Result> res;
    std::map<double, std::vector<cf32>> fmCache, nbCache;
    for (int i = 0; i < N; ++i) {
        benchStep(sc[i].label);
        if (progress) progress(i, N, sc[i].label);
        auto& cache = sc[i].fm ? fmCache : nbCache;
        if (!cache.count(sc[i].fs))
            cache[sc[i].fs] = sc[i].fm ? fmStation(sc[i].fs, 1.0, 200000.0) : nbSignal(sc[i].fs, 1.0, 200000.0);
        res.push_back(runOne(sc[i].id, sc[i].label, sc[i].fs, sc[i].mode, sc[i].bw, cache[sc[i].fs],
                             secondsPerScenario, sc[i].rds));
    }
    /* ★★★ LOCKED RANGE — THE LISTENER'S WHOLE CHAIN, NOT JUST ITS DEMODULATOR (2026-09-19).
     *  This timed a bare RxPipeline at channel rate, and on a Pi 2 read 3.4 % for an NFM listener the live
     *  server cost ~7 %. The missing half is the CHANNEL EXTRACT: the listener's own inverse transform, cut
     *  from the shared forward FFT (feedOneClient -> Channelizer::extract). Its cost barely changes with the
     *  channel's width, so it is most of a narrow listener and a rounding error for WFM — which is exactly
     *  why WFM already agreed with the live figure and NFM did not.
     *  ★★ MEASURED, NOT SCALED. A fudge factor fitted on one box would be wrong on the next; running the same
     *     two pieces the server runs is right everywhere. Live on the Pi 2, 6 NFM listeners: 4.0 % extract +
     *     2.2 % demod + 0.8 % sending each.
     *  ★ The shared forward FFT is NOT counted here — it is paid once, by vibe-dsp, however many listeners
     *    there are, and it is already in the rows above. What this row is, is the part that SCALES. */
    struct Lk { const char* id; const char* label; M mode; double bw; bool fm; };
    const Lk lk[] = {
        { "lk_wfm", "Locked range, per WFM listener", M::WFM,     200000, true  },
        { "lk_nfm", "Locked range, per NFM listener", M::NFM,      12500, false },
        { "lk_am",  "Locked range, per AM listener",  M::AM,       10000, false },
        { "lk_ssb", "Locked range, per SSB listener", M::SSB_USB,   2700, false },
    };
    std::map<std::string, double> perListener;
    {
        // The server's own geometry: fftSizeForRate (rate/75, at least 4096) and chanBinsFor (2.5x the
        // bandwidth, at least 24 kHz, a power of two dividing the FFT).
        const double fs = 2048000.0;
        int fftSize = 4096; while (fftSize < (int)(fs / 75.0) && fftSize < 32768) fftSize *= 2;
        const auto wide = fmStation(fs, 1.0, 200000.0);
        for (const auto& l : lk) {
            benchStep(l.label);
            if (progress) progress(N, N, l.label);
            int chanBins = 64;
            const double need = std::max(l.bw * 2.5, 24000.0);
            while (chanBins < fftSize && fs * chanBins / fftSize < need) chanBins <<= 1;
            const double chanRate = fs * chanBins / fftSize;
            res.push_back(runListener(l.id, l.label, fs, fftSize, chanBins, chanRate, l.mode, l.bw,
                                      wide, secondsPerScenario));
            perListener[l.id] = res.back().pct;
        }
    }
    /* ★★★ THE DECODERS, EACH ON THIS BOX (B6) — see vibe_benchmark_decoders.h. Their rows join the
     *  table like any other; the recommendation below is what becomes the owner's decoder limit. */
    double heaviestDec = 0; std::string heaviestId;
    for (const auto& d : benchdec::runDecoderRows(secondsPerScenario, [&](const std::string& l) {
             benchStep(l); if (progress) progress(N, N, l); })) {
        Result r; r.id = d.id; r.label = d.label; r.rate = 48000; r.pct = d.pct; r.hottest = "decoder";
        res.push_back(r);
        if (d.pct > heaviestDec) { heaviestDec = d.pct; heaviestId = d.id; }
    }
    if (moreRows) { if (progress) progress(N, N, "DAB+"); for (auto& r : moreRows()) res.push_back(r); }
    if (progress) progress(N, N, "done");
    { auto& p = benchProgress(); p.step.store(p.steps.load()); { std::lock_guard<std::mutex> lk(p.m); p.label = "done"; } }
    // ★ The recommended rate: the highest WFM rate that grades green, never below 1.024 MS/s.
    double rec = 1024000;
    for (const auto& r : res)
        if (r.id.rfind("wfm_", 0) == 0 && r.pct < 70 && r.rate > rec) rec = r.rate;
    // ★ Locked range: every listener is a demodulator of their own, so the ceiling is how many fit in one core's
    //   green budget. An ESTIMATE from the single-listener figure, labelled as one.
    // ★ Spare cores = all but the one the shared front (vibe-dsp) holds; each gets the green budget (70 %).
    const long cores = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    const double budget = 70.0 * std::max(1L, cores - 1);
    auto usersFor = [&](const char* id) {
        auto it = perListener.find(id);
        // ★ Capped at 20: past that the uplink decides, not the CPU (a fast machine "fits" thousands).
        return (it == perListener.end() || it->second <= 0) ? 0 : std::min(20, std::max(0, (int)std::floor(budget / it->second)));
    };
    std::string j = "{\"v\":1,\"at\":" + std::to_string((long long)time(nullptr)) + ",\"rows\":[";
    for (size_t i = 0; i < res.size(); ++i) {
        char b[320];
        snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"label\":\"%s\",\"rate\":%.0f,\"pct\":%.1f,\"grade\":\"%s\",\"thread\":\"%s\",\"threads\":{",
                 i ? "," : "", res[i].id.c_str(), res[i].label.c_str(), res[i].rate, res[i].pct, grade(res[i].pct),
                 res[i].hottest.c_str());
        j += b;
        for (size_t t = 0; t < res[i].threads.size(); ++t) {
            snprintf(b, sizeof b, "%s\"%s\":%.1f", t ? "," : "", res[i].threads[t].first.c_str(), res[i].threads[t].second);
            j += b;
        }
        j += "}}";
    }
    j += "],\"recommendRate\":" + std::to_string((long long)rec);
    j += ",\"cores\":" + std::to_string(cores);
    // ★ The all-core clock this run calibrated the throttle snail to (MHz), or absent when it was
    //   already calibrated at setup or the machine has no cpufreq to read.
    if (allCoreMHz > 0) j += ",\"allCoreMHz\":" + std::to_string((int)std::lround(allCoreMHz));
    // ★ Listeners the LINK carries at 100 kB/s each — Stuart's worst case, above the ~80-90 kB/s DAB+ peaks
    //   (a jitter buffer catching up). The smaller of this and the CPU figures is the honest ceiling.
    if (uplinkKBps > 0) {
        char nb[96]; snprintf(nb, sizeof nb, ",\"network\":{\"uplinkKBps\":%.0f,\"users\":%d}", uplinkKBps, (int)std::floor(uplinkKBps / 100.0));
        j += nb;
    } else j += ",\"network\":null";
    benchProgress().running.store(false);
    j += ",\"lockedUsers\":{\"wfm\":" + std::to_string(usersFor("lk_wfm")) + ",\"nfm\":" + std::to_string(usersFor("lk_nfm"))
       + ",\"am\":" + std::to_string(usersFor("lk_am")) + ",\"ssb\":" + std::to_string(usersFor("lk_ssb")) + "}";
    /* ★★★ HOW MANY DECODERS AT ONCE, WITH A FULL HOUSE LISTENING (Stuart: "with this current load
     *     another 4 users could have decoders but not all 10 at once"). The house is ten narrow-band
     *     listeners — the per-VFO case, where every listener costs their own chain — or as many as
     *     the box fits if that is fewer. `recommend` is what the setup page and the app write into
     *     the config as the owner's default decoder limit. */
    {
        const int house = std::min(10, usersFor("lk_nfm"));
        auto pl = perListener.find("lk_nfm");
        const double per = pl == perListener.end() ? 0.0 : pl->second;
        const int rec = benchdec::recommendDecoders(cores, per, house, heaviestDec);
        char db[256];
        snprintf(db, sizeof db, ",\"decoders\":{\"recommend\":%d,\"forUsers\":%d,\"perListenerPct\":%.1f,"
                 "\"heaviest\":\"%s\",\"heaviestPct\":%.1f}", rec, house, per, heaviestId.c_str(), heaviestDec);
        j += db;
    }
    j += "}";
    return j;
}

} // namespace vibe
