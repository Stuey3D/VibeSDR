// vibe_aprs_run.h — V12 APRS: one decoder run (a listener's, on their NFM/SSB audio, or a background VFO's) — the
// demodulator, the station list, the messages, and what is sent to the clients and the gallery. docs/v12/AIS-APRS.md.
//
// ★★ RUNS ON THE DECODER HOST'S OWN THREAD (vibe_decoder_host.h), fed 48 kHz audio; never on the audio thread.
// ★★ ONE PACKET, HEARD THREE TIMES (direct, then from two digipeaters) IS ONE PACKET: the APRS 1.0.1 duplicate rule —
//    the same source and the same information field within 30 s — counts it once; the paths it came by are kept for the
//    card ("via MB7UH*"), so a listener sees which digipeater carried it.
// ★ What goes out:
//     {"type":"aprs_packet","data":{t, from, kind, line, raw, lat?, lon?, key, sym, msg?}}  — each new packet
//     {"type":"aprs_list","data":{baud, frames, repaired, dupes, stations:[…], messages:[…]}} — every 2 s when changed,
//       and at once to a joiner (with each station's track; the clients add points from the packets after that).
// ★★ V12 (docs/v12/AX25-9600-FX25-IL2P.md): 9600 Bd (G3RUH, from the listener's CHANNEL IQ — feedIq — or from
//    discriminator audio), FX.25 and IL2P (1200 and 9600), and THE FRAME LOG — every frame, APRS or not (cubesat
//    telemetry, connected-mode traffic), as hex and ASCII with the satellite's name where the callsign gives it:
//     {"type":"aprs_frame","data":{age, from, to, path?, ctl, pid?, fec ("ax25"|"fx25"|"il2p"), fixed?, crc?, sat?,
//       len, hex, text, aprs (1 when it was also an APRS packet)}}  — each new frame
//     aprs_list also carries fx25, il2p (frames each brought), fixed (bytes corrected), "log":[…] (newest 40, to a joiner).
#pragma once
#include "vibe_aprs.h"
#include "vibe_g3ruh.h"       // ★ V12 9600 Bd G3RUH (docs/v12/AX25-9600-FX25-IL2P.md)
#include "vibe_sat_calls.h"
#include "vibe_sonate2_tlm.h"   // ★ V12 beta90 SONATE-2 housekeeping
#include "vibe_station_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vibeaprs {

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}
/** ★ Text off the air into JSON: invalid UTF-8 replaced byte by byte with '?', so one bad byte never breaks a client's
 *  JSON.parse (an APRS comment is whatever a TNC sent — Latin-1 is common). */
inline std::string utf8Clean(const std::string& s) {
    std::string o; size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + size_t(n) <= s.size();
        for (int k = 1; ok && k < n; ++k) ok = ((unsigned char)s[i + size_t(k)] >> 6) == 2;
        if (!ok) { o += '?'; ++i; continue; }
        o.append(s, i, size_t(n)); i += size_t(n);
    }
    return o;
}

struct Station {
    std::string key, from, name, kind;
    char symTable = '/', symCode = '/';
    double lat = NAN, lon = NAN, course = NAN, speedKn = NAN, altFt = NAN;
    std::string comment, status, micE, path, raw, line, via;
    std::string sat;   ///< ★ V12 beta90 1.9: the satellite that carried it ("ISS", "SONATE-2") — vibe_sat_calls.h / the pick
    Weather wx;
    bool killed = false, messaging = false;
    double first = 0, last = 0, posAt = -1;
    int count = 0;
    struct Pt { double t, lat, lon; };
    std::deque<Pt> trail;
};
struct Message { double t; std::string from, to, text, id, kind; };

class Run {
public:
    struct Opts {
        std::function<void(const std::string&)> json;          ///< a JSON text frame to every decoder socket
        std::function<void(const vibestation::Heard&)> heard;  ///< the gallery's copy (null = none)
        std::function<double()> rfHz;                          ///< the dial (for the gallery line), may be empty
        std::function<double()> clock;                         ///< seconds (tests); empty = steady clock
        bool fx25 = false, il2p = false;                       ///< ★ V12: also decode FX.25 / IL2P (AUTO = both on)
        std::string sat;                                       ///< ★ V12: a fixed satellite name (tests / tools), or ""
        /** ★ V12: the satellite engine's name for what this listener is locked on, asked per frame (vibe::sat::decoderView
         *  in the host — docs/v12/SATELLITE-ENGINE.md "the decoder hook"); empty / "" = none. */
        std::function<std::string()> satName;
    };
    /** One entry of the frame log. */
    struct LogEntry { double t; std::string from, to, path, ctl, sat, hex, text; int pid = -1, fec = 0, fixed = 0, len = 0; bool crc = false, aprs = false; };
    static constexpr size_t kLogMax = 60, kLogJoin = 40, kHexMax = 256;
    static constexpr size_t kMaxStations = 500, kTrailMax = 120, kMsgMax = 60;
    static constexpr double kKeepSec = 6 * 3600, kDupeSec = 30;

    /** `baud` 300, 1200 or ★ V12 9600 (G3RUH — fed the channel IQ, feedIq, or discriminator audio), or ★ 0 = AUTO:
     *  1200 Bd on the audio AND 9600 on the channel IQ at once, FX.25 and IL2P on both (the packet decoder's default). */
    Run(int baud, Opts o) : baud_(baud == 0 ? 0 : baud >= 4800 ? 9600 : baud), o_(std::move(o)) {
        if (baud_ == 0) { o_.fx25 = true; o_.il2p = true; }
        if (baud_ == 9600 || baud_ == 0) {
            vibeg3ruh::Opts g; g.fx25 = o_.fx25; g.il2p = o_.il2p;
            g3_.reset(new vibeg3ruh::Demod(g));
        }
        if (baud_ != 9600) {
            const int b = baud_ == 0 ? 1200 : baud_;
            // FX.25 and IL2P are VHF/UHF FM packet things: not on HF 300 Bd
            afsk_.reset(new AfskDemod(48000, b, false, b == 1200 && o_.fx25, b == 1200 && o_.il2p));
            afsk_->setMinVotes(0);   // ★ the run applies the lone repair rule after every path has voted (flushHeld_)
        }
    }
    /** 300, 1200, 9600 — or 0 for AUTO (1200 + 9600). */
    int baud() const { return baud_; }
    /** ★ V12: this run wants the listener's CHANNEL IQ — 9600 (G3RUH), and ★ 2026-11-22 1200 Bd's own narrow FM
     *  discriminator beside the listener's audio (iq1200_ — beta92: from the pipeline's decoder tap). Not 300 Bd HF (SSB audio, no FM to discriminate). */
    bool wantsIq() const { return g3_ != nullptr || baud_ == 1200; }

