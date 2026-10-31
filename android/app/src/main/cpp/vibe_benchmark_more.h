// VibeSDR — the benchmark's rows for EVERY decoder after the first eight (Stuart, 2026-10-31: "all these decoders need
// their own lines in the initial setup benchmark too"). vibe_benchmark_decoders.h holds the method and the first rows
// (RTTY, NAVTEX, WEFAX, SSTV, time signals, FT8+FT4, Band spots, LoRa); runDecoderRows calls runMoreDecoderRows here.
//
// ★★★ THE SERVER'S OWN OBJECTS, ON A SIGNAL THEY DECODE. A listener's decoder is started through a real DecoderHost —
//     the same start(ext, msg) the decoder socket calls, the same queue, the same decode thread at decoder priority — and
//     fed 48 kHz audio that carries a transmission it reads (our own transmitters: decoders/*.h encoders and
//     benchsynth/). A digital-spots mode is its registered engine (digi::modes()), fed one whole period at its own rate
//     with real transmissions stamped on the period's boundaries. A whole-radio mode (NDB, AIS, ISM) is its Service.
//     Every row carries `decoded`: what it got out of the signal — a row that decoded nothing timed only the search.
// ★★ AVERAGE PROCESS CPU (getrusage), as the first rows: CPU used ÷ signal seconds fed. Nothing else of ours runs (the
//    radio is stopped). A continuous decoder is fed as fast as its queue takes it, never dropping (the feed waits on
//    queued()), so the figure is the decoder's work; a slotted mode lets its worker finish each period before the next.
//    AIS and ISM have no queue to wait on: they are fed in REAL TIME, as the ADS-B rows are, and a row that dropped
//    says so in its label. ★★ They are SCORED as ADS-B is — by their HOTTEST THREAD against one core (Stuart,
//    2026-10-31: a single thread past 100 % is the real failure on a Pi 2), with the process total in the row's detail.
// ★★ A TIME BUDGET PER ROW (kRowWallSec): a continuous row stops feeding once it has used that much wall time and fed
//    at least kRowMinSec of signal — a Pi 2 measures less signal, never longer. A slotted row must finish its period
//    (a half-decoded WSPR minute measures nothing), so its cost is the period's decode, once. Every row reports its own
//    wall time (`ms`), and the run its total, so the owner sees what the measurement cost.
// ★★ REAL CLIPS WHERE A REAL BAND SETS THE COST (Stuart, 2026-10-31: "put samples on github that get downloaded when
//    running the benchmark"). The CW and PSK skimmers, the NDB skimmer and ISM's burst finder do work in proportion to
//    what is ON the band — carriers, keyed signals, bursts over a real noise floor — so a synthetic band would guess
//    it. Their input is decoders-bench.vbu8 (vibeserver/bench-clip/README-decoders.txt): seconds of Stuart's own
//    recordings, the ISM ones with every real device removed and synthetic sensors put in their place. Fetched once
//    with the DAB and ADS-B clips; WITHOUT it those rows read "not measured" (grade none), never a guess, and every
//    other row still runs. Everything else is synthetic: a single signal costs the same whatever else is on the air.
// ★ What each row sets — the defaults, the decoder limit — is decided in ONE place, defaultsFor (below), and sent with
//   the results; vibe_setup_page.h benchApplyDefaults and ServerModeScreen.tsx applyBench apply that list.
#pragma once
#include "vibe_decoder_host.h"
#include "decoders/digi_mode.h"
#include "decoders/digimodes/wspr_decoder.h"
#include "decoders/digimodes/q65_decoder.h"
#include "decoders/digimodes/fst4_decoder.h"
#include "decoders/digimodes/msk144_code.h"
#include "decoders/digimodes/js8_decoder.h"
#include "decoders/digimodes/js8_message.h"
#include "decoders/jtty_decoder.h"
#include "decoders/dsc_decoder.h"
#include "decoders/textmodes.h"
#include "decoders/feld_hell.h"
#include "decoders/tone_id.h"
#include "decoders/audio_nr.h"
#include "decoders/auto_notch.h"
#include "vibe_ndb_service.h"
#include "vibe_ais_service.h"
#include "vibe_ism_service.h"
#include "vibe_hdlc.h"
#include "benchsynth/cw_synth.h"
#include "benchsynth/psk_synth.h"
#include "benchsynth/sitor_synth.h"
#include "benchsynth/wspr_synth.h"
#include "benchsynth/q65_synth.h"
#include "benchsynth/fst4_synth.h"
#include "benchsynth/msk144_synth.h"
#if defined(VIBE_PAGING)
#include "benchsynth/flex_synth.h"
#endif
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <map>
#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace vibe {
namespace benchdec {

/** ★★ CPU seconds per THREAD of this process, with each thread's name — the whole-radio rows (AIS, ISM) are scored by
 *  their HOTTEST thread against one core, as ADS-B is (Stuart, 2026-10-31: a single thread past 100 % is the real
 *  failure on a Pi 2; total process CPU hides it across cores). Linux / Android: /proc/self/task; macOS: Mach
 *  thread_info. Empty where neither exists — the row then falls back to process CPU and says so. */
struct ThreadTime { double sec = 0; std::string name; };
inline std::map<uint64_t, ThreadTime> threadTimes() {
    std::map<uint64_t, ThreadTime> out;
#if defined(__linux__) || defined(__ANDROID__)
    const double hz = (double)sysconf(_SC_CLK_TCK);
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[96]; std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            char buf[1024]; const size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f); buf[n] = 0;
            const char* p = std::strrchr(buf, ')'); const char* q = std::strchr(buf, '(');
            unsigned long ut = 0, st = 0;
            if (!p || !q || std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
            ThreadTime t; t.sec = (ut + st) / hz; t.name.assign(q + 1, p);
            out[(uint64_t)std::atoll(e->d_name)] = t;
        }
        closedir(d);
    }
#elif defined(__APPLE__)
    thread_act_array_t th = nullptr; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return out;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_basic_info_data_t b; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        thread_identifier_info_data_t id; mach_msg_type_number_t ci = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t x; mach_msg_type_number_t cx = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&b, &c) == KERN_SUCCESS
            && thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ci) == KERN_SUCCESS) {
            ThreadTime t;
            t.sec = b.user_time.seconds + b.user_time.microseconds / 1e6 + b.system_time.seconds + b.system_time.microseconds / 1e6;
            if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&x, &cx) == KERN_SUCCESS) t.name = x.pth_name;
            out[id.thread_id] = t;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_act_t));
#endif
    return out;
}
/** Score a stretch by its hottest thread: `a` and `b` are threadTimes() at its start and end, `secs` its length. Fills
 *  `r.pct` (the hottest thread, % of one core), `r.hottest`, and `r.threads` — every thread that worked, and "process"
 *  (the total, kept visible in the row's detail). Falls back to `processPct` where threads cannot be read. */
inline void scoreHottest(Row& r, const std::map<uint64_t, ThreadTime>& a, const std::map<uint64_t, ThreadTime>& b,
                         double secs, double processPct) {
    double best = -1; std::string who;
    for (const auto& kv : b) {
        auto it = a.find(kv.first);
        const double used = 100.0 * (kv.second.sec - (it != a.end() ? it->second.sec : 0.0)) / secs;
        if (used < 0.05) continue;
        std::string nm = kv.second.name.empty() ? "thread" : kv.second.name;
        int same = 0; for (const auto& t : r.threads) same += t.first.rfind(nm, 0) == 0;
        if (same) nm += "#" + std::to_string(same + 1);   // ★ unnamed or twin threads: keys stay distinct in the JSON
        r.threads.push_back({ nm, used });
        if (used > best) { best = used; who = nm; }
    }
    r.threads.push_back({ "process", processPct });
    if (best >= 0) { r.pct = best; r.hottest = who; }
    else { r.pct = processPct; r.hottest = "process"; }   // ★ no per-thread clock here: the total, said as such
}

