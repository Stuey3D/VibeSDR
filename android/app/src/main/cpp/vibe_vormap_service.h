// vibe_vormap_service.h — V12 VOR / ILS MAP MODE: the whole 108–117.95 MHz navaid band swept window by window, EVERY 50 kHz
// channel of a window decoded at once (the single-channel receiver, decoders/nav_rx.h, one per channel), each beacon named
// by its OWN ident read off the air and matched to the published list near the receiver (vibe_vor_db.h). The clients draw
// it as a map (docs/v12/VOR-MAP.md): the stations heard at their listed positions, a VOR's radial as a line towards the
// receiver, and where radials from different stations cross, the receiver's fix.
//
// ★★★ STUART, 2026-11-24: "VOR/ILS shares no spectrum with other modes" — a map-based, WHOLE-RADIO mode treated the same
//     as NDB: the same owner switch, shared-dial and only-mode rules, benchmark-sets-default / never-hide, offered in the
//     same places. ★ No rate change, ever: the windows are cut from the rate the radio RUNS at.
//
// ★★ THE CHANNEL PLAN (ICAO Annex 10 Vol. I; nav_rx.h channelKind): 108.00–111.95 MHz, even tenths (108.00, .05, .20, .25 …)
//    are VORs and odd tenths (108.10, .15, .30, .35 …) ILS localisers; 112.00–117.95 MHz VOR only. A 50 kHz grid — 8.33 kHz
//    spacing is voice only and does not apply. 200 channels.
// ★★ THE SWEEP: the capture is stepped across the band in WINDOWS (plan()): as many channels as fit in 90 % of the running
//    rate (the radio's own roll-off is outside it), the windows as even as the channel count allows, each centred BETWEEN two
//    channels so the radio's DC remnant lands in no channel. An RTL at 2.4 MS/s: 5 windows of 40 channels. An RSP at 8 MS/s:
//    2 of 100; at 10 MS/s 2. Each window is held kDwellSec (30 s): the bearing (30 Hz + 9960 Hz) is continuous and is held
//    after 3 agreeing seconds, the Morse ident (1020 Hz) comes every 10–15 s — 30 s reads it twice, which with the
//    published station on that channel confirms it (vibe_nav_run.h's rule), or three readings by consensus.
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS (Network > Audio > Spectrum > Decoders). feed() copies into a
//     quarter-second queue that loses the OLDEST block and counts it. The decoder thread runs ONE front end per window (the
//     aviation datalink's: half-bands + one forward FFT, vibe_avi_front.h MultiChannel), and its channels are dealt across
//     worker threads (vibe_avi_workers.h — up to 4, decoder priority), each running nav::NavRx on its channels (the hot loops
//     NEON / SSE2 with scalar twins, nav_rx.h). A channel's state is touched only by its worker; a beacon's record is
//     written under one lock, once a second at most.
// ★★ MEMORY IS BOUNDED (the 256–512 MB boxes): the queue (¼ s of capture: 2.4 MB at 2.4 MS/s), the workers' rings (≤ 2 MB of
//    FFT bins each), one NavRx per channel of the WINDOW only (a few tens of KB each), 200 small beacon records.
// ★★ A NAME IS EARNED ON AIR: a beacon is named only by an ident read from it, matched to a station published on THAT
//    channel near the receiver. A channel with no ident read stays "unidentified" — never named from the list alone.
// ★★★ THE BEARING IS THE SECOND WITNESS (Stuart, beta94 — the XCover + HF+ 11-24: DTY 116.40 held at 054° FROM, −62 dB, "DTY"
//     read ONCE and never again over two sweeps, so it stayed unidentified). The 30 Hz bearing comes through at levels where
//     the 1020 Hz Morse only reads now and then. So: ONE read of a station listed on the channel, PLUS the measured radial
//     within ±6° (+ what 1 km of position error subtends, geoTolDeg) of the radial that station should send to THIS receiver
//     (expectedRadial: the great-circle bearing station → receiver, less the station's own variation) = identified. ★ The ident from the air is still required — the
//     bearing replaces the REPEAT read, never the read. ★ Only with the receiver's position; only kGeoMinKm–kGeoMaxKm from the
//     station (closer, a small error in the position swings the radial; further, the station is beyond what a ground
//     receiver hears, so a match there is chance). Without a position: today's rule (two reads, or three by consensus).
#pragma once
#include "decoders/nav_rx.h"
#include "decoders/ndb_skimmer.h"   // ndb::IdentVote — the NDB skimmer's consensus
#include "vibe_vor_db.h"
#include "vibe_avi_front.h"
#include "vibe_avi_workers.h"
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibevormap {

using cf32 = std::complex<float>;

constexpr int64_t kFirstChanHz = 108000000;   ///< the first navaid channel
constexpr int64_t kLastChanHz  = 117950000;   ///< the last
constexpr int64_t kChanStepHz  = 50000;       ///< the 50 kHz grid (8.33 kHz is voice only)
constexpr int     kChannels    = 200;
/** ★★ HOW LONG EACH WINDOW IS HELD. The bearing needs 3 agreeing seconds; the ident comes every 10–15 s and is read
 *  twice in 30 s (Stuart, 2026-11-24). */
constexpr double  kDwellSec    = 30.0;
/** ★ Samples thrown away after a retune: the tuner's PLL and the hardware's own queue still hold the last window. */
constexpr double  kSettleSec   = 0.30;
/** ★ Each channel passes ±12.5 kHz: the 9960 Hz subcarrier and its ±480 Hz deviation, with room for a station off frequency. */
constexpr double  kHalfWidthHz = 12500.0;
/** ★ The slowest channel rate accepted (nav_rx.h wants ≥ 24 kHz; the slice is flat to a quarter of its rate). */
constexpr double  kMinChanRate = 30000.0;
/** ★ The share of the capture used (the outer 5 % each side is the radio's anti-alias roll-off). */
constexpr double  kUsable      = 0.90;
/** ★ More windows than this and a sweep is too slow to be a map. ★★ 2026-11-24 (Stuart, "yes" to the Airspy HF+): 20, so the
 *  HF+ is in — 912 kS/s = 14 windows (~7 min a sweep), 768 kS/s = 16 (~8 min). VORs do not move, so a slow sweep still
 *  draws a true map; the status words say how long a sweep takes. (Was 12 and 1 MS/s, which left the HF+ out.) */
constexpr int     kMaxWindows  = 20;
/** ★★ THE GEOMETRY WITNESS (beta94, Stuart: "about ±6°"). The measured radial must be within this of the expected one. */
constexpr double  kGeoTolDeg   = 6.0;
/** ★★ PLUS the angle the receiver's own position error subtends at the station: the app publishes the phone's position
 *  COARSENED to about 1 km (setLocationJson), and an owner types a town or a locator. 1 km at 20 km is 2.9°, at 100 km
 *  0.6°. Measured, 11-24: DTY held 054–055° at Stuart's; the radial expected at the XCover's published position (52.29,
 *  −0.85) is 059.7° — 5.7° off, a hair inside a bare ±6°; with the allowance (8.6° at 21.7 km) it is safely in. */
constexpr double  kGeoPosKm    = 1.0;
inline double geoTolDeg(double km) { return kGeoTolDeg + std::atan2(kGeoPosKm, km) * 180.0 / M_PI; }
/** ★ Closer than this the expected radial is not trusted: 1 km of error in the receiver's position is already ~6° at 10 km
 *  (a Maidenhead 6-character square is ~5 × 4.6 km). The repeat read is then still needed. */
constexpr double  kGeoMinKm    = 10.0;
/** ★ Further than this a station is beyond what a receiver on the ground hears (a VOR's coverage is for aircraft; on the
 *  ground the horizon is tens of km, a hill or a lift a few hundred) — a radial "matching" one that far off is chance. */
constexpr double  kGeoMaxKm    = 300.0;
/** ★ The lowest running rate offered: ≥ 700 kS/s keeps the sweep to 20 windows (~10 min) at most. */
constexpr double  kMinRateHz   = 0.70e6;

inline int64_t chanHz(int i) { return kFirstChanHz + int64_t(i) * kChanStepHz; }
/** 1 VOR, 2 ILS localiser (nav_rx.h's channelKind on our grid). */
inline int chanKind(int64_t hz) { return vibe::nav::channelKind((double)hz); }

struct Window {
    int first = 0, last = 0;        ///< channel indices, inclusive
    double hwCentre = 0;            ///< where the radio is tuned (between two channels)
    double lo() const { return (double)chanHz(first) - kChanStepHz / 2; }
    double hi() const { return (double)chanHz(last) + kChanStepHz / 2; }
};
struct Plan { double rate = 0; std::vector<Window> w; };

/** ★ The windows for a capture of `rate`: as few as the band needs, as even as the channel count allows, each centred
 *  BETWEEN two channels (an even count is centred between its middle two; an odd one moved half a channel up). False when
 *  the rate cannot hold a channel or the band needs more than kMaxWindows. */
inline bool plan(double rate, Plan& p) {
    p = Plan();
    if (!(rate >= kMinRateHz)) return false;
    const double half = rate * kUsable / 2;
    // (n−1)·25 kHz + 25 kHz (the odd-count shift) + the channel's half-width ≤ half the usable span
    const int perWin = (int)std::floor((half - kHalfWidthHz - kChanStepHz / 2) / (kChanStepHz / 2)) + 1;
    if (perWin < 1) return false;
    const int nWin = (kChannels + perWin - 1) / perWin;
    if (nWin > kMaxWindows) return false;
    p.rate = rate;
    const int base = kChannels / nWin, extra = kChannels % nWin;
    int at = 0;
    for (int i = 0; i < nWin; ++i) {
        Window w;
        const int n = base + (i < extra ? 1 : 0);
        w.first = at; w.last = at + n - 1; at += n;
        w.hwCentre = 0.5 * ((double)chanHz(w.first) + (double)chanHz(w.last));
        if (n % 2) w.hwCentre += kChanStepHz / 2;   // ★ an odd count's middle is a channel: half a channel up
        p.w.push_back(w);
    }
    return true;
}
/** The window that holds channel `hz` (−1 = none). */
inline int windowOf(const Plan& p, int64_t hz) {
    const int i = (int)((hz - kFirstChanHz) / kChanStepHz);
    for (size_t k = 0; k < p.w.size(); ++k) if (i >= p.w[k].first && i <= p.w[k].last) return (int)k;
    return -1;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c >= 0x20) o += (char)c;
    }
    return o;
}
/** Idents are A–Z / 0–9 by construction (nav_rx.h's Morse table) — this keeps it so whatever arrives. */
inline std::string cleanIdent(const std::string& s) {
    std::string o;
    for (char c : s) if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += c;
    return o.substr(0, 6);
}

