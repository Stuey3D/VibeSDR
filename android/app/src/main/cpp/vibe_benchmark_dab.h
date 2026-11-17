// VibeSDR — the benchmark's DAB rows (Stuart, 2026-09-19). See vibe_benchmark.h for the method.
//
// ★★★ A REAL MULTIPLEX, NOT A SYNTHETIC ONE. A DAB receiver's cost is sync, FIC, Viterbi and the audio
//     decoder working on a real ensemble; a made-up OFDM signal would decode nothing and measure the idle path.
//     So the input is a real multiplex built from generated audio and made-up labels (vibeserver/bench-clip/ —
//     nothing broadcast, nothing anyone else owns), damaged like a weak signal, as 8-bit IQ the way an RTL delivers
//     it — dab-bench.vbu8, fetched from GitHub on the first run so the APK and the .deb stay small.
// ★★ 60 frames (5.76 s — station text and slides need seconds to complete), exactly: a whole number of 96 ms frames AND of 5-CIF DAB+ superframes, so the loop is seamless in
//    timing and the superframe count does not break at the seam.
// ★★ FED IN REAL TIME, not as fast as it will take it. DabService drops the OLDEST IQ when its worker falls behind
//    (feed(): inDropped_) — flooded, a slow box would throw most of the signal away and read CHEAPER than it is.
// ★★ DabService, not DabReceiver: the service is what the server runs — the MSC split, vibe-mp2, the PAD label
//    scanner — and the scanner is exactly the cost the two rows separate.
// ★ DAB+ AAC is decoded by FAAD2 on its own thread (vibe-faad2); the rows say "receiver" because they measure the
//   receiver's own threads.
//
// File: magic "VIBEBU81", double rate, double centreHz, uint32 sid (a DAB+ service in the clip), then u8 I/Q pairs.
#pragma once
#include "vibe_benchmark.h"
#include "vibe_dab_service.h"
#include "opus_audio_encoder.h"
#include "vibe_dab_bank.h"         // ★★ V12 §11.1: the wide-capture filter bank's own row   // ★★ a station's cost includes its ONE Opus encode (the coordinator's Pi 2 finding, 2026-11-17)
#include <memory>
#include <thread>
#include <atomic>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if !defined(__ANDROID__)
// ★ vibeserver/proc.h — fork+execvp, no shell. Every desktop includer links vibeserver_core, which
//   builds proc.cpp and puts vibeserver/ on the include path. Android never downloads (see below).
#include "proc.h"
#endif

namespace vibe {

struct DabClip { double rate = 0, centre = 0; uint32_t sid = 0; std::vector<float> iq; };

/** Load a VIBEBU81 clip as interleaved floats, as the server hands DabService its IQ. False if unreadable. */
inline bool loadDabClip(const std::string& path, DabClip& c) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[8] = {0};
    bool ok = std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, "VIBEBU81", 8) == 0
           && std::fread(&c.rate, sizeof c.rate, 1, fp) == 1 && std::fread(&c.centre, sizeof c.centre, 1, fp) == 1
           && std::fread(&c.sid, sizeof c.sid, 1, fp) == 1;
    std::vector<uint8_t> u;
    if (ok) {
        uint8_t b[65536]; size_t n;
        while ((n = std::fread(b, 1, sizeof b, fp)) > 0) u.insert(u.end(), b, b + n);
    }
    std::fclose(fp);
    if (!ok || u.size() < size_t(c.rate)) return false;          // ★ under a second: a truncated download
    c.iq.resize(u.size());
    for (size_t i = 0; i < u.size(); ++i) c.iq[i] = (float(u[i]) - 127.5f) / 127.5f;
    return true;
}

/** One DAB row: the clip looped through a fresh DabService for `seconds` of real time after `warm` seconds to
 *  lock, find the service and start its audio. Scored like the others — the hottest thread vs one core — and
 *  -1 if the chain never decoded a superframe (a row that decoded nothing measured nothing). */
