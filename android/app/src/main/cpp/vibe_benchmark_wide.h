// vibe_benchmark_wide.h — WIDE CAPTURE RATES, MEASURED (V12, 2026-11-03). Included by vibe_benchmark.h, after benchdetail.
//
// ★★★ WHY. A tester's USRP B210 on an 11th-gen i7 wants the whole FM band, 82–108 MHz with the Dutch pirate end, in ONE
//     capture — 26 MHz of band, so 30.72 MS/s — served as a LOCKED RANGE to many listeners ("so he gets the KA9Q benefit,
//     which is designed to handle super wide sample rates" — Stuart). Whether a machine can carry that is a measurement,
//     not a guess, and the USRP's rates above 16 MS/s are offered only where this says yes (vibe_usrp_caps.h ratesFor).
// ★★★ THE SERVER'S OWN LOCKED-RANGE PATH, THREAD FOR THREAD (local_sdr_shim.cpp — the USRP's sink, dspLoop,
//     feedClientChannels, handBlockToListeners, a ClientDsp's own thread):
//       · INTAKE (stands in for vibe-usrp): sc16 off the wire → fc32 (UHD's converter's job) → copied into the IQ queue
//         (enqueueIqFloat), ~5 ms a hand-off, the queue bounded at a quarter-second as the server's now is;
//       · FRONT END (stands in for vibe-dsp): the capture cleaner (IqCleaner — DC and I/Q image), the shared forward FFT
//         (Channelizer at fftSizeForRate), the block copied ONCE and posted to every listener's bounded queue
//         (listenerQueueBlocks), and the wide waterfall row off the same FFT (emitWideFromBins: power + log10 per bin at
//         fftRate × FFT_AVG rows a second);
//       · EACH LISTENER (its own thread, as ClientDsp::th): the channel cut from the block (Channelizer::extract at
//         chanBinsFor(200 kHz)), that listener's RxPipeline (WFM stereo + RDS, its own workers as served) and its Opus
//         encode where the build has libopus.
//     Paced in REAL TIME — fed at the capture's own rate, so a stage that cannot keep up shows as drops, the way it would
//     on air, and every thread's CPU is "% of one core in real time".
// ★★★ SCORED BY THE HOTTEST THREAD (Stuart, 2026-11-03: "so we dont run into the saturation of 1 core issue that we have
//     hit in the past"; docs/v12/DECODER-THREADING.md). A machine with eight idle cores still drops samples the moment ONE
//     thread passes 100 %. The fixed cost (intake + front end) and the added cost per listener are reported separately.
// ★★ BOUNDED. The test signal is 0.125 s, looped seamlessly (every tone and the carrier offset are whole cycles in it),
//    held as sc16 — 15 MB at 30.72 MS/s. Rates run ascending and stop at the first one this machine cannot carry.
#pragma once
#include "vibe_bench_profiles.h"   // ★ V12 the three locked-range profiles' rules (2026-11-18)
#include "vibe_listener_queue.h"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace vibe {
namespace benchwide {
using benchdetail::cf32;
using benchdetail::Result;
using vibedsp::RxPipeline;

/** The server's own geometry (local_sdr_shim.cpp fftSizeForRate / chanBinsFor) — restated because the shim is not a
 *  header; test-bench-hw checks the two stay equal at the rates that matter. */
inline int fftSizeForRate(double rate) { int s = 4096; while (s < (int)(rate / 75.0) && s < 32768) s *= 2; return s; }
inline int chanBinsFor(double rate, int fftSize, double bwHz) {
    return vibedsp::sliceBinsFor(fftSize, rate, std::max(bwHz * 2.5, 24000.0));   // ★ a power of two: the doubling, exactly
}
/** ★★ V12 (2026-11-19): the LISTENERS' forward FFT (local_sdr_shim.cpp lfft() — vibedsp.h listenerFftSize). The waterfall
 *  keeps fftSizeForRate's. VIBE_BENCH_POW2 (an A/B switch, never set by a server) measures the old power-of-two one. */
inline int listenerFftForRate(double rate) {
    const int p = fftSizeForRate(rate);
    return std::getenv("VIBE_BENCH_POW2") ? p : vibedsp::listenerFftSize(rate, p);
}
/** The wide row's rate: the server's default fftRate (20) × FFT_AVG (4) — emitWideFromBins decimates blocks to it. */
constexpr double kWideRowsPerSec = 80.0;

/** A broadcast station at `offset`, `seconds` long, as sc16 (the wire format) — benchdetail::fmStation, converted. */
inline std::vector<int16_t> fmStationSc16(double fs, double seconds, double offset) {
    const auto f = benchdetail::fmStation(fs, seconds, offset);
    std::vector<int16_t> o(f.size() * 2);
    for (size_t i = 0; i < f.size(); ++i) { o[2 * i] = (int16_t)std::lround(f[i].real() * 16000.0f); o[2 * i + 1] = (int16_t)std::lround(f[i].imag() * 16000.0f); }
    return o;
}

/** One bounded hand-off queue (the IQ queue, a listener's block queue): drops the OLDEST when full, and counts it. */
template <class T> struct BQ {
    std::mutex m; std::condition_variable cv; std::deque<T> q; size_t cap = 8, held = 0; long dropped = 0; bool stop = false;
    size_t (*weight)(const T&) = nullptr;
    void push(T v) {
        { std::lock_guard<std::mutex> lk(m);
          const size_t w = weight ? weight(v) : 1;
          while (!q.empty() && held + w > cap) { held -= weight ? weight(q.front()) : 1; q.pop_front(); ++dropped; }
          held += w; q.push_back(std::move(v)); }
        cv.notify_one();
    }
    bool pop(T& v) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return stop || !q.empty(); });
        if (q.empty()) return false;
        v = std::move(q.front()); q.pop_front(); held -= weight ? weight(v) : 1;
        return true;
    }
    void close() { { std::lock_guard<std::mutex> lk(m); stop = true; } cv.notify_all(); }
};

