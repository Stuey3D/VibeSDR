// vibe_adsb_service.h — ADS-B mode's decoder service: vibe_adsb.h on its own thread, fed from the DSP loop,
// with the aircraft report clients draw and the measurements ADS-B's gain rule steers by.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS. Thread priority on this project is Network > Audio
//     > Spectrum > Decoders (memory: feedback_thread_priority_order). feed() copies a block into a bounded
//     queue and returns; a full queue loses the OLDEST block and counts it. A Pi that cannot keep up loses
//     some replies — never the waterfall, never the socket.
// ★★ The decoder works at whatever rate the radio is ALREADY running (≥ 2.4 MS/s): ADS-B mode never changes
//    the sample rate (Stuart, 2026-10-24 — the 32-bit re-rate teardown, memory dab_crash_was_the_dongle_not_
//    the_code). See vsAdsbCapableHw in local_sdr_shim.cpp.
#pragma once
#include "vibe_adsb.h"
#include "vibe_adsb_capture.h"   // ★ CaptureMeter — the live "capture N %"
#include "vibe_adsb_data.h"     // what an aircraft IS (registration, type, operator, route) — V12 aircraft data
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibeadsb {

/** One second's measurements, for the gain rule and the status line. */
struct Health {
    double   noiseDbfs = -99;     ///< the demodulator's noise floor (mean power)
    double   msgsPerSec = 0;      ///< believed messages
    double   strongFrac = 0;      ///< share of believed messages within 3 dB of full scale
    uint64_t dropped = 0;         ///< capture blocks lost because the decoder was behind (since start)
    uint64_t msgsTotal = 0;       ///< believed messages since start, live — GainTrial counts its own windows from it
    int      aircraft = 0;        ///< aircraft heard in the last minute (the tracker's table)
    double   farNm = -1;          ///< the furthest of them with a position, NM; -1 = the server has no position of its own
    bool     valid = false;
};

class Service {
public:
    static constexpr double kMinRateHz = 2.4e6 - 1000;   ///< below this ADS-B is not offered — see test-adsb

    ~Service() { stop(); }

    /** Start decoding a capture at `rateHz`. lat/lon NAN when the server has no position. */
    void start(double rateHz, double lat, double lon) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        rate_ = rateHz;
        demod_.reset(new Demod(rateHz));
        tracker_ = Tracker();
        tracker_.setMaxRangeNm(360);                      // as dump1090-fa: a position further than this is a bad decode
        if (lat == lat && lon == lon) tracker_.setReceiver(lat, lon);
        rxLat_ = lat; rxLon_ = lon;
        queue_.clear();
        queued_ = 0;
        dropped_ = 0;
        t0_ = std::chrono::steady_clock::now();
        cap_.reset(rateHz, 0.0);
        winMsgs_ = winStrong_ = 0; lastGood_ = 0; winStart_ = 0; msgsTotal_ = 0;
        health_ = Health{};
        run_ = true;
        worker_ = std::thread([this] { loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_) return;
            run_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> il(infoMtx_);
        work_.clear(); infoJson_.clear(); infoDelta_.clear();
    }

    bool running() const { return run_.load(std::memory_order_relaxed); }
    double rate() const { return rate_; }

    /** From the DSP thread: copy and return. Never blocks on the decoder. */
    void feed(const std::complex<float>* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || !n) return;
        std::vector<std::complex<float>> b(iq, iq + n);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // ★★ BOUNDED IN TIME, NOT IN BLOCKS. A cap of 16 blocks was 8 ms of slack on an RSP, which hands over
            //    ~2 000 small blocks a second at 3 MS/s — the Lenovo's first live run dropped 64 at once. Half a second
            //    of capture whatever the block size; past that the OLDEST goes.
            cap_.add(n, now());                             // ★ counted on ARRIVAL, before any drop — see CaptureMeter
            queued_ += n;
            queue_.push_back({std::move(b), now()});
            while (queued_ > size_t(rate_ * kMaxQueuedSec) && queue_.size() > 1) {
                queued_ -= queue_.front().iq.size(); queue_.pop_front(); ++dropped_;
            }
        }
        cv_.notify_one();
    }

    /** The aircraft report: {"type":"adsb", …Tracker::json…, "rate":…, "noise":…, "dropped":…, "capture":…}.
     *  ★ capture: % of the promised samples this service RECEIVED over the last ~10 s, -1 until it knows (CaptureMeter). */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        const double tn = now();
        std::string t = tracker_.json(tn);
        char tail[192];
        std::snprintf(tail, sizeof tail, ",\"rate\":%.0f,\"noise\":%.1f,\"msgRate\":%.0f,\"dropped\":%llu,\"capture\":%d}",
                      rate_, health_.noiseDbfs, health_.msgsPerSec, (unsigned long long)dropped_, cap_.percent(tn));
        // Tracker::json is {"now":…,"messages":…,"aircraft":[…]} — open it up and add the type and the tail
        return "{\"type\":\"adsb\"," + t.substr(1, t.size() - 2) + tail;
    }

    /** ★★ WHAT EACH AIRCRAFT IS, SENT ONCE — not in the 1 fps report. `{"type":"adsb_info","aircraft":{"<hex>":{r, t,
     *  desc, wtc, eng, species, country, countryName, mil, op, opName, logo, sil, route{…}}}}` (ADSB.md), for the
     *  aircraft whose details are new or CHANGED since the last call (a callsign arrived, the route was judged).
     *  "" when nothing changed. The clients keep the fields per hex until the aircraft leaves the report.
     *  `{}` for a hex = it no longer has any. */
    std::string takeInfoDelta() {
        std::lock_guard<std::mutex> il(infoMtx_);
        if (infoDelta_.empty()) return {};
        std::string o = "{\"type\":\"adsb_info\",\"aircraft\":{";
        bool first = true;
        for (const auto& kv : infoDelta_) {
            auto it = infoJson_.find(kv);
            if (!first) o += ',';
            first = false;
            o += "\"" + kv + "\":" + (it == infoJson_.end() ? std::string("{}") : it->second);
        }
        infoDelta_.clear();
        return o + "}}";
    }
    /** Everything known, for a socket that has just joined (sent before its first report). "" if nothing. */
    std::string infoAll() {
        std::lock_guard<std::mutex> il(infoMtx_);
        if (infoJson_.empty()) return {};
        std::string o = "{\"type\":\"adsb_info\",\"full\":1,\"aircraft\":{";
        bool first = true;
        for (const auto& kv : infoJson_) { if (!first) o += ','; first = false; o += "\"" + kv.first + "\":" + kv.second; }
        return o + "}}";
    }

    /** One aircraft's trail, for GET /adsb/trail/<hex> — see vibe_adsb.h kTrailStepNm. */
    std::string trail(const std::string& hex) {
        std::lock_guard<std::mutex> lk(mtx_);
        return tracker_.trailJson(hex);
    }
    Health health() {
        std::lock_guard<std::mutex> lk(mtx_);
        Health h = health_;
        h.dropped = dropped_;
        h.msgsTotal = msgsTotal_;
        return h;
    }

    size_t aircraftCount() {
        std::lock_guard<std::mutex> lk(mtx_);
        return tracker_.aircraft().size();
    }

