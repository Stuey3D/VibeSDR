// vibe_bench_profiles.h — THE THREE LOCKED-RANGE PROFILES, THE WIDE BENCHMARK'S RULES AND THE USB BUS VERDICT (V12, 2026-11-18).
//
// ★★★ WHY (Stuart, 2026-11-17/18): "the cap should be based on the hardware running the server, so build it into the
//     benchmark" — and "no point in saying you can host 200 users but on SSB/AM when a server is being locked to DAB". So
//     the benchmark says, for the heaviest load of each kind a locked range carries, how many listeners THIS box holds:
//         CPU/RAM:
//         50 DAB users @ 8 MS/s
//         25 WFM + Advanced RDS users @ 8 MS/s
//         200 SSB/AM/NFM users @ 8 MS/s
//     The QUICK BENCHMARK (today's run) measures them at a rate the radios here offer; the WIDE BENCHMARK measures them at
//     the rate the owner picks from a wide radio's own list, then takes the radio there and checks the USB bus keeps up.
// ★★★ THE RULES, ONE PLACE, PURE (vibeserver/test-bench-wide.cpp holds them):
//     · a figure is the SMALLER of what the cores and what the RAM hold, and says which one set it ("by");
//     · the cores: SCORED BY THE HOTTEST THREAD — the capture's fixed path (intake, front end, a multiplex's front) or one
//       listener's thread past 70 % of a core = no listeners at all; else 70 % of every core, less one core for network,
//       spectrum and the audio clock, less the fixed path, shared out at the measured per-listener cost;
//     · the RAM: what is available now, less the shim's own 64 MB floor (NO OOM, ever — 256 MB boards exist) and the
//       capture's bounded buffers, shared out at the measured memory per listener;
//     · never a line for something not measured, never more users than either holds.
// ★ Header-only, no DSP: the measurements are vibe_benchmark_wide.h's; this decides what they mean.
#pragma once
#include "vibe_benchmark_hw.h"
#include "vibe_dab_wide.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <malloc.h>
#include <unistd.h>
#endif

