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
// ★ Linux decodes DAB+ AAC in an ffmpeg CHILD process, which /proc/self/task does not see. It is a small cost next
//   to the receiver; the rows say "receiver" for that reason.
//
// File: magic "VIBEBU81", double rate, double centreHz, uint32 sid (a DAB+ service in the clip), then u8 I/Q pairs.
#pragma once
#include "vibe_benchmark.h"
#include "vibe_dab_service.h"
#include <memory>
#include <thread>
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
    if (clipPath.empty() || !loadDabClip(clipPath, clip)) return out;
    // ★ These are the slowest rows (each is fed in real time, warm-up included), so they say so — see benchStep.
    benchStep("DAB+");
    out.push_back(runDabRow("dab",      "DAB+ @ 2.048 MS/s",                   clip, false, seconds));
    benchStep("DAB+ with the station label scan");
    out.push_back(runDabRow("dab_scan", "DAB+ + station label scan @ 2.048 MS/s", clip, true,  seconds));
    return out;
}

} // namespace vibe