struct ServiceOpts {
    int workers = -1;               ///< -1: vibeavi::defaultWorkers() (up to 4); 0: all on the decoder thread
    double dwellSec = kDwellSec;
    double settleSec = kSettleSec;
    bool lossless = false;          ///< tests / benches: the workers' rings and the queue never drop (the producer waits)
    bool forceScalar = false;       ///< nav_rx.h's scalar twins (the SIMD = scalar check)
};

class Service {
public:
    using Opts = ServiceOpts;
    ~Service() { stop(); }

    /** Start on a capture of `p.rate`. `havePos`: the receiver's position is known (lat, lon) — the nearest published
     *  station on a channel first, distances to 10 km. The caller tunes the radio to window 0 and calls tunedTo(0). */
    void start(const Plan& p, bool havePos, double lat, double lon, Opts o = Opts()) {
        stop();
        {
            std::lock_guard<std::mutex> lk(m_);
            plan_ = p; o_ = o; havePos_ = havePos; lat_ = lat; lon_ = lon;
            beacons_.clear(); report_.clear(); started_ = nowUtc();
            for (int i = 0; i < kChannels; ++i) {
                Beacon b; b.hz = chanHz(i); b.expect = chanKind(b.hz); b.win = windowOf(p, b.hz);
                b.cands = vibevor::on((int)(b.hz / 1000), b.expect == 2 ? 1 : 0, havePos, lat, lon);
                beacons_.emplace(b.hz, std::move(b));
            }
        }
        { std::lock_guard<std::mutex> lk(qm_); q_.clear(); queued_ = 0; }
        dropped_.store(0); wDropped_.store(0); wDroppedBase_ = 0; win_.store(0); gen_.store(0); sweep_.store(0); fedInWin_ = 0; hopAsked_ = false; streamPos_ = 0;
        lastReportMs_ = -100000; streamMs_.store(0);   // ★★ 11-24: the report clock restarts with the stream (a second ON sent no report until the old count was passed)
        decGen_ = -1; decWin_ = -1;
        run_.store(true);
        th_ = std::thread([this] { vibeDecoderThread("vibe-vormap"); loop_(); });
    }
    void stop() {
        if (!run_.exchange(false)) return;
        qcv_.notify_all(); roomCv_.notify_all();
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> wk(work_);
        dropWindow_();
    }
    bool running() const { return run_.load(std::memory_order_relaxed); }