private:
    static constexpr double kMaxQueuedSec = 0.5;           // capture the decoder may fall behind by before dropping

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    void loop() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<std::complex<float>> b = std::move(queue_.front().iq);
                const double t = queue_.front().arrived;
                queue_.pop_front();
                queued_ -= b.size();
                Demod* d = demod_.get();
                lk.unlock();
                // ★ The tracker is touched only on this thread and under mtx_ for report(); the demodulator
                //   only here. A frame is judged as soon as it is cut, so `known` sees the latest aircraft.
                // ★★★ STAMPED WITH THE BLOCK'S ARRIVAL, NOT WITH NOW. A busy server works through up to half a second
                //     of queued blocks in a burst; stamped as processed, positions really 0.5 s apart came out
                //     milliseconds apart, an airliner "flew" thousands of knots, and setPos wiped its trail — the
                //     longest-tracked aircraft showed no track at all (Stuart, 2026-10-25).
                d->feed(b.data(), b.size(),
                        [&](const Frame& f) {
                            std::lock_guard<std::mutex> g(mtx_);
                            if (tracker_.accept(f, t)) { ++winMsgs_; ++msgsTotal_; if (f.level > -3.0f) ++winStrong_; }
                        },
                        [&](uint32_t a) { std::lock_guard<std::mutex> g(mtx_); return tracker_.known(a, t); });
                lk.lock();
            }
            const double t = now();
            if (t - winStart_ >= 1.0) {
                const double span = t - winStart_;
                health_.msgsPerSec = winMsgs_ / span;
                health_.strongFrac = winMsgs_ ? double(winStrong_) / winMsgs_ : 0;
                health_.noiseDbfs = demod_ ? demod_->noiseDbfs() : -99;
                health_.valid = true;
                winMsgs_ = winStrong_ = 0;
                winStart_ = t;
                tracker_.expire(t, 60);
                // ★ how far it hears — the gain rule's "hearing little" judgement (GainControl::warning)
                health_.aircraft = int(tracker_.aircraft().size());
                health_.farNm = -1;
                if (rxLat_ == rxLat_ && rxLon_ == rxLon_) {
                    health_.farNm = 0;
                    for (const auto& kv : tracker_.aircraft())
                        if (kv.second.hasPos && t - kv.second.seenPos <= 60)
                            health_.farNm = std::max(health_.farNm, distanceNm(rxLat_, rxLon_, kv.second.lat, kv.second.lon));
                }
                // ★★ ENRICH WITH THE TRACKER LOCK RELEASED: a first lookup may fault a page in from the SD card,
                //    and report() — on the DSP thread — must never wait for that.
                std::vector<Seen> snap;
                snap.reserve(tracker_.aircraft().size());
                for (const auto& kv : tracker_.aircraft()) {
                    const Aircraft& a = kv.second;
                    snap.push_back({a.icao, a.nonIcao, a.callsign, a.hasPos && t - a.seenPos <= 60, a.lat, a.lon});
                }
                lk.unlock();
                enrich(snap, t);
                lk.lock();
            }
        }
    }

public:
    // ★ public for test-adsb-data, which drives enrichment without demodulating IQ
    struct Seen { uint32_t icao; bool nonIcao; std::string callsign; bool hasPos; double lat, lon; };
    struct Work { vibeadsbdata::Info info; std::string callsign; double judged = -1e9; bool hexDone = false; std::string sent; };

    /** Once per aircraft (and again when its callsign changes; its route re-judged every 15 s as it moves). On the
     *  decoder thread, never the DSP thread. Publishes only what CHANGED. */
    void enrich(const std::vector<Seen>& snap, double t) {
        vibeadsbdata::Db& db = vibeadsbdata::Db::get();
        std::unordered_map<uint32_t, bool> live;
        std::vector<std::pair<std::string, std::string>> changed;    // hex → json ("" = gone)
        for (const Seen& s : snap) {
            if (s.nonIcao) continue;                                  // ★ an anonymous address is no aircraft's identity
            live[s.icao] = true;
            Work& w = work_[s.icao];
            bool dirty = false;
            if (!w.hexDone) { db.lookupHex(s.icao, w.info); w.hexDone = true; dirty = true; }
            if (s.callsign != w.callsign) {
                w.callsign = s.callsign;
                db.lookupCallsign(s.callsign, w.info.r, s.icao, w.info);
                w.judged = -1e9;
                dirty = true;
            }
            if (s.hasPos && w.info.route.size() >= 2 && t - w.judged >= 15) {
                vibeadsbdata::Db::judge(w.info, s.lat, s.lon);
                w.judged = t;
                dirty = true;
            }
            if (!dirty) continue;
            char hx[8]; std::snprintf(hx, sizeof hx, "%06x", s.icao);
            char hu[8]; std::snprintf(hu, sizeof hu, "%06X", s.icao);
            vibeadsbdata::Db::resolveImages(hu, w.info);
            const std::string j = w.info.json();
            if (j != w.sent) { w.sent = j; changed.push_back({hx, j == "{}" ? std::string() : j}); }
        }
        std::vector<std::string> gone;
        for (auto it = work_.begin(); it != work_.end();) {
            if (live.count(it->first)) { ++it; continue; }
            char hx[8]; std::snprintf(hx, sizeof hx, "%06x", it->first);
            gone.push_back(hx);
            it = work_.erase(it);
        }
        std::lock_guard<std::mutex> il(infoMtx_);
        for (const auto& g : gone) { infoJson_.erase(g); infoDelta_.erase(g); }
        for (const auto& c : changed) {
            if (c.second.empty()) { if (infoJson_.erase(c.first)) infoDelta_.insert(c.first); }
            else { infoJson_[c.first] = c.second; infoDelta_.insert(c.first); }
        }
    }