/** ★ The per-row budget. A continuous row feeds until it has used kRowWallSec of wall time AND fed kRowMinSec of signal,
 *  or fed kRowMaxSec — so a fast box measures a longer stretch and a Pi 2 a shorter one, in about the same time. */
constexpr double kRowWallSec = 3.0, kRowMinSec = 4.0, kRowMaxSec = 12.0;
/** ★ Real-time rows (AIS, ISM): this much warm-up, then this much measured. */
constexpr double kRtWarmSec = 1.0, kRtSec = 3.0;

inline double wallNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── the clip ───────────────────────────────────────────────────────────────────────────────────────────────────────

/** One record of decoders-bench.vbu8: IQ as a radio or a skimmer's slice hands it over. */
struct DecClip { std::string id; double rate = 0, centre = 0; std::vector<std::complex<float>> iq; };

/** File: magic "VIBEDC81", then records of { char id[8] (NUL-padded), double rate, double centreHz, uint32 bits (8 = u8
 *  offset binary as rtl_sdr writes it, 16 = s16 little-endian), uint32 nComplex, the samples I,Q,I,Q… }. Empty if the
 *  file is missing, unreadable or truncated anywhere — a part-download is no clip at all. */
inline std::vector<DecClip> loadDecoderClips(const std::string& path) {
    std::vector<DecClip> out;
    if (path.empty()) return out;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return out;
    char magic[8] = {0};
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "VIBEDC81", 8) != 0) { std::fclose(fp); return out; }
    for (;;) {
        char id[9] = {0}; DecClip c; uint32_t bits = 0, n = 0;
        if (std::fread(id, 1, 8, fp) != 8) break;
        if (std::fread(&c.rate, sizeof c.rate, 1, fp) != 1 || std::fread(&c.centre, sizeof c.centre, 1, fp) != 1
            || std::fread(&bits, 4, 1, fp) != 1 || std::fread(&n, 4, 1, fp) != 1
            || !(c.rate >= 1000 && c.rate <= 1e7) || (bits != 8 && bits != 16) || n < 1000 || n > 32u * 1024 * 1024) { out.clear(); break; }
        c.id = id; c.iq.resize(n);
        if (bits == 8) {
            std::vector<uint8_t> u((size_t)n * 2);
            if (std::fread(u.data(), 1, u.size(), fp) != u.size()) { out.clear(); break; }
            for (size_t i = 0; i < n; ++i) c.iq[i] = { (u[2 * i] - 127.5f) / 127.5f, (u[2 * i + 1] - 127.5f) / 127.5f };
        } else {
            std::vector<int16_t> s((size_t)n * 2);
            if (std::fread(s.data(), 2, s.size(), fp) != s.size()) { out.clear(); break; }
            for (size_t i = 0; i < n; ++i) c.iq[i] = { s[2 * i] / 32768.0f, s[2 * i + 1] / 32768.0f };
        }
        out.push_back(std::move(c));
    }
    std::fclose(fp);
    return out;
}
inline const DecClip* clipNamed(const std::vector<DecClip>& v, const char* id) {
    for (const auto& c : v) if (c.id == id) return &c;
    return nullptr;
}

// ── counting what a decoder decoded ────────────────────────────────────────────────────────────────────────────────

/** A decoder socket that only counts: text characters (0x01 text frames), picture lines (0x01 image frames), and JSON
 *  messages whose text holds `jsonKey`. */
struct CountPeer : DecoderPeer {
    std::atomic<long> chars{0}, lines{0}, json{0}, items{0};   // items: the most list entries ("id") one JSON carried
    std::string jsonKey;
    bool image = false;   // ★ Feld Hell: 0x01 is a picture line, not text
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override {
        if (n < 1 || d[0] != 0x01) return;
        if (image) { lines.fetch_add(1); return; }
        if (n <= 13) return;
        long c = 0;
        for (size_t i = 13; i < n; ++i) if (d[i] > 0x20 && d[i] < 0x7F) ++c;
        chars.fetch_add(c);
    }
    void text(const std::string& s) override {
        if (jsonKey.empty() || s.find(jsonKey) == std::string::npos) return;
        json.fetch_add(1);
        long n = 0; for (size_t p = s.find("{\"id\":"); p != std::string::npos; p = s.find("{\"id\":", p + 1)) ++n;
        if (n > items.load()) items.store(n);
    }
};
/** …and the gallery's taps, which see the structured events (spots, DSC calls, APRS stations). */
struct CountTap : DecoderTap {
    std::atomic<long> events{0};
    void cwSpot(const std::string&, double, float, float, const std::string&) override { events.fetch_add(1); }
    void spot(const std::string&, const std::string&, const std::string&, int, double) override { events.fetch_add(1); }
    void station(const vibestation::Heard&) override { events.fetch_add(1); }
    void dscCall(const dsc::Call&, const std::string&, const std::string&, bool) override { events.fetch_add(1); }
};

/** What `decoded` counts for a row. */
enum class Count { Chars, Lines, Json, Events, Any };

// ── the runners ────────────────────────────────────────────────────────────────────────────────────────────────────

