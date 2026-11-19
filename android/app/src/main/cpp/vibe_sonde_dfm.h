// vibe_sonde_dfm.h — V12 radiosondes: the GRAW DFM-06 / DFM-09 / DFM-17 frame and the per-channel state that turns its
// frames into a sonde (docs/v12/SONDES-TYPES.md). Our own code, from LISTENER DOCUMENTATION of the DFM format (Graw
// publishes none — Stuart's RS41 decision applies). GPL decoders were read for facts only; nothing here is copied.
// ★ NOTHING HERE HAS BEEN DECODED OFF AIR YET — SONDES-TYPES.md lists which facts are unverified.
//
// The frame as the listener documentation describes it — 2500 chips/s FSK, Manchester ("01" = 1), 280 bits:
//   0…15     header 0x45CF (raw chips 10011010100110010101101001010101)
//   16…71    CONF: 7 Hamming(8,4) codewords, interleaved      → 7 nibbles: id, then a 24-bit value
//   72…175   DAT1: 13 codewords, interleaved                    → 13 nibbles: a 32-bit value, a 16-bit value, the packet id
//   176…279  DAT2: the same
// ★ INTERLEAVING: a block of L codewords is sent bit-plane by bit-plane — sent bit L·j + i is bit j of codeword i.
// ★ HAMMING(8,4), extended (distance 4): bits 0…3 the nibble (most significant first), 4…7 parity
//   p4 = d1⊕d2⊕d3, p5 = d0⊕d2⊕d3, p6 = d0⊕d1⊕d3, p7 = d0⊕d1⊕d2. One wrong bit is corrected, two are detected.
// ★ The DAT packets (id 0…8) cycle; the position needs several. Two layouts, chosen by packet 0's "mode" byte:
//   mode ≤ 2: 1 = satellites bitmap + ms in the minute, 2 = lat (1e-7°) + ground speed (cm/s), 3 = lon + direction
//             (0.01°), 4 = altitude (cm) + climb (cm/s), 8 = date (year 12 bits, month 4, day 5, hour 5, minute 6) + sats
//   mode 3/4: 0 = ms + speed, 1 = lat + direction, 2 = lon + climb, 3 = altitude
// ★ THE SERIAL arrives in halves on a CONF channel (id 0xA–0xD, next nibble 0xC): a 16-bit half and which half it is.
//   A sonde exists here only when the SAME serial has been assembled twice from clean packets. DFM-06: the serial is the
//   24-bit value of channel 6 (hex digits), recognised by a zero "null" channel 0x5A/0x5B, also required twice.
// ★★ NEVER A GUESSED TEMPERATURE: measurement channels 0…5 must all have arrived clean, and the type must be one whose
//    reference resistor is documented (DFM-06 / DFM-09: 220 kΩ; DFM-17 sent as 0xB: 332 kΩ). Then R = Rf·(f−f₁)/f₂ and the
//    thermistor's DATASHEET table (EPCOS B57540G0502, R25 = 5 kΩ) gives T — our own Steinhart–Hart fit of that table.
//    Pressure / humidity variants (DFM-09P, DFM-17P, 0xC / 0xD) use other channels: no temperature (calOf 0).
#pragma once
#include "vibe_rs41.h"
#include "vibe_sonde_manch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace vibedfm {

constexpr double kChipRate = 2500.0;
constexpr const char* kRawHeader = "10011010100110010101101001010101";
constexpr uint16_t kHeader = 0x45CF;
constexpr int kFrameBits = 280, kConfPos = 16, kDat1Pos = 72, kDat2Pos = 176;