private:

    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    double rate_ = 0;
    double rxLat_ = NAN, rxLon_ = NAN;                      // the server's own position, NAN when it has none
    std::unique_ptr<Demod> demod_;
    Tracker tracker_;
    struct Block { std::vector<std::complex<float>> iq; double arrived; };   // arrived = now() in feed()
    std::deque<Block> queue_;
    size_t queued_ = 0;                                     // samples in queue_
    uint64_t dropped_ = 0;
    CaptureMeter cap_;                                      // under mtx_
    std::chrono::steady_clock::time_point t0_;
    uint64_t winMsgs_ = 0, winStrong_ = 0, lastGood_ = 0, msgsTotal_ = 0;
    double winStart_ = 0;
    Health health_;
    // the aircraft details (decoder thread: work_; any thread under infoMtx_: the published JSON)
    std::unordered_map<uint32_t, Work> work_;
    std::mutex infoMtx_;
    std::map<std::string, std::string> infoJson_;
    std::set<std::string> infoDelta_;
};

/**
 * ADS-B's own gain rule — on our measurements, after the published behaviour of dump1090-fa's adaptive gain.
 *
 * ★★★ NOT VibeAGC. VibeAGC sets gain from the capture's average level; ADS-B is mostly noise with short strong
 *     bursts, so an average-level rule either leaves the noise floor far below the ADC's range (weak aircraft
 *     lost) or lets nearby aircraft clip. ADS-B wants the noise floor high enough to use the ADC's range and
 *     no higher.
 * ★ Works on the radio's OWN gain list (LocalSdrShim::getTunerGains: the RTL's table, SDRplay 0–49 dB, the
 *   Airspy/HackRF lists), so it is one rule for every radio rather than a control that works on one.
 */
/* ★★★ THE NOISE TARGET — WHY MESSAGES ALONE PINNED STUART'S FLIGHTAWARE PI AT 49.6 dB (2026-10-26).
 *  From 10-24 the rule was messages only: ramp from the middle, a higher rung kept unless CLEARLY worse ("a plateau
 *  climbs on"), stopped by a cliff. On the Pi 500's V4L (a wideband LaNA, no 1090 filter) the cliff is real — out-of-
 *  band energy overloads the tuner at 43.4 → 44.5 dB (196 → 52 msg/s) — and it settles at 40.2. On the FlightAware
 *  Pi (an LNA AND a 1090 MHz filter) nothing out of band reaches the tuner, so there IS no cliff: the message count
 *  keeps creeping up to the top (near aircraft's marginal replies decode a little more often), the climb never meets
 *  a reason to stop, and it pins the maximum. The 12.5-hour A/B against dump1090-fa on that Pi: at 49.6 we decoded
 *  5.8 % MORE messages and 2.3 % FEWER positioned aircraft; our best runs were the ones the climb happened to leave
 *  at 37–43 dB. MESSAGES ARE NOT RANGE — they are dominated by the near aircraft, and gain past the point where the
 *  noise floor fills the ADC buys near-aircraft messages at the price of clipping (57 % of replies within 3 dB of
 *  full scale at 49.6 on the 10-24 clip, 103 000 clipped samples a second) and of the distant ones.
 *
 *  So the dynamic range is the ceiling, as dump1090-fa's --adaptive-range does (its README: hold the noise at or
 *  below a target dynamic range under full scale, 30 dB on an RTL): the noise floor must stay at least 30 dB below
 *  full scale, and messages choose only UNDER that ceiling (they still find the V4L's cliff, an overloaded LaNA, the
 *  Pi 2's bare antenna that wants the top).
 *
 *  ★★ HOW −30 dBFS ON OUR SCALE WAS MAPPED. dump1090-fa judges the 40th percentile of |IQ| per second; graphs1090's
 *     "noise" is a different number again — the mean power of everything that was not a decoded reply (fruit,
 *     Mode A/C, undecoded replies included). Ours is Demod::noiseDbfs: the quietest 256-sample block of the last 16,
 *     ×1.1 — the thermal floor between replies. All three were measured on the same FlightAware clip (2.4 MS/s,
 *     49.6 dB, 1 340 msg/s) with the gain lowered in steps (scale, re-quantise to 8 bits): ours sits 0.3–0.7 dB
 *     above the 40th-percentile power and 11.3 dB BELOW graphs1090's noise. dump1090-fa's 30 dB of range on its
 *     percentile is therefore ≈ −30 dBFS on ours, and its graphs1090 "−20 dBFS" is ≈ −31 on ours. Checked against
 *     2.5 years of that Pi's graphs1090: at the gain dump1090-fa chose, our floor works out at −30.0 … −31.6 dBFS in
 *     every healthy month while its gain wandered 33 → 39 dB — the same quantity, held at the same place.
 *  ★ Our floor is a MINIMUM, so the fruit of a busy sky does not push it up and the target does not move with the
 *    traffic (graphs1090's noise does, by the 11 dB above).
 *  ★ The level-holding radio (an SDRplay, its IF AGC on) is exempt: its noise floor is the AGC's, not ours. */