inline void nameThread(const char* n) {
#if defined(__linux__) || defined(__ANDROID__)
    pthread_setname_np(pthread_self(), n);
#elif defined(__APPLE__)
    pthread_setname_np(n);
#else
    (void)n;
#endif
}

struct LockedFigures {
    double hottest = 0; std::string hottestName;
    double intake = 0, front = 0, perListener = 0, listenersTotal = 0, process = 0;
    double listenerHottest = 0;   // ★ V12 the hottest single thread any listener ran (its own, or its pipeline's worker)
    long dropped = 0;             // IQ buffers + listener blocks lost — anything above 0 = it did not keep up
    long iqDropped = 0, listenerDropped = 0;   // ★ …the two apart: the shared path's, the listeners'
    bool behind = false;          // the intake could not keep real time (its own pace slipped)
    double perListenerKB = -1;    // ★ one listener's memory (benchprof::perListenerKB) — -1 with no listeners
    double fixedKB = 0;           // ★ the shared path's buffers AT THEIR BOUND (the IQ queue, the blocks in flight)
};
/** ★★ V12 WHICH LISTENER (2026-11-18): the locked-range path is the same for every mode — only the listener's chain
 *  differs. WFM by default (the wide rows); NFM / AM / SSB for the narrow profile. */
struct LockedMode { RxPipeline::Mode mode = RxPipeline::Mode::WFM; double bw = 200000.0; };

/** ★★★ THE LOCKED-RANGE PATH AT `fs` WITH `nListeners` LISTENERS (WFM unless `lm` says otherwise), in real time — see
 *  the file note. */
