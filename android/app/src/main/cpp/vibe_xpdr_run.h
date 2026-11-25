// vibe_xpdr_run.h — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER as a listener's decoder: the DecoderHost's "xpdr" extension
// (docs/v12/TRANSPONDER-FINDER.md). A SATELLITE pick of a linear transponder (RS-44, FO-29, AO-7, AO-73, JO-97, MO-122,
// TO-108) — or of its beacon — attaches it (vibe_sat_engine.h decoderOf).
//
// ★★★ WHAT STUART ASKED FOR (11-24, on RS-44's busy passband):
//   · ONE wide IQ slice over the beacon AND the whole transponder, drawn like ORBCOMM's — no separate VFO B for the beacon.
//   · EVERY CW signal in it skimmed and decoded, the beacon included (rows with text and callsigns).
//   · EVERY voice signal a row with LISTEN, which snaps VFO A to it; ORBCOMM's arrows mark each signal on the waterfall.
//   · Manual tuning works as normal (tap, steps, drum) — the finder never fights it.
//   · The slice tracks the SATELLITE's Doppler; VFO A follows the PERSON: first guess = our own uplink Doppler inverted
//     and scaled (vibexpdr::uplinkDriftHz), then measured (an FLL on a CW tone, the voice's carrier from its speech for
//     SSB, held between overs) — a manual nudge always wins (Engine::nudge ends the follow and bumps its seq).
//
// ★★ WHERE IT SITS: the host asks the DSP thread for an IQ slice FIXED on the span's satellite-frame centre (iqFixed — as
//    ORBCOMM's band; neither the dial nor the lock moves it). Here, each block is turned by the model's Doppler at that
//    centre (a phase-continuous mix — CwRun's aim, done here), so every signal keeps ONE satellite-frame place all pass:
//    offset o in the mixed stream = satellite frame centre + o, RF now = centre + Doppler + o. The CW skimmer and the
//    VoiceFinder read the mixed stream on this decode thread (Network > Audio > Spectrum > Decoders).
//
// ★★ WHAT CLIENTS SEE: {"type":"xpdr_status","data":{…}} once a second —
//      slice {lo,hi} (RF now: what the waterfall shades), span {lo,hi} and beacon (satellite frame), dop (the model's
//      Doppler at the centre), rows [{id,"kind":"cw"|"voice",hz (RF now — the arrow),dial (satellite frame — where LISTEN
//      tunes the dial: a voice's suppressed carrier, a CW tone),snr,ago (s since heard),on (heard now),beacon,call,text,
//      wpm}], follow {id,src:"wait"|"guess"|"voice"|"cw"|"held",off (Hz from the dial)} while VFO A follows somebody.
//    A client's LISTEN = tune the dial to the row's `dial` (USB) and send {"type":"xpdr_follow","id":"v3"} on the
//    decoder socket; {"type":"xpdr_follow","id":""} stops following.
#pragma once

#include "decoders/cw_skimmer.h"
#include "vibe_xpdr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibexpdr {

namespace cw = vibe::cw;

class Run {
public:
    static constexpr double kEdgeHz = 3000.0;       ///< searched beyond the span's ends (a station at the very edge)
    static constexpr double kBeaconNearHz = 3000.0; ///< a CW row this close to the listed beacon IS the beacon
    static constexpr double kListenNearHz = 600.0;  ///< the dial within this of the chosen row = the LISTEN tune has landed
    static constexpr double kPendingSec = 10.0;     ///< a follow asked for whose tune never lands is forgotten
    static constexpr double kMeasuredSec = 1.5;     ///< a measurement newer than this steers; older = held + the guess
    static constexpr double kFollowEverySec = 0.25; ///< the follow's target is renewed this often

    struct FollowLink {
        std::function<bool(double& nominal, bool& following, unsigned& seq)> state;   ///< the lock's (false = no passband lock)
        std::function<void(double satHz)> set;
        std::function<void()> clear;
    };
    struct Opts {
        double widthHz = 100000;          ///< the slice asked for
        double centreHz = 0;              ///< the span's centre, satellite frame (the slice's fixed centre)
        double spanLoHz = 0, spanHiHz = 0, beaconHz = 0, upHz = 0;
        bool inverting = true, usb = true;
        std::function<void(const std::string&)> json;
        std::function<void(const std::string&)> log;
        std::function<double(double satHz)> dopplerHz;   ///< the model's Doppler at satHz now (0 = no lock: nothing turned out)
        FollowLink follow;
        std::function<void(const cw::Spot&, double rfHz)> spot;   ///< a CW callsign (the gallery's summary / PSK Reporter)
    };
    explicit Run(Opts o) : o_(std::move(o)) {}