// ── Hamming(8,4) ────────────────────────────────────────────────────────────────────────────────────────────────────
/** The 8 codeword bits (index 0 first) of a nibble. */
inline void hamEncode(uint8_t nib, uint8_t* c) {
    const int d0 = (nib >> 3) & 1, d1 = (nib >> 2) & 1, d2 = (nib >> 1) & 1, d3 = nib & 1;
    c[0] = uint8_t(d0); c[1] = uint8_t(d1); c[2] = uint8_t(d2); c[3] = uint8_t(d3);
    c[4] = uint8_t(d1 ^ d2 ^ d3); c[5] = uint8_t(d0 ^ d2 ^ d3); c[6] = uint8_t(d0 ^ d1 ^ d3); c[7] = uint8_t(d0 ^ d1 ^ d2);
}
/** Decode one codeword (hard bits) → nibble. Returns 0 clean, 1 one bit corrected, −1 not correctable. */
inline int hamDecode(const uint8_t* c, uint8_t& nib) {
    // the syndrome's four checks, each a parity row; a single error at bit k gives the k-th column
    const int s0 = c[1] ^ c[2] ^ c[3] ^ c[4], s1 = c[0] ^ c[2] ^ c[3] ^ c[5], s2 = c[0] ^ c[1] ^ c[3] ^ c[6], s3 = c[0] ^ c[1] ^ c[2] ^ c[7];
    const int syn = s0 << 3 | s1 << 2 | s2 << 1 | s3;
    static const int col[8] = { 0x7, 0xB, 0xD, 0xE, 0x8, 0x4, 0x2, 0x1 };
    uint8_t b[4] = { c[0], c[1], c[2], c[3] };
    int r = 0;
    if (syn) {
        r = -1;
        for (int k = 0; k < 8; ++k) if (syn == col[k]) { r = 1; if (k < 4) b[k] ^= 1; break; }
    }
    nib = uint8_t(b[0] << 3 | b[1] << 2 | b[2] << 1 | b[3]);
    return r;
}

/** A block of L nibbles from its 8·L sent bits (hard, 0/1). Returns corrections made, or −1 if any codeword failed. */
inline int decodeBlock(const uint8_t* sent, int L, uint8_t* nibs) {
    int fixes = 0;
    for (int i = 0; i < L; ++i) {
        uint8_t c[8];
        for (int j = 0; j < 8; ++j) c[j] = sent[L * j + i];
        const int r = hamDecode(c, nibs[i]);
        if (r < 0) return -1;
        fixes += r;
    }
    return fixes;
}
/** The 8·L sent bits of a block of L nibbles. */
inline void encodeBlock(const uint8_t* nibs, int L, uint8_t* sent) {
    for (int i = 0; i < L; ++i) {
        uint8_t c[8];
        hamEncode(nibs[i], c);
        for (int j = 0; j < 8; ++j) sent[L * j + i] = c[j];
    }
}

/** The 280 frame bits (0/1) for a CONF (7 nibbles) and two DATs (13 each). */
inline void buildFrame(const uint8_t* conf, const uint8_t* dat1, const uint8_t* dat2, uint8_t* bits) {
    for (int i = 0; i < 16; ++i) bits[i] = uint8_t((kHeader >> (15 - i)) & 1);
    encodeBlock(conf, 7, bits + kConfPos);
    encodeBlock(dat1, 13, bits + kDat1Pos);
    encodeBlock(dat2, 13, bits + kDat2Pos);
}
/** A DAT packet's nibbles: the 32-bit value, the 16-bit value, the id. */
inline void datNibbles(uint32_t v32, uint16_t v16, int id, uint8_t* n) {
    for (int k = 0; k < 8; ++k) n[k] = uint8_t((v32 >> (28 - 4 * k)) & 0xF);
    for (int k = 0; k < 4; ++k) n[8 + k] = uint8_t((v16 >> (12 - 4 * k)) & 0xF);
    n[12] = uint8_t(id & 0xF);
}
inline void confNibbles(int id, uint32_t v24, uint8_t* n) {
    n[0] = uint8_t(id & 0xF);
    for (int k = 0; k < 6; ++k) n[1 + k] = uint8_t((v24 >> (20 - 4 * k)) & 0xF);
}

/** A measurement's 24-bit "float": exponent in the top nibble, 20-bit mantissa (value = m / 2^e). */
inline double fl24(uint32_t v) { return double(v & 0xFFFFF) / double(1u << ((v >> 20) & 0xF)); }

// ── The thermistor (datasheet table, our own fit) ───────────────────────────────────────────────────────────────────
/** EPCOS B57540G0502 (R/T characteristic 8402, R25 = 5 kΩ): R/R25 at −55…+40 °C in 5 °C steps — the manufacturer's
 *  table. T from R by a Steinhart–Hart fit (1/T = a + b·ln R + c·ln³R) we make of it by least squares at first use. */
