// vibe_ism_service.h — V12 ISM mode's decoder service: the bands, the front end + protocols on their own threads, the
// privacy rule (per-server nicknames from a salted in-memory hash), the device list and the JSON the clients draw.
// docs/v12/ISM-DECODER.md.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS — ADS-B's rule (vibe_adsb_service.h): Network > Audio >
//     Spectrum > Decoders. feed() copies and returns; a full queue loses the OLDEST block and counts it.
// ★★★ THREE THREADS, ONE PER STAGE (2026-10-31). On the Pi 2 — the minimum spec — ISM 433 at full span was ONE thread at
//     106 % of a core (measured live): it could not keep up and missed bursts while three cores idled. The front end's
//     stages (vibe_ism.h) now run apart, each fed by a bounded queue that drops its OLDEST work and counts it:
//       vibe-ism   the detector (FFT, floor, where each burst is) — the one stage that must keep real time;
//       vibe-ism2  the channel mixer (each burst's narrow channel), one notch lower (vibeDecoderLagThread);
//       vibe-ism3  the slicer, the protocol decoders, the device list and the gallery, at the same lower notch.
//     A later stage may lag (sensors do not need their line this instant) and, if it falls far enough behind, drops; the
//     detector never waits for it. Every drop shows in the health pill ("too heavy") through droppedBlocks(). Memory
//     stays bounded: half a second of capture before the detector, half a second of work samples before the mixer, and
//     before the slicer four of the longest channels and a second more (~34 MB worst case at 2.4 MS/s on 433, ~48 MB on
//     868's wider channel, all of it only while behind).
//     It runs on a single core as well: the stages simply share it, and the detector's higher rung keeps it in time.
// ★★★ PRIVACY (Stuart, 2026-10-25). A TPMS, a key fob, a fixed-code remote is NEVER identified in anything this file
//     sends, logs or stores: no ID, no code, no raw bits. It is "TPMS #2" — a number handed out in the order the server
//     first heard each one, keyed by a SipHash of its ID under a salt drawn fresh at every start and held only in this
//     process's memory. The ID itself is wiped from the Reading the moment it is hashed. Public and private servers alike.
#pragma once

#include "vibe_ism_protocols.h"
#include "vibe_wmbus.h"        // ★ V12 2026-10-26: Wireless M-Bus meters on the 868 band (docs/v12/WMBUS-DECODER.md)
#include "vibe_ism_bands.h"    // the four bands, reachability, the landing band
#include "vibe_ism_sink.h"     // ★ V12 2026-10-25: the gallery's copy of every new reading (vibe_ism_gallery.h)
#include "vibe_adsb_capture.h"     // vibeadsb::CaptureMeter — the same live "capture N %" as ADS-B
#include "vibe_lora_track.h"      // ★ V12 2026-10-27: LoRa — LoRaWAN, Meshtastic, MeshCore (docs/v12/LORA-MESH.md)
#include "vibe_thread.h"          // the threads' names and rungs (vibe-ism, vibe-ism2, vibe-ism3)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vibeism {

// ★ THE BANDS live in vibe_ism_bands.h (2026-10-25) — the background-decoding config validates against them too.


// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SipHash-2-4 (Aumasson & Bernstein, 2012 — public algorithm, reference description) for the nickname keys.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint64_t sipHash24(uint64_t k0, uint64_t k1, uint64_t m) {
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    v3 ^= m; round(); round(); v0 ^= m;
    const uint64_t last = uint64_t(8) << 56;
    v3 ^= last; round(); round(); v0 ^= last;
    v2 ^= 0xff; round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  READABLE TEXT — "Oregon Scientific THGR810 · 18.4 °C · 62 % RH · ch 2"
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Units { bool mph = false; int pressure = 0; /* 0 PSI, 1 kPa, 2 bar */ };