namespace vibe {
namespace benchprof {

/** The green line: a thread at or under this % of one core has headroom (every row's grade). */
constexpr double kGreenPct = 70.0;
/** ★ The shim's own memory floor (local_sdr_shim.cpp: a DAB station is never opened under 64 MB available) — the RAM
 *  bound leaves it untouched. */
constexpr double kRamReserveKB = 64.0 * 1024.0;
/** ★ One DAB station's memory when the benchmark could not measure it: the top of the measured 3–9 MB (Pi 2 and Mac —
 *  docs/v12/DAB-OWN-STATION.md §6). The high end, so an unmeasured figure never claims more than RAM holds. */
constexpr double kDabStationKBFallback = 9.0 * 1024.0;

// ── which radios, which rates ──────────────────────────────────────────────────────────────────────────────────

/** ★★ A WIDE-RATE RADIO — the WIDE BENCHMARK is offered only for these: a USRP, a HackRF, an SDRplay RSP, an Airspy R2 /
 *  Mini. Not the HF+ (912 kS/s), not an RTL (2.4 MS/s at best), not an Icom (no IQ). */
inline bool wideCapable(const std::string& driver) {
    return driver == "usrp" || driver == "hackrf" || driver == "sdrplay" || driver == "airspy";
}

/** ★★ THE QUICK BENCHMARK'S PROFILE RATE: the highest rate of 8 MS/s or less that a radio here OFFERS (its list, never
 *  the gate's candidates) and that can carry the profile — Wfm: a radio reaching 88–108 MHz; Dab: one reaching Band III;
 *  None: any radio with a capture. No radio configured = 2.048 MS/s (today's locked-range rows). 0 = no radio here can.
 *  ★ 8 MS/s and no higher: past that is the WIDE BENCHMARK's question, at the rate the owner picks — and a Pi 2's run
 *    stays at the 2.4 MS/s its dongle offers. */
inline double profileRate(const benchhw::Hw& hw, benchhw::Need need) {
    if (hw.radios.empty()) return 2048000.0;
    double best = 0;
    for (const auto& r : hw.radios) {
        if (r.driver == "icom") continue;
        benchhw::Hw one; one.radios.push_back(r);
        if (need != benchhw::Need::None && !benchhw::gateNeed(one, need).run) continue;
        for (double v : r.rates) if (v <= 8e6 + 1 && v > best) {
            if (need == benchhw::Need::Dab && v < 2048000.0 - 1000.0) continue;   // ★ an ensemble needs 2.048 MS/s
            best = v;
        }
    }
    return best;
}

/** ★★ HOW MANY WHOLE BAND III BLOCKS a capture at `rateHz` holds at its best centre (vibe_dab_wide.h blocksIn, the
 *  server's own rule). Below 4 MS/s, or where fewer than two fit, it is the ordinary DAB mode: ONE block at its own
 *  2.048 MS/s. 0 = below 2.048 MS/s (no ensemble fits). */
inline int dabBlocksAt(double rateHz) {
    if (rateHz < 2048000.0 - 1000.0) return 0;
    if (rateHz < vibedab::wide::kMinRateHz - 1000.0) return 1;
    size_t best = 0;
    const size_t n = vibedab::kBandIIICount;
    for (size_t i = 0; i < n; ++i)
        for (size_t k = i; k < n; ++k) {
            const double c = 0.5 * (double(vibedab::kBandIII[i].centreHz) + double(vibedab::kBandIII[k].centreHz));
            best = std::max(best, vibedab::wide::blocksIn(c, rateHz).size());
        }
    return best >= 2 ? int(best) : 1;
}

// ── the users maths ────────────────────────────────────────────────────────────────────────────────────────────

/** What one profile costs here, as measured. % = of one core in real time; KB = resident memory. */
struct ProfileCost {
    double perPct = -1;           // one more listener: all its threads together
    double perHottestPct = -1;    // one listener's hottest single thread
    double fixedPct = 0;          // the shared path, every thread together (intake + front end [+ multiplexes + cutting])
    double fixedHottestPct = 0;   // the shared path's hottest single thread
    long   dropped = 0;           // IQ buffers the shared path lost while measured — anything = it did not keep up
    long   listenerDropped = 0;   // blocks a listener's own thread had to throw away — one listener did not keep up
    double perKB = -1;            // one listener's memory (-1 = not measured: no RAM bound — see profileUsers)
    double fixedKB = 0;           // the shared path's bounded buffers at their bound
};
struct ProfileUsers {
    int n = 0, cpuN = 0, ramN = -1;   // ramN -1 = RAM not measured
    std::string by = "cpu";           // "cpu" | "ram" | "capture" (the shared path itself could not keep up) | "none"
};

/** ★★★ THE FIGURE. See the file note. `availKB` < 0 = RAM not readable here (then the cores alone, said as "cpu"). */
inline ProfileUsers profileUsers(const ProfileCost& c, int cores, double availKB, double reserveKB = kRamReserveKB) {
    ProfileUsers u;
    if (!(c.perPct > 0) || cores < 1) { u.by = "none"; return u; }
    if (c.fixedHottestPct > kGreenPct || c.dropped > 0) { u.by = "capture"; return u; }
    if (c.perHottestPct > kGreenPct || c.listenerDropped > 0) { u.by = "cpu"; return u; }
    const double spare = kGreenPct * cores - kGreenPct - c.fixedPct;
    u.cpuN = spare > 0 ? int(std::floor(spare / c.perPct)) : 0;
    if (availKB > 0 && c.perKB > 0) {
        const double room = availKB - reserveKB - c.fixedKB;
        u.ramN = room > 0 ? int(std::floor(room / c.perKB)) : 0;
    }
    u.n = u.ramN >= 0 ? std::min(u.cpuN, u.ramN) : u.cpuN;
    u.by = (u.ramN >= 0 && u.ramN < u.cpuN) ? "ram" : "cpu";
    return u;
}

/** The capture's shared path at one rate (vibe_benchmark_wide.h runLocked): intake and front end. */
struct CaptureCost { double rate = 0, intakePct = -1, frontPct = -1; long dropped = 0; double fixedKB = 0;
                     bool ok() const { return intakePct >= 0 && frontPct >= 0; } };

/** ★★ DAB IN A LOCKED RANGE, as the shim runs it: every block in the capture decoding (its OFDM / FIC front end, `muxFront`
 *  — its hottest thread is the `dab` row's, `muxHottest`), the blocks cut out by per-multiplex DDCs or the filter bank,
 *  whichever is cheaper (vibe_dab_wide.h cutCostPct — the server's own choice), and each listener their own station
 *  (`perStation`, `stationKB`). Two blocks or more ride the capture's shared path too (`cap`); one block is the ordinary
 *  DAB mode at its own rate (no cutting, no channeliser). */
inline ProfileCost dabCost(double perStation, double muxFront, double muxHottest, double ddc8, const vibedab::wide::BankCost* bank,
                           double rateHz, int blocks, const CaptureCost& cap, double stationKB) {
    ProfileCost c;
    if (!(perStation > 0) || !(muxFront > 0) || blocks < 1) return c;
    bool useBank = false;
    const double cut = vibedab::wide::cutCostPct(blocks, rateHz, ddc8, bank, &useBank);
    if (cut < 0) return c;   // ★ several blocks and no cutting measured: not a figure
    c.perPct = perStation;
    c.perHottestPct = perStation;   // ★ its decode, FAAD2 and encode together — no single thread can be hotter
    c.fixedPct = double(blocks) * muxFront + cut;
    c.fixedHottestPct = std::max(0.0, muxHottest);
    if (blocks >= 2) {
        // ★ The cutting's own thread: the bank's forward FFT is one thread; a DDC is one thread per block.
        const double cutThread = useBank ? (bank ? bank->fwdAt(rateHz) : 0.0) : std::max(0.0, ddc8) * rateHz / 8e6;
        c.fixedHottestPct = std::max(c.fixedHottestPct, cutThread);
        if (cap.ok()) {
            c.fixedPct += cap.intakePct + cap.frontPct;
            c.fixedHottestPct = std::max({ c.fixedHottestPct, cap.intakePct, cap.frontPct });
            c.dropped = cap.dropped;
            c.fixedKB = cap.fixedKB;
        }
    }
    c.perKB = stationKB > 0 ? stationKB : kDabStationKBFallback;
    return c;
}

// ── the USB bus verdict ────────────────────────────────────────────────────────────────────────────────────────

/** The server's own counters at one moment (local_sdr_shim.cpp): samples the radio's library handed us, the holes it
 *  reported (usbDrops / usbDropSamples), our IQ queue's drops (iqDrops, the samples they cost), the holes that reached
 *  the DSP (iqGaps). */
struct UsbCounters {
    double t = 0;
    unsigned long long samples = 0, usbEvents = 0, usbLost = 0, iqDrops = 0, iqLost = 0, gaps = 0;
};
struct UsbVerdict {
    bool ok = false;
    double hz = 0, seconds = 0, deliveredPct = 0;
    unsigned long long delivered = 0, usbEvents = 0, usbLost = 0, iqDrops = 0, iqLost = 0, gaps = 0;
    std::string why;
};
/** ★★★ KEPT UP = at least 99.9 % of the nominal rate DELIVERED (librtlsdr and libhackrf report no drops of their own — what
 *  they lose shows only as fewer samples a second), AND no hole after the settle: none the radio's library reported, none
 *  our IQ queue made, none that reached the DSP. `a` / `b` = the counters at the start and end of the measured span. */
inline UsbVerdict usbVerdict(const UsbCounters& a, const UsbCounters& b, double hz) {
    UsbVerdict v;
    v.hz = hz;
    v.seconds = b.t - a.t;
    auto d = [](unsigned long long x, unsigned long long y) { return y >= x ? y - x : 0ull; };
    v.delivered = d(a.samples, b.samples);
    v.usbEvents = d(a.usbEvents, b.usbEvents); v.usbLost = d(a.usbLost, b.usbLost);
    v.iqDrops = d(a.iqDrops, b.iqDrops); v.iqLost = d(a.iqLost, b.iqLost); v.gaps = d(a.gaps, b.gaps);
    if (!(hz > 0) || !(v.seconds > 0)) { v.why = "not measured"; return v; }
    v.deliveredPct = 100.0 * double(v.delivered) / (hz * v.seconds);
    std::string why;
    auto add = [&](const std::string& s) { why += (why.empty() ? "" : "; ") + s; };
    char b2[96];
    if (v.deliveredPct < 99.9) { std::snprintf(b2, sizeof b2, "only %.2f %% of the samples arrived", v.deliveredPct); add(b2); }
    if (v.usbEvents) add(std::to_string(v.usbEvents) + " USB drop" + (v.usbEvents == 1 ? "" : "s") + " reported by the radio");
    if (v.iqDrops) add(std::to_string(v.iqDrops) + " IQ buffer" + (v.iqDrops == 1 ? "" : "s") + " dropped (the server fell behind)");
    if (v.gaps && !v.usbEvents && !v.iqDrops) add(std::to_string(v.gaps) + " gap" + (v.gaps == 1 ? "" : "s") + " in the stream");
    v.ok = why.empty();
    v.why = why;
    return v;
}
inline std::string usbJson(const UsbVerdict& v, double settleSec) {
    char b[512];
    std::string why;
    for (char c : v.why) { if (c == '"' || c == '\\') why += '\\'; why += c; }
    std::snprintf(b, sizeof b, "{\"ok\":%s,\"hz\":%.0f,\"deliveredPct\":%.3f,\"delivered\":%llu,\"seconds\":%.2f,\"settleSec\":%.1f,"
                  "\"usbDrops\":%llu,\"usbDropSamples\":%llu,\"iqDrops\":%llu,\"iqDroppedSamples\":%llu,\"gaps\":%llu,\"why\":\"%s\"}",
                  v.ok ? "true" : "false", v.hz, v.deliveredPct, v.delivered, v.seconds, settleSec, v.usbEvents, v.usbLost,
                  v.iqDrops, v.iqLost, v.gaps, why.c_str());
    return b;
}

// ── the machine ────────────────────────────────────────────────────────────────────────────────────────────────

/** Memory available to a new allocation now, KB (Linux / Android MemAvailable, MemFree + Cached on a pre-3.14 kernel;
 *  a Mac's free + inactive + purgeable pages). -1 = not readable. */
inline double availRamKB() {
#if defined(__linux__) || defined(__ANDROID__)
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[160]; long avail = -1, fr = -1, cached = -1;
    while (std::fgets(line, sizeof line, f)) {
        long v;
        if (std::sscanf(line, "MemAvailable: %ld kB", &v) == 1) avail = v;
        else if (std::sscanf(line, "MemFree: %ld kB", &v) == 1) fr = v;
        else if (std::sscanf(line, "Cached: %ld kB", &v) == 1) cached = v;
    }
    std::fclose(f);
    if (avail >= 0) return double(avail);
    return (fr >= 0 && cached >= 0) ? double(fr + cached) : -1;
#elif defined(__APPLE__)
    vm_statistics64_data_t s{}; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&s, &n) != KERN_SUCCESS) return -1;
    vm_size_t page = 0; host_page_size(mach_host_self(), &page);
    return double((unsigned long long)(s.free_count + s.inactive_count + s.purgeable_count) * page) / 1024.0;
#else
    return -1;
#endif
}
/** This process's resident memory, KB (0 = not readable). */
inline double rssKB() {
#if defined(__APPLE__)
    mach_task_basic_info info{}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS) return double(info.resident_size) / 1024.0;
    return 0;
