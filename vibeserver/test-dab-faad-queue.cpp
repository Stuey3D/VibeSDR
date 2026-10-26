// test-dab-faad-queue.cpp — DAB+ through FAAD2 ON ITS OWN THREAD (vibe-dabaac, AacDecoderFaadAsync in vibe_dab_faad.h).
// V12, 2026-10-26 — Stuart's Pi 2 broke up with FAAD2 decoding inline on the DAB worker; this moves it to its own thread
// behind a bounded drop-never-wait ring. Checked here, on a REAL off-air unit log (~/VibeSDR-test-audio/dab/logs):
//   1. SAME AUDIO — the threaded decoder, fed with back-pressure so nothing drops, produces exactly the samples the inline
//      decoder does for the same sequence of units and holes (bit-identical: it is the same decoder, in the same order).
//   2. A FULL RING NEVER BLOCKS — with the worker held, 200 units are submitted: every call returns at once (worst call
//      timed), the ones that do not fit are counted as dropped, and once the worker runs again the output is EXACTLY as
//      long as the programme (each dropped unit concealed in its place — the clock stays whole, nothing is spliced).
//   3. A SERVICE CHANGE — jobs queued before reset() are never run; the sink sees only the new generation; a fresh decoder.
//   4. THE LIVE SWITCH, BOTH WAYS — the AacDecoder the service drives (dabAacChoice): FAAD2 → FFmpeg → FAAD2, async()
//      following the switch, audio from each (FFmpeg only where it is installed), the FAAD2 thread reused, not leaked.
//   5. THE THREAD IS NAMED vibe-dabaac and is not the caller's.
// Exit 0 pass, 1 fail, 3 not run (no FAAD2 in the build, or no logs).
//   run: test-dab-faad-queue [<log.dabu>]
#include "vibe_dab_aacdec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#if !defined(VIBE_HAVE_FAAD2_DAB)
int main() { std::printf("   NOT RUN — no plain FAAD2 (libfaad_dab) in this build\n"); return 3; }
#else
using namespace std::chrono;
static int fails = 0, passes = 0;
static void check(bool c, const char* what) { std::printf("   %s %s\n", c ? "ok  " : "FAIL", what); c ? ++passes : ++fails; }

struct Slot { uint32_t sf = 0; int slot = 0, slots = 0; std::vector<uint8_t> adts; };
static bool loadLog(const std::string& path, std::vector<Slot>& L) {
    FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
    char m[8]; if (std::fread(m, 1, 8, f) != 8 || std::memcmp(m, "VIBEDABU", 8) != 0) { std::fclose(f); return false; }
    uint32_t sf; uint8_t h[4]; uint16_t len;
    while (std::fread(&sf, 4, 1, f) == 1 && std::fread(h, 1, 4, f) == 4 && std::fread(&len, 2, 1, f) == 1) {
        Slot s; s.sf = sf; s.slot = h[0]; s.slots = h[1]; s.adts.resize(len);
        if (len && std::fread(s.adts.data(), 1, len, f) != len) break;
        L.push_back(std::move(s));
    }
    std::fclose(f);
    return !L.empty();
}

/** The PCM the async decoder hands back, with the generation of every block. */
struct Collect {
    std::mutex m; std::vector<float> pcm; std::vector<uint32_t> gens; std::string thread;
    void operator()(uint32_t g, const float* p, size_t n) {
        std::lock_guard<std::mutex> lk(m);
        if (thread.empty()) { char nm[32] = {}; pthread_getname_np(pthread_self(), nm, sizeof nm); thread = nm; }
        pcm.insert(pcm.end(), p, p + n); gens.push_back(g);
    }
    size_t frames() { std::lock_guard<std::mutex> lk(m); return pcm.size() / 2; }
};

/** Drive one decoder with the log as DabService does: each slot either a unit or (empty) a hole of one unit. */
template <class D, class Wait> static void drive(D& d, const std::vector<Slot>& L, size_t n, vibedab::AacPcm& p, Wait wait) {
    for (size_t i = 0; i < n && i < L.size(); ++i) {
        if (L[i].adts.empty()) d.conceal(1, p); else d.decode(L[i].adts.data(), L[i].adts.size(), p);
        wait();
    }
}
static void waitIdle(vibedab::AacDecoderFaadAsync& a) {
    for (int i = 0; i < 4000 && (a.queuedJobs() > 0 || a.inFlightFrames() > 0); ++i) std::this_thread::sleep_for(microseconds(250));
}