inline std::string fmt1(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }
inline const char* compass(double deg) {
    static const char* p[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};
    int i = int(std::floor(std::fmod(deg + 11.25 + 3600.0, 360.0) / 22.5)) & 15;
    return p[i];
}
inline const char* kindLabel(const std::string& k) {
    if (k == "tpms") return "TPMS";
    if (k == "keyfob") return "Key fob";
    if (k == "suspectfob") return "Suspected key fob";
    if (k == "doorbell") return "Doorbell / remote";
    return "";
}
/** The device's name: "Oregon Scientific THGR810", or for a secret kind "TPMS #2" / "Key fob #1". */
inline std::string deviceName(const Reading& r, int nick) {
    if (r.kind == "meter" && r.meter) return wmbus::nameOf(*r.meter);     // "Water meter · Sensus · 12345678"
    if (r.secret) {
        std::string n = kindLabel(r.kind);
        return n + " #" + std::to_string(nick);
    }
    return r.model.empty() ? r.make : r.make + " " + r.model;
}
inline std::string readable(const Reading& r, int nick, const Units& u = Units()) {
    // ★ a meter's CORE line (no signal, no interval — the Tracker adds those): nothing in it has a unit choice
    if (r.kind == "meter" && r.meter) return wmbus::coreLine(*r.meter);
    std::vector<std::string> p;
    p.push_back(deviceName(r, nick));
    if (r.secret && r.kind == "tpms" && !r.model.empty()) p.push_back(r.model);   // "TPMS #2 · type B · …"
    if (r.secret && r.kind == "keyfob" && !r.make.empty()) p.push_back(r.make);    // "Key fob #1 · KeeLoq · …"
    if (r.kind == "tpms" && r.kpa == r.kpa) {
        if (u.pressure == 1) p.push_back(fmt1(r.kpa, 0) + " kPa");
        else if (u.pressure == 2) p.push_back(fmt1(r.kpa / 100.0, 2) + " bar");
        else p.push_back(fmt1(r.kpa * 0.1450377, 0) + " PSI");
    }
    if (r.tempC == r.tempC) p.push_back(fmt1(r.tempC, r.kind == "tpms" ? 0 : 1) + " \xc2\xb0" "C");
    if (r.hum == r.hum) p.push_back(fmt1(r.hum, 0) + " % RH");
    auto speed = [&](double kmh) { return u.mph ? fmt1(kmh / 1.609344, 0) + " mph" : fmt1(kmh, 0) + " km/h"; };
    if (r.windKmh == r.windKmh) {
        std::string w = "wind " + speed(r.windKmh);
        if (r.windDir == r.windDir) w += std::string(" ") + compass(r.windDir);
        p.push_back(w);
    }
    if (r.gustKmh == r.gustKmh) p.push_back("gust " + speed(r.gustKmh));
    if (r.rainMm == r.rainMm) p.push_back("rain " + fmt1(r.rainMm, 1) + " mm");
    if (r.rainRateMmH == r.rainRateMmH && r.rainRateMmH > 0) p.push_back(fmt1(r.rainRateMmH, 1) + " mm/h");
    if (r.uv == r.uv) p.push_back("UV " + fmt1(r.uv, 0));
    if (r.baroHpa == r.baroHpa) p.push_back(fmt1(r.baroHpa, 0) + " hPa");
    if (r.watts == r.watts) p.push_back(r.watts >= 1000 ? fmt1(r.watts / 1000.0, 2) + " kW" : fmt1(r.watts, 0) + " W");
    if (r.kind == "keyfob" || r.kind == "doorbell" || r.kind == "suspectfob") {
        if (!r.button.empty()) p.push_back("button " + r.button);
        else if (r.note == "learn") p.push_back("learn");
        else p.push_back("button press");
    }
    if (!r.extra.empty()) p.push_back(r.extra);
    if (r.battOk == 1) p.push_back("battery OK"); else if (r.battOk == 0) p.push_back("battery low");
    if (!r.secret && r.channel >= 0) p.push_back("ch " + std::to_string(r.channel));
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += " \xc2\xb7 "; s += p[i]; }
    return s;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (uint8_t(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SUSPECTED KEY FOBS — a car key we cannot name still shows up (Stuart, 2026-10-25: "add them to a list that maybe
//  says suspected keyfob"). His key had lit the waterfall and appeared nowhere in the list.
//
//  ★★ WHAT A PRESS LOOKS LIKE, measured on his key (raw IQ, the Pi 500's V4L, 16 presses): FSK at 434.134 MHz,
//     ±42.7 kHz, every run 200 or 400 µs (Manchester, 2.5 kbit/s) after an alternating preamble, 68 ms frames, the
//     SAME frame 2-4 times 161 ms apart (more while the button is held). Car keys are rolling-code and proprietary, so
//     the name stays "suspected" — the shape, not a protocol, is the evidence. So a press is:
//       · an UNDECODED burst, FSK (deviation 10-120 kHz), at least 10 dB over the noise, 15-200 ms long;
//       · its runs Manchester-shaped: ≥ 90 % within a quarter of T or 2T, T = its shortest common run, 50-1000 µs;
//       · REPEATED: an earlier such frame within 30 kHz, of the same length (±15 %), 60-600 ms before it.
//  ★★ WHAT IS NOT, from the same recording: a neighbour's FSK sensor at 434.283 (one 47 ms frame every 1-2 s — never
//     repeated inside 600 ms), a carrier blip at 434.139 every second (OOK, not FSK), and the DONGLE'S OWN IMAGES of the
//     key ±120 kHz away when it overloads (0 dBFS) — one press is counted ONCE: after a press, nothing within 250 kHz
//     counts until its frames have stopped for 0.5 s (a held button is one press). And anything that comes back on a
//     TIMER (three intervals alike within 10 %, ≥ 8 s apart) is a sensor, not a hand on a button: that frequency stops
//     counting and its row goes.
//  ★★★ PRIVACY: no bits are read, kept or sent. The row is "Suspected key fob #n" by frequency (50 kHz) and symbol
//     time — two fobs of one make on one frequency share a row, which is the price of never reading an ID.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class FobWatch {
public:
    /** Offer an UNDECODED burst. True = it completes a press: `out` is the reading to show. */
    bool offer(const Burst& b, double hwCentreHz, Reading& out) {
        const double t = b.tSec, f = hwCentreHz + b.offHz + b.carrierHz;
        while (!frames_.empty() && t - frames_.front().t > 1.5) frames_.pop_front();
        double T = 0, lenMs = 0;
        if (!candidate(b, T, lenMs)) return false;
        // ★ the same press (or its images): it lasts while its frames keep coming — a held button is ONE press — and the
        //   next press needs half a second of silence (his presses came 1.3 s apart; a fixed 1.5 s hold-off lost them)
        if (t < quietUntil_ && std::fabs(f - quietHz_) < 250e3) { quietUntil_ = t + 0.5; return false; }
        const long bucket = std::lround(f / 50e3);
        if (periodic_.count(bucket)) return false;
        bool repeat = false;
        for (const Frame& p : frames_) {
            const double dt = t - p.t;
            if (dt >= 0.060 && dt <= 0.600 && std::fabs(f - p.f) < 30e3 && std::fabs(lenMs - p.lenMs) <= 0.15 * p.lenMs) { repeat = true; break; }
        }
        frames_.push_back({t, f, lenMs});
        if (frames_.size() > 64) frames_.pop_front();
        if (!repeat) return false;
        quietUntil_ = t + 0.5; quietHz_ = f;
        // ★ a timer, not a hand: three intervals alike, ≥ 8 s apart
        auto& ts = presses_[bucket];
        ts.push_back(t);
        if (ts.size() > 4) ts.erase(ts.begin());
        if (ts.size() == 4) {
            const double a = ts[1] - ts[0], c = ts[2] - ts[1], d = ts[3] - ts[2];
            const double lo = std::min(a, std::min(c, d)), hi = std::max(a, std::max(c, d));
            if (lo >= 8.0 && hi <= lo * 1.1) { periodic_.insert(bucket); periodicHz_.push_back(f); return false; }
        }
        out = Reading();
        out.kind = "suspectfob"; out.make = "FSK"; out.secret = true;
        out.secretId = (uint64_t(bucket) << 12) ^ uint64_t(std::lround(T / 25.0));
        out.freqHz = f; out.levelDb = b.levelDb; out.snrDb = b.levelDb - b.noiseDb;
        ++presses_total_;
        return true;
    }
    /** Frequencies found to be on a timer since the last clear — their rows are removed by the caller. */
    std::vector<double> takePeriodic() { std::vector<double> v; v.swap(periodicHz_); return v; }
    uint64_t presses() const { return presses_total_; }
    void clear() { frames_.clear(); presses_.clear(); periodic_.clear(); periodicHz_.clear(); quietUntil_ = -1; }

    /** The burst-level test, public for the tests. */
    static bool candidate(const Burst& b, double& T, double& lenMs) {
        if (b.fsk.size() < 24 || b.fskDevHz < 10e3 || b.fskDevHz > 120e3 || b.levelDb - b.noiseDb < 10) return false;
        double all = 0; for (int d : b.fsk) all += std::abs(d);
        lenMs = all / 1000.0;
        if (lenMs < 15 || lenMs > 200) return false;
        // T: the shortest run that at least a fifth of the runs sit at (one stray glitch must not set it)
        std::vector<int> a; a.reserve(b.fsk.size()); for (int d : b.fsk) a.push_back(std::abs(d));
        std::sort(a.begin(), a.end());
        T = a[a.size() / 5];
        if (T < 50 || T > 1000) return false;
        size_t fit = 0;
        for (int d : a) if (std::fabs(d - T) <= 0.25 * T || std::fabs(d - 2 * T) <= 0.25 * T) ++fit;
        return double(fit) >= 0.9 * double(a.size());
    }

private:
    struct Frame { double t, f, lenMs; };
    std::deque<Frame> frames_;
    std::map<long, std::vector<double>> presses_;
    std::set<long> periodic_;
    std::vector<double> periodicHz_;
    double quietUntil_ = -1, quietHz_ = 0;
    uint64_t presses_total_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE DEVICE LIST — newest first, one line per device, repeats folded, last-heard time.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ V12 meters: the interval and the reception rate, from the access number (WMBUS-DECODER.md §4.3). A delta d =
 *  (acc − last) mod 256 of 1-16 is one sample: (t − lastT) over d steps. Reception = frames heard ÷ frames sent over the
 *  last hour.
 *  ★★ THE INTERVAL IS TIME OVER STEPS, NOT A MEDIAN OF GAPS (2026-10-26, test-wmbus): OMS asks meters to jitter their
 *     send time, and every single gap carries TWO jitters (this frame's and the last's) — the median of 8 such gaps read
 *     "every 17 s" for a 16 s meter with ±1 s jitter. The sum of the last 16 samples' time over the sum of their steps
 *     carries only the two end jitters; a sample more than a quarter off the median step is left out (a stray). ★ ASSUMED:
 *     ACC steps once per transmission; a meter whose ACC does not move (some only step it on a readout) falls back to
 *     the median raw gap and shows no reception figure. */
struct MeterTrack {
    int lastAcc = -1; double lastT = -1;
    std::deque<std::pair<double, int>> gaps;     ///< (seconds, steps) — steps 0 = a raw gap (ACC frozen)
    std::deque<std::pair<double, int>> rx;       ///< (time, steps) over the last hour
    bool frozen = false;
    void note(int acc, double t) {
        if (acc < 0) return;
        if (lastAcc < 0) { lastAcc = acc; lastT = t; return; }
        const int d = (acc - lastAcc) & 255;
        if (d == 0) {
            if (t - lastT < 3.0) return;                       // the same frame sent twice
            if (!frozen) gaps.clear();
            gaps.push_back({t - lastT, 0}); frozen = true; rx.clear();
        } else if (d <= 16) {
            if (frozen) gaps.clear();
            gaps.push_back({t - lastT, d}); rx.push_back({t, d}); frozen = false;
        } else { gaps.clear(); rx.clear(); }
        while (gaps.size() > 16) gaps.pop_front();
        while (!rx.empty() && t - rx.front().first > 3600) rx.pop_front();
        lastAcc = acc; lastT = t;
    }
    double every() const {
        if (gaps.empty()) return 0;
        std::vector<double> g;
        for (const auto& e : gaps) g.push_back(e.first / std::max(1, e.second));
        std::sort(g.begin(), g.end());
        const double med = g.size() & 1 ? g[g.size() / 2] : 0.5 * (g[g.size() / 2 - 1] + g[g.size() / 2]);
        if (frozen) return med;
        double sec = 0; int steps = 0;
        for (const auto& e : gaps) if (std::fabs(e.first / e.second - med) <= 0.25 * med) { sec += e.first; steps += e.second; }
        return steps ? sec / steps : med;
    }
    int rxPct() const {
        if (frozen || rx.size() < 2) return -1;
        int sent = 0; for (const auto& e : rx) sent += e.second;
        return sent > 0 ? int(std::lround(100.0 * double(rx.size()) / sent)) : -1;
    }
};

class Tracker {
public:
    static constexpr size_t kMaxDevices = 300;
    /** ★ Meters have their own cap: a town street can hold more meters than that within range of a good 868 antenna, and
     *  they must never push the weather station off the list (WMBUS-DECODER.md §5.5). */
    static constexpr size_t kMaxMeters = 500;
    Tracker() {
        std::random_device rd;
        k0_ = (uint64_t(rd()) << 32) ^ rd();
        k1_ = (uint64_t(rd()) << 32) ^ rd();
    }
    /** Take a reading heard at `t` (seconds). Returns true when the list changed. ★ Wipes r.secretId.
     *  `out` (when not null, and only when this returns true): the reading as the LIST counts it — its kind, its name and
     *  its readable line, nothing else (vibe_ism_sink.h) — for the gallery's daily log. */
    bool accept(Reading r, double t, Heard* out = nullptr) {
        std::string key;
        int nick = 0;
        if (r.secret) {
            const uint64_t h = sipHash24(k0_, k1_, r.secretId);
            r.secretId = 0;                                         // ★ gone — only the hash remains, in memory only
            const std::string fam = r.kind;
            auto it = nickOf_.find(h);
            if (it == nickOf_.end()) {
                if (nickOf_.size() > 4096) nickOf_.clear();           // bounded; numbering restarts, which is harmless
                it = nickOf_.emplace(h, ++nextNick_[fam]).first;
            }
            nick = it->second;
            char kb[40]; std::snprintf(kb, sizeof kb, "s%016llx", (unsigned long long)h);
            key = kb;                                                 // ★ internal map key; never sent (devices get "d<n>")
        } else if (r.kind == "meter" && r.meter) {
            key = "meter:" + r.meter->mfr + ":" + r.meter->id + ":" + std::to_string(r.meter->type);
        } else {
            key = r.make + "|" + r.model + "|" + std::to_string(r.channel) + "|" + r.pubId;
        }
        if (r.kind == "meter" && r.meter) return acceptMeter(key, std::move(r), t, out);
        if (r.confirm) {
            // ★ a weak check: the same values from the same device twice within 90 s, or nothing
            const std::string sig = key + "|" + readable(r, nick);
            auto pit = pending_.find(sig);
            if (pit == pending_.end() || t - pit->second > 90) {
                pending_[sig] = t;
                if (pending_.size() > 512) pending_.clear();
                return false;
            }
        }
        Dev& d = devs_[key];
        const std::string line = readable(r, nick);
        const bool fresh = d.count == 0;
        if (fresh) { d.id = ++nextId_; d.first = t; }
        /* ★ A SUSPECTED FOB'S READING IS ALREADY ONE PRESS (FobWatch merged its frames and images), so it is never folded
         *  as a repeat: Stuart's 16 presses 1-3 s apart chained through the 3 s fold into ONE reading (count 1, one log
         *  line). Every other kind still folds — a sensor or a KeeLoq fob sends several frames per transmission. */
        const bool repeat = !fresh && line == d.line && t - d.last < 3.0 && r.kind != "suspectfob";
        if (!repeat) ++d.count;
        d.r = r; d.nick = nick; d.line = line; d.last = t;
        ++decoded_;
        prune(t);
        if (!repeat && out) { out->kind = r.kind; out->name = deviceName(r, nick); out->line = line; out->freqHz = r.freqHz; out->secret = r.secret; }
        return !repeat;
    }
    void prune(double t) {
        size_t meters = 0;
        for (auto it = devs_.begin(); it != devs_.end();) {
            const bool brief = it->second.r.secret;                       // cars and fobs pass by: 15 min; sensors 2 h
            if (t - it->second.last > (brief ? 900 : 7200)) it = devs_.erase(it); else { meters += it->second.r.kind == "meter"; ++it; }
        }
        // ★ two caps: meters among themselves, everything else among itself — the least recently heard of its own sort goes
        for (int pass = 0; pass < 2; ++pass) {
            const bool m = pass == 1;
            size_t have = m ? meters : devs_.size() - meters;
            const size_t cap = m ? kMaxMeters : kMaxDevices;
            while (have > cap) {
                auto old = devs_.end();
                for (auto it = devs_.begin(); it != devs_.end(); ++it)
                    if ((it->second.r.kind == "meter") == m && (old == devs_.end() || it->second.last < old->second.last)) old = it;
                if (old == devs_.end()) break;
                devs_.erase(old); --have;
            }
        }
    }
    size_t meterCount() const { size_t n = 0; for (const auto& kv : devs_) n += kv.second.r.kind == "meter"; return n; }
    void clear() { devs_.clear(); pending_.clear(); }
    /** Remove the rows of `kind` within `hz` of `freq` (a suspected fob found to be a sensor on a timer). */
    void forget(const std::string& kind, double freq, double hz) {
        for (auto it = devs_.begin(); it != devs_.end();)
            if (it->second.r.kind == kind && std::fabs(it->second.r.freqHz - freq) < hz) it = devs_.erase(it); else ++it;
    }
    size_t size() const { return devs_.size(); }
    uint64_t decoded() const { return decoded_; }
    /** ★ V12 2026-10-25: the next nickname of `kind` will be above `floor` — today's gallery summary already names
     *  "TPMS #1"…"#floor" from before a restart, and a fresh salt cannot tell whether a car is one of those, so a new
     *  one must not take a name the day has already used. */
    void reserveNicks(const std::string& kind, int floor) { int& n = nextNick_[kind]; if (floor > n) n = floor; }

    /** The device list, newest first: [{…}, …]. ★ V12 LoRa: `extra` rows (vibe_lora_track.h — mesh nodes, LoRaWAN
     *  devices), each with its last-heard time, are merged in by that time. */
    std::string json(double t, const std::vector<std::pair<double, std::string>>* extra = nullptr) const {
        std::vector<const Dev*> v;
        for (const auto& kv : devs_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Dev* a, const Dev* b) { return a->last > b->last; });
        std::vector<std::pair<double, std::string>> more;
        if (extra) { more = *extra; std::sort(more.begin(), more.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); }
        size_t mi = 0;
        std::string o = "[";
        bool first = true;
        auto flushMore = [&](double before) {
            while (mi < more.size() && more[mi].first >= before) { if (!first) o += ','; first = false; o += more[mi++].second; }
        };
        for (const Dev* d : v) {
            flushMore(d->last);
            const Reading& r = d->r;
            if (!first) o += ',';
            first = false;
            char b[96];
            std::snprintf(b, sizeof b, "{\"key\":\"d%llu\"", (unsigned long long)d->id);
            o += b;
            o += ",\"kind\":\"" + jsonEsc(r.kind) + "\",\"make\":\"" + jsonEsc(r.make) + "\",\"model\":\"" + jsonEsc(r.model) + "\"";
            o += ",\"name\":\"" + jsonEsc(deviceName(r, d->nick)) + "\"";
            if (r.secret) o += ",\"nick\":" + std::to_string(d->nick);
            else {
                if (!r.pubId.empty()) o += ",\"id\":\"" + jsonEsc(r.pubId) + "\"";
                if (r.channel >= 0) o += ",\"ch\":" + std::to_string(r.channel);
            }
            o += ",\"line\":\"" + jsonEsc(d->line) + "\"";
            auto num = [&](const char* k, double x, int dp) { if (x == x) { o += ",\""; o += k; o += "\":" + fmt1(x, dp); } };
            num("temp", r.tempC, 1); num("hum", r.hum, 0); num("wind", r.windKmh, 1); num("gust", r.gustKmh, 1);
            num("dir", r.windDir, 0); num("rain", r.rainMm, 1); num("rainRate", r.rainRateMmH, 1); num("kpa", r.kpa, 1);
            num("uv", r.uv, 0); num("watts", r.watts, 0); num("baro", r.baroHpa, 1);
            if (r.battOk >= 0) o += ",\"batt\":" + std::to_string(r.battOk);
            if (!r.button.empty()) o += ",\"button\":\"" + jsonEsc(r.button) + "\"";
            if (!r.note.empty()) o += ",\"note\":\"" + jsonEsc(r.note) + "\"";
            if (!r.extra.empty() && r.kind != "meter") o += ",\"extra\":\"" + jsonEsc(r.extra) + "\"";
            if (r.kind == "meter" && r.meter) meterJson(o, *d);
            std::snprintf(b, sizeof b, ",\"freq\":%.0f,\"level\":%.1f,\"snr\":%.1f,\"seen\":%.0f,\"age\":%.0f,\"count\":%d}",
                          r.freqHz, r.levelDb, r.snrDb, std::max(0.0, t - d->last), std::max(0.0, t - d->first), d->count);
            o += b;
        }
        flushMore(-1e300);
        return o + "]";
    }

private:
    struct Dev { uint64_t id = 0; Reading r; int nick = 0; std::string line; double first = 0, last = 0; int count = 0;
                 std::string core; MeterTrack mt; };   // ★ core / mt: meters only

    /** ★ A METER (WMBUS-DECODER.md §4): one row per meter; a frame repeats only when its CORE line is the same inside 3 s
     *  (signal and access number change every frame and are not news). A DAMAGED frame (block 1 good, a later block bad)
     *  says the meter was heard — its row's time, signal and count move — but its readings are not touched. */
    bool acceptMeter(const std::string& key, Reading r, double t, Heard* out) {
        Dev& d = devs_[key];
        const bool fresh = d.count == 0;
        if (fresh) { d.id = ++nextId_; d.first = t; }
        if (r.meter->accOk) d.mt.note(r.meter->acc, t);
        if (r.meter->damaged && d.r.meter) {
            const float lv = r.levelDb, sn = r.snrDb; const double fq = r.freqHz;
            r = d.r; r.levelDb = lv; r.snrDb = sn; r.freqHz = fq;
        }
        const std::string core = wmbus::coreLine(*r.meter);
        const bool repeat = !fresh && core == d.core && t - d.last < 3.0;
        if (!repeat) ++d.count;
        d.r = std::move(r); d.core = core; d.last = t;
        d.line = meterLine(d);
        ++decoded_;
        prune(t);
        if (!repeat && out) { out->kind = "meter"; out->name = deviceName(d.r, 0); out->line = d.line; out->core = core; out->freqHz = d.r.freqHz; out->secret = false; }
        return !repeat;
    }
    /** The line as listed: the core, then "every 16 s" (once two frames have been heard), then "signal 24 dB". */
    static std::string meterLine(const Dev& d) {
        std::string s = d.core;
        const std::string ev = wmbus::everyText(d.mt.every());
        if (!ev.empty()) s += std::string(wmbus::kDot) + ev;
        s += std::string(wmbus::kDot) + "signal " + std::to_string(int(std::lround(std::max(0.0f, d.r.snrDb)))) + " dB";
        return s;
    }
    /** The meter's own JSON fields (§4.5). ★★ Nothing of an encrypted payload — its length in blocks, nothing else. */
    static void meterJson(std::string& o, const Dev& d) {
        const wmbus::Meter& m = *d.r.meter;
        auto str = [&](const char* k, const std::string& v) { o += ",\""; o += k; o += "\":\"" + jsonEsc(v) + "\""; };
        auto num = [&](const char* k, double v, int dp) { o += ",\""; o += k; o += "\":" + fmt1(v, dp); };
        // ★ old clients print `name · extra` when they rebuild a line (a listener on mph or kPa): the rest of the line,
        //   less "battery low" (they add it from `batt`) and the signal (they have `snr`)
        std::string extra;
        {
            char v[16]; std::snprintf(v, sizeof v, "v0x%02X", m.ver & 0xFF); extra = v;
            for (const auto& w : wmbus::payloadWords(m)) extra += std::string(wmbus::kDot) + w;
            const int sts = m.sts >= 0 ? m.sts & ~0x04 : -1;
            const std::string stx = wmbus::statusText(sts);
            if (!stx.empty()) extra += std::string(wmbus::kDot) + stx;
            if (!m.via.empty()) extra += std::string(wmbus::kDot) + "via " + m.via;
            const std::string ev = wmbus::everyText(d.mt.every());
            if (!ev.empty()) extra += std::string(wmbus::kDot) + ev;
        }
        str("extra", extra);
        str("mfr", m.mfr); str("wm", m.wm);
        num("ver", m.ver, 0); num("devType", m.type, 0);
        if (m.ellEnc) { num("sec", 99, 0); str("secText", "encrypted (AES, link layer)"); }
        else if (m.sec >= 0) { num("sec", m.sec, 0); str("secText", wmbus::secText(m.sec)); }
        if (m.encBlocks > 0) num("encBlocks", m.encBlocks, 0);
        if (m.acc >= 0) num("acc", m.acc, 0);
        if (m.sts >= 0) num("sts", m.sts, 0);
        if (m.sts >= 0 && (m.sts & 0x04)) num("batt", 0, 0);
        if (const double ev = d.mt.every(); ev > 0) num("every", ev, 0);
        if (const int rx = d.mt.rxPct(); rx >= 0) num("rx", rx, 0);
        if (!m.via.empty()) str("via", m.via);
        if (!m.note.empty()) str("note", m.note);
        if (m.authenticated) num("auth", 1, 0);
        if (m.damaged) num("damaged", 1, 0);
        if (m.sec != 0 && m.payloadLen > 0) num("encLen", m.payloadLen, 0);
        const char* lead = m.type == 0x08 ? "HCA units" : wmbus::energyType(m.type) ? "energy" : "volume";
        if (const auto* r0 = wmbus::findRec(m, lead, 0)) {
            if (r0->q == "volume") num("vol", r0->v, 3);
            else if (r0->q == "energy") num("energy", r0->u == "GJ" ? r0->v / 0.0036 : r0->v, 3);   // ★ always kWh in the JSON
        }
        if (const auto* fl = wmbus::findRec(m, "flow", 0, "m\xc2\xb3/h")) num("flow", fl->v, 3);
        if (!m.recs.empty()) {
            o += ",\"records\":[";
            for (size_t i = 0; i < m.recs.size() && i < 16; ++i) {
                const auto& r = m.recs[i];
                if (i) o += ',';
                o += "{\"q\":\"" + jsonEsc(r.q) + "\"";
                if (r.v == r.v) o += ",\"v\":" + fmt1(r.v, std::min(9, std::max(0, r.dec)));
                if (!r.u.empty()) o += ",\"u\":\"" + jsonEsc(r.u) + "\"";
                if (!r.t.empty()) o += ",\"t\":\"" + jsonEsc(r.t) + "\"";
                static const char* fn[4] = {"inst", "max", "min", "err"};
                o += ",\"st\":" + std::to_string(r.st) + ",\"fn\":\"" + fn[r.fn & 3] + "\"";
                if (r.tariff) o += ",\"tariff\":" + std::to_string(r.tariff);
                if (r.qualified) o += ",\"qual\":1";
                o += "}";
            }
            o += "]";
        }
    }
    uint64_t k0_, k1_;
    std::map<std::string, Dev> devs_;
    std::unordered_map<uint64_t, int> nickOf_;
    std::map<std::string, int> nextNick_;
    std::map<std::string, double> pending_;
    uint64_t nextId_ = 0, decoded_ = 0;
};

/** One second's measurements for the status line and the gain rule. */
struct Health { double noiseDb = -99; double clipFrac = 0; uint64_t bursts = 0, decoded = 0; bool valid = false; };

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE SERVICE
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Service {
public:
    ~Service() { stop(); }

    /** Start on `band` with the radio tuned to `hwCentreHz` at `rateHz`. `customHz` > 0: a frequency typed in ISM mode
     *  (an odd device) — the whole capture is searched and the report names that frequency. `keepList`: a band step
     *  keeps the devices already heard (and the time base, so their "last heard" stays true); entering the mode clears. */
    /** ★ V12 2026-10-25 — `sink`: who keeps the readings (the gallery's daily log and summary, vibe_ism_gallery.h). It
     *  lives exactly as long as this run: stop() joins the worker, then calls its finish() (the RAM buffer written). */
    void start(const Band& band, double hwCentreHz, double rateHz, double customHz = 0, bool keepList = false,
               std::shared_ptr<Sink> sink = nullptr) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        sink_ = std::move(sink);
        band_ = &band; hwCentre_ = hwCentreHz; rate_ = rateHz; customHz_ = customHz;
        det_.reset(new Detector(rateHz));
        det_->setDcGuardHz(15e3);
        mix_.reset(new ChanMixer(rateHz));
        // ★ V12 wM-Bus: on the 868 band (or a frequency typed inside it) the burst channel is ~500 kS/s and keeps its
        //   baseband — a T1/C1 meter's chip is 10 µs (vibe_wmbus.h). Everything else there slices as before (µs smoothing).
        meters_ = wantsMeters(band, customHz);
        keepBb_ = meters_;
        if (meters_) mix_->setChanRate(500e3);
        det_->setChanRate(mix_->chanRate());   // ★ how far a pending channel's mix centre may move (vibe_ism.h DChan::mixHz)
        // ★ detect inside the band's own span (+ a margin for drifting sensors), not on the tuner's skirts
        if (customHz <= 0) {
            const double mid = band.centreHz - hwCentreHz, half = band.needHz + 40e3;
            det_->setUsableHz(mid - half, mid + half);
        }
        if (!keepList) { t0_ = std::chrono::steady_clock::now(); tracker_.clear(); bursts_ = 0; unknown_ = 0; wm_ = wmbus::Stats{}; }
        fobs_.clear();   // ★ a new front end restarts burst time at 0
        { std::lock_guard<std::mutex> q(qMtx_); queue_.clear(); queued_ = 0; dropped_ = 0; cap_.reset(rateHz, now()); }
        { std::lock_guard<std::mutex> m(mMtx_); mixQ_.clear(); mixQueued_ = 0; droppedMix_ = 0; mixCap_ = size_t(det_->workRate() * 0.5); }
        /* ★★ THE SLICER'S QUEUE HOLDS A WHOLE SET OF THE LONGEST CHANNELS, and a second more. Four channels can close in
         *  the same block, each up to kMaxBurstS long; at 2 s of channel samples (the first cut) two of them already
         *  overflowed it, and the hog-heavy bench (tools/ism-bench/add-hogs.cpp) on the Pi 2 dropped 3. That much baseband
         *  was already in memory while those channels were open, before the split as well: this only bounds how long it
         *  may wait. 250 kS/s: ~15 MB at most; the 868 band's ~500 kS/s channel: ~28 MB — and only while behind. */
        { std::lock_guard<std::mutex> d(dMtx_); decQ_.clear(); decQueued_ = 0; droppedDec_ = 0;
          decCap_ = size_t((FeConst::kMaxChans * FeConst::kMaxBurstS + 1.0) * mix_->chanRate()); }
        noiseDb_.store(-99, std::memory_order_relaxed); clipFrac_.store(0, std::memory_order_relaxed); carriers_.store(0, std::memory_order_relaxed);
        health_ = Health{};
        // ★ V12 LoRa (vibe_lora_service.h): its own thread and queue; the band's channel plan, only what the capture
        //   holds, as many detectors as the owner's choice and the benchmark allow. Not on a typed frequency.
        if (!keepList) mesh_.clear();
        loraChans_ = 0;
        if (loraOn_ && customHz <= 0) loraChans_ = lora_.start(band.id, hwCentreHz, rateHz, loraBudget_);
        run_ = true;
        detTh_ = std::thread([this] { detLoop(); });
        mixTh_ = std::thread([this] { mixLoop(); });
        decTh_ = std::thread([this] { decLoop(); });
    }
    /** ★ V12 LoRa: on/off (the owner's switch) and the detector budget in vibelora::detCost units (∞ = every plan
     *  channel the capture holds; the default tier always runs while LoRa is on). Takes effect at the next start(). */
    void setLora(bool on, double budgetUnits) { std::lock_guard<std::mutex> lk(mtx_); loraOn_ = on; loraBudget_ = budgetUnits; }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); if (!run_) return; run_ = false; }
        { std::lock_guard<std::mutex> q(qMtx_); } qCv_.notify_all(); qSpace_.notify_all();
        { std::lock_guard<std::mutex> m(mMtx_); } mCv_.notify_all(); mSpace_.notify_all();
        { std::lock_guard<std::mutex> d(dMtx_); } dCv_.notify_all(); dSpace_.notify_all();
        if (detTh_.joinable()) detTh_.join();
        if (mixTh_.joinable()) mixTh_.join();
        if (decTh_.joinable()) decTh_.join();
        lora_.stop();
        // ★ The worker is gone, so nothing else touches the sink: its buffer is written here, once.
        std::shared_ptr<Sink> s;
        { std::lock_guard<std::mutex> lk(mtx_); s.swap(sink_); }
        if (s) s->finish();
    }
    /** ★ See Tracker::reserveNicks. Before start(), from the day's summary card. */
    void reserveNicks(const std::string& kind, int floor) { std::lock_guard<std::mutex> lk(mtx_); tracker_.reserveNicks(kind, floor); }
    bool running() const { return run_.load(std::memory_order_relaxed); }
    /** Work dropped because a stage was behind, since start — the health pill's "too heavy" (vibe_health.h BehindWatch):
     *  capture blocks the detector lost, work blocks the mixer lost and closed channels the slicer lost, together. */
    uint64_t droppedBlocks() {
        uint64_t n = 0;
        { std::lock_guard<std::mutex> q(qMtx_); n += dropped_; }
        { std::lock_guard<std::mutex> m(mMtx_); n += droppedMix_; }
        { std::lock_guard<std::mutex> d(dMtx_); n += droppedDec_; }
        return n;
    }
    /** ★ For the tests and ism-offline, BEFORE start(): a full queue WAITS instead of dropping (a recording replayed
     *  faster than real time must not lose work), and `tap` sees every sliced burst with what the decoders made of it,
     *  on the slicer's thread. Never set by the server. */
    void setLossless(bool on) { lossless_ = on; lora_.setLossless(on); }
    void setTap(std::function<void(const Burst&, const std::vector<Reading>&)> tap) { tap_ = std::move(tap); }
    /** ★ For the tests and ism-offline: the end of a recording — every open channel closes, as FrontEnd::flush closes
     *  them inline (a live server never calls it: its stream has no end, and stop() drops what is open, as before). */
    void endOfStream() {
        { std::lock_guard<std::mutex> lk(qMtx_); queue_.push_back(std::vector<cf>()); }   // ★ an empty block is the marker
        qCv_.notify_one();
    }
    /** ★ For the tests: true when every stage's queue is empty and nothing is being worked on. */
    bool idle() {
        { std::lock_guard<std::mutex> q(qMtx_); if (!queue_.empty() || detBusy_) return false; }
        { std::lock_guard<std::mutex> m(mMtx_); if (!mixQ_.empty() || mixBusy_) return false; }
        { std::lock_guard<std::mutex> d(dMtx_); if (!decQ_.empty() || decBusy_) return false; }
        return !lora_.running() || lora_.idle();
    }
    const Band* band() const { return band_; }

    /** From the DSP thread: copy and return. Never blocks on the decoder. */
    void feed(const cf* iq, size_t n) {
        if (!run_.load(std::memory_order_relaxed) || !n) return;
        lora_.feed(iq, n);   // ★ its own copy and queue — returns at once
        std::vector<cf> b(iq, iq + n);
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            if (lossless_) qSpace_.wait(lk, [&] { return !run_ || queue_.empty() || queued_ + n <= size_t(rate_ * 0.5); });
            cap_.add(n, now());
            queued_ += n;
            queue_.push_back(std::move(b));
            while (queued_ > size_t(rate_ * 0.5) && queue_.size() > 1) { queued_ -= queue_.front().size(); queue_.pop_front(); ++dropped_; }
        }
        qCv_.notify_one();
    }

    /** {"type":"ism", "band":"433", "bandName":…, "centre":Hz, "rate":…, "devices":[…], …} */
    std::string report() {
        std::lock_guard<std::mutex> lk(mtx_);
        const double t = now();
        std::string o = "{\"type\":\"ism\"";
        if (band_) o += std::string(",\"band\":\"") + band_->id + "\",\"bandName\":\"" + jsonEsc(band_->name) + "\"";
        char b[400];
        uint64_t dq = 0, dm = 0, dd = 0; int capPct = 0;
        { std::lock_guard<std::mutex> q(qMtx_); dq = dropped_; capPct = cap_.percent(t); }
        { std::lock_guard<std::mutex> m(mMtx_); dm = droppedMix_; }
        { std::lock_guard<std::mutex> d(dMtx_); dd = droppedDec_; }
        // ★ "dropped": everything any stage lost (as before, the health pill's figure); "drops": by stage — capture blocks
        //   before the detector, work blocks before the mixer, closed channels before the slicer. "carriers": frequencies
        //   held right now because something there keeps a channel busy for good (a carrier, a spur — vibe_ism.h)
        std::snprintf(b, sizeof b, ",\"centre\":%.0f,\"custom\":%d,\"rate\":%.0f,\"now\":%.1f,\"bursts\":%llu,\"decoded\":%llu,"
                      "\"unknown\":%llu,\"noise\":%.1f,\"dropped\":%llu,\"drops\":[%llu,%llu,%llu],\"carriers\":%d,\"capture\":%d,\"devices\":",
                      customHz_ > 0 ? customHz_ : (band_ ? band_->centreHz : 0), customHz_ > 0 ? 1 : 0, rate_, t,
                      (unsigned long long)bursts_, (unsigned long long)tracker_.decoded(), (unsigned long long)unknown_,
                      health_.noiseDb, (unsigned long long)(dq + dm + dd), (unsigned long long)dq, (unsigned long long)dm,
                      (unsigned long long)dd, carriers_.load(std::memory_order_relaxed), capPct);
        o += b;
        const auto loraRows = mesh_.rowsJson(t);
        o += tracker_.json(t, &loraRows);
        // ★ V12 LoRa: how many plan channels the capture holds and run, what was heard, and the public messages
        if (lora_.running()) {
            uint64_t ok = 0, bh = 0, bc = 0;
            lora_.counts(ok, bh, bc);
            std::snprintf(b, sizeof b, ",\"lora\":{\"chans\":%zu,\"plan\":%zu,\"dets\":%zu,\"frames\":%llu,\"badCrc\":%llu,\"texts\":%llu,"
                          "\"mapped\":%zu,\"dropped\":%llu}", loraChans_, lora_.planSize(), lora_.detectors(), (unsigned long long)ok,
                          (unsigned long long)bc, (unsigned long long)mesh_.texts(), mesh_.nodesWithPosition(), (unsigned long long)lora_.dropped());
            o += b;
            o += ",\"msgs\":" + mesh_.msgsJson(t);
        }
        // ★ V12 wM-Bus: frames whose later block failed its CRC (the meter is still listed), and the capture too narrow
        //   to hold 868.95's upper tone (0.9 MS/s): the clients say "868.95 not covered at 0.9 MS/s"
        if (meters_) {
            std::snprintf(b, sizeof b, ",\"meters\":%zu,\"damaged\":%llu,\"collector\":%llu%s", tracker_.meterCount(),
                          (unsigned long long)wm_.damaged, (unsigned long long)wm_.collector, wmGap() ? ",\"wmGap\":1" : "");
            o += b;
        }
        return o + "}";
    }
    /** ★ The 868 band, or a typed frequency inside 863-870 MHz: meters are looked for there. */
    static bool wantsMeters(const Band& band, double customHz) {
        if (customHz > 0) return customHz >= 863e6 && customHz <= 870e6;
        return std::string(band.id) == "868";
    }
    /** True when the capture cannot hold a T1/C1 meter's upper tone (868.95 + 50 kHz + 60 ppm ≈ 869.05 MHz): usable
     *  ±45 % of the rate around where the radio is tuned. At 0.9 MS/s around 868.6 it reaches 869.005. */
    bool wmGap() const { return meters_ && hwCentre_ + 0.45 * rate_ < 869.05e6; }
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }
    size_t deviceCount() { std::lock_guard<std::mutex> lk(mtx_); return tracker_.size() + mesh_.size(); }

    /** ★★ ONE FRAME, TWO CHANNELS. A T1/C1 burst is ~200 kHz wide, and the detector can open a second channel on its
     *  skirt; both may decode the same frame. Of the copies of one meter's frame heard within 0.2 s, keep the one best
     *  centred in its channel (its frequency is the true one — off-centre, the channel filter pulls the estimate). */
    static void dedupeMeters(std::vector<Reading>& v, const std::vector<double>& tOf) {
        std::vector<bool> drop(v.size(), false);
        for (size_t i = 0; i < v.size(); ++i) {
            if (drop[i] || v[i].kind != "meter" || !v[i].meter) continue;
            for (size_t j = i + 1; j < v.size(); ++j) {
                if (drop[j] || v[j].kind != "meter" || !v[j].meter) continue;
                const wmbus::Meter &a = *v[i].meter, &b = *v[j].meter;
                if (a.id != b.id || a.mfr != b.mfr || a.type != b.type || std::fabs(tOf[i] - tOf[j]) > 0.2) continue;
                if (std::fabs(b.carrierHz) < std::fabs(a.carrierHz)) { drop[i] = true; break; } else drop[j] = true;
            }
        }
        size_t k = 0;
        for (size_t i = 0; i < v.size(); ++i) if (!drop[i]) { if (k != i) v[k] = std::move(v[i]); ++k; }
        v.resize(k);
    }

    /** ★ For tests: run the decoder chain on one burst exactly as the worker does (no thread, no queue). */
    static void decodeBurst(const Burst& b, double hwCentreHz, std::vector<Reading>& out, wmbus::Stats* wm = nullptr) {
        const size_t first = out.size();
        /* ★★ V12 wM-Bus FIRST on a burst that carries its baseband (the 868 band): a 16-bit CRC per block is a far
         *  stronger check than most families', and it only looks at bursts at 868.30 / 868.95. A meter ends the search;
         *  a collector's frame (a command to a meter) is counted and listed as nothing. */
        if (b.bb) {
            wmbus::Meter m;
            const int k = wmbus::decodeBurst(b, hwCentreHz, m, wm);
            if (k == 2) return;
            if (k == 1) {
                Reading r;
                r.kind = "meter";
                const char* mk = wmbus::mfrName(m.mfr);
                r.make = mk ? mk : m.mfr;
                r.model = wmbus::typeText(m.type);
                r.pubId = m.id;                          // ★ the serial is shown (Stuart, 2026-10-25)
                if (m.sts >= 0 && (m.sts & 0x04)) r.battOk = 0;
                r.freqHz = hwCentreHz + b.offHz + m.carrierHz; r.levelDb = b.levelDb; r.snrDb = b.levelDb - b.noiseDb;
                r.meter = std::make_shared<const wmbus::Meter>(std::move(m));
                if (bandPlausible(r, r.freqHz)) out.push_back(std::move(r));
                return;
            }
        }
        decodeAll(b, out);
        for (size_t i = first; i < out.size(); ++i) {
            Reading& r = out[i];
            r.freqHz = hwCentreHz + b.offHz + b.carrierHz; r.levelDb = b.levelDb; r.snrDb = b.levelDb - b.noiseDb;
        }
        // ★ a family heard where it never transmits is noise (bandPlausible) — the burst then counts as unidentified
        out.erase(std::remove_if(out.begin() + long(first), out.end(), [](const Reading& r) { return !bandPlausible(r, r.freqHz); }), out.end());
    }

private:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

    /** One block of work for the mixer: the detector's events and the work samples they refer to (the capture block
     *  itself, moved, unless the capture is pre-decimated). */
    struct MixJob { uint64_t start = 0; std::vector<cf> x; std::vector<ChanEvent> ev; };

    // ── STAGE 1: vibe-ism, the detector. Keeps real time; never waits on the stages after it. ──
    void detLoop() {
        vibeDecoderThread("vibe-ism");
        std::unique_lock<std::mutex> lk(qMtx_);
        while (run_) {
            qCv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !queue_.empty(); });
            while (run_ && !queue_.empty()) {
                std::vector<cf> blk = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= blk.size();
                detBusy_ = true;
                lk.unlock();
                qSpace_.notify_all();
                MixJob j;
                j.start = det_->total();
                const bool pre = det_->pre() > 1;
                if (blk.empty()) det_->flush(j.ev);   // ★ endOfStream()
                else {
                    if (pre) j.x.reserve(blk.size() / size_t(det_->pre()) + 1);
                    det_->feed(blk.data(), blk.size(), j.ev, pre ? &j.x : nullptr);
                    if (!pre) j.x = std::move(blk);
                }
                noiseDb_.store(det_->noiseDb(), std::memory_order_relaxed);
                clipFrac_.store(det_->clipFrac(), std::memory_order_relaxed);
                carriers_.store(det_->holds(), std::memory_order_relaxed);
                pushMix(std::move(j));
                lk.lock();
                detBusy_ = false;
            }
        }
    }
    void pushMix(MixJob&& j) {
        {
            std::unique_lock<std::mutex> lk(mMtx_);
            if (lossless_) mSpace_.wait(lk, [&] { return !run_ || mixQ_.empty() || mixQueued_ + j.x.size() <= mixCap_; });
            mixQueued_ += j.x.size();
            mixQ_.push_back(std::move(j));
            // ★ behind by more than half a second: the OLDEST work goes (the mixer abandons the channels open across it)
            while (mixQueued_ > mixCap_ && mixQ_.size() > 1) { mixQueued_ -= mixQ_.front().x.size(); mixQ_.pop_front(); ++droppedMix_; }
        }
        mCv_.notify_one();
    }

    // ── STAGE 2: vibe-ism2, the channel mixer. May lag; drops (above) rather than slowing the detector. ──
    void mixLoop() {
        vibeDecoderLagThread("vibe-ism2");
        std::unique_lock<std::mutex> lk(mMtx_);
        while (run_) {
            mCv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !run_ || !mixQ_.empty(); });
            while (run_ && !mixQ_.empty()) {
                MixJob j = std::move(mixQ_.front());
                mixQ_.pop_front();
                mixQueued_ -= j.x.size();
                mixBusy_ = true;
                lk.unlock();
                mSpace_.notify_all();
                std::vector<RawBurst> raws;
                mix_->feed(j.x.data(), j.x.size(), j.start, j.ev.data(), j.ev.size(), [&](RawBurst&& r) { raws.push_back(std::move(r)); });
                if (!raws.empty()) pushDec(std::move(raws));
                lk.lock();
                mixBusy_ = false;
            }
        }
    }
    static size_t samplesOf(const std::vector<RawBurst>& v) { size_t n = 0; for (const auto& r : v) n += r.bb.size(); return n; }
    /** ★ One batch = the channels that closed in one capture block, kept together as the old single thread saw them
     *  (dedupeMeters looks for one meter frame's two copies within a batch). */
    void pushDec(std::vector<RawBurst>&& raws) {
        const size_t n = samplesOf(raws);
        {
            std::unique_lock<std::mutex> lk(dMtx_);
            if (lossless_) dSpace_.wait(lk, [&] { return !run_ || decQ_.empty() || decQueued_ + n <= decCap_; });
            decQueued_ += n;
            decQ_.push_back(std::move(raws));
            // ★ behind by more than four of the longest channels and a second: the OLDEST closed channels go unsliced
            while (decQueued_ > decCap_ && decQ_.size() > 1) { decQueued_ -= samplesOf(decQ_.front()); droppedDec_ += decQ_.front().size(); decQ_.pop_front(); }
        }
        dCv_.notify_one();
    }

    // ── STAGE 3: vibe-ism3, the slicer and the protocol decoders, then the device list, the gallery and LoRa's news. ──
    void decLoop() {
        vibeDecoderLagThread("vibe-ism3");
        double winStart = 0;
        std::unique_lock<std::mutex> dl(dMtx_);
        while (run_) {
            dCv_.wait_for(dl, std::chrono::milliseconds(250), [this] { return !run_ || !decQ_.empty(); });
            while (run_ && !decQ_.empty()) {
                std::vector<RawBurst> raws = std::move(decQ_.front());
                decQ_.pop_front();
                decQueued_ -= samplesOf(raws);
                decBusy_ = true;
                dl.unlock();
                dSpace_.notify_all();
                decodeBatch(raws);
                dl.lock();
                decBusy_ = false;
            }
            dl.unlock();
            housekeeping(winStart);
            dl.lock();
        }
    }
    void decodeBatch(std::vector<RawBurst>& raws) {
        const double hc = hwCentre_;
        std::vector<Burst> bs;
        for (auto& r : raws) { Burst b; if (FrontEnd::finish(std::move(r), keepBb_, b)) bs.push_back(std::move(b)); }
        if (bs.empty()) return;
        std::vector<Reading> got;
        wmbus::Stats wmLocal;
        std::vector<double> tOf;
        for (const Burst& b : bs) {
            size_t before = got.size(); decodeBurst(b, hc, got, &wmLocal);
            tOf.resize(got.size(), b.tSec);
            if (tap_) tap_(b, std::vector<Reading>(got.begin() + long(before), got.end()));
            if (got.size() != before) continue;
            ++unknownLocal_;
            Reading sf;
            if (fobs_.offer(b, hc, sf)) { got.push_back(sf); tOf.push_back(b.tSec); }   // ★ an undecoded press: "Suspected key fob"
        }
        dedupeMeters(got, tOf);
        std::unique_lock<std::mutex> lk(mtx_);
        for (double hz : fobs_.takePeriodic()) tracker_.forget("suspectfob", hz, 50e3);
        bursts_ += bs.size();
        wm_.tried += wmLocal.tried; wm_.frames += wmLocal.frames; wm_.damaged += wmLocal.damaged; wm_.collector += wmLocal.collector;
        unknownLocal_ -= std::min<uint64_t>(unknownLocal_, wmLocal.collector);   // ★ a collector's frame was identified
        unknown_ += unknownLocal_; unknownLocal_ = 0;
        const double t = now();
        std::vector<Heard> heard;
        for (auto& r : got) { Heard h; if (tracker_.accept(r, t, sink_ ? &h : nullptr) && sink_) heard.push_back(std::move(h)); }
        // ★ The gallery's copy OUTSIDE the lock: a report() asked for meanwhile never waits on a file.
        if (!heard.empty()) { auto s = sink_; lk.unlock(); for (const auto& h : heard) s->heard(h); }
    }
    void housekeeping(double& winStart) {
        std::unique_lock<std::mutex> lk(mtx_);
        // ★ V12 LoRa: what the LoRa thread decoded since the last look — into the list, and news to the gallery
        if (lora_.running()) {
            lk.unlock();
            auto lh = lora_.take();
            lk.lock();
            if (!lh.empty()) {
                const double tl = now();
                std::vector<vibelora::Event> ev;
                std::vector<vibelora::Fix> fx;
                for (const auto& h : lh) mesh_.accept(h.m, tl, sink_ ? &ev : nullptr, sink_ ? &fx : nullptr);
                if ((!ev.empty() || !fx.empty()) && sink_) {
                    auto s = sink_; lk.unlock();
                    for (const auto& e : ev) { Heard hd; hd.kind = e.kind; hd.name = e.name; hd.line = e.line; hd.freqHz = e.freqHz; s->heard(hd); }
                    // ★ V12: positioned mesh nodes — the gallery's "stations received" card (vibe_ism_gallery.h NodeCard)
                    for (const auto& f : fx) {
                        NodeFix n; n.net = f.net; n.type = f.type; n.name = f.name; n.node = f.node; n.lat = f.lat; n.lon = f.lon;
                        n.approx = f.approx; n.hops = f.hops; n.snr = f.snr; n.freqHz = f.freqHz; s->node(n);
                    }
                    lk.lock();
                }
            }
        }
        const double t = now();
        if (t - winStart >= 1.0 && sink_) { auto s = sink_; lk.unlock(); s->tick(); lk.lock(); }
        if (t - winStart >= 1.0) {
            winStart = t;
            health_.noiseDb = noiseDb_.load(std::memory_order_relaxed);
            health_.clipFrac = clipFrac_.load(std::memory_order_relaxed);
            health_.bursts = bursts_;
            health_.decoded = tracker_.decoded();
            health_.valid = true;
            tracker_.prune(t);
        }
    }

    std::mutex mtx_;                 ///< the device list, the counts, the health, the sink — and start/stop
    std::atomic<bool> run_{false};
    const Band* band_ = nullptr;
    double hwCentre_ = 0, rate_ = 0, customHz_ = 0;
    std::unique_ptr<Detector> det_;  ///< vibe-ism's alone while running
    std::unique_ptr<ChanMixer> mix_; ///< vibe-ism2's alone while running
    bool keepBb_ = false;
    std::atomic<double> noiseDb_{-99}, clipFrac_{0};   ///< the detector's, for the health (read by vibe-ism3)
    std::atomic<int> carriers_{0};                       ///< the detector's held hogs right now (vibe_ism.h kHoldQuietS)
    std::thread detTh_, mixTh_, decTh_;
    bool lossless_ = false;
    std::function<void(const Burst&, const std::vector<Reading>&)> tap_;
    // ★ before the detector: capture blocks (the DSP thread's copies)
    std::mutex qMtx_;
    std::condition_variable qCv_, qSpace_;
    std::deque<std::vector<cf>> queue_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0;
    bool detBusy_ = false;
    vibeadsb::CaptureMeter cap_;
    // ★ before the mixer: the detector's events with their work samples
    std::mutex mMtx_;
    std::condition_variable mCv_, mSpace_;
    std::deque<MixJob> mixQ_;
    size_t mixQueued_ = 0, mixCap_ = 0;
    uint64_t droppedMix_ = 0;
    bool mixBusy_ = false;
    // ★ before the slicer: closed channels, one batch per capture block
    std::mutex dMtx_;
    std::condition_variable dCv_, dSpace_;
    std::deque<std::vector<RawBurst>> decQ_;
    size_t decQueued_ = 0, decCap_ = 0;
    uint64_t droppedDec_ = 0;
    bool decBusy_ = false;
    uint64_t bursts_ = 0, unknown_ = 0, unknownLocal_ = 0;
    std::chrono::steady_clock::time_point t0_;
    Tracker tracker_;
    FobWatch fobs_;
    Health health_;
    bool meters_ = false;
    wmbus::Stats wm_;
    std::shared_ptr<Sink> sink_;   // ★ only vibe-ism3 reads it while running; start()/stop() swap it with the threads stopped
    // ★ V12 LoRa
    vibelora::Engine lora_;
    vibelora::MeshTable mesh_;
    bool loraOn_ = true;
    double loraBudget_ = INFINITY;
    size_t loraChans_ = 0;
};

/** ★ ISM's gain rule. Sensors are short strong bursts over a quiet band, like ADS-B, so it is not VibeAGC: start
 *  60 % up the radio's OWN gain list (any radio — the RTL's table, the RSP's 0–49 dB, the Airspy's list) and come
 *  down only when bursts clip; climb back one rung after two quiet minutes, never past the start. */
struct GainRule {
    double clipMax = 0.02;     ///< share of detector frames near full scale before we step down
    int decide(const Health& h, int idx, int startIdx, double quietFor) const {
        if (!h.valid) return 0;
        if (h.clipFrac > clipMax && idx > 0) return -1;
        if (h.clipFrac == 0 && idx < startIdx && quietFor > 120) return +1;
        return 0;
    }
};

} // namespace vibeism