inline LockedFigures runLocked(double fs, int nListeners, const std::vector<int16_t>& sc16, double warmSec, double measSec,
                               const LockedMode& lm = LockedMode()) {
    LockedFigures F;
    const int engineN = fftSizeForRate(fs);          // ★ the waterfall's
    const int fftSize = listenerFftForRate(fs);      // ★ the listeners' (the same, wherever a power of two fits)
    const int chanBins = chanBinsFor(fs, fftSize, lm.bw);
    const double chanRate = fs * chanBins / fftSize;
    const int centreBin = fftSize / 4;                       // the station is at fs/4 (fmStationSc16's offset)
    const size_t total = sc16.size() / 2;
    const size_t hand = std::max<size_t>(1024, (size_t)(fs * 0.005));   // ~5 ms a hand-off — usrp_source.cpp's recv size

    struct Blk { std::shared_ptr<std::vector<cf32>> bins; long long index = 0; };
    BQ<std::vector<cf32>> iq;
    iq.weight = [](const std::vector<cf32>& v) { return v.size(); };
    iq.cap = std::max<size_t>(262144, (size_t)(fs * 0.25));   // ★ the server's bound (enqueueIqFloat)

    struct Lis {
        BQ<Blk> q; RxPipeline pipe; vibedsp::Channelizer::ExtractCtx ectx; std::vector<cf32> slice; std::thread th;
#if defined(VIBE_HAVE_OPUS)
        benchdetail::ListenerAudio la;
#endif
    };
    std::vector<std::unique_ptr<Lis>> lis;
    vibedsp::Channelizer chan(fftSize);
    const size_t lcap = vibe::listenerQueueBlocks(fs, fftSize - fftSize / vibedsp::Channelizer::OVERLAP_DIV);
    std::atomic<bool> running{true};
    /* ★★ THE SHARED PATH'S MEMORY AT ITS BOUND, not as found: the IQ queue full (a quarter-second) and every listener
     *  queue's blocks in flight (shared between listeners — one copy each), plus the channeliser's own buffers. */
    F.fixedKB = (double(iq.cap) * sizeof(cf32) + double(lcap) * fftSize * sizeof(cf32) + 4.0 * fftSize * sizeof(cf32)) / 1024.0;
    const double heap0 = benchprof::heapKB(), rss0 = benchprof::rssKB();
    double heap1 = 0, rss1 = 0;

    const auto before = benchdec::threadTimes();
    for (int i = 0; i < nListeners; ++i) {
        auto L = std::make_unique<Lis>();
        L->q.cap = lcap;
        L->slice.resize((size_t)chanBins);
        RxPipeline::Callbacks cb;
        cb.rdsPs = benchdetail::noPs; cb.rdsText = benchdetail::noText; cb.rdsExt = benchdetail::noExt;
        static std::atomic<bool> rdsOff{false};
        L->pipe.setRdsExtWantedFlag(&rdsOff);
#if defined(VIBE_HAVE_OPUS)
        cb.ctx = &L->la;
        cb.audio = [](void* ctx, const float* pcm, int frames, int ch, int) {
            auto* a = (benchdetail::ListenerAudio*)ctx;
            if (!a || frames <= 0) return;
            a->i16.resize((size_t)frames * ch);
            for (int k = 0; k < frames * ch; ++k) {
                const float v = pcm[k] * 32767.0f;
                a->i16[k] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }
            a->pkts.clear();
            a->enc.encode(a->i16.data(), frames, ch, a->pkts);
            a->n += (long)a->pkts.size();
        };
#else
        cb.audio = benchdetail::noAudio;
#endif
        L->pipe.start(chanRate, 1024, 10.0, 48000, cb);
        L->pipe.setTune(0.0, lm.mode, lm.bw);
        Lis* lp = L.get();
        L->th = std::thread([lp, &chan, centreBin, chanBins] {
            nameThread("bw-listener");
            Blk b;
            while (lp->q.pop(b)) {
                const int got = chan.extract(b.bins->data(), centreBin, chanBins, lp->slice.data(), lp->ectx, b.index);
                if (got > 0) lp->pipe.feed(lp->slice.data(), got);
            }
        });
        lis.push_back(std::move(L));
    }
    // ── the front end (vibe-dsp) ──
    std::thread front([&] {
        nameThread("bw-front");
        vibeFlushDenormals();
        vibedsp::IqCleaner clean; clean.configure(fs);
        // ★ A/B switches for finding which stage costs what (VIBE_BENCH_NOENC's siblings) — never set by a server.
        const bool noClean = std::getenv("VIBE_BENCH_NOCLEAN") != nullptr, noWide = std::getenv("VIBE_BENCH_NOWIDE") != nullptr;
        std::vector<float> row((size_t)engineN);
        const double blockRate = fs / (double)chan.hop();
        const int decim = std::max(1, (int)std::lround(blockRate / kWideRowsPerSec));
        int wideDecim = 0;
        auto makeRow = [&](const cf32* bins, int nb) {
            const int half = nb / 2; const float scale = 1.0f / ((float)nb * (float)nb);
            for (int i = 0; i < nb; ++i) {
                const cf32& c = bins[(i + half) % nb];
                row[i] = 10.0f * std::log10((c.real() * c.real() + c.imag() * c.imag()) * scale + 1e-30f) - 8.0f;
            }
        };
        // ★★ The waterfall's own transform where the listeners' FFT is not its size (the shim's wideTap_), at its rows' rate.
        std::unique_ptr<vibedsp::PeriodicFft> wideTap;
        if (engineN != fftSize) wideTap.reset(new vibedsp::PeriodicFft(engineN));
        const int tapDecim = std::max(1, (int)std::lround(fs / (double)(engineN - engineN / vibedsp::Channelizer::OVERLAP_DIV) / kWideRowsPerSec));
        std::vector<cf32> buf;
        volatile float sink = 0;
        while (iq.pop(buf)) {
            if (!noClean) clean.process(buf.data(), (int)buf.size());
            if (wideTap && !noWide)
                wideTap->feed(buf.data(), (int)buf.size(), [&] { return tapDecim; },
                              [&](const cf32* b, int nb) { makeRow(b, nb); sink = sink + row[nb / 3]; });
            chan.feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int nb) {
                if (!lis.empty()) {
                    Blk b; b.bins = std::make_shared<std::vector<cf32>>(bins, bins + nb); b.index = chan.blockIndex();
                    for (auto& L : lis) L->q.push(b);
                }
                if (!wideTap && !noWide && ++wideDecim >= decim) {
                    wideDecim = 0;
                    makeRow(bins, nb);
                    sink = sink + row[nb / 3];
                }
            });
        }
        (void)sink;
    });
    // ── the intake (vibe-usrp), paced in real time, on its own thread ──
    std::map<uint64_t, benchdec::ThreadTime> t0, t1;
    double p0 = 0, p1 = 0;
    std::thread intake([&] {
        nameThread("bw-intake");
        std::vector<float> conv(hand * 2);
        size_t pos = 0;
        const double w0 = benchdec::wallNow();
        double fed = 0; bool measuring = false;
        while (fed < warmSec + measSec) {
            if (!measuring && fed >= warmSec) {
                t0 = benchdec::threadTimes(); p0 = benchdec::processCpu(); measuring = true;
                heap1 = benchprof::heapKB(); rss1 = benchprof::rssKB();   // ★ every listener built and warmed
            }
            // UHD's job: sc16 → fc32 (the same arithmetic; its SIMD converter is faster still)
            for (size_t i = 0; i < hand; ++i) {
                const size_t k = (pos + i) % total;
                conv[2 * i] = sc16[2 * k] * (1.0f / 32768.0f); conv[2 * i + 1] = sc16[2 * k + 1] * (1.0f / 32768.0f);
            }
            pos = (pos + hand) % total;
            // enqueueIqFloat's job: a copy into the queue's own buffer
            std::vector<cf32> v(hand);
            std::memcpy(v.data(), conv.data(), hand * sizeof(cf32));
            iq.push(std::move(v));
            fed += hand / fs;
            const double ahead = fed - (benchdec::wallNow() - w0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
            else if (ahead < -0.05) F.behind = true;
        }
        t1 = benchdec::threadTimes(); p1 = benchdec::processCpu();
    });
    intake.join();
    iq.close(); front.join();
    for (auto& L : lis) { L->q.close(); L->th.join(); }
    F.iqDropped = iq.dropped;
    for (auto& L : lis) F.listenerDropped += L->q.dropped;
    F.dropped = F.iqDropped + F.listenerDropped;
    F.perListenerKB = benchprof::perListenerKB(heap0, heap1, rss0, rss1, nListeners);
    // ── the figures, read before the pipelines stop (stop() joins their workers and takes their clocks with them) ──
    (void)before;
    for (const auto& kv : t1) {
        auto a = t0.find(kv.first);
        if (a == t0.end()) continue;
        const double pct = 100.0 * (kv.second.sec - a->second.sec) / measSec;
        if (pct < 0.05) continue;
        const std::string& nm = kv.second.name;
        if (nm == "bw-intake") F.intake = pct;
        else if (nm == "bw-front") F.front = pct;
        else if (!before.count(kv.first)) {   // the listeners' threads and their pipelines' workers
            F.listenersTotal += pct;
            F.listenerHottest = std::max(F.listenerHottest, pct);
        }
        if (pct > F.hottest) { F.hottest = pct; F.hottestName = nm.empty() ? "thread" : nm; }
    }
    F.process = 100.0 * (p1 - p0) / measSec;
    F.perListener = nListeners > 0 ? F.listenersTotal / nListeners : 0;
    for (auto& L : lis) L->pipe.stop();
    return F;
}

