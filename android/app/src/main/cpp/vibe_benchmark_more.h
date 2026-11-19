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
#include "vibe_g3ruh.h"        // ★ V12 9600 packet: the dec_aprs9600 row's transmitter
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
#include "vibe_dxscan_fm.h"          // ★★★ V12 the idle DX scanner's FM hop (the dec_dxscan row)
#include "vibe_ais_service.h"
#include "vibe_avi_service.h"   // ★ V12 aviation: the datalink rows (dec_vdl2, dec_acars)
#include "vibe_rs41_service.h"  // ★ V12 RS41 radiosondes: the SONDES row (dec_rs41)
#include "vibe_rs41_tx.h"
#include "vibe_sonde_tx.h"     // ★ V12 the DFM / M10 / M20 rows (dec_dfm, dec_m10)
#include "vibe_avi_mode.h"
#include "vibe_avi_tx.h"
#include "vibe_ism_service.h"
#include "vibe_hdlc.h"
#include "vibe_freedv.h"             // ★ V12 FreeDV as a mode (the dec_freedv row) — the system's libcodec2, dlopen'd
#include "decoders/balloon_tx.h"     // ★ V12 balloons (the dec_balloon row) — our own transmitter
#include "decoders/tones_tx.h"       // ★ V12 DTMF + 5-tone (the dec_tones row) — our own transmitter
#include "vibe_m17.h"                // ★ V12 M17 as a mode (the dec_m17 row) — our own receiver, Codec 2 from the same library
#include "vibe_dvsig_tx.h"           // ★ V12 DV signalling (the dec_dvsig row) — our own receiver and transmitter
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
    static vibeavi::Tracker benchAircraft;   // ★ HFDL's messages go here, never onto a server's map
    env.aircraft = &benchAircraft;
    env.tap = [tap](const std::string&, const std::string&) { return tap; };
    env.cwSpotTap = [tap](bool) { return tap; };
    env.pskSpotTap = [tap](bool) { return tap; };
    env.rttySkimSpotTap = [tap] { return tap; };
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
/** ★★ V12 THE RTTY SKIMMER'S CLIP (2026-11-14): 20 s of 48 kHz IQ with twelve RTTY signals of mixed settings (45.45 /
 *  50 / 75 baud, 170 / 450 / 850 Hz, normal and reverse) at +15 dB in 2.5 kHz — synthesised, so the row measures on every
 *  box whether or not the decoder clip is there. ITA2 letters + figures, 1.5 stop bits, phase-continuous FSK. */
inline DecClip rttySkimClip() {
    DecClip c; c.id = "rttysk"; c.rate = 48000; c.centre = 7040000;
    const double rate = c.rate, secs = 20;
    c.iq.assign((size_t)(rate * secs), std::complex<float>(0, 0));
    uint32_t s = 12345;
    auto gauss = [&] { double u = 0; for (int k = 0; k < 6; ++k) { s = s * 1664525u + 1013904223u; u += (s >> 8) / 16777216.0; } return (u - 3.0) * 1.414; };
    for (auto& v : c.iq) v = std::complex<float>((float)(gauss() * 0.707), (float)(gauss() * 0.707));
    const double bauds[] = { 45.45, 45.45, 50, 75 }, shifts[] = { 170, 170, 450, 850 };
    for (int k = 0; k < 12; ++k) {
        const double hz = -20000 + k * 3600, baud = bauds[k % 4], shift = shifts[k % 4];
        const bool rev = k % 3 == 1;
        const std::string text = "CQ CQ DE G" + std::to_string(k % 10) + "ABC G" + std::to_string(k % 10) + "ABC PSE K 599 0" + std::to_string(10 + k) + "\n";
        std::vector<uint8_t> hb;   // half bits
        auto frame = [&](int code) { hb.push_back(0); hb.push_back(0); for (int b = 0; b < 5; ++b) { hb.push_back((code >> b) & 1); hb.push_back((code >> b) & 1); } hb.push_back(1); hb.push_back(1); hb.push_back(1); };
        static const char* L = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
        static const char* F = "\0" "3\n- \a87\r$4',!:(5\")2#6019?&\0./;\0";
        for (int i = 0; i < 20; ++i) hb.push_back(1);
        bool figs = false; frame(31);
        for (char ch : text) {
            int code = ch == ' ' ? 4 : ch == '\n' ? 2 : -1; bool f = figs;
            for (int i = 1; i < 32 && code < 0; ++i) if (L[i] == ch) { code = i; f = false; }
            for (int i = 1; i < 32 && code < 0; ++i) if (F[i] == ch) { code = i; f = true; }
            if (code < 0) continue;
            if (f != figs) { frame(f ? 27 : 31); figs = f; }
            frame(code);
        }
        const double A = std::sqrt(std::pow(10.0, 1.5) * 2500.0 / rate), hbSec = 1.0 / (2 * baud);
        double ph = 0, t = 0;
        for (size_t i = (size_t)(rate * (0.5 + 0.2 * k)); i < c.iq.size(); ++i) {
            size_t h = (size_t)(t / hbSec);
            if (h >= hb.size()) { t -= hb.size() * hbSec; h = 0; }
            const bool mark = hb[h] != 0;
            ph += 2 * M_PI * (hz + ((mark != rev) ? shift / 2 : -shift / 2)) / rate;
            if (ph > M_PI) ph -= 2 * M_PI;
            c.iq[i] += std::complex<float>((float)(A * std::cos(ph)), (float)(A * std::sin(ph)));
            t += 1.0 / rate;
        }
    }
    return c;
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
/** ★ V12 9600 packet: three bursts (plain AX.25, FX.25 16, IL2P) from our own G3RUH transmitter (vibe_g3ruh.h), as
 *  channel IQ at `fs` with white noise at `ebn0` dB, 50 ms of noise between them. */
inline std::vector<std::complex<float>> pkt9600Clip(double fs, double ebn0) {
    std::vector<std::complex<float>> out;
    std::mt19937 g(96); std::normal_distribution<float> d(0.f, 1.f);
    const float sd = float(std::sqrt(fs / (9600.0 * std::pow(10.0, ebn0 / 10)) / 2));
    auto frame = [](const std::string& src, const std::string& info) {
        std::vector<uint8_t> b;
        auto addr = [&](const std::string& c, int ssid, bool last) { for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < (int)c.size() ? c[i] : ' ') << 1)); b.push_back(uint8_t(0x60 | (ssid << 1) | (last ? 1 : 0))); };
        addr("APRS", 0, false); addr(src, 0, true); b.push_back(0x03); b.push_back(0xF0);
        b.insert(b.end(), info.begin(), info.end());
        return b;
    };
    vibepkt::Scrambler scr; int lvl = 0;
    for (int k = 0; k < 3; ++k) {
        const auto f = frame(k == 0 ? "M0ABC" : k == 1 ? "G4FXA" : "G4ILP", "!5130.00N/00007.00W>VibeSDR 9600 benchmark");
        std::vector<int> line;
        if (k == 2) line = vibeil2p::lineBits(vibeil2p::encode(f, true, true), 24);
        else {
            std::vector<int> data;
            for (int i = 0; i < 24; ++i) vibehdlc::flagBits(data);
            if (k == 0) { std::vector<uint8_t> h = f; vibehdlc::appendFcs(h); vibehdlc::stuffBits(h, data); vibehdlc::flagBits(data); }
            else { auto b = vibefx25::encodeBits(f, 16, 1, 1); data.insert(data.end(), b.begin(), b.end()); }
            for (int i = 0; i < 4; ++i) vibehdlc::flagBits(data);
            line = vibeg3ruh::lineFromData(data, scr, lvl);
        }
        const auto iq = vibeg3ruh::fmIq(vibeg3ruh::baseband(line, fs, 9600), fs, 3000);
        for (size_t i = 0; i < size_t(fs * 0.05); ++i) out.push_back({ sd * d(g), sd * d(g) });
        for (const auto& v : iq) out.push_back(v + std::complex<float>(sd * d(g), sd * d(g)));
    }
    return out;
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
/** ★ V12 balloons: what a payload sends — UKHAS sentences at 300 Bd 8N2 600 Hz, then a Horus v2 packet, 20 dB in 3 kHz;
 *  the receiver's whole search runs on it (every RTTY candidate, Horus beside). */