#elif defined(__linux__) || defined(__ANDROID__)
    long pages = 0, rss = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; std::fclose(f); }
    return double(rss) * double(sysconf(_SC_PAGESIZE)) / 1024.0;
#else
    return 0;
#endif
}
/** The heap in use, KB — counts what a listener allocates even where the allocator reuses pages RSS already holds
 *  (an earlier row's freed buffers), which RSS alone would read as free. 0 = not readable. */
inline double heapKB() {
#if defined(__APPLE__)
    malloc_statistics_t s{}; malloc_zone_statistics(nullptr, &s);
    return double(s.size_in_use) / 1024.0;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 m = mallinfo2();
    return double(m.uordblks + m.hblkhd) / 1024.0;
#elif defined(__ANDROID__) || defined(__GLIBC__)
    const struct mallinfo m = mallinfo();
    return double((unsigned long)m.uordblks + (unsigned long)m.hblkhd) / 1024.0;
#else
    return 0;
#endif
}
/** One listener's memory from two readings around building `n` of them: the larger of the heap and RSS growth. */
inline double perListenerKB(double heap0, double heap1, double rss0, double rss1, int n) {
    if (n < 1) return -1;
    const double g = std::max(heap1 - heap0, rss1 - rss0);
    return g > 0 ? g / n : -1;
}