/** The verdict's headroom: the hottest thread at or under this, and nothing dropped (green, as every row). */
constexpr double kHeadroomPct = 70.0;

struct WideVerdict {
    double hz = 0;
    double lockedHottest = -1, fixedPct = -1, perListenerPct = -1; int lockedUsers = 0; bool lockedOk = false;
    double fullHottest = -1; bool fullOk = false;
    std::string lockedThread, fullThread;
};

/** ★★★ EVERY WIDE RATE, ascending: the locked-range path with 1, 5 and 10 WFM listeners, and the full-radio path (the
 *  shared pipeline at that rate — runOne, as the standard WFM rows). Rows go into `res`; a rate past the first this
 *  machine cannot carry is not run and is listed in `notRun` with why. */
inline std::vector<WideVerdict> runWideRows(const std::vector<double>& rates, double secondsPerScenario,
                                            std::vector<Result>& res, std::vector<std::pair<std::string, std::string>>& notRun,
                                            const std::function<void(const std::string&)>& step) {
    std::vector<WideVerdict> out;
    const int Ns[] = { 1, 5, 10 };
    bool lockedStop = false, fullStop = false;
    std::string lockedStopAt, fullStopAt;
    const double meas = std::max(1.5, std::min(3.0, secondsPerScenario / 2));
    for (double fs : rates) {
        WideVerdict v; v.hz = fs;
        const std::string rw = benchhw::rateWords(fs), rid = benchhw::rateId(fs);
        // ★ The full-radio row frees its own copy before the locked rows build theirs: one test signal in memory at a time.
        if (!fullStop) {
            const std::string label = "WFM stereo @ " + rw + " (full radio)";
            if (step) step(label);
            Result r;
            {
                const auto x = benchdetail::fmStation(fs, 0.125, 200000.0);
                const double t = benchdec::rowClock();
                r = benchdetail::runOne("wfm_" + rid, label, fs, RxPipeline::Mode::WFM, 200000, x, meas, false);
                r.ms = (int)std::lround(1000.0 * (benchdec::rowClock() - t));
            }
            v.fullHottest = r.pct; v.fullThread = r.hottest; v.fullOk = r.pct <= kHeadroomPct;
            res.push_back(r);
            if (r.pct > 85.0) { fullStop = true; fullStopAt = rw; }
        } else notRun.push_back({ "wfm_" + rid, "not run — " + fullStopAt + " was already too much for one core here (full radio)" });
        if (!lockedStop) {
            const auto sc16 = fmStationSc16(fs, 0.125, fs / 4);
            for (int n : Ns) {
                const std::string id = "lr_" + rid + "_" + std::to_string(n);
                const std::string label = "Locked range @ " + rw + ", " + std::to_string(n) + " WFM listener" + (n == 1 ? "" : "s");
                if (lockedStop) { notRun.push_back({ id, "not run — the capture front end could not keep up at " + lockedStopAt }); continue; }
                if (step) step(label);
                const double t = benchdec::rowClock();
                const LockedFigures F = runLocked(fs, n, sc16, 0.5, meas);
                Result r; r.id = id; r.label = label; r.rate = fs; r.pct = F.hottest; r.hottest = F.hottestName;
                r.ms = (int)std::lround(1000.0 * (benchdec::rowClock() - t));
                r.threads = { { "intake", F.intake }, { "front end", F.front }, { "per listener", F.perListener },
                              { "listeners", F.listenersTotal }, { "process", F.process } };
                r.dropped = F.dropped + (F.behind ? 1 : 0);
                if (r.dropped > 0) r.label += " (fell behind)";
                res.push_back(r);
                const bool ok = F.hottest <= kHeadroomPct && r.dropped == 0;
                if (n == 1) {
                    v.fixedPct = std::max(F.intake, F.front); v.lockedHottest = F.hottest; v.lockedThread = F.hottestName;
                    v.lockedOk = ok;
                }
                if (n > 1 && F.perListener > 0 && v.perListenerPct < 0) v.perListenerPct = F.perListener;
                if (ok) v.lockedUsers = n;
                // ★ One listener the capture cannot carry: no more listeners, and no wider rate.
                if (n == 1 && (F.hottest > 100.0 || r.dropped > 0)) { lockedStop = true; lockedStopAt = rw; }
                else if (!ok) break;   // ★ this many listeners do not fit; more will not either (the rate is still judged)
            }
        } else for (int n : Ns) notRun.push_back({ "lr_" + rid + "_" + std::to_string(n), "not run — the capture front end could not keep up at " + lockedStopAt });
        out.push_back(v);
    }
    return out;
}