inline benchdetail::Result runDabRow(const std::string& id, const std::string& label, const DabClip& clip,
                                     bool scanLabels, double seconds, double warm = 6.0) {
    using namespace benchdetail;
    Result r; r.id = id; r.label = label; r.rate = clip.rate;
    auto& scan = vibedab::dabScanLabels();
    const int scanWas = scan.load();
    scan.store(scanLabels ? 1 : 0);
    const auto before = threadCpu();
    const double selfBefore = threadSelfCpu();
    auto svc = std::make_unique<vibedab::DabService>();
    svc->setRfRate(clip.rate);
    svc->setRfCentre(clip.centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - clip.centre) < 1000.0) { svc->setChannel(i); break; }
    svc->setService(clip.sid);
    const size_t blk = size_t(clip.rate / 20) * 2;                 // 50 ms of interleaved I/Q per feed
    const size_t total = clip.iq.size();
    size_t pos = 0;
    std::vector<float> pcm(48000);
    std::map<long, double> mid; double selfMid = 0; bool measuring = false;
    std::string droppedAtMid;
    const auto t0 = std::chrono::steady_clock::now();
    double fedSecs = 0;
    for (;;) {
        if (!measuring && fedSecs >= warm) { mid = threadCpu(); selfMid = threadSelfCpu(); measuring = true;
            droppedAtMid = svc->json(); }
        if (fedSecs >= warm + seconds) break;
        const size_t n = std::min(blk, total - pos);
        svc->feed(clip.iq.data() + pos, n / 2);
        pos = (pos + n) % total;
        fedSecs += double(n / 2) / clip.rate;
        while (svc->takePcm(pcm.data(), pcm.size() / 2) > 0) {}    // ★ a listener drains it; so do we
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fedSecs * 1e6)));
    }
    const auto after = threadCpu();
    const double selfEnd = threadSelfCpu();
    std::map<long, std::string> names;
    for (const auto& kv : after) if (!before.count(kv.first)) names[kv.first] = threadName(kv.first);
    const std::string js = svc->json();
    svc.reset();                                                   // ★ AFTER the reads — see runOne
    scan.store(scanWas);
    unsigned sfOk = 0;
    if (const char* p = std::strstr(js.c_str(), "\"sfOk\":")) sfOk = unsigned(std::strtoul(p + 7, nullptr, 10));
    // The feeding thread plays vibe-dsp: it runs the 2.4 → 2.048 converter when the rate asks for it.
    double best = (selfEnd - selfMid) / seconds; r.hottest = "vibe-dsp";
    r.threads.push_back({ "vibe-dsp", 100.0 * best });
    (void)selfBefore;
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;
        auto m = mid.find(kv.first);
        const double used = (kv.second - (m != mid.end() ? m->second : 0.0)) / seconds;
        r.threads.push_back({ names[kv.first], 100.0 * used });
        if (used > best) { best = used; r.hottest = names[kv.first]; }
    }
    r.pct = sfOk > 0 ? 100.0 * best : -1.0;
    // ★ VIBE_BENCH_DEBUG=1: the decoder's own counters — how the clip's damage is checked (RS must be fixing).
    if (std::getenv("VIBE_BENCH_DEBUG")) {
        auto num = [&](const char* k) { const std::string key = std::string("\"") + k + "\":";
            const char* p = std::strstr(js.c_str(), key.c_str()); return p ? std::strtoul(p + key.size(), nullptr, 10) : 0UL; };
        // ★★ DROPPED IN THE MEASURED WINDOW, not since the start: IQ thrown away is work not done, and a row that
        //    dropped is a row that read cheap. The warm-up's own drops (cold caches, first allocations) are not the
        //    measurement's, so the figure at the start of the window is subtracted.
        unsigned long dropWarm = 0;
        if (const char* p = std::strstr(droppedAtMid.c_str(), "\"dropped\":")) dropWarm = std::strtoul(p + 10, nullptr, 10);
        std::fprintf(stderr, "[%s] sfTried %lu sfOk %lu rsFixed %lu rsLost %lu aacDecoded %lu aus %lu dropped %lu (warm-up %lu)\n",
                     id.c_str(), num("sfTried"), num("sfOk"), num("rsFixed"), num("rsLost"), num("aacDecoded"),
                     num("aus"), num("dropped") - dropWarm, dropWarm);
    }
    return r;
}

