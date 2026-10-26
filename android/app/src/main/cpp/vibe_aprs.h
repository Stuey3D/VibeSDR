// vibe_aprs.h — V12 APRS: Bell 202 AFSK 1200 Bd (VHF) and 300 Bd (HF) demodulation, AX.25 UI frames, the APRS formats
// (positions, compressed, Mic-E, objects, items, weather, messages, status, telemetry, NMEA) and readable stations.
// docs/v12/AIS-APRS.md. Framing is vibe_hdlc.h (shared with AIS); the listener/background run is vibe_aprs_run.h.
//
// ★★★ FROM THE PUBLISHED SPECS, written fresh: AX.25 Link Access Protocol v2.2 (TAPR, 1998) — addresses, SSIDs, the
//     has-been-repeated bit, UI frames; APRS Protocol Reference 1.0.1 (TAPR, 2000) — every format below, Mic-E (ch. 10),
//     compressed positions (ch. 9), weather (ch. 12), messages (ch. 14); the APRS 1.1 addenda (base-91 Mic-E altitude,
//     `!DAO!` precision). Bell 202: mark 1200 Hz, space 2200 Hz. HF packet (300 Bd): 1600/1800 Hz, 200 Hz shift.
//     GPL modems were read for behaviour only — none copied (code, tables or structure).
//
// ★★ THE DEMODULATOR (AfskDemod): the audio cut to 12 kS/s (a SIMD FIR, vibedsp dotReal), then one quadrature
//    correlator per tone over exactly one bit (a matched filter for a tone burst), each tone's level followed by its own
//    peak tracker so a transmitter's pre-emphasis (or a receiver's de-emphasis) cannot tilt the decision. Then SEVERAL
//    SLICERS: mark − g·space at five tone balances (−6…+6 dB) — the improvement that matters on real FM audio, where
//    the two tones arrive up to 6 dB apart — each with its own PLL bit clock and deframer. 300 Bd adds five tuning
//    offsets (−60…+60 Hz in 30 Hz steps), because an HF operator is never exactly on frequency (test-aprs: three
//    offsets lost a quarter of the frames half-way between them at 12 dB). A frame any slicer gets through the
//    FCS counts once: the cleanest copy is kept (vibe_ais.h's rule).
// ★ A frame repaired by one flipped level (vibe_hdlc.h) must still have twelve valid address characters, the UI
//   control and the APRS PID — about one garbage frame in three million could pass that.
#pragma once
#include "vibe_hdlc.h"
#include "vibedsp.h"
#include "simd_internal.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vibeaprs {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AX.25
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Addr { std::string call; int ssid = 0; bool h = false; };
inline std::string addrText(const Addr& a) { return a.ssid ? a.call + "-" + std::to_string(a.ssid) : a.call; }