/** ★ The gate's figures from the verdicts: the highest rate at which each path kept up WITH HEADROOM, rates checked in
 *  ascending order and the first failure ending the run of passes (a rate that passes above one that failed is noise,
 *  not capacity). 0 = none passed. */
inline void gateFrom(const std::vector<WideVerdict>& vs, double& lockedHz, double& fullHz) {
    lockedHz = fullHz = 0;
    bool lStop = false, fStop = false;
    for (const auto& v : vs) {
        if (!lStop) { if (v.lockedOk) lockedHz = v.hz; else lStop = true; }
        if (!fStop) { if (v.fullOk) fullHz = v.hz; else fStop = true; }
    }
}

/** ★★★ THE CAPTURE VERDICT as JSON — `"capture":{lockedMaxHz, fullMaxHz, headroomPct, rates:[…]}` (no leading comma): the
 *  rate gate's input (vibe_benchmark_hw.h gateFromBenchJson). Written by the WIDE benchmark into its entry since
 *  2026-11-19 (the quick run is an RTL's); an older quick result carries one at the top level, which still counts. */
inline std::string captureJson(const std::vector<WideVerdict>& wideV) {
    double lmax = 0, fmax = 0;
    gateFrom(wideV, lmax, fmax);
    char b[320];
    std::snprintf(b, sizeof b, "\"capture\":{\"lockedMaxHz\":%.0f,\"fullMaxHz\":%.0f,\"headroomPct\":%.0f,\"rates\":[",
                  lmax, fmax, kHeadroomPct);
    std::string j = b;
    for (size_t k = 0; k < wideV.size(); ++k) {
        const auto& v = wideV[k];
        std::snprintf(b, sizeof b, "%s{\"hz\":%.0f,\"lockedOk\":%s,\"lockedHottestPct\":%.1f,\"lockedThread\":\"%s\","
                      "\"fixedPct\":%.1f,\"perListenerPct\":%.1f,\"lockedUsers\":%d,\"fullOk\":%s,\"fullHottestPct\":%.1f}",
                      k ? "," : "", v.hz, v.lockedOk ? "true" : "false", v.lockedHottest, v.lockedThread.c_str(),
                      v.fixedPct, v.perListenerPct, v.lockedUsers, v.fullOk ? "true" : "false", v.fullHottest);
        j += b;
    }
    return j + "]}";
}