/** ★ Resident memory now, KB (0 = unknown). For the own-station row's per-station figure. */
inline long dabBenchRssKB() {
#if defined(__APPLE__)
    mach_task_basic_info info{}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS) return long(info.resident_size / 1024);
    return 0;
#elif defined(__linux__)
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; std::fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
#else
    return 0;
#endif
}

/** ★★ V12 OWN STATION (docs/v12/DAB-OWN-STATION.md): WHAT ONE MORE STATION COSTS. The same multiplex as the `dab` row
 *  through a fresh DabService, plus K = 3 STATIONS on three other services (48 k DAB+, as most real stations are) fed by
 *  its tap — what the server runs when three more listeners each pick their own. Fed in real time like the `dab` row.
 *  The figure is (every thread this run − every thread of the `dab` row) / K: one station's own decode (vibe-dab-st) and
 *  its FAAD2 (vibe-faad2), % of a core. -1 if a station decoded nothing. Memory: RSS after warm-up minus before the
 *  stations opened, per station (dabStationMemKB).
 *  ★★★ AND ITS ONE OPUS ENCODE (2026-11-17, live on the Pi 2: five own stations on 10C saturated the box — each station's
 *      stereo encode is ~12 % of a Cortex-A7 core and the row had left it out, so N read ~2× too high). Each station here
 *      gets its own `vibe-dab-enc` thread taking 20 ms of its PCM and encoding it at the server's stereo bitrate, as the
 *      server's per-station encoder does — so "one more station" is the decode AND what it costs to send. */
inline benchdetail::Result runDabStationRow(const DabClip& clip, const benchdetail::Result& dabRow, double seconds, double warm = 6.0) {
    using namespace benchdetail;
    Result r; r.id = "dec_dab_station"; r.label = "DAB+: each extra station (own station per listener)"; r.rate = clip.rate;
    auto& scan = vibedab::dabScanLabels();
    const int scanWas = scan.load();
    scan.store(0);
    const auto before = threadCpu();
    auto svc = std::make_unique<vibedab::DabService>();
    svc->setRfRate(clip.rate);
    svc->setRfCentre(clip.centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - clip.centre) < 1000.0) { svc->setChannel(i); break; }
    svc->setService(clip.sid);
    const long rss0 = dabBenchRssKB();
    static constexpr int K = 3;
    // ★ The bench clip's services are C001…C00B (vibeserver/bench-clip/make-clip.sh); three that are not the played one.
    std::vector<std::shared_ptr<vibedab::DabService>> st;
    for (uint32_t s = 0xC001; s <= 0xC00B && int(st.size()) < K; ++s) if (s != clip.sid) st.push_back(svc->openStation(s));
    const size_t blk = size_t(clip.rate / 20) * 2;
    size_t pos = 0;
    std::vector<float> pcm(48000);
    // ★★ each station's encoder, on its own thread, 20 ms at a time — the server's shape (vibe-dab-enc)
    std::atomic<bool> encRun{true};
    std::vector<std::thread> enc;
#ifdef VIBE_HAVE_OPUS
    for (auto& s : st) if (s) enc.emplace_back([&encRun, sp = s] {
        vibeThreadName("vibe-dab-enc");
        vibe::OpusAudioEncoder opus; opus.setBitrate(96000);   // ★ the server's default stereo rate (64 k × 3/2, opusBitrateFor)
        std::vector<float> f(960 * 2); std::vector<int16_t> q(960 * 2); std::vector<std::vector<uint8_t>> pk;
        auto next = std::chrono::steady_clock::now();
        while (encRun.load()) {
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
            const size_t got = sp->takePcm(f.data(), 960);
            for (size_t i = 0; i < 960 * 2; ++i) { const float v = i < got * 2 ? std::max(-1.0f, std::min(1.0f, f[i])) : 0.0f; q[i] = int16_t(std::lround(v * 32767.0f)); }
            pk.clear(); opus.encode(q.data(), 960, 2, pk);
        }
    });