// ── the JSON ───────────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★ benchmark.json holds the QUICK result at the top and the WIDE results under "wide", one per radio. Each run keeps
//    the other's: the quick run carries an existing "wide" block across (benchKeepWide), the wide run writes its entry
//    into the saved file without touching anything else (benchWithWide). A tiny scanner, string-aware, not a parser:
//    the file is ours and well-formed.

/** Skip one JSON value starting at `i` (after whitespace); returns the index just past it. */
inline size_t jsonSkip(const std::string& j, size_t i) {
    while (i < j.size() && std::isspace((unsigned char)j[i])) ++i;
    if (i >= j.size()) return i;
    if (j[i] == '"') {
        for (++i; i < j.size(); ++i) { if (j[i] == '\\') { ++i; continue; } if (j[i] == '"') return i + 1; }
        return i;
    }
    if (j[i] == '{' || j[i] == '[') {
        int depth = 0;
        for (; i < j.size(); ++i) {
            const char c = j[i];
            if (c == '"') { i = jsonSkip(j, i) - 1; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
        }
        return i;
    }
    while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != ']') ++i;
    return i;
}
/** The span of top-level `key` in object `j`: [keyAt, valueEnd) covers `"key":value`, [valueAt, valueEnd) the value.
 *  false = not there (or `j` is not an object). */