// ── ★★★ V12 THE THREE LOCKED-RANGE PROFILES (2026-11-18 — vibe_bench_profiles.h has the rules) ─────────────────────────
//
// ★★ Each profile is the locked-range path above, thread for thread, with `profileListeners` listeners of its kind:
//    the shared path (intake + front end) is its fixed cost, the listeners' threads together / their number its cost per
//    listener, and the heap / RSS growth while they were built its memory per listener. The QUICK and WIDE benchmarks
//    both measure through here; they differ only in the rate.
// ★★ ADVANCED RDS rides on top of a WFM listener: its own cost is (WFM + Advanced RDS) − WFM, every thread of each, at
//    the same rate (runOne, the rdsx row's own way). It runs on the listener's demodulated MPX, so it does not depend on
//    the capture rate — and the locked-range loop is too short for RDS to lock, which is what the scopes need to run.

/** The profile runs' test signal: 0.125 s, at most 4 Mi samples (16 MB of sc16) at any rate — bounded on a 56 MS/s
 *  capture as on a Pi 2 (NO OOM, ever). A multiple of 4 samples: the carrier at fs/4 loops on a whole cycle. */
inline size_t profileSamples(double fs) {
    size_t n = (size_t)(fs * 0.125);
    n = std::min<size_t>(n, (size_t)1 << 22);
    return n & ~(size_t)3;
}
/** A narrowband AM two-tone at `offset`, straight to sc16 (no float copy of the whole signal at a wide rate). */
inline std::vector<int16_t> nbSignalSc16(double fs, size_t n, double offset) {
    std::vector<int16_t> o(n * 2);
    double ph = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        const double a = 0.3 * (1.0 + 0.5 * std::sin(2 * M_PI * 700.0 * t) + 0.3 * std::sin(2 * M_PI * 1700.0 * t));
        ph += 2 * M_PI * offset / fs; if (ph > 2 * M_PI) ph -= 2 * M_PI;
        o[2 * i] = (int16_t)std::lround(a * std::cos(ph) * 16000.0); o[2 * i + 1] = (int16_t)std::lround(a * std::sin(ph) * 16000.0);
    }
    return o;
}
/** How many listeners a profile run builds: one per spare core, 1 … 4 — enough to average, never enough to swamp a Pi 2. */
inline int profileListeners(int cores) { return std::max(1, std::min(4, cores - 1)); }