struct Ax25 {
    Addr dest, src;
    std::vector<Addr> digis;
    uint8_t control = 0, pid = 0;
    std::string info;
};
/** An address field byte is an ASCII capital, a digit or a space, shifted left one. */
inline bool addrByteOk(uint8_t b) {
    const char c = char(b >> 1);
    return (b & 1) == 0 && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ');
}
/** Parse a frame (bytes without the FCS). False = not an AX.25 UI frame with the APRS PID. */
inline bool parseAx25(const std::vector<uint8_t>& b, Ax25& f) {
    if (b.size() < 16) return false;
    size_t p = 0;
    std::vector<Addr> as;
    for (;;) {
        if (p + 7 > b.size() || as.size() >= 10) return false;
        Addr a;
        for (int i = 0; i < 6; ++i) {
            if (!addrByteOk(b[p + size_t(i)])) return false;
            const char c = char(b[p + size_t(i)] >> 1);
            if (c != ' ') a.call += c;
            else if (i == 0) return false;
        }
        const uint8_t s = b[p + 6];
        a.ssid = (s >> 1) & 0x0F;
        a.h = (s & 0x80) != 0;
        const bool last = (s & 1) != 0;
        if (a.call.empty()) return false;
        as.push_back(a);
        p += 7;
        if (last) break;
    }
    if (as.size() < 2 || p + 2 > b.size()) return false;
    f.dest = as[0]; f.src = as[1];
    f.digis.assign(as.begin() + 2, as.end());
    f.control = b[p]; f.pid = b[p + 1];
    if (f.control != 0x03 || f.pid != 0xF0) return false;   // ★ UI frame, no layer 3 — what APRS uses
    f.info.assign(b.begin() + long(p) + 2, b.end());
    return !f.info.empty();
}
/** TNC2 monitor format: SRC>DEST,DIGI1*,DIGI2:info — the station card's "raw" line. */
inline std::string tnc2(const Ax25& f) {
    std::string s = addrText(f.src) + ">" + addrText(f.dest);
    int lastH = -1;
    for (size_t i = 0; i < f.digis.size(); ++i) if (f.digis[i].h) lastH = int(i);
    for (size_t i = 0; i < f.digis.size(); ++i) { s += "," + addrText(f.digis[i]); if (int(i) == lastH) s += "*"; }
    return s + ":" + f.info;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE DEMODULATOR
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Frame { std::vector<uint8_t> bytes; int repaired = 0; float quality = 0; int slicer = 0; };

inline bool aprsFrameOk(const std::vector<uint8_t>& b) { Ax25 f; return parseAx25(b, f); }

class AfskDemod {
public:
    /** `fsIn` the audio rate (48000 from the decoder host); `baud` 1200 (Bell 202) or 300 (HF). `single`: one slicer
     *  only (balanced, on tune) — the baseline test-aprs measures the extra slicers against. */
    AfskDemod(double fsIn, int baud, bool single = false) : baud_(baud) {
        decim_ = std::max(1, int(std::lround(fsIn / 12000.0)));
        fs_ = fsIn / decim_;
        const double hi = baud == 300 ? 2400 : 2900;
        lp_ = vibedsp::designLowpass(hi / fsIn, 1200 / fsIn);
        std::reverse(lp_.begin(), lp_.end());
        hist_.assign(lp_.size() * 2, 0.f);
        const double mark = baud == 300 ? 1600 : 1200, space = baud == 300 ? 1800 : 2200;
        const std::vector<double> offs = single || baud != 300 ? std::vector<double>{ 0 } : std::vector<double>{ -60, -30, 0, 30, 60 };
        const std::vector<float> gains = single ? std::vector<float>{ 1.f }
                                       : baud == 300 ? std::vector<float>{ 0.7071f, 1.f, 1.4142f }
                                       : std::vector<float>{ 0.5f, 0.7071f, 1.f, 1.4142f, 2.f };
        L_ = std::max(4, int(std::lround(fs_ / baud)));
        for (double o : offs) {
            Pair p;
            p.m.init(mark + o, fs_, L_); p.s.init(space + o, fs_, L_);
            pairs_.push_back(std::move(p));
        }
        for (size_t pi = 0; pi < pairs_.size(); ++pi)
            for (float g : gains) {
                Slicer s;
                s.pair = int(pi); s.gain = g;
                s.clk.reset(new vibehdlc::BitClock(fs_, baud, 0));
                vibehdlc::Deframer::Options o;
                o.minBytes = 18; o.maxBytes = 340;
                o.repairMax = 8; o.validate = aprsFrameOk; o.validateClean = true;
                s.df.reset(new vibehdlc::Deframer(o));
                sl_.push_back(std::move(s));
            }
        peakA_ = float(1.0 - std::exp(-1.0 / (fs_ / baud * 40)));
    }
    int slicers() const { return int(sl_.size()); }
    double rate() const { return fs_; }

    /** Audio in (any level). `out(Frame)` for each new frame. */
    template <class Out> void feed(const float* x, int n, Out&& out) {
        const int K = int(lp_.size());
        for (int i = 0; i < n; ++i) {
            hist_[size_t(hp_)] = x[i]; hist_[size_t(hp_ + K)] = x[i];
            hp_ = (hp_ + 1) % K;
            if (++dph_ < decim_) continue;
            dph_ = 0;
            const float y = vibedsp::dotReal(&hist_[size_t(hp_)], lp_.data(), K);
            sample_(y, out);
        }
    }
    uint64_t repaired() const { uint64_t r = 0; for (auto& s : sl_) r += s.df->framesRepaired(); return r; }

private:
    /** One tone's correlator over the last L samples: Σ x·e^{−jωn}, kept as a running sum of a ring of products. */
    struct Tone {
        std::complex<double> rot{1, 0}, ph{1, 0};
        std::vector<std::complex<float>> ring;
        std::complex<double> acc{0, 0};
        int rp = 0, renorm = 0;
        float peak = 1e-6f;
        void init(double hz, double fs, int L) { rot = std::polar(1.0, -2 * M_PI * hz / fs); ring.assign(size_t(L), {0, 0}); }
        float step(float x) {
            const std::complex<float> v(float(x * ph.real()), float(x * ph.imag()));
            acc += std::complex<double>(v) - std::complex<double>(ring[size_t(rp)]);
            ring[size_t(rp)] = v; rp = (rp + 1) % int(ring.size());
            ph *= rot;
            if (++renorm >= 1024) { renorm = 0; ph /= std::abs(ph); acc = 0; for (auto& r : ring) acc += std::complex<double>(r); }
            return float(std::abs(acc));
        }
    };
    struct Pair { Tone m, s; float mN = 0, sN = 0; };
    struct Slicer { int pair = 0; float gain = 1; std::unique_ptr<vibehdlc::BitClock> clk; std::unique_ptr<vibehdlc::Deframer> df; };

    template <class Out> void sample_(float y, Out& out) {
        for (auto& p : pairs_) {
            const float m = p.m.step(y), s = p.s.step(y);
            // ★ each tone's own level: a fast-rising, slowly-falling peak — the tilt of pre-/de-emphasis divided out
            p.m.peak += (m > p.m.peak ? 0.3f : peakA_) * (m - p.m.peak);
            p.s.peak += (s > p.s.peak ? 0.3f : peakA_) * (s - p.s.peak);
            p.mN = m / std::max(p.m.peak, 1e-9f); p.sN = s / std::max(p.s.peak, 1e-9f);
        }
        for (size_t k = 0; k < sl_.size(); ++k) {
            Slicer& s = sl_[k];
            const Pair& p = pairs_[size_t(s.pair)];
            const float d = p.mN - s.gain * p.sN;
            s.clk->push(d, s.df->inFrame() ? 0.75f : 0.45f, [&](float lvl) {
                s.df->push(lvl, [&](const vibehdlc::Frame& fr) { offer_(fr, int(k), out); });
            });
        }
        ++t_;
        if (hasPend_ && t_ >= pendDue_) flush_(out);
    }
    template <class Out> void offer_(const vibehdlc::Frame& fr, int slicer, Out& out) {
        uint32_t h = 2166136261u;
        for (uint8_t c : fr.bytes) h = (h ^ c) * 16777619u;
        for (auto& r : recent_) if (r.h == h && t_ - r.t < uint64_t(fs_ * 0.5)) return;
        if (hasPend_ && pendH_ == h) {
            if ((pend_.repaired && !fr.repaired) || (pend_.repaired == fr.repaired && fr.quality > pend_.quality))
                { pend_.bytes = fr.bytes; pend_.repaired = fr.repaired; pend_.quality = fr.quality; pend_.slicer = slicer; }
            return;
        }
        if (hasPend_) flush_(out);
        pend_.bytes = fr.bytes; pend_.repaired = fr.repaired; pend_.quality = fr.quality; pend_.slicer = slicer;
        pendH_ = h; hasPend_ = true; pendDue_ = t_ + uint64_t(fs_ / baud_ * 3) + 1;
    }
    template <class Out> void flush_(Out& out) {
        hasPend_ = false;
        recent_[rp_] = { pendH_, t_ }; rp_ = (rp_ + 1) % kRecent;
        out(pend_);
    }

    int baud_, decim_ = 4, dph_ = 0, L_ = 10;
    double fs_ = 12000;
    std::vector<float> lp_, hist_;
    int hp_ = 0;
    std::vector<Pair> pairs_;
    std::vector<Slicer> sl_;
    float peakA_ = 0.001f;
    uint64_t t_ = 0;
    Frame pend_; uint32_t pendH_ = 0; bool hasPend_ = false; uint64_t pendDue_ = 0;
    struct Rec { uint32_t h = 0; uint64_t t = 0; };
    static constexpr int kRecent = 8;
    Rec recent_[kRecent];
    int rp_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE APRS FORMATS (APRS 1.0.1)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Weather {
    double windDir = NAN, windMph = NAN, gustMph = NAN, tempF = NAN, rain1hIn = NAN, rain24hIn = NAN, rainMidIn = NAN,
           hum = NAN, baroMbar = NAN, lum = NAN;
    bool any() const { return windDir == windDir || windMph == windMph || tempF == tempF || hum == hum || baroMbar == baroMbar || rain1hIn == rain1hIn; }
};

struct Packet {
    std::string kind;            ///< position | weather | object | item | message | ack | bulletin | status | telemetry | mice | nmea | other
    std::string from;            ///< the AX.25 source (or the third party's)
    std::string name;            ///< an object/item name ("" = the station itself)
    bool killed = false;         ///< an object/item removed
    bool hasPos = false;
    double lat = NAN, lon = NAN;
    int ambiguity = 0;           ///< digits blanked (position ambiguity)
    char symTable = '/', symCode = ' ';
    double course = NAN, speedKn = NAN, altFt = NAN;
    std::string comment, status, micE;
    Weather wx;
    // messages
    std::string to, text, msgId;
    // PHG
    int phgPower = -1, phgHeight = -1, phgGain = -1, phgDir = -1;
    std::string path, raw;
    bool messaging = false;      ///< '=' / '@': the station accepts messages
};

inline int digitsAt(const std::string& s, size_t at, int n, bool& ok, int& blanks) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
        const char c = at + size_t(i) < s.size() ? s[at + size_t(i)] : 'x';
        if (c == ' ') { ++blanks; v = v * 10; continue; }
        if (c < '0' || c > '9') { ok = false; return 0; }
        v = v * 10 + (c - '0');
    }
    return v;
}
/** "DDMM.hhN" + table + "DDDMM.hhW" + code — 19 characters. */
inline bool parseUncompressed(const std::string& s, size_t at, Packet& p) {
    if (s.size() < at + 19) return false;
    bool ok = true; int bl = 0;
    const int la = digitsAt(s, at, 2, ok, bl), lm = digitsAt(s, at + 2, 2, ok, bl);
    if (s[at + 4] != '.') return false;
    const int lh = digitsAt(s, at + 5, 2, ok, bl);
    const char ns = s[at + 7];
    const int amb = bl;
    const int oa = digitsAt(s, at + 9, 3, ok, bl), om = digitsAt(s, at + 12, 2, ok, bl);
    if (s[at + 14] != '.') return false;
    const int oh = digitsAt(s, at + 15, 2, ok, bl);
    const char ew = s[at + 17];
    if (!ok || (ns != 'N' && ns != 'S' && ns != 'n' && ns != 's') || (ew != 'E' && ew != 'W' && ew != 'e' && ew != 'w')) return false;
    if (la > 90 || lm > 59 || oa > 180 || om > 59) return false;
    double lat = la + (lm + lh / 100.0) / 60.0, lon = oa + (om + oh / 100.0) / 60.0;
    // ★ position ambiguity: the blanked digits put the fix in the middle of the square they leave
    if (amb >= 4) { lat += 0.5; lon += 0.5; } else if (amb == 3) { lat += 5.0 / 60; lon += 5.0 / 60; }
    else if (amb == 2) { lat += 0.5 / 60; lon += 0.5 / 60; } else if (amb == 1) { lat += 0.05 / 60; lon += 0.05 / 60; }
    if (ns == 'S' || ns == 's') lat = -lat;
    if (ew == 'W' || ew == 'w') lon = -lon;
    if (lat > 90 || lat < -90 || lon > 180 || lon < -180) return false;
    p.lat = lat; p.lon = lon; p.hasPos = true; p.ambiguity = amb;
    p.symTable = s[at + 8]; p.symCode = s[at + 18];
    return true;
}
inline double b91(const std::string& s, size_t at, int n, bool& ok) {
    double v = 0;
    for (int i = 0; i < n; ++i) {
        const int c = at + size_t(i) < s.size() ? (unsigned char)s[at + size_t(i)] - 33 : -1;
        if (c < 0 || c > 90) { ok = false; return 0; }
        v = v * 91 + c;
    }
    return v;
}
/** Compressed: table, YYYY, XXXX, code, cs, T — 13 characters. */
inline bool parseCompressed(const std::string& s, size_t at, Packet& p) {
    if (s.size() < at + 13) return false;
    const char tbl = s[at];
    if (!(tbl == '/' || tbl == '\\' || (tbl >= 'A' && tbl <= 'Z') || (tbl >= 'a' && tbl <= 'j'))) return false;
    bool ok = true;
    const double y = b91(s, at + 1, 4, ok), x = b91(s, at + 5, 4, ok);
    if (!ok) return false;
    p.lat = 90 - y / 380926.0; p.lon = -180 + x / 190463.0;
    if (p.lat > 90 || p.lat < -90 || p.lon > 180 || p.lon < -180) return false;
    p.hasPos = true;
    // ★ the overlay letters a–j stand for the digits 0–9 in a compressed position
    p.symTable = (tbl >= 'a' && tbl <= 'j') ? char('0' + (tbl - 'a')) : tbl;
    p.symCode = s[at + 9];
    const char c = s[at + 10], sp = s[at + 11], t = s[at + 12];
    if (c != ' ' && t >= 33) {
        const int T = t - 33;
        if (((T >> 3) & 3) == 2 && c >= 33 && sp >= 33) p.altFt = std::pow(1.002, (c - 33) * 91 + (sp - 33));
        else if (c >= '!' && c <= 'z' && sp >= 33) { p.course = (c - 33) * 4.0; p.speedKn = std::pow(1.08, sp - 33) - 1; if (p.course == 0) p.course = NAN; }
    }
    return true;
}
/** Weather fields "c…s…g…t…r…p…P…h…b…" (any order after the first two, as APRS 1.0.1 ch. 12 allows). */
inline void parseWeather(const std::string& s, size_t at, Weather& w, size_t* end = nullptr) {
    size_t i = at;
    auto num = [&](size_t a, int n, double& out) {
        if (a + size_t(n) > s.size()) return false;
        std::string v = s.substr(a, size_t(n));
        bool dig = false;
        for (char c : v) { if ((c >= '0' && c <= '9')) dig = true; else if (c != '-' && c != ' ' && c != '.') return false; }
        if (dig) out = std::atof(v.c_str());
        return true;
    };
    while (i < s.size()) {
        const char k = s[i];
        int n = 0; double* dst = nullptr;
        switch (k) {
            case 'c': n = 3; dst = &w.windDir; break;   case 's': n = 3; dst = &w.windMph; break;
            case 'g': n = 3; dst = &w.gustMph; break;   case 't': n = 3; dst = &w.tempF; break;
            case 'r': n = 3; dst = &w.rain1hIn; break;  case 'p': n = 3; dst = &w.rain24hIn; break;
            case 'P': n = 3; dst = &w.rainMidIn; break; case 'h': n = 2; dst = &w.hum; break;
            case 'b': n = 5; dst = &w.baroMbar; break;  case 'L': n = 3; dst = &w.lum; break;
            case 'l': n = 3; dst = &w.lum; break;
            default: break;
        }
        if (!dst) break;
        double v = NAN;
        if (!num(i + 1, n, v)) break;
        if (k == 'l' && v == v) v += 1000;
        if (k == 'r' || k == 'p' || k == 'P') v = v == v ? v / 100.0 : v;
        if (k == 'b') v = v == v ? v / 10.0 : v;
        if (k == 'h' && v == 0) v = 100;
        *dst = v;
        i += size_t(n) + 1;
    }
    if (end) *end = i;
}
/** After a position: CSE/SPD, PHG, and for the weather symbol the wind "ddd/sss" then the weather fields. */
inline void parseExtension(const std::string& s, size_t at, Packet& p) {
    size_t i = at;
    const bool wxSym = p.symCode == '_';
    if (s.size() >= i + 7 && s[i + 3] == '/' && std::isdigit((unsigned char)s[i + 4]) | (s[i + 4] == '.')) {
        bool ok = true; int bl = 0;
        const int a = digitsAt(s, i, 3, ok, bl), b = digitsAt(s, i + 4, 3, ok, bl);
        if (ok || (s.compare(i, 3, "...") == 0)) {
            if (wxSym) { if (ok) { p.wx.windDir = a; p.wx.windMph = b; } }
            else if (ok) { if (a > 0 && a <= 360) p.course = a % 360; p.speedKn = b; }
            i += 7;
        }
    } else if (s.size() >= i + 7 && s.compare(i, 3, "PHG") == 0) {
        p.phgPower = s[i + 3] - '0'; p.phgHeight = s[i + 4] - '0'; p.phgGain = s[i + 5] - '0'; p.phgDir = s[i + 6] - '0';
        i += 7;
    }
    if (wxSym) { size_t e = i; parseWeather(s, i, p.wx, &e); i = e; }
    p.comment = i < s.size() ? s.substr(i) : "";
}
/** "/A=001234" in a comment: the altitude in feet, taken out of the comment. */
inline void takeAltitude(Packet& p) {
    const size_t a = p.comment.find("/A=");
    if (a == std::string::npos || p.comment.size() < a + 9) return;
    bool ok = true; int bl = 0;
    const bool neg = p.comment[a + 3] == '-';
    const int v = digitsAt(p.comment, a + (neg ? 4 : 3), neg ? 5 : 6, ok, bl);
    if (!ok) return;
    p.altFt = neg ? -v : v;
    p.comment.erase(a, 9);
}
/** "!DAO!" (APRS 1.1): one more digit of latitude and longitude — applied, and taken out of the comment. */
inline void takeDao(Packet& p) {
    const size_t a = p.comment.find('!');
    if (a == std::string::npos || p.comment.size() < a + 5 || p.comment[a + 4] != '!' || !p.hasPos) return;
    const char d = p.comment[a + 1], la = p.comment[a + 2], lo = p.comment[a + 3];
    double dlat = 0, dlon = 0;
    if (d == 'W' && std::isdigit((unsigned char)la) && std::isdigit((unsigned char)lo)) { dlat = (la - '0') * 0.001 / 60; dlon = (lo - '0') * 0.001 / 60; }
    else if (d == 'w' && la >= 33 && la <= 123 && lo >= 33 && lo <= 123) { dlat = (la - 33) / 91.0 * 0.01 / 60; dlon = (lo - 33) / 91.0 * 0.01 / 60; }
    else return;
    p.lat += p.lat >= 0 ? dlat : -dlat; p.lon += p.lon >= 0 ? dlon : -dlon;
    p.comment.erase(a, 5);
}
inline void trimComment(std::string& c) {
    std::string o;
    for (unsigned char ch : c) if (ch >= 0x20 && ch != 0x7f) o += char(ch);   // ★ printable only (UTF-8 kept)
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(0, 1);
    c = o.size() > 200 ? o.substr(0, 200) : o;
}
/** Position (uncompressed or compressed) at `at`, then its extension and comment. */
inline bool parsePositionAt(const std::string& s, size_t at, Packet& p) {
    if (at < s.size() && (std::isdigit((unsigned char)s[at]) || s[at] == ' ')) {
        if (!parseUncompressed(s, at, p)) return false;
        parseExtension(s, at + 19, p);
    } else {
        if (!parseCompressed(s, at, p)) return false;
        if (p.symCode == '_') { size_t e = at + 13; parseWeather(s, at + 13, p.wx, &e); p.comment = e < s.size() ? s.substr(e) : ""; }
        else p.comment = s.size() > at + 13 ? s.substr(at + 13) : "";
    }
    takeAltitude(p); takeDao(p); trimComment(p.comment);
    if (p.symCode == '_' || p.wx.any()) p.kind = p.kind == "object" || p.kind == "item" ? p.kind : "weather";
    return true;
}