inline bool jsonTopSpan(const std::string& j, const std::string& key, size_t& keyAt, size_t& valueAt, size_t& valueEnd) {
    size_t i = j.find('{');
    if (i == std::string::npos) return false;
    ++i;
    while (i < j.size()) {
        while (i < j.size() && (std::isspace((unsigned char)j[i]) || j[i] == ',')) ++i;
        if (i >= j.size() || j[i] != '"') return false;
        const size_t ks = i, ke = jsonSkip(j, i);
        const std::string name = j.substr(ks + 1, ke >= ks + 2 ? ke - ks - 2 : 0);
        i = ke;
        while (i < j.size() && (std::isspace((unsigned char)j[i]) || j[i] == ':')) ++i;
        const size_t vs = i, ve = jsonSkip(j, i);
        if (name == key) { keyAt = ks; valueAt = vs; valueEnd = ve; return true; }
        i = ve;
    }
    return false;
}
/** The raw text of top-level `key`'s value; "" when absent. */
inline std::string jsonTop(const std::string& j, const std::string& key) {
    size_t k, s, e;
    return jsonTopSpan(j, key, k, s, e) ? j.substr(s, e - s) : std::string();
}
inline double jsonNum(const std::string& obj, const std::string& key, double def = -1) {
    const std::string v = jsonTop(obj, key);
    if (v.empty() || v == "null") return def;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    return end && end != v.c_str() ? x : def;
}
inline std::vector<double> jsonNumArray(const std::string& arr) {
    std::vector<double> o;
    size_t i = arr.find('[');
    if (i == std::string::npos) return o;
    ++i;
    while (i < arr.size() && arr[i] != ']') {
        char* end = nullptr;
        const double x = std::strtod(arr.c_str() + i, &end);
        if (!end || end == arr.c_str() + i) { ++i; continue; }
        o.push_back(x);
        i = size_t(end - arr.c_str());
    }
    return o;
}
/** `j` with top-level `key` set to `value` (raw JSON): replaced where it is, appended where it is not. */
inline std::string jsonWithTop(const std::string& j, const std::string& key, const std::string& value) {
    size_t k, s, e;
    if (jsonTopSpan(j, key, k, s, e)) return j.substr(0, s) + value + j.substr(e);
    const size_t close = j.rfind('}');
    if (close == std::string::npos) return "{\"" + key + "\":" + value + "}";
    size_t p = close;
    while (p > 0 && std::isspace((unsigned char)j[p - 1])) --p;
    const bool empty = p > 0 && j[p - 1] == '{';
    return j.substr(0, close) + (empty ? "" : ",") + "\"" + key + "\":" + value + j.substr(close);
}
/** ★ The quick run's result with the saved file's "wide" block carried across (a quick run never erases a wide one). */
inline std::string benchKeepWide(const std::string& quick, const std::string& saved) {
    const std::string w = jsonTop(saved, "wide");
    return w.empty() || w[0] != '{' ? quick : jsonWithTop(quick, "wide", w);
}
/** ★ The saved file with one radio's wide entry written in (`key` = its serial, or "radio<n>"); everything else as it was.
 *  No saved file = a bare {"v":0} holder: the quick figures are simply absent until the quick run is made. */