    /** 48 kHz audio from the decoder thread (at 9600: FM-discriminator audio — a flat, wide one only; a normal NFM
     *  listener's audio is filtered too narrow for 9600 and the host feeds the channel IQ instead). */
    void feedAudio(const float* x, int n) {
        // ★ 9600 reads the audio only where no channel IQ comes (a test, a WAV of a radio's data port): once the host
        //   hands it IQ, the listener's narrow NFM audio would only cost CPU.
        if (g3_ && !iqFed_) g3_->feedAudio(x, n, 48000, [&](const vibepkt::PktFrame& f) { onPkt_(f); });
        /* ★★ beta92: the listener's audio is 1200 Bd's path ONLY WHERE NO IQ COMES (an Icom's audio, HF 300 Bd, a test):
         *  while the IQ receiver runs it adds nothing worth its CPU — test-aprs-onair --audio-check, 18 rows of 48 packets
         *  at 7–9 dB (pre-emphasised, flat and de-emphasised TXs, VOICE and ALL): IQ alone 692, IQ + audio 699; the
         *  --iq-sweep (deviation ±3–6 kHz, ±2 kHz off tune): 822 either way. And it is the listener's NFM chain, which
         *  APRS no longer depends on (Stuart 11-23). */
        /* ★★★ beta93 HOTFIX (Stuart 11-24, the XCover's Airspy on 144.800: clear S5 packets, "no packets yet"; the Lenovo
         *  silent overnight): the listener's audio path runs ALWAYS again, beside the IQ receiver — beta92 switched it off
         *  whenever IQ came, so a fault anywhere in the IQ feed on a real server silenced APRS completely. Both paths offer
         *  to offer1200_, which drops a frame another path gave within 3 s. Cost: one AfskDemod at 48 kHz (beta91's). */
        if (afsk_) afsk_->feed(x, n, [&](const Frame& f) { offer1200_(f, audioT_, 0); });
        audioT_ += n / 48000.0;
        flushHeld_();
        tickList_();
    }
    /** ★ V12 9600 (and ★ 1200's narrow path): the listener's channel IQ (dial at DC) at `rate` Hz. */
    void feedIq(const std::complex<float>* x, int n, double rate) {
        if (n <= 0 || !(rate > 0)) return;
        if (g3_) { iqFed_ = true; g3_->feedIq(x, n, rate, [&](const vibepkt::PktFrame& f) { onPkt_(f); }); }
        if (baud_ == 0 || baud_ == 1200) iq1200_(x, n, rate);
        flushHeld_();
        tickList_();
        // ★ VIBE_PKT_LOG=1: what the 9600 receiver sees, every 5 s, to stderr — the first thing to look at on a live capture
        static const bool dbg = std::getenv("VIBE_PKT_LOG") != nullptr;
        if (dbg) {
            iqSamples_ += uint64_t(n);
            const double t = now();
            if (t - iqLogAt_ >= 5.0) {
                iqLogAt_ = t;
                std::fprintf(stderr, "[packet] 9600: channel IQ at %.0f S/s (%llu samples), eye %.2f, AFC %+.0f Hz, FX.25 tags %llu, IL2P syncs %llu, frames %zu\n",
                             rate, (unsigned long long)iqSamples_, g3_->eye(), g3_->afcHz(), (unsigned long long)g3_->fx25Tags(),
                             (unsigned long long)g3_->il2pSyncs(), log_.size());
            }
        }
    }
    double afcHz() const { return g3_ ? g3_->afcHz() : 0; }
    /** ★ 1200 Bd frames each path brought FIRST (the other path's copy is dropped): the listener's audio, the narrow IQ. */
    uint64_t framesFromAudio() const { return fromAudio_; }
    uint64_t framesFromNarrow() const { return fromNarrow_; }
    /** ★ For tests and the offline tool: one frame, as the demodulator hands it over. */
    void onFrame(const Frame& f) {
        if (f.fec == vibepkt::kFx25) ++fx25_; else if (f.fec == vibepkt::kIl2p) ++il2p_;
        fixed_ += uint64_t(f.fixed);
        Ax25 ax;
        bool aprs = parseAx25(f.bytes, ax);
        // ★ V12: a UI frame with the APRS PID whose information field is BINARY and of no APRS type is telemetry (a
        //   cubesat's, a node's), not an APRS station: it goes to the frame log only, never onto the station list.
        if (aprs && binaryInfo_(ax.info) && parsePacket(ax).kind == "other") aprs = false;
        if (!aprs) { logFrame_(f, false); return; }
        // ★ an APRS frame goes in the frame log AFTER its packet (aprs_packet first, then aprs_frame), dupes included —
        //   the log is a raw monitor: every frame heard, a digipeated copy too
        struct LogAfter { Run* r; const Frame& f; ~LogAfter() { r->logFrame_(f, true); } } logAfter{ this, f };
        ++frames_; if (f.repaired) ++repaired_;
        const double t = now();
        // ★ the duplicate rule (source + information field within 30 s)
        const std::string dk = addrText(ax.src) + "|" + ax.info;
        for (auto it = dupes_.begin(); it != dupes_.end();) { if (t - it->second > kDupeSec) it = dupes_.erase(it); else ++it; }
        Packet p = parsePacket(ax);
        curSat_ = satOf_(ax);
        auto d = dupes_.find(dk);
        if (d != dupes_.end()) {
            ++dupes_n_;
            // a digipeated copy: remember the path it came by (and the satellite that carried it), nothing else
            auto st = stations_.find(keyOf(p));
            if (st != stations_.end() && !p.path.empty() && p.path.find('*') != std::string::npos) st->second.via = viaOf(p.path);
            if (st != stations_.end() && !curSat_.empty()) st->second.sat = curSat_;
            return;
        }
        dupes_[dk] = t;
        accept(p, t);
    }
    /** The whole list (to a joiner, with tracks; every 2 s, without). */
    std::string listJson(bool withTrails) {
        const double t = now();
        prune(t);
        std::string o = "{\"type\":\"aprs_list\",\"data\":{\"baud\":" + std::to_string(baud_ == 0 ? 1200 : baud_) + (baud_ == 0 ? ",\"auto\":1" : "")
                      + ",\"frames\":" + std::to_string(frames_)
                      + ",\"repaired\":" + std::to_string(repaired_) + ",\"dupes\":" + std::to_string(dupes_n_)
                      + ",\"fx25\":" + std::to_string(fx25_) + ",\"il2p\":" + std::to_string(il2p_) + ",\"fixed\":" + std::to_string(fixed_)
                      + ",\"other\":" + std::to_string(other_) + (o_.fx25 ? ",\"fx25On\":1" : "") + (o_.il2p ? ",\"il2pOn\":1" : "")
                      + (o_.sat.empty() ? std::string() : ",\"sat\":\"" + jsonEsc(utf8Clean(o_.sat)) + "\"") + ",\"stations\":[";
        std::vector<const Station*> v;
        for (const auto& kv : stations_) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const Station* a, const Station* b) { return a->last > b->last; });
        bool first = true;
        for (const Station* s : v) { if (!first) o += ','; first = false; stationJson(*s, t, withTrails, o); }
        o += "],\"messages\":[";
        for (size_t i = 0; i < msgs_.size(); ++i) {
            const Message& m = msgs_[i];
            if (i) o += ',';
            o += "{\"age\":" + fmt(t - m.t, 0) + ",\"from\":\"" + jsonEsc(m.from) + "\",\"to\":\"" + jsonEsc(m.to) + "\",\"text\":\""
               + jsonEsc(utf8Clean(m.text)) + "\",\"kind\":\"" + m.kind + "\"" + (m.id.empty() ? "" : ",\"id\":\"" + jsonEsc(m.id) + "\"") + "}";
        }
        o += "]";
        if (withTrails && !log_.empty()) {
            o += ",\"log\":[";
            const size_t from = log_.size() > kLogJoin ? log_.size() - kLogJoin : 0;
            for (size_t i = from; i < log_.size(); ++i) { if (i > from) o += ','; logJson_(log_[i], t, o); }
            o += "]";
        }
        return o + "}}";
    }
    size_t stationCount() const { return stations_.size(); }
    size_t logCount() const { return log_.size(); }
    uint64_t fx25Frames() const { return fx25_; }
    uint64_t il2pFrames() const { return il2p_; }
    uint64_t otherFrames() const { return other_; }
    uint64_t frames() const { return frames_; }
    uint64_t repaired() const { return repaired_; }

