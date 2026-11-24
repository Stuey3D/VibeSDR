// vibe_orbcomm_run.h — ORBCOMM as a listener's decoder (V12, 2026-11-24): the DecoderHost's "orbcomm" extension.
//
// ★★★ WHERE IT SITS (the LRPT pattern, vibe_lrpt_run.h): the host asks the DSP thread for an IQ SLICE at the listener's
//     dial — one listed channel ± Doppler (40 kHz) or the whole listed band (≤ 640 kHz) — and hands each block to this on
//     its own decode thread (Network > Audio > Spectrum > Decoders). Every ORBCOMM carrier in the slice is decoded
//     (vibe_orbcomm.h Receiver, ≤ maxChans), so a band pick hears the other birds too; each carrier is named by the
//     satellite's own position report (vibe_orbcomm_id.h).
//
// ★★ WHAT CLIENTS SEE:
//    · {"type":"orbcomm_status","data":{…}} once a second — the birds being decoded (name, how it was named, carrier
//      frequency, SNR, measured vs predicted Doppler, az/el, packet counts, the latest housekeeping), the carriers, the
//      pick, the counts by packet type, and the sky dots added since the last status (the live sky plot).
//    · {"type":"orbcomm_pkt","data":{…}} for each packet worth a line (never fill or sync; housekeeping only when it
//      CHANGES, at most once a minute when not; ≤ kMaxPktsPerSec) — and the same as a text line in the decoder box.
//    · SUBSCRIBER MESSAGES (type 1A) are NEVER decoded: each is shown as "subscriber message — not decoded" with its raw
//      bytes in hex (Stuart: "encrypted messages not decoded but we can show the raw data").
// ★★ WHAT IS KEPT: sky dots (vibe_skyplot.h, one per satellite per 5 s, the machine's one file, tagged with this radio);
//    at the pass's end (the pick sets) or a stop, this radio's ORBCOMM sky plot as a PNG for the gallery.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "vibe_orbcomm_id.h"
#include "vibe_skyplot.h"

namespace vibeorb {

class Run {
public:
    static constexpr int kMaxPktsPerSec = 12;
    static constexpr double kRepeatSec = 60;     ///< an unchanged housekeeping packet is re-shown at most this often
    static constexpr double kBirdsEverySec = 600;

    struct Opts {
        double widthHz = 40000;                                         ///< the slice asked for (the carrier window)
        int maxChans = 0;                                               ///< 0 = AUTO (4), else 1 … kMaxChans
        std::function<void(const std::string&)> json;
        std::function<void(const std::string&)> text;                   ///< a line for the decoder box (and the gallery's log)
        std::function<std::vector<Bird>()> birds;                       ///< ORBCOMM birds with elements
        std::function<bool(vibe::sat::Observer&)> observer;             ///< the station's location (false = none)
        std::function<long long()> pickNorad;                           ///< the SATELLITE pick's bird (0 = none / not ORBCOMM)
        std::function<int()> satInRange;                                ///< 1 up · 0 not · −1 no pick
        std::function<double()> now;                                    ///< UTC seconds (test seam); null = the system clock
        std::function<void(const std::vector<vibe::skyplot::Dot>&)> dots;   ///< the sky plot's store
        std::function<std::string(std::string& title)> skyPng;           ///< this radio's ORBCOMM plot as PNG (pass end)
        std::function<void(const std::string&, const std::string&, int, int, bool)> picture;
        std::function<void(const std::string&)> log;
    };
    explicit Run(Opts o) : o_(std::move(o)) {
        Receiver::Cfg c;
        c.maxChans = o_.maxChans > 0 ? std::min(kMaxChans, o_.maxChans) : 4;
        c.windowHz = std::max(10000.0, o_.widthHz / 2);
        rx_ = Receiver(c);
        rx_.onPacket = [this](const Rx& r) { onRx_(r); };
    }
    ~Run() { finish(true); }

    void setMaxChans(int n) { rx_.setMaxChans(n > 0 ? n : 4); }