#endif
    std::map<long, double> mid; bool measuring = false; long rssWarm = 0;
    const auto t0 = std::chrono::steady_clock::now();
    double fedSecs = 0;
    for (;;) {
        if (!measuring && fedSecs >= warm) { mid = threadCpu(); measuring = true; rssWarm = dabBenchRssKB(); }
        if (fedSecs >= warm + seconds) break;
        const size_t n = std::min(blk, clip.iq.size() - pos);
        svc->feed(clip.iq.data() + pos, n / 2);
        pos = (pos + n) % clip.iq.size();
        fedSecs += double(n / 2) / clip.rate;
        while (svc->takePcm(pcm.data(), pcm.size() / 2) > 0) {}
        if (enc.empty()) for (auto& s : st) if (s) while (s->takePcm(pcm.data(), pcm.size() / 2) > 0) {}
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fedSecs * 1e6)));
    }
    const auto after = threadCpu();
    encRun.store(false);
    for (auto& t : enc) t.join();
    unsigned sfMin = ~0u; int open = 0;
    for (auto& s : st) {
        if (!s) continue;
        ++open;
        const std::string js = s->json();
        unsigned v = 0; if (const char* p = std::strstr(js.c_str(), "\"sfOk\":")) v = unsigned(std::strtoul(p + 7, nullptr, 10));
        sfMin = std::min(sfMin, v);
    }
    for (auto& s : st) if (s) svc->closeStation(s);
    st.clear();
    svc.reset();
    scan.store(scanWas);
    double total = 0, stThreads = 0, faad = 0, encT = 0;
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;
        auto m = mid.find(kv.first);
        const double used = 100.0 * (kv.second - (m != mid.end() ? m->second : 0.0)) / seconds;
        total += used;
        const std::string nm = threadName(kv.first);
        if (nm.rfind("vibe-dab-st", 0) == 0) stThreads += used;
        if (nm.rfind("vibe-faad2", 0) == 0) faad += used;
        if (nm.rfind("vibe-dab-enc", 0) == 0) encT += used;
    }
    double dabTotal = 0;
    for (const auto& t : dabRow.threads) if (t.first != "vibe-dsp") dabTotal += t.second;   // ★ the feeder is not the receiver
    const double per = std::max(0.0, total - dabTotal) / K;
    r.pct = (open == K && sfMin != ~0u && sfMin > 0) ? per : -1.0;
    r.hottest = "per station";
    r.threads.push_back({ "per station", per });
    r.threads.push_back({ "vibe-dab-st (each)", stThreads / K });
    r.threads.push_back({ "vibe-faad2 (all)", faad });
    r.threads.push_back({ "vibe-dab-enc (each)", encT / K });
    const long memPer = (rssWarm > 0 && rss0 > 0) ? std::max(0L, (rssWarm - rss0) / K) : -1;
    vibedab::own::benchMemKB().store(memPer);
    if (std::getenv("VIBE_BENCH_DEBUG"))
        std::fprintf(stderr, "[dec_dab_station] per station %.2f %% (total %.1f − dab %.1f) / %d; vibe-dab-st each %.2f %%; vibe-dab-enc each %.2f %%; mem %ld KB each; sfOk min %u\n",
                     per, total, dabTotal, K, stThreads / K, encT / K, memPer, sfMin == ~0u ? 0 : sfMin);
    return r;
}

/** ★★ V12 DAB PHASE 2: WHAT ONE MORE MULTIPLEX COSTS in a wide locked range (Stuart, 2026-11-17: "the cap should be based on
 *  the hardware running the server, so build it into the benchmark"). Two parts:
 *   · its DDC — one block cut out of an 8 MS/s capture down to 2.048 MS/s (vibe_dab_ddc.h, what each multiplex decoder
 *     runs). Its cost does not depend on what is on air, so it is timed on 8 MS/s of synthetic IQ, flat out on this
 *     thread, and reported as % of a core at real time;
 *   · its OFDM / FIC front end — the `dab` row's threads less what its own station's decode costs (the station row's
 *     decode + FAAD2 each): a multiplex's own stream is counted as a station by the capacity rule (vibe_dab_wide.h).
 *  The row's pct is the two together; its threads carry each, which runBenchmark writes into the dabStations block. */
