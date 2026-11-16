// balloon_ukhas.h — V12 high-altitude balloons, part 1: the UKHAS TELEMETRY SENTENCE (docs/v12/BALLOONS.md).
//
// The UK High Altitude Society's sentence (ukhas.org.uk, "communication:protocol"), as amateur balloon payloads send it
// over RTTY on 434 MHz and as the Horus Binary decoder writes it for its own packets:
//
//     $$CALLSIGN,sentence_id,time,latitude,longitude,altitude[,anything else…]*CHECKSUM\n
//
//   • "$$" (one or more '$') starts it; the CHECKSUM is CRC-16-CCITT (polynomial 0x1021, initial 0xFFFF, no reflection,
//     no final XOR) over every character between the "$$" and the '*', written as four hex digits.
//   • time "HH:MM:SS" (or "HHMMSS"), latitude / longitude in decimal degrees, altitude in metres.
//   ★ Only the CRC-16 form is believed. The older two-hex-digit XOR checksum is NOT accepted: one chance in 256 of a
//     garbled line passing it is a false position on a map every few minutes of noise, and a position is exactly what a
//     listener chases a balloon by. A sentence that fails its CRC is counted, never shown as telemetry.
//   ★ Bounded: a sentence is at most kMaxLen characters; anything longer is dropped (the framer never grows).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace balloon {

/** CRC-16-CCITT (0x1021, initial 0xFFFF, MSB first, no final XOR) — the UKHAS sentence's and Horus Binary's checksum. */
inline uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
inline uint16_t crc16(const std::string& s) { return crc16((const uint8_t*)s.data(), s.size()); }

/** One position report, from either decoder. NaN / -1 = not sent. */
struct Telemetry {
    std::string source;          ///< "ukhas" (RTTY sentence) or "horus" (Horus Binary)
    std::string callsign;        ///< the payload's name ("HORUS-256" for a Horus payload ID the server cannot name)
    long   count = -1;           ///< sentence / frame counter
    int    hh = -1, mm = -1, ss = -1;
    double lat = NAN, lon = NAN, alt = NAN;   ///< degrees, degrees, metres (lat/lon NaN = no fix)
    double speedKmh = NAN, tempC = NAN, battV = NAN;
    int    sats = -1;
    std::vector<std::string> extra;          ///< UKHAS: the fields after the altitude, as sent
    std::string custom;          ///< Horus v2: the nine custom bytes, hex
    std::string sentence;        ///< UKHAS: the whole line as received; Horus: the UKHAS line it is written as
    int    horusId = -1;         ///< Horus payload ID
    int    horusVersion = 0;     ///< 1 or 2
    int    fecFixed = 0;         ///< Horus: bits Golay corrected
};

/** "12:34:56" or "123456" → h, m, s (false = not a time). */
inline bool parseTime(const std::string& t, int& h, int& m, int& s) {
    int a = -1, b = -1, c = -1;
    if (t.size() == 8 && t[2] == ':' && t[5] == ':') {
        for (int i : { 0, 1, 3, 4, 6, 7 }) if (t[i] < '0' || t[i] > '9') return false;
        a = (t[0] - '0') * 10 + (t[1] - '0'); b = (t[3] - '0') * 10 + (t[4] - '0'); c = (t[6] - '0') * 10 + (t[7] - '0');
    } else if (t.size() == 6) {
        for (char ch : t) if (ch < '0' || ch > '9') return false;
        a = (t[0] - '0') * 10 + (t[1] - '0'); b = (t[2] - '0') * 10 + (t[3] - '0'); c = (t[4] - '0') * 10 + (t[5] - '0');
    } else return false;
    if (a > 23 || b > 59 || c > 60) return false;
    h = a; m = b; s = c;
    return true;
}
/** A decimal number, all of `f` (false = empty or anything else in it). */
inline bool parseNum(const std::string& f, double& v) {
    if (f.empty() || f.size() > 24) return false;
    char* e = nullptr;
    v = std::strtod(f.c_str(), &e);
    return e && *e == 0 && std::isfinite(v);
}
inline bool callsignOk(const std::string& c) {
    if (c.empty() || c.size() > 20) return false;
    for (unsigned char ch : c)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '/'))
            return false;
    return true;
}

/** ★ A whole line → telemetry. `line` may hold anything before the "$$" (the RTTY text in front of it); the sentence is
 *  the LAST "$…$" run's text up to the '*'. Returns 1 = a valid sentence (out filled), 0 = not a sentence at all,
 *  -1 = a sentence whose CRC (or fields) failed. */