struct ProfileMeasure { benchprof::ProfileCost cost; benchprof::CaptureCost cap; int listeners = 0; double rate = 0; };
/** ★ One profile at `fs`: `nL` listeners of `lm` on the real locked-range path (runLocked). */
inline ProfileMeasure measureProfile(double fs, int nL, const std::vector<int16_t>& sc16, const LockedMode& lm, double meas) {
    ProfileMeasure m; m.rate = fs; m.listeners = nL;
    const LockedFigures F = runLocked(fs, nL, sc16, 0.5, meas, lm);
    m.cap.rate = fs; m.cap.intakePct = F.intake; m.cap.frontPct = F.front;
    m.cap.dropped = F.iqDropped + (F.behind ? 1 : 0); m.cap.fixedKB = F.fixedKB;
    auto& c = m.cost;
    c.perPct = nL > 0 ? F.perListener : -1;
    c.perHottestPct = F.listenerHottest;
    c.fixedPct = F.intake + F.front;
    c.fixedHottestPct = std::max(F.intake, F.front);
    c.dropped = m.cap.dropped;
    c.listenerDropped = F.listenerDropped;
    c.perKB = F.perListenerKB;
    c.fixedKB = F.fixedKB;
    return m;
}
inline double threadSum(const Result& r) { double s = 0; for (const auto& t : r.threads) s += std::max(0.0, t.second); return s; }
/** ★ Advanced RDS per listener: (WFM + Advanced RDS) − WFM at 2.048 MS/s, every thread of each (see the note above). */
inline double rdsIncrementPct(double seconds) {
    const auto x = benchdetail::fmStation(2048000.0, 1.0, 200000.0);
    const Result on = benchdetail::runOne("rdsx_inc", "", 2048000.0, RxPipeline::Mode::WFM, 200000, x, seconds, true);
    const Result off = benchdetail::runOne("wfm_inc", "", 2048000.0, RxPipeline::Mode::WFM, 200000, x, seconds, false);
    return std::max(0.0, threadSum(on) - threadSum(off));
}

/** What the three profiles came to — the quick run fills it at its rates, the wide run at the owner's. */
struct ThreeProfiles {
    bool fm = false, narrow = false, dab = false;
    double fmRate = 0, narrowRate = 0, dabRate = 0;
    int dabBlocks = 0, fmListeners = 0, narrowListeners = 0;
    benchprof::ProfileCost fmC, narrowC, dabC;
    benchprof::ProfileCost wfmC;   // ★ WFM alone — the same run, without Advanced RDS's cost (Stuart, 2026-11-19)
    std::string narrowMode;    // "nfm" | "am" | "ssb" — the heaviest, whose cost the figure is
    double rdsIncPct = -1;
    std::string dabWhy;        // ★ why DAB has no figure, in words (absent = it has one, or it does not apply here)
};
/** `"fm":{…},"narrow":{…},"dab":{…}` — only the profiles that were measured (never a line for one that was not). */
inline std::string threeProfilesJson(const ThreeProfiles& p, int cores, double availKB) {
    std::string j;
    auto add = [&](const char* k, const std::string& v) { j += std::string(j.empty() ? "" : ",") + "\"" + k + "\":" + v; };
    if (p.fm) {
        char e[96]; std::snprintf(e, sizeof e, ",\"rdsPct\":%.2f,\"listeners\":%d", p.rdsIncPct, p.fmListeners);
        add("fm", benchprof::profileJson(benchprof::profileUsers(p.fmC, cores, availKB), p.fmC, p.fmRate, e));
        /* ★★ "A figure for WFM without advanced RDS too" (Stuart, 2026-11-19): the same listeners, the same run, with
         *  Advanced RDS's own cost left out — what a server holds when its listeners leave Advanced RDS off. Not "wfm":
         *  lockedUsers.wfm is the older listener count every app still reads. */
        char e2[48]; std::snprintf(e2, sizeof e2, ",\"listeners\":%d", p.fmListeners);
        add("fmPlain", benchprof::profileJson(benchprof::profileUsers(p.wfmC, cores, availKB), p.wfmC, p.fmRate, e2));
    }
    if (p.narrow) {
        char e[96]; std::snprintf(e, sizeof e, ",\"mode\":\"%s\",\"listeners\":%d", p.narrowMode.c_str(), p.narrowListeners);
        add("narrow", benchprof::profileJson(benchprof::profileUsers(p.narrowC, cores, availKB), p.narrowC, p.narrowRate, e));
    }
    if (p.dab) {
        char e[48]; std::snprintf(e, sizeof e, ",\"blocks\":%d", p.dabBlocks);
        add("dab", benchprof::profileJson(benchprof::profileUsers(p.dabC, cores, availKB), p.dabC, p.dabRate, e));
    } else if (!p.dabWhy.empty()) add("dabWhy", "\"" + benchprof::jsonEsc(p.dabWhy) + "\"");
    return j;
}