/** ★ A listener's decoder, through a DecoderHost exactly as the server starts it. `audio` (48 kHz) is looped as needed. */
inline Row hostAudioRow(const std::string& id, const std::string& label, const std::string& ext, const std::string& msg,
                        const std::vector<float>& audio, Count what, const std::string& jsonKey = "") {
    Row r; r.id = id; r.label = label;
    if (audio.empty()) return r;
    auto peer = std::make_shared<CountPeer>(); peer->jsonKey = jsonKey; peer->image = what == Count::Lines;
    auto tap = std::make_shared<CountTap>();
    DecoderHost::Env env;
    env.dialHz = [] { return 14070000.0; };
    env.tap = [tap](const std::string&, const std::string&) { return tap; };
    env.cwSpotTap = [tap](bool) { return tap; };
    env.pskSpotTap = [tap](bool) { return tap; };
    double fedSec = 0, pct = -1;
    {
        DecoderHost h(env, "bench");
        h.addPeer(peer);
        if (h.start(ext, msg) != DecoderHost::Start::Ok) return r;
        const double w0 = wallNow(), c0 = processCpu();
        const int blk = 960;   // ★ 20 ms — the server's audio block
        size_t pos = 0;
        while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            while (h.queued() > 24000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const int n = (int)std::min<size_t>((size_t)blk, audio.size() - pos);
            h.feed(audio.data() + pos, n);
            pos = (pos + (size_t)n) % audio.size();
            fedSec += n / 48000.0;
        }
        for (int i = 0; i < 4000 && h.queued() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        settleCpu(5.0);
        pct = 100.0 * (processCpu() - c0) / fedSec;
        h.stop();
    }
    r.pct = pct;
    r.decoded = what == Count::Chars ? (int)peer->chars.load() : what == Count::Lines ? (int)peer->lines.load()
              : what == Count::Json ? (int)peer->json.load() : what == Count::Events ? (int)tap->events.load()
              : (int)(peer->chars.load() + peer->json.load() + tap->events.load());
    return r;
}

/** ★ A skimmer on IQ (CW / PSK), through the host's IQ path — the clip's slice, looped. */
inline Row hostIqRow(const std::string& id, const std::string& label, const std::string& ext, const std::string& msg,
                     const DecClip* clip, const std::string& jsonKey) {
    Row r; r.id = id; r.label = label;
    if (!clip || clip->iq.empty()) { r.label += " (no clip)"; return r; }
    auto peer = std::make_shared<CountPeer>(); peer->jsonKey = jsonKey;
    auto tap = std::make_shared<CountTap>();
    DecoderHost::Env env;
    env.dialHz = [c = clip->centre] { return c; };
    env.cwSpotTap = [tap](bool) { return tap; };
    env.pskSpotTap = [tap](bool) { return tap; };
    double fedSec = 0, pct = -1;
    {
        DecoderHost h(env, "bench");
        h.addPeer(peer);
        if (h.start(ext, msg) != DecoderHost::Start::Ok) return r;
        const double w0 = wallNow(), c0 = processCpu();
        const size_t blk = (size_t)(clip->rate / 50);
        size_t pos = 0;
        while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            while (h.queuedIq() > 0.5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t n = std::min(blk, clip->iq.size() - pos);
            h.feedIq(clip->iq.data() + pos, (int)n, clip->rate, clip->centre);
            pos = (pos + n) % clip->iq.size();
            fedSec += (double)n / clip->rate;
        }
        for (int i = 0; i < 4000 && h.queuedIq() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        settleCpu(5.0);
        pct = 100.0 * (processCpu() - c0) / fedSec;
        /* ★ The signal list goes out at most once a WALL second, and the feed ran far faster than that — so, after the
         *  measurement, one more block a second later makes the skimmer send the list it has: `decoded` is how many
         *  signals it was decoding. Not in the figure (the CPU was read above). */
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        h.feedIq(clip->iq.data(), (int)std::min(blk, clip->iq.size()), clip->rate, clip->centre);
        for (int i = 0; i < 2000 && h.queuedIq() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        h.stop();
    }
    r.pct = pct;
    r.decoded = (int)peer->items.load();
    return r;
}

/** ★ A digital-spots mode's ENGINE (digi::modes()), fed `audio` at the mode's own rate from UTC `utc0`. A slotted engine
 *  decodes each period on a worker of its own and SKIPS a period if the last is still decoding — so the feed lets the
 *  worker finish (settleCpu) at every `paceSec` of audio, as a real stream would give it the time. */
inline Row engineRow(const digi::ModeDef& m, const std::vector<float>& audio, double utc0, double paceSec, bool hasSignal) {
    Row r; r.id = "spot_" + m.name; r.label = "Digital spots: " + m.name;
    for (auto& ch : r.id) ch = (char)std::tolower((unsigned char)ch);
    if (audio.empty() || !m.make) return r;
    std::atomic<int> got{0};
    double pct = -1; uint64_t skipped = 0, frames = 0;
    {
        std::unique_ptr<digi::Engine> e;
        // ★ a mode whose set-up depends on its dial is timed at its HEAVIEST dial: Q65 on 6 m, where Q65-15 runs too
        try { e = m.makeAt ? m.makeAt(m.name == "Q65" ? 50.310e6 : 0.0) : m.make(); } catch (...) { return r; }
        if (!e) return r;
        e->onSpot = [&](const digi::Spot&) { got.fetch_add(1); };
        e->loaded = [] { return true; };   // ★ ONE PASS — what a loaded box pays (the FT8 row's rule)
        const double c0 = processCpu();
        const int blk = std::max(1, m.sampleRate / 50);
        double nextPace = paceSec;
        for (size_t i = 0; i < audio.size(); i += (size_t)blk) {
            const int n = (int)std::min<size_t>((size_t)blk, audio.size() - i);
            e->process(audio.data() + i, n, utc0 + (double)(i + (size_t)n) / m.sampleRate);
            if (paceSec > 0 && (double)(i + (size_t)n) / m.sampleRate >= nextPace) { settleCpu(10.0); nextPace += paceSec; }
        }
        settleCpu(30.0);
        pct = 100.0 * (processCpu() - c0) / ((double)audio.size() / m.sampleRate);
        skipped = e->slotsSkipped();
        frames = e->slotsDecoded();
        if (std::getenv("VIBE_BENCH_DEBUG"))
            std::fprintf(stderr, "[%s] spots %d, slots/frames decoded %llu, skipped %llu, %.1f s of audio\n", m.name.c_str(), got.load(),
                         (unsigned long long)e->slotsDecoded(), (unsigned long long)skipped, (double)audio.size() / m.sampleRate);
    }
    r.pct = pct;
    // ★ A free-running mode (JTTY, PSK, MSK144) counts what it decoded — frames — when no spot was due (JTTY spots a call
    //   once per ten minutes per signal; a frame decoded is the work done either way).
    r.decoded = m.slotSec > 0 ? got.load() : std::max<int>(got.load(), (int)frames);
    if (!hasSignal) r.label += " (searching noise)";
    if (skipped) r.label += " (" + std::to_string(skipped) + " skipped)";
    return r;
}

// ── the signals ────────────────────────────────────────────────────────────────────────────────────────────────────

/** Gaussian noise of σ `sigma` added to `x` (deterministic). */
inline void addNoise(std::vector<float>& x, float sigma, uint32_t seed) {
    std::mt19937 g(seed); std::normal_distribution<float> d(0.f, sigma);
    for (auto& v : x) v += d(g);
}
/** Scale to a peak of 0.5 (the decoders are level-free; this keeps the int-free path honest). */
inline void normalise(std::vector<float>& x) {
    float pk = 1e-9f; for (float v : x) pk = std::max(pk, std::fabs(v));
    for (auto& v : x) v *= 0.5f / pk;
}
/** Continuous-phase FSK at 48 kHz from a bit list (1 = the higher tone). */
inline void fskBits(std::vector<float>& out, const std::vector<int>& bits, double baud, double mark1Hz, double space0Hz) {
    const double fs = 48000, spb = fs / baud;
    double ph = 0, t = 0;
    for (size_t k = 0; k < bits.size(); ++k) {
        const double hz = bits[k] ? mark1Hz : space0Hz, end = (k + 1) * spb;
        for (; t < end; t += 1) { ph += 2 * M_PI * hz / fs; out.push_back((float)std::sin(ph)); }
    }
}
/** ITA2 (letters only) RTTY, 45.45 Bd 170 Hz, 5N1.5, centred on `cf`: real text, so RTTY AUTO must find, lock and print. */
inline std::vector<float> rttyText(const std::string& text, double cf) {
    static const char* L = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";   // ITA2 letters by code 0–31
    std::vector<int> bits;
    auto ch = [&](int code) { bits.push_back(0); for (int b = 0; b < 5; ++b) bits.push_back((code >> b) & 1); bits.push_back(1); bits.push_back(1); };
    for (int i = 0; i < 40; ++i) bits.push_back(1);          // mark idle
    for (int k = 0; k < 2; ++k) ch(31);                       // LTRS
    for (char c : text) {
        int code = -1;
        for (int k = 0; k < 32; ++k) if (L[k] == c && k != 0) code = k;
        if (code >= 0) ch(code);
    }
    for (int i = 0; i < 20; ++i) bits.push_back(1);
    // ★ 1.5 stop bits: every third bit of a character pair is half length — approximated by 7 bits at 45.45 Bd with the
    //   stop doubled, which every receiver that takes 1.5 also takes (the receiver resyncs on each start bit).
    std::vector<float> a;
    fskBits(a, bits, 45.45, cf + 85, cf - 85);
    return a;
}
/** EAS / SAME (47 CFR § 11.31): 520.83 Bd AFSK, mark 2083.3 Hz (1) / space 1562.5 Hz (0), LSB first, a 16-byte 0xAB
 *  preamble, the header three times with a second's silence between. */
inline std::vector<float> sameAlert(const std::string& hdr) {
    std::vector<float> a(48000 / 2, 0.f);
    for (int burst = 0; burst < 3; ++burst) {
        std::vector<int> bits;
        auto byte = [&](uint8_t b) { for (int i = 0; i < 8; ++i) bits.push_back((b >> i) & 1); };
        for (int i = 0; i < 16; ++i) byte(0xAB);
        for (char c : hdr) byte((uint8_t)c);
        fskBits(a, bits, 520.83, 2083.3, 1562.5);
        a.insert(a.end(), 48000, 0.f);
    }
    return a;
}
/** SELCAL (ICAO Annex 10 Vol III): two pulses of two tones, 1 s each, 0.2 s apart. Tones by letter (the first 16). */
inline std::vector<float> selcalCall(const char* code /* "ABCD" */) {
    static const char* letters = "ABCDEFGHJKLMPQRS";
    static const double hz[16] = { 312.6, 346.7, 384.6, 426.6, 473.2, 524.8, 582.1, 645.7, 716.1, 794.3, 881.0, 977.2,
                                   1083.9, 1202.3, 1333.5, 1479.1 };
    auto f = [&](char c) { const char* p = std::strchr(letters, c); return p ? hz[p - letters] : 0.0; };
    std::vector<float> a(48000 / 2, 0.f);
    auto pair = [&](double f1, double f2, double sec) {
        for (int i = 0; i < (int)(sec * 48000); ++i) { const double t = i / 48000.0; a.push_back((float)(0.4 * std::sin(2 * M_PI * f1 * t) + 0.4 * std::sin(2 * M_PI * f2 * t))); }
    };
    pair(f(code[0]), f(code[1]), 1.0); a.insert(a.end(), 9600, 0.f); pair(f(code[2]), f(code[3]), 1.0);
    a.insert(a.end(), 48000, 0.f);
    return a;
}
/** APRS: an AX.25 UI frame, HDLC (flags, stuffing, FCS), NRZI, Bell 202 AFSK 1200 Bd at 48 kHz. */
inline std::vector<float> aprsPacket(const std::string& src, int ssid, const std::string& info) {
    std::vector<uint8_t> b;
    auto addr = [&](const std::string& call, int s, bool last) {
        for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < (int)call.size() ? call[(size_t)i] : ' ') << 1));
        b.push_back(uint8_t(0x60 | (s << 1) | (last ? 1 : 0)));
    };
    addr("APRS", 0, false); addr(src, ssid, true);
    b.push_back(0x03); b.push_back(0xF0);
    b.insert(b.end(), info.begin(), info.end());
    vibehdlc::appendFcs(b);
    std::vector<int> bits;
    for (int i = 0; i < 30; ++i) vibehdlc::flagBits(bits);
    vibehdlc::stuffBits(b, bits);
    vibehdlc::flagBits(bits); vibehdlc::flagBits(bits);
    std::vector<int> lv; int level = 1; vibehdlc::nrzi(bits, lv, level);
    std::vector<float> a(4800, 0.f);
    fskBits(a, lv, 1200, 1200, 2200);   // ★ level 1 = mark 1200 Hz, 0 = space 2200 Hz
    a.insert(a.end(), 24000, 0.f);
    return a;
}
/** A single CW signal (ITU-R M.1677 timing, 22 WPM) at 700 Hz, 48 kHz real. */
inline std::vector<float> cwSignal(const std::string& text) {
    cwsynth::Rng rng(5);
    cwsynth::Keying k; k.wpm = 22;
    std::vector<cwsynth::Tone> t(1); t[0].hz = 700; t[0].snrDb = 15; t[0].env = cwsynth::keyEnvelope(text, k, 48000, rng, 0.3);
    std::vector<std::complex<float>> iq; std::vector<float> re;
    cwsynth::synth(t, 48000, false, t[0].env.size() / 48000.0, rng, iq, re);
    return re;
}
/** A single PSK31 signal at 1000 Hz, `rate` Hz real. */
inline std::vector<float> pskSignal(const std::string& text, double rate) {
    psksynth::Rng rng(6);
    std::vector<psksynth::Sig> s(1); s[0].hz = 1000; s[0].snrDb = 15; s[0].text = text; s[0].startSec = 0.3;
    std::vector<std::complex<float>> iq; std::vector<float> re;
    psksynth::synth(s, rate, false, psksynth::duration(s[0]) + 0.6, rng, iq, re);
    return re;
}
/** A JTTY message (packMessage → frames → the 4-GFSK waveform) at `rate`, tone 0 at `f0`, in noise. */
inline std::vector<float> jttyMessage(const std::string& text, double rate, double f0) {
    std::vector<uint64_t> frames;
    if (!jtty::packMessage(text, frames)) return {};
    std::vector<uint8_t> tones;
    for (uint64_t w : frames) { uint8_t t[jtty::kFrameSyms]; jtty::encodeFrame(w, t); tones.insert(tones.end(), t, t + jtty::kFrameSyms); }
    std::vector<float> a((size_t)(rate * 0.5), 0.f);
    jtty::synthesise(tones.data(), (int)tones.size(), f0, rate, 1.0f, a, a.size());
    a.insert(a.end(), (size_t)(rate * 2.0), 0.f);   // ★ a quiet tail: the receiver closes the message on it
    addNoise(a, 0.3f, 17);
    return a;
}

