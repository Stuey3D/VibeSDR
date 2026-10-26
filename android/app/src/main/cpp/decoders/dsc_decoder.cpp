// VibeSDR V12 — maritime DSC (ITU-R M.493). See dsc_decoder.h for the standard and what this receiver adds to it.
#include "dsc_decoder.h"
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibe_maritime_mid.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace vibe {
namespace dsc {

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The code
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

uint16_t codeword(int v) {
    v &= 127;
    int ones = 0;
    for (int k = 0; k < 7; ++k) ones += (v >> k) & 1;
    const int zeros = 7 - ones;
    uint16_t w = (uint16_t)v;                                   // bits 0–6: the value, least significant first
    w |= (uint16_t)(((zeros >> 2) & 1) << 7);                   // bits 7–9: the count of 0s, most significant first
    w |= (uint16_t)(((zeros >> 1) & 1) << 8);
    w |= (uint16_t)((zeros & 1) << 9);
    return w;
}

namespace {
struct Tables {
    int16_t word[1024];
    float pm[128][10];       // ±1 per bit, +1 = binary 1
    Tables() {
        for (int i = 0; i < 1024; ++i) word[i] = -1;
        for (int v = 0; v < 128; ++v) {
            const uint16_t w = codeword(v);
            word[w] = (int16_t)v;
            for (int k = 0; k < 10; ++k) pm[v][k] = ((w >> k) & 1) ? 1.0f : -1.0f;
        }
    }
};
const Tables& T() { static const Tables t; return t; }

/** Score of every word on soft bits `w` (sum over bits of w·±1). */
inline float score(const float* w, int v) {
    const float* p = T().pm[v];
    float s = 0;
    for (int k = 0; k < 10; ++k) s += w[k] * p[k];
    return s;
}
/** The best and second-best valid words for `w`, optionally only among `allowed`. */
struct Ml { int best = -1, second = -1; float sBest = -1e30f, sSecond = -1e30f, energy = 0; };
Ml ml(const float* w, const int* allowed = nullptr, int nAllowed = 0) {
    Ml r;
    for (int k = 0; k < 10; ++k) r.energy += std::fabs(w[k]);
    const int n = allowed ? nAllowed : 128;
    for (int i = 0; i < n; ++i) {
        const int v = allowed ? allowed[i] : i;
        const float s = score(w, v);
        if (s > r.sBest) { r.second = r.best; r.sSecond = r.sBest; r.best = v; r.sBest = s; }
        else if (s > r.sSecond) { r.second = v; r.sSecond = s; }
    }
    return r;
}
const int kFormats[] = { kGeoArea, kDistress, kGroup, kAllShips, kIndividual, kAutomatic };
const int kEoss[] = { kEosAckRq, kEosAckBq, kEosOther };
} // namespace

int decodeWord(uint16_t w) { return T().word[w & 1023]; }

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The symbols' meanings
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

bool isFormat(int v) { for (int f : kFormats) if (v == f) return true; return false; }
bool isEos(int v) { return v == kEosAckRq || v == kEosAckBq || v == kEosOther; }

const char* formatWord(int v) {
    switch (v) {
        case kGeoArea: return "geographic area";
        case kDistress: return "distress alert";
        case kGroup: return "group";
        case kAllShips: return "all ships";
        case kIndividual: return "individual";
        case kAutomatic: return "semi-automatic / automatic";
        default: return "";
    }
}
const char* categoryWord(int v) {
    switch (v) {
        case kRoutine: return "routine";
        case kBusiness: return "ship's business";
        case kSafety: return "safety";
        case kUrgency: return "urgency";
        case kCatDistress: return "distress";
        default: return "";
    }
}
const char* natureWord(int v) {
    switch (v) {
        case 100: return "Fire, explosion";
        case 101: return "Flooding";
        case 102: return "Collision";
        case 103: return "Grounding";
        case 104: return "Listing, in danger of capsizing";
        case 105: return "Sinking";
        case 106: return "Disabled and adrift";
        case 107: return "Undesignated distress";
        case 108: return "Abandoning ship";
        case 109: return "Piracy / armed robbery attack";
        case 110: return "Man overboard";
        case 112: return "EPIRB emission";
        default: return "";
    }
}
const char* tc1Word(int v) {
    switch (v) {
        case 100: return "F3E/G3E all modes telephony";
        case 101: return "F3E/G3E duplex telephony";
        case 103: return "polling";
        case 104: return "unable to comply";
        case 105: return "end of call";
        case 106: return "data";
        case 109: return "J3E telephony";
        case 110: return "distress acknowledgement";
        case 112: return "distress relay";
        case 113: return "F1B/J2B FEC telex";
        case 115: return "F1B/J2B ARQ telex";
        case 118: return "test";
        case 121: return "ship position";
        case 126: return "no information";
        default: return "";
    }
}
const char* tc2Word(int v) {
    switch (v) {
        case 100: return "no reason given";
        case 101: return "congestion at the switching centre";
        case 102: return "busy";
        case 103: return "queue indication";
        case 104: return "station barred";
        case 105: return "no operator available";
        case 106: return "operator temporarily unavailable";
        case 107: return "equipment disabled";
        case 108: return "unable to use the proposed channel";
        case 109: return "unable to use the proposed mode";
        case 110: return "ships of States not parties to an armed conflict";
        case 111: return "medical transports";
        case 112: return "pay-phone / public call office";
        case 113: return "fax / data (ITU-T V.21)";
        case 126: return "no information";
        default: return "";
    }
}
const char* subsequentWord(int v) {
    switch (v) {
        case 100: return "F3E/G3E telephony";
        case 109: return "J3E telephony";
        case 113: return "F1B/J2B FEC telex";
        case 115: return "F1B/J2B ARQ telex";
        case 126: return "no information";
        default: return "";
    }
}

int Call::level() const {
    if (format == kDistress || category == kCatDistress) return 3;
    if (category == kUrgency) return 2;
    if (category == kSafety) return 1;
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// Reading a call
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace {
bool digitsOf(const std::vector<int>& s, size_t at, int n, std::string& out) {
    out.clear();
    if (at + (size_t)n > s.size()) return false;
    for (int i = 0; i < n; ++i) {
        const int v = s[at + (size_t)i];
        if (v < 0 || v > 99) return false;
        out += (char)('0' + v / 10); out += (char)('0' + v % 10);
    }
    return true;
}
int num(const std::string& d, size_t a, size_t n) { int v = 0; for (size_t i = 0; i < n; ++i) v = v * 10 + (d[a + i] - '0'); return v; }

bool readMmsi(const std::vector<int>& s, size_t& i, std::string& out) {
    std::string d;
    if (!digitsOf(s, i, 5, d)) return false;
    if (d[9] != '0') return false;            // ★ the tenth digit of an MMSI field is always 0 (M.493 §6) — garbage is not
    out = d.substr(0, 9);
    i += 5;
    return true;
}
bool readPosition(const std::vector<int>& s, size_t& i, Position& p) {
    std::string d;
    if (!digitsOf(s, i, 5, d)) return false;
    i += 5;
    p = Position();
    if (d == "9999999999") return true;       // ★ position unknown
    const int q = d[0] - '0';
    const int la = num(d, 1, 2), lam = num(d, 3, 2), lo = num(d, 5, 3), lom = num(d, 8, 2);
    if (q > 3 || la > 90 || lam > 59 || lo > 180 || lom > 59) return false;
    p.known = true; p.quadrant = q;
    p.latDeg = la; p.latMin = lam; p.lonDeg = lo; p.lonMin = lom;
    p.lat = (la + lam / 60.0) * ((q == 2 || q == 3) ? -1 : 1);
    p.lon = (lo + lom / 60.0) * ((q == 1 || q == 3) ? -1 : 1);
    return true;
}
bool readTime(const std::vector<int>& s, size_t& i, int& h, int& m) {
    if (i + 2 > s.size()) return false;
    const int a = s[i], b = s[i + 1];
    if (a == 88 && b == 88) { h = m = -1; i += 2; return true; }
    if (a > 23 || b > 59) return false;
    h = a; m = b; i += 2;
    return true;
}
bool inSet(int v, std::initializer_list<int> set) { for (int x : set) if (v == x) return true; return false; }
bool validCategory(int v) { return inSet(v, { 100, 106, 108, 110, 112 }); }
bool validTc1(int v) { return *tc1Word(v) != 0; }
bool validTc2(int v) { return *tc2Word(v) != 0; }
bool validNature(int v) { return *natureWord(v) != 0; }
bool validSubsequent(int v) { return *subsequentWord(v) != 0; }

/** The distress fields of an alert, a relay or an acknowledgement: nature, position, time, subsequent communications. */
bool readDistressFields(const std::vector<int>& s, size_t& i, Call& c) {
    if (i >= s.size() || !validNature(s[i])) return false;
    c.nature = s[i++];
    if (!readPosition(s, i, c.pos)) return false;
    if (!readTime(s, i, c.utcHour, c.utcMin)) return false;
    if (i >= s.size() || !validSubsequent(s[i])) return false;
    c.subsequent = s[i++];
    return true;
}

/** A frequency field (3 symbols, or 4 when its first digit is 4), "" if it holds no information. */
bool readFreq(const std::vector<int>& s, size_t& i, size_t end, std::string& out) {
    out.clear();
    if (i >= end) return false;
    if (s[i] == 126) {                        // ★ "no information": 126 126 126
        if (i + 3 > end) return false;
        for (int k = 0; k < 3; ++k) if (s[i + (size_t)k] != 126) return false;
        i += 3; return true;
    }
    const int d0 = s[i] / 10;
    const int n = d0 == 4 ? 4 : 3;
    std::string d;
    if (i + (size_t)n > end || !digitsOf(s, i, n, d)) return false;
    i += (size_t)n;
    char b[48];
    if (d0 <= 2) {                            // 100 Hz units (M.493 §8.1)
        std::snprintf(b, sizeof b, "%.1f kHz", num(d, 0, 6) / 10.0);
    } else if (d0 == 4) {                     // 10 Hz units
        std::snprintf(b, sizeof b, "%.2f kHz", num(d, 1, 7) / 100.0);
    } else if (d0 == 3) {                     // an MF/HF working channel number
        std::snprintf(b, sizeof b, "HF channel %d", num(d, 1, 5));
    } else if (d0 == 9) {                     // a VHF channel
        std::snprintf(b, sizeof b, "Ch %02d", num(d, 2, 4));
    } else {
        std::snprintf(b, sizeof b, "frequency %s", d.c_str());
    }
    out = b;
    return true;
}
} // namespace

bool parse(const std::vector<int>& s, Call& c, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    c = Call();
    if (s.size() < 3) return fail("too short");
    c.format = s[0];
    if (!isFormat(c.format)) return fail("not a format specifier");
    c.eos = s.back();
    if (!isEos(c.eos)) return fail("no end of sequence");
    c.symbols = s;
    const size_t end = s.size() - 1;          // the fields are s[1 .. end-1]
    std::vector<int> f(s.begin(), s.begin() + (long)end);   // F + fields (the EOS cut off)
    size_t i = 1;
    if (c.format == kDistress) {
        if (!readMmsi(f, i, c.self)) return fail("self-ID");
        if (!readDistressFields(f, i, c)) return fail("distress fields");
        if (i != end) return fail("length");
        return c.eos == kEosOther ? true : fail("distress alert EOS");
    }
    if (c.format == kGeoArea) {
        std::string d;
        if (!digitsOf(f, i, 5, d)) return fail("area");
        i += 5;
        Area& a = c.area;
        a.quadrant = d[0] - '0'; a.latDeg = num(d, 1, 2); a.lonDeg = num(d, 3, 3); a.dLat = num(d, 6, 2); a.dLon = num(d, 8, 2);
        if (a.quadrant > 3 || a.latDeg > 90 || a.lonDeg > 180) return fail("area values");
        a.valid = true;
        a.lat = a.latDeg * ((a.quadrant == 2 || a.quadrant == 3) ? -1.0 : 1.0);
        a.lon = a.lonDeg * ((a.quadrant == 1 || a.quadrant == 3) ? -1.0 : 1.0);
    } else if (c.format == kGroup || c.format == kIndividual || c.format == kAutomatic) {
        if (!readMmsi(f, i, c.address)) return fail("address");
    }
    if (i >= end || !validCategory(f[i])) return fail("category");
    c.category = f[i++];
    if (!readMmsi(f, i, c.self)) return fail("self-ID");
    if (i >= end || !validTc1(f[i])) return fail("first telecommand");
    c.tc1 = f[i++];
    // ★ A relay or an acknowledgement carries the distressed ship's own fields instead of a second telecommand.
    if (c.category == kCatDistress && (c.tc1 == 110 || c.tc1 == 112) && c.format != kGroup && c.format != kAutomatic) {
        if (!readMmsi(f, i, c.distressId)) return fail("distress ID");
        if (!readDistressFields(f, i, c)) return fail("distress fields");
        if (i != end) return fail("length");
        return true;
    }
    if (i >= end || !validTc2(f[i])) return fail("second telecommand");
    c.tc2 = f[i++];
    /* ★★ THE REST, AS M.493 LAYS IT OUT — and nothing else: "message 2" is TWO frequency fields (3 symbols each, 4 when
     *  the first digit is 4; 126 126 126 when there is nothing to say) or a POSITION (55, five symbols, then its time);
     *  a semi-automatic call then carries the telephone number ("message 3": 105/106 and up to 8 symbols). Read this
     *  strictly, a call cut short by a misread EOS does not pass as a shorter one. */
    if (i < end && f[i] == 55) {
        ++i;
        if (!readPosition(f, i, c.posReply)) return fail("position");
        c.hasPosReply = true;
        if (i + 2 <= end && f[i] <= 99 && f[i + 1] <= 99 && !(c.format == kAutomatic && (f[i] == 105 || f[i] == 106))) {
            if (!readTime(f, i, c.posReplyHour, c.posReplyMin)) return fail("position time");
        }
    } else {
        for (int k = 0; k < 2; ++k) {
            std::string fr;
            if (!readFreq(f, i, end, fr)) return fail("frequency");
            if (!fr.empty()) c.freqs.push_back(fr);
        }
    }
    if (c.format == kAutomatic && i < end) {
        const int v = f[i];
        if (v != 105 && v != 106) return fail("number");
        ++i;
        std::string d;
        const int n = (int)(end - i);
        if (n < 1 || n > 8 || !digitsOf(f, i, n, d)) return fail("number");
        i = end;
        c.number = v == 105 ? d.substr(1) : d;   // ★ 105 = an odd number of digits: the first (padding) 0 goes
    }
    if (i != end) return fail("length");
    return true;
}

bool parseExpansion(const std::vector<int>& s, Call& c) {
    if (s.size() < 2 || !isEos(s.back())) return false;
    size_t i = 0;
    const size_t end = s.size() - 1;
    int read = 0;
    while (i < end) {
        const int spec = s[i++];
        std::string d;
        if (spec == 100) {                    // enhanced position: 4 symbols — lat, then lon, ten-thousandths of a minute
            if (!digitsOf(s, i, 4, d)) return false;
            i += 4;
            Position& p = c.hasPosReply && !c.pos.known ? c.posReply : c.pos;
            if (p.known) {
                p.latFrac = num(d, 0, 4); p.lonFrac = num(d, 4, 4);
                const double la = p.latDeg + (p.latMin + p.latFrac / 10000.0) / 60.0;
                const double lo = p.lonDeg + (p.lonMin + p.lonFrac / 10000.0) / 60.0;
                p.lat = la * (p.lat < 0 ? -1 : 1); p.lon = lo * (p.lon < 0 ? -1 : 1);
            }
        } else if (spec == 102 || spec == 103 || spec == 106) {   // speed (0.1 kn) · course (0.1°) · persons on board
            if (!digitsOf(s, i, 2, d)) return false;
            i += 2;
            const int v = num(d, 0, 4);
            if (spec == 102) c.speedKn = v / 10.0;
            else if (spec == 103) { if (v > 3600) return false; c.courseDeg = v / 10.0; }
            else c.persons = v;
        } else {
            return read > 0;                  // ★ a specifier we do not read: keep what was read before it
        }
        ++read;
    }
    c.expansions += read;
    return read > 0;
}

int eccOf(const std::vector<int>& s) {
    int x = 0;
    for (int v : s) x ^= (v & 127);
    return x;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The readable line
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

static const char* kDot = " \xc2\xb7 ";      // " · "
static const char* kDeg = "\xc2\xb0";        // "°"

std::string describeMmsi(const std::string& d9) {
    int m = -1;
    const mid::Kind k = mid::kindOf(d9, m);
    std::string iso;
    const std::string country = mid::countryOf(d9, &iso);
    std::string s;
    if (k == mid::Kind::Ship) s = d9;
    else s = std::string(mid::kindWord(k)) + " " + d9;
    if (!country.empty()) s += " (" + country + ")";
    return s;
}

std::string positionText(const Position& p) {
    if (!p.known) return "position unknown";
    char b[96];
    const char ns = (p.quadrant == 2 || p.quadrant == 3) ? 'S' : 'N';
    const char ew = (p.quadrant == 1 || p.quadrant == 3) ? 'W' : 'E';
    if (p.latFrac >= 0)
        std::snprintf(b, sizeof b, "%02d%s%02d.%04d'%c %03d%s%02d.%04d'%c", p.latDeg, kDeg, p.latMin, p.latFrac, ns,
                      p.lonDeg, kDeg, p.lonMin, p.lonFrac, ew);
    else
        std::snprintf(b, sizeof b, "%02d%s%02d'%c %03d%s%02d'%c", p.latDeg, kDeg, p.latMin, ns, p.lonDeg, kDeg, p.lonMin, ew);
    return b;
}

static std::string areaText(const Area& a) {
    char b[128];
    const char ns = (a.quadrant == 2 || a.quadrant == 3) ? 'S' : 'N';
    const char ew = (a.quadrant == 1 || a.quadrant == 3) ? 'W' : 'E';
    std::snprintf(b, sizeof b, "ships in the area %02d%s%c %03d%s%c, %d%s south \xc3\x97 %d%s east", a.latDeg, kDeg, ns,
                  a.lonDeg, kDeg, ew, a.dLat, kDeg, a.dLon, kDeg);
    return b;
}

static std::string upper(std::string s) { for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch); return s; }

std::string describe(const Call& c) {
    std::vector<std::string> p;
    // ── what it is
    if (c.format == kDistress) p.push_back("DISTRESS");
    else if (c.category == kCatDistress && c.tc1 == 110) p.push_back("DISTRESS ACKNOWLEDGEMENT");
    else if (c.category == kCatDistress && c.tc1 == 112) p.push_back("DISTRESS RELAY");
    else {
        std::string h = upper(categoryWord(c.category));
        if (c.tc1 == 118) h += " TEST";
        else if (c.tc1 == 121) h += " POSITION";
        else if (c.tc1 == 103) h += " POLLING";
        p.push_back(h);
    }
    // ── who to, who from
    if (c.format == kDistress) {
        p.push_back("MMSI " + describeMmsi(c.self));
    } else {
        std::string to;
        if (c.format == kAllShips) to = "all ships";
        else if (c.format == kGeoArea) to = areaText(c.area);
        else if (c.format == kGroup) to = "to " + describeMmsi(c.address);
        else to = "to " + describeMmsi(c.address);
        p.push_back(to);
        p.push_back("from " + describeMmsi(c.self));
    }
    // ── the distress information
    if (c.format == kDistress || !c.distressId.empty()) {
        if (!c.distressId.empty()) p.push_back("ship in distress " + describeMmsi(c.distressId));
        if (*natureWord(c.nature)) p.push_back(natureWord(c.nature));
        p.push_back(positionText(c.pos));
        if (c.utcHour >= 0) { char b[24]; std::snprintf(b, sizeof b, "%02d:%02d UTC", c.utcHour, c.utcMin); p.push_back(b); }
        else p.push_back("time unknown");
        if (c.subsequent >= 0 && c.subsequent != 126) p.push_back(std::string("then ") + subsequentWord(c.subsequent));
    } else {
        // ── the telecommands and what follows them
        if (c.tc1 == 104) {
            std::string w = "unable to comply";
            if (c.tc2 >= 0 && c.tc2 != 126) w += std::string(": ") + tc2Word(c.tc2);
            p.push_back(w);
        } else {
            if (c.tc1 >= 0 && c.tc1 != 126 && c.tc1 != 118 && c.tc1 != 121 && c.tc1 != 103) p.push_back(tc1Word(c.tc1));
            if (c.tc2 >= 0 && c.tc2 != 126) p.push_back(tc2Word(c.tc2));
        }
        if (!c.freqs.empty()) {
            std::string f = c.freqs[0];
            for (size_t k = 1; k < c.freqs.size(); ++k) f += " / " + c.freqs[k];
            p.push_back(f);
        }
        if (c.hasPosReply) {
            std::string s = positionText(c.posReply);
            if (c.posReplyHour >= 0) { char b[24]; std::snprintf(b, sizeof b, " at %02d:%02d UTC", c.posReplyHour, c.posReplyMin); s += b; }
            p.push_back(s);
        }
        if (!c.number.empty()) p.push_back("number " + c.number);
    }
    if (!std::isnan(c.speedKn)) { char b[32]; std::snprintf(b, sizeof b, "%.1f kn", c.speedKn); p.push_back(b); }
    if (!std::isnan(c.courseDeg)) { char b[32]; std::snprintf(b, sizeof b, "course %.1f%s", c.courseDeg, kDeg); p.push_back(b); }
    if (c.persons >= 0) p.push_back(std::to_string(c.persons) + " on board");
    if (c.eos == kEosAckRq) p.push_back("acknowledgement requested");
    else if (c.eos == kEosAckBq) p.push_back("acknowledgement");
    if (c.check == Call::Check::Corrected)
        p.push_back(c.corrected == 1 ? "1 symbol corrected" : std::to_string(c.corrected) + " symbols corrected");
    else if (c.check == Call::Check::Failed) p.push_back("UNCONFIRMED (check character failed)");
    std::string out;
    for (size_t k = 0; k < p.size(); ++k) { if (k) out += kDot; out += p[k]; }
    return out;
}

static std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += (char)ch;
    }
    return o;
}

std::string toJson(const Call& c, const std::string& extra) {
    std::string j = "{";
    auto kv = [&](const char* k, const std::string& v) { if (j.size() > 1) j += ','; j += '"'; j += k; j += "\":\""; j += jsonEsc(v); j += '"'; };
    auto kn = [&](const char* k, double v, const char* fmt = "%.6g") {
        if (!std::isfinite(v)) return;
        char b[48]; std::snprintf(b, sizeof b, fmt, v);
        if (j.size() > 1) j += ','; j += '"'; j += k; j += "\":"; j += b;
    };
    kv("format", formatWord(c.format));
    kn("fmt", c.format, "%.0f");
    if (c.category >= 0) kv("category", categoryWord(c.category));
    kn("level", c.level(), "%.0f");
    std::string iso;
    kv("self", c.self); kv("selfCountry", mid::countryOf(c.self, &iso)); kv("selfIso", iso);
    { int m; kv("selfKind", mid::kindWord(mid::kindOf(c.self, m))); }
    if (!c.address.empty()) { kv("to", c.address); kv("toCountry", mid::countryOf(c.address, &iso)); kv("toIso", iso); }
    if (c.area.valid) {
        kn("areaLat", c.area.lat, "%.4f"); kn("areaLon", c.area.lon, "%.4f");
        kn("areaDLat", c.area.dLat, "%.0f"); kn("areaDLon", c.area.dLon, "%.0f");
    }
    if (!c.distressId.empty()) { kv("distressId", c.distressId); kv("distressCountry", mid::countryOf(c.distressId, &iso)); kv("distressIso", iso); }
    if (c.nature >= 0) kv("nature", natureWord(c.nature));
    const Position& p = c.pos.known ? c.pos : c.posReply;
    if (p.known) { kn("lat", p.lat, "%.5f"); kn("lon", p.lon, "%.5f"); kv("posText", positionText(p)); }
    if (c.utcHour >= 0) { char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", c.utcHour, c.utcMin); kv("time", b); }
    if (c.tc1 >= 0) kv("tc1", tc1Word(c.tc1));
    if (c.tc2 >= 0) kv("tc2", tc2Word(c.tc2));
    if (!c.freqs.empty()) {
        j += ",\"freqs\":[";
        for (size_t k = 0; k < c.freqs.size(); ++k) { if (k) j += ','; j += '"' + jsonEsc(c.freqs[k]) + '"'; }
        j += ']';
    }
    if (!c.number.empty()) kv("number", c.number);
    kv("eos", c.eos == kEosAckRq ? "ack requested" : c.eos == kEosAckBq ? "ack" : "");
    kv("check", c.check == Call::Check::Ok ? "ok" : c.check == Call::Check::Corrected ? "corrected" : "unconfirmed");
    kn("corrected", c.corrected, "%.0f");
    kn("quality", c.quality, "%.2f");
    kv("band", c.vhf ? "vhf" : "hf");
    kv("line", describe(c));
    if (!extra.empty()) { j += ','; j += extra; }
    j += '}';
    return j;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// Building calls
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace build {
static void digits(std::vector<int>& s, const std::string& d) {
    for (size_t i = 0; i + 1 < d.size(); i += 2) s.push_back((d[i] - '0') * 10 + (d[i + 1] - '0'));
}
void mmsi(std::vector<int>& s, const std::string& d9) { digits(s, d9 + "0"); }
void position(std::vector<int>& s, double lat, double lon) {
    const int q = (lat >= 0 ? 0 : 2) + (lon >= 0 ? 0 : 1);
    const double a = std::fabs(lat), o = std::fabs(lon);
    int la = (int)a, lam = (int)std::lround((a - la) * 60); if (lam == 60) { ++la; lam = 0; }
    int lo = (int)o, lom = (int)std::lround((o - lo) * 60); if (lom == 60) { ++lo; lom = 0; }
    char b[16]; std::snprintf(b, sizeof b, "%d%02d%02d%03d%02d", q, la, lam, lo, lom);
    digits(s, b);
}
void unknownPosition(std::vector<int>& s) { for (int i = 0; i < 5; ++i) s.push_back(99); }
void time(std::vector<int>& s, int h, int m) { if (h < 0) { s.push_back(88); s.push_back(88); } else { s.push_back(h); s.push_back(m); } }
void freqKhz(std::vector<int>& s, double khz) { char b[16]; std::snprintf(b, sizeof b, "%06ld", std::lround(khz * 10)); digits(s, b); }
void vhfChannel(std::vector<int>& s, int ch) { char b[16]; std::snprintf(b, sizeof b, "90%04d", ch); digits(s, b); }
void noFreq(std::vector<int>& s) { s.push_back(126); s.push_back(126); s.push_back(126); }

std::vector<uint8_t> transmission(const std::vector<int>& primary, const std::vector<std::vector<int>>& expansions, int dotBits) {
    // The DX stream: phasing 125 ×6, the format specifier twice, the fields, EOS, ECC, the expansions (each with its
    // ECC), and EOS twice more. The RX stream: 111 … 104, then the DX stream from its first format specifier, two
    // symbols behind it — so it ends with the last ECC.
    std::vector<int> dx(6, 125);
    dx.push_back(primary[0]);
    dx.insert(dx.end(), primary.begin(), primary.end());
    dx.push_back(eccOf(primary));
    for (const auto& e : expansions) { dx.insert(dx.end(), e.begin(), e.end()); dx.push_back(eccOf(e)); }
    const int last = primary.back();
    dx.push_back(last); dx.push_back(last);
    std::vector<int> rx;
    for (int r = 7; r >= 0; --r) rx.push_back(104 + r);
    for (size_t i = 6; i + 2 < dx.size(); ++i) rx.push_back(dx[i]);
    std::vector<uint8_t> bits;
    for (int i = 0; i < dotBits; ++i) bits.push_back((uint8_t)((i & 1) ? 0 : 1));
    for (size_t i = 0; i < dx.size(); ++i) {
        const uint16_t a = codeword(dx[i]);
        for (int k = 0; k < 10; ++k) bits.push_back((uint8_t)((a >> k) & 1));
        if (i < rx.size()) {
            const uint16_t b = codeword(rx[i]);
            for (int k = 0; k < 10; ++k) bits.push_back((uint8_t)((b >> k) & 1));
        }
    }
    return bits;
}

std::vector<float> modulate(const std::vector<uint8_t>& bits, bool vhf, double rate, double centreHz, bool invert,
                            double baudPpm, double startPhase) {
    const double baud = (vhf ? 1200.0 : 100.0) * (1.0 + baudPpm * 1e-6);
    const double f1 = vhf ? 1300.0 : centreHz - 85.0, f0 = vhf ? 2100.0 : centreHz + 85.0;   // ★ 1 (B) = the lower tone
    const double spb = rate / baud;
    std::vector<float> out;
    out.reserve((size_t)(bits.size() * spb) + 2);
    double ph = startPhase, t = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        const bool one = (bits[i] != 0) != invert;
        const double w = 2 * M_PI * (one ? f1 : f0) / rate;
        const double endT = (double)(i + 1) * spb;
        while (t < endT) { out.push_back((float)std::sin(ph)); ph += w; t += 1.0; }
        if (ph > 2 * M_PI * 1e6) ph = std::fmod(ph, 2 * M_PI);
    }
    return out;
}
} // namespace build

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The receiver
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace {
/** Blackman-windowed sinc lowpass, `n` taps, cutoff `fc` at `fs`, unity gain at DC. */
std::vector<float> lowpass(int n, double fc, double fs) {
    std::vector<float> h((size_t)n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double m = i - (n - 1) / 2.0;
        const double x = 2 * fc / fs * m;
        const double sinc = std::fabs(m) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[(size_t)i] = (float)(sinc * w);
        sum += sinc * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

/** ★ The FIR's inner loop — two dot products of one tap set with the I and Q delay lines. NEON on ARMv7/AArch64,
 *  SSE2 on x86 (the i386 package and the x86-64 floor), scalar elsewhere and under VIBE_FORCE_SCALAR. */
inline void dot2(const float* h, const float* re, const float* im, int n, float& yr, float& yi) {
    int i = 0;
#if VIBE_NEON
    float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t hv = vld1q_f32(h + i);
        ar = vmlaq_f32(ar, hv, vld1q_f32(re + i));
        ai = vmlaq_f32(ai, hv, vld1q_f32(im + i));
    }
    float sr = vaddvq_f32(ar), si = vaddvq_f32(ai);
#elif VIBE_SSE
    __m128 ar = _mm_setzero_ps(), ai = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        const __m128 hv = _mm_loadu_ps(h + i);
        ar = _mm_add_ps(ar, _mm_mul_ps(hv, _mm_loadu_ps(re + i)));
        ai = _mm_add_ps(ai, _mm_mul_ps(hv, _mm_loadu_ps(im + i)));
    }
    float sr = vibedsp::sseAddv(ar), si = vibedsp::sseAddv(ai);
#else
    float sr = 0, si = 0;
#endif
    for (; i < n; ++i) { sr += h[i] * re[i]; si += h[i] * im[i]; }
    yr = sr; yi = si;
}

/** ★ The DX and RX copies of one symbol, summed. DSC_INVALID_W < 1 would count a copy that is not itself a valid word
 *  (while the other is) for less — meant for a burst that wrecks one copy with strong, meaningless bits. MEASURED
 *  (dsc_bench snr 10): in noise it COSTS — 0.5 took MF/HF −8 dB from 95.0 to 91.9 % and −10 dB from 31 to 26 %; 0.25
 *  worse again — because a copy with one bit wrong is "invalid" too and its nine good bits are worth keeping. So: 1. */
#ifndef DSC_INVALID_W
#define DSC_INVALID_W 1.0f
#endif
inline uint16_t hardOf(const float* w) { uint16_t h = 0; for (int k = 0; k < 10; ++k) if (w[k] > 0) h |= (uint16_t)(1 << k); return h; }
inline void combine(const float* a, const float* b, float* out) {
    const bool va = decodeWord(hardOf(a)) >= 0, vb = decodeWord(hardOf(b)) >= 0;
    const float ga = (va || !vb) ? 1.0f : DSC_INVALID_W, gb = (vb || !va) ? 1.0f : DSC_INVALID_W;
    for (int k = 0; k < 10; ++k) out[k] = ga * a[k] + gb * b[k];
}
constexpr int kRing = 8192;          // soft samples kept (HF: 68 bits of look-back is all the search needs; ample)
constexpr int kMaxCands = 4;
constexpr float kSyncC = 0.55f;      // the phasing correlation a sync must reach (calibrated on noise: test-dsc)
constexpr float kPhasingKeep = 0.45f;  // …and keep once all of the phasing is in
constexpr float kFormatMin = 0.40f;  // the four format-specifier copies' normalised score
#ifndef DSC_WEAK_REL
#define DSC_WEAK_REL 0.12f
#endif
constexpr float kWeakRel = DSC_WEAK_REL;
#ifndef DSC_GAP
#define DSC_GAP 0.20f
#endif
constexpr float kGap = DSC_GAP;       // a correction's lead over the next valid assignment (share of a symbol's energy)
constexpr float kGap0 = 0.10f;        // …and a call passing as received over its nearest rival, else UNCONFIRMED
/* ★★ What a correction may overrule (share of a symbol's energy) and how many positions may be in doubt around it —
 *  from the bench (dsc_bench snr 20, 2,600 accepted calls): one change ≤ 0.04 with ≤ 6 in doubt kept 150 right
 *  corrections for 1–2 wrong; a pair ≤ 0.06 with ≤ 4 in doubt, 16 for 1. Looser doubled the wrong ones for a few more. */
constexpr float kFix1 = 0.04f, kFix2 = 0.06f;
constexpr int kDoubt1 = 6, kDoubt2 = 4;
constexpr int kDoubtOk = 5;           // a call passing as received with this many positions in doubt is UNCONFIRMED   // a position "in doubt": its best word ahead by under this share of its energy
} // namespace

struct Receiver::Cand {
    int64_t T0 = 0;                  // the first sample of slot 0's first bit
    int pol = 1;
    float c0 = 0;
    int next = 0;                    // the next slot to read
    double phNum = 0, phDen = 0; int phSlots = 0;
    std::vector<std::array<float, 10>> dx, rx;   // by message index m: DX[m+6], RX[m+8]
    std::vector<int> sym; std::vector<float> margin;
    bool formatOk = false;
    int eosAt = -1;                  // a symbol that read as an EOS, waiting for the ECC after it
    // timing
    double tAcc = 0; int tCnt = 0;
    // signal lost
    float eRef = 0; int weak = 0;
    // primary done, reading expansions
    bool primaryDone = false;
    Call call;
    int expStart = -1;
    int eosV = -1;                   // the EOS value that symbol read as
    bool dead = false, done = false;
};

Receiver::Receiver(const RxConfig& cfg) : cfg_(cfg) {
    if (cfg_.vhf) {
        S_ = 8; fd_ = 9600; toneHz_ = 400;
        d1_ = std::max(1, (int)std::lround(cfg_.rate / 9600.0)); d2_ = 1;
        h1_ = lowpass(48, 2000, cfg_.rate);
    } else {
        S_ = 12; fd_ = 1200; toneHz_ = 85;
        d1_ = std::max(1, (int)std::lround(cfg_.rate / 6000.0)); d2_ = 5;
        h1_ = lowpass(std::max(12, 6 * d1_), 500, cfg_.rate);
        h2_ = lowpass(50, 330, 6000);
    }
    b1re_.assign(h1_.size() * 2, 0); b1im_.assign(h1_.size() * 2, 0);
    if (!h2_.empty()) { b2re_.assign(h2_.size() * 2, 0); b2im_.assign(h2_.size() * 2, 0); }
    setCentre(cfg_.vhf ? 1700.0 : cfg_.centreHz);
    const double wt = 2 * M_PI * toneHz_ / fd_;
    tStRe_ = std::cos(wt); tStIm_ = std::sin(wt);
    hiRe_.assign((size_t)S_, 0); hiIm_.assign((size_t)S_, 0); loRe_.assign((size_t)S_, 0); loIm_.assign((size_t)S_, 0);
    ring_.assign(kRing, 0.0f); ringMask_ = kRing - 1;
    reg_.assign((size_t)S_, 0);
    std::memset(lut_, -1, sizeof lut_);
    for (int r = 0; r < 8; ++r) {
        const uint16_t w = codeword(111 - r);
        uint16_t pat = 0;                                  // newest bit in bit 0: the k-th bit sent at bit 9-k
        for (int k = 0; k < 10; ++k) if ((w >> k) & 1) pat |= (uint16_t)(1 << (9 - k));
        lut_[pat] = (int8_t)r;
    }
}

Receiver::~Receiver() { for (Cand* c : cands_) delete c; }

void Receiver::setCentre(double hz) {
    if (!cfg_.vhf) cfg_.centreHz = hz;
    const double w = 2 * M_PI * hz / cfg_.rate;
    ncoStRe_ = std::cos(w); ncoStIm_ = std::sin(w);
}

void Receiver::setState(int s) {
    if (s == state_) return;
    state_ = s;
    if (onState) onState(s);
}

void Receiver::process(const float* x, int n) { frontEnd(x, n); }

void Receiver::frontEnd(const float* x, int n) {
    const int L1 = (int)h1_.size(), L2 = (int)h2_.size();
    for (int i = 0; i < n; ++i) {
        // ── mix the band centre to 0 Hz: x · e^{-jθ}
        const float re = (float)(x[i] * ncoRe_), im = (float)(-x[i] * ncoIm_);
        const double nr = ncoRe_ * ncoStRe_ - ncoIm_ * ncoStIm_;
        ncoIm_ = ncoRe_ * ncoStIm_ + ncoIm_ * ncoStRe_; ncoRe_ = nr;
        if (++ncoN_ >= 1024) { ncoN_ = 0; const double g = 1.0 / std::sqrt(ncoRe_ * ncoRe_ + ncoIm_ * ncoIm_); ncoRe_ *= g; ncoIm_ *= g; }
        // ── stage 1
        p1_ = p1_ + 1 == L1 ? 0 : p1_ + 1;
        b1re_[(size_t)p1_] = b1re_[(size_t)(p1_ + L1)] = re;
        b1im_[(size_t)p1_] = b1im_[(size_t)(p1_ + L1)] = im;
        if (++ph1_ < d1_) continue;
        ph1_ = 0;
        float zr, zi;
        dot2(h1_.data(), &b1re_[(size_t)p1_ + 1], &b1im_[(size_t)p1_ + 1], L1, zr, zi);
        // ── stage 2 (MF/HF only: 6 kHz → 1200 Hz)
        if (L2) {
            p2_ = p2_ + 1 == L2 ? 0 : p2_ + 1;
            b2re_[(size_t)p2_] = b2re_[(size_t)(p2_ + L2)] = zr;
            b2im_[(size_t)p2_] = b2im_[(size_t)(p2_ + L2)] = zi;
            if (++ph2_ < d2_) continue;
            ph2_ = 0;
            dot2(h2_.data(), &b2re_[(size_t)p2_ + 1], &b2im_[(size_t)p2_ + 1], L2, zr, zi);
        }
        // ── the two tones to 0 Hz, each summed over one bit (integrate and dump at every sample)
        const float ur = (float)tRe_, ui = (float)tIm_;
        hiRe_[(size_t)bx_] = zr * ur + zi * ui;  hiIm_[(size_t)bx_] = zi * ur - zr * ui;   // z · conj(u): +tone → 0
        loRe_[(size_t)bx_] = zr * ur - zi * ui;  loIm_[(size_t)bx_] = zi * ur + zr * ui;   // z · u:       -tone → 0
        bx_ = bx_ + 1 == S_ ? 0 : bx_ + 1;
        const double tr = tRe_ * tStRe_ - tIm_ * tStIm_;
        tIm_ = tRe_ * tStIm_ + tIm_ * tStRe_; tRe_ = tr;
        if (++tN_ >= 1024) { tN_ = 0; const double g = 1.0 / std::sqrt(tRe_ * tRe_ + tIm_ * tIm_); tRe_ *= g; tIm_ *= g; }
        float ar = 0, ai = 0, br = 0, bi = 0;
        for (int k = 0; k < S_; ++k) { ar += hiRe_[(size_t)k]; ai += hiIm_[(size_t)k]; br += loRe_[(size_t)k]; bi += loIm_[(size_t)k]; }
        pushSoft(std::sqrt(ar * ar + ai * ai), std::sqrt(br * br + bi * bi));
    }
}

void Receiver::pushSoft(float hm, float lm) {
    // ── each tone's "on" level, and the noise (the weaker tone's level), slowly
    const float aOn = 1.0f / (60.0f * S_), aN = 1.0f / (120.0f * S_);
    if (noise_ <= 0) { noise_ = std::max(1e-9f, std::min(hm, lm)); hiOn_ = loOn_ = std::max(hm, lm); }
    if (hm > 2 * lm) hiOn_ += aOn * (hm - hiOn_);
    if (lm > 2 * hm) loOn_ += aOn * (lm - loOn_);
    noise_ += aN * (std::min(hm, lm) - noise_);
    // ★ The two tones brought to one level (a VHF transmitter's pre-emphasis lifts 2100 Hz; HF fades one tone at a time),
    //   at most 2:1 — then the difference, in units of the noise, is the soft bit.
    float gh = 1, gl = 1;
    if (hiOn_ > 0 && loOn_ > 0) {
        const float m = 0.5f * (hiOn_ + loOn_);
        gh = std::max(0.5f, std::min(2.0f, m / hiOn_)); gl = std::max(0.5f, std::min(2.0f, m / loOn_));
    }
    // ★★ The B state (binary 1) is the LOWER tone on both: 1615 Hz of the MF/HF pair through a USB receiver (measured on
    //    Stuart's UberSDR, 8414.5 kHz, 2026-10-26: every call came in on the lower-tone-is-1 sense) and 1300 Hz on VHF.
    float s = (lm * gl - hm * gh) / (noise_ + 1e-12f);
    s = std::max(-30.0f, std::min(30.0f, s));
    const int64_t t = n_;
    ring_[(size_t)(t & ringMask_)] = s;
    const int p = (int)(t % S_);
    uint16_t& rg = reg_[(size_t)p];
    rg = (uint16_t)(((rg << 1) | (s > 0 ? 1 : 0)) & 0x3FF);
    if (lut_[rg] >= 0) trigger(t, lut_[rg], 1);
    const uint16_t inv = (uint16_t)(~rg & 0x3FF);
    if (lut_[inv] >= 0) trigger(t, lut_[inv], -1);
    ++n_;
    evalPending();
    for (Cand* c : cands_) advance(*c);
    // ── the finished and the dead go
    bool any = false;
    for (size_t i = 0; i < cands_.size();) {
        Cand* c = cands_[i];
        if (c->dead || c->done) {
            if (c->dead) ++st_.falseSyncs;
            delete c; cands_.erase(cands_.begin() + (long)i);
            continue;
        }
        if (c->formatOk) any = true;
        ++i;
    }
    setState(any ? 1 : 0);
}

void Receiver::trigger(int64_t t, int r, int pol) {
    if (r < 2) return;                             // ★ at least two DX/RX pairs of phasing behind it (see evalPending)
    for (const auto& p : pending_) if (p.pol == pol && p.r == r && t - p.t <= S_) return;
    if (pending_.size() > 64) return;
    pending_.push_back({ t, r, pol });
}

void Receiver::evalPending() {
    for (size_t pi = 0; pi < pending_.size();) {
        const Pending p = pending_[pi];
        if (n_ - 1 < p.t + S_ / 2) { ++pi; continue; }
        pending_.erase(pending_.begin() + (long)pi);
        const int s = 2 * p.r + 1;                 // the RX phasing symbol r is slot 2r+1
        const int jMin = std::max(0, s - 7);
        float bestC = -1; int64_t bestT0 = 0;
        for (int d = -S_ / 2; d <= S_ / 2; ++d) {
            const int64_t tEnd = p.t + d;
            const int64_t T0 = tEnd - (int64_t)10 * (s + 1) * S_ + 1;
            if (T0 + (int64_t)10 * jMin * S_ < n_ - kRing + 2 * S_) continue;   // fell out of the ring
            if (tEnd > n_ - 1) continue;
            double num = 0, den = 0; int known = 0;
            for (int j = jMin; j <= s; ++j) {
                int v;
                if (j & 1) v = 111 - (j - 1) / 2;
                else if (j / 2 <= 5) v = 125;
                else continue;                     // a format specifier: not known yet
                ++known;
                const float* pm = T().pm[v];
                for (int k = 0; k < 10; ++k) {
                    const float b = p.pol * slotSoft(T0 + (int64_t)(10 * j + k + 1) * S_ - 1);
                    num += b * pm[k]; den += std::fabs(b);
                }
            }
            if (known < 5 || den <= 0) continue;
            const float C = (float)(num / den);
            if (C > bestC) { bestC = C; bestT0 = T0; }
        }
        if (bestC < kSyncC) continue;
        bool dup = false;
        for (Cand* c : cands_) if (c->pol == p.pol && std::llabs(c->T0 - bestT0) <= S_) { dup = true; break; }
        if (dup) continue;
        if ((int)cands_.size() >= kMaxCands) {
            // ★ Full: the weakest candidate that has not yet read a format specifier makes way for a better one.
            int worst = -1;
            for (int i = 0; i < (int)cands_.size(); ++i)
                if (!cands_[(size_t)i]->formatOk && (worst < 0 || cands_[(size_t)i]->c0 < cands_[(size_t)worst]->c0)) worst = i;
            if (worst < 0 || cands_[(size_t)worst]->c0 >= bestC) continue;
            delete cands_[(size_t)worst]; cands_.erase(cands_.begin() + worst);
        }
        Cand* c = new Cand();
        c->T0 = bestT0; c->pol = p.pol; c->c0 = bestC;
        cands_.push_back(c);
    }
}

void Receiver::advance(Cand& c) {
    // ★ A slot is read once its last bit, and the sample after it (the timing's "late"), are in.
    while (!c.dead && !c.done) {
        const int64_t last = c.T0 + (int64_t)(10 * c.next + 10) * S_ - 1;
        if (last + 1 > n_ - 1) break;
        if (last - (int64_t)10 * S_ < n_ - kRing) { c.dead = true; break; }
        if (!readSlot(c, c.next)) break;
        ++c.next;
    }
}

bool Receiver::readSlot(Cand& c, int j) {
    std::array<float, 10> w;
    for (int k = 0; k < 10; ++k) {
        const int64_t be = c.T0 + (int64_t)(10 * j + k + 1) * S_ - 1;
        w[(size_t)k] = c.pol * slotSoft(be);
        // ★ Fine timing: |early| against |late| at every bit end — balanced when the bit boundaries are where we think.
        const float a = std::fabs(slotSoft(be - 1)), b = std::fabs(slotSoft(be + 1));
        if (a + b > 0) { c.tAcc += (b - a) / (a + b); ++c.tCnt; }
    }
#ifndef DSC_NO_TRACK
    if (c.tCnt >= 30) {
        if (c.tAcc > 0.15 * c.tCnt) ++c.T0;
        else if (c.tAcc < -0.15 * c.tCnt) --c.T0;
        c.tAcc = 0; c.tCnt = 0;
    }
#endif
    // ── the phasing: RX symbols in the odd slots up to 15, DX 125 in the even slots up to 10
    const bool phasingSlot = (j & 1) ? j <= 15 : j <= 10;
    if (phasingSlot) {
        const int v = (j & 1) ? 111 - (j - 1) / 2 : 125;
        const float* pm = T().pm[v];
        for (int k = 0; k < 10; ++k) { c.phNum += w[(size_t)k] * pm[k]; c.phDen += std::fabs(w[(size_t)k]); }
        ++c.phSlots;
        if (j == 11) {
            if (c.phDen <= 0 || c.phNum / c.phDen < kPhasingKeep) { c.dead = true; return true; }
            c.eRef = (float)(c.phDen / (10.0 * c.phSlots));
        }
    }
    if (!(j & 1) && j >= 12) {
        const size_t m = (size_t)(j / 2 - 6);
        if (c.dx.size() <= m) c.dx.resize(m + 1);
        c.dx[m] = w;
        return true;
    }
    if ((j & 1) && j >= 17) {
        const size_t m = (size_t)((j - 1) / 2 - 8);
        if (c.rx.size() <= m) c.rx.resize(m + 1);
        c.rx[m] = w;
        // ── symbol m has both its copies
        if (c.dx.size() <= m) { c.dead = true; return true; }
        if (m == 1) {
            // ★ The format specifier, on all FOUR of its copies — and only a format specifier will do.
            float sum[10];
            for (int k = 0; k < 10; ++k) sum[k] = c.dx[0][(size_t)k] + c.dx[1][(size_t)k] + c.rx[0][(size_t)k] + c.rx[1][(size_t)k];
            const Ml f = ml(sum, kFormats, 6);
            const Ml all = ml(sum);
            if (f.energy <= 0 || f.sBest / f.energy < kFormatMin || all.sBest > f.sBest + 0.25f * f.energy) { c.dead = true; return true; }
            c.sym.assign(2, f.best);
            c.margin.assign(2, (f.sBest - f.sSecond) / f.energy);
            c.formatOk = true;
            ++st_.syncs;
            return true;
        }
        if (m == 0) return true;
        float sum[10];
        combine(c.dx[m].data(), c.rx[m].data(), sum);
        const Ml r = ml(sum);
        c.sym.push_back(r.best);
        c.margin.push_back(r.energy > 0 ? (r.sBest - r.sSecond) / r.energy : 0);
        // ★ Signal gone: four symbols in a row at under a sixth of the phasing's level — the call is over or lost.
        if (r.energy < 10.0f * c.eRef / 6.0f) { if (++c.weak >= 4) { if (c.primaryDone) { emit(c); c.done = true; } else c.dead = true; return true; } }
        else c.weak = 0;
        const int mi = (int)m;
        /* ★ An EOS misread in BOTH its copies is still found: the DX stream sends it twice more after the ECC, so the
         *  symbol before this one is judged on four copies (its DX and RX, and the DX two and three places on). */
        if (c.eosAt < 0 && !c.primaryDone && mi >= 10 && (size_t)mi + 2 < c.dx.size()) {
            float s4[10];
            const size_t e = (size_t)mi - 1;
            for (int k = 0; k < 10; ++k) s4[k] = c.dx[e][(size_t)k] + c.rx[e][(size_t)k] + c.dx[e + 2][(size_t)k] + c.dx[e + 3][(size_t)k];
            const Ml r4 = ml(s4);
            float es = -1e30f; int ev = -1;
            for (int v : kEoss) { const float sc = score(s4, v); if (sc > es) { es = sc; ev = v; } }
            if (r4.energy > 0 && isEos(r4.best)) { c.eosAt = mi - 1; c.eosV = ev; }   // ★ strictly: 126 is two bits from 127
        }
        // ── the end: an EOS, then the ECC after it
        if (c.eosAt >= 0 && mi == c.eosAt + 1) {
            const int at = c.eosAt;
            c.eosAt = -1;
            if (!c.primaryDone) { if (finishPrimary(c, at)) return true; /* not the end after all: read on */ }
            else {
                // an expansion message ends here
                std::vector<int> e(c.sym.begin() + c.expStart, c.sym.begin() + at + 1);
                const int ecc = c.sym[(size_t)at + 1];
                bool ok = eccOf(e) == ecc;
                if (!ok) {
                    // ★ one symbol put right by the check character: the least certain one whose change fits
                    int bestI = -1; float bestCost = 1e30f; int bestV = 0;
                    const int syn = eccOf(e) ^ ecc;
                    for (int i = c.expStart; i <= at + 1; ++i) {
                        const int v = c.sym[(size_t)i] ^ syn;
                        float s2[10];
                        for (int k = 0; k < 10; ++k) s2[k] = c.dx[(size_t)i][(size_t)k] + c.rx[(size_t)i][(size_t)k];
                        const float cost = score(s2, c.sym[(size_t)i]) - score(s2, v);
                        if (cost < bestCost) { bestCost = cost; bestI = i; bestV = v; }
                    }
                    if (bestI >= 0 && bestCost < 0.6f * 10.0f * c.eRef) {
                        if (bestI <= at) { e[(size_t)(bestI - c.expStart)] = bestV; ok = eccOf(e) == ecc; }
                        else ok = true;
                    }
                }
                if (ok) parseExpansion(e, c.call);
                // another expansion message?
                const size_t nx = (size_t)at + 2;
                const Ml d = nx < c.dx.size() ? ml(c.dx[nx].data()) : Ml();
                if (ok && d.best >= 100 && d.best <= 106 && !isEos(d.best) && d.energy > 0 && d.sBest / d.energy > 0.5f) {
                    c.expStart = (int)nx;
                    return true;
                }
                emit(c); c.done = true; return true;
            }
        }
        float eosScore = -1e30f; int eosV = -1;
        for (int v : kEoss) { const float sc = score(sum, v); if (sc > eosScore) { eosScore = sc; eosV = v; } }
        // ★ An EOS — or close enough to one that the check character should be asked (the shortest call ends at 9).
        if (r.energy > 0 && (isEos(r.best) || eosScore >= r.sBest - 0.08f * r.energy) && mi >= 9) { c.eosAt = mi; c.eosV = eosV; }
        if (mi > (c.primaryDone ? c.expStart + 24 : 70)) {
            if (c.primaryDone) { emit(c); c.done = true; } else c.dead = true;
#ifdef DSC_DEBUG
            std::fprintf(stderr, "dsc: too long at %d\n", mi);
#endif
        }
    }
    return true;
}

/** ★★ THE END OF THE PRIMARY CALL: the EOS at `mEos`, its ECC after it. The check character is used to CORRECT: the
 *  cheapest change of one symbol (or two of the least certain) that makes the check pass and still reads as a call.
 *  @return true if the candidate is finished with (emitted, or given up). */
bool Receiver::finishPrimary(Cand& c, int mEos) {
    const int nPos = mEos;                       // positions: 0 = F (four copies), 1..mEos-1 = m+1 → m = 2..mEos, + ECC
    // the soft words per position
    std::vector<std::array<float, 10>> w((size_t)nPos + 1);
    for (int k = 0; k < 10; ++k) w[0][(size_t)k] = c.dx[0][(size_t)k] + c.dx[1][(size_t)k] + c.rx[0][(size_t)k] + c.rx[1][(size_t)k];
    for (int p = 1; p < nPos; ++p) {
        const size_t m = (size_t)p + 1;
        combine(c.dx[m].data(), c.rx[m].data(), w[(size_t)p].data());
    }
    // ★ The EOS: its DX and RX copies, and the two the DX stream repeats after the ECC when they read as the same EOS.
    int endEvidence = 0;
    {
        const size_t m = (size_t)mEos;
        std::array<float, 10>& e = w[(size_t)nPos - 1];
        for (int k = 0; k < 10; ++k) e[(size_t)k] = c.dx[m][(size_t)k] + c.rx[m][(size_t)k];
        for (size_t extra = m + 2; extra <= m + 3; ++extra) {
            if (extra >= c.dx.size()) continue;
            const Ml d = ml(c.dx[extra].data());
            if (d.best == c.eosV) { ++endEvidence; for (int k = 0; k < 10; ++k) e[(size_t)k] += c.dx[extra][(size_t)k]; }
        }
    }
    std::array<float, 10> eccW;
    for (int k = 0; k < 10; ++k) eccW[(size_t)k] = c.dx[(size_t)mEos + 1][(size_t)k] + c.rx[(size_t)mEos + 1][(size_t)k];
    w[(size_t)nPos] = eccW;
    const int nAll = nPos + 1;                   // data positions 0..nPos-1, ECC at nPos
    std::vector<int> best((size_t)nAll);
    std::vector<float> marg((size_t)nAll), rel((size_t)nAll);
    float eSum = 0;
    for (int p = 0; p < nAll; ++p) {
        Ml r = p == 0 ? ml(w[0].data(), kFormats, 6) : p == nPos - 1 ? ml(w[(size_t)p].data(), kEoss, 3) : ml(w[(size_t)p].data());
        best[(size_t)p] = r.best;
        marg[(size_t)p] = r.sBest - r.sSecond;
        rel[(size_t)p] = r.energy > 0 ? (r.sBest - r.sSecond) / r.energy : 0;
        eSum += r.energy;
    }
    const float eMean = eSum / (float)nAll;
    /* ★ How many positions were genuinely in doubt (their best word barely ahead of the next). Correction is safe only
     *  when the doubt is in one or two places — a call read uncertainly all through gets no help from one XOR. */
    int weak = 0;
    for (int p = 0; p < nAll; ++p) if (rel[(size_t)p] < kWeakRel) ++weak;
    auto toSyms = [&](const std::vector<int>& b) {
        std::vector<int> s(b.begin(), b.begin() + nPos);
        return s;
    };
    int syn = 0;
    for (int p = 0; p < nPos; ++p) syn ^= best[(size_t)p];
    syn ^= best[(size_t)nPos];
    // candidate assignments, cheapest first
    struct Alt { float cost; int p, a, q, b; };
    std::vector<Alt> alts;
    alts.push_back({ 0.0f, -1, 0, -1, 0 });
    auto costOf = [&](int p, int v) { return score(w[(size_t)p].data(), best[(size_t)p]) - score(w[(size_t)p].data(), v); };
    if (syn) for (int p = 0; p < nAll; ++p) alts.push_back({ costOf(p, best[(size_t)p] ^ syn), p, best[(size_t)p] ^ syn, -1, 0 });
    // ★ Pairs among the six least certain positions, each trying its four next-best words.
    {
        std::vector<int> idx((size_t)nAll);
        for (int p = 0; p < nAll; ++p) idx[(size_t)p] = p;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return marg[(size_t)a] < marg[(size_t)b]; });
        const int K = std::min(6, nAll);
        for (int ia = 0; ia < K; ++ia) {
            const int p = idx[(size_t)ia];
            // its four next-best words
            std::vector<std::pair<float, int>> cand;
            for (int v = 0; v < 128; ++v) if (v != best[(size_t)p]) cand.push_back({ costOf(p, v), v });
            std::partial_sort(cand.begin(), cand.begin() + 4, cand.end());
            for (int ib = 0; ib < K; ++ib) {
                const int q = idx[(size_t)ib];
                if (q == p) continue;
                for (int t = 0; t < 4; ++t) {
                    const int a = cand[(size_t)t].second;
                    const int bq = best[(size_t)q] ^ syn ^ best[(size_t)p] ^ a;
                    if (bq == best[(size_t)q]) continue;
                    alts.push_back({ cand[(size_t)t].first + costOf(q, bq), p, a, q, bq });
                }
            }
        }
    }
    std::sort(alts.begin(), alts.end(), [](const Alt& a, const Alt& b) { return a.cost < b.cost; });
    /* ★★★ A CORRECTION MUST BE CHEAP AND THE ONLY CHEAP ONE. One changed symbol can ALWAYS be made to satisfy an XOR
     *  check — so the check alone proves nothing about a change; what proves it is the soft evidence. The winner (the
     *  cheapest assignment that passes the check and reads as a valid call) is taken only if it overrules little — a
     *  fraction of one symbol's worth of signal — and the runner-up costs clearly more. Calibrated in test-dsc / the
     *  bench so that a wrong call is rarer than a missed one at every SNR. */
    const float fix1 = kFix1 * eMean, fix2 = kFix2 * eMean, gapMin = kGap * eMean, gap0 = kGap0 * eMean;
    const float horizon = 0.8f * eMean;
    Call call;
    bool got = false;
    int found = 0; float c1 = 0, c2 = -1; int k1 = 0; std::vector<int> b1;
    auto rank = [&](int p, int v) {
        const float sv = score(w[(size_t)p].data(), v);
        int r = 0;
        for (int u = 0; u < 128; ++u) if (u != v && score(w[(size_t)p].data(), u) > sv) ++r;
        return r;
    };
    // ★ The two cheapest assignments that pass the check, read as a valid call and change only CONTENDERS (a changed
    //   symbol was the second-best word where it stands — the third for a pair): when two symbols are misread, the one
    //   change that satisfies the check is almost never a contender.
    for (const Alt& a : alts) {
        if (a.cost > horizon) break;
        std::vector<int> b = best;
        if (a.p >= 0) b[(size_t)a.p] = a.a;
        if (a.q >= 0) b[(size_t)a.q] = a.b;
        int x = 0; for (int p = 0; p < nPos; ++p) x ^= b[(size_t)p];
        if (x != b[(size_t)nPos]) continue;
        if (found == 1 && b == b1) continue;
        if (a.p >= 0 && rank(a.p, a.a) > (a.q >= 0 ? 2 : 1)) continue;
        if (a.q >= 0 && rank(a.q, a.b) > 2) continue;
        Call tmp;
        if (!parse(toSyms(b), tmp)) continue;
        if (found == 0) {
            found = 1; c1 = a.cost; b1 = b; call = tmp;
            for (int p = 0; p < nPos; ++p) if (b[(size_t)p] != best[(size_t)p]) ++k1;
            continue;
        }
        found = 2; c2 = a.cost;
        break;
    }
    /* ★★★ THE GAP DECIDES. Taken: the cheapest valid assignment, if the next one costs clearly more (`gap`). A call that
     *  passes as received but has a nearly-as-good rival (two misreads that cancel in the XOR) is shown UNCONFIRMED; a
     *  correction without a clear gap, or that overrules too much, is not made. (Calibrated on the bench: of the calls
     *  a correction let through, wrong ones had gaps of 0.15–0.17 of a symbol's energy, right ones up to 0.4+.) */
    const float gap = found == 2 ? c2 - c1 : horizon - c1;
    if (found >= 1) {
        if (c1 == 0.0f) {
            call.check = gap >= gap0 && weak < kDoubtOk ? Call::Check::Ok : Call::Check::Failed;
            got = true;
        } else if (gap >= gapMin && c1 <= (k1 <= 1 ? fix1 : fix2) && weak <= (k1 <= 1 ? kDoubt1 : kDoubt2)) {
            call.check = k1 ? Call::Check::Corrected : Call::Check::Ok;   // ★ only the ECC misread: no damage to the call
            call.corrected = k1;
            got = true;
        }
    }
    if (!got && endEvidence < 2) return false;    // ★ no check passed and the DX repeats do not both say "it ended here": read on
    /* ★★ A call whose check character fails and that cannot be put right is NOT shown — not even marked: on the bench
     *  such calls were wrong more often than right (131 of 213), i.e. the marking would only have been noise. The
     *  transmitting station repeats (a distress alert five times over); one of the repeats will pass. */
#ifdef DSC_DEBUG
    if (!got) { std::fprintf(stderr, "dsc: dead at EOS %d (evidence %d, syn %d, found %d c1 %.3f k1 %d weak %d)\n", mEos, endEvidence, syn, found, eMean > 0 ? c1 / eMean : 0, k1, weak); }
#endif
    if (!got) { c.dead = true; return true; }
    // quality and disagreement
    float q = 0;
    for (int p = 0; p < nPos; ++p) q += std::min(1.0f, marg[(size_t)p] / (eMean > 0 ? eMean : 1));
    call.quality = q / (float)nPos;
    call.fixCost = eMean > 0 && found ? c1 / eMean : 0;
    call.fixGap = c2 >= 0 && eMean > 0 ? (c2 - c1) / eMean : -1;
    call.doubt = weak;
    for (int p = 1; p < nPos; ++p) {
        const size_t m = (size_t)p + 1;
        uint16_t hd = 0, hr = 0;
        for (int k = 0; k < 10; ++k) { if (c.dx[m][(size_t)k] > 0) hd |= (uint16_t)(1 << k); if (c.rx[m][(size_t)k] > 0) hr |= (uint16_t)(1 << k); }
        if (decodeWord(hd) != decodeWord(hr) || decodeWord(hd) < 0) ++call.disagreed;
    }
    call.vhf = cfg_.vhf;
    if (c.pol < 0) ++st_.inverted;
    c.call = call;
    c.primaryDone = true;
    // the corrected symbols back into the stream (the expansion's ECC does not cover them, but keep them straight)
    // ── an expansion message next? The DX stream's next symbol is its specifier instead of an EOS repeat.
    const size_t nx = (size_t)mEos + 2;
    if (nx < c.dx.size()) {
        const Ml d = ml(c.dx[nx].data());
        if (d.best >= 100 && d.best <= 106 && d.energy > 0 && d.sBest / d.energy > 0.5f) { c.expStart = (int)nx; return false; }
    }
    emit(c);
    c.done = true;
    return true;
}

void Receiver::emit(Cand& c) {
    if (!c.primaryDone) return;
    ++st_.calls;
    if (c.call.check == Call::Check::Corrected) ++st_.corrected;
    if (c.call.check == Call::Check::Failed) ++st_.failed;
    if (onCall) onCall(c.call);
}

} // namespace dsc
} // namespace vibe