/** ★★ THE FM AND NARROW PROFILES AT `fmRate` / `narrowRate` (0 = that profile does not apply here). `narrowModes` = the
 *  modes to try, heaviest kept (the quick run passes the one its lk_ rows found heaviest; the wide run all three).
 *  `rdsInc` ≥ 0 = Advanced RDS's cost already known (the quick run's own rows); else it is measured. Steps named as they
 *  start; the capture's cost at each rate run goes into `caps` (DAB at the same rate reuses it). */
inline void measureFmNarrow(ThreeProfiles& P, double fmRate, double narrowRate, const std::vector<std::string>& narrowModes,
                            double rdsInc, int cores, double meas, std::vector<benchprof::CaptureCost>& caps,
                            const std::function<void(const std::string&)>& step) {
    const int nL = profileListeners(cores);
    if (fmRate > 0) {
        if (step) step("Locked range: WFM listeners @ " + benchhw::rateWords(fmRate));
        ProfileMeasure m;
        { const auto x = fmStationSc16(fmRate, profileSamples(fmRate) / fmRate, fmRate / 4); m = measureProfile(fmRate, nL, x, LockedMode(), meas); }
        if (step) step("Advanced RDS, per listener");
        P.rdsIncPct = rdsInc >= 0 ? rdsInc : rdsIncrementPct(std::max(1.5, meas));
        P.fm = true; P.fmRate = fmRate; P.fmListeners = nL; P.fmC = m.cost; P.wfmC = m.cost;
        P.fmC.perPct += P.rdsIncPct;
        P.fmC.perHottestPct += P.rdsIncPct;   // ★ conservative: as if Advanced RDS shared the listener's hottest thread
        caps.push_back(m.cap);
    } else if (step) { step(""); step(""); }
    if (narrowRate > 0 && !narrowModes.empty()) {
        const auto x = nbSignalSc16(narrowRate, profileSamples(narrowRate), narrowRate / 4);
        bool first = true;
        for (const auto& md : narrowModes) {
            const LockedMode lm = md == "am" ? LockedMode{ RxPipeline::Mode::AM, 10000 }
                                : md == "ssb" ? LockedMode{ RxPipeline::Mode::SSB_USB, 2700 }
                                : LockedMode{ RxPipeline::Mode::NFM, 12500 };
            if (step) step("Locked range: " + std::string(md == "am" ? "AM" : md == "ssb" ? "SSB" : "NFM") + " listeners @ " + benchhw::rateWords(narrowRate));
            const ProfileMeasure m = measureProfile(narrowRate, nL, x, lm, meas);
            if (first || m.cost.perPct > P.narrowC.perPct) { P.narrowC = m.cost; P.narrowMode = md; }
            if (first) caps.push_back(m.cap);
            first = false;
        }
        P.narrow = true; P.narrowRate = narrowRate; P.narrowListeners = nL;
    }
}
/** ★ The capture's own cost at `fs`: from a profile already run at that rate, else a run with no listeners. */
inline benchprof::CaptureCost captureAt(double fs, double meas, std::vector<benchprof::CaptureCost>& caps) {
    for (const auto& c : caps) if (std::fabs(c.rate - fs) < 1.0 && c.ok()) return c;
    const auto x = nbSignalSc16(fs, profileSamples(fs), fs / 4);
    const ProfileMeasure m = measureProfile(fs, 0, x, LockedMode{ RxPipeline::Mode::NFM, 12500 }, meas);
    caps.push_back(m.cap);
    return m.cap;
}
/** ★★ THE DAB PROFILE at `rate` from `d` (vibe_bench_profiles.h dabCost): the blocks that rate holds, every one decoding,
 *  a station per listener. Two blocks or more ride the capture's shared path, measured here (captureAt). */
inline void dabProfile(ThreeProfiles& P, double rate, const benchprof::DabInputs& d, double meas,
                       std::vector<benchprof::CaptureCost>& caps) {
    const int blocks = benchprof::dabBlocksAt(rate);
    if (blocks < 1) return;
    if (!d.ok()) return;
    benchprof::CaptureCost cap;
    if (blocks >= 2) cap = captureAt(rate, meas, caps);
    P.dabC = benchprof::dabCost(d.perStation, d.muxFront, d.muxHottest, d.ddc8, d.bank.ok() ? &d.bank : nullptr, rate, blocks, cap, d.stationKB);
    if (!(P.dabC.perPct > 0)) return;
    P.dab = true; P.dabBlocks = blocks;
    P.dabRate = blocks >= 2 ? rate : 2048000.0;   // ★ one block = the ordinary DAB mode, at its own rate
}

}  // namespace benchwide
}  // namespace vibe
