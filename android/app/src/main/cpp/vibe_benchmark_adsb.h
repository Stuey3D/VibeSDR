// VibeSDR — the benchmark's ADS-B rows (Stuart, 2026-10-24). See vibe_benchmark.h for the method, and
// vibe_benchmark_dab.h, whose shape these rows copy.
//
// ★★★ CPU ONLY — THE RADIO IS NOT TOUCHED (Stuart, 2026-10-24). Whether a radio and its USB bus DELIVER every sample
//     is a different question with a different answer on every box; the live "capture N %" in ADS-B mode answers that
//     (vibe_adsb_capture.h). These rows ask only whether THIS CPU can decode what ADS-B is handed at the two rates it
//     runs at: 2.4 MS/s (an RTL, an Airspy, a HackRF under the rate switch) and 3.0 MS/s (an SDRplay RSP).
// ★★★ A REAL SKY, NOT A SYNTHETIC ONE. The decoder's cost is mostly the preamble search over noise, plus the work on
//     every candidate that passes it — and how many pass depends on what a real 1090 MHz band looks like (Mode A/C,
//     overlapping replies, multipath). One second of Stuart's own recording of public ADS-B broadcasts (his FlightAware
//     antenna, 2026-10-24 — vibeserver/bench-clip/README-adsb.txt), resampled to both rates, as 8-bit IQ the way a
//     radio delivers it — adsb-bench.vbu8, fetched on the first run beside the DAB clip so the APK and the .deb stay small.
// ★★ FED IN REAL TIME through vibeadsb::Service, the thing the server runs — its queue DROPS the oldest when its
//    thread falls behind, so a flood would read a slow box CHEAP. Looped: a tracker seeing the same replies again
//    does the same work on them (CRC, the ICAO lookup, the table), so a second of sky on a loop is honest.
// ★ The feeding thread plays vibe-dsp: it converts the u8 IQ to floats as the server's reader path does, and copies
//   each block into the service's queue. Scored as the hottest thread against one core, like every other row.
//
// File: magic "VIBEAB81", then records of { double rate, double centreHz, uint32 nBytes, nBytes of u8 I/Q }.
#pragma once
#include "vibe_benchmark.h"
#include "vibe_adsb_service.h"
#include <memory>
#include <thread>
#if !defined(__ANDROID__)
#include "proc.h"   // ★ see vibe_benchmark_dab.h — fork+execvp, no shell
#endif

namespace vibe {

struct AdsbClip { double rate = 0, centre = 0; std::vector<std::complex<float>> iq; };

/** Load every record of a VIBEAB81 clip. Empty if unreadable or any record is shorter than half a second. */
inline std::vector<AdsbClip> loadAdsbClips(const std::string& path) {
    std::vector<AdsbClip> out;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return out;
    char magic[8] = {0};
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "VIBEAB81", 8) != 0) { std::fclose(fp); return out; }
    for (;;) {
        AdsbClip c; uint32_t n = 0;
        if (std::fread(&c.rate, sizeof c.rate, 1, fp) != 1) break;
        if (std::fread(&c.centre, sizeof c.centre, 1, fp) != 1 || std::fread(&n, sizeof n, 1, fp) != 1
            || c.rate < 1e6 || c.rate > 1e7 || n < uint32_t(c.rate) || n > 64u * 1024 * 1024) { out.clear(); break; }
        std::vector<uint8_t> u(n);
        if (std::fread(u.data(), 1, n, fp) != n) { out.clear(); break; }   // ★ truncated download
        c.iq.resize(n / 2);
        for (size_t i = 0; i < c.iq.size(); ++i)
            c.iq[i] = { (float(u[2 * i]) - 127.5f) / 127.5f, (float(u[2 * i + 1]) - 127.5f) / 127.5f };
        out.push_back(std::move(c));
    }
    std::fclose(fp);
    return out;
}

/** The pieces a row needs from a source of IQ, so the row logic can be tested without a recording. */
struct AdsbRowStats { double fedSecs = 0; uint64_t messages = 0; int capture = -1; };

/** One ADS-B row: the clip looped through a fresh Service for `seconds` of real time after `warm` seconds.
 *  -1 if nothing decoded (a row that decoded nothing measured nothing). `stats`, if given, gets what was seen. */