private:
    /** ★ V12 beta90 1.9: the satellite a frame came through — the SATELLITE pick's bird (asked per frame), a fixed name,
     *  else a callsign we are sure of (vibe_sat_calls.h): the source, or a digipeater that marked it (H bit). "" = none. */
    std::string satOf_(const Ax25& ax) const {
        std::string n = o_.satName ? o_.satName() : std::string();
        if (n.empty()) n = o_.sat;
        if (n.empty()) n = vibesat::byCallsign(ax.src.call);
        if (n.empty()) for (const Addr& dg : ax.digis) if (dg.h) { const std::string m = vibesat::byCallsign(dg.call); if (!m.empty()) n = m; }
        return n;
    }
    /** ★ V12 beta90 1.3: APRS 1.0.1 telemetry "T#sss,a1,a2,a3,a4,a5,bbbbbbbb" heard through a satellite → sat_tlm. The
     *  analogue values are RAW (their EQNS coefficients come in a separate message, not kept here); the 8 bits exact. */
    void telemetry_(const Packet& p, const std::string& sat) {
        if (!o_.json || sat.empty() || p.kind != "telemetry") return;
        const std::string& t = p.raw;
        const size_t at = t.find("T#");
        if (at == std::string::npos) return;
        std::vector<std::string> f;
        std::string cur;
        for (size_t i = at + 2; i < t.size() && f.size() < 7; ++i) {
            const char c = t[i];
            if (c == ',') { f.push_back(cur); cur.clear(); continue; }
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || (f.size() == 0 && std::isalnum((unsigned char)c))) cur += c; else break;
        }
        if (f.size() < 7 && !cur.empty()) f.push_back(cur);
        if (f.size() < 6) return;
        std::string j = "{\"type\":\"sat_tlm\",\"data\":{\"sat\":\"" + jsonEsc(utf8Clean(sat)) + "\",\"format\":\"aprs-t\",\"from\":\""
                      + jsonEsc(utf8Clean(p.from)) + "\",\"frame\":\"T#" + jsonEsc(f[0].substr(0, 8)) + "\"";
        char* e = nullptr;
        const long seq = std::strtol(f[0].c_str(), &e, 10);
        if (e && *e == 0 && !f[0].empty()) j += ",\"seq\":" + std::to_string(seq);
        j += ",\"values\":[";
        bool first = true;
        for (int i = 1; i <= 5 && i < (int)f.size(); ++i) {
            char* e2 = nullptr; const double v = std::strtod(f[(size_t)i].c_str(), &e2);
            if (!e2 || *e2 != 0 || f[(size_t)i].empty()) continue;
            if (!first) j += ","; first = false;
            j += "{\"k\":\"a" + std::to_string(i) + "\",\"name\":\"Analogue " + std::to_string(i) + "\",\"v\":" + fmt(v, v == std::floor(v) ? 0 : 3) + ",\"raw\":true}";
        }
        if (f.size() >= 7 && f[6].size() == 8)
            for (int b = 0; b < 8; ++b) {
                if (f[6][(size_t)b] != '0' && f[6][(size_t)b] != '1') break;
                if (!first) j += ","; first = false;
                j += "{\"k\":\"b" + std::to_string(b + 1) + "\",\"name\":\"Bit " + std::to_string(b + 1) + "\",\"v\":" + std::string(1, f[6][(size_t)b]) + "}";
            }
        j += "],\"hex\":\"\"}}";
        o_.json(j);
    }
    static bool binaryInfo_(const std::string& s) {
        for (unsigned char c : s) if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f) return true;
        return false;
    }
    void tickList_() {
        const double t = now();
        if (dirty_ && t - lastList_ >= 2.0) { lastList_ = t; dirty_ = false; if (o_.json) o_.json(listJson(false)); }
    }
    void onPkt_(const vibepkt::PktFrame& p) {
        Frame f; f.bytes = p.bytes; f.repaired = p.repaired; f.quality = p.quality; f.slicer = p.slicer; f.fec = p.fec; f.fixed = p.fixed; f.crc = p.crc;
        onFrame(f);
    }
    /** ★ The frame log: every frame (APRS or not). A non-APRS frame also goes to the gallery's day log. */
    void logFrame_(const Frame& f, bool aprs) {
        Ax25 ax; bool hasPid = false;
        if (!parseAx25Frame(f.bytes, ax, hasPid)) return;
        LogEntry e;
        e.t = now(); e.from = addrText(ax.src); e.to = addrText(ax.dest); e.ctl = controlName(ax.control);
        if (hasPid) e.pid = ax.pid;
        for (size_t i = 0; i < ax.digis.size(); ++i) { if (i) e.path += ','; e.path += addrText(ax.digis[i]); if (ax.digis[i].h) e.path += '*'; }
        e.sat = o_.satName ? o_.satName() : std::string();
        if (e.sat.empty()) e.sat = o_.sat;
        if (e.sat.empty()) e.sat = vibesat::byCallsign(ax.src.call);
        if (e.sat.empty()) for (const Addr& d : ax.digis) if (d.h) { const std::string n = vibesat::byCallsign(d.call); if (!n.empty()) e.sat = n; }
        e.fec = f.fec; e.fixed = f.fixed; e.crc = f.crc; e.aprs = aprs; e.len = int(ax.info.size());
        static const char* hx = "0123456789abcdef";
        const size_t n = std::min(ax.info.size(), kHexMax);
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = (unsigned char)ax.info[i];
            e.hex += hx[c >> 4]; e.hex += hx[c & 15];
            e.text += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
        }
        if (!aprs) ++other_;
        // ★ V12 beta90 1.3: SONATE-2's housekeeping (DP0SNX, a CCSDS transfer frame) → named values (vibe_sonate2_tlm.h)
        if (!aprs && o_.json && ax.src.call == "DP0SNX") {
            const vibesonate2::Result r = vibesonate2::decode(ax.info, e.sat.empty() ? std::string("SONATE-2") : e.sat, tlmNow_());
            if (!r.json.empty()) o_.json(r.json);
        }
        log_.push_back(e);
        while (log_.size() > kLogMax) log_.pop_front();
        dirty_ = true;
        if (o_.json) { std::string j = "{\"type\":\"aprs_frame\",\"data\":"; logJson_(e, e.t, j); o_.json(j + "}"); }
        if (!aprs && o_.heard) {
            vibestation::Heard h;
            h.key = e.from; h.kind = "frame"; h.name = e.sat.empty() ? e.from : e.sat + " " + e.from;
            h.freqHz = o_.rfHz ? o_.rfHz() : 0;
            h.line = e.from + ">" + e.to + (e.path.empty() ? "" : "," + e.path) + " " + e.ctl
                   + (e.sat.empty() ? "" : " \xc2\xb7 " + e.sat) + (e.fec ? std::string(" \xc2\xb7 ") + (e.fec == vibepkt::kFx25 ? "FX.25" : "IL2P") : "")
                   + " \xc2\xb7 " + std::to_string(e.len) + " bytes: " + e.hex.substr(0, 64) + (e.hex.size() > 64 ? "\xe2\x80\xa6" : "");
            o_.heard(h);
        }
    }
    static void logJson_(const LogEntry& e, double t, std::string& o) {
        o += "{\"age\":" + fmt(t - e.t, 0) + ",\"from\":\"" + jsonEsc(e.from) + "\",\"to\":\"" + jsonEsc(e.to) + "\"";
        if (!e.path.empty()) o += ",\"path\":\"" + jsonEsc(e.path) + "\"";
        o += ",\"ctl\":\"" + jsonEsc(e.ctl) + "\"";
        if (e.pid >= 0) { char b[8]; std::snprintf(b, sizeof b, "%02X", e.pid); o += ",\"pid\":\""; o += b; o += "\""; }
        o += ",\"fec\":\""; o += vibepkt::fecName(e.fec); o += "\"";
        if (e.fixed) o += ",\"fixed\":" + std::to_string(e.fixed);
        if (e.crc) o += ",\"crc\":1";
        if (!e.sat.empty()) o += ",\"sat\":\"" + jsonEsc(utf8Clean(e.sat)) + "\"";
        o += ",\"len\":" + std::to_string(e.len) + ",\"hex\":\"" + e.hex + "\",\"text\":\"" + jsonEsc(e.text) + "\"";
        if (e.aprs) o += ",\"aprs\":1";
        o += "}";
    }
    /** The wall clock for sat_tlm's "t" (the run's own clock is for ages, and is a test seam). */
    static double tlmNow_() { return (double)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    double now() const {
        if (o_.clock) return o_.clock();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }
    static std::string keyOf(const Packet& p) { return p.name.empty() ? p.from : "obj:" + p.name; }
    static std::string viaOf(const std::string& path) {
        // the last digipeater that marked the packet (the one with '*'); generic aliases (WIDEn-N) are not a station
        const size_t star = path.find('*');
        if (star == std::string::npos) return "";
        size_t b = path.rfind(',', star); b = b == std::string::npos ? 0 : b + 1;
        std::string v = path.substr(b, star - b);
        if (v.compare(0, 4, "WIDE") == 0 || v.compare(0, 5, "TRACE") == 0 || v.compare(0, 5, "RELAY") == 0) {
            // the station before the alias is the one that used it
            if (b >= 2) { size_t b2 = path.rfind(',', b - 2); b2 = b2 == std::string::npos ? 0 : b2 + 1; v = path.substr(b2, b - 1 - b2); }
            else v.clear();
        }
        return v;
    }

    void accept(const Packet& p, double t) {
        if (p.kind == "message" || p.kind == "bulletin" || p.kind == "ack") {
            msgs_.push_back({ t, p.from, p.to, p.text, p.msgId, p.kind });
            while (msgs_.size() > kMsgMax) msgs_.pop_front();
        }
        Station& s = stations_[keyOf(p)];
        if (s.count == 0) { s.key = keyOf(p); s.first = t; }
        ++s.count; s.last = t;
        s.from = p.from; s.name = p.name; s.raw = p.raw; s.path = p.path;
        if (!p.path.empty()) s.via = viaOf(p.path);
        if (!curSat_.empty()) s.sat = curSat_;
        if (p.kind != "message" && p.kind != "ack" && p.kind != "bulletin") s.kind = s.kind.empty() || p.hasPos || p.kind == "weather" ? p.kind : s.kind;
        else if (s.kind.empty()) s.kind = "station";
        if (p.hasPos) {
            s.symTable = p.symTable; s.symCode = p.symCode; s.lat = p.lat; s.lon = p.lon; s.posAt = t;
            s.course = p.course; s.speedKn = p.speedKn; s.messaging = p.messaging;
            if (p.altFt == p.altFt) s.altFt = p.altFt;
            if (!p.comment.empty()) s.comment = p.comment;
            if (!p.micE.empty()) s.micE = p.micE;
            bool add = s.trail.empty();
            if (!add) {
                const auto& q = s.trail.back();
                const double dy = (p.lat - q.lat) * 111320.0, dx = (p.lon - q.lon) * 111320.0 * std::cos(p.lat * M_PI / 180);
                add = dx * dx + dy * dy > 400;   // ★ 20 m: a parked tracker's GPS wander is not a track
            }
            if (add) { s.trail.push_back({ t, p.lat, p.lon }); while (s.trail.size() > kTrailMax) s.trail.pop_front(); }
        }
        if (p.wx.any()) s.wx = p.wx;
        if (!p.status.empty() && (p.kind == "status" || p.kind == "other")) s.status = p.status;
        s.killed = p.killed;
        s.line = readable(p);
        dirty_ = true;
        prune(t);
        if (o_.json) {
            std::string j = "{\"type\":\"aprs_packet\",\"data\":{\"age\":0,\"key\":\"" + jsonEsc(utf8Clean(s.key)) + "\",\"from\":\"" + jsonEsc(utf8Clean(p.from))
                          + "\",\"kind\":\"" + p.kind + "\",\"line\":\"" + jsonEsc(utf8Clean(s.line)) + "\",\"raw\":\"" + jsonEsc(utf8Clean(p.raw)) + "\"";
            if (p.hasPos) j += ",\"lat\":" + fmt(p.lat, 5) + ",\"lon\":" + fmt(p.lon, 5);
            if (!p.to.empty()) j += ",\"to\":\"" + jsonEsc(p.to) + "\",\"text\":\"" + jsonEsc(utf8Clean(p.text)) + "\"";
            if (!s.sat.empty()) j += ",\"sat\":\"" + jsonEsc(utf8Clean(s.sat)) + "\"";
            o_.json(j + "}}");
        }
        telemetry_(p, curSat_);
        if (o_.heard) {
            vibestation::Heard h;
            h.key = s.key; h.kind = p.kind; h.name = s.key; h.freqHz = o_.rfHz ? o_.rfHz() : 0;
            h.line = utf8Clean(s.line);
            if (p.hasPos) h.line += " \xc2\xb7 " + fmt(p.lat, 4) + " " + fmt(p.lon, 4);
            // ★ V12 beta90 1.9: "via ISS" — the satellite that carried it (with the digipeater's call when it differs)
            if (!s.sat.empty()) h.line += " \xc2\xb7 via " + utf8Clean(s.sat) + (!s.via.empty() && s.via != s.sat ? " (" + s.via + ")" : std::string());
            else if (!s.via.empty()) h.line += " \xc2\xb7 via " + s.via;
            o_.heard(h);
        }
    }
    void prune(double t) {
        for (auto it = stations_.begin(); it != stations_.end();) { if (t - it->second.last > kKeepSec) it = stations_.erase(it); else ++it; }
        while (stations_.size() > kMaxStations) {
            auto old = stations_.begin();
            for (auto it = stations_.begin(); it != stations_.end(); ++it) if (it->second.last < old->second.last) old = it;
            stations_.erase(old);
        }
    }
    static void stationJson(const Station& s, double t, bool trails, std::string& o) {
        o += "{\"key\":\"" + jsonEsc(utf8Clean(s.key)) + "\",\"from\":\"" + jsonEsc(utf8Clean(s.from)) + "\",\"kind\":\"" + s.kind + "\"";
        if (!s.name.empty()) o += ",\"name\":\"" + jsonEsc(utf8Clean(s.name)) + "\"";
        // ★ a symbol only once the station has sent one (a position): a station heard only by its messages has none
        if (s.posAt >= 0) {
            const char st[3] = { s.symTable, s.symCode, 0 };
            o += ",\"sym\":\"" + jsonEsc(utf8Clean(st)) + "\"";
            const std::string sn = symbolName(s.symTable, s.symCode);
            if (!sn.empty()) o += ",\"symName\":\"" + jsonEsc(sn) + "\"";
        }
        auto num = [&](const char* k, double x, int dp) { if (x == x) { o += ",\""; o += k; o += "\":" + fmt(x, dp); } };
        num("lat", s.lat, 5); num("lon", s.lon, 5); num("course", s.course, 0); num("speed", s.speedKn == s.speedKn ? s.speedKn * 1.852 : NAN, 1);
        num("alt", s.altFt == s.altFt ? s.altFt * 0.3048 : NAN, 0);
        auto str = [&](const char* k, const std::string& v) { if (!v.empty()) { o += ",\""; o += k; o += "\":\"" + jsonEsc(utf8Clean(v)) + "\""; } };
        str("comment", s.comment); str("status", s.status); str("micE", s.micE); str("path", s.path); str("via", s.via); str("raw", s.raw);
        str("sat", s.sat);   // ★ V12 beta90 1.9
        str("line", s.line);
        if (s.wx.any()) {
            o += ",\"wx\":{";
            bool f = true;
            auto w = [&](const char* k, double v, int dp) { if (v == v) { if (!f) o += ','; f = false; o += "\""; o += k; o += "\":" + fmt(v, dp); } };
            w("tempC", s.wx.tempF == s.wx.tempF ? (s.wx.tempF - 32) * 5 / 9 : NAN, 1); w("hum", s.wx.hum, 0);
            w("windDir", s.wx.windDir, 0); w("windKmh", s.wx.windMph == s.wx.windMph ? s.wx.windMph * 1.609344 : NAN, 1);
            w("gustKmh", s.wx.gustMph == s.wx.gustMph ? s.wx.gustMph * 1.609344 : NAN, 1);
            w("rain1hMm", s.wx.rain1hIn == s.wx.rain1hIn ? s.wx.rain1hIn * 25.4 : NAN, 1);
            w("rainTodayMm", s.wx.rainMidIn == s.wx.rainMidIn ? s.wx.rainMidIn * 25.4 : NAN, 1);
            w("hPa", s.wx.baroMbar, 1);
            o += "}";
        }
        if (s.killed) o += ",\"killed\":1";
        if (s.messaging) o += ",\"msg\":1";
        o += ",\"seen\":" + fmt(t - s.last, 0) + ",\"age\":" + fmt(t - s.first, 0) + ",\"count\":" + std::to_string(s.count)
           + ",\"posAge\":" + fmt(s.posAt >= 0 ? t - s.posAt : -1, 0);
        if (trails && s.trail.size() > 1) {
            o += ",\"trail\":[";
            for (size_t i = 0; i < s.trail.size(); ++i) { if (i) o += ','; o += "[" + fmt(s.trail[i].lat, 5) + "," + fmt(s.trail[i].lon, 5) + "]"; }
            o += "]";
        }
        o += "}";
    }

    /* ★★★ 1200 Bd FROM IQ — THE DECODER'S OWN RECEIVER (beta92, Stuart 11-23: "surely aprs needs to be iq based and
     *  bypass the nfm processing completely then it can work the best it can"). The host feeds this the pipeline's
     *  DECODER TAP (RxPipeline::setDecoderTap, ±kTapHz, cut from the cascade BEFORE the listener's channel filter), so the
     *  listener's width, mode, Voice / ALL, de-emphasis, squelch and mute never reach it; where no tap exists (a test, an
     *  old path) it reads the listener's channel IQ the same way.
     *    ① an AFC — the carrier's mean frequency, power-weighted (arg of z·z*₋₁ smoothed over ~20 ms, measured on the
     *       widest filter's output, closed loop; it moves only while the envelope says a carrier is there, and is pulled
     *       home over ~2 s of hiss) — and one NCO
     *       that puts a tracker 1–2 kHz off 144.800 back on DC (±kAfcMaxHz). Without it a ±2 kHz error lost nearly
     *       everything at 8–10 dB (test-aprs-onair --iq-sweep noafc);
     *    ② THREE channel filters side by side, ±5, ±6.5 and ±8 kHz (kIqHalves). At the FM threshold the noise IN the
     *       filter decides which packets survive, and the best width follows the TX's deviation: at CNR 8 dB a normal ±3
     *       kHz tracker gave 30/32 through ±5 kHz and 7/32 through ±7.5; an over-deviated ±6 kHz one 0/32 and 30/32. No
     *       one width wins (see the numbers in test-aprs-onair --iq-sweep);
     *    ③ an ideal discriminator each (flat: AfskDemod's per-tone levels and twist slicers take any pre-/de-emphasis);
     *    ④ the same AfskDemod as the audio path, one per filter (FX.25, IL2P).
     *  Any copy of a frame another path already gave within 3 s is dropped (offer1200_). */
public:
    /** ★ The decoder tap the host asks the pipeline for: ±12 kHz around the dial (the widest filter + the AFC's reach +
     *  the skirts all fit). */
    static constexpr double kTapHz = 12000;
    static constexpr double kIqTransHz = 2000, kAfcMaxHz = 3000;
    static constexpr int kIqPaths = 3;
    static constexpr double kIqHalves[kIqPaths] = { 5000, 6500, 8000 };
    /** ★ Tunables of the IQ path — fixed in the server, varied only by test-aprs-onair's sweeps (halfHz > 0: that ONE
     *  width instead of the three). */
    struct IqTune { double halfHz = 0; double transHz = 0; bool afc = true; double afcMaxHz = kAfcMaxHz; bool decimate = true;
                    int mf = -1; double mfHalf = 0; int mfK = 0; double mfRate = 0; std::vector<float> mfBm, mfBs; };
    void setIqTune(const IqTune& t) { tune_ = t; iqRx_.clear(); }
    double iqAfcHz() const { return afcHz_; }
    /** ★ The IQ paths' frames by filter (narrowest first) — for tests. */
    uint64_t framesFromIqPath(int k) const { return k >= 0 && k < int(iqRx_.size()) ? iqRx_[size_t(k)].frames : 0; }
private:
    struct IqRx {
        double half = 0, rate = 0;
        std::unique_ptr<vibedsp::FirDecimator> fir;
        std::unique_ptr<AfskDemod> afsk;
        std::vector<std::complex<float>> z;
        std::vector<float> out;
        std::complex<float> prev{1, 0};
        uint64_t frames = 0;
    };
    void iq1200_(const std::complex<float>* x, int n, double rate) {
        const IqTune& tu = tune_;
        const double trans = tu.transHz > 0 ? tu.transHz : kIqTransHz;
        if (std::fabs(rate - nbRate_) > 0.5 || (iqRx_.empty() && !nbNone_)) {
            nbRate_ = rate;
            iqRx_.clear();
            std::vector<double> hs;
            if (tu.halfHz > 0) hs.push_back(tu.halfHz); else hs.assign(kIqHalves, kIqHalves + kIqPaths);
            for (double h : hs) {
                if (rate < 2 * h + trans) continue;   // a channel too narrow to hold this width
                IqRx r; r.half = h;
                /* ★ Decimated only where the rate is TWICE what the stopband needs: the discriminator wants headroom over
                 *  the widest instantaneous frequency (a ±6 kHz tracker + noise peaks), or the phase steps alias. Measured:
                 *  ÷2 at the 40.5 kS/s tap (20 kS/s out) cost the ±8 kHz filter 68 of the sweep's packets (723 → 655) —
                 *  so at a ~40 kS/s tap nothing is decimated. */
                const int D = tu.decimate ? std::max(1, int(rate / (4 * (h + trans)))) : 1;
                r.fir.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(h / rate, trans / rate), D));
                r.afsk.reset(new AfskDemod(rate / D, 1200, false, o_.fx25, o_.il2p));
                r.afsk->setMinVotes(0);
                r.rate = rate / D;
                iqRx_.push_back(std::move(r));
            }
            nbNone_ = iqRx_.empty();
            mfSetup_(rate);
            afcAcc_ = { 0, 0 }; afcHz_ = 0; ncoPh_ = { 1, 0 }; afcP2_ = afcP4_ = 0;
            // ★ the AFC's time constants, per sample of the WIDEST filter's output (where it measures)
            const double ra = iqRx_.empty() ? rate : iqRx_.back().rate;
            afcA_ = float(1.0 - std::exp(-1.0 / (0.020 * ra)));     // the estimate: ~20 ms
            afcLeak_ = 1.0 - std::exp(-1.0 / (2.0 * ra));           // home with no carrier: ~2 s
        }
        if (iqRx_.empty()) return;   // a channel too narrow for any width: the audio path alone
        iqSeenAt_ = audioT_;
        nbMix_.resize(size_t(n));
        // ① the NCO: the AFC's correction (the estimate the last block left; a block is ≤ 20 ms)
        const bool afc = tu.afc;
        if (afc) {
            const std::complex<double> rot = std::polar(1.0, -2 * M_PI * afcHz_ / rate);
            for (int i = 0; i < n; ++i) { nbMix_[size_t(i)] = x[i] * std::complex<float>(ncoPh_); ncoPh_ *= rot; }
            ncoPh_ /= std::abs(ncoPh_);
        } else std::copy(x, x + n, nbMix_.begin());
        int mAfc = 0;
        const size_t widest = iqRx_.size() - 1;
        for (size_t k = 0; k < iqRx_.size(); ++k) {
            IqRx& r = iqRx_[k];
            r.z.resize(size_t(r.fir->maxOut(n))); r.out.resize(r.z.size());
            // ② the channel filter (SIMD FIR)
            const int m = r.fir->process(nbMix_.data(), n, r.z.data());
            // ③ the discriminator — and on the widest, the AFC's estimate
            std::complex<float> acc = afcAcc_, prev = r.prev;
            float p2 = afcP2_, p4 = afcP4_;
            const bool est = k == widest;
            for (int i = 0; i < m; ++i) {
                const std::complex<float> z = r.z[size_t(i)];
                const std::complex<float> d = z * std::conj(prev);
                r.out[size_t(i)] = (d.real() != 0 || d.imag() != 0) ? std::atan2(d.imag(), d.real()) : 0.f;
                if (est) { const float e = std::norm(z); acc += afcA_ * (d - acc); p2 += afcA_ * (e - p2); p4 += afcA_ * (e * e - p4); }
                if (std::norm(z) > 0) prev = z;
            }
            r.prev = prev;
            if (est) { afcAcc_ = acc; afcP2_ = p2; afcP4_ = p4; mAfc = m; }
            // ④ the modem
            const int path = 1 + int(k);
            r.afsk->feed(r.out.data(), m, [&](const Frame& f) { if (offer1200_(f, iqT_, path)) ++r.frames; });
        }
        mfRun_(n);
        if (afc) {
            const std::complex<float> acc = afcAcc_;
            // the carrier's residual offset from the filters' centre — power-weighted, so FM hiss alone pulls it nowhere
            const double eHz = (acc.real() != 0 || acc.imag() != 0) ? std::atan2(acc.imag(), acc.real()) * iqRx_.back().rate / (2 * M_PI) : 0.0;
            /* ★ ONLY ON A CARRIER: FM is a constant envelope (E|z|⁴ / (E|z|²)² → 1), band noise a Rayleigh one (→ 2). The
             *  correction is weighted by how carrier-like the last ~20 ms were (full at ≤ 1.25 — a carrier ~7 dB over the
             *  noise in the filter — none at ≥ 1.75), and only hiss lets it drift home. Without this the AFC random-walked
             *  in pure noise to its ±3 kHz limit over 20 minutes; with it, it stays near 0 (test-aprs-onair, the noise check). */
            const double kurt = afcP2_ > 0 ? double(afcP4_) / (double(afcP2_) * double(afcP2_)) : 2.0;
            const double w = std::clamp((1.75 - kurt) / 0.5, 0.0, 1.0);
            afcHz_ += w * std::min(1.0, mAfc * afcA_ * 0.5) * eHz;   // a fraction of the error per block: a ~40 ms loop
            afcHz_ -= afcHz_ * (1.0 - w) * std::min(1.0, mAfc * afcLeak_);
            afcHz_ = std::clamp(afcHz_, -tu.afcMaxHz, tu.afcMaxHz);
        }
        iqT_ += n / rate;
    }
    /* ★★★ 2026-11-24 THE TONE BANK PATH (FmToneBank, docs/v12/APRS-WEAK.md): the AFC-corrected IQ, cut to ±mfHalf and
     *  decimated to ~20 kS/s, straight into the matched tone bank — no discriminator — then the same slicers. */
    void mfSetup_(double rate) {
        mf_.reset(); mfAfsk_.reset();
        const IqTune& tu = tune_;
        int on = tu.mf;
        if (on < 0) { const char* e = std::getenv("APRS_MF"); on = e ? std::atoi(e) : 1; }
        if (!on) return;
        const double half = tu.mfHalf > 0 ? tu.mfHalf : 8000, trans = 2000;
        const double want = tu.mfRate > 0 ? tu.mfRate : 20000;
        if (rate < 2 * half + trans) return;
        const int D = std::max(1, int(rate / want));
        mfFir_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(half / rate, trans / rate), D));
        const double r2 = rate / D;
        std::vector<float> bm = tu.mfBm.empty() ? std::vector<float>{ 1.5f, 2.5f, 3.6f, 5.0f } : tu.mfBm;
        std::vector<float> bs = tu.mfBs.empty() ? std::vector<float>{ 0.9f, 1.6f, 2.6f } : tu.mfBs;
        mf_.reset(new FmToneBank(r2, 1200, bm, bs, tu.mfK > 0 ? tu.mfK : 12));
        mfAfsk_.reset(new AfskDemod(r2, 1200, false, o_.fx25, o_.il2p, true));
        mfAfsk_->setMinVotes(0);
    }
    void mfRun_(int n) {
        if (!mf_) return;
        mfZ_.resize(size_t(mfFir_->maxOut(n)));
        const int m = mfFir_->process(nbMix_.data(), n, mfZ_.data());
        mfM_.resize(size_t(m)); mfS_.resize(size_t(m));
        for (int i = 0; i < m; ++i) mf_->step(mfZ_[size_t(i)], mfM_[size_t(i)], mfS_[size_t(i)]);
        const int path = 1 + kIqPaths;
        mfAfsk_->feedTones(mfM_.data(), mfS_.data(), m, [&](const Frame& f) { if (offer1200_(f, iqT_, path)) ++mfFrames_; });
    }