    // ── the DSP thread ──
    /** Copy and return. Over a quarter of a second queued, the OLDEST block goes (and is counted). It is also the
     *  sweep's clock: the dwell is counted in samples fed, so the window changes on time even when the decoder lags. */
    void feed(const cf32* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || n == 0) return;
        Block b; b.v.assign(iq, iq + n); b.gen = gen_.load(std::memory_order_relaxed); b.win = win_.load(std::memory_order_relaxed);
        b.pos = streamPos_;
        streamPos_ += (int64_t)n;
        {
            std::unique_lock<std::mutex> lk(qm_);
            if (o_.lossless) roomCv_.wait(lk, [&] { return !run_.load() || queued_ < (size_t)(plan_.rate * 0.25) || q_.empty(); });
            queued_ += n;
            q_.push_back(std::move(b));
            while (!o_.lossless && queued_ > (size_t)(plan_.rate * 0.25) && q_.size() > 1) { queued_ -= q_.front().v.size(); q_.pop_front(); dropped_.fetch_add(1); }
        }
        qcv_.notify_one();
        fedInWin_ += (int64_t)n;
        if (!hopAsked_ && plan_.w.size() > 1 && (double)fedInWin_ >= (o_.settleSec + o_.dwellSec) * plan_.rate) {
            hopAsked_ = true;
            want_ = (win_.load() + 1) % (int)plan_.w.size();
        }
        if (plan_.w.size() == 1 && (double)fedInWin_ >= (o_.settleSec + o_.dwellSec) * plan_.rate) {
            fedInWin_ = 0; sweep_.fetch_add(1);   // ★ one window holds the whole band: a "sweep" is a dwell
        }
    }
    /** The window the radio should go to now, or −1. (The DSP thread, after feed.) */
    int hopDue() const { return hopAsked_ ? want_ : -1; }
    /** The radio is on window `w` now (the DSP thread, right after the retune). Samples fed from here are that window's. */
    void tunedTo(int w) {
        if (w < 0 || w >= (int)plan_.w.size()) return;
        if (w == 0 && win_.load() != 0) sweep_.fetch_add(1);
        win_.store(w);
        gen_.fetch_add(1);
        fedInWin_ = 0; hopAsked_ = false;
    }
    const Plan& plan() const { return plan_; }
    int window() const { return win_.load(); }
    int sweep() const { return sweep_.load(); }
    double windowCentre() const { const int w = win_.load(); return w >= 0 && w < (int)plan_.w.size() ? plan_.w[(size_t)w].hwCentre : 0.0; }
    uint64_t droppedBlocks() const { return dropped_.load() + wDropped_.load(); }
    double queuedSec() const { std::lock_guard<std::mutex> lk(qm_); return plan_.rate > 0 ? (double)queued_ / plan_.rate : 0.0; }
    /** Wait until the queue and the workers are empty (tests, benches). */
    void drain() {
        for (int i = 0; i < 20000; ++i) {
            bool idle;
            { std::lock_guard<std::mutex> lk(qm_); idle = q_.empty() && !busy_; }
            if (idle) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::lock_guard<std::mutex> wk(work_);
        if (workers_) workers_->flush();
    }

    /** The last report — {"type":"vormap",…} — or "" before the first. A copy: never waits on the decoder. */
    std::string report() const { std::lock_guard<std::mutex> lk(rm_); return report_; }
    /** ★★ V12 beta94 THE COMMUNITY MAP (vibe_community.h): the beacons NAMED by an ident read off the air and placed from the
     *  published list, heard since `sinceUtc` — [ident, hz, lat, lon, last UTC, name, cc, "vor"|"loc"] — and NOTHING about
     *  the receiver: no radial, no bearing, no distance (a radial and a station is a fix of the server). Comma-joined. */
    std::string communityRows(int64_t sinceUtc, size_t maxRows) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o;
        char b[300];
        size_t n = 0;
        for (const auto& kv : beacons_) {
            const Beacon& x = kv.second;
            if (!x.heard || x.lastUtc < sinceUtc || identOf_(x).empty()) continue;
            const vibevor::Station* st = stationOf_(x);
            if (!st) continue;
            std::snprintf(b, sizeof b, "%s[\"%s\",%lld,%.4f,%.4f,%lld,\"%s\",\"%s\",\"%s\"]", n ? "," : "", jsonEsc(st->r->ident).c_str(), (long long)x.hz,
                          st->r->lat, st->r->lon, (long long)x.lastUtc, jsonEsc(st->name()).c_str(), jsonEsc(st->r->cc).c_str(), x.expect == 2 ? "loc" : "vor");
            o += b;
            if (++n >= maxRows) break;
        }
        return o;
    }
    /** For the admin line / the bench. */
    int heardNow() const { return heard_.load(); }
    int identifiedNow() const { return identified_.load(); }
    /** Build the report now (tests; the decoder thread does it once a second of stream). */
    void buildReportNow() { buildReport_(); }

    // ── test seams ──
    void setClockForTest(std::function<int64_t()> c) { std::lock_guard<std::mutex> lk(m_); clock_ = std::move(c); }
    /** One beacon's record, for the tests: what was decoded on channel `hz`. */
    struct Seen { bool heard = false; int held = 0; double radial = NAN, sigma = NAN, ddm = NAN, expRadial = NAN, idRadial = NAN; int reads = 0;
                  std::string ident, reading, station, idBy; int64_t lastMs = -1; };
    Seen seenForTest(int64_t hz) const {
        std::lock_guard<std::mutex> lk(m_);
        Seen s;
        auto it = beacons_.find(hz);
        if (it == beacons_.end()) return s;
        const Beacon& b = it->second;
        s.heard = b.heard; s.held = b.held; s.radial = b.radial; s.sigma = b.sigma; s.ddm = b.ddm;
        s.ident = identOf_(b); s.reading = b.reading; s.lastMs = b.lastStreamMs;
        s.idBy = idBy_(b); s.reads = readsOf_(b, s.ident); s.expRadial = b.geoExp; s.idRadial = b.geoMeas;
        const vibevor::Station* st = stationOf_(b);
        if (st) s.station = st->r->ident;
        return s;
    }
    /** ★ Tests: a word of Morse read on channel `hz`, and a held VOR bearing — as a worker would report them. */
    void wordForTest(int64_t hz, const std::string& w) { Live L; L.hz = hz; L.rate = 1; onWord_(L, w, 10.0); }
    void bearingForTest(int64_t hz, double radial, double sigma = 0.5) {
        Live L; L.hz = hz; L.rate = 1;
        vibe::nav::Report r; r.kind = vibe::nav::Report::Vor; r.bearing = radial; r.sigma = sigma; r.seconds = 3; r.last.carrier = 1e-3;
        onSecond_(L, r);
    }
    /** Channel rate the current window runs at (0 before one). */
    double chanRate() const { std::lock_guard<std::mutex> wk(work_); return mc_ ? mc_->chanRate() : 0.0; }