/** Mic-E (APRS 1.0.1 ch. 10): the latitude and message bits in the destination, the rest in the info field. */
inline bool parseMicE(const std::string& dest, const std::string& s, Packet& p) {
    if (dest.size() < 6 || s.size() < 9) return false;
    int dig[6]; int bits[3] = {0, 0, 0}; bool custom = false;
    int blanks = 0;
    for (int i = 0; i < 6; ++i) {
        const char c = dest[size_t(i)];
        if (c >= '0' && c <= '9') dig[i] = c - '0';
        else if (c >= 'A' && c <= 'J') { dig[i] = c - 'A'; if (i < 3) { bits[i] = 1; custom = true; } }
        else if (c >= 'P' && c <= 'Y') { dig[i] = c - 'P'; if (i < 3) bits[i] = 1; }
        else if (c == 'K' || c == 'L' || c == 'Z') { dig[i] = 0; ++blanks; if (i < 3 && c != 'L') { bits[i] = 1; if (c == 'K') custom = true; } }
        else return false;
    }
    const bool north = dest[3] >= 'P', lonOff = dest[4] >= 'P', west = dest[5] >= 'P';
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
    if (!north) lat = -lat;
    int d = (unsigned char)s[1] - 28;
    if (lonOff) d += 100;
    if (d >= 180 && d <= 189) d -= 80; else if (d >= 190 && d <= 199) d -= 190;
    int m = (unsigned char)s[2] - 28; if (m >= 60) m -= 60;
    const int h = (unsigned char)s[3] - 28;
    if (d < 0 || d > 180 || m < 0 || m > 59 || h < 0 || h > 99) return false;
    double lon = d + (m + h / 100.0) / 60.0;
    if (west) lon = -lon;
    if (std::fabs(lat) > 90) return false;
    const int sp = (unsigned char)s[4] - 28, dc = (unsigned char)s[5] - 28, se = (unsigned char)s[6] - 28;
    int speed = sp * 10 + dc / 10; if (speed >= 800) speed -= 800;
    int course = (dc % 10) * 100 + se; if (course >= 400) course -= 400;
    p.lat = lat; p.lon = lon; p.hasPos = true; p.ambiguity = blanks;
    if (speed > 0) p.speedKn = speed;
    if (course > 0 && course <= 360) p.course = course % 360;
    p.symCode = s[7]; p.symTable = s[8];
    static const char* stdMsg[8] = { "Emergency", "Priority", "Special", "Committed", "Returning", "In Service", "En Route", "Off Duty" };
    const int code = bits[0] * 4 + bits[1] * 2 + bits[2];
    p.micE = code == 0 ? "Emergency" : custom ? "Custom " + std::to_string(7 - code) : stdMsg[code];
    std::string rest = s.substr(9);
    // ★ the base-91 altitude "xxx}" (metres + 10 000), wherever it sits near the start
    const size_t br = rest.find('}');
    if (br != std::string::npos && br >= 3 && br <= 4) {
        bool ok = true; const double a = b91(rest, br - 3, 3, ok);
        if (ok) { p.altFt = (a - 10000) * 3.28084; rest.erase(0, br + 1); }
    }
    // ★ the leading type byte of some trackers (`>` `]` …) is a device mark, not text
    if (!rest.empty() && (rest[0] == '>' || rest[0] == ']' || rest[0] == '`' || rest[0] == '\'')) rest.erase(0, 1);
    if (!rest.empty() && (rest.back() == '=' || rest.back() == '^')) rest.pop_back();
    p.comment = rest;
    takeDao(p); trimComment(p.comment);
    p.kind = "mice";
    return true;
}