inline std::vector<float> balloonAudio() {
    using namespace vibe::balloon;
    tx::Keying k;
    tx::Rtty r; r.baud = 300; r.shift = 600; r.centre = 1500; r.bits = 8; r.stop = 2;
    for (int i = 0; i < 3; ++i) {
        tx::rtty(tx::bytesOf(tx::sentence("BENCH", i, 12, 0, i * 5, 52.0 + i * 0.001, -1.0, 20000 + 10 * i)), r, k, i ? 0.2 : 1.0);
        tx::horus(tx::horusPacket(2, 256, i, 12, 0, i * 5 + 2, 52.0f, -1.0f, 20000), 1100, 270, k);
    }
    std::vector<float> a; double ph = 0;
    tx::render(k, 48000, 0.1f, a, ph);
    std::mt19937 g(53); std::normal_distribution<float> n(0.f, 0.02f);
    for (float& v : a) v += n(g);
    return a;
}
/** ★ V12 tones: what a repeater user and a PMR fleet send — DTMF strings and 5-tone calls (ZVEI-1, CCIR, EEA), 20 dB in 3 kHz,
 *  with speech-like gaps of noise between; both decoders run on it. */
inline std::vector<float> tonesAudio() {
    using namespace vibe::tones;
    std::vector<float> a;
    for (int i = 0; i < 3; ++i) {
        tx::silence(a, 0.5); tx::dtmf(a, "*73#1234");
        tx::silence(a, 0.5); tx::fiveTone(a, 0, "12345");
        tx::silence(a, 0.5); tx::fiveTone(a, 5, "40719");
        tx::silence(a, 0.5); tx::fiveTone(a, 6, "23356");
    }
    tx::silence(a, 1.0);
    std::mt19937 g(71); std::normal_distribution<float> n(0.f, 0.03f);
    for (float& v : a) v += n(g);
    return a;
}
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
        /* ★★ "every LoRa channel" (2026-10-31): the owner's "lora+all", an option UNDER LoRa. Inverted against every other
         *  switch — GREEN ticks it ON on a fresh setup, anything else leaves it off with the figure beside it (both readers'
         *  own rule, as Band and Advanced RDS have). Never primary: it never switches anything off. */
        { "dec_lora_all", { "lora+all", false, false } },
        { "dec_cw",       { "cw",     true,  true  } }, { "dec_cwskim",   { "cw",     false, true  } },
        { "dec_psk",      { "psk",    true,  true  } }, { "dec_pskskim",  { "psk",    false, true  } },
        { "dec_rttyskim", { "rtty",   false, true  } },   // ★ inside RTTY's switch, which carries the green one-signal decoder
        { "dec_balloon",  { "balloon", true, true  } },   // ★ V12 balloons: a listener's decoder (a slot)
        { "dec_tones",    { "tones",  true,  true  } },   // ★ V12 DTMF + 5-tone: a listener's decoder (a slot)
        { "dec_ale",      { "ale",    true,  true  } },   // ★ V12 2G HF ALE: a listener's decoder (a slot)
        { "dec_jtty",     { "jtty",   true,  true  } }, { "dec_sitora",   { "sitora", true,  true  } },
        { "dec_dsc",      { "dsc",    true,  true  } }, { "dec_same",     { "same",   true,  true  } },
        { "dec_selcal",   { "selcal", true,  true  } }, { "dec_mfsk",     { "mfsk",   true,  true  } },
        { "dec_hell",     { "hell",   true,  true  } }, { "dec_aprs",     { "aprs",   true,  true  } },
        // ★ V12 9600 packet + FX.25 + IL2P (AUTO): inside the APRS / packet switch, a decoder slot like it
        { "dec_aprs9600", { "aprs",   false, true  } },
        { "dec_paging",   { "",       false, true  } },   // ★ the phone's own app on its own radio: no owner switch
        { "dec_ndb",      { "ndb",    true,  false } }, { "dec_ais",      { "ais",    true,  false } },
        // ★ V12 aviation: VDL2 (12 channels, the heavier band) sets the datalink mode's default; ACARS is shown beside it
        { "dec_vdl2",     { "avdata", true,  false } }, { "dec_acars",    { "avdata", false, false } },
        { "dec_hfdl",     { "hfdl",   true,  true  } },
        // ★ V12 RS41 radiosondes: the SONDES mode's default (a whole-radio mode — not the decoder limit's yardstick)
        { "dec_rs41",     { "sondes", true,  false } },
        // ★ V12 the other types run inside the same SONDES receiver: shown beside RS41's row, RS41 keeps the default
        { "dec_dfm",      { "sondes", false, false } }, { "dec_m10",      { "sondes", false, false } },
        { "dec_ism",      { "ism",    true,  false } }, { "dec_wmbus",    { "ism",    false, false } },
        { "dec_tone",     { "",       false, false } }, { "dec_nr",       { "",       false, false } },
        { "dec_notch",    { "",       false, false } },
        { "dec_dxscan",   { "",       false, false } },   // ★★★ V12 the DX scanner: flagged, never switched off (no BLOCKABLE switch)
        /* ★★ V12 FreeDV (vibe_freedv.h): a listener's MODE, not a decoder slot. Not green = "freedv" off by default on a fresh
         *  setup (the owner can always tick it back — the benchmark sets the default, never hides). */
        { "dec_freedv",   { "freedv", true,  false } },
        /* ★★ V12 FreeDV RADE V1: its own switch UNDER FreeDV ("rade"). Not green = RADE off by default on a fresh setup, the
         *  codec2 sub-modes untouched (the owner can always tick it back). */
        { "dec_rade",     { "rade",   true,  false } },
        /* ★★ V12 M17 (vibe_m17.h): a listener's MODE, like FreeDV. Not green = "m17" off by default on a fresh setup (the
         *  owner can always tick it back — the benchmark sets the default, never hides). */
        { "dec_m17",      { "m17",    true,  false } },
        /* ★★ V12 DV signalling (vibe_dvsig.h): a listener's MODE, like M17. Not green = "dvsig" off by default on a fresh
         *  setup (the owner can always tick it back — the benchmark sets the default, never hides). */
        { "dec_dvsig",    { "dvsig",  true,  false } },
        /* ★★ V12 DAB own station per listener (docs/v12/DAB-OWN-STATION.md, vibe_benchmark_dab.h): switches nothing off and is
         *  no decoder slot — its figure sets N (the dabStations block), which the own-station rule compares with the user limit. */
        { "dec_dab_station", { "",    false, false } },
    };
    for (const auto& e : t) if (id == e.id) return e.r;
    if (id.rfind("spot_", 0) == 0) return { "spots", false, true };   // ★ every digital-spots mode: inside "Digital spots"
    return { "", false, false };
}