struct Thermistor {
    double a = 0, b = 0, c = 0;
    static const Thermistor& get() { static Thermistor t; return t; }
    Thermistor() {
        static const double rr[20] = { 51.991, 37.989, 28.07, 20.96, 15.809, 12.037, 9.2484, 7.1668, 5.5993, 4.4087,
                                       3.4971, 2.7936, 2.2468, 1.8187, 1.4813, 1.2136, 1.0, 0.82845, 0.68991, 0.57742 };
        double M[3][4] = {{0}};
        for (int i = 0; i < 20; ++i) {
            const double L = std::log(rr[i] * 5000.0), y = 1.0 / (273.15 - 55.0 + 5.0 * i);
            const double x[3] = { 1, L, L * L * L };
            for (int r = 0; r < 3; ++r) { for (int k = 0; k < 3; ++k) M[r][k] += x[r] * x[k]; M[r][3] += x[r] * y; }
        }
        for (int p = 0; p < 3; ++p) {          // Gauss–Jordan, partial pivot
            int best = p;
            for (int r = p + 1; r < 3; ++r) if (std::fabs(M[r][p]) > std::fabs(M[best][p])) best = r;
            for (int k = 0; k < 4; ++k) std::swap(M[p][k], M[best][k]);
            for (int r = 0; r < 3; ++r) if (r != p) { const double f = M[r][p] / M[p][p]; for (int k = 0; k < 4; ++k) M[r][k] -= f * M[p][k]; }
        }
        a = M[0][3] / M[0][0]; b = M[1][3] / M[1][1]; c = M[2][3] / M[2][2];
    }
    double celsius(double R) const {
        if (!(R > 50 && R < 5e6)) return NAN;
        const double L = std::log(R);
        return 1.0 / (a + b * L + c * L * L * L) - 273.15;
    }
    /** The inverse (for our transmitter): R for T, by bisection on the fit. */
    double ohms(double tC) const {
        double lo = 50, hi = 5e6;
        for (int i = 0; i < 100; ++i) { const double m = std::sqrt(lo * hi); if (celsius(m) > tC) lo = m; else hi = m; }
        return std::sqrt(lo * hi);
    }
};

// ── One channel's DFM state: frames → a sonde ───────────────────────────────────────────────────────────────────────
class Chan {
public:
    explicit Chan(double chRate) : rx_(chRate, kChipRate, kRawHeader, 3) {}
    uint64_t syncs() const { return rx_.syncs(); }
    uint64_t framesSeen() const { return seen_; }

    /** Channel samples; onFrame(vibers41::Frame&) about once a second for a proven sonde. */
    template <class F> void process(const vibesonde::cf32* z, int n, F&& onFrame) {
        Sink<F> s{ this, onFrame };
        rx_.process(z, n, s);
    }

    // ── the frame, decoded (public for the tests) ──
    /** Judge one frame's 280 soft bits (header included). Returns false when no block of it is clean. */
    template <class F> bool frame(const float* soft, double snrDb, double offHz, double tS, F& onFrame) {
        uint8_t hb[kFrameBits];
        for (int i = 0; i < kFrameBits; ++i) hb[i] = soft[i] > 0 ? 1 : 0;
        uint8_t conf[7], d1[13], d2[13];
        const int rc = decodeBlock(hb + kConfPos, 7, conf);
        const int r1 = decodeBlock(hb + kDat1Pos, 13, d1);
        const int r2 = decodeBlock(hb + kDat2Pos, 13, d2);
        const bool okC = rc >= 0 && rc <= 1, ok1 = r1 >= 0 && r1 <= 3, ok2 = r2 >= 0 && r2 <= 3;   // ★ strict: no soft guesses
        if (!okC && !ok1 && !ok2) return false;
        ++seen_;
        snr_ = snrDb; off_ = offHz;
        if (okC) conf_(conf, tS, rc == 0);
        // ★ out once a second — right after packet 8 (the date, the epoch's last), before the next packet starts a new
        //   epoch — or every 5 s if it never comes; and only for a proven serial
        bool sent = false;
        auto put = [&](const uint8_t* d) {
            dat_(d, tS);
            if (d[12] == 8 && !serial_.empty()) { lastOut_ = tS; emit_(onFrame, tS); sent = true; }
        };
        if (ok1) put(d1);
        if (ok2) put(d2);
        if (!sent && !serial_.empty() && tS - lastOut_ >= 5.0) { lastOut_ = tS; emit_(onFrame, tS); }
        return true;
    }
    const std::string& serial() const { return serial_; }

private:
    template <class F> struct Sink {
        Chan* c; F& f;
        int need(const float*, int, float) { return kFrameBits - 16; }
        void done(const float* soft, int n, float, double snr, double off, double tS) {
            float full[kFrameBits];
            for (int i = 0; i < 16; ++i) full[i] = ((kHeader >> (15 - i)) & 1) ? 1.f : -1.f;
            for (int i = 0; i < n && i + 16 < kFrameBits; ++i) full[16 + i] = soft[i];
            c->frame(full, snr, off, tS, f);
        }
    };