struct GainTarget {
    static constexpr double kFloorDbfs = -30.0;   ///< the noise floor's ceiling (Demod::noiseDbfs scale)
    static constexpr double kDownHystDb = 1.0;    ///< over the target by more than this, held kChangeDelayS → one rung down
    static constexpr double kChangeDelayS = 10.0; ///< dump1090-fa's range change delay: no second change within this
    static constexpr double kSettleS = 3.0;       ///< after a gain write, before the floor counts again
};

/* ★★★ A STEP MUST EARN ITS PLACE IN MESSAGES (Stuart, 2026-10-24, the Pi 500 V4L on beta11: "when I connect I see
 *  around 250+ messages and after a minute or so it drops to 170 or so"). GainRule judges the noise floor, and on a
 *  hot front end (that V4L: −16.9 dBFS at the top of its list) the floor says "come down" while the sky says the
 *  opposite — the rule took it 49.6 → 44.5 dB and lost a third of the messages. dump1090-fa's 30 dB of range suits
 *  Stuart's FlightAware antenna; it is not a law of every feed. So the rule only PROPOSES a step; the step is then
 *  tried against where we are, A/B — the other gain, then this one, three times each — and kept only if it decoded
 *  MORE. Alternating legs, not before/after, so a sky that fills or empties over the minute lands on both sides.
 *  ★ A tie keeps the HIGHER gain: the top of the list is where we start (dump1090-fa's --gain 60) and the distant
 *    aircraft are the ones a lower gain loses.
 *  ★ The first ~2 s of each leg are not counted: the tuner and the decoder's noise floor (16 blocks) settle. */
class GainTrial {
public:
    static constexpr double kSettleS = 1.5;   ///< after each gain write, not counted
    static constexpr double kCountS = 6.0;    ///< counted per leg
    static constexpr int kLegs = 6;           ///< alternating, the proposed gain first: 3 legs each

    bool active() const { return legs_ > 0; }
    /** Start trying `proposed` against `current` (indices into the radio's gain list). Returns the index to set now.
     *  `breaksTie`: the noise rule's proposal (true) is taken unless it CLEARLY loses — messages first, the noise rule
     *  breaks ties (an empty sky, or an overloaded top where both sides decode nothing). An exploring step (false)
     *  must clearly WIN. "Clearly" = by 3 % and by two standard deviations of the counts (they are Poisson). */
    int start(int current, int proposed, double t, bool breaksTie = true) {
        idx_[0] = current; idx_[1] = proposed; breaksTie_ = breaksTie;
        msgs_[0] = msgs_[1] = 0; secs_[0] = secs_[1] = 0;
        legs_ = kLegs; side_ = 1; legAt_ = t; countFrom_ = -1;
        return idx_[1];
    }
    /** Feed at any cadence (the shim's 2 s tick). Returns the index the radio should be at; once the trial is over
     *  active() is false and the return value is the winner. */
    int step(double t, uint64_t msgsTotal) {
        if (!active()) return winner_;
        if (countFrom_ < 0) {
            if (t - legAt_ >= kSettleS) { countFrom_ = t; countMsgs_ = msgsTotal; }
            return idx_[side_];
        }
        if (t - countFrom_ < kCountS) return idx_[side_];
        msgs_[side_] += double(msgsTotal - countMsgs_);
        secs_[side_] += t - countFrom_;
        if (--legs_ == 0) {
            const double r0 = rate(0), r1 = rate(1);
            const double margin = std::max(0.03 * std::max(r0, r1),
                                           2.0 * std::sqrt(r0 / std::max(1e-9, secs_[0]) + r1 / std::max(1e-9, secs_[1])));
            const bool keep = breaksTie_ ? !(r0 - r1 > margin) : (r1 - r0 > margin);
            winner_ = keep ? idx_[1] : idx_[0];
            proposedWon_ = keep;
            return winner_;
        }
        side_ ^= 1; legAt_ = t; countFrom_ = -1;
        return idx_[side_];
    }
    double rate(int side) const { return secs_[side] > 0 ? msgs_[side] / secs_[side] : 0; }   ///< 0 current, 1 proposed
    bool proposedWon() const { return proposedWon_; }
    int current() const { return idx_[0]; }
    int proposed() const { return idx_[1]; }
    void cancel() { legs_ = 0; }
private:
    int idx_[2] = {0, 0};
    double msgs_[2] = {0, 0}, secs_[2] = {0, 0};
    int legs_ = 0, side_ = 0, winner_ = 0;
    double legAt_ = 0, countFrom_ = -1;
    uint64_t countMsgs_ = 0;
    bool proposedWon_ = false, breaksTie_ = true;
};

/* ★★★ THE SEARCH AT ENTRY — THE GAIN IS FOUND BY MESSAGES (Stuart, 2026-10-24, the Pi 500 V4L: a YouLoop into a Nooelec
 *  LaNA, "2 aircraft on the loop vs 111 on flight aware, this loop on OWRX always surprised me with how good it was").
 *  Its log: 49.6 and 48.0 dB decoded 0–1 msg/s at −18 dBFS (the LNA in front overloads the dongle at the top), 44.5 dB
 *  14–40. No noise-floor rule knows where a given feed peaks — dump1090-fa's suits a bare antenna. So at entry we LOOK:
 *  every other rung from the top down, ~8 s each, counting believed messages, and land on the best. It stops once past
 *  the peak (two rungs in a row each more than 5 % under the best), so a feed that peaks at the top costs ~24 s.
 *  ★ An empty sky (the best under 2 msg/s) proves nothing: the rule's own choice then stands (`found()` false). */
class GainSearch {
public:
    static constexpr double kSettleS = 1.5, kCountS = 5.0;
    static constexpr double kQuietMsgs = 2.0;   ///< msg/s: below this the sky is too empty to judge by