/** $GPRMC / $GPGGA (raw GPS, APRS 1.0.1 ch. 7) — the fix only. */
inline bool parseNmea(const std::string& s, Packet& p) {
    std::vector<std::string> f; std::string cur;
    for (char c : s) { if (c == ',' || c == '*') { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur);
    auto ll = [](const std::string& v, const std::string& h, int degDigits, double& out) {
        if (v.size() < size_t(degDigits) + 2) return false;
        const double deg = std::atof(v.substr(0, size_t(degDigits)).c_str()), mn = std::atof(v.substr(size_t(degDigits)).c_str());
        out = deg + mn / 60; if (h == "S" || h == "W") out = -out; return true;
    };
    double lat, lon;
    if (f.size() >= 10 && (f[0] == "$GPRMC" || f[0] == "$GNRMC") && f[2] == "A" && ll(f[3], f[4], 2, lat) && ll(f[5], f[6], 3, lon)) {
        p.lat = lat; p.lon = lon; p.hasPos = true;
        if (!f[7].empty()) p.speedKn = std::atof(f[7].c_str());
        if (!f[8].empty()) p.course = std::atof(f[8].c_str());
    } else if (f.size() >= 10 && (f[0] == "$GPGGA" || f[0] == "$GNGGA") && f[6] != "0" && ll(f[2], f[3], 2, lat) && ll(f[4], f[5], 3, lon)) {
        p.lat = lat; p.lon = lon; p.hasPos = true;
        if (!f[9].empty()) p.altFt = std::atof(f[9].c_str()) * 3.28084;
    } else return false;
    p.symTable = '/'; p.symCode = '>';
    p.kind = "nmea";
    return std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180;
}

/** Parse one APRS info field from `from` to `dest`. Always fills kind (other = shown raw). */
inline void parseInfo(const std::string& from, const std::string& dest, const std::string& info, Packet& p, int depth = 0) {
    p.from = from; p.kind = "other";
    if (info.empty()) return;
    const char dti = info[0];
    switch (dti) {
        case '!': case '=': {
            p.kind = "position"; p.messaging = dti == '=';
            // ★ APRS 1.0.1: '!' may follow up to 40 characters of a beacon's text; the common case is at 1
            if (!parsePositionAt(info, 1, p)) p.kind = "other";
            return;
        }
        case '/': case '@': {
            p.kind = "position"; p.messaging = dti == '@';
            if (info.size() < 9 || !parsePositionAt(info, 8, p)) p.kind = "other";
            return;
        }
        case ';': {
            if (info.size() < 18 + 13) return;
            p.kind = "object"; p.name = info.substr(1, 9);
            while (!p.name.empty() && p.name.back() == ' ') p.name.pop_back();
            p.killed = info[10] == '_';
            if (!parsePositionAt(info, 18, p)) { p.kind = "other"; p.name.clear(); }
            return;
        }
        case ')': {
            size_t e = 1;
            while (e < info.size() && e <= 10 && info[e] != '!' && info[e] != '_') ++e;
            if (e >= info.size() || e < 4) return;
            p.kind = "item"; p.name = info.substr(1, e - 1); p.killed = info[e] == '_';
            if (!parsePositionAt(info, e + 1, p)) { p.kind = "other"; p.name.clear(); }
            return;
        }
        case ':': {
            if (info.size() < 11 || info[10] != ':') return;
            p.to = info.substr(1, 9);
            while (!p.to.empty() && p.to.back() == ' ') p.to.pop_back();
            std::string t = info.substr(11);
            if (t.compare(0, 3, "ack") == 0 || t.compare(0, 3, "rej") == 0) { p.kind = "ack"; p.msgId = t.substr(3); p.text = t.substr(0, 3); return; }
            const size_t br = t.rfind('{');
            if (br != std::string::npos && t.size() - br <= 6) { p.msgId = t.substr(br + 1); t.resize(br); }
            p.text = t; trimComment(p.text);
            p.kind = p.to.compare(0, 3, "BLN") == 0 ? "bulletin" : (p.to.compare(0, 3, "PAR") == 0 || p.to.compare(0, 3, "UNI") == 0 || p.to.compare(0, 3, "EQN") == 0 || p.to.compare(0, 3, "BIT") == 0) ? "telemetry" : "message";
            return;
        }
        case '>': {
            p.kind = "status";
            std::string t = info.substr(1);
            if (t.size() >= 7 && t[6] == 'z' && std::all_of(t.begin(), t.begin() + 6, [](char c) { return std::isdigit((unsigned char)c); })) t.erase(0, 7);
            p.status = t; trimComment(p.status);
            return;
        }
        case '`': case '\'': case 0x1c: case 0x1d:
            if (!parseMicE(dest, info, p)) p.kind = "other";
            return;
        case '_': {
            // positionless weather: MDHM then c…s…
            if (info.size() < 9) return;
            p.kind = "weather";
            parseWeather(info, 9, p.wx);
            return;
        }
        case 'T': p.kind = "telemetry"; p.text = info; trimComment(p.text); return;
        case '$': if (parseNmea(info, p)) return; p.kind = "other"; return;
        case '}': {
            // ★ third party: an embedded TNC2 packet (an internet gateway, a satellite) — parsed as its own source
            if (depth > 1) return;
            const size_t gt = info.find('>'), co = info.find(':');
            if (gt == std::string::npos || co == std::string::npos || gt > co) return;
            const std::string src = info.substr(1, gt - 1);
            std::string dst = info.substr(gt + 1, co - gt - 1);
            const size_t cm = dst.find(','); if (cm != std::string::npos) dst.resize(cm);
            parseInfo(src, dst, info.substr(co + 1), p, depth + 1);
            p.comment = p.comment.empty() ? "via " + from : p.comment + " (via " + from + ")";
            return;
        }
        case '<': p.kind = "capabilities"; p.text = info.substr(1); trimComment(p.text); return;
        default: break;
    }
    // ★ APRS 1.0.1: a '!' anywhere in the first 40 characters still starts a position (old TNC beacons)
    const size_t bang = info.find('!');
    if (bang != std::string::npos && bang < 40 && bang + 1 < info.size() && parsePositionAt(info, bang + 1, p)) { p.kind = "position"; return; }
    p.status = info; trimComment(p.status);
}

inline Packet parsePacket(const Ax25& f) {
    Packet p;
    parseInfo(addrText(f.src), f.dest.call, f.info, p);
    std::string path;
    int lastH = -1;
    for (size_t i = 0; i < f.digis.size(); ++i) if (f.digis[i].h) lastH = int(i);
    for (size_t i = 0; i < f.digis.size(); ++i) { if (i) path += ","; path += addrText(f.digis[i]); if (int(i) == lastH) path += "*"; }
    p.path = path;
    p.raw = tnc2(f);
    return p;
}

// ── readable text ──────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string fmt(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }
/** ★ A short name for the symbol (APRS 1.0.1 Appendix 2, primary and alternate tables) — what the card says. */
inline std::string symbolName(char table, char code) {
    const bool alt = table != '/';
    switch (code) {
        case '!': return alt ? "emergency" : "police station";
        case '#': return alt ? "digipeater (overlay)" : "digipeater";
        case '$': return alt ? "bank" : "phone";
        case '&': return alt ? "gateway" : "HF gateway";
        case '\'': return alt ? "crash site" : "small aircraft";
        case '-': return alt ? "house (HF)" : "house";
        case '.': return "X";
        case '/': return "dot";
        case '<': return alt ? "advisory" : "motorcycle";
        case '=': return alt ? "railway" : "railway engine";
        case '>': return alt ? "car" : "car";
        case '?': return alt ? "info kiosk" : "server";
        case '@': return alt ? "hurricane" : "hurricane";
        case 'A': return alt ? "box" : "aid station";
        case 'C': return alt ? "coastguard" : "canoe";
        case 'E': return alt ? "smoke" : "eyeball";
        case 'F': return alt ? "freezing rain" : "farm vehicle";
        case 'H': return alt ? "haze" : "hotel";
        case 'I': return alt ? "rain shower" : "TCP/IP";
        case 'K': return alt ? "kenwood" : "school";
        case 'O': return alt ? "balloon" : "balloon";
        case 'P': return alt ? "parking" : "police";
        case 'R': return alt ? "restaurant" : "RV";
        case 'S': return alt ? "satellite" : "shuttle";
        case 'U': return alt ? "sunny" : "bus";
        case 'X': return alt ? "pharmacy" : "helicopter";
        case 'Y': return alt ? "radios" : "yacht";
        case '[': return alt ? "wall cloud" : "jogger";
        case '^': return alt ? "aircraft" : "large aircraft";
        case '_': return alt ? "weather site" : "weather station";
        case 'a': return alt ? "ARRL / ARES" : "ambulance";
        case 'b': return alt ? "blowing dust" : "bicycle";
        case 'f': return alt ? "funnel cloud" : "fire engine";
        case 'j': return alt ? "work zone" : "jeep";
        case 'k': return alt ? "SUV" : "truck";
        case 'l': return alt ? "area" : "laptop";
        case 'n': return alt ? "node" : "node";
        case 'r': return alt ? "restrooms" : "repeater";
        case 's': return alt ? "ship" : "boat";
        case 'u': return alt ? "truck" : "truck";
        case 'v': return alt ? "van" : "van";
        case 'y': return alt ? "skywarn" : "Yagi at QTH";
        default: return "";
    }
}
/** The station list's line: "M0ABC-9 · car · 45 km/h NE · 120 m · Mic-E En Route · going to the club". Metric. */
inline std::string readable(const Packet& p) {
    static const std::string dot = " \xc2\xb7 ";
    std::string s = p.name.empty() ? p.from : p.name + " (" + p.from + ")";
    if (p.killed) return s + dot + "removed";
    if (p.kind == "message" || p.kind == "bulletin") return s + " \xe2\x86\x92 " + p.to + ": " + p.text;
    if (p.kind == "ack") return s + " \xe2\x86\x92 " + p.to + ": " + p.text + " " + p.msgId;
    if (p.kind == "status") return s + dot + "status: " + p.status;
    if (p.kind == "telemetry") return s + dot + "telemetry";
    if (p.kind == "capabilities") return s + dot + "capabilities: " + p.text;
    const std::string sym = symbolName(p.symTable, p.symCode);
    if (!sym.empty() && p.hasPos) s += dot + sym;
    if (p.wx.any()) {
        const Weather& w = p.wx;
        if (w.tempF == w.tempF) s += dot + fmt((w.tempF - 32) * 5 / 9, 1) + " \xc2\xb0" "C";
        if (w.hum == w.hum) s += dot + fmt(w.hum, 0) + " % RH";
        if (w.windMph == w.windMph) {
            s += dot + "wind " + fmt(w.windMph * 1.609344, 0) + " km/h";
            if (w.windDir == w.windDir && w.windMph > 0) { static const char* c8[8] = {"N","NE","E","SE","S","SW","W","NW"}; s += std::string(" ") + c8[int(std::floor(std::fmod(w.windDir + 22.5, 360) / 45)) & 7]; }
        }
        if (w.gustMph == w.gustMph) s += dot + "gust " + fmt(w.gustMph * 1.609344, 0) + " km/h";
        if (w.rain1hIn == w.rain1hIn && w.rain1hIn > 0) s += dot + "rain " + fmt(w.rain1hIn * 25.4, 1) + " mm/h";
        if (w.rainMidIn == w.rainMidIn) s += dot + fmt(w.rainMidIn * 25.4, 1) + " mm today";
        if (w.baroMbar == w.baroMbar) s += dot + fmt(w.baroMbar, 1) + " hPa";
    }
    if (p.speedKn == p.speedKn && p.speedKn >= 1) {
        s += dot + fmt(p.speedKn * 1.852, 0) + " km/h";
        if (p.course == p.course) { static const char* c8[8] = {"N","NE","E","SE","S","SW","W","NW"}; s += std::string(" ") + c8[int(std::floor(std::fmod(p.course + 22.5, 360) / 45)) & 7]; }
    }
    if (p.altFt == p.altFt) s += dot + fmt(p.altFt * 0.3048, 0) + " m";
    if (!p.micE.empty()) s += dot + "Mic-E " + p.micE;
    if (!p.comment.empty()) s += dot + p.comment;
    if (p.kind == "other" && !p.status.empty()) s += dot + p.status;
    return s;
}

/** ★ The region's APRS frequency from the receiver's position (IARU regions and the national exceptions everyone uses):
 *  the Americas 144.390 (Brazil 145.570, Argentina 144.930), Australia 145.175, New Zealand 144.575, Japan 144.640,
 *  China 144.640, Thailand 145.525, South Korea 144.620; the rest of the world 144.800. NaN position → 144.800. */
inline double regionalHz(double lat, double lon) {
    if (!(lat == lat) || !(lon == lon)) return 144.800e6;
    if (lon < -25 && lon > -170) {
        if (lat < -22 && lon > -75 && lon < -53 && lat > -56) return 144.930e6;    // Argentina
        if (lat < 6 && lat > -34 && lon > -74 && lon < -34) return 145.570e6;     // Brazil
        return 144.390e6;
    }
    if (lat < -9 && lat > -45 && lon > 112 && lon < 155) return 145.175e6;        // Australia
    if (lat < -33 && lat > -48 && lon > 165 && lon < 179) return 144.575e6;       // New Zealand
    if (lat > 24 && lat < 46 && lon > 128 && lon < 146) return 144.640e6;         // Japan
    if (lat > 33 && lat < 39 && lon > 124 && lon < 130) return 144.620e6;         // South Korea
    if (lat > 18 && lat < 54 && lon > 73 && lon < 128) return 144.640e6;          // China
    if (lat > 5 && lat < 21 && lon > 97 && lon < 106) return 145.525e6;           // Thailand
    return 144.800e6;
}

} // namespace vibeaprs