inline benchdetail::Result runAdsbRow(const std::string& id, const std::string& label, const AdsbClip& clip,
                                      double seconds, double warm = 2.0, AdsbRowStats* stats = nullptr) {
    using namespace benchdetail;
    Result r; r.id = id; r.label = label; r.rate = clip.rate;
    if (clip.iq.empty()) { r.pct = -1; return r; }
    const auto before = threadCpu();
    auto svc = std::make_unique<vibeadsb::Service>();
    svc->start(clip.rate, NAN, NAN);
    // ★ The block the server's DSP loop hands the service is the capture's own block — ~10 ms at these rates.
    const size_t blk = size_t(clip.rate / 100);
    const size_t total = clip.iq.size();
    std::vector<std::complex<float>> conv(blk);
    size_t pos = 0;
    std::map<long, double> mid; double selfMid = 0; bool measuring = false;
    const auto t0 = std::chrono::steady_clock::now();
    double fedSecs = 0;
    for (;;) {
        if (!measuring && fedSecs >= warm) { mid = threadCpu(); selfMid = threadSelfCpu(); measuring = true; }
        if (fedSecs >= warm + seconds) break;
        const size_t n = std::min(blk, total - pos);
        // ★ The copy stands in for the reader path's u8 -> float conversion, which the feeding thread pays live.
        std::memcpy(conv.data(), clip.iq.data() + pos, n * sizeof conv[0]);
        svc->feed(conv.data(), n);
        pos = (pos + n) % total;
        fedSecs += double(n) / clip.rate;
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fedSecs * 1e6)));
    }
    // ★ Let the worker finish what is queued — at most kMaxQueuedSec of IQ — so its work is in the figure.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto after = threadCpu();
    const double selfEnd = threadSelfCpu();
    std::map<long, std::string> names;
    for (const auto& kv : after) if (!before.count(kv.first)) names[kv.first] = threadName(kv.first);
    const std::string js = svc->report();
    svc.reset();                                                   // ★ AFTER the reads — see runOne
    uint64_t msgs = 0; int capture = -1;
    if (const char* p = std::strstr(js.c_str(), "\"messages\":")) msgs = std::strtoull(p + 11, nullptr, 10);
    if (const char* p = std::strstr(js.c_str(), "\"capture\":")) capture = std::atoi(p + 10);
    double best = (selfEnd - selfMid) / seconds; r.hottest = "vibe-dsp";
    r.threads.push_back({ "vibe-dsp", 100.0 * best });
    for (const auto& kv : after) {
        if (before.count(kv.first)) continue;
        auto m = mid.find(kv.first);
        const double used = (kv.second - (m != mid.end() ? m->second : 0.0)) / seconds;
        r.threads.push_back({ names[kv.first], 100.0 * used });
        if (used > best) { best = used; r.hottest = names[kv.first]; }
    }
    r.pct = msgs > 0 ? 100.0 * best : -1.0;
    if (stats) { stats->fedSecs = fedSecs; stats->messages = msgs; stats->capture = capture; }
    if (std::getenv("VIBE_BENCH_DEBUG"))
        std::fprintf(stderr, "[%s] messages %llu, capture %d %%\n", id.c_str(), (unsigned long long)msgs, capture);
    return r;
}

/** ★★ FETCHED ONCE, KEPT — exactly ensureDabClip's arrangement (temporary name, moved into place only once it
 *  opens as a clip). Android hands in a path its own Kotlin downloaded (VibeBenchmark.kt). */
inline std::string ensureAdsbClip(const std::string& cacheDir,
                                  const std::string& url = "https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v2/adsb-bench.vbu8.gz") {
#if defined(__ANDROID__)
    (void)cacheDir; (void)url; return "";
#else
    const std::string path = cacheDir + "/adsb-bench.vbu8";
    if (!loadAdsbClips(path).empty()) return path;
    const std::string tmp = path + ".part";
    const std::string gz  = path + ".part.gz";
    auto fail = [&]() { std::remove(gz.c_str()); std::remove(tmp.c_str()); return std::string(); };
    if (vibeproc::run({ "curl", "-sSL", "--max-time", "900", "-o", gz, url }) != 0) return fail();
    if (vibeproc::runToFile({ "gzip", "-dc", "--", gz }, tmp) != 0) return fail();
    std::remove(gz.c_str());
    if (loadAdsbClips(tmp).empty()) { std::remove(tmp.c_str()); return ""; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return ""; }
    return path;
#endif
}

/** ★★ THE DECODER ROWS' CLIP (vibe_benchmark_more.h, 2026-10-31): decoders-bench.vbu8 — seconds of real band for the
 *  CW and PSK skimmers, the NDB skimmer and ISM (vibeserver/bench-clip/README-decoders.txt). Fetched once and kept, exactly
 *  as the ADS-B clip (here because this header already has the fetcher; Android's Kotlin fetches its own copy). Without it
 *  those rows read "not measured" and every other row still runs — a box with no internet still measures itself. */
inline std::string ensureDecoderClip(const std::string& cacheDir,
                                     const std::string& url = "https://github.com/Stuey3D/VibeSDR/releases/download/bench-clip-v3/decoders-bench.vbu8.gz") {
#if defined(__ANDROID__)
    (void)cacheDir; (void)url; return "";
#else
    const std::string path = cacheDir + "/decoders-bench.vbu8";
    if (!benchdec::loadDecoderClips(path).empty()) return path;
    const std::string tmp = path + ".part";
    const std::string gz  = path + ".part.gz";
    auto fail = [&]() { std::remove(gz.c_str()); std::remove(tmp.c_str()); return std::string(); };
    if (vibeproc::run({ "curl", "-sSL", "--max-time", "900", "-o", gz, url }) != 0) return fail();
    if (vibeproc::runToFile({ "gzip", "-dc", "--", gz }, tmp) != 0) return fail();
    std::remove(gz.c_str());
    if (benchdec::loadDecoderClips(tmp).empty()) { std::remove(tmp.c_str()); return ""; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return ""; }
    return path;
#endif
}

/** Both ADS-B rows — 2.4 and 3.0 MS/s — or none if no clip. Ids adsb_2400 / adsb_3000, from each record's own rate. */
inline std::vector<benchdetail::Result> runAdsbRows(const std::string& clipPath, double seconds) {
    std::vector<benchdetail::Result> out;
    if (clipPath.empty()) return out;
    const auto clips = loadAdsbClips(clipPath);
    for (const auto& c : clips) {
        char id[24], label[48];
        std::snprintf(id, sizeof id, "adsb_%d", int(std::lround(c.rate / 1e3)));
        std::snprintf(label, sizeof label, "ADS-B @ %.1f MS/s", c.rate / 1e6);
        benchStep(label);
        out.push_back(runAdsbRow(id, label, c, seconds));
    }
    return out;
}

} // namespace vibe