inline benchdetail::Result runDabMuxRow(const benchdetail::Result& dabRow, const benchdetail::Result& stRow, double seconds) {
    using namespace benchdetail;
    Result r; r.id = "dec_dab_mux"; r.label = "DAB+: each multiplex in a wide capture (8 MS/s)"; r.rate = 8000000;
    vibedab::DabDdc ddc;
    if (!ddc.configure(8000000.0, 1712000.0)) { r.pct = -1; return r; }
    const size_t blk = 80000;   // 10 ms of 8 MS/s
    std::vector<float> in(blk * 2), out; out.reserve(blk);
    uint32_t x = 12345u;
    for (auto& v : in) { x = x * 1664525u + 1013904223u; v = float(int32_t(x) >> 8) * (1.0f / 8388608.0f) * 0.25f; }
    const double want = std::max(1.0, seconds * 0.5);   // ★ seconds of 8 MS/s input to push through
    const auto t0 = std::chrono::steady_clock::now();
    double fed = 0;
    while (fed < want) { out.clear(); ddc.process(in.data(), blk, out); fed += double(blk) / 8e6; }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double ddcPct = 100.0 * wall / fed;
    double dabTot = 0;
    for (const auto& t : dabRow.threads) if (t.first != "vibe-dsp") dabTot += t.second;
    double stDecode = 0;
    for (const auto& t : stRow.threads) {
        if (t.first == "vibe-dab-st (each)") stDecode += t.second;
        if (t.first == "vibe-faad2 (all)") stDecode += t.second / 3.0;   // ★ K = 3 stations (runDabStationRow)
    }
    const double front = (dabRow.pct >= 0 && dabTot > 0) ? std::max(dabTot * 0.25, dabTot - stDecode) : -1.0;
    r.pct = front > 0 ? ddcPct + front : -1.0;
    r.hottest = "per multiplex";
    r.threads.push_back({ "per multiplex", r.pct });
    r.threads.push_back({ "DDC @ 8 MS/s", ddcPct });
    r.threads.push_back({ "OFDM/FIC front end", front });
    return r;
}

/** ★★ V12 §11.1 THE FILTER BANK'S COSTS (vibe_dab_bank.h) — what the shim and the DAB listener figure weigh against the
 *  per-mux DDCs ("the benchmark sets the default"). At 8, 16, 32 and 56 MS/s, on synthetic IQ (the cost does not depend on
 *  what is on air), flat out on this thread: the forward FFT alone (`fwd@R`), and the bank with K = 4 slices (`slice@R` =
 *  the difference / K). The row's pct is the 56 MS/s × 26-block total (the B210 case); its threads carry every figure. */
inline benchdetail::Result runDabBankRow(double seconds) {
    using namespace benchdetail;
    Result r; r.id = "dec_dab_bank"; r.label = "DAB+: the wide-capture filter bank (56 MS/s, 26 blocks)"; r.rate = 56000000;
    const double secs = std::max(0.1, std::min(0.3, seconds / 20.0));
    double fwd56 = 0, slice56 = 0;
    for (double rate : { 8e6, 16e6, 32e6, 56e6 }) {
        const size_t n = size_t(rate * secs);
        std::vector<float> x(2 * n);
        uint32_t s = 2463534242u;
        for (auto& v : x) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; v = float(int32_t(s) >> 8) * (1.0f / 8388608.0f) * 0.2f; }
        auto timed = [&](int k) {
            vibedab::bank::Bank b; b.configure(rate);
            for (int i = 0; i < k; ++i) b.setSlice(size_t(i), -0.40 * rate + 0.9e6 + i * 1.712e6);
            std::vector<std::vector<float>> o(static_cast<size_t>(k) + 0);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < n; i += 65536) { b.process(x.data() + 2 * i, std::min<size_t>(65536, n - i), o); for (auto& v : o) v.clear(); }
            return 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / secs;
        };
        const double f = timed(0), withK = timed(4), sl = std::max(0.0, (withK - f) / 4.0);
        char nm[32];
        std::snprintf(nm, sizeof nm, "fwd@%d", int(rate / 1e6)); r.threads.push_back({ nm, f });
        std::snprintf(nm, sizeof nm, "slice@%d", int(rate / 1e6)); r.threads.push_back({ nm, sl });
        if (rate == 56e6) { fwd56 = f; slice56 = sl; }
    }
    r.pct = fwd56 + 26 * slice56;
    r.hottest = "bank";
    return r;
}

