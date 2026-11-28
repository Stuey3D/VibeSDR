// vibe_bg_budget.h — ONE CPU BUDGET FOR THE WHOLE BOX: background decoding on every radio sheds in the OWNER'S ORDER
// (V12 b97-cpu-budget, 2026-11-28, the Pi 500).
//
// ★★★ WHY. The Pi 500 runs four radio processes on four cores: the RSP1B's all-band skimmer (80/60/40/30 m, 8 MS/s), the
//     V4's (20 m) and a V4L's (17 m), ISM 868 / LoRa and the Meteor passes on the other V4L. From 11-26 14:09 (the 20 m and
//     17 m profiles added) the box sat at load 20, 0 % idle, and the RSP1B's background channeliser dropped 30-43 s of IQ a
//     MINUTE — every band losing ~70 % of every slot, PSK Reporter at ~35 % of its normal day. Each process shed (or did not)
//     on its own view; none could see that ANOTHER radio's background work was what starved it.
// ★★★ THE OWNER'S RULES. "Depth before bands" (Stuart 11-25): a category gives up decode depth first, then its extra modes,
//     then whole bands / VFOs. And (Stuart 11-28) a DRAG-TO-ORDER priority list of the box's background modes: under load
//     the LOWEST in the list sheds first, the top keeps its full depth longest. Satellite passes and listeners' own decoding
//     are not background — they stay above all of it whatever the list says. DSC and NAVTEX (distress watches) are never
//     shed by the budget (BG-EASE's rule: never pause a safety watch for background decoding's own sake).
//
// ── The categories (the list's rows; a VFO's decoder id → its row) ─────────────────────────────────────────────────
//   band      all-band skimmers (Band VFOs)           steps: depth ×5 (LoadLadder), extra modes off per band, bands paused
//   skim      single skimmers (FT8, CW/PSK/RTTY, NDB) steps: one VFO paused per step
//   text      text decoders (RTTY weather, SITOR…)    steps: one VFO paused per step
//   pictures  pictures (WEFAX, SSTV)                  steps: one VFO paused per step (idle before mid-chart)
//   ★ ISM / LoRa is not a row (yet): it runs as its radio's own kept-running mode, not background decoding, and the budget
//     has no step that would act on it — a row it could not honour would be a dead control (AGENTS.md).
//
// ── The ledger (files beside the decoder slots, the BgRegistry way) ───────────────────────────────────────────────
//   <dir>/bgbudget-<id>.state   each radio's line, flock-held while its process lives (a crashed one counts for nothing):
//       class (0 background only, 1 a listener on, 2 a satellite pass on), its background CPU (cores), its front end's IQ
//       drops (s a minute), whether its pass / listener decoder dropped input, and per category: VFOs, steps taken, steps
//       available, measured cost.
//   <dir>/bgbudget.step         the last step anyone took on the box ("<ms>"): ONE step per kShedStepMs box-wide, so each
//       step's effect shows before the next is judged.
//   <dir>/bg-priority           the owner's order, as the front door last saved it (ids, comma-separated).
//   Every process runs the SAME pure decision on the SAME ledger, so they agree on the victim; only the victim acts.
//
// ── Over budget (any one) ─────────────────────────────────────────────────────────────────────────────────────────
//   • a radio's background front end dropping more than kFrontDropSecPerMin of IQ a minute (the channels fell behind);
//   • a satellite pass's or a listener's decoder dropping input (most urgent — the budget's whole point);
//   • the machine busier than kBusyHigh % (kBusyHighPass % while a pass is on anywhere: headroom for the pass).
//   Under budget for kCalmMs (busy under kBusyLow %, no drops anywhere) → one step given back per kRestoreStepMs, the TOP
//   of the list first (the reverse of shedding).
//   ★ Measured, never the benchmark's word: a benchmark older than the decoders now running would be wrong (the Pi 500's
//     was from 19/11, 15 rows short). The budget reads /proc/stat and each runner's own thread clocks.
//
// ★ Pure apart from Board (a few small files); test-bg-budget.cpp drives Policy with a simulated 4-core box.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibe {
namespace bg {
namespace budget {

enum Cat : int { kBand = 0, kSkim = 1, kText = 2, kPictures = 3, kCatN = 4 };

inline const char* catId(int c) {
    switch (c) { case kBand: return "band"; case kSkim: return "skim"; case kText: return "text"; case kPictures: return "pictures"; }
    return "?";
}
inline const char* catLabel(int c) {
    switch (c) {
        case kBand: return "All-band skimmers (Band: FT8, FT4, WSPR, CW, RTTY, PSK on whole bands)";
        case kSkim: return "Single skimmers (FT8, CW, PSK, RTTY skimmers, NDB)";
        case kText: return "Text decoders (RTTY weather, SITOR, APRS and the rest)";
        case kPictures: return "Pictures (WEFAX, SSTV)";
    }
    return "?";
}
/** A VFO's decoder id → its row; -1 = a distress watch (DSC, NAVTEX): never shed by the budget. */
inline int catOf(const std::string& dec) {
    if (dec == "dsc" || dec == "dsc-vhf" || dec == "navtex") return -1;
    if (dec == "spots-band") return kBand;
    if (dec == "wefax" || dec == "wefax-stations" || dec == "sstv") return kPictures;
    if (dec == "ft8" || dec == "ndb-skim" || dec.rfind("cw-skim", 0) == 0 || dec.rfind("psk-skim", 0) == 0 || dec.rfind("rtty-skim", 0) == 0)
        return kSkim;
    return kText;
}

// ── The owner's order ─────────────────────────────────────────────────────────────────────────────────────────────
/** Top first. The default on a fresh box: the all-band skimmers keep their depth longest (Stuart's goal: the best all-band
 *  skimmer), pictures give way first (BG-EASE's order: a chart is the cheapest loss to the PSK Reporter feed). */
struct Order {
    std::array<int, kCatN> top{ { kBand, kSkim, kText, kPictures } };   ///< top[0] = the owner's highest priority
    int rankOf(int c) const { for (int i = 0; i < kCatN; ++i) if (top[(size_t)i] == c) return i; return kCatN; }
    std::string str() const { std::string s; for (int i = 0; i < kCatN; ++i) { if (i) s += ","; s += catId(top[(size_t)i]); } return s; }
};
inline Order defaultOrder() { return Order(); }
/** "band,skim,…" → an order. Unknown ids are ignored, repeats count once, missing rows are appended in the default order —
 *  so an older or hand-edited list always reads as a complete one. */
inline Order parseOrder(const std::string& s) {
    Order o; std::vector<int> seen;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        std::string id = s.substr(p, q - p);
        while (!id.empty() && (id.front() == ' ' || id.front() == '"')) id.erase(id.begin());
        while (!id.empty() && (id.back() == ' ' || id.back() == '"' || id.back() == '\n')) id.pop_back();
        for (int c = 0; c < kCatN; ++c) if (id == catId(c) && std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
        p = q + 1;
    }
    for (int c : defaultOrder().top) if (std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
    for (int i = 0; i < kCatN; ++i) o.top[(size_t)i] = seen[(size_t)i];
    return o;
}

// ── One radio's line in the ledger ────────────────────────────────────────────────────────────────────────────────
struct CatState {
    int vfos = 0;          ///< background VFOs of this row on this radio (running, or paused by the budget)
    int level = 0;         ///< budget steps taken on this row here
    int maxLevel = 0;      ///< steps it could take (band: 5 depth + a chips step and a pause step per band; else a pause per VFO)
    double costPct = 0;    ///< what the row costs now, % of one core (its threads' own clocks)
    int nextKind = 2;      ///< the next step here: 0 depth, 1 a band's extra modes, 2 a whole VFO — so depth drops on EVERY
                           ///  radio's bands before any band loses a mode, and modes everywhere before any band pauses
};
enum Cls : int { kIdle = 0, kListener = 1, kPass = 2 };
struct Entry {
    std::string id;              ///< the radio's serial (file name)
    std::string name;            ///< for the words ("RSP1B")
    int cls = kIdle;
    double bgCores = 0;          ///< its background decoding's CPU, cores
    double frontDropPerMin = 0;  ///< s of IQ a minute its background front end dropped
    bool urgentDrop = false;     ///< its satellite pass's / a listener's decoder dropped input since the last tick
    std::string passName;        ///< the pass on it ("Meteor-M2-4 LRPT"), "" = none
    std::array<CatState, kCatN> cat{};
};
/** One line of text; names may not carry a newline or a tab (the separators). */
inline std::string serialize(const Entry& e) {
    auto clean = [](std::string s) { for (auto& ch : s) if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' '; if (s.size() > 60) s.resize(60); return s; };
    char b[160];
    std::snprintf(b, sizeof b, "v1\t%d\t%.3f\t%.3f\t%d\t", e.cls, e.bgCores, e.frontDropPerMin, e.urgentDrop ? 1 : 0);
    std::string s = b + clean(e.name) + "\t" + clean(e.passName);
    for (int c = 0; c < kCatN; ++c) {
        std::snprintf(b, sizeof b, "\t%d %d %d %.2f %d", e.cat[(size_t)c].vfos, e.cat[(size_t)c].level, e.cat[(size_t)c].maxLevel, e.cat[(size_t)c].costPct,
                      e.cat[(size_t)c].nextKind);
        s += b;
    }
    return s + "\n";
}
inline bool deserialize(const std::string& line, Entry& e) {
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) { size_t q = line.find('\t', p); f.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
    if (f.size() < 7 + kCatN || f[0] != "v1") return false;
    e.cls = std::atoi(f[1].c_str()); e.bgCores = std::atof(f[2].c_str()); e.frontDropPerMin = std::atof(f[3].c_str());
    e.urgentDrop = f[4] == "1"; e.name = f[5]; e.passName = f[6];
    while (!e.passName.empty() && e.passName.back() == '\n') e.passName.pop_back();
    for (int c = 0; c < kCatN; ++c) {
        CatState& s = e.cat[(size_t)c];
        if (std::sscanf(f[(size_t)(7 + c)].c_str(), "%d %d %d %lf %d", &s.vfos, &s.level, &s.maxLevel, &s.costPct, &s.nextKind) < 4) return false;
    }
    return true;
}

// ── The box, as measured this tick ────────────────────────────────────────────────────────────────────────────────
struct Box {
    int64_t nowMs = 0;
    int cores = 1;
    double busyPct = -1;        ///< /proc/stat, 0..100 (-1 unknown: only the drop signals count)
    double clockFrac = 1.0;     ///< the CPU's clock now / its maximum (a power-limited or hot Pi runs slower)
    double curGHz = 0, maxGHz = 0;
    bool throttled = false;     ///< throttled now (heat or power)
    int64_t lastStepMs = -1;    ///< the last budget step anyone on the box took (bgbudget.step), -1 never
};

// ── The thresholds, with their reasons ────────────────────────────────────────────────────────────────────────────
constexpr double kFrontDropSecPerMin = 1.5;  ///< BG-EASE's allowance (2.5 % of the capture): above it slots start going
constexpr double kBusyHigh = 95.0;           ///< nothing dropping but the box this busy: no headroom left for anyone
constexpr double kBusyHighPass = 85.0;       ///< …while a pass is on anywhere: keep a core's worth free for it
constexpr double kBusyLow = 80.0;            ///< a step comes back only below this (the gap is the hysteresis)
/** ★★ (live on the Pi 500, 16:12 11-28) DROPS ARE A CPU PROBLEM ONLY WHEN THE CPU IS SHORT. The first test build shed the
 *  WEFAX and RTTY VFOs for 1.5-9 s/min of front-end drops with the box 35 % busy — drops shedding could not cure (a
 *  hiccup, then the pauses' own chart saves). A front end's drops count only with the machine at least this busy (or
 *  its load unknown). A pass's decoder dropping stays urgent at any load. */
constexpr double kDropsNeedBusy = 85.0;
constexpr int64_t kShedStepMs = 15000;       ///< one step box-wide per FT8 slot — its effect shows before the next
constexpr int64_t kRestoreStepMs = 30000;    ///< and back half as fast
constexpr int64_t kUrgentStepMs = 4000;      ///< a pass's (or a listener's) decoder dropping: a step every other tick — its harm is now
constexpr int64_t kCalmMs = 180000;          ///< 3 calm minutes before anything comes back (BG-EASE's restore wait)

enum class Act { None, Shed, Restore };
struct Decision {
    Act act = Act::None;
    std::string id;     ///< the radio that acts
    int cat = -1;       ///< the row it acts on
    std::string why;    ///< in plain words, for its log line ("to keep the Meteor-M2-4 LRPT pass whole")
};

/** ★ The box's capacity in words, "" at full speed: "this machine is power-limited or hot: running at 1.5 GHz of 2.4".
 *  ★★ Only when it IS limited: throttled now, or its clock CAPPED below the chip's maximum (scaling_max_freq). The current
 *     clock alone means nothing — an ondemand / schedutil governor idles it down (the Pi 500 read 1.7 of 2.2 at 35 % busy,
 *     and the first test build said "power-limited" in every line). */
inline std::string powerWords(const Box& b) {
    if (!(b.clockFrac < 0.95) && !b.throttled) return std::string();
    char t[160];
    if (b.maxGHz > 0 && b.curGHz > 0)
        std::snprintf(t, sizeof t, "this machine is power-limited or hot: running at %.1f GHz of %.1f", b.curGHz, b.maxGHz);
    else std::snprintf(t, sizeof t, "this machine is power-limited or hot (throttled)");
    return t;
}

class Policy {
public:
    /** ★ One tick, the same on every radio process. `entries` = every live radio's line (this one included). */
    Decision tick(const Box& b, const std::vector<Entry>& entries, const Order& order) {
        Decision d;
        if (firstMs_ < 0) firstMs_ = b.nowMs;
        // ── what is wrong, worst first ──
        std::string pass;
        for (auto& e : entries) if (e.cls == kPass && !e.passName.empty()) { pass = e.passName; break; }
        std::string why;
        bool urgent = false;
        for (auto& e : entries) if (e.urgentDrop) {
            urgent = true;
            why = e.cls == kPass ? "to keep the " + (e.passName.empty() ? std::string("satellite") : e.passName) + " pass whole (its decoder fell behind)"
                                 : "for the listener on " + e.name + " (their decoder fell behind)";
            break;
        }
        if (why.empty()) {
            const Entry* worst = nullptr;
            const bool short_ = b.busyPct < 0 || b.busyPct >= kDropsNeedBusy;
            if (short_)
                for (auto& e : entries) if (e.frontDropPerMin > kFrontDropSecPerMin && (!worst || e.frontDropPerMin > worst->frontDropPerMin)) worst = &e;
            if (worst) {
                char t[200];
                std::snprintf(t, sizeof t, "to keep %s's background channels fed (it dropped %.1f s of IQ a minute)", worst->name.c_str(), worst->frontDropPerMin);
                why = t;
            }
        }
        const double high = pass.empty() ? kBusyHigh : kBusyHighPass;
        if (why.empty() && b.busyPct >= high) {
            char t[200];
            if (!pass.empty()) std::snprintf(t, sizeof t, "to keep the %s pass whole (the machine is %.0f %% busy)", pass.c_str(), b.busyPct);
            else std::snprintf(t, sizeof t, "the machine is %.0f %% busy", b.busyPct);
            why = t;
        }
        const std::string pw = powerWords(b);
        if (!why.empty() && !pw.empty()) why += " \xe2\x80\x94 " + pw;
        if (!why.empty()) {
            lastOverMs_ = b.nowMs;
            if (b.lastStepMs >= 0 && b.nowMs - b.lastStepMs < (urgent ? kUrgentStepMs : kShedStepMs)) return d;
            // ── the victim: the LOWEST row with anything left to give, on the radio it costs most (a pass's radio never) ──
            for (int i = kCatN - 1; i >= 0; --i) {
                const int c = order.top[(size_t)i];
                const Entry* v = nullptr;
                for (auto& e : entries) {
                    if (e.cls == kPass) continue;
                    const CatState& s = e.cat[(size_t)c];
                    if (s.vfos <= 0 || s.level >= s.maxLevel) continue;
                    // ★ the gentlest step first box-wide (depth before modes before whole VFOs — Stuart's order across radios
                    //   too); then a radio with nobody on it before one with a listener; then the costlier; then the id
                    if (!v) { v = &e; continue; }
                    const CatState& w = v->cat[(size_t)c];
                    if (s.nextKind != w.nextKind) { if (s.nextKind < w.nextKind) v = &e; continue; }
                    if (e.cls != v->cls) { if (e.cls < v->cls) v = &e; continue; }
                    if (s.costPct > w.costPct + 0.5 || (std::fabs(s.costPct - w.costPct) <= 0.5 && e.id < v->id)) v = &e;
                }
                if (v) { d.act = Act::Shed; d.id = v->id; d.cat = c; d.why = why; return d; }
            }
            return d;   // ★ nothing left to shed (only distress watches, or all at their floor): the health card says TOO HEAVY
        }
        // ── calm: give back, the TOP of the list first (a process just started has seen no calm yet: it waits too) ──
        if (firstMs_ < 0) firstMs_ = b.nowMs;
        const int64_t since = std::max(lastOverMs_, firstMs_);
        const bool calm = (b.busyPct < 0 || b.busyPct < kBusyLow) && b.nowMs - since >= kCalmMs;
        if (!calm) return d;
        if (b.lastStepMs >= 0 && b.nowMs - b.lastStepMs < kRestoreStepMs) return d;
        for (int i = 0; i < kCatN; ++i) {
            const int c = order.top[(size_t)i];
            const Entry* v = nullptr;
            for (auto& e : entries) {
                const CatState& s = e.cat[(size_t)c];
                if (s.level <= 0) continue;
                if (!v || s.level > v->cat[(size_t)c].level || (s.level == v->cat[(size_t)c].level && e.id < v->id)) v = &e;
            }
            if (v) { d.act = Act::Restore; d.id = v->id; d.cat = c; d.why = "calm for 3 minutes"; return d; }
        }
        return d;
    }
    /** ★ Test seam / a restart: when the box was last over budget. */
    void setLastOver(int64_t ms) { lastOverMs_ = ms; }
private:
    int64_t lastOverMs_ = -1, firstMs_ = -1;
};

/** ★ A row's live state in words, summed over the box (the Setup page beside each row). */
inline std::string stateWords(int c, const std::vector<Entry>& entries) {
    int vfos = 0, lvlMax = 0, paused = 0, chips = 0, bands = 0;
    std::string passOn;
    for (auto& e : entries) {
        const CatState& s = e.cat[(size_t)c];
        if (s.vfos <= 0) continue;
        if (e.cls == kPass) { passOn = e.name; continue; }   // ★ that radio's background gives the capture to its pass
        vfos += s.vfos;
        if (c == kBand) {
            bands += s.vfos;
            lvlMax = std::max(lvlMax, std::min(s.level, 5));
            const int rest = std::max(0, s.level - 5);
            chips += std::min(rest, s.vfos);
            paused += std::max(0, rest - s.vfos);
        } else paused += s.level;
    }
    const std::string forPass = passOn.empty() ? std::string() : "paused for the satellite pass on " + passOn;
    if (vfos == 0) return passOn.empty() ? "not running on this box" : forPass;
    std::string t;
    if (c == kBand) {
        static const char* depth[] = { "full depth", "depth reduced (no Dig deeper)", "depth reduced (no OSD or a-priori)",
                                       "depth reduced (one pass a slot)", "depth reduced (fewer candidates)", "depth at the minimum" };
        t = depth[lvlMax];
        if (chips > 0) t += ", CW / RTTY / PSK off on " + std::to_string(chips) + " band" + (chips == 1 ? "" : "s");
        if (paused > 0) t += ", " + std::to_string(paused) + " of " + std::to_string(bands) + " bands paused";
        return forPass.empty() ? t : t + "; " + forPass;
    }
    t = paused == 0 ? std::string("running") : std::to_string(paused) + " of " + std::to_string(vfos) + " paused";
    return forPass.empty() ? t : t + "; " + forPass;
}

// ── The ledger's files ────────────────────────────────────────────────────────────────────────────────────────────
class Board {
public:
    ~Board() { close(); }
    void open(const std::string& dir, const std::string& id) {
        if (fd_ >= 0 || dir.empty()) return;
        dir_ = dir; id_ = id;
        fd_ = ::open(path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX | LOCK_NB) != 0) { ::close(fd_); fd_ = -1; }
    }
    void close() {
        if (fd_ < 0) return;
        ::unlink(path().c_str());
        ::flock(fd_, LOCK_UN); ::close(fd_); fd_ = -1; last_.clear();
    }
    bool isOpen() const { return fd_ >= 0; }
    const std::string& dir() const { return dir_; }
    const std::string& id() const { return id_; }
    void publish(const Entry& e) {
        if (fd_ < 0) return;
        const std::string s = serialize(e);
        if (s == last_) return;
        last_ = s;
        if (::ftruncate(fd_, 0) == 0) (void)!::pwrite(fd_, s.data(), s.size(), 0);
    }
    /** Every live radio's line (a dead process's file is unlocked: it counts for nothing). */
    static std::vector<Entry> all(const std::string& dir) {
        std::vector<Entry> out;
        if (dir.empty()) return out;
        DIR* d = ::opendir(dir.c_str());
        if (!d) return out;
        while (dirent* de = ::readdir(d)) {
            const std::string f = de->d_name;
            if (f.rfind("bgbudget-", 0) != 0 || f.size() < 16 || f.compare(f.size() - 6, 6, ".state") != 0) continue;
            const int fd = ::open((dir + "/" + f).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            const bool alive = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
            if (!alive) ::flock(fd, LOCK_UN);
            char b[1024] = {0};
            const ssize_t got = alive ? ::pread(fd, b, sizeof b - 1, 0) : 0;
            ::close(fd);
            if (got <= 0) continue;
            Entry e;
            if (!deserialize(std::string(b, (size_t)got), e)) continue;
            e.id = f.substr(9, f.size() - 15);
            out.push_back(e);
        }
        ::closedir(d);
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
        return out;
    }
    static int64_t lastStep(const std::string& dir) {
        if (dir.empty()) return -1;
        const int fd = ::open((dir + "/bgbudget.step").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        char b[32] = {0};
        const ssize_t got = ::pread(fd, b, sizeof b - 1, 0);
        ::close(fd);
        return got > 0 ? std::atoll(b) : -1;
    }
    static void noteStep(const std::string& dir, int64_t ms) {
        if (dir.empty()) return;
        const std::string p = dir + "/bgbudget.step", t = p + ".tmp" + std::to_string((long long)::getpid());
        const int fd = ::open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return;
        const std::string s = std::to_string((long long)ms);
        (void)!::write(fd, s.data(), s.size());
        ::close(fd);
        ::rename(t.c_str(), p.c_str());
    }
    /** ★ The owner's order as the front door last saved it ("" = never saved: the default). */
    static std::string readOrder(const std::string& dir) {
        if (dir.empty()) return std::string();
        const int fd = ::open((dir + "/bg-priority").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::string();
        char b[256] = {0};
        const ssize_t got = ::pread(fd, b, sizeof b - 1, 0);
        ::close(fd);
        return got > 0 ? std::string(b, (size_t)got) : std::string();
    }
    static bool writeOrder(const std::string& dir, const Order& o) {
        if (dir.empty()) return false;
        const std::string p = dir + "/bg-priority", t = p + ".tmp" + std::to_string((long long)::getpid());
        const int fd = ::open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        const std::string s = o.str();
        const bool ok = ::write(fd, s.data(), s.size()) == (ssize_t)s.size();
        ::close(fd);
        return ok && ::rename(t.c_str(), p.c_str()) == 0;
    }
private:
    std::string path() const { return dir_ + "/bgbudget-" + id_ + ".state"; }
    std::string dir_, id_, last_;
    int fd_ = -1;
};

/** ★ The CPU's clock now and at most (Linux cpufreq; 0 = unknown), for powerWords. */
/** ★ curGHz = the clock the CPU is ALLOWED now (scaling_max_freq — lowered by a thermal / power cap), maxGHz = the chip's
 *  maximum (cpuinfo_max_freq). Never scaling_cur_freq: that is the governor's idea of the moment, not a limit. */
inline void readClock(double& curGHz, double& maxGHz) {
    curGHz = maxGHz = 0;
    auto rd = [](const char* p) -> double {
        FILE* f = std::fopen(p, "r");
        if (!f) return 0;
        long long v = 0;
        const int k = std::fscanf(f, "%lld", &v);
        std::fclose(f);
        return k == 1 ? (double)v / 1e6 : 0;   // kHz → GHz
    };
    curGHz = rd("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    maxGHz = rd("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
}

}  // namespace budget
}  // namespace bg
}  // namespace vibe