public:
    uint64_t framesFromToneBank() const { return mfFrames_; }
private:
    std::unique_ptr<FmToneBank> mf_;
    std::unique_ptr<AfskDemod> mfAfsk_;
    std::unique_ptr<vibedsp::FirDecimator> mfFir_;
    std::vector<std::complex<float>> mfZ_;
    std::vector<float> mfM_, mfS_;
    uint64_t mfFrames_ = 0;
    /** ★ One 1200 Bd frame from any path (0 = the listener's audio, 1… = the IQ filters): a copy of the same bytes another
     *  path gave within 3 s (signal time) is dropped. True = it was new. */
    bool offer1200_(const Frame& f, double t, int path = 0) {
        const uint32_t h = frameHash(f.bytes);
        // ★ a copy of a frame still held: its votes join (resolveCandidates); a copy of one already gone out: dropped
        for (Frame& x : held_) if (x.bytes == f.bytes) { offerCandidate(held_, f); return false; }
        for (const auto& r : seen1200_) if (r.h == h && std::fabs(r.t - t) < 3.0 && r.path != path) return false;
        seen1200_[seenP_] = { h, t, path }; seenP_ = (seenP_ + 1) % kSeen;
        ++(path ? fromNarrow_ : fromAudio_);
        /* ★★★ 2026-11-24 held ~0.3 s (signal time) beside the other paths' copies, then resolveCandidates (vibe_aprs.h):
         *  a repaired copy one path got wrong never goes out beside the right copy another path got clean. */
        if (held_.empty()) heldT_ = t;
        offerCandidate(held_, f);
        return true;
    }
    void flushHeld_(bool all = false) {
        if (held_.empty()) return;
        const double now = std::max(audioT_, iqT_);
        if (!all && now - heldT_ < kHoldSec) return;
        std::vector<Frame> v; v.swap(held_);
        conflicts1200_ += uint64_t(resolveCandidates(v));
        for (const Frame& f : v) { if (f.votes < 1.f) { ++lone1200_; continue; } onFrame(f); }   // the lone repair rule (AfskDemod::setMinVotes)
    }