// ── the digital-spots modes' periods (UTC base: a whole multiple of every period, 2027-01-15) ───────────────────────
constexpr double kSpotT0 = 1800000000.0;

/** One period (or more, `periods`) of audio for mode `m` at 12 kHz, with real transmissions; `utc0` = the first sample.
 *  `pace` = how often the feed lets the engine's worker finish. Empty audio = this build has no transmitter for it (the
 *  row then searches noise and says so). */
inline std::vector<float> spotAudio(const digi::ModeDef& m, double& utc0, double& pace, bool& signal) {
    const double fs = m.sampleRate;
    signal = true;
    const std::string& n = m.name;
    auto window = [&](double startUtc, double secs) { utc0 = startUtc; return std::vector<float>((size_t)(secs * fs), 0.f); };
    auto putAt = [&](std::vector<float>& a, const std::vector<float>& w, double atUtc) {
        const long off = std::lround((atUtc - utc0) * fs);
        for (size_t i = 0; i < w.size(); ++i) { const long j = off + (long)i; if (j >= 0 && j < (long)a.size()) a[(size_t)j] += w[i]; }
    };
    std::vector<float> a;
    if (n == "WSPR") {
        pace = 0;
        a = window(kSpotT0 - wspr::kLeadSec - 1.0, 120.0 + wspr::kLeadSec + 3.0);
        std::vector<float> w((size_t)((120.0 + wspr::kLeadSec) * fs), 0.f);
        wsprsynth::Sig s; s.msg = wsprsynth::msg1("G4ABC", "IO91", 37); s.centreHz = 1500; s.snr = -18;
        wsprsynth::addSignal(w, (int)fs, s, 1.0);
        s.msg = wsprsynth::msg1("K1ABC", "FN42", 30); s.centreHz = 1450; s.snr = -20; wsprsynth::addSignal(w, (int)fs, s, 1.0, 9);
        putAt(a, w, kSpotT0 - wspr::kLeadSec);
        addNoise(a, 1.0f, 41);
    } else if (n == "Q65") {
        pace = 15;
        a = window(kSpotT0 - 2.0, 63.0);
        addNoise(a, 1.0f, 42);
        auto put = [&](const char* text, int P, double periodStart, double f0) {
            uint8_t t[q65::kSymbols]; if (!q65::encodeText(text, t)) return;
            std::vector<float> w((size_t)q65::windowSamples(*q65::periodFor(P)), 0.f);
            q65synth::Sig s; s.tones = t; s.submode = 0; s.f0 = f0; s.dt = 0.2; s.snrDb = -16;
            q65synth::addSignal(w, P, s, 1);
            putAt(a, w, periodStart - q65::kWinLeadSec);
        };
        put("CQ K1ABC FN42", 60, kSpotT0, 1200);
        put("G4ABC DL1AAA JO62", 30, kSpotT0 + 30, 2100);
        put("CQ F5ABC JN18", 15, kSpotT0 + 45, 800);
    } else if (n == "FST4" || n == "FST4W") {
        const int P = n == "FST4" ? 60 : 120;
        pace = 0;
        a = window(kSpotT0 - 2.0, P + 5.0);
        addNoise(a, 1.0f, 43);
        uint8_t t[160];
        const bool ok = n == "FST4" ? fst4::encodeText("CQ K1ABC FN42", t) : fst4::encodeWspr("G4ABC", "IO91", 23, t);
        if (ok) {
            const fst4::Period* p = fst4::periodFor(P);
            std::vector<float> w(p ? (size_t)((p->sec + 2) * 12000) : 0, 0.f);
            fst4synth::addSignal(w, P, t, n == "FST4" ? 1000 : 1500, 0.1, -16);
            putAt(a, w, kSpotT0 - fst4::kWinLeadSec);
        } else signal = false;
    } else if (n == "MSK144") {
        pace = 0;
        a = window(kSpotT0, 8.0);
        addNoise(a, 1.0f, 44);
        uint8_t t[144];
        if (msk144::encodeText("CQ K1ABC FN42", t)) {
            msksynth::addPing(a, t, 1500, 1.0, 144 * 4, 0, 5);
            msksynth::addPing(a, t, 1500, 4.5, 144 * 3, 37, 3);
        } else signal = false;
    } else if (n == "JS8") {
        pace = 6;
        a = window(kSpotT0, 31.0);
        uint8_t bits72[72];
        if (js8::packHeartbeat("G0NRM", "IO91", -1, bits72)) {
            uint8_t info[87], tones[79];
            js8Info87(bits72, js8::kFlagFirst | js8::kFlagLast, info);
            const Js8SubmodeInfo& in = js8Info(Js8Submode::Normal);
            js8Tones(info, Js8Submode::Normal, tones);
            for (double slot = 0; slot + in.period <= 30.0 + 1e-9; slot += in.period) {
                const double baud = fs / in.nsps12k; double ph = 0.3;
                const long i0 = std::lround((slot + in.startDelay) * fs);
                for (int k = 0; k < 79; ++k) for (int j = 0; j < in.nsps12k; ++j) {
                    const long i = i0 + (long)k * in.nsps12k + j;
                    if (i >= 0 && i < (long)a.size()) a[(size_t)i] += 0.05f * (float)std::cos(ph);
                    ph += 2 * M_PI * (1000.0 + tones[k] * baud) / fs;
                }
            }
        } else signal = false;
        addNoise(a, 0.02f, 45);
    } else if (n == "FT2") {
        pace = 3.75;
        a = window(kSpotT0, 15.0);
        ftx_message_t mm; ftx_message_init(&mm);
        if (ftx_message_encode(&mm, nullptr, "CQ IU8LMC JN70") == FTX_MESSAGE_RC_OK) {
            uint8_t tones[FT4_NN]; ft4_encode(mm.payload, tones);
            const int nsps = 288;   // ★ FT2: FT4's frame at twice its speed (decoders/ft2_decoder.h)
            std::vector<double> pulse((size_t)3 * nsps);
            for (int i = 0; i < 3 * nsps; ++i) { const double t = i / (double)nsps - 1.5; pulse[(size_t)i] = (std::erf(5.336446 * (t + 0.5)) - std::erf(5.336446 * (t - 0.5))) / 2; }
            for (int s = 0; s < 4; ++s) {
                std::vector<double> dphi((size_t)(FT4_NN + 2) * nsps, 2 * M_PI * 1500.0 / fs);
                for (int i = 0; i < FT4_NN; ++i) for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += 2 * M_PI / nsps * tones[i] * pulse[(size_t)j];
                const long s0 = std::lround((s * 3.75 + 0.1) * fs); double phi = 0;
                for (int k = 0; k < FT4_NN * nsps; ++k) { const long i = s0 + k; if (i >= 0 && i < (long)a.size()) a[(size_t)i] += 0.05f * (float)std::sin(phi); phi += dphi[(size_t)(k + nsps)]; }
            }
        } else signal = false;
        addNoise(a, 0.02f, 46);
    } else if (n == "JTTY") {
        pace = 0;
        a = jttyMessage("CQ G4ABC IO91", fs, 1000);
        utc0 = kSpotT0;
        if (a.empty()) signal = false;
    } else if (n == "PSK") {
        pace = 0;
        a = pskSignal("CQ CQ DE G4ABC G4ABC PSE K", fs);
        utc0 = kSpotT0;
    } else {
        // ★ A mode this file has no transmitter for yet: it still gets a row — its search over noise, said as such.
        pace = m.slotSec > 0 ? std::min(15.0, m.slotSec) : 0;
        const double secs = m.slotSec > 0 ? m.slotSec + 3.0 : 8.0;
        a = window(kSpotT0 - 1.0, secs);
        addNoise(a, 1.0f, 47);
        signal = false;
    }
    normalise(a);
    return a;
}