    bool active() const { return at_ < order_.size(); }
    /** `order`: indices into the gain list, tried in this order. `climbing`: the order RISES (the shim's ramp from the
     *  middle up — Stuart, 2026-10-24: "start in the middle and ramp up") — then a later, higher gain takes the lead
     *  unless it is clearly WORSE (a plateau climbs on: the distant aircraft are the ones gain buys); falling, a later,
     *  lower gain must clearly BEAT the best (a plateau stays high). */
    /** `ceilingDbfs` (climbing only; NAN = none): a rung whose noise floor, averaged over its counted seconds, is
     *  above this ends the climb there, and the result is chosen from the rungs below it — the dynamic-range
     *  ceiling (GainTarget). The messages decide only under it. */
    int start(const std::vector<int>& order, double t, bool climbing = false, double ceilingDbfs = NAN) {
        order_ = order; rates_.assign(order.size(), -1); floors_.assign(order.size(), NAN); at_ = 0; legAt_ = t;
        countFrom_ = -1; best_ = -1; below_ = 0;
        climbing_ = climbing; cliffAfterBest_ = false; ceiling_ = ceilingDbfs; ceilingAt_ = -1; cliff_ = false;
        fSum_ = 0; fN_ = 0;
        return order_.empty() ? -1 : order_[0];
    }
    /** `noiseDbfs`: Health::noiseDbfs now (NAN or ≤ −98 = not known); averaged over each rung's counted seconds. */
    int step(double t, uint64_t msgsTotal, double noiseDbfs = NAN) {
        if (!active()) return result();
        if (countFrom_ < 0) {
            if (t - legAt_ >= kSettleS) { countFrom_ = t; countMsgs_ = msgsTotal; fSum_ = 0; fN_ = 0; }
            return order_[at_];
        }
        if (noiseDbfs == noiseDbfs && noiseDbfs > -98) { fSum_ += std::pow(10.0, noiseDbfs / 10.0); ++fN_; }
        if (t - countFrom_ < kCountS) return order_[at_];
        rates_[at_] = double(msgsTotal - countMsgs_) / (t - countFrom_);
        if (fN_) floors_[at_] = 10.0 * std::log10(fSum_ / fN_);
        // ★★ OVER THE CEILING: this rung is out — the choice is made among the rungs below it (at least the one before)
        if (climbing_ && ceiling_ == ceiling_ && floors_[at_] == floors_[at_] && floors_[at_] > ceiling_) {
            ceilingAt_ = int(at_);
            at_ = order_.size();
            return result();
        }
        // ★ a LOWER gain must clearly beat the best (5 %, past the counts' noise): a flat curve stays at the top — the
        //   recording read 1351 at 49.6 and 1390 at 25.4 dB, and "25.4" would have been chosen for nothing
        if (best_ < 0 || rates_[at_] > (climbing_ ? 0.97 : 1.05) * rates_[size_t(best_)]) best_ = int(at_);
        // ★ past the peak = two rungs in a row each more than 5 % under the best (one could be a quiet moment)
        below_ = int(at_) > best_ && rates_[at_] < 0.95 * rates_[size_t(best_)] ? below_ + 1 : 0;
        /* ★★ A CLIFF STOPS IT AT ONCE (Stuart's V4L, beta13: 43.4 dB = 196 msg/s, 44.5 = 52, and the ramp went on to try
         *  49.6 = 8 — eight seconds of nearly nothing). A rung under HALF the best is the front end overloading, and
         *  every rung past it is worse: no second rung is needed to prove it. */
        const bool cliff = int(at_) > best_ && rates_[at_] < 0.5 * rates_[size_t(best_)] && rates_[size_t(best_)] >= kQuietMsgs;
        if (cliff && int(at_) == best_ + 1) cliffAfterBest_ = true;
        if (cliff) cliff_ = true;
        const bool pastPeak = cliff || (below_ >= 2 && rates_[size_t(best_)] >= kQuietMsgs);
        ++at_;
        if (pastPeak) at_ = order_.size();
        if (!active()) return result();
        legAt_ = t; countFrom_ = -1;
        return order_[at_];
    }
    /** True once over, with a sky busy enough to have judged by. */
    bool found() const { return !active() && best_ >= 0 && rates_[size_t(best_)] >= kQuietMsgs; }
    /** Where the ceiling stopped the climb (an index into order()), -1 = it did not. */
    int ceilingAt() const { return ceilingAt_; }
    /** The highest rung tried whose floor was under the ceiling — the noise rule's own answer, for an empty sky. */
    int underCeiling() const {
        int k = ceilingAt_ >= 0 ? ceilingAt_ - 1 : int(at_ < order_.size() ? at_ : order_.size()) - 1;
        while (k > 0 && rates_[size_t(k)] < 0) --k;
        return k >= 0 && !order_.empty() ? order_[size_t(k)] : -1;
    }
    bool cliff() const { return cliff_; }
    const std::vector<double>& floors() const { return floors_; }   ///< dBFS per rung, NAN = not measured
    /** The gain to settle on. ★ OFF THE CLIFF'S EDGE: when the rung after the best fell off a cliff and the rung before
     *  it is on the same plateau (within 5 %), the one before — the V4L's 40.2 dB rather than 43.4, the same 196 msg/s,
     *  with a busier band's headroom. */
    int result() const {
        if (best_ < 0) return order_.empty() ? -1 : order_[0];
        if (cliffAfterBest_ && best_ > 0 && rates_[size_t(best_ - 1)] >= 0.95 * rates_[size_t(best_)]) return order_[size_t(best_ - 1)];
        return order_[size_t(best_)];
    }
    double bestRate() const { return best_ >= 0 ? rates_[size_t(best_)] : 0; }
    const std::vector<int>& order() const { return order_; }
    const std::vector<double>& rates() const { return rates_; }   ///< -1 = not tried
    void cancel() { at_ = order_.size(); }
private:
    std::vector<int> order_;
    std::vector<double> rates_, floors_;
    size_t at_ = 0;
    int best_ = -1, below_ = 0, ceilingAt_ = -1, fN_ = 0;
    double ceiling_ = NAN, fSum_ = 0;
    bool climbing_ = false, cliffAfterBest_ = false, cliff_ = false;
    double legAt_ = 0, countFrom_ = -1;
    uint64_t countMsgs_ = 0;
};