private:
    struct Block { std::vector<cf32> v; int gen = 0, win = 0; int64_t pos = 0; };
    struct Beacon {
        int64_t hz = 0; int expect = 0, win = -1;
        std::vector<vibevor::Station> cands;
        vibe::ndb::IdentVote vote;
        std::map<std::string, int> seen;   ///< readings that ARE a station listed on this channel, counted
        std::string reading;
        double wpm = 0;
        bool heard = false;
        int held = 0, heldSecs = 0;        ///< 1 VOR bearing, 2 ILS deviation — the last one held
        double radial = NAN, sigma = NAN, ddm = NAN, sdm = NAN, levelDb = -200;
        int64_t firstUtc = 0, lastUtc = 0, lastStreamMs = -1;
        /** ★★ The geometry witness, once it has spoken (it stays: the station does not move): the candidate it named, the
         *  radial expected of it and the one measured then. geoIdx −1 = not (yet). */
        int geoIdx = -1;
        double geoExp = NAN, geoMeas = NAN;
    };
    /** One channel of the window being decoded (its worker's own). */
    struct Live {
        int64_t hz = 0;
        std::unique_ptr<vibe::nav::NavRx> rx;
        double rate = 0;
        int64_t t0Ms = 0, samples = 0;
        int64_t ms() const { return t0Ms + (int64_t)((double)samples * 1000.0 / rate); }
    };

    int64_t nowUtc() const {
        if (clock_) return clock_();
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    /** ★ The ident: the NDB skimmer's consensus (three clean identical readings …), OR a reading that IS a station
     *  published on this channel, read twice (the list is the second witness) — vibe_nav_run.h's rule — OR (beta94) read
     *  ONCE with the measured radial matching that station's expected one (the bearing is the second witness, geoCheck_). */
    static std::string identOf_(const Beacon& b) {
        if (!b.vote.ident().empty()) return b.vote.ident();
        for (const auto& kv : b.seen) if (kv.second >= 2) return kv.first;
        if (b.geoIdx >= 0) return b.cands[(size_t)b.geoIdx].r->ident;
        return std::string();
    }
    /** Why it was trusted — "votes" (three alike), "reads" (a listed ident read twice), "radial" (read once + the bearing),
     *  "" (not). Follows identOf_'s order. */
    static const char* idBy_(const Beacon& b) {
        if (!b.vote.ident().empty()) return "votes";
        for (const auto& kv : b.seen) if (kv.second >= 2) return "reads";
        if (b.geoIdx >= 0) return "radial";
        return "";
    }
    static int readsOf_(const Beacon& b, const std::string& id) {
        auto it = b.seen.find(id);
        return it == b.seen.end() ? 0 : it->second;
    }
    /** The published station the ident read names, or null. Never one the air has not named. The one the bearing
     *  confirmed when it did (same ident), else the nearest of that ident. */
    static const vibevor::Station* stationOf_(const Beacon& b) {
        const std::string id = identOf_(b);
        if (id.empty()) return nullptr;
        if (b.geoIdx >= 0 && id == b.cands[(size_t)b.geoIdx].r->ident) return &b.cands[(size_t)b.geoIdx];
        for (const auto& s : b.cands) if (id == s.r->ident) return &s;
        return nullptr;
    }
    /** ★★★ THE GEOMETRY WITNESS (beta94). A listed ident read at least once on this channel + a held VOR radial within
     *  geoTolDeg of the radial THAT station (kGeoMinKm–kGeoMaxKm away) should send here = that station. Each candidate is
     *  judged on its OWN expected radial, so a read of another same-channel station's ident does not borrow this one's
     *  bearing, and a misread with the wrong radial names nothing. Needs the receiver's position. (Under m_.) */
    void geoCheck_(Beacon& b) {
        if (!havePos_ || b.geoIdx >= 0 || b.held != 1 || b.expect != 1 || !std::isfinite(b.radial)) return;
        int best = -1; double bestErr = 1e9, bestExp = NAN;
        for (size_t i = 0; i < b.cands.size(); ++i) {
            const vibevor::Station& s = b.cands[i];
            if (readsOf_(b, s.r->ident) < 1) continue;
            double km = NAN;
            const double exp = vibevor::expectedRadial(*s.r, lat_, lon_, &km);
            if (!(km >= kGeoMinKm && km <= kGeoMaxKm)) continue;
            const double err = vibevor::angleDiff(b.radial, exp);
            if (err <= geoTolDeg(km) && err < bestErr) { best = (int)i; bestErr = err; bestExp = exp; }
        }
        if (best < 0) return;
        b.geoIdx = best; b.geoExp = bestExp; b.geoMeas = b.radial;
    }

    // ── the decoder thread ──
    void loop_() {
        while (run_.load()) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(qm_);
                qcv_.wait_for(lk, std::chrono::milliseconds(200), [this] { return !run_.load() || !q_.empty(); });
                if (!run_.load()) break;
                if (q_.empty()) continue;
                b = std::move(q_.front()); q_.pop_front();
                queued_ -= b.v.size();
                busy_ = true;
            }
            roomCv_.notify_all();
            process_(b);
            { std::lock_guard<std::mutex> lk(qm_); busy_ = false; }
        }
    }
    void process_(const Block& b) {
        std::lock_guard<std::mutex> wk(work_);
        const double rate = plan_.rate;
        if (b.gen != decGen_) {
            // ★ a new window: the old one's channels finish (their last blocks), then the new front end and receivers
            dropWindow_();
            buildWindow_(b.win, b.pos + (int64_t)(o_.settleSec * rate));
            decGen_ = b.gen; decWin_ = b.win;
            settleLeft_ = (int64_t)(o_.settleSec * rate);
        }
        const cf32* x = b.v.data();
        size_t n = b.v.size();
        if (settleLeft_ > 0) {
            const size_t k = (size_t)std::min<int64_t>(settleLeft_, (int64_t)n);
            settleLeft_ -= (int64_t)k; x += k; n -= k;
        }
        if (n && workers_) { workers_->feed(x, n); wDropped_.store(wDroppedBase_ + workers_->dropped()); }
        const int64_t ms = (int64_t)((double)(b.pos + (int64_t)b.v.size()) * 1000.0 / rate);
        if (ms - lastReportMs_ >= 1000) { lastReportMs_ = ms; streamMs_.store(ms); buildReport_(); }
    }
    void dropWindow_() {
        if (workers_) { if (o_.lossless) workers_->flush(); workers_->stop(); wDroppedBase_ += workers_->dropped(); wDropped_.store(wDroppedBase_); }   // ★ (lossless rings: 0)
        workers_.reset();
        lives_.clear();
        mc_.reset();
    }
    void buildWindow_(int w, int64_t startPos) {
        if (w < 0 || w >= (int)plan_.w.size()) return;
        const Window& W = plan_.w[(size_t)w];
        std::vector<double> ch;
        for (int i = W.first; i <= W.last; ++i) ch.push_back((double)chanHz(i));
        mc_.reset(new vibeavi::MultiChannel(plan_.rate, W.hwCentre, ch, kHalfWidthHz, kMinChanRate));
        const double cr = mc_->chanRate();
        const int64_t t0 = (int64_t)((double)startPos * 1000.0 / plan_.rate);
        lives_.clear();
        lives_.resize(ch.size());
        for (size_t c = 0; c < ch.size(); ++c) {
            Live& L = lives_[c];
            L.hz = (int64_t)ch[c]; L.rate = cr; L.t0Ms = t0; L.samples = 0;
            L.rx.reset(new vibe::nav::NavRx(cr, o_.forceScalar));
            Live* lp = &L;
            L.rx->onSecond = [this, lp](const vibe::nav::Report& r) { onSecond_(*lp, r); };
            L.rx->keyer.onWord = [this, lp](const std::string& word, double wpm) { onWord_(*lp, word, wpm); };
        }
        const int nW = o_.workers < 0 ? vibeavi::defaultWorkers() : o_.workers;
        /* ★★ THE RINGS WAIT, THE QUEUE DROPS. This is the decoder's own thread, not the DSP's, so it may wait for a worker
         *  (the rings' lossless mode): a worker's wake-up at decoder priority can lag by more than a few FFT blocks under
         *  load, and a ring that overwrote then cut every channel's stream (measured on the Mac server with the fake radio:
         *  ~450 blocks a second lost, no bearing anywhere). Back-pressure reaches the quarter-second queue in feed(), which
         *  drops the OLDEST and counts it — the one place a sample is ever let go. ★ Memory: ≤ 2 MB of bins per worker. */
        const int slots = std::max(8, std::min(64, (int)(2.0e6 / ((double)mc_->fftSize() * sizeof(cf32)))));
        workers_.reset(new vibeavi::Workers(*mc_, nW, [this](int c, const cf32* z, int k) {
            Live& L = lives_[(size_t)c];
            L.rx->feed(z, k);
            L.samples += k;
        }, /*lossless=*/true, slots));
    }
    /** A worker's channel finished a second: a believed bearing / deviation is recorded. */
    void onSecond_(Live& L, const vibe::nav::Report& r) {
        if (r.kind == vibe::nav::Report::None) return;
        std::lock_guard<std::mutex> lk(m_);
        auto it = beacons_.find(L.hz);
        if (it == beacons_.end()) return;
        Beacon& b = it->second;
        const int64_t u = nowUtc();
        if (!b.heard) b.firstUtc = u;
        b.heard = true; b.lastUtc = u; b.lastStreamMs = L.ms();
        b.levelDb = r.last.carrier > 0 ? 20.0 * std::log10(r.last.carrier) : -200;
        b.heldSecs = r.seconds;
        if (r.kind == vibe::nav::Report::Vor) { b.held = 1; b.radial = r.bearing; b.sigma = r.sigma; geoCheck_(b); }
        else { b.held = 2; b.ddm = r.ddm; b.sdm = r.sdm; }
    }
    /** A word of Morse off a channel: a ballot for its ident. */
    void onWord_(Live& L, const std::string& word, double wpm) {
        const std::string w = cleanIdent(word);
        if (w.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        auto it = beacons_.find(L.hz);
        if (it == beacons_.end()) return;
        Beacon& b = it->second;
        b.reading = w; b.wpm = wpm;
        bool listed = false;
        for (const auto& s : b.cands) if (w == s.r->ident) listed = true;
        b.vote.add(w, L.ms());
        if (listed) { ++b.seen[w]; geoCheck_(b); }
        if (!identOf_(b).empty()) {
            const int64_t u = nowUtc();
            if (!b.heard) b.firstUtc = u;
            b.heard = true; b.lastUtc = u; b.lastStreamMs = L.ms();
        }
    }

    void buildReport_() {
        const int64_t nowMs = streamMs_.load();
        const int w = win_.load(), nWin = (int)plan_.w.size();
        const double perWin = o_.settleSec + o_.dwellSec;
        // ★★ FRESH = heard within the last sweep: one full pass of the band (and one more dwell, for a beacon heard early
        //    in one visit and late in the next). One window: two dwells.
        const double freshMs = 1000.0 * (nWin > 1 ? nWin * perWin + o_.dwellSec : 2 * o_.dwellSec);
        const double inWin = plan_.rate > 0 ? (double)fedInWinSnapshot_() / plan_.rate : 0.0;
        std::string o;
        char b[400];
        const Window* W = w >= 0 && w < nWin ? &plan_.w[(size_t)w] : nullptr;
        std::snprintf(b, sizeof b, "{\"type\":\"vormap\",\"now\":%lld,\"rate\":%.0f,\"window\":%d,\"windows\":%d,\"wlo\":%.0f,\"whi\":%.0f,\"centre\":%.0f,"
                      "\"sweep\":%d,\"inWin\":%.1f,\"dwell\":%.0f,\"progress\":%.3f,\"pos\":%s,\"wins\":[",
                      (long long)0, plan_.rate, w, nWin, W ? W->lo() : 0.0, W ? W->hi() : 0.0, W ? W->hwCentre : 0.0, sweep_.load(),
                      std::min(inWin, perWin), o_.dwellSec,
                      nWin > 0 ? std::min(1.0, (w + std::min(1.0, inWin / perWin)) / nWin) : 0.0, havePos_ ? "true" : "false");
        o = b;
        for (int i = 0; i < nWin; ++i) { std::snprintf(b, sizeof b, "%s[%.0f,%.0f]", i ? "," : "", plan_.w[(size_t)i].lo(), plan_.w[(size_t)i].hi()); o += b; }
        o += "],\"beacons\":[";
        int heard = 0, ident = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            const int64_t u = nowUtc();
            o.replace(o.find("\"now\":0"), 7, "\"now\":" + std::to_string((long long)u));
            bool first = true;
            for (const auto& kv : beacons_) {
                const Beacon& x = kv.second;
                if (!x.heard) continue;
                ++heard;
                const std::string id = identOf_(x);
                if (!id.empty()) ++ident;
                const vibevor::Station* st = stationOf_(x);
                const bool fresh = x.lastStreamMs >= 0 && (double)(nowMs - x.lastStreamMs) <= freshMs;
                std::snprintf(b, sizeof b, "%s{\"hz\":%lld,\"kind\":\"%s\",\"win\":%d,\"ident\":\"%s\",\"reading\":\"%s\",\"votes\":%d,\"held\":%d,\"level\":%.1f,"
                              "\"first\":%lld,\"last\":%lld,\"fresh\":%s",
                              first ? "" : ",", (long long)x.hz, x.expect == 2 ? "loc" : "vor", x.win, id.c_str(), cleanIdent(x.reading).c_str(),
                              x.vote.votes(), x.held, x.levelDb, (long long)x.firstUtc, (long long)x.lastUtc, fresh ? "true" : "false");
                o += b; first = false;
                if (!id.empty()) {
                    // ★★ WHY it was trusted (the clients' card says it in words): by what, how many reads, and for the
                    //    bearing witness the radial expected of the station and the one measured when it spoke.
                    std::snprintf(b, sizeof b, ",\"idBy\":\"%s\",\"reads\":%d", idBy_(x), readsOf_(x, id)); o += b;
                    if (x.geoIdx >= 0 && id == x.cands[(size_t)x.geoIdx].r->ident) {
                        std::snprintf(b, sizeof b, ",\"expRadial\":%.1f,\"idRadial\":%.1f", x.geoExp, x.geoMeas); o += b;
                    }
                }
                if (x.wpm > 0) { std::snprintf(b, sizeof b, ",\"wpm\":%.1f", x.wpm); o += b; }
                if (std::isfinite(x.radial)) {
                    std::snprintf(b, sizeof b, ",\"radial\":%.1f,\"sigma\":%.2f", x.radial, x.sigma); o += b;
                    if (st) { std::snprintf(b, sizeof b, ",\"trueBrg\":%.1f", std::fmod(x.radial + st->r->var + 720.0, 360.0)); o += b; }
                }
                if (std::isfinite(x.ddm)) { std::snprintf(b, sizeof b, ",\"ddm\":%.4f,\"sdm\":%.3f", x.ddm, x.sdm); o += b; }
                if (st) {
                    std::snprintf(b, sizeof b, ",\"station\":{\"ident\":\"%s\",\"name\":\"%s\",\"cc\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"var\":%.1f,\"dme\":\"%s\"",
                                  jsonEsc(st->r->ident).c_str(), jsonEsc(st->name()).c_str(), jsonEsc(st->r->cc).c_str(), st->r->lat, st->r->lon, st->r->var,
                                  jsonEsc(st->r->dme).c_str());
                    o += b;
                    // ★ the distance to 10 km, as vibe_nav_run.h tells listeners (a radial and an exact distance is a fix of the server)
                    if (std::isfinite(st->km)) { std::snprintf(b, sizeof b, ",\"km10\":%.0f,\"brg\":%.0f", std::round(st->km / 10) * 10, st->brg); o += b; }
                    o += "}";
                }
                o += "}";
            }
        }
        std::snprintf(b, sizeof b, "],\"heard\":%d,\"identified\":%d,\"dropped\":%llu,\"credit\":\"%s\"}", heard, ident,
                      (unsigned long long)droppedBlocks(), vibevor::kStationCredit);
        o += b;
        heard_.store(heard); identified_.store(ident);
        std::lock_guard<std::mutex> lk(rm_);
        report_ = std::move(o);
    }
    int64_t fedInWinSnapshot_() const { return fedInWin_; }   // ★ a benign race: a progress figure, read once a second

    Plan plan_;
    Opts o_;
    bool havePos_ = false;
    double lat_ = 0, lon_ = 0;
    int64_t started_ = 0;
    std::function<int64_t()> clock_;

    // the DSP thread's sweep clock
    std::atomic<int> win_{0}, gen_{0}, sweep_{0};
    int64_t fedInWin_ = 0, streamPos_ = 0;
    bool hopAsked_ = false;
    int want_ = 0;

    // the queue
    mutable std::mutex qm_;
    std::condition_variable qcv_, roomCv_;
    std::deque<Block> q_;
    size_t queued_ = 0;
    bool busy_ = false;
    std::atomic<uint64_t> dropped_{0}, wDropped_{0};
    uint64_t wDroppedBase_ = 0;
    std::atomic<bool> run_{false};
    std::thread th_;

    // the decoder's window
    mutable std::mutex work_;
    int decGen_ = -1, decWin_ = -1;
    int64_t settleLeft_ = 0, lastReportMs_ = -100000;
    std::atomic<int64_t> streamMs_{0};
    std::unique_ptr<vibeavi::MultiChannel> mc_;
    std::unique_ptr<vibeavi::Workers> workers_;
    std::vector<Live> lives_;

    // the beacons and the report
    mutable std::mutex m_, rm_;
    std::map<int64_t, Beacon> beacons_;
    std::string report_;
    std::atomic<int> heard_{0}, identified_{0};
};

} // namespace vibevormap