inline std::string benchWithWide(const std::string& saved, const std::string& key, const std::string& entry) {
    std::string base = saved;
    { size_t a = base.find('{'); if (a == std::string::npos) base = "{\"v\":0}"; }
    std::string w = jsonTop(base, "wide");
    if (w.empty() || w[0] != '{') w = "{}";
    return jsonWithTop(base, "wide", jsonWithTop(w, key, entry));
}
inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    return o;
}
/** ★★ DAB's inputs to its profile — the quick run's own rows, or the saved quick result (the wide run reads them from
 *  benchmark.json rather than decoding a multiplex again). -1 = not measured. */
struct DabInputs {
    double perStation = -1, muxFront = -1, muxHottest = -1, ddc8 = -1, stationKB = -1;
    vibedab::wide::BankCost bank;
    bool ok() const { return perStation > 0 && muxFront > 0; }
};
/** The saved quick result's DAB figures: dabStations {perStationPct, muxFrontPct, ddc8Pct, memKB, bank{…}} and the `dab`
 *  row's hottest thread. A result from before the multiplex row (no muxFrontPct) reads not-ok: "run the quick benchmark
 *  again". */
inline DabInputs dabInputsFromJson(const std::string& saved) {
    DabInputs d;
    const std::string st = jsonTop(saved, "dabStations");
    if (st.empty() || st[0] != '{') return d;
    d.perStation = jsonNum(st, "perStationPct");
    d.muxFront = jsonNum(st, "muxFrontPct");
    d.ddc8 = jsonNum(st, "ddc8Pct");
    d.stationKB = jsonNum(st, "memKB");
    const std::string bk = jsonTop(st, "bank");
    if (!bk.empty() && bk[0] == '{') {
        d.bank.rates = jsonNumArray(jsonTop(bk, "rates"));
        d.bank.fwd = jsonNumArray(jsonTop(bk, "fwd"));
        d.bank.slice = jsonNumArray(jsonTop(bk, "slice"));
    }
    // ★ The `dab` row: {"id":"dab",…,"pct":66.0,…} — its hottest thread is one multiplex's.
    const std::string rows = jsonTop(saved, "rows");
    const size_t at = rows.find("{\"id\":\"dab\",");
    if (at != std::string::npos) {
        const size_t e = jsonSkip(rows, at);
        d.muxHottest = jsonNum(rows.substr(at, e - at), "pct");
    }
    if (!(d.muxHottest >= 0)) d.muxHottest = d.muxFront;
    return d;
}

/** One profile as the page and the app read it — n, the rate measured at, which bound set it, both bounds and the costs. */
inline std::string profileJson(const ProfileUsers& u, const ProfileCost& c, double rate, const std::string& extra = "") {
    char b[512];
    std::snprintf(b, sizeof b, "{\"n\":%d,\"rate\":%.0f,\"by\":\"%s\",\"cpuN\":%d,\"ramN\":%d,\"perPct\":%.2f,\"perHottestPct\":%.2f,"
                  "\"fixedPct\":%.1f,\"fixedHottestPct\":%.1f,\"perKB\":%.0f,\"fixedKB\":%.0f,\"dropped\":%ld,\"listenerDropped\":%ld",
                  u.n, rate, u.by.c_str(), u.cpuN, u.ramN, c.perPct, c.perHottestPct, c.fixedPct, c.fixedHottestPct, c.perKB,
                  c.fixedKB, c.dropped, c.listenerDropped);
    return std::string(b) + extra + "}";
}

}  // namespace benchprof
}  // namespace vibe