/* ★★★ THE WHOLE RULE IN ONE PLACE (2026-10-26) — what adsbTick used to spell out inline, here so it can be tested.
 *
 *  ENTRY: the ramp, middle up, every other rung, ~6.5 s each (GainSearch, climbing), measuring messages AND the noise
 *    floor. It stops at the first of: a rung over the noise ceiling (GainTarget::kFloorDbfs) — the choice is then made
 *    among the rungs below; a cliff in messages (an overloading front end); two rungs past the peak. The rung between
 *    the last good one and the one over the ceiling is then tried A/B (the ramp skips every other rung).
 *    ★ The middle already over the ceiling (a hot feed): DESCEND, a rung every 4 s, until the floor is under it.
 *    ★ An empty sky: the noise rule alone — the highest rung the ramp found under the ceiling.
 *  STEADY: the floor (smoothed, α 0.3 a 2-s tick, and only once a gain write has settled) more than 1 dB over the
 *    ceiling for 10 s → one rung down, at once (dump1090-fa's change delay; the half-step hysteresis of its range rule
 *    is our 1 dB, about half a rung at the top of an R820T's list). Every ten minutes one rung either side, alternately,
 *    kept only if it CLEARLY decodes more (GainTrial) — and up only when the rung above is predicted under the ceiling
 *    (floor + the step, dB for dB: the noise of an LNA-fed feed follows the gain). So it cannot hunt: down needs the
 *    floor over the line, up needs it a whole step under.
 *  ★★ STRONG-REPLY PRESSURE: more than 30 % of believed replies within 3 dB of full scale, held 30 s, PROPOSES a rung
 *    down, tried A/B and taken unless it clearly loses messages; climbing then waits 30 minutes. 30 %, not dump1090-fa's
 *    burst rule or our first 5 %: the FlightAware Pi runs ~11 % strong at its right gain (graphs1090, 2.5 years) and
 *    the V4L 8 %; backing off for a strong NEARBY aircraft loses every distant one (the 10-24 lesson). It is the
 *    pile-up of clipping (57 % at 49.6 dB on the FA clip) that it is for.
 *  LEVEL-HOLDING RADIOS (an SDRplay: our gain chooses the LNA state, the IF AGC keeps its level): no noise ceiling —
 *    the floor is the AGC's — and NEVER ABOVE WHERE IT STARTS: from three-quarters up it only looks DOWN (a lower rung
 *    must clearly win), the ten-minute looks never climb past the start, and strong-reply pressure steps it down.
 *  ★★★ NEVER FREEZES THE WATERFALL — a gain change is a gain write and nothing else (feedback rule).
 *
 *  ★★ "HEARING LITTLE" (owner visibility, after the FlightAware Pi's May–June 2025: an antenna fault nobody knew of —
 *     dump1090-fa pinned 49.6 dB, its noise 10 dB under normal, 17–41 aircraft, 41–70 NM instead of 160–185). At the
 *     TOP rung for 10 minutes, the floor still 6 dB or more UNDER the ceiling (the gain has run out before the noise
 *     filled the ADC — a working ADS-B feed gets there long before), and the sky thin: nothing further than 100 NM in
 *     those 10 minutes (with the server's position), else under 40 aircraft and 400 msg/s. Replayed on the 2.5 years
 *     of graphs1090: 121 of the fault months' 126 rows at full gain, 0 of 2 249 healthy rows. Cleared after 2 minutes
 *     out of that state. Not on a level-holding radio (its floor says nothing of the feed). */
class GainControl {
public:
    static constexpr double kExploreS = 600.0;
    static constexpr double kStrongMax = 0.30, kStrongHoldS = 30.0, kStrongInhibitS = 1800.0;
    static constexpr double kDescendS = 4.0;
    static constexpr double kWarnMarginDb = 6.0, kWarnAfterS = 600.0, kWarnClearS = 120.0, kWarnFarNm = 100.0;
    static constexpr double kWarnAircraft = 40, kWarnMsgs = 400;

    /** `gains`: the radio's list, tenths of a dB, ascending. Returns the index to set now (-1 = no list). */
    int begin(const std::vector<int>& gains, bool levelHeld, double t) {
        gains_ = gains; held_ = levelHeld; trial_.cancel(); search_.cancel();
        floorEma_ = NAN; strongEma_ = 0; overSince_ = strongSince_ = warnSince_ = clearSince_ = -1;
        upInhibit_ = 0; strongRetryAt_ = 0; exploreAt_ = 0; exploreUp_ = false; warn_ = false; farLog_.clear(); trialForStrong_ = false;
        const int n = int(gains_.size());
        if (n == 0) { idx_ = -1; phase_ = Phase::Off; return -1; }
        const int mid = n / 2;
        std::vector<int> order;
        if (held_) {
            start_ = std::min(n - 1, n * 3 / 4);
            for (int i = start_; i >= 0; i -= 2) order.push_back(i);
            idx_ = search_.start(order, t, false);
        } else {
            start_ = n - 1;
            for (int i = mid; i < n; i += 2) order.push_back(i);
            if (order.back() != n - 1) order.push_back(n - 1);
            idx_ = search_.start(order, t, true, GainTarget::kFloorDbfs);
        }
        phase_ = Phase::Ramp; changedAt_ = t;
        why_ = "finding the gain: trying each step for a few seconds";
        return idx_;
    }

    /** Every tick (the shim's 2 s). Returns the index the radio should be at. */
    int tick(double t, const Health& h) {
        if (phase_ == Phase::Off || idx_ < 0) return idx_;
        const int n = int(gains_.size());
        const int was = idx_;
        observe(t, h);
        switch (phase_) {
        case Phase::Ramp: case Phase::LookDown: {
            idx_ = search_.step(t, h.msgsTotal, h.noiseDbfs);
            if (search_.active()) break;
            rampDone(t);
            break;
        }
        case Phase::Descend:
            if (t - changedAt_ < kDescendS) break;
            if (floorEma_ != floorEma_) break;   // ★ judged on the settled floor, not the write's own transient
            if (floorEma_ > GainTarget::kFloorDbfs && idx_ > 0) { --idx_; break; }
            phase_ = Phase::Steady; exploreAt_ = t + kExploreS;
            setWhy(floorEma_ > GainTarget::kFloorDbfs ? Reason::Bottom : Reason::Target);
            break;
        case Phase::Steady:
            steady(t, h, n);
            break;
        case Phase::Off: break;
        }
        if (idx_ != was) { changedAt_ = t; floorEma_ = NAN; overSince_ = -1; }
        judgeWarning(t, h, n);
        return idx_;
    }