int main(int argc, char** argv) {
    std::string path;
    if (argc > 1) path = argv[1];
    else {
        const char* h = getenv("HOME"); const std::string dir = std::string(h ? h : "") + "/VibeSDR-test-audio/dab/logs";
        // ★ a damaged log: holes (CRC failures) as well as units, so concealment is in the sequence
        for (const char* c : { "12Bx-snr15-fd2-C229.dabu", "12B-C229.dabu" }) { const std::string p = dir + "/" + c; if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); path = p; break; } }
    }
    std::vector<Slot> L;
    if (path.empty() || !loadLog(path, L)) { std::printf("   NOT RUN — no unit log (~/VibeSDR-test-audio/dab/logs)\n"); return 3; }
    size_t holes = 0; for (auto& s : L) holes += s.adts.empty();
    std::printf("── %s: %zu slots, %zu holes, %d units per super frame\n", path.c_str(), L.size(), holes, L.front().slots);
    const size_t N = std::min<size_t>(L.size(), 600);

    // ── 1. the same audio as inline ──
    std::printf("── 1. threaded = inline, sample for sample\n");
    {
        vibedab::AacDecoderFaad inl; vibedab::AacPcm a; drive(inl, L, N, a, [] {});
        vibedab::AacDecoderFaadAsync as; Collect col; as.setSink(std::ref(col)); vibedab::AacPcm unused;
        size_t i = 0;
        drive(as, L, N, unused, [&] { ++i; while (as.queuedJobs() > 8) std::this_thread::sleep_for(microseconds(200)); });
        waitIdle(as);
        size_t diff = 0; const size_t n = std::min(a.interleaved.size(), col.pcm.size());
        for (size_t k = 0; k < n; ++k) diff += a.interleaved[k] != col.pcm[k];
        std::printf("   inline %zu frames, threaded %zu frames, %zu samples differ; dropped %llu\n", a.interleaved.size() / 2, col.pcm.size() / 2, diff, (unsigned long long)as.dropped());
        check(a.interleaved.size() == col.pcm.size() && diff == 0, "bit-identical to the inline decoder");
        check(as.dropped() == 0, "nothing dropped when the producer keeps pace");
        check(col.thread == "vibe-dabaac", ("decoded on a thread named vibe-dabaac (\"" + col.thread + "\")").c_str());
        check(as.concealed() == inl.concealed(), "the same units concealed");
        std::printf("   CPU: decode %.3f s, resample %.3f s, total %.3f s for %.1f s of programme\n", as.cpuDecodeSeconds(), as.cpuResampleSeconds(), as.cpuSeconds(), double(col.pcm.size() / 2) / 48000.0);
    }

    // ── 2. a full ring never blocks; drops are concealed in place ──
    std::printf("── 2. full ring: drop, never wait\n");
    {
        vibedab::AacDecoderFaadAsync as; Collect col; as.setSink(std::ref(col)); vibedab::AacPcm unused;
        // learn the unit length first (a decoded unit)
        // ★ FAAD2 returns nothing for an HE-AAC stream's FIRST unit (its SBR start-up) — a few, so one has produced audio
        size_t k = 0;
        for (int got = 0; k < L.size() && got < 4; ++k) if (!L[k].adts.empty()) { as.decode(L[k].adts.data(), L[k].adts.size(), unused); waitIdle(as); ++got; }
        const int uf = as.unitFrames();
        check(uf > 0, ("unit length known after one unit (" + std::to_string(uf) + " frames)").c_str());
        const size_t base = col.frames();
        as.setPausedForTest(true);
        const int M = 200; double worst = 0;
        for (int i = 0; i < M; ++i) {
            const Slot& s = L[(k + 1 + size_t(i)) % L.size()];
            const auto t0 = steady_clock::now();
            if (s.adts.empty()) as.conceal(1, unused); else as.decode(s.adts.data(), s.adts.size(), unused);
            worst = std::max(worst, duration<double, std::micro>(steady_clock::now() - t0).count());
        }
        const uint64_t dropped = as.dropped();
        std::printf("   %d units in with the worker held: %d jobs queued, %llu dropped, worst call %.1f us\n", M, as.queuedJobs(), (unsigned long long)dropped, worst);
        check(worst < 2000.0, "no submit waited (worst call under 2 ms, with the worker held)");
        check(dropped > 0 && as.queuedJobs() <= vibedab::AacDecoderFaadAsync::kSlots, "the ring is bounded and the overflow counted as dropped");
        // ★ in flight covers everything owed: queued + carried — so the service's clock never fills it twice
        check(as.inFlightFrames() == uint64_t(M) * uint64_t(uf), ("in-flight = every unit owed (" + std::to_string(as.inFlightFrames()) + " frames)").c_str());
        const int64_t conc0 = as.concealed();
        as.setPausedForTest(false);
        for (int i = 0; i < 4000 && as.queuedJobs() > 0; ++i) std::this_thread::sleep_for(microseconds(250));
        // one more unit carries the remaining owed units with it
        const Slot& s = L[(k + 1 + M) % L.size()];
        if (s.adts.empty()) as.conceal(1, unused); else as.decode(s.adts.data(), s.adts.size(), unused);
        waitIdle(as);
        const size_t got = col.frames() - base;
        std::printf("   after: %zu frames for %d units (%d each); concealed +%lld\n", got, M + 1, uf, (long long)(as.concealed() - conc0));
        check(got == size_t(M + 1) * size_t(uf), "the output is exactly as long as the programme — every dropped unit concealed in its place");
        check(as.concealed() - conc0 >= int64_t(dropped), "each dropped unit was concealed");
    }

    // ── 3. a service change mid-queue ──
    std::printf("── 3. reset with jobs queued\n");
    {
        vibedab::AacDecoderFaadAsync as; Collect col; as.setSink(std::ref(col)); vibedab::AacPcm unused;
        drive(as, L, 20, unused, [&] { while (as.queuedJobs() > 4) std::this_thread::sleep_for(microseconds(200)); }); waitIdle(as);
        const uint32_t g0 = as.generation();
        as.setPausedForTest(true);
        drive(as, L, 6, unused, [] {});                  // old service's units, queued
        as.reset();
        const uint32_t g1 = as.generation();
        check(as.unitFrames() == 0 && as.inFlightFrames() == 6u * 2880u, "reset forgets the unit length at once (in flight still counted until skipped)");
        size_t before; { std::lock_guard<std::mutex> lk(col.m); before = col.gens.size(); }
        as.setPausedForTest(false);
        drive(as, L, 40, unused, [&] { while (as.queuedJobs() > 4) std::this_thread::sleep_for(microseconds(200)); });   // the new service
        waitIdle(as);
        bool onlyNew = true; size_t blocks = 0;
        { std::lock_guard<std::mutex> lk(col.m); for (size_t i = before; i < col.gens.size(); ++i) { onlyNew &= col.gens[i] == g1; ++blocks; } }
        std::printf("   generation %u → %u; %zu blocks after the reset; decoded %lld, dropped %llu\n", g0, g1, blocks, (long long)as.decoded(), (unsigned long long)as.dropped());
        check(g1 != g0 && onlyNew && blocks >= 20, "jobs queued before the reset never ran; only the new generation reached the sink");
        check(as.inFlightFrames() == 0, "nothing left in flight");
    }

    // ── 4. the live switch, both ways, through the decoder DabService drives ──
    std::printf("── 4. the owner's live switch: FAAD2 → FFmpeg → FAAD2\n");
    {
        const bool ff = vibedab::AacDecoderFfmpegAvailable();
        vibedab::dabAacChoice().store(1);
        vibedab::AacDecoder d; Collect col; d.setSink(std::ref(col)); vibedab::AacPcm p;
        d.reset();
        check(d.async() && std::string(d.backend()) == "FAAD2", "FAAD2 chosen: async, on its own thread");
        for (size_t i = 0; i < 40; ++i) if (!L[i].adts.empty()) d.decode(L[i].adts.data(), L[i].adts.size(), p);
        for (int i = 0; i < 400 && d.inFlightFrames(); ++i) std::this_thread::sleep_for(milliseconds(5));
        const size_t f1 = col.frames();
        check(f1 > 0 && p.interleaved.empty(), "FAAD2's audio came through the sink, not the call");
        vibedab::dabAacChoice().store(0);
        check(d.switchPending(), "switching to FFmpeg is pending");
        d.reset();
        check(!d.async() && !d.switchPending(), "FFmpeg now: decodes in the call");
        if (ff) {
            for (size_t i = 0; i < 120 && p.interleaved.empty(); ++i) { if (!L[i].adts.empty()) d.decode(L[i].adts.data(), L[i].adts.size(), p); std::this_thread::sleep_for(milliseconds(10)); d.drain(p); }
            check(!p.interleaved.empty(), "FFmpeg returned audio");
        } else std::printf("   (no ffmpeg on this machine — its half not run)\n");
        check(col.frames() == f1, "FAAD2's thread sent nothing while FFmpeg played");
        vibedab::dabAacChoice().store(1);
        check(d.switchPending(), "and back: switching to FAAD2 is pending");
        d.reset();
        for (size_t i = 0; i < 40; ++i) if (!L[i].adts.empty()) d.decode(L[i].adts.data(), L[i].adts.size(), p);
        for (int i = 0; i < 400 && d.inFlightFrames(); ++i) std::this_thread::sleep_for(milliseconds(5));
        check(d.async() && col.frames() > f1, "FAAD2 again, audio through the sink");
        d.stop(); d.close();
        vibedab::dabAacChoice().store(0);
    }
    std::printf("\ntest-dab-faad-queue: %d checks, %d failed\n", passes + fails, fails);
    return fails ? 1 : 0;
}
#endif