// ── LoRa (ISM mode), threaded and in real time ─────────────────────────────────────────────────────────────────────

/** ★★ LoRa as ISM mode runs it (vibe_lora_service.h): the THREADED engine (vibe-lora / vibe-lora2 / vibe-lora3) on the 868
 *  plan at the server's own 868 capture (868.6 MHz, 2.4 MS/s), fed in real time like the ISM and AIS rows, and SCORED BY
 *  THE HOTTEST THREAD — the one that would fall behind first. The process total and each thread stay in the detail.
 *  `budgetUnits`: 0 = the default channels only (dec_lora); ∞ = every channel the capture holds (dec_lora_all, the
 *  owner's "lora+all"). The signal is noise at the band's level with two LoRaWAN SF7 uplinks in it — one on a default
 *  channel (868.1), one on a channel only "every channel" runs (867.9) — so the row also shows the channels decode.
 *  ★ `decoded` counts good frames over the warm-up and the measured stretch. "(fell behind)" if any stage dropped. */
inline Row loraRealTimeRow(const char* id, const char* label, double budgetUnits) {
    Row r; r.id = id; r.label = label;
    const double w0 = wallNow();
    const double fs = 2.4e6, centre = 868.6e6;
    std::vector<vibelora::cf> x((size_t)fs);   // ★ one second, looped
    { std::mt19937 g(12); std::normal_distribution<float> d(0.f, (float)std::sqrt(0.5e-4)); for (auto& v : x) v = { d(g), d(g) }; }
    auto uplink = [&](double hz, double at, uint32_t devAddr) {
        vibelora::Phy p; p.sf = 7; p.bw = 125e3; p.cr = 1; p.sync = 0x34; p.preamble = 8; p.ldro = vibelora::ldroFor(7, 125e3);
        std::vector<uint8_t> b = { 0x40 };                                   // LoRaWAN unconfirmed data up
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(devAddr >> (8 * i)));
        b.push_back(0); b.push_back(7); b.push_back(0); b.push_back(1);      // FCtrl, FCnt, FPort
        for (int i = 0; i < 8; ++i) b.push_back(uint8_t(0xE0 + i));          // the (opaque) payload
        for (int i = 0; i < 4; ++i) b.push_back(0xC3);                       // MIC
        const auto w = vibelora::modulate(p, vibelora::encode(p, b), fs, hz - centre, 0, 0.4, 0.3);
        const float amp = (float)std::sqrt(std::pow(10.0, 10.0 / 10) * 1e-4 * p.bw / fs);   // 10 dB SNR in its bandwidth
        const size_t s0 = (size_t)(at * fs);
        for (size_t i = 0; i < w.size() && s0 + i < x.size(); ++i) x[s0 + i] += w[i] * amp;
    };
    uplink(868.1e6, 0.10, 0x260B1234);
    uplink(867.9e6, 0.55, 0x260B5678);
    vibelora::Engine e;
    if (!e.start("868", centre, fs, budgetUnits)) { r.label += " (no channels)"; return r; }
    const size_t blk = (size_t)(fs / 100);
    const double t0 = wallNow(); double fed = 0, c0 = 0; size_t pos = 0; bool measuring = false;
    std::map<uint64_t, ThreadTime> th0;
    int frames = 0;
    while (fed < kRtWarmSec + kRtSec) {
        if (!measuring && fed >= kRtWarmSec) { c0 = processCpu(); th0 = threadTimes(); measuring = true; }
        e.feed(x.data() + pos, blk);
        pos = (pos + blk) % x.size(); fed += blk / fs;
        const double ahead = fed - (wallNow() - t0);
        if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scoreHottest(r, th0, threadTimes(), kRtSec, 100.0 * (processCpu() - c0) / kRtSec);
    for (const auto& h : e.take()) if (h.f.crcOk || !h.f.crcOn) ++frames;
    r.decoded = frames;
    if (e.dropped()) r.label += " (fell behind)";
    e.stop();
    r.ms = (int)std::lround(1000.0 * (wallNow() - w0));
    return r;
}

// ── every row ──────────────────────────────────────────────────────────────────────────────────────────────────────

/** Every decoder row after the first eight. `clips`: decoders-bench.vbu8's records (empty = the clip rows read "not
 *  measured"). `step` names each row as it starts (the progress bar). `countOnly`: count the rows, run nothing — the
 *  progress bar's step total comes from the same list, so a decoder added here cannot leave the bar short. */
