// vibe_bg_fullcpu.h — NOBODY LISTENING: BACKGROUND DECODING MAY USE THE WHOLE MACHINE (V12 beta94, 2026-11-25).
//
// ★★★ THE OWNER'S RULE (Stuart, 25 Nov): "if nobody is using the pi then the full cpu can be allowed for background use,
//     soon as a listener joins and starts using then the background gets cut back as needed".
//     What he saw (Pi 500, RSP1B, a Band profile at 6.8 MHz): "Band: CW paused — the CPU is behind (a slotted mode skipped
//     a slot)" every few minutes, RTTY paused too, a WEFAX VFO "waiting — no decoder slot free (listeners come first)",
//     the RTTY skimmer on 2 search workers — while the machine sat about 47 % idle. Every limit was sized for a box with a
//     listener on it, and stayed that size with nobody there.
//
// ── The two states ────────────────────────────────────────────────────────────────────────────────────────────────
//  NORMAL (anybody streaming from this radio, or still inside the calm period after the last one left): today's budget,
//    exactly — the benchmark's / owner's figures, decoder slots shared with listeners, the pause rule at 1.5 s / any skip.
//  FULL (nobody streaming for the calm period — the owner's idle grace, 30 s…10 min — and the machine not hot, not on
//    battery): background decoding may spread over `cores` of this machine:
//      • the RTTY skimmer's search workers up to `cores` (never below the owner's / benchmark's figure — the FLOOR);
//      • `extraSlots` decoder slots over the box's limit, for BACKGROUND only (a WEFAX VFO "waiting for a decoder slot"
//        runs) — a listener anywhere on the box still gets one at once (BgRegistry::makeRoomForListener asks as many
//        background VFOs to give way as the listener needs);
//      • the Band governor's budget × `cores` (it is one core's green rung — kBudgetPct of ONE core — and the engines run
//        on their own threads);
//      • the pause rule LOOSER: a skipped slot pauses CW / RTTY only when the machine has no headroom (pausing them then
//        frees CPU; with headroom it frees nothing — the slow engine is single-threaded), and a backlog only when it has
//        lasted kFullBehindHoldSec (a burst no longer sheds). The slotted modes keep their slots either way.
//    Every thread keeps its rung (nice 19, vibe_thread.h): FULL uses more of the IDLE CPU; it never outranks the OS, the
//    radio's capture or its DSP.
//
// ── A listener joins ──────────────────────────────────────────────────────────────────────────────────────────────
//  NORMAL at once (no calm period that way round). The decoder slots a listener needs are released immediately, as
//  before; extras over the box limit are given back lowest VFO first; the RTTY workers drop to the floor; the Band budget
//  falls to its normal cap (the governor sheds lowest priority first) and the pause rule tightens — CW, then RTTY pause
//  first if the decode falls behind, the slotted modes keep their slots.
//
// ── Many radios on one machine (the Pi 500: four processes, one per radio, four cores) ─────────────────────────────
//  "Full CPU" cannot mean every idle radio's process takes every core. Each process says on the box board
//  (<slot dir>/bgcpu-<id>.idle, flock-held for liveness, the BgRegistry way) whether its radio is idle with background
//  decoding running. Then, from MEASURED load (/proc/stat via vibehealth::Sampler — machine busy %):
//      share   = max(1, cores ÷ idle radios on the box)           — its fair part of the machine
//      spare   = cores × (100 − busy %) ÷ 100                      — what is free right now
//      allowance (cores) starts at min(share, max(1, ⌊spare⌋)); every kStepMs it goes UP one while spare ≥ kSpareUp
//      cores and it is under `share`, and DOWN one (never below 1) while spare < kSpareDown. Spare CPU is used only
//      while it is actually spare; a listener's radio on the same box simply leaves less spare.
//  Unknown load (Android: only this process is visible): `share` is capped at half the cores — a phone never gets all of
//  them on a guess.
//
// ── Phones and the handheld (same code) ─────────────────────────────────────────────────────────────────────────────
//  Never FULL while the health sampler says the CPU is throttled (heat or power) or the temperature is HIGH or worse, or
//  while running on battery (not charging, not on mains). Leaving FULL for one of those is logged with the reason.
//
// ★ Pure apart from Board (a few small files): Policy takes every input as a number (test-bg-fullcpu.cpp drives it).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace vibe {
namespace bg {
namespace fullcpu {

// ── The thresholds, with their reasons ─────────────────────────────────────────────────────────────────────────────
constexpr double kMinCalmSec = 30.0;        ///< the shortest calm period, whatever the owner's idle grace (a reload is not a leave)
constexpr double kMaxCalmSec = 600.0;       ///< …and the longest
constexpr int64_t kStepMs = 10000;          ///< the allowance moves at most one core every 10 s (5 health ticks)
constexpr double kSpareUp = 1.25;           ///< a whole core free and a quarter to spare: take one more
constexpr double kSpareDown = 0.4;          ///< under 0.4 of a core free (90 % busy on 4 cores): give one back
constexpr double kSkipNeedsSpare = 0.5;     ///< FULL: a skipped slot pauses CW/RTTY only with less than half a core free
constexpr double kFullBehindHoldSec = 10.0; ///< FULL: a backlog over the pause line must last this long to pause
constexpr int kMaxRttyWorkers = 8;          ///< RttySkimRun's own cap

/** ★ What the decoder threads read, lock-free. One per runner (shared_ptr), written by the shim's health tick. */
struct Live {
    std::atomic<bool> full{false};
    std::atomic<int>  cores{1};          ///< cores this radio's background may spread over (1 = today's single-core budget)
    std::atomic<int>  rttyWorkers{0};    ///< RTTY skimmer search workers now (0 = the floor, unchanged)
    std::atomic<int>  extraSlots{0};     ///< decoder slots over the box's limit, background only
    std::atomic<int>  spareMilli{-1};    ///< the machine's spare cores × 1000 (-1 = unknown)
    /** The Band governor's cap multiplier. */
    double budgetScale() const { return full.load(std::memory_order_relaxed) ? (double)std::max(1, cores.load(std::memory_order_relaxed)) : 1.0; }
    /** Does a skipped slot count for the pause rule (and the governor's "too heavy" cut)? NORMAL: always. FULL: only with
     *  no headroom (or none known). */
    bool skipCounts() const {
        if (!full.load(std::memory_order_relaxed)) return true;
        const int s = spareMilli.load(std::memory_order_relaxed);
        return s < 0 || s < (int)(kSkipNeedsSpare * 1000);
    }
    /** How long a backlog over the pause line must last before it pauses the next continuous mode (0 = at once). */
    double behindHoldSec() const { return full.load(std::memory_order_relaxed) ? kFullBehindHoldSec : 0.0; }
};

/** One health tick's measurements (~2 s). */
struct Inputs {
    int64_t nowMs = 0;
    int     streaming = 0;          ///< listeners actually streaming from this radio (a HELD session is not)
    bool    bgRunning = false;      ///< this radio's background decoding is running
    int     cores = 0;              ///< the machine's
    double  machineBusyPct = -1;    ///< 0..100, -1 unknown (Android)
    int     idleRadios = 1;         ///< radios on the box idle with background running (this one included), from the board
    bool    hot = false;            ///< throttled (heat / power) or temperature HIGH+
    bool    onBattery = false;      ///< a battery present and neither charging nor on mains
    double  graceSec = 300.0;       ///< the owner's idle grace (g_vsIdleGraceSec)
    int     rttyFloor = 1;          ///< the owner's / benchmark's RTTY search workers — never fewer
};

/** What background decoding may use now. */
struct Budget {
    bool full = false;
    int  cores = 1;          ///< allowance (FULL), 1 otherwise
    int  share = 1;          ///< this radio's fair part of the box (FULL)
    int  rttyWorkers = 1;
    int  extraSlots = 0;
    double spare = -1;       ///< spare cores measured (-1 unknown)
};

class Policy {
public:
    /** ★ One tick. Returns the budget now; `said()` is a log line when the state or the allowance changed ("" = none). */
    Budget tick(const Inputs& in) {
        said_.clear();
        const int cores = std::max(1, in.cores);
        const double spare = in.machineBusyPct >= 0 ? cores * (100.0 - std::min(100.0, in.machineBusyPct)) / 100.0 : -1.0;
        const double calm = std::max(kMinCalmSec, std::min(kMaxCalmSec, std::isfinite(in.graceSec) ? in.graceSec : 300.0));
        const char* blockWhy = in.hot ? "the CPU is hot or throttled" : in.onBattery ? "it is running on battery" : nullptr;
        const int floor = std::max(1, in.rttyFloor);

        if (in.streaming > 0) {
            quietSince_ = -1;
            if (b_.full) {
                said_ = "background decoding: a listener joined \xe2\x80\x94 background cut back to the normal budget ("
                      + std::to_string(floor) + " RTTY search worker" + (floor == 1 ? "" : "s")
                      + ", decoder slots shared with listeners, CW/RTTY paused first if the decode falls behind)";
            }
            return normal(floor, spare);
        }
        if (!in.bgRunning) { quietSince_ = -1; b_ = normal(floor, spare); return b_; }   // ★ nothing to give the CPU to
        if (quietSince_ < 0) quietSince_ = in.nowMs;
        if (blockWhy) {
            if (b_.full || !blockSaid_) {
                said_ = std::string("background decoding: nobody listening, but ") + blockWhy
                      + " \xe2\x80\x94 staying at the normal budget";
                blockSaid_ = true;
            }
            return normal(floor, spare);
        }
        blockSaid_ = false;
        if (!b_.full && (double)(in.nowMs - quietSince_) < calm * 1000.0) return normal(floor, spare);

        // ── FULL: this radio's share of the box, and the allowance inside it from what is measured spare ──
        int share = std::max(1, cores / std::max(1, in.idleRadios));
        if (spare < 0) share = std::max(1, std::min(share, cores / 2));   // ★ unknown load (Android): never every core on a guess
        if (!b_.full) {
            b_.full = true;
            b_.cores = spare < 0 ? share : std::max(1, std::min(share, (int)std::floor(spare)));
            stepAt_ = in.nowMs;
            said_ = "background decoding: nobody listening \xe2\x80\x94 using the whole CPU ("
                  + std::to_string(cores) + " core" + (cores == 1 ? "" : "s")
                  + (in.idleRadios > 1 ? ", shared with " + std::to_string(in.idleRadios - 1) + " other idle radio" + (in.idleRadios == 2 ? "" : "s") : std::string())
                  + "): up to " + std::to_string(b_.cores) + " core" + (b_.cores == 1 ? "" : "s") + " now";
        } else if (in.nowMs - stepAt_ >= kStepMs) {
            const int was = b_.cores;
            if (b_.cores > share) b_.cores = share;                                      // ★ another radio went idle
            else if (spare >= kSpareUp && b_.cores < share) ++b_.cores;
            else if (spare >= 0 && spare < kSpareDown && b_.cores > 1) --b_.cores;
            if (b_.cores != was) {
                stepAt_ = in.nowMs;
                char b[200];
                std::snprintf(b, sizeof b, "background decoding: nobody listening \xe2\x80\x94 %s to %d core%s (%.1f of %d free, this radio's share %d)",
                              b_.cores > was ? "spreading" : "easing back", b_.cores, b_.cores == 1 ? "" : "s",
                              spare < 0 ? 0.0 : spare, cores, share);
                said_ = b;
            }
        }
        b_.share = share;
        b_.spare = spare;
        b_.rttyWorkers = std::min(kMaxRttyWorkers, std::max(floor, b_.cores));
        b_.extraSlots = b_.cores;
        return b_;
    }
    const std::string& said() const { return said_; }
    const Budget& budget() const { return b_; }
    /** Publish a budget for the decoder threads. */
    static void store(Live& l, const Budget& b) {
        l.cores.store(b.cores, std::memory_order_relaxed);
        l.rttyWorkers.store(b.rttyWorkers, std::memory_order_relaxed);
        l.extraSlots.store(b.extraSlots, std::memory_order_relaxed);
        l.spareMilli.store(b.spare < 0 ? -1 : (int)std::lround(b.spare * 1000.0), std::memory_order_relaxed);
        l.full.store(b.full, std::memory_order_relaxed);
    }
private:
    Budget normal(int floor, double spare) {
        b_ = Budget();
        b_.rttyWorkers = floor; b_.spare = spare;
        return b_;
    }
    Budget b_;
    int64_t quietSince_ = -1, stepAt_ = 0;
    bool blockSaid_ = false;
    std::string said_;
};

// ── The box board: which radios are idle with background decoding running ─────────────────────────────────────────
/** ★ <dir>/bgcpu-<id>.idle, held by an exclusive flock while the process lives (a crashed radio's file reads as dead),
 *  containing "1" (idle, background running) or "0". No directory (Android, a test without one) = this radio alone. */
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
        ::flock(fd_, LOCK_UN); ::close(fd_); fd_ = -1; last_ = -1;
    }
    bool isOpen() const { return fd_ >= 0; }
    void publish(bool idle) {
        if (fd_ < 0 || (int)idle == last_) return;
        last_ = (int)idle;
        const char c = idle ? '1' : '0';
        if (::ftruncate(fd_, 0) == 0) (void)!::pwrite(fd_, &c, 1, 0);
    }
    /** Live radios on the box that say idle. `self` counts this one whatever its file says (≥ 1 when it is idle). */
    static int countIdle(const std::string& dir) {
        if (dir.empty()) return 0;
        int n = 0;
        DIR* d = ::opendir(dir.c_str());
        if (!d) return 0;
        while (dirent* e = ::readdir(d)) {
            const std::string f = e->d_name;
            if (f.rfind("bgcpu-", 0) != 0 || f.size() < 12 || f.compare(f.size() - 5, 5, ".idle") != 0) continue;
            const int fd = ::open((dir + "/" + f).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            const bool alive = ::flock(fd, LOCK_SH | LOCK_NB) != 0;   // ★ held by its process = alive
            if (!alive) ::flock(fd, LOCK_UN);
            char c = 0;
            if (alive && ::pread(fd, &c, 1, 0) == 1 && c == '1') ++n;
            ::close(fd);
        }
        ::closedir(d);
        return n;
    }
private:
    std::string path() const { return dir_ + "/bgcpu-" + id_ + ".idle"; }
    std::string dir_, id_;
    int fd_ = -1, last_ = -1;
};

}  // namespace fullcpu
}  // namespace bg
}  // namespace vibe
