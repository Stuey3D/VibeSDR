// vibe_dvsig_tx.h — OUR OWN TRANSMITTER for the digital-voice signalling decoder's tests and benchmark row (V12,
// docs/v12/DV-SIGNALLING.md). Each system's frames built from the same published facts as the receiver (vibe_dvsig_fec.h
// names every source), then 4-FSK through a root-raised-cosine (α = 0.2) or GMSK (BT = 0.5), FM-modulated to IQ at 24 kHz.
//
// ★★ A misreading shared by this transmitter and the receiver passes every test — the spec's own vectors are checked where
//    a spec gives them (test-dvsig §1), and the first real-air check is owed (DV-SIGNALLING.md "RESUME HERE").
// ★ The "voice" bits are random bytes: this transmitter has no vocoder and never will.
#pragma once
#include "vibe_dvsig.h"

#include <random>

namespace vibe {
namespace dvsig {
namespace tx {

using Syms = std::vector<int8_t>;
inline void addDibits(Syms& s, const uint8_t* bits, int n) { for (int i = 0; i + 1 < n; i += 2) s.push_back(int8_t(sym4(bits[i], bits[i + 1]))); }
inline void addHex(Syms& s, uint64_t v, int nBits) { const auto h = hexSyms(v, nBits); s.insert(s.end(), h.begin(), h.end()); }

// ── Modulators ────────────────────────────────────────────────────────────────────────────────────────────────────────
/** 4-FSK (or two-level) symbols at `baud` → IQ at 24 kHz, `devPerUnit` Hz per symbol unit, `offHz` off the dial. */
inline std::vector<cf32> fsk(const Syms& syms, int baud, double devPerUnit, double offHz = 0, double* phase = nullptr, bool gauss = false) {
    const int sps = 24000 / baud;
    std::vector<float> shape;
    if (gauss) {   // GMSK, BT 0.5: a Gaussian pulse of one symbol convolved with a rectangle of one symbol
        const double bt = 0.5, a = std::sqrt(std::log(2.0) / 2.0) / bt;
        const int n = 4 * sps + 1, h = n / 2;
        std::vector<float> g(static_cast<size_t>(n)); double sum = 0;
        for (int i = 0; i < n; ++i) { const double t = double(i - h) / sps; g[size_t(i)] = float(std::exp(-(M_PI * M_PI / (a * a)) * t * t)); sum += g[size_t(i)]; }
        shape.assign(size_t(n + sps - 1), 0.f);
        for (int i = 0; i < n; ++i) for (int k = 0; k < sps; ++k) shape[size_t(i + k)] += float(g[size_t(i)] / sum);
    } else {
        // ★ RRC with unity DC gain; an impulse a symbol scaled by sps gives a steady level of the symbol's value
        shape = rrcTaps(sps, 0.2, 10);
        for (auto& v : shape) v = float(v * sps);
    }
    const size_t n = syms.size() * size_t(sps) + shape.size();
    std::vector<float> f(n, 0.f);
    for (size_t s = 0; s < syms.size(); ++s) {
        const float a = float(syms[s]);
        for (size_t t = 0; t < shape.size(); ++t) f[s * size_t(sps) + t] += a * shape[t];
    }
    std::vector<cf32> iq(n);
    double ph = phase ? *phase : 0.0;
    for (size_t i = 0; i < n; ++i) {
        ph += 2 * M_PI * (double(f[i]) * devPerUnit + offHz) / 24000.0;
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        iq[i] = cf32(float(std::cos(ph)), float(std::sin(ph)));
    }
    if (phase) *phase = ph;
    return iq;
}
inline void silence(std::vector<cf32>& v, double sec) { v.insert(v.end(), size_t(sec * 24000), cf32(0, 0)); }
/** AWGN to a C/N (dB) measured in `bwHz` of the 24 kHz stream (the carrier at unit power). */
inline void awgn(std::vector<cf32>& v, double cnDb, double bwHz, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double pn = std::pow(10.0, -cnDb / 10.0) * (24000.0 / bwHz);
    const float sd = float(std::sqrt(pn / 2.0));
    for (auto& z : v) z += cf32(nd(rng) * sd, nd(rng) * sd);
}

// ═══ DMR ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════
struct DmrCall {
    int cc = 1, slot = 1;              ///< slot 1/2 (BS downlink) — for MS-sourced the label is not sent
    bool group = true, privacy = false;
    uint32_t src = 2345678, dst = 235;
    std::string alias;                 ///< talker alias (7-bit), empty = none
    bool gps = false; double lat = 0, lon = 0;
    int superframes = 6;
    std::string sms;                   ///< if set, a Short Data Defined (ISO 8859-1) message is sent instead of voice
};
inline void lcBytes(const DmrCall& c, int flco, uint8_t* lc) {
    std::memset(lc, 0, 9);
    lc[0] = uint8_t(flco & 0x3F);
    if (flco == 0 || flco == 3) {
        lc[2] = c.privacy ? 0x40 : 0;
        lc[3] = uint8_t(c.dst >> 16); lc[4] = uint8_t(c.dst >> 8); lc[5] = uint8_t(c.dst);
        lc[6] = uint8_t(c.src >> 16); lc[7] = uint8_t(c.src >> 8); lc[8] = uint8_t(c.src);
    } else if (flco >= 4 && flco <= 7) {   // talker alias, 7-bit ASCII
        uint8_t bits[217] = {};
        for (size_t i = 0; i < c.alias.size() && i < 31; ++i) putBits(bits, int(7 * i), 7, uint32_t(c.alias[i]) & 0x7F);
        if (flco == 4) {
            lc[2] = uint8_t((0 << 6) | ((c.alias.size() & 0x1F) << 1) | bits[0]);
            for (int i = 0; i < 6; ++i) lc[3 + i] = uint8_t(takeBits(bits, 1 + 8 * i, 8));
        } else {
            const int off = 49 + 56 * (flco - 5);
            for (int i = 0; i < 7; ++i) lc[2 + i] = uint8_t(takeBits(bits, off + 8 * i, 8));
        }
    } else if (flco == 8) {
        int32_t lon = int32_t(std::lround(c.lon * 33554432.0 / 360.0)), lat = int32_t(std::lround(c.lat * 16777216.0 / 180.0));
        const uint32_t ulon = uint32_t(lon) & 0x1FFFFFF, ulat = uint32_t(lat) & 0xFFFFFF;
        lc[2] = uint8_t((1 << 1) | (ulon >> 24)); lc[3] = uint8_t(ulon >> 16); lc[4] = uint8_t(ulon >> 8); lc[5] = uint8_t(ulon);
        lc[6] = uint8_t(ulat >> 16); lc[7] = uint8_t(ulat >> 8); lc[8] = uint8_t(ulat);
    }
}
/** One 264-bit burst (no CACH): 108 + 48 + 108. `mid48` = SYNC or EMB+embedded; `info` 216 bits (voice or 196+slot). */
inline void dmrBurstSyms(Syms& s, const uint8_t* first108, const uint8_t* mid48, const uint8_t* last108) {
    addDibits(s, first108, 108); addDibits(s, mid48, 48); addDibits(s, last108, 108);
}
inline void syncBits(uint64_t v, bool data, uint8_t* b48) { for (int i = 0; i < 48; ++i) b48[i] = uint8_t(((v >> (47 - i)) & 1u) ^ (data ? 1u : 0u)); }
/** A data burst's 264 bits: info₁ 98 · slot type 10 · sync 48 · slot type 10 · info₂ 98. */
inline void dataBurst(Syms& s, int cc, int dt, const uint8_t* b12, uint64_t syncVoice) {
    uint8_t info[96], tx[196]; bytesToBitsMsb(b12, 96, info); bptc196Encode(info, tx);
    const uint32_t st = golay20Encode(uint8_t(((cc & 0xF) << 4) | (dt & 0xF)));
    uint8_t stb[20]; for (int i = 0; i < 20; ++i) stb[i] = uint8_t((st >> (19 - i)) & 1u);
    uint8_t f[108], m[48], l[108];
    std::memcpy(f, tx, 98); std::memcpy(f + 98, stb, 10);
    syncBits(syncVoice, true, m);
    std::memcpy(l, stb + 10, 10); std::memcpy(l + 10, tx + 98, 98);
    dmrBurstSyms(s, f, m, l);
}
inline void voiceBurst(Syms& s, std::mt19937& rng, const uint8_t* mid48) {
    uint8_t f[108], l[108];
    for (auto& v : f) v = uint8_t(rng() & 1); for (auto& v : l) v = uint8_t(rng() & 1);
    dmrBurstSyms(s, f, mid48, l);
}
inline void embMid(int cc, int lcss, const uint8_t* frag32, uint8_t* m48) {
    const uint16_t e = qr16Encode(uint8_t(((cc & 0xF) << 3) | (lcss & 3)));
    for (int i = 0; i < 8; ++i) m48[i] = uint8_t((e >> (15 - i)) & 1u);
    std::memcpy(m48 + 8, frag32, 32);
    for (int i = 0; i < 8; ++i) m48[40 + i] = uint8_t((e >> (7 - i)) & 1u);
}
inline void cachSyms(Syms& s, int tc, std::mt19937& rng) {
    const uint32_t cw = h7_4().encode(uint32_t((1u << 3) | (uint32_t(tc & 1) << 2)));   // AT busy, TC, LCSS 00
    uint8_t b[24];
    for (auto& v : b) v = uint8_t(rng() & 1);   // the 17 payload bits: not decoded by us
    const int at[7] = {0, 4, 8, 12, 14, 18, 22};
    for (int i = 0; i < 7; ++i) b[at[i]] = uint8_t((cw >> (6 - i)) & 1u);
    addDibits(s, b, 24);
}
inline void idleBytes(uint8_t* b) { for (int i = 0; i < 12; ++i) b[i] = uint8_t(0xA5 ^ (i * 37)); }
/** The 12-octet header / terminator / CSBK / data header with its check (RS or CRC with the mask). */
inline void lcWithRs(const uint8_t* lc9, uint32_t mask, uint8_t* b12) {
    std::memcpy(b12, lc9, 9); uint8_t p[3]; rs129Parity(lc9, p);
    b12[9] = uint8_t(p[0] ^ (mask >> 16)); b12[10] = uint8_t(p[1] ^ (mask >> 8)); b12[11] = uint8_t(p[2] ^ mask);
}
inline void withCrc(uint8_t* b12, uint16_t mask) { const uint16_t c = uint16_t(crcCcittMsb(b12, 10) ^ mask); b12[10] = uint8_t(c >> 8); b12[11] = uint8_t(c); }

/** A whole DMR transmission as symbols. `bs` = a repeater's downlink (CACH, both slots, the other slot idle); else an
 *  MS-sourced simplex burst every 60 ms (the gaps are no carrier: `gaps` gets their symbol positions). */
inline Syms dmr(const DmrCall& c, bool bs, uint32_t seed, std::vector<std::pair<size_t, size_t>>* gaps = nullptr) {
    std::mt19937 rng(seed);
    Syms s;
    const uint64_t sync = bs ? kDmrBsVoice : kDmrMsVoice;
    const int si = c.slot == 2 ? 1 : 0;
    std::vector<std::function<void(Syms&)>> bursts;   // the traffic slot's bursts, in order
    uint8_t lc[9], b12[12];
    if (!c.sms.empty()) {
        // Short Data: Defined (DD 3 = ISO 8859-1): header + rate-½ unconfirmed blocks; the last carries the CRC-32
        std::vector<uint8_t> d(c.sms.begin(), c.sms.end());
        while ((d.size() + 4) % 12) d.push_back(0);
        const size_t n = d.size();
        std::vector<uint8_t> sw(n); for (size_t i = 0; i + 1 < n; i += 2) { sw[i] = d[i + 1]; sw[i + 1] = d[i]; }
        std::vector<uint8_t> bits(n * 8); bytesToBitsMsb(sw.data(), int(n * 8), bits.data());
        const uint32_t crc = crcBits(bits.data(), int(n * 8), 32, 0x04C11DB7u, 0);
        d.push_back(uint8_t(crc)); d.push_back(uint8_t(crc >> 8)); d.push_back(uint8_t(crc >> 16)); d.push_back(uint8_t(crc >> 24));
        const int blocks = int(d.size() / 12);
        uint8_t h[12] = {};
        h[0] = uint8_t((c.group ? 0x80 : 0) | (((blocks >> 4) & 3) << 4) | 13);
        h[1] = uint8_t((10 << 4) | (blocks & 0x0F));
        h[2] = uint8_t(c.dst >> 16); h[3] = uint8_t(c.dst >> 8); h[4] = uint8_t(c.dst);
        h[5] = uint8_t(c.src >> 16); h[6] = uint8_t(c.src >> 8); h[7] = uint8_t(c.src);
        h[8] = uint8_t(3 << 2 | 1); h[9] = 0;
        withCrc(h, kMaskDataHdr);
        std::array<uint8_t, 12> hh; std::memcpy(hh.data(), h, 12);
        bursts.push_back([=](Syms& o) { dataBurst(o, c.cc, 6, hh.data(), sync); });
        for (int k = 0; k < blocks; ++k) { std::array<uint8_t, 12> bk; std::memcpy(bk.data(), d.data() + 12 * k, 12); bursts.push_back([=](Syms& o) { dataBurst(o, c.cc, 7, bk.data(), sync); }); }
    } else {
        lcBytes(c, c.group ? 0 : 3, lc);
        lcWithRs(lc, kMaskVoiceHdr, b12);
        std::array<uint8_t, 12> hdr; std::memcpy(hdr.data(), b12, 12);
        for (int k = 0; k < 2; ++k) bursts.push_back([=](Syms& o) { dataBurst(o, c.cc, 1, hdr.data(), sync); });
        // embedded LC rotation: the call's LC, then alias header + 3 blocks, then GPS
        std::vector<int> rot = {c.group ? 0 : 3};
        if (!c.alias.empty()) { rot.push_back(4); rot.push_back(5); rot.push_back(6); rot.push_back(7); }
        if (c.gps) rot.push_back(8);
        for (int sf = 0; sf < c.superframes; ++sf) {
            uint8_t elc[9]; lcBytes(c, rot[size_t(sf) % rot.size()], elc);
            uint8_t frag[4][32]; embEncode(elc, frag);
            std::array<std::array<uint8_t, 48>, 6> mids;
            uint8_t m[48];
            syncBits(sync, false, m); std::memcpy(mids[0].data(), m, 48);
            for (int k = 0; k < 4; ++k) { embMid(c.cc, k == 0 ? 1 : k == 3 ? 2 : 3, frag[k], m); std::memcpy(mids[size_t(k + 1)].data(), m, 48); }
            uint8_t nul[32] = {}; embMid(c.cc, 0, nul, m); std::memcpy(mids[5].data(), m, 48);
            for (int k = 0; k < 6; ++k) { const auto mm = mids[size_t(k)]; const uint32_t sd = rng(); bursts.push_back([=](Syms& o) { std::mt19937 r(sd); voiceBurst(o, r, mm.data()); }); }
        }
        lcWithRs(lc, kMaskTerm, b12);
        std::array<uint8_t, 12> term; std::memcpy(term.data(), b12, 12);
        bursts.push_back([=](Syms& o) { dataBurst(o, c.cc, 2, term.data(), sync); });
    }
    uint8_t idle[12]; idleBytes(idle);
    if (bs) {
        // the downlink: every 30 ms a CACH and a burst; the traffic slot alternates with an idle other slot
        for (size_t k = 0; k < bursts.size(); ++k) {
            for (int sl = 0; sl < 2; ++sl) {
                cachSyms(s, sl, rng);
                if (sl == si) bursts[k](s); else dataBurst(s, c.cc, 9, idle, sync);
            }
        }
    } else {
        for (size_t k = 0; k < bursts.size(); ++k) {
            bursts[k](s);
            const size_t g0 = s.size();
            for (int i = 0; i < 144 + 12; ++i) s.push_back(0);   // the other slot + the CACH time: nothing sent
            if (gaps) gaps->push_back({g0, s.size()});
        }
    }
    return s;
}
inline std::vector<cf32> dmrIq(const DmrCall& c, bool bs, uint32_t seed, double offHz = 0) {
    std::vector<std::pair<size_t, size_t>> gaps;
    const Syms s = dmr(c, bs, seed, &gaps);
    auto iq = fsk(s, 4800, 648.0, offHz);
    for (const auto& g : gaps) for (size_t i = g.first * 5 + 40; i < g.second * 5 && i < iq.size(); ++i) iq[i] = cf32(0, 0);
    return iq;
}

// ═══ YSF ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════
struct YsfCall { std::string src = "G4ABC", dst = "ALL", down = "GB7XX", up = ""; int dg = 0; bool gps = false; double lat = 0, lon = 0; int frames = 16; int dt = 2; };
inline void ysfCs(const std::string& s, uint8_t* p10) { for (int i = 0; i < 10; ++i) p10[i] = uint8_t(i < int(s.size()) ? s[size_t(i)] : ' '); }
inline void ysfFrame(Syms& s, const uint8_t* fich4, const uint8_t* payload720 /* 360 dibits */) {
    addHex(s, kYsfSync, 40);
    const Bits f = ysfFichEncode(fich4);
    addDibits(s, f.data(), 200);
    addDibits(s, payload720, 720);
}
/** Mic-E encode (APRS101 ch. 10) for the YSF GPS block. */
inline void micEEncode(double lat, double lon, uint8_t* d6, uint8_t* i3) {
    const bool n = lat >= 0, w = lon < 0; lat = std::fabs(lat); lon = std::fabs(lon);
    const int ld = int(lat); const double lmf = (lat - ld) * 60.0; const int lm = int(lmf); const int lh = int(std::lround((lmf - lm) * 100)) % 100;
    const int dig[6] = {ld / 10, ld % 10, lm / 10, lm % 10, lh / 10, lh % 10};
    int od = int(lon); const double omf = (lon - od) * 60.0; const int om = int(omf); const int oh = int(std::lround((omf - om) * 100)) % 100;
    const bool p100 = od >= 100 || od < 10;
    for (int k = 0; k < 6; ++k) {
        const bool hi = (k == 3 && n) || (k == 4 && p100) || (k == 5 && w);
        d6[k] = uint8_t(hi ? 'P' + dig[k] : '0' + dig[k]);
    }
    int dd = od;
    if (od < 10) dd = od + 90 + 100;          // encodes as 190..199 → −190
    else if (od >= 100 && od <= 109) dd = od + 80;   // 180..189 → −80
    i3[0] = uint8_t((p100 ? dd - 100 : dd) + 28);
    i3[1] = uint8_t(om + 28); i3[2] = uint8_t(oh + 28);
}
inline Syms ysf(const YsfCall& c, uint32_t seed) {
    std::mt19937 rng(seed);
    Syms s;
    auto fichOf = [&](int fi, int fn, int ft, int dt) { std::array<uint8_t, 4> f{}; f[0] = uint8_t((fi << 6) | (0 << 2) | 0); f[1] = uint8_t((0 << 6) | (fn << 3) | ft); f[2] = uint8_t(dt & 3); f[3] = uint8_t(c.dg & 0x7F); return f; };
    auto csd = [&](const std::string& a, const std::string& b, uint8_t* bits360) {
        uint8_t o[22]; ysfCs(a, o); ysfCs(b, o + 10); ysfWhiten(o, 20); ysfAddCrc(o, 22);
        const Bits v = ysfConvIl(o, 22, 9); std::memcpy(bits360, v.data(), 360);
    };
    auto hdrFrame = [&](int fi) {
        uint8_t c1[360], c2[360], pl[720];
        csd(c.dst, c.src, c1); csd(c.down, c.up, c2);
        for (int b = 0; b < 5; ++b) { std::memcpy(pl + 144 * b, c1 + 72 * b, 72); std::memcpy(pl + 144 * b + 72, c2 + 72 * b, 72); }
        const auto f = fichOf(fi, 0, 7, c.dt);
        ysfFrame(s, f.data(), pl);
    };
    // the GPS block (FN 6 and 7 of a VD2 superframe): 0, type "\"b", 0, radio, 6 Mic-E chars, 3 lon chars, …, 0x03, sum
    uint8_t g[20] = {};
    if (c.gps) {
        g[0] = 0x00; g[1] = 0x22; g[2] = 0x62; g[3] = 0x00; g[4] = 0x28;
        micEEncode(c.lat, c.lon, g + 5, g + 11);
        g[14] = 0x03; unsigned sum = 0; for (int k = 0; k <= 14; ++k) sum += g[k]; g[15] = uint8_t(sum);
    }
    hdrFrame(0);
    for (int k = 0; k < c.frames; ++k) {
        const int fn = k % 8;
        uint8_t pl[720];
        for (auto& v : pl) v = uint8_t(rng() & 1);
        uint8_t o[12] = {};
        const std::string* cs = fn == 0 ? &c.dst : fn == 1 ? &c.src : fn == 2 ? &c.down : fn == 3 ? &c.up : nullptr;
        if (cs) ysfCs(*cs, o); else if ((fn == 6 || fn == 7) && c.gps) std::memcpy(o, g + 10 * (fn - 6), 10); else std::memset(o, ' ', 10);
        ysfWhiten(o, 10); ysfAddCrc(o, 12);
        const Bits d = ysfConvIl(o, 12, 5);
        for (int b = 0; b < 5; ++b) std::memcpy(pl + 144 * b, d.data() + 40 * b, 40);
        const auto f = fichOf(1, fn, 7, 2);
        ysfFrame(s, f.data(), pl);
    }
    hdrFrame(2);
    return s;
}

// ═══ D-STAR ════════════════════════════════════════════════════════════════════════════════════════════════════════════
struct DstarCall { std::string my = "G4ABC", my2 = "ID51", ur = "CQCQCQ", rpt1 = "GB7XX  B", rpt2 = "GB7XX  G"; std::string msg = "Hello from VibeSDR "; std::string dprs; int frames = 84; bool header = true; };
inline void dsBits(Syms& s, const char* b) { for (const char* p = b; *p; ++p) s.push_back(*p == '1' ? 1 : -1); }
inline void dsBytesLsb(Syms& s, const uint8_t* p, int n) { for (int i = 0; i < n; ++i) for (int b = 0; b < 8; ++b) s.push_back(((p[i] >> b) & 1) ? 1 : -1); }
inline void dsHeader41(const DstarCall& c, uint8_t* h) {
    std::memset(h, ' ', 41); h[0] = 0x40; h[1] = 0; h[2] = 0;
    auto put = [&](int off, const std::string& v, int n) { for (int i = 0; i < n; ++i) h[off + i] = uint8_t(i < int(v.size()) ? v[size_t(i)] : ' '); };
    put(3, c.rpt2, 8); put(11, c.rpt1, 8); put(19, c.ur, 8); put(27, c.my, 8); put(35, c.my2, 4);
    const uint16_t crc = crcX25(h, 39); h[39] = uint8_t(crc); h[40] = uint8_t(crc >> 8);
}
inline Syms dstar(const DstarCall& c, uint32_t seed) {
    std::mt19937 rng(seed);
    Syms s;
    uint8_t h[41]; dsHeader41(c, h);
    if (c.header) {
        for (int i = 0; i < 64; ++i) s.push_back((i & 1) ? -1 : 1);   // bit sync 1010…
        dsBits(s, "111011001010000");
        const Bits hb = dstarHeaderEncode(h);
        for (int i = 0; i < 660; ++i) s.push_back(hb[size_t(i)] ? 1 : -1);
    } else {
        for (int i = 0; i < 2000; ++i) s.push_back((rng() & 1) ? 1 : -1);   // joined late: something random before
    }
    // slow-data blocks: message (4 × 0x40+n), the header again (0x55 ×8 + 0x51), then D-PRS lines (0x35 …)
    std::vector<std::array<uint8_t, 6>> blocks;
    for (int n = 0; n < 4; ++n) { std::array<uint8_t, 6> b{}; b[0] = uint8_t(0x40 | n); for (int k = 0; k < 5; ++k) b[size_t(1 + k)] = uint8_t(size_t(5 * n + k) < c.msg.size() ? c.msg[size_t(5 * n + k)] : ' '); blocks.push_back(b); }
    for (int o = 0; o < 41; o += 5) { std::array<uint8_t, 6> b{}; const int nn = std::min(5, 41 - o); b[0] = uint8_t(0x50 | nn); for (int k = 0; k < nn; ++k) b[size_t(1 + k)] = h[o + k]; for (int k = nn; k < 5; ++k) b[size_t(1 + k)] = 0x66; blocks.push_back(b); }
    if (!c.dprs.empty()) { const std::string l = c.dprs + "\r"; for (size_t o = 0; o < l.size(); o += 5) { std::array<uint8_t, 6> b{}; const int nn = int(std::min<size_t>(5, l.size() - o)); b[0] = uint8_t(0x30 | nn); for (int k = 0; k < nn; ++k) b[size_t(1 + k)] = uint8_t(l[o + size_t(k)]); for (int k = nn; k < 5; ++k) b[size_t(1 + k)] = 0x66; blocks.push_back(b); } }
    size_t bi = 0;
    const Bits& pn = dstarPn();
    for (int j = 0; j < c.frames; ++j) {
        for (int i = 0; i < 72; ++i) s.push_back((rng() & 1) ? 1 : -1);   // voice: random, never read
        if (j % 21 == 0) { dsBits(s, kDsDataBits); continue; }
        const int half = (j % 21 - 1) & 1;
        std::array<uint8_t, 6> b{}; b.fill(0x66);
        if ((j % 21) <= 20) b = blocks[bi % blocks.size()];
        if (half == 1) ++bi;
        uint8_t o[3] = {b[size_t(3 * half)], b[size_t(3 * half + 1)], b[size_t(3 * half + 2)]};
        for (int i = 0; i < 24; ++i) { const int bit = ((o[i >> 3] >> (i & 7)) & 1) ^ pn[size_t(i)]; s.push_back(bit ? 1 : -1); }
    }
    dsBits(s, "101010101010101010101010101010100001001101011110");   // the end pattern
    for (int i = 0; i < 48; ++i) s.push_back(-1);
    return s;
}
inline std::vector<cf32> dstarIq(const DstarCall& c, uint32_t seed, double offHz = 0) { return fsk(dstar(c, seed), 4800, 1200.0, offHz, nullptr, true); }

// ═══ NXDN ══════════════════════════════════════════════════════════════════════════════════════════════════════════════
struct NxdnCall { int ran = 1; uint32_t src = 1234, dst = 10; bool group = true; int frames = 16; };
inline Syms nxdn(const NxdnCall& c, uint32_t seed) {
    std::mt19937 rng(seed);
    Syms s;
    auto l3 = [&](int type, uint8_t* bits80) {
        uint8_t o[10] = {}; o[0] = uint8_t(type & 0x3F); o[1] = 0; o[2] = c.group ? 0x00 : 0x80;
        o[3] = uint8_t(c.src >> 8); o[4] = uint8_t(c.src); o[5] = uint8_t(c.dst >> 8); o[6] = uint8_t(c.dst);
        bytesToBitsMsb(o, 80, bits80);
    };
    auto frame = [&](int rfct, int fct, int opt, const uint8_t* sacch26, const uint8_t* f1, const uint8_t* f2) {
        std::vector<int8_t> f;
        addHex(f, kNxFsw, 20);
        uint8_t lich = uint8_t((rfct << 6) | (fct << 4) | (opt << 2) | (1 << 1));
        const int par = ((lich >> 7) ^ (lich >> 6) ^ (lich >> 5) ^ (lich >> 4)) & 1; lich = uint8_t(lich | par);
        for (int i = 0; i < 8; ++i) f.push_back(int8_t(sym4((lich >> (7 - i)) & 1, 1)));
        uint8_t si[32]; std::memcpy(si, sacch26, 26); putBits(si, 26, 6, nxdnCrc6(si, 26));
        const Bits sb = nxdnEncode(si, 32, false);
        addDibits(f, sb.data(), 60);
        for (int h = 0; h < 2; ++h) {
            const uint8_t* fa = h == 0 ? f1 : f2;
            if (fa) { uint8_t fi[92]; std::memcpy(fi, fa, 80); putBits(fi, 80, 12, nxdnCrc12(fi, 80)); const Bits fb = nxdnEncode(fi, 92, true); addDibits(f, fb.data(), 144); }
            else { uint8_t r[144]; for (auto& v : r) v = uint8_t(rng() & 1); addDibits(f, r, 144); }
        }
        const Bits& pn = nxdnPn();
        for (size_t k = 0; k < f.size(); ++k) if (pn[k]) f[k] = int8_t(-f[k]);
        s.insert(s.end(), f.begin(), f.end());
    };
    uint8_t vc[80]; l3(0x01, vc);
    uint8_t rel[80]; l3(0x08, rel);
    uint8_t sh[26] = {}; putBits(sh, 0, 2, 0); putBits(sh, 2, 6, uint32_t(c.ran));
    for (int k = 0; k < 2; ++k) frame(1, 0, 0, sh, vc, vc);   // header: FACCH1 VCALL in both halves
    for (int k = 0; k < c.frames; ++k) {
        const int idx = k % 4;
        uint8_t sa[26] = {}; putBits(sa, 0, 2, uint32_t(3 - idx)); putBits(sa, 2, 6, uint32_t(c.ran)); std::memcpy(sa + 8, vc + 18 * idx, 18);
        frame(1, 2, 3, sa, nullptr, nullptr);
    }
    frame(1, 0, 0, sh, rel, rel);
    return s;
}
inline std::vector<cf32> nxdnIq(const NxdnCall& c, uint32_t seed, bool n96 = false, double offHz = 0) { return fsk(nxdn(c, seed), n96 ? 4800 : 2400, n96 ? 800.0 : 350.0, offHz); }

// ═══ dPMR ══════════════════════════════════════════════════════════════════════════════════════════════════════════════
struct DpmrCall { uint32_t own = 1234567, called = 7654321; int cc = 0x28A; int superframes = 4; };
inline Syms dpmr(const DpmrCall& c, uint32_t seed) {
    std::mt19937 rng(seed);
    Syms s;
    for (int i = 0; i < 9; ++i) addHex(s, 0x5F, 8);   // preamble
    addHex(s, kPmFs1v, 48);
    uint8_t hi[80] = {};
    putBits(hi, 0, 4, 0); putBits(hi, 4, 24, c.called); putBits(hi, 28, 24, c.own); putBits(hi, 52, 3, 0); putBits(hi, 55, 2, 0); putBits(hi, 57, 2, 1);
    putBits(hi, 72, 8, dpmrCrc8(hi, 72));
    const Bits hb = dpmrBlockEncode(hi, 80);
    auto ccSyms = [&](Syms& o) { for (int i = 0; i < 12; ++i) o.push_back(((c.cc >> (11 - i)) & 1) ? -3 : 3); };
    addDibits(s, hb.data(), 120); ccSyms(s); addDibits(s, hb.data(), 120);
    for (int sf = 0; sf < c.superframes; ++sf) {
        for (int fn = 0; fn < 4; ++fn) {
            if ((fn & 1) == 0) addHex(s, kPmFs2v, 24); else ccSyms(s);
            uint8_t cch[48] = {};
            const uint32_t id = fn == 0 ? (c.called >> 12) : fn == 1 ? (c.called & 0xFFF) : fn == 2 ? (c.own >> 12) : (c.own & 0xFFF);
            putBits(cch, 0, 2, uint32_t(fn)); putBits(cch, 2, 12, id);
            putBits(cch, 41, 7, dpmrCrc7(cch, 41));
            const Bits cb = dpmrBlockEncode(cch, 48);
            addDibits(s, cb.data(), 72);
            for (int i = 0; i < 144; ++i) s.push_back(int8_t(sym4(int(rng() & 1), int(rng() & 1))));
        }
    }
    addHex(s, kPmFs3v, 24);
    uint8_t en[24] = {}; putBits(en, 17, 7, dpmrCrc7(en, 17));
    Bits eb = dpmrBlockEncode(en, 24);
    addDibits(s, eb.data(), 36); addDibits(s, eb.data(), 36);
    return s;
}
inline std::vector<cf32> dpmrIq(const DpmrCall& c, uint32_t seed, double offHz = 0) { return fsk(dpmr(c, seed), 2400, 350.0, offHz); }

inline std::vector<cf32> ysfIq(const YsfCall& c, uint32_t seed, double offHz = 0) { return fsk(ysf(c, seed), 4800, 600.0, offHz); }

}  // namespace tx
}  // namespace dvsig
}  // namespace vibe