    int index() const { return idx_; }
    bool busy() const { return search_.active() || trial_.active() || phase_ == Phase::Descend; }
    bool levelHeld() const { return held_; }
    /** Why the gain is where it is — a short line for the GAIN key's card. */
    const std::string& why() const { return why_; }
    /** "" or the owner's warning (see the class note). */
    std::string warning() const {
        if (!warn_) return {};
        char b[320];
        if (farLogMax() >= 0)
            std::snprintf(b, sizeof b, "The receiver is at full gain and hearing little: its noise floor is %.0f dB below where a working "
                          "ADS-B feed sits and nothing has been heard beyond %.0f NM. The antenna or its feed may be faulty — check the "
                          "cable and connectors, and any LNA or bias-T.", GainTarget::kFloorDbfs - floorShown(), farLogMax());
        else
            std::snprintf(b, sizeof b, "The receiver is at full gain and hearing little: its noise floor is %.0f dB below where a working "
                          "ADS-B feed sits. The antenna or its feed may be faulty — check the cable and connectors, and any LNA or bias-T.",
                          GainTarget::kFloorDbfs - floorShown());
        return b;
    }
    double floorDbfs() const { return floorShown(); }
    const GainSearch& search() const { return search_; }
    const GainTrial& trial() const { return trial_; }
    /** A listener took the gain: whatever was being tried is void. */
    void cancel() { search_.cancel(); trial_.cancel(); if (phase_ != Phase::Off) phase_ = Phase::Steady; }

private:
    enum class Phase { Off, Ramp, LookDown, Descend, Steady };
    enum class Reason { Target, Top, Cliff, Peak, Quiet, Bottom, Strong, Held, Kept, Moved };

    double gainDb(int i) const { return gains_[size_t(i)] / 10.0; }
    double floorShown() const { return floorEma_ == floorEma_ ? floorEma_ : floorNow_; }
    double farLogMax() const {
        double m = -1;
        for (const auto& p : farLog_) m = std::max(m, p.second);
        return m;
    }

    void observe(double t, const Health& h) {
        if (!h.valid) return;
        floorNow_ = h.noiseDbfs > -98 ? h.noiseDbfs : NAN;
        if (t - changedAt_ >= GainTarget::kSettleS && floorNow_ == floorNow_)
            floorEma_ = floorEma_ == floorEma_ ? floorEma_ + 0.3 * (floorNow_ - floorEma_) : floorNow_;
        strongEma_ += 0.3 * (h.strongFrac - strongEma_);
        // ★ the furthest heard, a sample per tick for the last 10 minutes (the warning's "nothing beyond …")
        if (h.farNm >= 0) farLog_.push_back({t, h.farNm});
        while (!farLog_.empty() && t - farLog_.front().first > kWarnAfterS) farLog_.pop_front();
        msgsEma_ += 0.1 * (h.msgsPerSec - msgsEma_);
        aircraft_ = h.aircraft; haveFar_ = h.farNm >= 0;
    }

    void setWhy(Reason r) {
        reason_ = r;
        char b[200];
        const double g = gainDb(idx_), f = floorShown();
        switch (r) {
        case Reason::Target: std::snprintf(b, sizeof b, "noise floor at its target (%.0f dBFS, 30 dB under full scale) — more gain would only fill the ADC with noise", f); break;
        case Reason::Top:    std::snprintf(b, sizeof b, "the top of the radio's range — the noise floor (%.0f dBFS) still has room", f); break;
        case Reason::Cliff:  std::snprintf(b, sizeof b, "messages fell away above %.1f dB — the front end overloads there", g); break;
        case Reason::Peak:   std::snprintf(b, sizeof b, "decodes the most messages here, noise floor %.0f dBFS", f); break;
        case Reason::Quiet:  std::snprintf(b, sizeof b, "too few aircraft to judge by — set by the noise floor (%.0f dBFS)", f); break;
        case Reason::Bottom: std::snprintf(b, sizeof b, "the bottom of the range and the noise floor is still high (%.0f dBFS)", f); break;
        case Reason::Strong: std::snprintf(b, sizeof b, "backed off: strong replies were clipping (%.0f %% within 3 dB of full scale)", strongEma_ * 100); break;
        case Reason::Held:   std::snprintf(b, sizeof b, "the radio holds its own level (IF AGC); this decodes the most"); break;
        case Reason::Kept:   std::snprintf(b, sizeof b, "checked one step either side: this still decodes the most"); break;
        case Reason::Moved:  std::snprintf(b, sizeof b, "checked one step either side: this one decodes more"); break;
        }
        why_ = b;
    }

    /** The floor the search measured at rung `i`, if it tried it — so the reason quotes the chosen rung's floor, not
     *  the last one tried (the rung over the ceiling). */
    void floorFromSearch(int i) {
        for (size_t k = 0; k < search_.order().size(); ++k)
            if (search_.order()[k] == i && search_.floors()[k] == search_.floors()[k]) { floorNow_ = search_.floors()[k]; floorEma_ = NAN; return; }
    }
    void rampDone(double t) {
        const int n = int(gains_.size()), mid = n / 2;
        const GainSearch& s = search_;
        exploreAt_ = t + kExploreS;
        phase_ = Phase::Steady;
        if (held_) {
            idx_ = s.found() ? s.result() : start_;
            floorFromSearch(idx_);
            setWhy(Reason::Held);
            return;
        }
        if (s.ceilingAt() == 0) {
            // ★ the very first rung tried is already over the ceiling: walk down
            phase_ = Phase::Descend;
            idx_ = std::max(0, s.order()[0] - 1);
            why_ = "the noise floor is over its target — stepping down";
            return;
        }
        if (!s.found()) {
            const int u = s.underCeiling();
            idx_ = u >= 0 ? u : s.order()[0];
            floorFromSearch(idx_);
            setWhy(Reason::Quiet);
        } else if (s.ceilingAt() < 0 && !lookedDown_ && s.result() == s.order().front() && mid >= 2) {
            // the middle was the best: the peak may be below it — look down the same way
            lookedDown_ = true;
            std::vector<int> order;
            for (int i = mid; i >= 0; i -= 2) order.push_back(i);
            idx_ = search_.start(order, t, false);
            phase_ = Phase::LookDown;
            return;
        } else {
            idx_ = s.result();
            floorFromSearch(idx_);
            setWhy(s.ceilingAt() >= 0 ? Reason::Target : s.cliff() ? Reason::Cliff : idx_ == n - 1 ? Reason::Top : Reason::Peak);
        }
        lookedDown_ = false;
        // ★ the ramp skips every other rung: the one between the choice and the rung over the ceiling is tried A/B
        if (s.ceilingAt() > 0) {
            const int over = s.order()[size_t(s.ceilingAt())];
            if (idx_ + 1 < over && predictedFloor(idx_ + 1) <= GainTarget::kFloorDbfs) {
                idx_ = trial_.start(idx_, idx_ + 1, t, false);
                trialForStrong_ = false;
            }
        }
    }