inline std::vector<Row> runMoreDecoderRows(const std::vector<DecClip>& clips, const std::function<void(const std::string&)>& step,
                                           int* countOnly = nullptr) {
    std::vector<Row> rows;
    auto run = [&](std::string id, const std::string& label, const std::function<Row()>& f) {
        // ★ V12 THE BENCHMARK TAILORED TO THE RADIOS HERE (vibe_benchmark_hw.h): a row no radio here can use is not run —
        //   not counted, not timed; the caller lists it as "not run" with the reason. Ids are lower-case (spot_*).
        for (auto& ch : id) ch = (char)std::tolower((unsigned char)ch);
        if (rowSkipped(id)) return;
        if (countOnly) { ++*countOnly; return; }
        if (step) step(label);
        const double w0 = wallNow();
        Row r = f();
        r.ms = (int)std::lround(1000.0 * (wallNow() - w0));
        rows.push_back(std::move(r));
    };
    const std::string attach = "{\"type\":\"audio_extension_attach\",";

    // ── a listener's decoders, on 48 kHz audio ──
    run("dec_rttyauto", "Decoder: RTTY AUTO", [&] {
        auto a = rttyText("RYRYRYRY CQ CQ DE G4ABC G4ABC THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG\r\n", 1500);
        addNoise(a, 0.05f, 1);
        return hostAudioRow("dec_rttyauto", "Decoder: RTTY AUTO", "fsk", attach + "\"extension_name\":\"fsk\",\"auto\":true}", a, Count::Chars);
    });
    run("dec_cw", "Decoder: CW", [&] {
        return hostAudioRow("dec_cw", "Decoder: CW", "cw", attach + "\"extension_name\":\"cw\"}", cwSignal("CQ CQ DE G4ABC G4ABC K   "), Count::Chars);
    });
    run("dec_cwskim", "Decoder: CW skimmer", [&] {
        return hostIqRow("dec_cwskim", "Decoder: CW skimmer (48 kHz)", "cwskim", attach + "\"extension_name\":\"cwskim\",\"width\":48000}",
                         clipNamed(clips, "cw40"), "cw_signals");
    });
    run("dec_psk", "Decoder: PSK31", [&] {
        return hostAudioRow("dec_psk", "Decoder: PSK31", "psk", attach + "\"extension_name\":\"psk\",\"center_frequency\":1000}",
                            pskSignal("CQ CQ DE G4ABC G4ABC PSE K ", 48000), Count::Chars);
    });
    run("dec_pskskim", "Decoder: PSK skimmer", [&] {
        return hostIqRow("dec_pskskim", "Decoder: PSK skimmer (48 kHz)", "pskskim", attach + "\"extension_name\":\"pskskim\",\"width\":48000}",
                         clipNamed(clips, "psk20"), "psk_signals");
    });
    /* ★★ V12 the RTTY skimmer (docs/v12/RTTY-SKIMMER.md): twelve signals of mixed settings at 48 kHz, its search workers
     *  at this box's default. The figure also sets that default (vibe_benchmark.h: "rttySkimWorkers"). */
    run("dec_rttyskim", "Decoder: RTTY skimmer", [&] {
        const DecClip c = rttySkimClip();
        return hostIqRow("dec_rttyskim", "Decoder: RTTY skimmer (48 kHz, 12 signals)", "rttyskim",
                         attach + "\"extension_name\":\"rttyskim\",\"width\":48000}", &c, "rtty_signals");
    });
    run("dec_jtty", "Decoder: JTTY", [&] {
        return hostAudioRow("dec_jtty", "Decoder: JTTY", "jtty", attach + "\"extension_name\":\"jtty\"}", jttyMessage("CQ G4ABC IO91", 48000, 1000), Count::Any);
    });
    /* ★★ V12 balloons (docs/v12/BALLOONS.md): UKHAS RTTY + Horus Binary — the whole AUTO search (24 RTTY candidates) and
     *  the Horus demodulator on one listener's audio. Sets the "balloon" switch's default; never hides it. */
    run("dec_balloon", "Decoder: Balloons", [&] {
        return hostAudioRow("dec_balloon", "Decoder: Balloons (UKHAS RTTY + Horus, AUTO)", "balloon", attach + "\"extension_name\":\"balloon\"}",
                            balloonAudio(), Count::Any);
    });
    /* ★★ V12 tones (docs/v12/TONES.md): DTMF and 5-tone selective calling, both on one listener's audio. Sets the "tones"
     *  switch's default; never hides it. */
    run("dec_tones", "Decoder: DTMF / 5-tone", [&] {
        return hostAudioRow("dec_tones", "Decoder: DTMF + 5-tone selective calling", "tones", attach + "\"extension_name\":\"tones\"}",
                            tonesAudio(), Count::Any);
    });
    /* ★★ V12 2026-11-19 2G HF ALE (docs/v12/HF-DATA.md): a sounding and a call (our own transmitter) on one listener's
     *  audio — every (offset, timing) register searching all the while. Sets the "ale" switch's default; never hides it. */
    run("dec_ale", "Decoder: ALE", [&] {
        std::vector<float> a;
        for (const auto& w : { aleCallWords("", "G4ABC", 4), aleCallWords("W1XYZ", "G4ABC", 6) }) {
            auto x = aleSynth(w, 48000); a.insert(a.end(), x.begin(), x.end()); a.insert(a.end(), 48000, 0.f);
        }
        for (auto& v : a) v *= 0.3f;
        addNoise(a, 0.05f, 9);
        return hostAudioRow("dec_ale", "Decoder: ALE (2G, MIL-STD-188-141)", "ale", attach + "\"extension_name\":\"ale\"}", a, Count::Any);
    });
    run("dec_sitora", "Decoder: SITOR-A", [&] {
        const auto L = sitorsynth::makeLink("002320204", "364775427", "GOOD MORNING THIS IS A TEST +?", "QSL +?", "73 +?", 4, 7);
        return hostAudioRow("dec_sitora", "Decoder: SITOR-A", "sitora", attach + "\"extension_name\":\"sitora\",\"center_frequency\":1000}",
                            sitorsynth::render(L, 48000, 1000, 15, -6, 1), Count::Chars);
    });
    run("dec_dsc", "Decoder: DSC", [&] {
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
    run("dec_same", "Decoder: EAS / SAME", [&] {
        auto a = sameAlert("ZCZC-WXR-TOR-024031-024033+0030-3041345-KLWX/NWS-");
        addNoise(a, 0.05f, 3);
        return hostAudioRow("dec_same", "Decoder: EAS / SAME", "same", attach + "\"extension_name\":\"same\"}", a, Count::Any);
    });
    run("dec_selcal", "Decoder: SELCAL", [&] {
        auto a = selcalCall("AFHL");
        addNoise(a, 0.05f, 4);
        return hostAudioRow("dec_selcal", "Decoder: SELCAL", "selcal", attach + "\"extension_name\":\"selcal\"}", a, Count::Any);
    });
    /* ★ (2026-11-19) the one AUTO now holds DOMINOEX (+ its FEC option) and MT63 too — every receiver runs on whatever is
     *  on the band, so one MFSK16 over measures the whole key's cost as before (docs/v12/HF-DATA.md) */
    run("dec_mfsk", "Decoder: MFSK, Olivia, Contestia, THOR, DominoEX, MT63", [&] {
        auto a = textModeSynth(TextModeDecoder::Family::Mfsk, "mfsk16", "CQ CQ DE G4ABC G4ABC PSE K\n", 48000, 1500);
        addNoise(a, 0.1f, 5);
        return hostAudioRow("dec_mfsk", "Decoder: MFSK / Olivia / Contestia / THOR / DominoEX / MT63 (AUTO)", "mfsk",
                            attach + "\"extension_name\":\"mfsk\",\"mode\":\"auto\"}", a, Count::Chars);
    });
    run("dec_hell", "Decoder: Feld Hell", [&] {
        auto a = feldHellSynth("CQ CQ DE G4ABC G4ABC K ", 48000, 1000);
        addNoise(a, 0.05f, 6);
        return hostAudioRow("dec_hell", "Decoder: Feld Hell", "hell", attach + "\"extension_name\":\"hell\"}", a, Count::Lines);
    });
    run("dec_aprs", "Decoder: APRS", [&] {
        auto a = aprsPacket("M0ABC", 9, "!5049.00N/00118.00W>VibeSDR benchmark");
        auto b = aprsPacket("G4ABC", 7, "!5130.00N/00007.00W-test");
        a.insert(a.end(), b.begin(), b.end());
        addNoise(a, 0.05f, 7);
        return hostAudioRow("dec_aprs", "Decoder: APRS (1200 Bd)", "aprs", attach + "\"extension_name\":\"aprs\",\"baud_rate\":1200}", a, Count::Events);
    });
    /* ★★ V12 9600 PACKET (docs/v12/AX25-9600-FX25-IL2P.md) — the packet decoder in AUTO (1200 on the audio AND 9600 G3RUH
     *  on the channel IQ, FX.25 and IL2P on both), fed 9600 bursts from our own transmitter (plain AX.25, FX.25 and IL2P
     *  in turn) at 16 dB Eb/N0 as 48 kS/s channel IQ, in real time. The row's proof that it decodes: frames counted. */
    run("dec_aprs9600", "Decoder: packet 9600 + FX.25 + IL2P", [&] {
        Row r; r.id = "dec_aprs9600"; r.label = "Decoder: packet AUTO (1200 + 9600, FX.25, IL2P)";
        auto peer = std::make_shared<CountPeer>(); peer->jsonKey = "\"type\":\"aprs_frame\"";
        DecoderHost::Env env; env.dialHz = [] { return 435.0e6; };
        DecoderHost h(env, "bench");
        h.addPeer(peer);
        if (h.start("aprs", attach + "\"extension_name\":\"aprs\",\"baud_rate\":1200,\"packet\":\"auto\"}") != DecoderHost::Start::Ok) return r;
        const std::vector<std::complex<float>> iq = pkt9600Clip(48000.0, 16.0);
        std::vector<float> silence(480, 0.f);
        const double c0 = processCpu(), w0 = wallNow(); double fedSec = 0; size_t pos = 0;
        while (fedSec < kRowMaxSec && (fedSec < kRowMinSec || wallNow() - w0 < kRowWallSec)) {
            const size_t n = std::min<size_t>(480, iq.size() - pos);
            h.feedChannelIq(iq.data() + pos, (int)n, 48000);
            h.feed(silence.data(), (int)n);   // ★ the 1200 half reads the listener's audio (silent here) — its cost counts too
            pos = (pos + n) % iq.size(); fedSec += n / 48000.0;
            const double ahead = fedSec - (wallNow() - w0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
        }
        settleCpu(5.0);
        r.pct = 100.0 * (processCpu() - c0) / fedSec;
        h.stop();
        r.decoded = (int)peer->json.load();   // ★ frames in the frame log (each burst once)
        return r;
    });
#if defined(VIBE_PAGING)
    /* ★★ PAGING — LOCAL HARDWARE ONLY (the Android app; the R36S handheld), so the row exists only where the decoder is
     *  offered (AGENTS.md: never show what cannot work here). SYNTHETIC ONLY: paging frequencies carry
     *  private messages, so no recording of one is ever used. FLEX 1600, our own transmitter, on the channel IQ. */
    // ★★ …and only in a process that may run it (DecoderHost::pagingProcess: the Android app, the R36S's radio process).
    //    The desktop VibeServer compiles the engine for the handheld, so on a server this row is not drawn at all.
    if (DecoderHost::pagingHere()) run("dec_paging", "Decoder: Paging", [&] {
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
        run("spot_" + m.name, "Digital spots: " + m.name, [&] {
            double utc0 = kSpotT0, pace = 0; bool sig = true;
            const auto a = spotAudio(m, utc0, pace, sig);
            return engineRow(m, a, utc0, pace, sig);
        });
    }

    // ── whole-radio modes ──
    run("dec_ndb", "Decoder: NDB beacons", [&] {
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
    /* ★★★ V12 THE IDLE DX SCANNER'S FM HOP (vibe_dxscan_fm.h) — 8 broadcast stations with RDS in one 2.4 MS/s capture, every
     *  one decoded at once (the spectrum, a channel per station from one forward FFT, the discriminator, the pilot loop, the
     *  RDS front end), across the hop's worker threads. % of a core over the signal fed, in real time. The scanner reads
     *  it as its budget: a box that measured red decodes the stations in ROUNDS (dxPeakBudget), and the setup page says
     *  "not recommended" beside the switch — never hidden. ★ The capture is synthesised once (1 s, looped). */
    run("dec_dxscan", "Decoder: DX scanner, FM sweep (8 stations @ 2.4 MS/s)", [&] {
        Row r; r.id = "dec_dxscan"; r.label = "Decoder: DX scanner, FM sweep (8 stations @ 2.4 MS/s)";
        const double fs = 2.4e6, centre = 98.95e6;
        const size_t n = (size_t)fs;
        std::vector<std::complex<float>> iq(n, std::complex<float>(0, 0));
        { std::mt19937 g(11); std::normal_distribution<float> d(0.f, 0.002f); for (auto& v : iq) v = { d(g), d(g) }; }
        static const double offs[8] = { -850e3, -650e3, -450e3, -150e3, 50e3, 250e3, 550e3, 750e3 };
        // ★ each station: a mono programme, the pilot and RDS group 0A with real checkwords on 57 kHz (vibe_benchmark.h's
        //   fmStation, with its own PI), so the row measures the decoder doing its whole job
        auto enc = [](uint16_t data, int off) { const uint16_t cw = vibedsp::RdsDecoder::checkword(data) ^ vibedsp::RdsDecoder::OFFSET[off];
                                                return ((uint32_t)data << 10) | cw; };
        for (int s = 0; s < 8; ++s) {
            std::vector<int> bits;
            const char* PS = "DXBENCH ";
            for (int addr = 0; addr < 4; ++addr) {
                const uint32_t blk[4] = { enc((uint16_t)(0xC200 + s), 0), enc((uint16_t)(addr & 3), 1), enc(0x1234, 2),
                                          enc((uint16_t)(((uint8_t)PS[addr * 2] << 8) | (uint8_t)PS[addr * 2 + 1]), 4) };
                for (int b = 0; b < 4; ++b) for (int k = 25; k >= 0; --k) bits.push_back((blk[b] >> k) & 1);
            }
            std::vector<int> m(bits.size()); int prev = 0;
            for (size_t k = 0; k < bits.size(); ++k) { m[k] = prev ^ bits[k]; prev = m[k]; }
            const double a = 0.1 + 0.05 * (s % 3);
            double ph = 0;
            for (size_t i = 0; i < n; ++i) {
                const double t = i / fs;
                const long kb = (long)std::floor(t * 1187.5);
                const double manch = (((t * 1187.5 - kb) < 0.5) ? 1.0 : -1.0) * (m[(size_t)kb % m.size()] ? 1.0 : -1.0);
                const double mpx = 0.5 * std::cos(2 * M_PI * (800.0 + 100 * s) * t) + 0.09 * std::cos(2 * M_PI * 19000.0 * t)
                                 + 0.045 * manch * std::cos(2 * M_PI * 57000.0 * t);
                ph += 2 * M_PI * (offs[s] + 75000.0 * mpx) / fs;
                if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
                iq[i] += std::complex<float>((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
            }
        }
        vibe::dx::FmHopEngine e;
        vibe::dx::FmHopEngine::Params p;
        p.rateHz = fs; p.centreHz = centre; p.dcHz = centre; p.rasterKhz = 100;
        p.loHz = 98.1e6; p.hiHz = 99.8e6; p.dwellSec = 3.0; p.minDwellSec = 3.0;   // ★ the whole dwell, always — a fair cost
        const double c0 = processCpu(), w0 = wallNow();
        e.start(p);
        double fedSec = 0; size_t pos = 0;
        const size_t blk = (size_t)(fs / 100);
        while (e.running() && fedSec < 30) {   // ★ 30 s of signal is a backstop only: the hop ends after its dwell
            // ★ 0.1 s, BELOW the engine's queue cap: that is min(0.5 s, 512 k samples) = 0.22 s at 2.4 MS/s, so the old 0.25 s
            //   never waited — the feeder outran the hop, every block was dropped, and the row read "decoded 0" (11-05)
            while (e.running() && e.queuedSec() > 0.1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t k = std::min(blk, n - pos);
            e.feed(iq.data() + pos, (int)k);
            pos = (pos + k) % n; fedSec += (double)k / fs;
        }
        e.abort();   // joins; a finished hop keeps its results (abort marks only an unfinished one)
        r.pct = 100.0 * (processCpu() - c0) / std::max(0.5, p.settleSec + p.specSec + p.dwellSec);
        int ids = 0;
        for (const auto& h : e.results()) if (h.pi && vibe::dx::fmConfirmed(h.groups, h.psComplete)) ++ids;
        r.decoded = ids;
        r.ms = (int)((wallNow() - w0) * 1000);
        return r;
    });
    run("dec_ais", "Decoder: AIS", [&] {
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
    /* ★★ V12 AIRCRAFT DATALINK (docs/v12/AVIATION.md §Cost): the mode's two bands as the server runs them — vibeavi::Service
     *  on its own thread (VDL2's channels on its workers), fed IN REAL TIME, SCORED BY THE HOTTEST THREAD (as AIS).
     *  The traffic is the transmitters of the tests (vibe_avi_tx.h): bursts on three channels in one second of noise, looped. */
    auto aviRow = [&](const char* id, const char* label, const char* bandId) {
        run(id, label, [&] {
            Row r; r.id = id; r.label = label;
            const vibeavi::Band* b = vibeavi::bandById(bandId);
            if (!b) return r;
            const double fs = 2.4e6;
            std::vector<vibeavi::cf32> x((size_t)fs);
            { std::mt19937 g(9); std::normal_distribution<float> d(0.f, 0.02f); for (auto& v : x) v = { d(g), d(g) }; }
            const double nPow = 2 * 0.02 * 0.02 * 10500.0 / fs;
            for (int k = 0; k < 3; ++k) {
                const double off = b->chans[size_t(k % b->chans.size())] - b->centreHz;
                if (std::string(bandId) == "vdl2") {
                    char txt[64]; std::snprintf(txt, sizeof txt, "M%02dABE%04d/BENCH %d", k, 1000 + k, k);
                    auto f = vibeavitx::avlcAcars(0x400100u + uint32_t(k), 0x10949Au, ".G-BNCH", "H1", char('0' + k), txt);
                    vibeavitx::vdl2Burst(x, fs, off, std::sqrt(nPow * 300.0), size_t((0.05 + 0.3 * k) * fs), vibeavitx::vdl2Symbols({ f }), 0.3 * k);
                } else {
                    auto blk = vibeavitx::makeBlock('2', ".G-BNCH", 0x15, "H1", char('0' + k), "M01ABE1234/BENCH");
                    vibeavitx::acarsBurst(x, fs, off, 0.1, size_t((0.05 + 0.3 * k) * fs), vibeavitx::airBits(blk));
                }
            }
            vibeavi::Tracker tr(false);
            vibeavi::Service svc(tr);
            svc.start(b->id, b->centreHz, fs, b->chans);
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
            scoreHottest(r, th0, threadTimes(), kRtSec, 100.0 * (processCpu() - c0) / kRtSec);
            r.decoded = (int)svc.messages();
            if (svc.dropped()) r.label += " (fell behind)";
            svc.stop();
            return r;
        });
    };
    aviRow("dec_vdl2", "Decoder: VDL2 aircraft (12 channels @ 2.4 MS/s)", "vdl2");
    aviRow("dec_acars", "Decoder: ACARS aircraft (4 channels @ 2.4 MS/s)", "acars");
    /* ★★ V12 RADIOSONDES (docs/v12/RS41.md): the SONDES mode as the server runs it — vibers41::Service on its own thread,
     *  one 2 MHz band at 2.4 MS/s, fed IN REAL TIME, SCORED BY THE HOTTEST THREAD (as AIS and the datalink). Three sondes
     *  from our own transmitter (vibe_rs41_tx.h), one frame a second each, in four seconds of noise, looped. */
    /* ★ The service as the server runs it: `x` (2.4 MS/s, looped) fed IN REAL TIME, scored by the hottest thread. */
    auto sondeService = [&](Row& r, std::vector<vibers41::cf32>& x, double fs) {
        const vibers41::Band* b = vibers41::bandById("mid");
        vibers41::Service svc;
        svc.start(*b, b->centreHz, fs);
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
        scoreHottest(r, th0, threadTimes(), kRtSec, 100.0 * (processCpu() - c0) / kRtSec);
        r.decoded = (int)svc.tracker().frames();
        if (svc.dropped()) r.label += " (fell behind)";
        svc.stop();
    };
    run("dec_rs41", "Decoder: RS41 radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)", [&] {
        Row r; r.id = "dec_rs41"; r.label = "Decoder: RS41 radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)";
        if (!vibers41::bandById("mid")) return r;
        const double fs = 2.4e6;
        const int secs = 4;
        std::vector<vibers41::cf32> x(size_t(fs) * secs);
        { std::mt19937 g(9); std::normal_distribution<float> d(0.f, 0.02f); for (auto& v : x) v = { d(g), d(g) }; }
        const double n10k = 2 * 0.02 * 0.02 * 10e3 / fs;
        for (int k = 0; k < 3; ++k) {
            vibers41tx::Sonde s(uint32_t(70 + k));
            char ser[9]; std::snprintf(ser, sizeof ser, "B%07d", 1000 + k); s.serial = ser;
            vibers41tx::Gfsk tx(fs, -600e3 + 550e3 * k, std::sqrt(n10k * 100), 2400, 0.05 + 0.2 * k);
            for (int f = 0; f < secs; ++f) tx.addFrame(f, vibers41tx::frameOnAir(s, f));
            tx.render(x.data(), x.size(), 0);
        }
        sondeService(r, x, fs);
        return r;
    });
    /* ★★ V12 the DFM and M10 / M20 (docs/v12/SONDES-TYPES.md): three sondes of the type in the same band, the same service.
     *  The DFM sends continuously (≈ 4.5 frames a second, serial in halves), so its clip is 12 s to let a serial complete. */
    run("dec_dfm", "Decoder: DFM radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)", [&] {
        Row r; r.id = "dec_dfm"; r.label = "Decoder: DFM radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)";
        if (!vibers41::bandById("mid")) return r;
        const double fs = 2.4e6;
        const int secs = 12;
        std::vector<vibers41::cf32> x(size_t(fs) * secs);
        { std::mt19937 g(10); std::normal_distribution<float> d(0.f, 0.02f); for (auto& v : x) v = { d(g), d(g) }; }
        const double n10k = 2 * 0.02 * 0.02 * 10e3 / fs;
        for (int k = 0; k < 3; ++k) {
            vibesondetx::Dfm d; d.serial = 19000100u + uint32_t(k);
            vibesondetx::ChipFsk tx(fs, -600e3 + 550e3 * k, std::sqrt(n10k * 100), 2000, vibedfm::kChipRate, 0.05 + 0.2 * k);
            tx.put(0, d.chips(secs));
            tx.render(x.data(), x.size(), 0);
        }
        sondeService(r, x, fs);
        return r;
    });
    run("dec_m10", "Decoder: M10 / M20 radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)", [&] {
        Row r; r.id = "dec_m10"; r.label = "Decoder: M10 / M20 radiosondes (3 sondes, a 2 MHz band @ 2.4 MS/s)";
        if (!vibers41::bandById("mid")) return r;
        const double fs = 2.4e6;
        const int secs = 4;
        std::vector<vibers41::cf32> x(size_t(fs) * secs);
        { std::mt19937 g(11); std::normal_distribution<float> d(0.f, 0.02f); for (auto& v : x) v = { d(g), d(g) }; }
        const double n10k = 2 * 0.02 * 0.02 * 10e3 / fs;
        for (int k = 0; k < 3; ++k) {
            vibesondetx::Mxx m; m.m20 = k == 1; m.sn[4] = uint8_t(0x40 + k); m.sn20 += uint32_t(k) << 10;
            vibesondetx::ChipFsk tx(fs, -600e3 + 550e3 * k, std::sqrt(n10k * 100), 4800, m.m20 ? 9600.0 : 9615.0, 0.05 + 0.2 * k);
            for (int f = 0; f < secs; ++f) tx.put(tx.indexAt(f), m.chips(f));
            tx.render(x.data(), x.size(), 0);
        }
        sondeService(r, x, fs);
        return r;
    });
    run("dec_hfdl", "Decoder: HFDL", [&] {
        // ★ a listener's dial: two 300 bit/s bursts in USB audio (the carrier 1440 Hz in), with noise
        std::vector<float> a;
        std::mt19937 rng(11);
        for (int k = 0; k < 2; ++k) {
            char flt[7]; std::snprintf(flt, sizeof flt, "BEN%03d", k);
            std::vector<vibeavi::cf32> sym, bb;
            vibeavitx::hfdlSymbols(0, vibeavitx::hfdlPerfMpdu(20 + k, 7, flt, 52.0, -8.0), sym);
            vibeavitx::hfdlModulate(sym, bb, 2.0, 12.0, -99, 0, rng);
            const auto au = vibeavitx::hfdlUsbAudio(bb, 48000);
            a.insert(a.end(), au.begin(), au.end());
            a.insert(a.end(), 48000 * 5, 0.f);   // ★ the decoder holds a burst to its longest end
        }
        addNoise(a, 0.02f, 12);
        return hostAudioRow("dec_hfdl", "Decoder: HFDL (one dial)", "hfdl", attach + "\"extension_name\":\"hfdl\"}", a, Count::Any);
    });
    auto ismRow = [&](const char* id, const char* label, const char* clipId, const char* bandId) {
        run(id, label, [&] {
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
    // ★★ LoRa on every channel the 868 capture holds (the owner's "lora+all") — dec_lora's sibling (the default channels,
    //    one of the first eight). Its grade decides whether "every channel" starts ticked on a fresh setup.
    run("dec_lora_all", "Decoder: LoRa (868, every channel)", [&] {
        Row r = loraRealTimeRow("dec_lora_all", "Decoder: LoRa (868, every channel)", INFINITY);
        r.ms = -1;   // ★ run() times the row itself
        return r;
    });

    /* ★★ V12 FreeDV AS A MODE — the WORST case of one listener: AUTO searching, all three sub-modes' demodulators at once
     *  (once one locks the other two close, so a decoding listener costs about a third of this). Fed in real time, as the
     *  audio callback does, on band noise; vibe-freedv's own thread is in the process figure. Not measured where libcodec2
     *  is not installed (the mode is then not offered at all). */
    run("dec_freedv", "FreeDV (per listener, AUTO searching)", [&] {
        Row r; r.id = "dec_freedv"; r.label = "FreeDV (per listener, AUTO searching 700D/700E/1600)";
        // ★ the codec2 trio only: RADE V1 has its own row (dec_rade) and its own switch, so it cannot take the mode off
        if (!vibe::freedv::api().ok) { r.label += " \xe2\x80\x94 libcodec2 not installed"; return r; }
        std::vector<float> a((size_t)48000 * 4);
        std::mt19937 g(11); std::normal_distribution<float> d(0.f, 0.1f);
        for (auto& v : a) v = d(g);
        vibe::freedv::FreeDvListener L(vibe::freedv::kAuto, vibe::freedv::kSbAuto, false, /*withRade*/ false);
        std::vector<float> out(960);
        const double t0 = wallNow(); double fedSec = 0, c0 = 0; size_t pos = 0; bool measuring = false;
        while (fedSec < 1.0 + kRowMinSec) {
            if (!measuring && fedSec >= 1.0) { c0 = processCpu(); measuring = true; }   // ★ past the modems' opening
            L.feedAudio(a.data() + pos, 960, 1, 48000.0);
            L.takePcm(out.data(), 960);
            pos = (pos + 960) % a.size(); fedSec += 960 / 48000.0;
            const double ahead = fedSec - (wallNow() - t0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
        }
        r.pct = 100.0 * (processCpu() - c0) / (fedSec - 1.0);
        // ★ …then, unmeasured, 6 s of a 700D station from the library's own transmitter: the row's proof that it decodes
        const std::vector<float> sig = vibe::freedv::testAudio48k(vibe::freedv::k700D, 6.0);
        for (size_t i = 0; i + 960 <= sig.size(); i += 960) {
            std::vector<float> b(sig.begin() + long(i), sig.begin() + long(i) + 960);
            for (auto& v : b) v += d(g) * 0.3f;
            L.feedAudio(b.data(), 960, 1, 48000.0);
            L.takePcm(out.data(), 960);
            while (L.queuedFrames() > 24000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        r.decoded = (int)L.speechBlocks();
        if (L.droppedBlocks()) r.label += " (fell behind)";
        return r;
    });

    /* ★★★ V12 FreeDV RADE V1 (docs/v12/FREEDV.md "RADE") — ONE LISTENER'S WORST CASE, the dearer of its two states:
     *  SEARCHING (the pilot search over 960 timings × 40 frequencies every 120 ms — what a RADE receiver does whenever nobody
     *  is transmitting) and DECODING (the neural decoder + the FARGAN vocoder, ~300 M multiply-adds a second). Both are fed
     *  in real time as the audio callback does; vibe-freedv's own thread is in the process figure. The decoding half is a
     *  real RADE over from rade_c's own transmitter (linked into the server for this row: librade_vibe_tx.a) at 10 dB, and
     *  its speech blocks are the row's proof that it decodes. Not GREEN = the owner's "rade" switch (under FreeDV) starts
     *  OFF on a fresh setup — offered, never hidden; the owner can tick it back. Not measured where RADE is not built in. */
    run("dec_rade", "FreeDV RADE V1 (per listener)", [&] {
        Row r; r.id = "dec_rade"; r.label = "FreeDV RADE V1 (per listener, the dearer of searching and decoding)";
        if (!vibe::freedv::radeBuilt()) { r.label += " \xe2\x80\x94 not built in"; return r; }
        std::mt19937 g(13); std::normal_distribution<float> d(0.f, 0.1f);
        std::vector<float> out(960);
        // ── searching: band noise ──
        double searchPct = 0;
        {
            std::vector<float> a((size_t)48000 * 4);
            for (auto& v : a) v = d(g);
            vibe::freedv::FreeDvListener L(vibe::freedv::kRade);
            const double t0 = wallNow(); double fedSec = 0, c0 = 0; size_t pos = 0; bool measuring = false;
            while (fedSec < 1.0 + kRowMinSec / 2) {
                if (!measuring && fedSec >= 1.0) { c0 = processCpu(); measuring = true; }
                L.feedAudio(a.data() + pos, 960, 1, 48000.0);
                L.takePcm(out.data(), 960);
                pos = (pos + 960) % a.size(); fedSec += 960 / 48000.0;
                const double ahead = fedSec - (wallNow() - t0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
            }
            searchPct = 100.0 * (processCpu() - c0) / (fedSec - 1.0);
            if (L.droppedBlocks()) r.label += " (fell behind searching)";
        }
        // ── decoding: a RADE over at 10 dB, measured once the receiver is in sync (2 s in) ──
        double decodePct = -1;
#if defined(VIBE_HAVE_RADE_TX)
        {
            const std::vector<float> sig = vibe::freedv::testAudio48k(vibe::freedv::kRade, 2.0 + kRowMinSec / 2 + 0.5, 0.1f);
            const float ns = 0.0894f;   // ★ 10 dB in 3 kHz: the signal's rms 0.1, noise σ² · 3/24 kHz = 0.01 / 10
            vibe::freedv::FreeDvListener L(vibe::freedv::kRade);
            const double t0 = wallNow(); double fedSec = 0, c0 = 0; bool measuring = false;
            std::vector<float> b(960);
            for (size_t i = 0; i + 960 <= sig.size() && fedSec < 2.0 + kRowMinSec / 2; i += 960) {
                if (!measuring && fedSec >= 2.0) { c0 = processCpu(); measuring = true; }
                for (int k = 0; k < 960; ++k) b[size_t(k)] = sig[i + size_t(k)] + d(g) * (ns / 0.1f);
                L.feedAudio(b.data(), 960, 1, 48000.0);
                L.takePcm(out.data(), 960);
                fedSec += 960 / 48000.0;
                const double ahead = fedSec - (wallNow() - t0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
            }
            if (measuring) decodePct = 100.0 * (processCpu() - c0) / (fedSec - 2.0);
            while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            r.decoded = (int)L.speechBlocks();
            if (L.droppedBlocks()) r.label += " (fell behind decoding)";
        }
#endif
        r.pct = std::max(searchPct, decodePct);
        char b[96];
        if (decodePct >= 0) std::snprintf(b, sizeof b, " \xe2\x80\x94 searching %.1f %%, decoding %.1f %%", searchPct, decodePct);
        else std::snprintf(b, sizeof b, " \xe2\x80\x94 searching %.1f %% (decoding not measured)", searchPct);
        r.label += b;
        return r;
    });

    /* ★★ V12 M17 AS A MODE — one listener's receiver on a VOICE transmission (the dearer case: the 4-FSK chain runs
     *  always, and on top of it the Viterbi, Golay and Codec 2 3200 for every frame). Our own transmitter makes an M17
     *  over from the spec (vibe_m17.h voiceTransmission — its speech encoded by the system's libcodec2), at 18 dB C/N in
     *  9 kHz, as 48 kHz channel IQ fed in real time; vibe-m17's own thread is in the process figure. The row's proof that
     *  it decodes is the voice frames counted. Not measured where libcodec2 is not installed (the mode is then not offered). */
    run("dec_m17", "M17 (per listener, a voice over)", [&] {
        Row r; r.id = "dec_m17"; r.label = "M17 (per listener, decoding a voice over)";
        if (!vibe::m17::available()) { r.label += " \xe2\x80\x94 libcodec2 not installed"; return r; }
        std::vector<short> sp(size_t(8000 * 5));
        for (size_t i = 0; i < sp.size(); ++i) sp[i] = short(3000 * std::sin(2 * M_PI * (150.0 + 40 * std::sin(i / 4000.0)) * double(i) / 8000.0));
        const int frames = int(kRowMinSec / 0.04) + 30;
        const std::vector<int8_t> syms = vibe::m17::voiceTransmission("@ALL", "BENCH", frames, sp);
        std::vector<vibe::m17::cf32> iq = vibe::m17::modulate(syms, 48000, 150.0);
        std::mt19937 g(17); std::normal_distribution<float> d(0.f, float(std::sqrt(48000.0 / 9000.0 / std::pow(10.0, 1.8) / 2)));
        for (auto& v : iq) v += vibe::m17::cf32(d(g), d(g));
        vibe::m17::M17Listener L;
        std::vector<float> out(480);
        const double t0 = wallNow(); double fedSec = 0, c0 = 0; size_t pos = 0; bool measuring = false;
        while (pos + 480 <= iq.size()) {
            if (!measuring && fedSec >= 0.5) { c0 = processCpu(); measuring = true; }
            L.feedIq(iq.data() + pos, 480, 48000.0);
            L.takePcm(out.data(), 480);
            pos += 480; fedSec += 480 / 48000.0;
            const double ahead = fedSec - (wallNow() - t0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
        }
        r.pct = 100.0 * (processCpu() - c0) / std::max(1e-3, fedSec - 0.5);
        while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        r.decoded = L.voiceFrames();
        if (L.droppedBlocks()) r.label += " (fell behind)";
        return r;
    });

    /* ★★ V12 DV SIGNALLING AS A MODE — one listener's receiver in AUTO (every system's correlators running) on a DMR
     *  repeater downlink carrying a group call with a talker alias, from our own transmitter (vibe_dvsig_tx.h) at 18 dB C/N
     *  in 9 kHz, as 48 kHz channel IQ fed in real time; vibe-dvsig's own thread is in the process figure. The proof that it
     *  decodes: the DMR bursts that checked out. */
    run("dec_dvsig", "DV signalling (per listener, AUTO)", [&] {
        Row r; r.id = "dec_dvsig"; r.label = "DMR / D-STAR / YSF / NXDN / dPMR signalling (per listener, AUTO on a DMR call)";
        vibe::dvsig::tx::DmrCall c; c.alias = "BENCH"; c.superframes = int(kRowMinSec / 0.36) + 4;
        std::vector<vibe::dvsig::cf32> a = vibe::dvsig::tx::dmrIq(c, true, 77, 150.0);
        vibe::dvsig::tx::awgn(a, 18, 9000, 78);
        vibedsp::RationalResampler ri(24000.0, 48000, vibedsp::RationalResampler::ExactRate{}), rq(24000.0, 48000, vibedsp::RationalResampler::ExactRate{});
        std::vector<float> re(a.size()), im(a.size()); for (size_t i = 0; i < a.size(); ++i) { re[i] = a[i].real(); im[i] = a[i].imag(); }
        std::vector<float> x(size_t(ri.maxOut(int(a.size())))), y(x.size());
        const int n = std::min(ri.process(re.data(), int(re.size()), x.data()), rq.process(im.data(), int(im.size()), y.data()));
        std::vector<vibe::dvsig::cf32> iq(static_cast<size_t>(n)); for (int i = 0; i < n; ++i) iq[size_t(i)] = vibe::dvsig::cf32(x[size_t(i)], y[size_t(i)]);
        vibe::dvsig::Listener L;
        const double t0 = wallNow(); double fedSec = 0, c0 = 0; size_t pos = 0; bool measuring = false;
        while (pos + 480 <= iq.size()) {
            if (!measuring && fedSec >= 0.5) { c0 = processCpu(); measuring = true; }
            L.feedIq(iq.data() + pos, 480, 48000.0);
            pos += 480; fedSec += 480 / 48000.0;
            const double ahead = fedSec - (wallNow() - t0);
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(ahead * 1e6)));
        }
        r.pct = 100.0 * (processCpu() - c0) / std::max(1e-3, fedSec - 0.5);
        while (L.queuedFrames() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::string ids; L.status("bench", &ids, nullptr, nullptr);
        r.decoded = ids.find("\"alias\":\"BENCH\"") != std::string::npos ? 1 : 0;
        if (L.droppedBlocks()) r.label += " (fell behind)";
        return r;
    });

    // ── a listener's own filters (per listener, on their DSP thread — no decoder slot) ──
    run("dec_tone", "CTCSS / DCS tone ID", [&] {
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
        run(id, label, [&] {
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