// ── which rows set what (ONE RULE — both readers apply what this says) ───────────────────────────────────────────

/** ★★★ THE DEFAULTS RULE for a decoder row.
 *   • ★ THE FIRST EIGHT ROWS (RTTY, NAVTEX, WEFAX, SSTV, time, FT8+FT4, Band, LoRa) keep their old behaviour (Stuart,
 *     2026-10-31): below green they are FLAGGED, never switched off here — a box that has been fine never loses RTTY or
 *     FT8 on a fresh setup. Band and Advanced RDS keep the rules the readers already had for them.
 *   • `sw`: the owner's switch the row stands for (the setup page's BLOCKABLE id, vsModeBlocked). When the row is the
 *     switch's MAIN decoder (`primary`) and it measured below GREEN, the switch starts OFF on a fresh setup — offered,
 *     never hidden: the owner can tick it back on (Stuart, 2026-10-25: "never hide any controls").
 *   • A row that is not its switch's main decoder (a skimmer beside its one-signal decoder, RTTY AUTO beside RTTY, a
 *     digital-spots mode inside "Digital spots") never switches anything off: turning the switch off would take the
 *     green decoder away with it. It is flagged instead (the "Too heavy for this box" line lists it).
 *   • `slot`: the row is a decoder SLOT (vibe_decoder_host.h) — it counts toward the decoder limit's yardstick. Only a
 *     GREEN slot row does: one that is not green is either off by default or, alone, already more than a core's green
 *     budget, which a limit cannot fix. Whole-radio modes (NDB, AIS, ISM, LoRa) and per-listener filters (tone ID,
 *     noise reduction, the auto notch) are not slots. */