/** ★★ FETCHED ONCE, KEPT (Stuart, 2026-09-19): the clip is 18 MB gzipped, too big to ship in an APK or a .deb and
 *  needed only when the benchmark runs. Desktop/Linux fetches it here with curl; Android hands in a path its own
 *  Kotlin downloaded. Returns the clip's path, or empty if it could not be had — the DAB rows are then simply
 *  absent, never a guessed figure.
 *  ★ A part-downloaded file is worse than none (it would grade a row "none" and look like a broken decoder), so the
 *    download lands on a temporary name and is only moved into place once it opens as a clip. */
inline std::string ensureDabClip(const std::string& cacheDir,
                                 const std::string& url = "https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v1/dab-bench.vbu8.gz") {
#if defined(__ANDROID__)
    (void)cacheDir; (void)url; return "";
#else
    const std::string path = cacheDir + "/dab-bench.vbu8";
    DabClip probe;
    if (loadDabClip(path, probe)) return path;                 // already here and whole
    const std::string tmp = path + ".part";
    const std::string gz  = path + ".part.gz";
    // ★★★ NO SHELL (CodeQL cpp/command-line-injection, 2026-09-28). This used to be one `system()` line —
    //     curl | gzip > '<tmp>' — and `cacheDir` is the config directory, which comes from the command line or
    //     the environment (TMPDIR for vibeserver-bench). A single quote in that path closed ours and the rest
    //     ran as shell. vibeproc takes an ARGUMENT VECTOR, so a path is one argument whatever bytes it holds.
    //     The pipeline becomes two programs through a temporary file, as proc.h says a pipeline must.
    auto fail = [&]() { std::remove(gz.c_str()); std::remove(tmp.c_str()); return std::string(); };
    if (vibeproc::run({ "curl", "-sSL", "--max-time", "900", "-o", gz, url }) != 0) return fail();
    if (vibeproc::runToFile({ "gzip", "-dc", "--", gz }, tmp) != 0) return fail();
    std::remove(gz.c_str());
    if (!loadDabClip(tmp, probe)) { std::remove(tmp.c_str()); return ""; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return ""; }
    return path;
#endif
}

/** Both DAB rows — the station alone, and with the whole-multiplex label scan — or none if no clip. */
inline std::vector<benchdetail::Result> runDabRows(const std::string& clipPath, double seconds) {
    std::vector<benchdetail::Result> out;
    DabClip clip;
    if (benchdec::rowSkipped("dab")) return out;   // ★ V12: no radio here reaches Band III (vibe_benchmark_hw.h)
    if (clipPath.empty() || !loadDabClip(clipPath, clip)) return out;
    // ★ These are the slowest rows (each is fed in real time, warm-up included), so they say so — see benchStep.
    benchStep("DAB+");
    out.push_back(runDabRow("dab",      "DAB+ @ 2.048 MS/s",                   clip, false, seconds));
    benchStep("DAB+ with the station label scan");
    out.push_back(runDabRow("dab_scan", "DAB+ + station label scan @ 2.048 MS/s", clip, true,  seconds));
    // ★★ V12 own station per listener: what each extra station costs (docs/v12/DAB-OWN-STATION.md) — sets N
    benchStep("DAB+: each extra station");
    out.push_back(runDabStationRow(clip, out.front(), seconds));
    // ★★ V12 Phase 2: what each multiplex costs in a wide locked range — the DAB listener capacity (vibe_dab_wide.h)
    benchStep("DAB+: each multiplex in a wide capture");
    out.push_back(runDabMuxRow(out.front(), out.back(), seconds));
    // ★★ V12 §11.1: the filter bank — the other way to cut many blocks out of a wide capture
    benchStep("DAB+: the wide-capture filter bank");
    out.push_back(runDabBankRow(seconds));
    return out;
}

} // namespace vibe