    void conf_(const uint8_t* n, double tS, bool clean) {
        const int id = n[0];
        uint32_t v24 = 0;
        for (int k = 1; k < 7; ++k) v24 = (v24 << 4) | n[k];
        const uint32_t v20 = v24 & 0xFFFFF;
        if (id <= 8) { conf_v_[id] = v24; confAt_[id] = tS; }
        // the DFM-06 "null" channel: 0x5A / 0x5B followed by zeros
        if (id == 5 && (n[1] == 0xA || n[1] == 0xB) && v20 == 0) nul_ = 0x50 | n[1];
        if (id == 6 && nul_ >= 0) {
            bool bcd = true;
            for (int k = 1; k < 7; ++k) if (n[k] > 9) bcd = false;
            if (bcd && v24 != 0) {
                if (v24 == sn6_) adopt_(hex6_(v24), "DFM-06", 220e3);
                sn6_ = v24;
            }
        }
        // DFM-09 / -17: the serial channel, 0xA–0xD then 0xC, a 16-bit half and which half (0 / 1)
        if (id >= 0xA && id <= 0xD && n[1] == 0xC) {
            const int hl = int(v20 & 0xF);
            if (hl > 1) return;
            if (snCh_ != id) { snCh_ = id; halfMask_ = 0; halfClean_ = 0; }
            half_[hl] = uint16_t((v20 >> 4) & 0xFFFF);
            halfMask_ |= 1 << hl;
            if (clean) halfClean_ |= 1 << hl; else halfClean_ &= ~(1 << hl);
            if (halfMask_ == 3) {
                halfMask_ = 0;
                const bool bothClean = halfClean_ == 3;
                const uint32_t sn = (uint32_t(half_[0]) << 16) | half_[1];
                if (sn == 0) return;
                // ★ adopted when both halves came from codewords with NO correction, or when the same serial is
                //   assembled twice (a corrected bit could have been corrected wrongly)
                if (sn == cand_ || bothClean) {
                    char b[16]; std::snprintf(b, sizeof b, "%u", sn);
                    const char* model = id == 0xA ? (sn >= 23000000u ? "DFM-17" : "DFM-09") : id == 0xB ? "DFM-17" : id == 0xC ? "DFM-09P" : "DFM-17P";
                    // ★ the reference resistor only where it is documented for that id (else no temperature)
                    const double rf = id == 0xA && sn < 23000000u ? 220e3 : id == 0xB ? 332e3 : 0;
                    adopt_(b, model, rf);
                }
                cand_ = sn;
            }
        }
    }
    static std::string hex6_(uint32_t v) { char b[8]; std::snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
    void adopt_(const std::string& sn, const char* model, double rf) {
        if (serial_ == sn) return;
        if (!serial_.empty()) {   // ★ another sonde on this channel: none of the old one's measurements carry over
            for (auto& x : confAt_) x = -1;
            for (auto& x : datAt_) x = -1;
            frnr_ = prevFrnr_ = -1;
        }
        serial_ = sn; model_ = model; rf_ = rf;
    }

    void dat_(const uint8_t* n, double tS) {
        const int id = n[12];
        if (id > 8) return;
        uint32_t v32 = 0; uint16_t v16 = 0;
        for (int k = 0; k < 8; ++k) v32 = (v32 << 4) | n[k];
        for (int k = 8; k < 12; ++k) v16 = uint16_t((v16 << 4) | n[k]);
        // ★ EPOCHS: packet 0 opens one. Every packet is tagged with the epoch it arrived in, and a position is built
        //   only from packets of ONE epoch (a latitude never pairs with the next second's altitude).
        if (id == 0) {
            const int mode = int((v32 >> 8) & 0xFF);
            if (mode >= 1 && mode <= 4) mode_ = mode;
            prevFrnr_ = frnr_;
            frnr_ = mode_ <= 2 ? int(v32 & 0xFF) : -1;
            ++epoch_;
        }
        v32_[id] = v32; v16_[id] = v16; datAt_[id] = tS; epochOf_[id] = epoch_;
    }
    bool fresh_(int id, double tS) const { return datAt_[id] >= 0 && tS - datAt_[id] < 2.0; }

    template <class F> void emit_(F& onFrame, double tS) {
        vibers41::Frame fr;
        fr.kind = "DFM"; fr.model = model_;
        fr.statusOk = true; fr.serial = serial_;
        fr.frameNo = -1;
        fr.snrDb = snr_; fr.offsetHz = off_;
        const bool m2 = mode_ <= 2;
        const int iLat = m2 ? 2 : 1, iLon = m2 ? 3 : 2, iAlt = m2 ? 4 : 3, iMs = m2 ? 1 : 0;
        const long ep = epochOf_[iLat];
        if (fresh_(iLat, tS) && fresh_(iLon, tS) && fresh_(iAlt, tS) && epochOf_[iLon] == ep && epochOf_[iAlt] == ep
            && (m2 ? epochOf_[2] == ep && epochOf_[3] == ep && epochOf_[4] == ep : true)) {
            fr.frameNo = ep == epoch_ ? frnr_ : ep == epoch_ - 1 ? prevFrnr_ : -1;
            const double lat = int32_t(v32_[iLat]) / 1e7, lon = int32_t(v32_[iLon]) / 1e7, alt = int32_t(v32_[iAlt]) / 1e2;
            double hv, dir, vv;
            if (m2) { hv = int16_t(v16_[2]) / 1e2; dir = v16_[3] / 1e2; vv = int16_t(v16_[4]) / 1e2; }
            else    { hv = fresh_(0, tS) ? int16_t(v16_[0]) / 1e2 : NAN; dir = v16_[1] / 1e2; vv = int16_t(v16_[2]) / 1e2; }
            if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -500 && alt < 60000 && !(lat == 0 && lon == 0)
                && std::fabs(vv) < 200 && (!std::isfinite(hv) || std::fabs(hv) < 300)) {
                fr.posOk = true;
                fr.lat = lat; fr.lon = lon; fr.alt = alt; fr.vU = vv;
                if (std::isfinite(hv) && dir >= 0 && dir <= 360) { fr.vE = hv * std::sin(dir * M_PI / 180); fr.vN = hv * std::cos(dir * M_PI / 180); }
                else { fr.vE = fr.vN = 0; }
                if (fresh_(8, tS)) fr.sats = int(v16_[8] >> 8);
            }
        }
        if (fresh_(8, tS) && fresh_(iMs, tS)) {
            const uint32_t d = v32_[8];
            const int y = int(d >> 20), mo = int((d >> 16) & 0xF), dd = int((d >> 11) & 0x1F), hh = int((d >> 6) & 0x1F), mi = int(d & 0x3F);
            const double ms = m2 ? double(v16_[1]) : double(v32_[0] >> 16);
            if (y >= 2015 && y <= 2100 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh < 24 && mi < 60 && ms < 61000)
                fr.unixTime = double(vibesonde::daysFromCivil(y, mo, dd)) * 86400 + hh * 3600 + mi * 60 + ms / 1000.0;
        }
        // ★ calibration: channels 0…5 clean (the temperature and its two references among them)
        int have = 0;
        for (int k = 0; k < 6; ++k) if (confAt_[k] >= 0) ++have;
        fr.calN = have;
        fr.calOf = rf_ > 0 ? 6 : 0;
        if (rf_ > 0 && have == 6) {
            const double f = fl24(conf_v_[0]), f1 = fl24(conf_v_[3]), f2 = fl24(conf_v_[4]);
            if (f > f1 && f1 > 0 && f2 > 0) {
                const double t = Thermistor::get().celsius(rf_ * (f - f1) / f2);
                if (std::isfinite(t) && t > -100 && t < 60) fr.tempC = t;
            }
        }
        onFrame(fr);
    }

    vibesonde::ManchRx rx_;
    uint64_t seen_ = 0;
    double snr_ = 0, off_ = 0, lastOut_ = -1e9;
    uint32_t conf_v_[9] = {0}; double confAt_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t v32_[9] = {0}; uint16_t v16_[9] = {0}; double datAt_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int mode_ = 2, frnr_ = -1, prevFrnr_ = -1;
    long epoch_ = 0, epochOf_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int snCh_ = -1, halfMask_ = 0, halfClean_ = 0, nul_ = -1;
    uint16_t half_[2] = {0, 0};
    uint32_t cand_ = 0, sn6_ = 0;
    std::string serial_, model_;
    double rf_ = 0;
};

}  // namespace vibedfm