public:
    static constexpr double kHoldSec = 0.3;
    /** ★ 1200 Bd copies dropped because two disagreed (the demodulators' own and across paths). */
    uint64_t conflicts1200() const { return conflicts1200_ + (afsk_ ? afsk_->conflicts() : 0); }
    /** ★ Tests / tools at the end of a recording: whatever is still held goes out now. */
    void flush() { flushHeld_(true); }
private:
    std::vector<Frame> held_; double heldT_ = 0; uint64_t conflicts1200_ = 0, lone1200_ = 0;
public:
    uint64_t loneRepairs1200() const { return lone1200_; }
private:
    struct Seen { uint32_t h = 0; double t = -100; int path = 0; };
    static constexpr int kSeen = 16;
    Seen seen1200_[kSeen];
    int seenP_ = 0;
    double audioT_ = 0, iqT_ = 0, nbRate_ = 0, iqSeenAt_ = -100;
    uint64_t fromAudio_ = 0, fromNarrow_ = 0;
    std::vector<IqRx> iqRx_;
    bool nbNone_ = false;
    std::vector<std::complex<float>> nbMix_;
    std::complex<float> afcAcc_{0, 0};
    float afcP2_ = 0, afcP4_ = 0;   // ★ the widest filter's power and power² (the AFC's carrier test)
    std::complex<double> ncoPh_{1, 0};
    IqTune tune_;
    double afcHz_ = 0, afcLeak_ = 0;
    float afcA_ = 0;

    int baud_;
    Opts o_;
    std::unique_ptr<AfskDemod> afsk_;
    std::unique_ptr<vibeg3ruh::Demod> g3_;
    std::deque<LogEntry> log_;
    bool iqFed_ = false;
    uint64_t iqSamples_ = 0; double iqLogAt_ = -10;
    uint64_t fx25_ = 0, il2p_ = 0, fixed_ = 0, other_ = 0;
    std::map<std::string, Station> stations_;
    std::map<std::string, double> dupes_;
    std::deque<Message> msgs_;
    uint64_t frames_ = 0, repaired_ = 0, dupes_n_ = 0;
    bool dirty_ = false;
    std::string curSat_;   // ★ the frame being handled: its satellite (satOf_)
    double lastList_ = -10;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

} // namespace vibeaprs