    /** One IQ block from the host (decode thread): `rate`, centred on `centreRfHz` (the fixed slice, clamped into the capture). */
    void feedIq(const cf32* x, int n, double rate, double centreRfHz) {
        if (n <= 0 || !(rate > 0)) return;
        const double dop = o_.dopplerHz ? o_.dopplerHz(o_.centreHz) : 0.0;
        dop_ = std::isfinite(dop) ? dop : 0.0;
        const double aim = o_.centreHz + dop_;
        // ★ turn the block so the span's satellite-frame centre sits at 0 Hz (phase carried between blocks)
        const double d = aim - centreRfHz;
        if (std::fabs(d) < rate * 0.45) {
            if (mix_.size() < (size_t)n) mix_.resize((size_t)n);
            const double w = -2.0 * M_PI * d / rate;
            const std::complex<double> step(std::cos(w), std::sin(w));
            std::complex<double> ph = mixPh_;
            for (int i = 0; i < n; ++i) {
                mix_[(size_t)i] = cf32((float)(x[i].real() * ph.real() - x[i].imag() * ph.imag()),
                                       (float)(x[i].real() * ph.imag() + x[i].imag() * ph.real()));
                ph *= step;
            }
            mixPh_ = ph / std::abs(ph);
            x = mix_.data();
        }
        if (!sk_ || std::fabs(rate - rate_) > 0.5) build_(rate);
        sk_->feedIq(x, n);
        vf_->feed(x, n);
        if (tone_) tone_->feed(x, n);
        t_ += (double)n / rate;
        if (!firstLogged_ && o_.log) {
            firstLogged_ = true;
            char b[220];
            std::snprintf(b, sizeof b, "TRANSPONDER: IQ arriving \xe2\x80\x94 a %.0f kS/s slice, searching %.4f\xe2\x80\x93%.4f MHz (satellite frame), Doppler %+.0f Hz",
                          rate / 1000, (o_.centreHz + lo_) / 1e6, (o_.centreHz + hi_) / 1e6, dop_);
            o_.log(b);
        }
        if (t_ - followAt_ >= kFollowEverySec) { followAt_ = t_; followTick_(); }
        if (t_ - statusAt_ >= 1.0) { statusAt_ = t_; status_(); }
    }
    /** {"type":"xpdr_follow","id":…} from a listener — "" stops following. */
    void follow(const std::string& id) {
        if (id.empty() || id.size() > 12) { stopFollow_("stopped"); return; }
        Row r;
        if (!rowOf_(id, r)) { stopFollow_("gone"); return; }
        fid_ = id; fKind_ = r.voice ? 1 : 2;
        fPendingAt_ = t_; fActive_ = false;
        fMeasSat_ = r.dialSat; fMeasT_ = wall_(); fMeasDrift_ = guessDrift_(r.dialSat);
        fSrc_ = "wait";
        if (!r.voice && sk_) { if (!tone_) tone_.reset(new ToneTracker(rate_, r.dialSat - o_.centreHz)); else tone_->reset(r.dialSat - o_.centreHz); }
        else tone_.reset();
        if (o_.log) o_.log("TRANSPONDER: LISTEN " + id + (r.voice ? " (voice)" : " (CW)"));
    }
    /** The last status JSON (tests). */
    const std::string& lastStatus() const { return last_; }
    int cwRows() const { return sk_ ? (int)sk_->signals(false).size() : 0; }
    int voiceRows() const { return vf_ ? (int)vf_->voices().size() : 0; }

private:
    struct Row {
        std::string id; bool voice = false, on = false, beacon = false;
        double offset = 0, dialSat = 0, ago = 0; float snr = 0, wpm = 0;
        std::string call, text;
    };
    double wall_() const { return t_; }   // the stream clock (a test feeding faster than real time stays consistent)
    void build_(double rate) {
        rate_ = rate;
        const double half = 0.47 * rate;
        lo_ = std::max(-half, o_.spanLoHz - o_.centreHz - kEdgeHz);
        hi_ = std::min(half, o_.spanHiHz - o_.centreHz + kEdgeHz);
        cw::Skimmer::Config c;
        c.rate = rate; c.complexIn = true; c.loHz = lo_; c.hiHz = hi_;
        c.maxSignals = std::max(32, std::min(240, (int)((hi_ - lo_) / 300.0)));
        sk_.reset(new cw::Skimmer(c));
        sk_->onSpot = [this](const cw::Spot& sp) { if (o_.spot) o_.spot(sp, o_.centreHz + dop_ + sp.offsetHz); };
        vf_.reset(new VoiceFinder(rate, lo_, hi_, o_.usb));
        tone_.reset();
    }
    static std::string clean_(const std::string& s, size_t max) {
        std::string o;
        for (char c : s) { if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') o += c; }
        if (o.size() > max) o = o.substr(o.size() - max);
        return o;
    }
    std::vector<Row> rows_() const {
        std::vector<Row> v;
        if (!sk_ || !vf_) return v;
        const int64_t nowMs = sk_->nowMs();
        for (const auto& s : sk_->signals(true)) {
            Row r; r.id = "c" + std::to_string(s.id); r.voice = false; r.on = s.active;
            r.offset = s.offsetHz; r.dialSat = o_.centreHz + s.offsetHz;
            if (tone_ && fid_ == r.id && tone_->measured()) r.dialSat = o_.centreHz + tone_->hz();
            r.ago = std::max(0.0, (nowMs - s.lastMs) / 1000.0); r.snr = s.snrDb; r.wpm = s.wpm;
            r.beacon = o_.beaconHz > 0 && std::fabs(r.dialSat - o_.beaconHz) <= kBeaconNearHz;
            r.call = clean_(s.call, 12); r.text = clean_(s.text, 60);
            v.push_back(std::move(r));
        }
        for (const auto& s : vf_->voices()) {
            Row r; r.id = "v" + std::to_string(s.id); r.voice = true; r.on = s.active;
            r.offset = s.centreHz; r.dialSat = o_.centreHz + s.carrierHz;
            r.ago = std::max(0.0, vf_->now() - s.lastT); r.snr = s.snrDb;
            v.push_back(std::move(r));
        }
        return v;
    }
    bool rowOf_(const std::string& id, Row& out) const {
        for (auto& r : rows_()) if (r.id == id) { out = r; return true; }
        return false;
    }
    /** Our downlink Doppler at `satHz` turned into the station's own drift (the first guess). */
    double guessDrift_(double satHz) const {
        if (!o_.dopplerHz) return 0.0;
        const double dDown = o_.dopplerHz(satHz);
        const double up = o_.upHz > 0 ? o_.upHz + 0.5 * (o_.spanHiHz - o_.spanLoHz) : 0.0;
        return uplinkDriftHz(dDown, up, satHz, o_.inverting);
    }
    void stopFollow_(const char* why) {
        if (!fid_.empty() && o_.log) o_.log(std::string("TRANSPONDER: follow ended (") + why + ")");
        if (fActive_ && o_.follow.clear) o_.follow.clear();
        fid_.clear(); fActive_ = false; tone_.reset(); fSrc_.clear();
    }
    void followTick_() {
        if (fid_.empty() || !o_.follow.state) return;
        double nominal = 0; bool following = false; unsigned seq = 0;
        if (!o_.follow.state(nominal, following, seq)) { stopFollow_("no lock"); return; }
        Row r; const bool have = rowOf_(fid_, r);
        if (!fActive_) {
            // ★ the LISTEN tune has to land first (it arrives on another socket): the dial near the chosen station
            if (have && std::fabs(nominal - r.dialSat) <= kListenNearHz) {
                fActive_ = true; fSeq_ = seq; fSrc_ = "guess";
                fMeasSat_ = r.dialSat; fMeasT_ = wall_(); fMeasDrift_ = guessDrift_(r.dialSat);
                if (o_.log) { char b[120]; std::snprintf(b, sizeof b, "TRANSPONDER: VFO A follows %s from %.4f MHz", fid_.c_str(), r.dialSat / 1e6); o_.log(b); }
            } else if (t_ - fPendingAt_ > kPendingSec) { stopFollow_("the dial never reached it"); return; }
            else return;
        } else if (seq != fSeq_) { fActive_ = false; stopFollow_("the listener tuned"); return; }   // ★ a manual nudge always wins
        // the target: measured while they are heard; between overs the last measurement + the guessed drift since
        bool measured = false;
        if (have && r.on) {
            if (r.voice) {
                VoiceView_ vv;
                if (voiceMeasured_(r.id, vv)) { fMeasSat_ = r.dialSat; fMeasT_ = wall_(); fMeasDrift_ = guessDrift_(r.dialSat); measured = true; fSrc_ = "voice"; }
            } else if (tone_ && tone_->measured() && wall_() - tone_->measuredAt() <= kMeasuredSec) {
                fMeasSat_ = o_.centreHz + tone_->hz(); fMeasT_ = wall_(); fMeasDrift_ = guessDrift_(fMeasSat_); measured = true; fSrc_ = "cw";
            }
        }
        double target = fMeasSat_;
        if (!measured) {
            target = fMeasSat_ + (guessDrift_(fMeasSat_) - fMeasDrift_);
            if (fSrc_ != "guess") fSrc_ = "held";
        }
        fTarget_ = target;
        if (o_.follow.set) o_.follow.set(target);
        fOff_ = target - nominal;
    }
    struct VoiceView_ { double at = 0; };
    bool voiceMeasured_(const std::string& id, VoiceView_& v) const {
        if (!vf_ || id.size() < 2) return false;
        Voice x;
        if (!vf_->voice(std::atoi(id.c_str() + 1), x)) return false;
        v.at = x.measuredAt;
        return vf_->now() - x.measuredAt <= kMeasuredSec;
    }
    static std::string num_(double v, int dp) { char b[48]; std::snprintf(b, sizeof b, "%.*f", dp, std::isfinite(v) ? v : 0.0); return b; }
    void status_() {
        const double rfC = o_.centreHz + dop_;
        std::string j = "{\"type\":\"xpdr_status\",\"data\":{";
        j += "\"slice\":{\"lo\":" + num_(rfC + lo_, 0) + ",\"hi\":" + num_(rfC + hi_, 0) + "}";
        j += ",\"span\":{\"lo\":" + num_(o_.spanLoHz, 0) + ",\"hi\":" + num_(o_.spanHiHz, 0) + "}";
        if (o_.beaconHz > 0) j += ",\"beacon\":" + num_(o_.beaconHz, 0);
        j += ",\"dop\":" + num_(dop_, 1) + ",\"rows\":[";
        auto rows = rows_();
        // ★ bounded: the 40 freshest
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { if (a.on != b.on) return a.on; return a.ago < b.ago; });
        if (rows.size() > 40) rows.resize(40);
        bool first = true;
        for (const auto& r : rows) {
            if (!first) j += ',';
            first = false;
            j += "{\"id\":\"" + r.id + "\",\"kind\":\"" + (r.voice ? "voice" : "cw") + "\",\"hz\":" + num_(rfC + (r.voice ? r.offset : r.dialSat - o_.centreHz), 0)
               + ",\"dial\":" + num_(r.dialSat, 0) + ",\"snr\":" + num_(r.snr, 1) + ",\"ago\":" + num_(r.ago, 0)
               + ",\"on\":" + (r.on ? "true" : "false");
            if (r.beacon) j += ",\"beacon\":true";
            if (!r.voice) {
                if (r.wpm > 0) j += ",\"wpm\":" + num_(r.wpm, 0);
                if (!r.call.empty()) j += ",\"call\":\"" + r.call + "\"";
                if (!r.text.empty()) j += ",\"text\":\"" + r.text + "\"";
            }
            j += '}';
        }
        j += "]";
        if (!fid_.empty()) j += ",\"follow\":{\"id\":\"" + fid_ + "\",\"src\":\"" + fSrc_ + "\",\"off\":" + num_(fActive_ ? fOff_ : 0.0, 0) + "}";
        j += "}}";
        last_ = j;
        if (o_.json) o_.json(j);
    }

    Opts o_;
    std::unique_ptr<cw::Skimmer> sk_;
    std::unique_ptr<VoiceFinder> vf_;
    std::unique_ptr<ToneTracker> tone_;
    std::vector<cf32> mix_;
    std::complex<double> mixPh_{1, 0};
    double rate_ = 0, lo_ = 0, hi_ = 0, dop_ = 0, t_ = 0, statusAt_ = 0, followAt_ = 0;
    bool firstLogged_ = false;
    // the follow
    std::string fid_, fSrc_;
    int fKind_ = 0;
    bool fActive_ = false;
    unsigned fSeq_ = 0;
    double fPendingAt_ = 0, fMeasSat_ = 0, fMeasT_ = 0, fMeasDrift_ = 0, fTarget_ = 0, fOff_ = 0;
    std::string last_;
};

} // namespace vibexpdr