struct RowRule { const char* sw; bool primary; bool slot; };
inline RowRule ruleFor(const std::string& id) {
    struct E { const char* id; RowRule r; };
    static const E t[] = {
        { "dec_rtty",     { "rtty",   false,  true  } }, { "dec_rttyauto", { "rtty",   false, true  } },
        { "dec_navtex",   { "navtex", false,  true  } }, { "dec_wefax",    { "wefax",  false,  true  } },
        { "dec_sstv",     { "sstv",   false,  true  } }, { "dec_time",     { "time",   false,  true  } },
        { "dec_ft8",      { "spots",  false,  true  } }, { "dec_ft8band",  { "",       false, false } },   // ★ its own rule (spotsband)
        { "dec_lora",     { "",       false, false } },   // ★ its own budget (vibelora::budgetFromBench)
        { "dec_cw",       { "cw",     true,  true  } }, { "dec_cwskim",   { "cw",     false, true  } },
        { "dec_psk",      { "psk",    true,  true  } }, { "dec_pskskim",  { "psk",    false, true  } },
        { "dec_jtty",     { "jtty",   true,  true  } }, { "dec_sitora",   { "sitora", true,  true  } },
        { "dec_dsc",      { "dsc",    true,  true  } }, { "dec_same",     { "same",   true,  true  } },
        { "dec_selcal",   { "selcal", true,  true  } }, { "dec_mfsk",     { "mfsk",   true,  true  } },
        { "dec_hell",     { "hell",   true,  true  } }, { "dec_aprs",     { "aprs",   true,  true  } },
        { "dec_paging",   { "",       false, true  } },   // ★ the phone's own app on its own radio: no owner switch
        { "dec_ndb",      { "ndb",    true,  false } }, { "dec_ais",      { "ais",    true,  false } },
        { "dec_ism",      { "ism",    true,  false } }, { "dec_wmbus",    { "ism",    false, false } },
        { "dec_tone",     { "",       false, false } }, { "dec_nr",       { "",       false, false } },
        { "dec_notch",    { "",       false, false } },
    };
    for (const auto& e : t) if (id == e.id) return e.r;
    if (id.rfind("spot_", 0) == 0) return { "spots", false, true };   // ★ every digital-spots mode: inside "Digital spots"
    return { "", false, false };
}

// ── every row ──────────────────────────────────────────────────────────────────────────────────────────────────────

/** Every decoder row after the first eight. `clips`: decoders-bench.vbu8's records (empty = the clip rows read "not
 *  measured"). `step` names each row as it starts (the progress bar). `countOnly`: count the rows, run nothing — the
 *  progress bar's step total comes from the same list, so a decoder added here cannot leave the bar short. */