    /** One IQ block from the host (decode thread). */
    void feedIq(const std::complex<float>* iq, int n, double rate, double centreHz) {
        if (n <= 0 || !(rate > 0)) return;
        const double t = now_();
        if (t - birdsAt_ > kBirdsEverySec || birdsAt_ == 0) refreshBirds_(t);
        /* ★★ THE LOG SAYS WHETHER IQ ARRIVES (the XCover's HF+, 2026-11-24: "0 good, no carrier" and no way to tell a slice that
         *  never came from a sky with nothing in it): one line at the first block, one a minute after. */
        if (!firstLogged_ && o_.log) {
            firstLogged_ = true;
            char b[200];
            std::snprintf(b, sizeof b, "ORBCOMM: IQ arriving \xe2\x80\x94 a %.0f kS/s slice centred %.4f MHz (searching %.4f\xe2\x80\x93%.4f MHz), %d birds with elements",
                          rate / 1000, centreHz / 1e6, (centreHz - std::min(o_.widthHz / 2, 0.45 * rate)) / 1e6, (centreHz + std::min(o_.widthHz / 2, 0.45 * rate)) / 1e6,
                          int(id_.birds().size()));
            o_.log(b);
        }
        rx_.feed(iq, n, rate, centreHz, t - n / rate);
        if (t - statusAt_ >= 1.0) { statusAt_ = t; tick_(t); }
    }
    /** The pass is over (or the decoder stops): flush the dots, the gallery's sky plot. */
    void finish(bool partial) {
        if (finished_) return;
        finished_ = true;
        flushDots_(1e18);
        passEnd_(partial);
    }
    // ── for tests ──
    uint64_t good() const { return good_; }
    uint64_t messages() const { return msgs_; }
    const std::map<long long, int>& goodBySat() const { return goodBySat_; }
private:
    struct Sat {
        std::string name, how; long long norad = 0;
        int chan = 0, dcn = 0, scId = -1;
        double rfHz = 0, snr = 0, dop = 0, pred = 0, az = 0, el = -90, last = 0;
        uint64_t ok = 0, msgs = 0;
        std::string eph, down, up, elements, ncc;   // the latest housekeeping, as JSON fragments
    };
    struct Bucket { double t0 = 0; int snr = -127; int n = 0; float az = 0, el = 0; };

