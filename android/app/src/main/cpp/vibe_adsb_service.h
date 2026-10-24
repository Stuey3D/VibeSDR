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
            queue_.push_back(std::move(b));
            while (queued_ > size_t(rate_ * kMaxQueuedSec) && queue_.size() > 1) {
                queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_;
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
                std::vector<std::complex<float>> b = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= b.size();
                Demod* d = demod_.get();
                lk.unlock();
                // ★ The tracker is touched only on this thread and under mtx_ for report(); the demodulator
                //   only here. A frame is judged as soon as it is cut, so `known` sees the latest aircraft.
                const double t = now();
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
    std::unique_ptr<Demod> demod_;
    Tracker tracker_;
    std::deque<std::vector<std::complex<float>>> queue_;
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
 * ADS-B's own gain rule — the same idea as dump1090-fa's adaptive range, on our measurements.
 *
 * ★★★ NOT VibeAGC. VibeAGC sets gain from the capture's average level; ADS-B is mostly noise with short strong
 *     bursts, so an average-level rule either leaves the noise floor far below the ADC's range (weak aircraft
 *     lost) or lets nearby aircraft clip. ADS-B wants the noise floor high enough to use the ADC's range and
 *     backs off only when replies start to clip.
 * ★ Works on the radio's OWN gain list (LocalSdrShim::getTunerGains: the RTL's table, SDRplay 0–49 dB, the
 *   Airspy/HackRF lists), so it is one rule for every radio rather than a control that works on one.
 */
/* ★★★ THE GAIN RULE — MAXIMUM, AND DOWN ONLY FOR DYNAMIC RANGE (Stuart, 2026-10-24: "maximum gain for the RTL in
 *  ADSB mode and if we can be inspired by the way that my flight aware box handles the gain"). His FlightAware Pi runs
 *  dump1090-fa 10.2 with --gain 60 (the top) and --adaptive-range, ADAPTIVE_BURST=no: it starts at the top and comes
 *  down only while the noise floor leaves too little room under full scale — settled at 38.6 dB, noise −30.6 dBFS, ~11 %
 *  of its signals "strong", two gain changes in fifteen minutes, ~1600 msg/s. Our own rule after that behaviour (its
 *  code is GPL and not used): a strong aircraft is NOT a reason to come down — a reply that clips is still decoded, and
 *  backing off for it (our first rule: more than 5 % within 3 dB of full scale) loses every distant one. That rule held
 *  the Pi 500 V4L at 40–44 dB against the Pi 2's 49.6.
 *  ★ The votes and holds that keep it calm are the shim's (adsbTick): down after 6 s of asking, up after 30 s, never
 *    up within 60 s of a cut. */
struct GainRule {
    double rangeDb = 30;       ///< the noise floor must sit at least this far below full scale (dump1090-fa's 30 dB)
    double climbSpareDb = 3;   ///< climb only with this much to spare — about one rung at the top of an R820T's list
    double strongMax = 0.05;   ///< levelHeld radios only: more than 5 % of replies within 3 dB of full scale = too hot
    /** ★★ THE RADIO HOLDS THE LEVEL ITSELF (an SDRplay: our gain picks the LNA state and its IF AGC stays on). The
     *  noise floor then does not follow the gain — the Lenovo's RSP sat at −43 dBFS while the rule climbed 37 → 49 dB
     *  for nothing. Such a radio only ever steps DOWN, when replies clip; it never climbs past where it started. */
    bool levelHeld = false;

    /** -1 = step down, +1 = step up, 0 = stay. */
    int decide(const Health& h) const {
        if (!h.valid) return 0;
        if (levelHeld) return h.strongFrac > strongMax ? -1 : 0;
        if (h.noiseDbfs > -rangeDb) return -1;
        if (h.noiseDbfs < -(rangeDb + climbSpareDb)) return +1;
        return 0;
    }
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
    int start(const std::vector<int>& order, double t, bool climbing = false) {
        order_ = order; rates_.assign(order.size(), -1); at_ = 0; legAt_ = t; countFrom_ = -1; best_ = -1; below_ = 0;
        climbing_ = climbing;
        return order_.empty() ? -1 : order_[0];
    }
    int step(double t, uint64_t msgsTotal) {
        if (!active()) return result();
        if (countFrom_ < 0) {
            if (t - legAt_ >= kSettleS) { countFrom_ = t; countMsgs_ = msgsTotal; }
            return order_[at_];
        }
        if (t - countFrom_ < kCountS) return order_[at_];
        rates_[at_] = double(msgsTotal - countMsgs_) / (t - countFrom_);
        // ★ a LOWER gain must clearly beat the best (5 %, past the counts' noise): a flat curve stays at the top — the
        //   recording read 1351 at 49.6 and 1390 at 25.4 dB, and "25.4" would have been chosen for nothing
        if (best_ < 0 || rates_[at_] > (climbing_ ? 0.97 : 1.05) * rates_[size_t(best_)]) best_ = int(at_);
        // ★ past the peak = two rungs in a row each more than 5 % under the best (one could be a quiet moment)
        below_ = int(at_) > best_ && rates_[at_] < 0.95 * rates_[size_t(best_)] ? below_ + 1 : 0;
        const bool pastPeak = below_ >= 2 && rates_[size_t(best_)] >= kQuietMsgs;
        ++at_;
        if (pastPeak) at_ = order_.size();
        if (!active()) return result();
        legAt_ = t; countFrom_ = -1;
        return order_[at_];
    }
    /** True once over, with a sky busy enough to have judged by. */
    bool found() const { return !active() && best_ >= 0 && rates_[size_t(best_)] >= kQuietMsgs; }
    int result() const { return best_ >= 0 ? order_[size_t(best_)] : (order_.empty() ? -1 : order_[0]); }
    double bestRate() const { return best_ >= 0 ? rates_[size_t(best_)] : 0; }
    const std::vector<int>& order() const { return order_; }
    const std::vector<double>& rates() const { return rates_; }   ///< -1 = not tried
    void cancel() { at_ = order_.size(); }
private:
    std::vector<int> order_;
    std::vector<double> rates_;
    size_t at_ = 0;
    int best_ = -1, below_ = 0;
    bool climbing_ = false;
    double legAt_ = 0, countFrom_ = -1;
    uint64_t countMsgs_ = 0;
};

} // namespace vibeadsb
