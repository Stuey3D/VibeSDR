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
    const double need = std::max(bwHz * 2.5, 24000.0);
    int b = 64;
    while (b < fftSize && rate * (double)b / (double)fftSize < need) b <<= 1;
    return std::min(b, fftSize);
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
    long dropped = 0;             // IQ buffers + listener blocks lost — anything above 0 = it did not keep up
    bool behind = false;          // the intake could not keep real time (its own pace slipped)
};

/** ★★★ THE LOCKED-RANGE PATH AT `fs` WITH `nListeners` WFM LISTENERS, in real time — see the file note. */
inline LockedFigures runLocked(double fs, int nListeners, const std::vector<int16_t>& sc16, double warmSec, double measSec) {
    LockedFigures F;
    const int fftSize = fftSizeForRate(fs);
    const int chanBins = chanBinsFor(fs, fftSize, 200000.0);
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
        L->pipe.setTune(0.0, RxPipeline::Mode::WFM, 200000);
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
        std::vector<float> row((size_t)fftSize);
        const double blockRate = fs / (double)chan.hop();
        const int decim = std::max(1, (int)std::lround(blockRate / kWideRowsPerSec));
        int wideDecim = 0;
        std::vector<cf32> buf;
        volatile float sink = 0;
        while (iq.pop(buf)) {
            if (!noClean) clean.process(buf.data(), (int)buf.size());
            chan.feed(buf.data(), (int)buf.size(), [&](const cf32* bins, int nb) {
                if (!lis.empty()) {
                    Blk b; b.bins = std::make_shared<std::vector<cf32>>(bins, bins + nb); b.index = chan.blockIndex();
                    for (auto& L : lis) L->q.push(b);
                }
                if (!noWide && ++wideDecim >= decim) {
                    wideDecim = 0;
                    const int half = nb / 2; const float scale = 1.0f / ((float)nb * (float)nb);
                    for (int i = 0; i < nb; ++i) {
                        const cf32& c = bins[(i + half) % nb];
                        row[i] = 10.0f * std::log10((c.real() * c.real() + c.imag() * c.imag()) * scale + 1e-30f) - 8.0f;
                    }
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
            if (!measuring && fed >= warmSec) { t0 = benchdec::threadTimes(); p0 = benchdec::processCpu(); measuring = true; }
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
    F.dropped = iq.dropped;
    for (auto& L : lis) F.dropped += L->q.dropped;
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
        else if (!before.count(kv.first)) F.listenersTotal += pct;   // the listeners' threads and their pipelines' workers
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

}  // namespace benchwide
}  // namespace vibe