inline int parseSentence(const std::string& line, Telemetry& out) {
    const size_t star = line.rfind('*');
    if (star == std::string::npos) return 0;
    size_t d = line.rfind("$$", star);
    if (d == std::string::npos) return 0;
    while (d > 0 && line[d - 1] == '$') --d;
    size_t b = d; while (b < star && line[b] == '$') ++b;
    const std::string body = line.substr(b, star - b);
    if (body.size() < 10) return 0;
    for (unsigned char ch : body) if (ch < 0x20 || ch > 0x7e) return -1;
    // the four hex digits after the '*'
    if (line.size() < star + 5) return -1;
    unsigned want = 0;
    for (size_t i = star + 1; i < star + 5; ++i) {
        const char ch = line[i];
        const int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
        if (v < 0) return -1;
        want = (want << 4) | (unsigned)v;
    }
    if (crc16(body) != want) return -1;
    // fields
    std::vector<std::string> f;
    { size_t p = 0; for (;;) { const size_t c = body.find(',', p); f.push_back(body.substr(p, c == std::string::npos ? std::string::npos : c - p)); if (c == std::string::npos) break; p = c + 1; if (f.size() > 64) return -1; } }
    if (f.size() < 6 || !callsignOk(f[0])) return -1;
    Telemetry t;
    t.source = "ukhas";
    t.callsign = f[0];
    { double c; if (parseNum(f[1], c) && c >= 0 && c < 1e9 && c == std::floor(c)) t.count = (long)c; }
    parseTime(f[2], t.hh, t.mm, t.ss);
    double la, lo, al;
    if (parseNum(f[3], la) && parseNum(f[4], lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180 && !(la == 0 && lo == 0)) { t.lat = la; t.lon = lo; }
    if (parseNum(f[5], al) && al > -1000 && al < 100000) t.alt = al;
    for (size_t i = 6; i < f.size(); ++i) t.extra.push_back(f[i]);
    t.sentence = line.substr(d, star + 5 - d);
    out = std::move(t);
    return 1;
}

/** ★ The UKHAS line for some fields (our own transmitter, and how a Horus packet is written out): "$$" + body + "*" +
 *  the CRC in hex + "\n". */
inline std::string makeSentence(const std::string& body) {
    char c[8]; std::snprintf(c, sizeof c, "*%04X\n", crc16(body));
    return "$$" + body + c;
}

/** ★ One character stream → sentences. Characters go in as the RTTY decoder prints them; a sentence is judged at its
 *  newline (or once its four CRC digits are in, whichever comes first). Bounded: kMaxLen characters held at most. */
class SentenceFramer {
public:
    static constexpr size_t kMaxLen = 300;
    std::function<void(const Telemetry&)> onSentence;
    /** A line that looked like a sentence ("$$…*XXXX") and failed its CRC or fields — never telemetry. */
    std::function<void(const std::string&)> onRejected;
    unsigned long good = 0, bad = 0;          ///< sentences that passed / failed their CRC
    void put(char c) {
        if (c == '$') {
            // ★ a '$' after text starts a new line: what was held is judged first (a lost newline loses nothing)
            if (!buf_.empty() && buf_.back() != '$') judge_();
            buf_ += c;
            if (buf_.size() > 8) buf_.erase(0, buf_.size() - 8);   // only '$'s are held here
            return;
        }
        if (buf_.empty()) return;            // nothing until a '$'
        if (buf_.size() == 1) { buf_.clear(); return; }   // one '$' alone is not a start ("$$")
        if (c == '\n' || c == '\r') { judge_(); return; }
        buf_ += c;
        if (buf_.size() > kMaxLen) { buf_.clear(); return; }
        const size_t st = buf_.rfind('*');
        if (st != std::string::npos && buf_.size() >= st + 5) judge_();
    }
    void reset() { buf_.clear(); }
private:
    void judge_() {
        if (buf_.size() >= 12 && buf_.compare(0, 2, "$$") == 0) {
            Telemetry t;
            const int r = parseSentence(buf_, t);
            if (r > 0) { ++good; if (onSentence) onSentence(t); }
            else if (r < 0) { ++bad; if (onRejected) onRejected(buf_); }
        }
        buf_.clear();
    }
    std::string buf_;
};

} // namespace balloon
} // namespace vibe