inline std::vector<Row> runMoreDecoderRows(const std::vector<DecClip>& clips, const std::function<void(const std::string&)>& step,
                                           int* countOnly = nullptr) {
    std::vector<Row> rows;
    auto run = [&](const std::string& label, const std::function<Row()>& f) {
        if (countOnly) { ++*countOnly; return; }
        if (step) step(label);
        const double w0 = wallNow();
        Row r = f();
        r.ms = (int)std::lround(1000.0 * (wallNow() - w0));
        rows.push_back(std::move(r));
    };
    const std::string attach = "{\"type\":\"audio_extension_attach\",";

    // ── a listener's decoders, on 48 kHz audio ──
    run("Decoder: RTTY AUTO", [&] {
        auto a = rttyText("RYRYRYRY CQ CQ DE G4ABC G4ABC THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG\r\n", 1500);
        addNoise(a, 0.05f, 1);
        return hostAudioRow("dec_rttyauto", "Decoder: RTTY AUTO", "fsk", attach + "\"extension_name\":\"fsk\",\"auto\":true}", a, Count::Chars);
    });
    run("Decoder: CW", [&] {
        return hostAudioRow("dec_cw", "Decoder: CW", "cw", attach + "\"extension_name\":\"cw\"}", cwSignal("CQ CQ DE G4ABC G4ABC K   "), Count::Chars);
    });
    run("Decoder: CW skimmer", [&] {
        return hostIqRow("dec_cwskim", "Decoder: CW skimmer (48 kHz)", "cwskim", attach + "\"extension_name\":\"cwskim\",\"width\":48000}",
                         clipNamed(clips, "cw40"), "cw_signals");
    });
    run("Decoder: PSK31", [&] {
        return hostAudioRow("dec_psk", "Decoder: PSK31", "psk", attach + "\"extension_name\":\"psk\",\"center_frequency\":1000}",
                            pskSignal("CQ CQ DE G4ABC G4ABC PSE K ", 48000), Count::Chars);
    });
    run("Decoder: PSK skimmer", [&] {
        return hostIqRow("dec_pskskim", "Decoder: PSK skimmer (48 kHz)", "pskskim", attach + "\"extension_name\":\"pskskim\",\"width\":48000}",
                         clipNamed(clips, "psk20"), "psk_signals");
    });
    run("Decoder: JTTY", [&] {
        return hostAudioRow("dec_jtty", "Decoder: JTTY", "jtty", attach + "\"extension_name\":\"jtty\"}", jttyMessage("CQ G4ABC IO91", 48000, 1000), Count::Any);
    });
    run("Decoder: SITOR-A", [&] {
        const auto L = sitorsynth::makeLink("002320204", "364775427", "GOOD MORNING THIS IS A TEST +?", "QSL +?", "73 +?", 4, 7);
        return hostAudioRow("dec_sitora", "Decoder: SITOR-A", "sitora", attach + "\"extension_name\":\"sitora\",\"center_frequency\":1000}",
                            sitorsynth::render(L, 48000, 1000, 15, -6, 1), Count::Chars);
    });
    run("Decoder: DSC", [&] {
        // ★ A test call (ITU-R M.493 format 120, category safety, "test" 118, EOS 117) — the decoder's own test case
        std::vector<int> s{ 120 };
        dsc::build::mmsi(s, "002320004"); s.push_back(108); dsc::build::mmsi(s, "232001234"); s.push_back(118); s.push_back(126);
        dsc::build::noFreq(s); dsc::build::noFreq(s); s.push_back(117);
        const auto bits = dsc::build::transmission(s, {}, 200);
        auto a = dsc::build::modulate(bits, false, 48000, 1700);
        a.insert(a.begin(), 9600, 0.f); a.insert(a.end(), 24000, 0.f);
        addNoise(a, 0.05f, 2);
        return hostAudioRow("dec_dsc", "Decoder: DSC (MF/HF)", "dsc", attach + "\"extension_name\":\"dsc\",\"band\":\"hf\"}", a, Count::Any);
    });
    run("Decoder: EAS / SAME", [&] {
        auto a = sameAlert("ZCZC-WXR-TOR-024031-024033+0030-3041345-KLWX/NWS-");
        addNoise(a, 0.05f, 3);
        return hostAudioRow("dec_same", "Decoder: EAS / SAME", "same", attach + "\"extension_name\":\"same\"}", a, Count::Any);
    });
    run("Decoder: SELCAL", [&] {
        auto a = selcalCall("AFHL");
        addNoise(a, 0.05f, 4);
        return hostAudioRow("dec_selcal", "Decoder: SELCAL", "selcal", attach + "\"extension_name\":\"selcal\"}", a, Count::Any);
    });
    run("Decoder: MFSK, Olivia, Contestia, THOR", [&] {
        auto a = textModeSynth(TextModeDecoder::Family::Mfsk, "mfsk16", "CQ CQ DE G4ABC G4ABC PSE K\n", 48000, 1500);
        addNoise(a, 0.1f, 5);
        return hostAudioRow("dec_mfsk", "Decoder: MFSK / Olivia / Contestia / THOR (AUTO)", "mfsk",
                            attach + "\"extension_name\":\"mfsk\",\"mode\":\"auto\"}", a, Count::Chars);
    });
    run("Decoder: Feld Hell", [&] {
        auto a = feldHellSynth("CQ CQ DE G4ABC G4ABC K ", 48000, 1000);
        addNoise(a, 0.05f, 6);
        return hostAudioRow("dec_hell", "Decoder: Feld Hell", "hell", attach + "\"extension_name\":\"hell\"}", a, Count::Lines);
    });
    run("Decoder: APRS", [&] {
        auto a = aprsPacket("M0ABC", 9, "!5049.00N/00118.00W>VibeSDR benchmark");
        auto b = aprsPacket("G4ABC", 7, "!5130.00N/00007.00W-test");
        a.insert(a.end(), b.begin(), b.end());
        addNoise(a, 0.05f, 7);
        return hostAudioRow("dec_aprs", "Decoder: APRS (1200 Bd)", "aprs", attach + "\"extension_name\":\"aprs\",\"baud_rate\":1200}", a, Count::Events);
    });
#if defined(VIBE_PAGING)
    /* ★★ PAGING — ANDROID LOCAL HARDWARE ONLY (the define is the Android library's), so the row exists only on the build
     *  that offers the decoder (AGENTS.md: never show what cannot work here). SYNTHETIC ONLY: paging frequencies carry
     *  private messages, so no recording of one is ever used. FLEX 1600, our own transmitter, on the channel IQ. */
    run("Decoder: Paging", [&] {
        Row r; r.id = "dec_paging"; r.label = "Decoder: Paging (POCSAG / FLEX, AUTO)";
        auto peer = std::make_shared<CountPeer>();
        DecoderHost::Env env; env.dialHz = [] { return 153.0e6; };
        DecoderHost h(env, "bench");
        h.addPeer(peer);
        if (h.start("paging", attach + "\"extension_name\":\"paging\",\"mode\":\"auto\"}") != DecoderHost::Start::Ok) return r;
        std::vector<flexsyn::TxPage> pages(2);
        pages[0].cap = 123456; pages[0].kind = "alpha"; pages[0].text = "BENCHMARK PAGE ONE";
        pages[1].cap = 234567; pages[1].kind = "alpha"; pages[1].text = "BENCHMARK PAGE TWO";
        std::vector<flexsyn::Sym> sy;
        for (int f = 0; f < 4; ++f) { auto fr = flexsyn::frame(1600, 2, 1, 10 + f, f == 0 ? pages : std::vector<flexsyn::TxPage>{}); sy.insert(sy.end(), fr.begin(), fr.end()); }
        const auto iq = flexsyn::fmIq(sy, 31250, 0, false, 24.0, 21, 16000);
        const double c0 = processCpu(), w0 = wallNow(); double fedSec = 0; size_t pos = 0;
        while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            const size_t n = std::min<size_t>(625, iq.size() - pos);
            h.feedChannelIq(iq.data() + pos, (int)n, 31250);
            pos = (pos + n) % iq.size(); fedSec += n / 31250.0;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        settleCpu(5.0);
        r.pct = 100.0 * (processCpu() - c0) / fedSec;
        h.stop();
        r.decoded = (int)peer->chars.load();
        return r;
    });
#endif

    // ── the digital-spots modes, each its own engine (FT8 and FT4 are the dec_ft8 row) ──
    for (const auto& m : digi::modes()) {
        if (m.name == "FT8" || m.name == "FT4") continue;
        run("Digital spots: " + m.name, [&] {
            double utc0 = kSpotT0, pace = 0; bool sig = true;
            const auto a = spotAudio(m, utc0, pace, sig);
            return engineRow(m, a, utc0, pace, sig);
        });
    }

    // ── whole-radio modes ──
    run("Decoder: NDB beacons", [&] {
        Row r; r.id = "dec_ndb"; r.label = "Decoder: NDB beacons";
        const DecClip* c = clipNamed(clips, "ndb");
        if (!c) { r.label += " (no clip)"; return r; }
        vibendb::Service s;
        const double half = c->rate * 0.45;
        s.startFed(c->rate, c->centre, c->centre - half, c->centre + half, false, 0, 0);
        const double c0 = processCpu(), w0 = wallNow(); double fedSec = 0; size_t pos = 0;
        const size_t blk = (size_t)(c->rate / 50);
        while (fedSec < 2 * kRowMaxSec && (fedSec < 2 * kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            while (s.fedQueuedSec() > 0.5) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t n = std::min(blk, c->iq.size() - pos);
            s.feedSlice(c->iq.data() + pos, (int)n, c->rate, c->centre);
            pos = (pos + n) % c->iq.size(); fedSec += (double)n / c->rate;
        }
        for (int i = 0; i < 4000 && s.fedQueuedSec() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        settleCpu(5.0);
        r.pct = 100.0 * (processCpu() - c0) / fedSec;
        s.buildReportForTest();
        r.decoded = s.identifiedNow();
        s.stop();
        return r;
    });
    run("Decoder: AIS", [&] {
        Row r; r.id = "dec_ais"; r.label = "Decoder: AIS ships @ 2.4 MS/s";
        const double fs = 2.4e6;
        std::vector<std::complex<float>> x((size_t)fs);   // ★ one second, looped
        { std::mt19937 g(8); std::normal_distribution<float> d(0.f, 0.02f); for (auto& v : x) v = { d(g), d(g) }; }
        // ★ Type 1 position reports (M.1371 Annex 8) on both channels, GMSK BT 0.4 at 9600 Bd (Annex 2)
        for (int k = 0; k < 6; ++k) {
            std::vector<int> bits;
            auto u = [&](uint64_t v, int len) { for (int i = len - 1; i >= 0; --i) bits.push_back(int((v >> i) & 1)); };
            auto s = [&](int64_t v, int len) { u(uint64_t(v) & ((1ull << len) - 1), len); };
            u(1, 6); u(0, 2); u(235012345u + (uint32_t)k, 30); u(0, 4); s(0, 8); u(123, 10); u(1, 1);
            s(std::lround(-1.3 * 600000), 28); s(std::lround(50.8 * 600000), 27); u(2456, 12); u(247, 9); u(30, 6); u(0, 2); u(0, 3); u(0, 1); u(0x1234, 19);
            std::vector<uint8_t> msg((bits.size() + 7) / 8, 0);
            for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) msg[i / 8] |= uint8_t(1 << (7 - i % 8));
            vibehdlc::appendFcs(msg);
            std::vector<int> air;
            for (int i = 0; i < 24; ++i) air.push_back(i & 1);
            vibehdlc::flagBits(air); vibehdlc::stuffBits(msg, air); vibehdlc::flagBits(air);
            for (int i = 0; i < 8; ++i) air.push_back(1);
            std::vector<int> lv; int level = 0; vibehdlc::nrzi(air, lv, level);
            const double sps = fs / 9600.0, BT = 0.4, sg = std::sqrt(std::log(2.0)) / (2 * M_PI * BT), kk = std::sqrt(2.0) * sg;
            const double off = (k % 2 ? 162.025e6 : 161.975e6) - 162.0e6;
            const size_t s0 = (size_t)((0.05 + 0.15 * k) * fs), len = (size_t)((lv.size() + 6) * sps);
            double ph = 0;
            for (size_t i = 0; i < len && s0 + i < x.size(); ++i) {
                const double t = i / sps - 3.0;   // in symbols, 3 T in
                double f = 0;
                const long kc = (long)std::floor(t);
                for (long j = kc - 3; j <= kc + 3; ++j) {
                    if (j < 0 || j >= (long)lv.size()) continue;
                    const double tt = t - (double)j - 0.5;
                    f += (lv[(size_t)j] ? 1.0 : -1.0) * 0.5 * (std::erf((tt + 0.5) / kk) - std::erf((tt - 0.5) / kk));
                }
                ph += 2 * M_PI * (off + 2400.0 * f) / fs;
                x[s0 + i] += std::complex<float>(0.3f * (float)std::cos(ph), 0.3f * (float)std::sin(ph));
            }
        }
        vibeais::Service svc;
        svc.start(162.0e6, fs);
        const size_t blk = (size_t)(fs / 100);
        const double t0 = wallNow(); double fed = 0, c0 = 0; size_t pos = 0; bool measuring = false;
        std::map<uint64_t, ThreadTime> th0;
        while (fed < kRtWarmSec + kRtSec) {
            if (!measuring && fed >= kRtWarmSec) { c0 = processCpu(); th0 = threadTimes(); measuring = true; }
            svc.feed(x.data() + pos, blk);
            pos = (pos + blk) % x.size(); fed += blk / fs;
            const double ahead = fed - (wallNow() - t0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // ★★ SCORED BY THE HOTTEST THREAD against one core, as ADS-B (a whole-radio mode on its own worker); the
        //    process total stays in the row's detail.
        scoreHottest(r, th0, threadTimes(), kRtSec, 100.0 * (processCpu() - c0) / kRtSec);
        const std::string j = svc.report();
        if (const char* p = std::strstr(j.c_str(), "\"msgs\":")) r.decoded = std::atoi(p + 7);
        if (svc.droppedBlocks()) r.label += " (fell behind)";
        svc.stop();
        return r;
    });
    auto ismRow = [&](const char* id, const char* label, const char* clipId, const char* bandId) {
        run(label, [&] {
            Row r; r.id = id; r.label = label;
            const DecClip* c = clipNamed(clips, clipId);
            const vibeism::Band* band = nullptr;
            for (const auto& b : vibeism::bands()) if (std::string(b.id) == bandId) band = &b;
            if (!c || !band) { r.label += " (no clip)"; return r; }
            vibeism::Service svc;
            svc.setLora(false, 0);   // ★ LoRa is its own row (dec_lora)
            svc.start(*band, c->centre, c->rate);
            const size_t blk = (size_t)(c->rate / 100);
            const double t0 = wallNow(); double fed = 0, c0 = 0; size_t pos = 0; bool measuring = false;
            std::map<uint64_t, ThreadTime> th0;
            while (fed < kRtWarmSec + kRtSec) {
                if (!measuring && fed >= kRtWarmSec) { c0 = processCpu(); th0 = threadTimes(); measuring = true; }
                const size_t n = std::min(blk, c->iq.size() - pos);
                svc.feed(c->iq.data() + pos, n);
                pos = (pos + n) % c->iq.size(); fed += (double)n / c->rate;
                const double ahead = fed - (wallNow() - t0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            scoreHottest(r, th0, threadTimes(), kRtSec, 100.0 * (processCpu() - c0) / kRtSec);   // ★ as AIS, as ADS-B
            const std::string j = svc.report();
            int devices = 0;
            for (size_t p = j.find("\"model\":"); p != std::string::npos; p = j.find("\"model\":", p + 1)) ++devices;
            r.decoded = devices;
            if (svc.droppedBlocks()) r.label += " (fell behind)";
            svc.stop();
            return r;
        });
    };
    ismRow("dec_ism", "Decoder: ISM sensors @ 2.4 MS/s (433 MHz)", "ism433", "433");
    ismRow("dec_wmbus", "Decoder: ISM meters (wM-Bus) @ 2.4 MS/s (868 MHz)", "ism868", "868");

    // ── a listener's own filters (per listener, on their DSP thread — no decoder slot) ──
    run("CTCSS / DCS tone ID", [&] {
        Row r; r.id = "dec_tone"; r.label = "CTCSS / DCS tone ID (per NFM listener)";
        const double fs = 32000;   // ★ an NFM listener's channel at 2.048 MS/s (the benchmark's own geometry)
        std::vector<std::complex<float>> x((size_t)(fs * 4));
        std::mt19937 g(9); std::normal_distribution<float> d(0.f, 0.05f);
        double ph = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = i / fs;
            const double dev = 2500.0 * std::sin(2 * M_PI * 1000.0 * t) * (0.5 + 0.5 * std::sin(2 * M_PI * 0.7 * t))
                             + 500.0 * std::sin(2 * M_PI * 88.5 * t);   // ★ speech-band audio + CTCSS 88.5 Hz
            ph += 2 * M_PI * dev / fs;
            x[i] = std::complex<float>((float)std::cos(ph) + d(g), (float)std::sin(ph) + d(g));
        }
        ToneId tone(fs);
        const double c0 = processCpu(), w0 = wallNow(); double fedSec = 0; size_t pos = 0;
        std::string last; int locks = 0;
        while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            const size_t n = std::min<size_t>(640, x.size() - pos);
            tone.feedIq(x.data() + pos, (int)n);
            const std::string l = tone.state().label();
            if (l != last) { if (!l.empty()) ++locks; last = l; }
            pos = (pos + n) % x.size(); fedSec += n / fs;
        }
        r.pct = 100.0 * (processCpu() - c0) / fedSec;
        r.decoded = locks;
        if (!last.empty()) r.label += " \xe2\x80\x94 " + last;
        return r;
    });
    auto audioFilterRow = [&](const char* id, const char* label, bool nr) {
        run(label, [&] {
            Row r; r.id = id; r.label = label;
            std::vector<float> a((size_t)48000 * 4);
            std::mt19937 g(10); std::normal_distribution<float> d(0.f, 0.05f);
            for (size_t i = 0; i < a.size(); ++i) {
                const double t = i / 48000.0;
                a[i] = (float)(0.2 * std::sin(2 * M_PI * 300.0 * t) * (0.5 + 0.5 * std::sin(2 * M_PI * 3.0 * t))
                             + 0.1 * std::sin(2 * M_PI * 1234.0 * t)) + d(g);   // ★ a voice-like burst, a heterodyne, noise
            }
            const double c0 = processCpu(), w0 = wallNow(); double fedSec = 0; size_t pos = 0;
            AudioNR n; AutoNotch an; std::vector<float> out, blk(960);
            while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
                std::memcpy(blk.data(), a.data() + pos, 960 * sizeof(float));
                if (nr) { out.clear(); n.process(blk.data(), 960, out); } else an.process(blk.data(), 960);
                pos = (pos + 960) % a.size(); fedSec += 960 / 48000.0;
            }
            r.pct = 100.0 * (processCpu() - c0) / fedSec;
            return r;
        });
    };
    audioFilterRow("dec_nr", "Noise reduction (per listener)", true);
    audioFilterRow("dec_notch", "Auto notch (per listener)", false);
    return rows;
}

/** How many rows runMoreDecoderRows makes on this build (the progress bar's steps). */
inline int moreDecoderSteps() { int n = 0; runMoreDecoderRows({}, nullptr, &n); return n; }

} // namespace benchdec
} // namespace vibe