    /** The floor at rung i, from the floor now at idx_: dB for dB with the gain (an LNA-fed feed's noise follows it). */
    double predictedFloor(int i) const {
        const double f = floorShown();
        return f == f ? f + (gainDb(i) - gainDb(idx_)) : NAN;
    }

    void steady(double t, const Health& h, int n) {
        if (trial_.active()) {
            idx_ = trial_.step(t, h.msgsTotal);
            if (trial_.active()) { why_ = "checking one step either side — the message rate dips while it does"; return; }
            exploreAt_ = t + kExploreS;
            if (trialForStrong_ && trial_.proposedWon()) { setWhy(Reason::Strong); upInhibit_ = t + kStrongInhibitS; }
            // ★ pressure whose cut CLEARLY lost messages is not tried again for ten minutes — no dips every 30 s
            else if (trialForStrong_) strongRetryAt_ = t + kExploreS;
            else setWhy(trial_.proposedWon() ? Reason::Moved : Reason::Kept);
            if (!held_ && (reason_ == Reason::Kept || reason_ == Reason::Moved) && floorShown() == floorShown() &&
                predictedFloor(std::min(n - 1, idx_ + 1)) > GainTarget::kFloorDbfs && idx_ < n - 1) setWhy(Reason::Target);
            trialForStrong_ = false;
            return;
        }
        // ★★ THE NOISE CEILING, with hysteresis: over by more than 1 dB for 10 s → one rung down, no trial
        if (!held_ && floorEma_ == floorEma_ && floorEma_ > GainTarget::kFloorDbfs + GainTarget::kDownHystDb) {
            if (overSince_ < 0) overSince_ = t;
            if (t - overSince_ >= GainTarget::kChangeDelayS && t - changedAt_ >= GainTarget::kChangeDelayS && idx_ > 0) {
                --idx_; setWhy(Reason::Target); return;
            }
        } else overSince_ = -1;
        // ★★ strong-reply pressure: a rung down, proposed and tried
        if (strongEma_ > kStrongMax && h.msgsPerSec >= 5 && t >= strongRetryAt_) {
            if (strongSince_ < 0) strongSince_ = t;
            if (t - strongSince_ >= kStrongHoldS && idx_ > 0) {
                strongSince_ = -1; trialForStrong_ = true;
                idx_ = trial_.start(idx_, idx_ - 1, t, true);
                return;
            }
        } else strongSince_ = -1;
        // ★ every ten minutes one rung either side, alternately — kept only if it clearly decodes more
        if (exploreAt_ > 0 && t >= exploreAt_) {
            exploreAt_ = t + kExploreS;
            exploreUp_ = !exploreUp_;
            const int up = idx_ + 1, dn = idx_ - 1;
            const bool upOk = up < n && t >= upInhibit_ &&
                              (held_ ? up <= start_ : (floorShown() == floorShown() && predictedFloor(up) <= GainTarget::kFloorDbfs));
            const int nb = exploreUp_ ? (upOk ? up : -1) : dn;
            if (nb >= 0 && nb < n) { trialForStrong_ = false; idx_ = trial_.start(idx_, nb, t, false); }
        }
    }

    void judgeWarning(double t, const Health& h, int n) {
        (void)h;
        const double f = floorShown();
        const bool thin = haveFar_ ? farLogMax() < kWarnFarNm : (aircraft_ < kWarnAircraft && msgsEma_ < kWarnMsgs);
        const bool cond = !held_ && phase_ == Phase::Steady && !trial_.active() && idx_ == n - 1 &&
                          f == f && f <= GainTarget::kFloorDbfs - kWarnMarginDb && thin;
        if (cond) {
            clearSince_ = -1;
            if (warnSince_ < 0) warnSince_ = t;
            if (t - warnSince_ >= kWarnAfterS) warn_ = true;
        } else {
            warnSince_ = -1;
            if (warn_) {
                if (trial_.active()) return;   // ★ a look one rung down is not leaving the top
                if (clearSince_ < 0) clearSince_ = t;
                if (t - clearSince_ >= kWarnClearS) { warn_ = false; clearSince_ = -1; }
            }
        }
    }

    std::vector<int> gains_;
    bool held_ = false, exploreUp_ = false, warn_ = false, trialForStrong_ = false, lookedDown_ = false, haveFar_ = false;
    int idx_ = -1, start_ = 0, aircraft_ = 0;
    Phase phase_ = Phase::Off;
    Reason reason_ = Reason::Peak;
    GainSearch search_;
    GainTrial trial_;
    double changedAt_ = 0, floorEma_ = NAN, floorNow_ = NAN, strongEma_ = 0, msgsEma_ = 0;
    double overSince_ = -1, strongSince_ = -1, upInhibit_ = 0, strongRetryAt_ = 0, exploreAt_ = 0, warnSince_ = -1, clearSince_ = -1;
    std::deque<std::pair<double, double>> farLog_;
    std::string why_;
};

} // namespace vibeadsb