    double now_() const {
        if (o_.now) return o_.now();
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void refreshBirds_(double t) {
        birdsAt_ = t;
        if (o_.birds) id_.setBirds(o_.birds());
        vibe::sat::Observer ob;
        if (o_.observer && o_.observer(ob)) id_.setObserver(ob);
    }
    static std::string jsonEsc(const std::string& s) { return vibe::logradio::jsonEsc(s); }
    static std::string fmt(const char* f, double a) { char b[64]; std::snprintf(b, sizeof b, f, a); return b; }
    static std::string chanList(const std::vector<int>& v, bool up) {
        std::string s;
        for (int c : v) { if (!s.empty()) s += ", "; s += fmt("%.4f", (up ? uplinkHz(c) : downlinkHz(c)) / 1e6); }
        return s;
    }
    static std::string chanJson(const std::vector<int>& v, bool up) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); i++) { if (i) s += ','; s += fmt("%.0f", up ? uplinkHz(v[i]) : downlinkHz(v[i])); }
        return s + "]";
    }

    void onRx_(const Rx& r) {
        if (!r.p.ok) { bad_++; return; }
        good_++;
        typeCount_[r.p.type()]++;
        const Ident::Answer a = id_.onPacket(r, r.t);
        const long long key = a.norad ? a.norad : -(long long)r.chan;     // an unnamed carrier keys by its number
        Sat& s = sats_[key];
        s.norad = a.norad; s.name = a.norad ? a.name : "ORBCOMM (not yet named)"; s.how = a.how;
        s.chan = r.chan; s.rfHz = r.rfHz; s.snr = r.snrDb; s.last = r.t; s.ok++;
        s.dcn = id_.channelOf(r.chan); s.scId = id_.scIdOf(r.chan);
        if (a.sky) { s.az = a.azDeg; s.el = a.elDeg; s.dop = a.dopplerHz; s.pred = a.predictedHz; }
        if (a.norad) { goodBySat_[a.norad]++; seenThisPass_[a.norad] = true; }
        if (a.norad && a.sky) addDot_(a.norad, r.t, int(std::lround(r.snrDb)), float(a.azDeg), float(a.elDeg));
        // the packet's line
        const uint8_t ty = r.p.type();
        if (ty == kFill || ty == kSync) return;
        std::string what, vals = "{}";
        bool housekeeping = true;
        if (ty == kEphemeris) {
            Ephemeris e; decodeEphemeris(r.p, e, r.t);
            char b[256];
            std::snprintf(b, sizeof b, "position %.2f%s %.2f%s, %.0f km up, %.2f km/s; clock %s", std::fabs(e.latDeg), e.latDeg >= 0 ? "N" : "S",
                          std::fabs(e.lonDeg), e.lonDeg >= 0 ? "E" : "W", e.altKm, e.speedKmS, clockText_(e.unix, r.t).c_str());
            what = b;
            std::snprintf(b, sizeof b, "{\"lat\":%.3f,\"lon\":%.3f,\"altKm\":%.1f,\"kmS\":%.3f,\"clock\":%.0f,\"id\":%d,\"week\":%d,\"tow\":%d}",
                          e.latDeg, e.lonDeg, e.altKm, e.speedKmS, e.unix, e.id, e.week, e.tow);
            vals = b; s.eph = vals;
            // ★ a position changes every report: not a line each time (every 4 s) — once a minute
            if (r.t - lastEph_[key] < kRepeatSec) return;
            lastEph_[key] = r.t;
        } else if (ty == kDownlink) {
            DownlinkInfo d; decodeDownlink(r.p, d);
            what = "downlink channels (" + std::to_string(d.index + 1) + "/" + std::to_string(std::max(1, d.count)) + "): " + chanList(d.channels, false) + " MHz";
            vals = "{\"part\":" + std::to_string(d.index + 1) + ",\"of\":" + std::to_string(d.count) + ",\"hz\":" + chanJson(d.channels, false) + "}";
            s.down = vals;
        } else if (ty == kUplink) {
            UplinkInfo u; decodeUplink(r.p, u);
            what = "uplink channels " + chanList(u.channels, true) + " MHz; " + std::to_string(u.retries) + " retries, " + std::to_string(u.slots) + " access slots";
            vals = "{\"hz\":" + chanJson(u.channels, true) + ",\"retries\":" + std::to_string(u.retries) + ",\"slots\":" + std::to_string(u.slots) + "}";
            s.up = vals;
        } else if (ty == kElements) {
            ElementsInfo e; decodeElements(r.p, e);
            what = "orbital elements for spacecraft " + std::to_string(e.sc) + ": mean anomaly " + fmt("%.3f", e.meanAnomalyDeg) + "°, mean motion " + fmt("%.6f", e.meanMotionRevDay) + " rev/day";
            vals = "{\"sc\":" + std::to_string(e.sc) + ",\"ma\":" + fmt("%.4f", e.meanAnomalyDeg) + ",\"mm\":" + fmt("%.8f", e.meanMotionRevDay) + "}";
            s.elements = vals;
        } else if (ty == kNcc) {
            what = "network control centre (gateway) information — fields not published";
            s.ncc = "\"" + r.p.hex() + "\"";
        } else if (ty == kMessage) {
            housekeeping = false;
            msgs_++; s.msgs++;
            what = "subscriber message \xe2\x80\x94 not decoded (part " + std::to_string((r.p.b[1] & 15) + 1) + " of " + std::to_string(std::max(1, r.p.b[1] >> 4)) + ")";
        } else {
            /* ★ FM116 2026-11-24: types 0A 0B 0D 0E 13 (≈ 7 % of good packets, most of them clean) are in no published
             *  description — shown raw, deduplicated like housekeeping, never interpreted. */
            what = "system packet type " + fmt("%02X", ty) + " — not published, shown raw";
            housekeeping = true;
        }
        if (housekeeping && ty != kEphemeris) {   // unchanged housekeeping: once a minute
            const std::string k = std::to_string(key) + ":" + r.p.hex();
            auto it = lastSame_.find(k);
            if (it != lastSame_.end() && r.t - it->second < kRepeatSec) return;
            lastSame_[k] = r.t;
            if (lastSame_.size() > 512) lastSame_.clear();
        }
        if (r.t - pktSecAt_ >= 1.0) { pktSecAt_ = r.t; pktSec_ = 0; }
        if (++pktSec_ > kMaxPktsPerSec) { dropped_++; return; }
        const std::string sat = s.name;
        if (o_.text) o_.text(shortName_(sat) + " \xc2\xb7 " + what + (ty == kMessage ? " \xc2\xb7 " + r.p.hex() : std::string()));
        if (o_.json) {
            char b[160];
            std::snprintf(b, sizeof b, "{\"type\":\"orbcomm_pkt\",\"data\":{\"t\":%.1f,\"norad\":%lld,\"type\":\"%s\",\"snr\":%.1f,\"rf\":%.0f,", r.t, a.norad, typeName(ty), r.snrDb, r.rfHz);
            o_.json(std::string(b) + "\"sat\":\"" + jsonEsc(sat) + "\",\"text\":\"" + jsonEsc(what) + "\",\"hex\":\"" + r.p.hex() + "\",\"raw\":" + (ty == kMessage ? "true" : "false")
                    + ",\"v\":" + vals + ",\"fixed\":" + std::to_string(r.p.repaired) + "}}");
        }
    }
    static std::string shortName_(const std::string& n) {
        return n.rfind("ORBCOMM ", 0) == 0 && n.size() > 8 ? "ORBCOMM " + n.substr(8) : n;
    }
    static std::string clockText_(double sat, double ours) {
        if (!(sat > 0)) return "not read";
        const time_t tt = time_t(sat);
        struct tm g{}; gmtime_r(&tt, &g);
        char b[64]; std::snprintf(b, sizeof b, "%02d:%02d:%02d UTC (%+.1f s from ours)", g.tm_hour, g.tm_min, g.tm_sec, sat - ours);
        return b;
    }
    void addDot_(long long norad, double t, int snr, float az, float el) {
        Bucket& b = buckets_[norad];
        if (b.n && t - b.t0 >= vibe::skyplot::kBucketSec) flushOne_(norad, b);
        if (!b.n) { b.t0 = t; b.snr = snr; }
        b.snr = std::max(b.snr, snr); b.n++; b.az = az; b.el = el;
    }
    void flushOne_(long long norad, Bucket& b) {
        if (!b.n) return;
        vibe::skyplot::Dot d;
        d.t = uint32_t(b.t0); d.azDeg = b.az; d.elDeg = b.el; d.snr = int8_t(std::max(-127, std::min(127, b.snr)));
        d.kind = vibe::skyplot::kOrbcomm; d.count = uint16_t(std::min(65535, b.n)); d.norad = uint32_t(norad);
        pendingDots_.push_back(d); liveDots_.push_back(d);
        b = Bucket();
    }
    void flushDots_(double t) {
        for (auto& kv : buckets_) if (kv.second.n && t - kv.second.t0 >= vibe::skyplot::kBucketSec) flushOne_(kv.first, kv.second);
        if (!pendingDots_.empty() && o_.dots) o_.dots(pendingDots_);
        dotsWritten_ += pendingDots_.size();
        pendingDots_.clear();
    }
    /** ★ The pick tracked but nothing heard for a bucket: a grey dot — only once the bird has been heard this pass (an
     *  ORBCOMM bird can be switched off, or on a channel outside the slice: grey would then blame the aerial). */
    void quietDot_(double t) {
        const long long pk = o_.pickNorad ? o_.pickNorad() : 0;
        if (!pk || !seenThisPass_[pk] || !id_.hasObserver()) return;
        Bucket& b = buckets_[pk];
        if (b.n) { quietFrom_ = t; return; }
        if (t - quietFrom_ < vibe::skyplot::kBucketSec) return;
        quietFrom_ = t;
        for (Bird bd : id_.birds()) if (bd.norad == pk && bd.ok) {
            vibe::sat::Observer ob;
            if (!o_.observer || !o_.observer(ob)) return;
            const vibe::sat::Look L = vibe::sat::look(bd.sgp, ob, t);
            if (!L.ok || L.elDeg < 0) return;
            vibe::skyplot::Dot d; d.t = uint32_t(t); d.azDeg = float(L.azDeg); d.elDeg = float(L.elDeg); d.snr = vibe::skyplot::kNothing;
            d.kind = vibe::skyplot::kOrbcomm; d.norad = uint32_t(pk);
            pendingDots_.push_back(d); liveDots_.push_back(d);
            return;
        }
    }
    void tick_(double t) {
        if (o_.log && t - minuteLogAt_ >= 60) {
            minuteLogAt_ = t;
            std::string pk;
            int k = 0;
            for (const auto& p : rx_.peaks()) { if (k++ >= 4) break; char c[48]; std::snprintf(c, sizeof c, " %.4f(+%.1f dB)", (rx_.sliceCentre() + p.offsetHz) / 1e6, p.db); pk += c; }
            int locked = 0, open = 0; for (auto& c : rx_.channels()) { open++; if (c.locked) locked++; }
            char b[320];
            std::snprintf(b, sizeof b, "ORBCOMM: %.0f s of IQ fed (%.0f kS/s at %.4f MHz), %d carriers open, %d locked, %llu good / %llu bad packets; refused %llu spurs, %llu not SDPSK; peaks:%s",
                          rx_.secondsFed(), rx_.sliceRate() / 1000, rx_.sliceCentre() / 1e6, open, locked, (unsigned long long)good_, (unsigned long long)bad_,
                          (unsigned long long)rx_.spursRejected(), (unsigned long long)rx_.notSdpskRejected(), pk.empty() ? " none" : pk.c_str());
            o_.log(b);
        }
        for (auto it = sats_.begin(); it != sats_.end();) { if (t - it->second.last > 600) { lastEph_.erase(it->first); buckets_.erase(it->first); it = sats_.erase(it); } else ++it; }   // ★ bounded
        quietDot_(t);
        flushDots_(t);
        const int up = o_.satInRange ? o_.satInRange() : -1;
        if (lastUp_ == 1 && up == 0) passEnd_(false);   // the pick has set
        lastUp_ = up;
        if (!o_.json) { liveDots_.clear(); return; }
        std::string j = "{\"type\":\"orbcomm_status\",\"data\":{\"t\":" + fmt("%.0f", t) + ",\"good\":" + std::to_string(good_) + ",\"bad\":" + std::to_string(bad_)
                        + ",\"messages\":" + std::to_string(msgs_) + ",\"dropped\":" + std::to_string(dropped_) + ",\"maxChans\":" + std::to_string(rx_.maxChans())
                        + ",\"located\":" + (id_.hasObserver() ? "true" : "false") + ",\"birds\":" + std::to_string(id_.birds().size()) + ",\"counts\":{";
        bool first = true;
        for (auto& kv : typeCount_) { j += std::string(first ? "" : ",") + "\"" + typeName(kv.first) + "\":" + std::to_string(kv.second); first = false; }
        j += "},\"sats\":[";
        first = true;
        for (auto& kv : sats_) {
            const Sat& s = kv.second;
            if (t - s.last > 30) continue;   // gone (set, or the carrier dropped)
            char b[400];
            std::snprintf(b, sizeof b, "{\"norad\":%lld,\"how\":\"%s\",\"carrier\":%d,\"rf\":%.0f,\"snr\":%.1f,\"dop\":%.0f,\"pred\":%.0f,\"az\":%.1f,\"el\":%.1f,\"dcn\":%d,\"nominal\":%.0f,\"scId\":%d,\"ok\":%llu,\"messages\":%llu,\"age\":%.0f,",
                          s.norad, s.how.c_str(), s.chan, s.rfHz, s.snr, s.dop, s.pred, s.az, s.el, s.dcn, s.dcn ? downlinkHz(s.dcn) : 0.0, s.scId, (unsigned long long)s.ok,
                          (unsigned long long)s.msgs, t - s.last);
            j += std::string(first ? "" : ",") + b + "\"name\":\"" + jsonEsc(s.name) + "\"";
            if (!s.eph.empty()) j += ",\"eph\":" + s.eph;
            if (!s.down.empty()) j += ",\"down\":" + s.down;
            if (!s.up.empty()) j += ",\"up\":" + s.up;
            if (!s.elements.empty()) j += ",\"elements\":" + s.elements;
            j += "}";
            first = false;
        }
        j += "],\"carriers\":[";
        first = true;
        for (auto& c : rx_.channels()) {
            char b[200];
            std::snprintf(b, sizeof b, "%s{\"id\":%d,\"rf\":%.0f,\"snr\":%.1f,\"lock\":%s,\"ok\":%llu,\"bad\":%llu,\"fixed\":%llu", first ? "" : ",", c.id, c.rfHz, c.snrDb,
                          c.locked ? "true" : "false", (unsigned long long)c.ok, (unsigned long long)c.bad, (unsigned long long)c.repaired);
            j += b; first = false;
            // ★ the bird on this carrier, once named (the waterfall's carrier mark)
            for (auto& kv : sats_) if (kv.second.chan == c.id && kv.second.norad && t - kv.second.last < 30) { j += ",\"sat\":\"" + jsonEsc(kv.second.name) + "\""; break; }
            j += "}";
        }
        {   /* ★★ THE SLICE (2026-11-24, the XCover's HF+ showed no carrier and nothing said whether IQ arrived): what the decoder is
             *  fed and where it searches — the clients draw lo…hi on the waterfall ("every carrier decoded here") — and the
             *  scanner's strongest peaks over its floor, so a live log shows a carrier seen but not opened. */
            char b[260];
            std::snprintf(b, sizeof b, "],\"slice\":{\"rate\":%.0f,\"centre\":%.0f,\"lo\":%.0f,\"hi\":%.0f,\"fedSec\":%.1f,\"blocks\":%llu,\"moves\":%llu,\"floorDb\":%.1f},\"peaks\":[",
                          rx_.sliceRate(), rx_.sliceCentre(), rx_.sliceCentre() - rx_.sliceHalfHz(), rx_.sliceCentre() + rx_.sliceHalfHz(), rx_.secondsFed(),
                          (unsigned long long)rx_.blocksFed(), (unsigned long long)rx_.centreMoves(), rx_.floorDb());
            j += b;
            int k = 0;
            for (const auto& pk : rx_.peaks()) {
                if (k >= 6) break;
                char c[64]; std::snprintf(c, sizeof c, "%s{\"rf\":%.0f,\"db\":%.1f}", k ? "," : "", rx_.sliceCentre() + pk.offsetHz, pk.db);
                j += c; k++;
            }
        }
        j += "],\"refused\":{\"spurs\":" + std::to_string(rx_.spursRejected()) + ",\"notSdpsk\":" + std::to_string(rx_.notSdpskRejected()) + ",\"last\":[";
        {   // ★ the last few turned away: where, why, how long banned (a live log shows what was refused)
            bool f1 = true;
            for (const auto& rf : rx_.refusedRecent()) {
                char b[140];
                std::snprintf(b, sizeof b, "%s{\"rf\":%.0f,\"why\":\"%s\",\"ago\":%.0f,\"ban\":%.0f}", f1 ? "" : ",", rf.rfHz, rf.why, t - rf.t, rf.banSec);
                j += b; f1 = false;
            }
        }
        j += "]}";   // ★ candidates the carrier checks turned away
        j += ",\"pick\":" + std::to_string(o_.pickNorad ? o_.pickNorad() : 0) + ",\"up\":" + std::to_string(up) + ",\"dots\":[";
        for (size_t i = 0; i < liveDots_.size() && i < 64; i++) {
            const auto& d = liveDots_[i];
            char b[96];
            if (d.snr == vibe::skyplot::kNothing) std::snprintf(b, sizeof b, "%s[%d,%d,null,%u,%u]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), d.t, d.norad);
            else std::snprintf(b, sizeof b, "%s[%d,%d,%d,%u,%u]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), int(d.snr), d.t, d.norad);
            j += b;
        }
        liveDots_.clear();
        j += "]}}";
        o_.json(j);
    }
    void passEnd_(bool partial) {
        bool any = false;
        for (auto& kv : seenThisPass_) if (kv.second) any = true;
        if (!any) return;
        seenThisPass_.clear();
        if (o_.log) o_.log("ORBCOMM: pass over — " + std::to_string(good_) + " good packets, " + std::to_string(msgs_) + " subscriber messages (raw), "
                           + std::to_string(dotsWritten_) + " sky dots");
        if (o_.skyPng && o_.picture) {
            std::string title;
            const std::string png = o_.skyPng(title);
            if (!png.empty()) o_.picture(title.empty() ? "sky plot" : title, png, 520, 584, partial);
        }
    }

    Opts o_;
    Receiver rx_;
    Ident id_;
    std::map<long long, Sat> sats_;
    std::map<long long, Bucket> buckets_;
    std::map<long long, double> lastEph_;
    std::map<long long, bool> seenThisPass_;
    std::map<std::string, double> lastSame_;
    std::map<uint8_t, uint64_t> typeCount_;
    std::map<long long, int> goodBySat_;
    std::vector<vibe::skyplot::Dot> pendingDots_, liveDots_;
    double birdsAt_ = 0, statusAt_ = 0, pktSecAt_ = 0, quietFrom_ = 0;
    int pktSec_ = 0, lastUp_ = -1;
    uint64_t good_ = 0, bad_ = 0, msgs_ = 0, dropped_ = 0, dotsWritten_ = 0;
    bool finished_ = false, firstLogged_ = false;
    double minuteLogAt_ = 0;
};

}  // namespace vibeorb
