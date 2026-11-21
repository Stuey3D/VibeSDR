// vibe_dvsig.h — DIGITAL-VOICE SIGNALLING (V12, docs/v12/DV-SIGNALLING.md): who is on a DMR, D-STAR, System Fusion (YSF),
// NXDN or dPMR channel — colour code / RAN, slot, talkgroup or private call, source → destination, callsigns, talker
// alias, slow-data text, GPS — and NEVER THE VOICE.
//
// ★★★ NO VOICE, EVER (Stuart). AMBE / AMBE+2 / IMBE are permanently excluded: the vocoder bits of every system are skipped
//     by their position in the frame and never read. Encrypted traffic is NAMED as encrypted and nothing more.
// ★★★ THE LAW (UK WTA 2006 s.48) IS DECIDED HERE, ON THE SERVER: the receiver keeps everything, and json() writes two
//     copies — the IDENTITIES copy (callsigns, IDs, aliases, text, GPS) and the PUBLIC copy ("DMR heard · CC 1 · slot 2").
//     Which copy a socket gets is the shim's decision (vibe_dvsig_policy below): identities on an AMATEUR frequency (the
//     band plan of the server's ITU region, as TONES does), elsewhere only to the owner's own signed-in sockets and only
//     with the owner's opt-in. Non-amateur positions never reach a map.
//
// The pieces: vibe_dvsig_fec.h (codes), vibe_dvsig_front.h (the shared 4-FSK / GMSK front end and the sync correlators),
// then here: one class per system that takes the front end's candidates, reads its frames and follows a transmission; the
// Receiver (AUTO or one system pinned, the log of transmissions, the report); the Listener (the worker thread).
//
// ★★ NOT A FALSE STATION ON NOISE. A sync match is only ever a CANDIDATE. Nothing is logged until a code with a check
//    passes: DMR — a burst's slot type (Golay) AND its BPTC AND its CRC / Reed-Solomon, or three voice bursts whose EMB
//    (QR(16,7)) agree on the colour code at the right 60 ms spacing; YSF — the FICH's four Golay words AND its CRC-16;
//    D-STAR — the header's CRC-16 after the Viterbi, or two data syncs exactly 420 ms apart; NXDN — FACCH1's CRC-12, or
//    two consecutive SACCH CRC-6 with the same RAN; dPMR — the header's CRC-8 with every Hamming word clean, or two
//    consecutive CCH CRC-7 at 80 ms. test-dvsig runs minutes of noise and analogue FM speech and requires ZERO entries.
#pragma once
#include "vibe_dmr_id.h"   // ★ V12 beta90: DMR ID → callsign (owner switch, RadioID.net per-ID lookup)
#include "vibe_dvsig_front.h"
#include "vibe_bands.h"
#include "vibe_thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace vibe {
namespace dvsig {

enum class Sys : int { None = 0, Dmr, Dstar, Ysf, Nxdn, Dpmr };
inline const char* sysName(Sys s) {
    switch (s) { case Sys::Dmr: return "DMR"; case Sys::Dstar: return "D-STAR"; case Sys::Ysf: return "YSF"; case Sys::Nxdn: return "NXDN"; case Sys::Dpmr: return "dPMR"; default: return ""; }
}
inline const char* sysKey(Sys s) {
    switch (s) { case Sys::Dmr: return "dmr"; case Sys::Dstar: return "dstar"; case Sys::Ysf: return "ysf"; case Sys::Nxdn: return "nxdn"; case Sys::Dpmr: return "dpmr"; default: return "auto"; }
}
inline Sys sysFromKey(const std::string& k) {
    if (k == "dmr") return Sys::Dmr; if (k == "dstar") return Sys::Dstar; if (k == "ysf") return Sys::Ysf;
    if (k == "nxdn") return Sys::Nxdn; if (k == "dpmr") return Sys::Dpmr; return Sys::None;
}

/** VIBE_DVSIG_LOG=1: every frame a system reads, to stderr (what it was, how it decoded) — the first-capture tool. */
inline bool logOn() { static const bool on = [] { const char* e = std::getenv("VIBE_DVSIG_LOG"); return e && *e && *e != '0'; }(); return on; }
#define DVLOG(...) do { if (::vibe::dvsig::logOn()) std::fprintf(stderr, __VA_ARGS__); } while (0)

// ── The legal rule's inputs ────────────────────────────────────────────────────────────────────────────────────────────
/** "" when `hz` is not inside an amateur allocation of `region`'s band plan, else the band's label ("70 cm amateur").
 *  Region unknown (0) = Region 1, the narrowest 2 m (144–146) and 70 cm (430–440): PMR446 is never amateur. */
inline std::string amateurBand(double hz, int region) {
    if (!(hz > 0)) return {};
    for (const auto& b : vibebands::namedBands(region > 0 ? region : 1))
        if (hz >= b.lo && hz <= b.hi && std::string(b.label).find("amateur") != std::string::npos) return b.label;
    return {};
}

// ── A transmission, as logged ─────────────────────────────────────────────────────────────────────────────────────────
struct Entry {
    uint64_t id = 0;
    int64_t t0 = 0, t1 = 0;            ///< ms since the epoch: first and last heard
    Sys sys = Sys::None;
    int slot = 0;                      ///< DMR 1/2, 0 = not known (an MS-sourced burst carries no slot number)
    int cc = -1;                       ///< DMR colour code · NXDN RAN · dPMR channel code (12 bits)
    std::string kind = "voice";        ///< voice · data · sms · position · preamble · header
    bool ids = false;                  ///< an address was decoded (src/dst or callsigns)
    bool group = false;                ///< DMR/NXDN: to a talkgroup (else a private / individual call)
    uint32_t src = 0, dst = 0;         ///< DMR (24 bit), NXDN (16 bit), dPMR (24 bit)
    std::string from, to, via1, via2;  ///< callsigns — D-STAR MY / UR / RPT1 / RPT2; YSF source / destination / down / up
    std::string alias;                 ///< DMR talker alias
    std::string text;                  ///< D-STAR slow-data message, DMR / YSF short data text
    std::string mode;                  ///< YSF "DN" / "VW" / "V/D 1" / "data"; D-STAR "via repeater" …
    int dg = -1;                       ///< YSF DG-ID
    bool hasGps = false; double lat = 0, lon = 0;
    bool enc = false;                  ///< marked encrypted — nothing else is said about it
    bool open = true;
    float ber = 0;                     ///< % of coded bits the FEC corrected (identity frames)
};

/** What a system hands the receiver. */
class Sink {
public:
    virtual ~Sink() = default;
    virtual int64_t nowMs() const = 0;
    /** A new (or the same, when `key` matches an open one) transmission; returns its id. `key` is the system's own
     *  handle for "this over" (e.g. DMR slot) so updates land on the right entry. */
    virtual Entry* entry(Sys s, int key, bool create) = 0;
    virtual void changed() = 0;
    virtual void close(Sys s, int key) = 0;
};

inline std::string cleanText(const uint8_t* p, size_t n, bool trimRight = true) {
    std::string s;
    for (size_t i = 0; i < n; ++i) { const uint8_t c = p[i]; if (c == 0) break; s += (c >= 0x20 && c < 0x7F) ? char(c) : ' '; }
    if (trimRight) { while (!s.empty() && s.back() == ' ') s.pop_back(); }
    size_t a = 0; while (a < s.size() && s[a] == ' ') ++a;
    return s.substr(a);
}
inline bool printableFrac(const std::string& s, double minFrac) {
    if (s.empty()) return false;
    size_t ok = 0; for (char c : s) if (std::isalnum(uint8_t(c)) || c == ' ' || c == '/' || c == '-' || c == '.') ++ok;
    return double(ok) / double(s.size()) >= minFrac;
}

// Frame reading shared by the systems: levelled symbols off a path at offsets from an anchor (the sync's last symbol).
struct FrameReader {
    const Ring& r;
    double anchor;
    Levels lv;
    float z(int k) const { return symZ(r, anchor, k, lv); }
    /** Soft dibits for symbols [k0, k0+n) into `soft` (2n). */
    void soft(int k0, int n, int8_t* soft) const { for (int i = 0; i < n; ++i) softDibit(z(k0 + i), soft + 2 * i); }
    void hard(int k0, int n, uint8_t* bits) const { for (int i = 0; i < n; ++i) hardDibit(z(k0 + i), bits + 2 * i); }
};
/** Fit the levels from a 4-FSK sync (symbol values `s`, ending at offset 0), then refine on the decided symbols of the
 *  frame [k0, k1] (M17's method: the payload's own symbols are many more than the sync's). */
inline bool fitFrame(const Ring& r, double anchor, const std::vector<int8_t>& s, int k0, int k1, Levels& lv, bool refine = true) {
    const int L = int(s.size());
    std::vector<int> ks(static_cast<size_t>(L));
    for (int i = 0; i < L; ++i) ks[size_t(i)] = i - (L - 1);
    if (!fitLevels(r, anchor, ks.data(), s.data(), L, lv)) return false;
    if (!refine) return true;
    for (int it = 0; it < 2; ++it) {
        double a = 0, b = 0, aa = 0, ab = 0; int n = 0;
        for (int k = k0; k <= k1; ++k) {
            const double y = r.interp(anchor + double(k) * Path::kSps);
            const double zz = (y - lv.dc) / lv.gain;
            const double ideal = zz >= 2 ? 3 : zz >= 0 ? 1 : zz >= -2 ? -1 : -3;
            a += ideal; b += y; aa += ideal * ideal; ab += ideal * y; ++n;
        }
        const double den = n * aa - a * a;
        if (!(den > 0)) break;
        const double g = (n * ab - a * b) / den;
        if (!(g > 1e-6)) break;
        lv.gain = g; lv.dc = (b - g * a) / n;
    }
    double e = 0; int n = 0;
    for (int k = k0; k <= k1; ++k) {
        const double zz = (r.interp(anchor + double(k) * Path::kSps) - lv.dc) / lv.gain;
        const double ideal = zz >= 2 ? 3 : zz >= 0 ? 1 : zz >= -2 ? -1 : -3;
        e += (zz - ideal) * (zz - ideal); ++n;
    }
    lv.sigma = float(std::sqrt(e / std::max(1, n)));
    return true;
}
/** Best |ρ| of `pat` within ±w samples of `c`; returns the refined (fractional) position, or −1. */
inline double refineSync(const Path& p, int pat, double c, int w, float minRho, float* rhoOut, int sign = 0) {
    const long ci = long(std::lround(c));
    float best = 0; long bp = ci;
    for (long q = ci - w; q <= ci + w; ++q) {
        const float r = p.rhoAt(pat, q);
        if (sign > 0 && r < 0) continue;
        if (sign < 0 && r > 0) continue;
        if (std::fabs(r) > std::fabs(best)) { best = r; bp = q; }
    }
    if (rhoOut) *rhoOut = best;
    if (std::fabs(best) < minRho) return -1;
    const float a0 = std::fabs(p.rhoAt(pat, bp - 1)), a1 = std::fabs(best), a2 = std::fabs(p.rhoAt(pat, bp + 1));
    const float den = a0 - 2 * a1 + a2;
    const double frac = std::fabs(den) < 1e-9f ? 0.0 : std::max(-0.5, std::min(0.5, double(0.5f * (a0 - a2) / den)));
    return double(bp) + frac;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// DMR — ETSI TS 102 361-1/-2. A 30 ms burst = CACH (12 symbols, BS outbound only) + 54 + 24 (SYNC or EMB+embedded) + 54.
//   Offsets from the sync's last symbol (0): CACH −89…−78 · info₁ −77…−29 · slot type −28…−24 · SYNC −23…0 ·
//   slot type 1…5 · info₂ 6…54. Voice bursts: EMB −23…−20 and −3…0, embedded signalling −19…−4.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class DmrSys {
public:
    DmrSys(Path& p, Sink& s) : p_(p), out_(s) {}
    void cand(int pat, double pos, float rho) {
        if (locked_) return;   // ★ while following a channel the timebase looks for the syncs itself
        if (pend_.size() >= 16) { auto w = std::min_element(pend_.begin(), pend_.end(), [](const Pend& a, const Pend& b) { return std::fabs(a.rho) < std::fabs(b.rho); }); if (std::fabs(w->rho) >= std::fabs(rho)) return; pend_.erase(w); }
        pend_.push_back({pat, pos, rho});
    }
    void service() {
        const long end = p_.ring.end();
        // search: candidate bursts whose samples have all arrived
        while (!locked_ && !pend_.empty()) {
            auto it = std::min_element(pend_.begin(), pend_.end(), [](const Pend& a, const Pend& b) { return a.pos < b.pos; });
            if (!(it->pos + 60.0 * Path::kSps < double(end))) break;
            const Pend c = *it; pend_.erase(it);
            trySearch(c);
        }
        while (locked_) {
            const double pos = anchor_ + double(kAbs_ - anchorK_) * 720.0;
            if (!(pos + 62.0 * Path::kSps < double(end))) break;
            track(pos);
            ++kAbs_;
        }
    }
    bool tracking() const { return locked_; }
    void reset() { endAll(); locked_ = false; pend_.clear(); }
    struct Stats { long bursts = 0, dataOk = 0, embOk = 0, lcOk = 0; } stats;

private:
    struct Pend { int pat; double pos; float rho; };
    struct Slot {
        bool voice = false; int vb = 0; int lcss = -1; int nfrag = 0; uint8_t frag[4][32] = {}; int bad = 0;
        int embGood = 0, embCc = -1;
        uint8_t ta[32] = {}; int taFmt = -1, taLen = 0; uint8_t taHave = 0;
        bool open = false;
        // data assembly
        int dataLeft = 0, dataDpf = 0, dataSap = 0, dataDd = -1, dataConf = 0; std::vector<uint8_t> data;
    };
    static int labelOfPat(int pat) { return pat == kDmrDm1 ? 1 : pat == kDmrDm2 ? 2 : 0; }
    int key(int si) const { return 1 + si; }

    /** Read a burst at `pos`; `pat`/`rho` say which sync (if any) is there. `si` = which of the two TDMA positions (0/1)
     *  it is — from the CACH on a repeater's downlink, else from its place in the 60 ms frame. Returns true when
     *  something checked out. */
    bool burst(double pos, int pat, float rho, bool haveSync, int si) {
        ++stats.bursts;
        FrameReader fr{p_.ring, pos, {}};
        // levels: from the sync if there is one; else from the last good levels (voice bursts carry no known symbols)
        if (haveSync) {
            const auto& ps = patsA()[size_t(pat)].s;
            std::vector<int8_t> s(ps.begin(), ps.end());
            if (rho < 0) for (auto& v : s) v = int8_t(-v);
            if (!fitFrame(p_.ring, pos, s, -77, 54, fr.lv, false)) return false;
        } else {
            if (!(lv_.gain > 0)) return false;
            fr.lv = lv_;
        }
        // CACH (BS outbound): the TACT's TC bit says the slot — TS 102 361-1 B.4.1 places AT, TC, LCSS and the Hamming(7,4)
        // parity at bits 0, 4, 8, 12, 14, 18, 22 of the 24
        if (bs_) {
            uint8_t cb[24]; fr.hard(-89, 12, cb);
            uint32_t cw = (uint32_t(cb[0]) << 6) | (uint32_t(cb[4]) << 5) | (uint32_t(cb[8]) << 4) | (uint32_t(cb[12]) << 3) |
                          (uint32_t(cb[14]) << 2) | (uint32_t(cb[18]) << 1) | uint32_t(cb[22]);
            if (h7_4().correct(cw) >= 0) { si = int((cw >> 5) & 1u); label_[si] = si + 1; }
        } else if (haveSync && labelOfPat(pat)) label_[si] = labelOfPat(pat);
        if (haveSync) lv_ = fr.lv;
        Slot& S = sl_[si];
        if (haveSync && rho > 0) {   // ── voice burst A (voice sync)
            S.voice = true; S.vb = 0; S.bad = 0;
            return true;
        }
        if (haveSync && rho < 0) return dataBurst(fr, si, S);
        // ── no sync: a voice burst B–F on a slot in voice (EMB + embedded signalling)
        if (!S.voice) return false;
        uint8_t eb[16], em[32];
        fr.hard(-23, 4, eb); fr.hard(-3, 4, eb + 8); fr.hard(-19, 16, em);
        uint16_t er = 0; for (int i = 0; i < 16; ++i) er = uint16_t((er << 1) | eb[i]);
        int qe = 0;
        const int emb = qr16Decode(er, &qe);
        S.vb++;
        if (emb < 0 || qe > 1) { if (++S.bad >= 6) endSlot(si); return false; }
        S.bad = 0;
        const int cc = (emb >> 3) & 0xF, lcss = emb & 3;
        ++stats.embOk;
        if (S.embCc == cc) ++S.embGood; else { S.embCc = cc; S.embGood = 1; }
        // ★ three EMBs that agree → a DMR voice call is on this slot, even before its addresses decode
        if (S.embGood >= 3 && !S.open) openVoice(si, cc);
        if (S.open) if (Entry* e = out_.entry(Sys::Dmr, key(si), false)) { e->t1 = out_.nowMs(); if (e->cc < 0) e->cc = cc; e->slot = label_[si]; }
        // embedded LC fragments: first (01), continuation (11) ×2, last (10)
        if (lcss == 1) { std::memcpy(S.frag[0], em, 32); S.nfrag = 1; }
        else if (lcss == 3 && S.nfrag >= 1 && S.nfrag <= 2) { std::memcpy(S.frag[S.nfrag], em, 32); S.nfrag++; }
        else if (lcss == 2 && S.nfrag == 3) {
            std::memcpy(S.frag[3], em, 32); S.nfrag = 0;
            uint8_t lc[9]; int fx = 0;
            if (embDecode(S.frag, lc, &fx)) { ++stats.lcOk; fullLc(lc, si, cc, false, float(fx) * 100.f / 128.f); }
        } else if (lcss != 0) S.nfrag = 0;
        return true;
    }

    bool dataBurst(const FrameReader& fr, int si, Slot& S) {
        const int slot = label_[si];
        int8_t st[20];
        for (int i = 0; i < 5; ++i) softDibit(fr.z(-28 + i), st + 2 * i);
        for (int i = 0; i < 5; ++i) softDibit(fr.z(1 + i), st + 10 + 2 * i);
        int gd = 0;
        const int slotType = golay20Decode(st, &gd);
        if (slotType < 0 || gd > 3) { DVLOG("dmr data burst @%.1f: slot type fails (golay %d dist %d) z %.2f %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f %.2f\n", fr.anchor, slotType, gd, fr.z(-28), fr.z(-27), fr.z(-26), fr.z(-25), fr.z(-24), fr.z(1), fr.z(2), fr.z(3), fr.z(4), fr.z(5)); return false; }
        const int cc = (slotType >> 4) & 0xF, dt = slotType & 0xF;
        uint8_t raw[196], info[96];
        fr.hard(-77, 49, raw); fr.hard(6, 49, raw + 98);
        int fx = 0;
        if (!bptc196Decode(raw, info, &fx)) { DVLOG("dmr data burst @%.1f: cc %d type %d, BPTC fails\n", fr.anchor, cc, dt); return false; }
        uint8_t b[12]; bitsToBytesMsb(info, 96, b);
        DVLOG("dmr data burst @%.1f si %d: cc %d type %d, golay %d, bptc %d fixes\n", fr.anchor, si, cc, dt, gd, fx);
        const float ber = float(fx + gd) * 100.f / 216.f;
        auto crcOk = [&](uint16_t mask) { const uint16_t c = uint16_t(crcCcittMsb(b, 10) ^ mask); return b[10] == uint8_t(c >> 8) && b[11] == uint8_t(c & 0xFF); };
        switch (dt) {
        case 1: case 2: {   // Voice LC header / Terminator with LC: RS(12,9) with the type's mask
            uint8_t p3[3]; rs129Parity(b, p3);
            const uint32_t m = dt == 1 ? kMaskVoiceHdr : kMaskTerm;
            if (b[9] != uint8_t(p3[0] ^ (m >> 16)) || b[10] != uint8_t(p3[1] ^ (m >> 8)) || b[11] != uint8_t(p3[2] ^ m)) return false;
            ++stats.dataOk;
            fullLc(b, si, cc, true, ber);
            if (dt == 2) { endSlot(si); }
            else { S.voice = true; S.vb = 5; S.bad = 0; }   // ★ the voice superframe follows the header
            return true;
        }
        case 3: {   // CSBK
            if (!crcOk(kMaskCsbk)) return false;
            ++stats.dataOk;
            const int op = b[0] & 0x3F, fid = b[1];
            if (op == 0x3D && fid == 0) {   // Preamble CSBK: who is about to send data (or a CSBK)
                Entry* e = out_.entry(Sys::Dmr, 10 + si, true);
                e->slot = slot; e->cc = cc; e->kind = (b[2] & 0x80) ? "data" : "preamble"; e->group = (b[2] & 0x40) != 0;
                e->dst = (uint32_t(b[4]) << 16) | (uint32_t(b[5]) << 8) | b[6]; e->src = (uint32_t(b[7]) << 16) | (uint32_t(b[8]) << 8) | b[9];
                e->ids = true; e->t1 = out_.nowMs(); e->ber = ber;
                out_.changed();
            }
            return true;
        }
        case 0:   // PI header: privacy on this slot's call
            if (!crcOk(kMaskPi)) return false;
            ++stats.dataOk;
            if (Entry* e = out_.entry(Sys::Dmr, key(si), false)) { e->enc = true; out_.changed(); }
            return true;
        case 6: {   // Data header
            if (!crcOk(kMaskDataHdr)) return false;
            ++stats.dataOk;
            const int dpf = b[0] & 0x0F;
            Entry* e = out_.entry(Sys::Dmr, 10 + si, true);
            e->slot = slot; e->cc = cc; e->kind = "data"; e->group = (b[0] & 0x80) != 0;
            e->dst = (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 8) | b[4]; e->src = (uint32_t(b[5]) << 16) | (uint32_t(b[6]) << 8) | b[7];
            e->ids = true; e->t1 = out_.nowMs(); e->ber = ber;
            S.dataDpf = dpf; S.dataSap = b[1] >> 4; S.data.clear(); S.dataDd = -1; S.dataConf = dpf == 3;
            if (dpf == 2 || dpf == 3) S.dataLeft = b[8] & 0x7F;
            else if (dpf == 13) { S.dataLeft = (((b[0] >> 4) & 3) << 4) | (b[1] & 0x0F); S.dataDd = b[8] >> 2; }
            else S.dataLeft = 0;
            out_.changed();
            return true;
        }
        case 7: {   // Rate ½ data block (no check of its own: the message CRC-32 at the end decides)
            if (S.dataLeft <= 0) return true;
            const int skip = S.dataConf ? 2 : 0;
            S.data.insert(S.data.end(), b + skip, b + 12);
            if (--S.dataLeft == 0) finishData(si, S);
            return true;
        }
        case 8: case 10:   // Rate ¾ / rate 1 data: counted, not decoded (see DV-SIGNALLING.md "Not built")
            if (S.dataLeft > 0) { S.dataLeft = 0; if (Entry* e = out_.entry(Sys::Dmr, 10 + si, false)) { e->text = dt == 8 ? "(rate \xc2\xbe data, not decoded)" : "(rate 1 data, not decoded)"; out_.changed(); } }
            return true;
        case 9:   // Idle: nothing to say, but it is DMR
            return true;
        default:
            return true;
        }
    }
    void finishData(int si, Slot& S) {
        Entry* e = out_.entry(Sys::Dmr, 10 + si, false);
        if (!e || S.data.size() < 6) return;
        // ★ the message CRC-32 (B.3.9): over the octets in 16-bit words least significant octet first; the four CRC octets
        //   least significant first. (The byte order of the stored CRC is our reading; the other order is accepted too.)
        const size_t n = S.data.size() - 4;
        std::vector<uint8_t> sw(n);
        for (size_t i = 0; i + 1 < n; i += 2) { sw[i] = S.data[i + 1]; sw[i + 1] = S.data[i]; }
        if (n & 1) sw[n - 1] = S.data[n - 1];
        std::vector<uint8_t> bits(n * 8); bytesToBitsMsb(sw.data(), int(n * 8), bits.data());
        const uint32_t c = crcBits(bits.data(), int(n * 8), 32, 0x04C11DB7u, 0);
        const uint8_t* q = S.data.data() + n;
        const bool le = q[0] == uint8_t(c) && q[1] == uint8_t(c >> 8) && q[2] == uint8_t(c >> 16) && q[3] == uint8_t(c >> 24);
        const bool be = q[3] == uint8_t(c) && q[2] == uint8_t(c >> 8) && q[1] == uint8_t(c >> 16) && q[0] == uint8_t(c >> 24);
        if (!le && !be) { e->text = "(data, CRC failed)"; out_.changed(); return; }
        if (S.dataDpf == 13) {   // Short Data: Defined — text in the formats we can show
            std::string t;
            const uint8_t* d = S.data.data(); const size_t m = n;
            if (S.dataDd == 2) { std::vector<uint8_t> bb(m * 8); bytesToBitsMsb(d, int(m * 8), bb.data()); for (size_t i = 0; i + 7 <= m * 8; i += 7) { const uint8_t ch = uint8_t(takeBits(bb.data(), int(i), 7)); if (ch) t += char(ch); } }
            else if (S.dataDd == 3 || S.dataDd == 18) t = std::string(reinterpret_cast<const char*>(d), m);
            else if (S.dataDd == 19 || S.dataDd == 20 || S.dataDd == 21) { for (size_t i = 0; i + 1 < m; i += 2) { const unsigned u = S.dataDd == 21 ? (d[i] | (d[i + 1] << 8)) : ((d[i] << 8) | d[i + 1]); if (u && u < 0x80) t += char(u); else if (u) t += '?'; } }
            e->kind = "sms"; e->text = cleanText(reinterpret_cast<const uint8_t*>(t.data()), t.size());
        } else if (S.dataSap == 4 && n >= 28 && (S.data[0] >> 4) == 4 && S.data[9] == 17) {   // IPv4 + UDP
            const unsigned dport = (S.data[22] << 8) | S.data[23];
            char b[64]; std::snprintf(b, sizeof b, "IP data to UDP port %u, %zu bytes", dport, n);
            e->text = b;
        } else {
            char b[48]; std::snprintf(b, sizeof b, "data, %zu bytes", n);
            e->text = b;
        }
        out_.changed();
    }
    void openVoice(int si, int cc) {
        Slot& S = sl_[si];
        Entry* e = out_.entry(Sys::Dmr, key(si), true);
        e->slot = label_[si]; e->cc = cc; e->kind = "voice"; e->t1 = out_.nowMs();
        S.open = true; S.taFmt = -1; S.taHave = 0; S.taLen = 0;
        out_.changed();
    }
    void fullLc(const uint8_t* lc, int si, int cc, bool fromHeader, float ber) {
        Slot& S = sl_[si];
        const int flco = lc[0] & 0x3F, fid = lc[1];
        if ((flco == 0 || flco == 3) && (fid == 0 || fid == 0x10)) {
            const uint32_t dst = (uint32_t(lc[3]) << 16) | (uint32_t(lc[4]) << 8) | lc[5], src = (uint32_t(lc[6]) << 16) | (uint32_t(lc[7]) << 8) | lc[8];
            Entry* e = out_.entry(Sys::Dmr, key(si), false);
            if (e && S.open && e->ids && (e->src != src || e->dst != dst)) { endSlot(si); e = nullptr; }
            if (!S.open) openVoice(si, cc);
            e = out_.entry(Sys::Dmr, key(si), false);
            if (!e) return;
            const bool was = e->ids;
            e->group = flco == 0; e->src = src; e->dst = dst; e->ids = true; e->cc = cc; e->t1 = out_.nowMs();
            if (lc[2] & 0x40) e->enc = true;
            if (fromHeader || !was) e->ber = ber;
            out_.changed();
        } else if (flco >= 4 && flco <= 7 && fid == 0) {   // talker alias header / blocks 1–3
            if (!S.open) return;
            if (flco == 4) { S.taFmt = (lc[2] >> 6) & 3; S.taLen = (lc[2] >> 1) & 0x1F; }
            const int blk = flco - 4;
            // ★ the 217 alias bits: the header's 49 (bit 0 of octet 2, then octets 3–8), then 56 per block
            uint8_t bits[56]; int nb = 0;
            if (blk == 0) { bits[nb++] = lc[2] & 1; for (int i = 3; i < 9; ++i) for (int k = 7; k >= 0; --k) bits[nb++] = (lc[i] >> k) & 1; }
            else { for (int i = 2; i < 9; ++i) for (int k = 7; k >= 0; --k) bits[nb++] = (lc[i] >> k) & 1; }
            const int off = blk == 0 ? 0 : 49 + 56 * (blk - 1);
            for (int i = 0; i < nb; ++i) { const int q = off + i; S.ta[q >> 3] = uint8_t((S.ta[q >> 3] & ~(0x80u >> (q & 7))) | (bits[i] ? (0x80u >> (q & 7)) : 0)); }
            S.taHave |= uint8_t(1u << blk);
            if (S.taFmt >= 0) {
                std::string a;
                uint8_t all[217]; bytesToBitsMsb(S.ta, 217, all);
                const int want = S.taLen ? S.taLen : 31;
                if (S.taFmt == 0) { for (int c = 0; c < want && 7 * c + 7 <= 217; ++c) { const int bitEnd = 7 * c + 7; const int need = bitEnd <= 49 ? 0 : 1 + (bitEnd - 49 - 1) / 56; if (!((S.taHave >> need) & 1)) break; a += char(takeBits(all, 7 * c, 7)); } }
                else {
                    // 8-bit / UTF-8 / UTF-16BE: whole octets from octet 3 of the header (the 49 bits' first is reserved)
                    std::vector<uint8_t> oc; for (int c = 0; c < 27; ++c) { const int bitStart = 1 + 8 * c; const int need = bitStart + 8 <= 49 ? 0 : 1 + (bitStart + 8 - 49 - 1) / 56; if (!((S.taHave >> need) & 1)) break; oc.push_back(uint8_t(takeBits(all, bitStart, 8))); }
                    if (S.taFmt == 3) { for (size_t i = 0; i + 1 < oc.size() && int(i / 2) < want; i += 2) { const unsigned u = (oc[i] << 8) | oc[i + 1]; a += (u >= 0x20 && u < 0x7F) ? char(u) : '?'; } }
                    else { for (size_t i = 0; i < oc.size() && int(i) < want; ++i) a += char(oc[i]); }
                }
                const std::string clean = cleanText(reinterpret_cast<const uint8_t*>(a.data()), a.size());
                if (Entry* e = out_.entry(Sys::Dmr, key(si), false)) if (!clean.empty() && clean != e->alias) { e->alias = clean; out_.changed(); }
            }
        } else if (flco == 8 && fid == 0) {   // GPS Info (TS 102 361-2 §7.1.1.3, 7.2.15–17)
            if (!S.open) return;
            const int perr = (lc[2] >> 1) & 7;
            int32_t lon = int32_t((uint32_t(lc[2] & 1) << 24) | (uint32_t(lc[3]) << 16) | (uint32_t(lc[4]) << 8) | lc[5]);
            if (lon & 0x1000000) lon -= 0x2000000;
            int32_t lat = int32_t((uint32_t(lc[6]) << 16) | (uint32_t(lc[7]) << 8) | lc[8]);
            if (lat & 0x800000) lat -= 0x1000000;
            if (perr == 7) return;   // ★ "position error not known" — the radio may not have a fix
            Entry* e = out_.entry(Sys::Dmr, key(si), false);
            if (!e) return;
            e->lat = double(lat) * 180.0 / 16777216.0; e->lon = double(lon) * 360.0 / 33554432.0; e->hasGps = true;
            out_.changed();
        }
    }
    void endSlot(int si) {
        Slot& S = sl_[si];
        S.voice = false; S.nfrag = 0; S.embGood = 0; S.embCc = -1; S.bad = 0;
        if (S.open) { out_.close(Sys::Dmr, key(si)); S.open = false; }
        std::memset(S.ta, 0, sizeof S.ta); S.taHave = 0; S.taFmt = -1;
    }
    void endAll() { endSlot(0); endSlot(1); out_.close(Sys::Dmr, 10); out_.close(Sys::Dmr, 11); }

    void trySearch(const Pend& c) {
        bs_ = c.pat == kDmrBs;
        label_[0] = labelOfPat(c.pat); label_[1] = label_[0] ? 3 - label_[0] : 0;
        if (c.rho < 0) {   // a data sync: must decode outright
            if (!burst(c.pos, c.pat, c.rho, true, 0)) return;
        } else {           // a voice sync: taken on trust for one superframe — the EMBs that follow must agree
            if (std::fabs(c.rho) < 0.85f) return;
            burst(c.pos, c.pat, c.rho, true, 0);
        }
        locked_ = true; anchor_ = c.pos; anchorK_ = 0; originK_ = 0; kAbs_ = 1; misses_ = 0; pat_ = c.pat;
    }
    void track(double pos) {
        // a sync here? (any DMR pattern — a repeater's downlink carries BS syncs, a direct-mode channel DM syncs)
        float best = 0; int bestPat = -1; double bestPos = -1;
        for (int q : {kDmrBs, kDmrMs, kDmrDm1, kDmrDm2}) {
            float r = 0; const double rp = refineSync(p_, q, pos, 3, 0.70f, &r);
            if (rp >= 0 && std::fabs(r) > std::fabs(best)) { best = r; bestPat = q; bestPos = rp; }
        }
        const int si = int((kAbs_ - originK_) & 1);
        bool ok;
        if (bestPat >= 0) {
            if (bestPat == kDmrBs) bs_ = true;
            ok = burst(bestPos, bestPat, best, true, si);
            if (ok) { anchor_ = bestPos; anchorK_ = kAbs_; }
        } else ok = burst(pos, pat_, 0.f, false, si);
        if (ok) misses_ = 0;
        else if (++misses_ >= 12) { endAll(); locked_ = false; }
    }

    Path& p_;
    Sink& out_;
    std::deque<Pend> pend_;
    bool locked_ = false, bs_ = false;
    double anchor_ = 0; long anchorK_ = 0, kAbs_ = 0, originK_ = 0;
    int misses_ = 0, pat_ = kDmrBs;
    int label_[2] = {0, 0};
    Levels lv_;
    Slot sl_[2];
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// YSF — a 100 ms frame of 480 symbols: sync 20 (−19…0) · FICH 100 (1…100) · 5 blocks of 72 symbols (101…460).
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class YsfSys {
public:
    YsfSys(Path& p, Sink& s) : p_(p), out_(s) {}
    void cand(int pat, double pos, float rho) {
        (void)pat;
        if (locked_ || rho < 0) return;
        if (pend_.size() >= 16) pend_.pop_front();
        pend_.push_back(pos);
    }
    void service() {
        const long end = p_.ring.end();
        while (!locked_ && !pend_.empty()) {
            if (!(pend_.front() + 462.0 * Path::kSps < double(end))) break;
            const double pos = pend_.front(); pend_.pop_front();
            if (frame(pos, true)) { locked_ = true; next_ = pos + 2400.0; misses_ = 0; }
        }
        while (locked_ && next_ + 462.0 * Path::kSps < double(end)) {
            float r = 0;
            const double rp = refineSync(p_, kYsf, next_, 3, 0.5f, &r, +1);
            const double pos = rp >= 0 ? rp : next_;
            if (frame(pos, rp >= 0)) misses_ = 0;
            else if (++misses_ >= 3) { finish(); locked_ = false; break; }
            next_ = pos + 2400.0;
        }
    }
    bool tracking() const { return locked_; }
    void reset() { finish(); locked_ = false; pend_.clear(); }
    long fichOk = 0;

private:
    bool frame(double pos, bool haveSync) {
        FrameReader fr{p_.ring, pos, {}};
        if (haveSync || !(lv_.gain > 0)) { if (!fitFrame(p_.ring, pos, patsA()[kYsf].s, 1, 460, fr.lv)) return false; }
        else fr.lv = lv_;
        int8_t fs[200]; fr.soft(1, 100, fs);
        uint8_t f[4]; int fe = 0;
        if (!ysfFichDecode(fs, f, &fe)) return false;
        ++fichOk;
        lv_ = fr.lv;
        const int fi = f[0] >> 6, cm = (f[0] >> 2) & 3, fn = (f[1] >> 3) & 7, ft = f[1] & 7, dt = f[2] & 3, dg = f[3] & 0x7F;
        (void)ft;
        if (fi == 3) return true;   // test frame
        good_++;
        // ★ an entry once the FICH has passed twice (a header passes once and opens it at once)
        Entry* e = out_.entry(Sys::Ysf, 0, fi == 0 || good_ >= 2);
        if (!e) return true;
        e->t1 = out_.nowMs(); e->dg = dg; e->group = cm != 3;
        e->mode = dt == 2 ? "DN" : dt == 3 ? "VW" : dt == 0 ? "V/D 1" : "data";
        e->kind = dt == 1 ? "data" : "voice";
        if (!e->ids) e->ber = float(fe) * 100.f / 200.f;
        auto block = [&](int b, int sym0, int nsym, int8_t* dst) { fr.soft(101 + 72 * b + sym0, nsym, dst); };
        if (fi == 0 || fi == 2) {   // header / terminator: CSD1 (dest, src) and CSD2 (downlink, uplink)
            int8_t c1[360], c2[360];
            for (int b = 0; b < 5; ++b) { block(b, 0, 36, c1 + 72 * b); block(b, 36, 36, c2 + 72 * b); }
            uint8_t o[22];
            if (ysfDeIlViterbi(c1, 22, 9, o) <= 60 && ysfCrcOk(o, 22)) { ysfWhiten(o, 20); setCall(e, o, 0); setCall(e, o + 10, 1); }
            if (ysfDeIlViterbi(c2, 22, 9, o) <= 60 && ysfCrcOk(o, 22)) { ysfWhiten(o, 20); setCall(e, o, 2); setCall(e, o + 10, 3); }
            out_.changed();
            if (fi == 2) { finish(); locked_ = false; }
            return true;
        }
        if (dt == 0) {   // V/D mode 1: a 9-byte DCH in each block
            int8_t c[360];
            for (int b = 0; b < 5; ++b) block(b, 0, 36, c + 72 * b);
            uint8_t o[22];
            if (ysfDeIlViterbi(c, 22, 9, o) <= 60 && ysfCrcOk(o, 22)) {
                ysfWhiten(o, 20);
                if (fn == 0) { setCall(e, o, 0); setCall(e, o + 10, 1); }
                else if (fn == 1) { setCall(e, o, 2); setCall(e, o + 10, 3); }
                else if (fn >= 3) { std::memcpy(gps_ + 20 * (fn - 3), o, 20); gpsHave_ |= uint8_t(1u << fn); gpsLen_ = 20 * (fn - 2); tryGps(e); }
                out_.changed();
            }
        } else if (dt == 2) {   // V/D mode 2 (DN): a 5-byte DCH in each block
            int8_t c[200];
            for (int b = 0; b < 5; ++b) block(b, 0, 20, c + 40 * b);
            uint8_t o[12];
            if (ysfDeIlViterbi(c, 12, 5, o) <= 40 && ysfCrcOk(o, 12)) {
                ysfWhiten(o, 10);
                if (fn <= 3) setCall(e, o, fn == 0 ? 1 : fn == 1 ? 0 : fn);
                else if (fn == 6 || fn == 7) { std::memcpy(gps_ + 10 * (fn - 6), o, 10); gpsHave_ |= uint8_t(1u << fn); gpsLen_ = 10 * (fn - 5); tryGps(e); }
                out_.changed();
            }
        }
        return true;
    }
    /** which: 0 = destination (shown "to"), 1 = source ("from"), 2 = downlink, 3 = uplink. */
    void setCall(Entry* e, const uint8_t* p, int which) {
        const std::string s = cleanText(p, 10);
        if (s.empty() || !printableFrac(s, 0.9)) return;
        if (which == 0) e->to = s; else if (which == 1) { e->from = s; e->ids = true; } else if (which == 2) e->via1 = s; else e->via2 = s;
        if (which == 0 && s != "ALL") e->ids = e->ids || !e->from.empty();
    }
    /** ★ The GPS carried in the DCH's data frames (FN 3–7 in V/D 1, FN 6–7 in V/D 2): a block ending 0x03 + an additive
     *  checksum, two type octets, a radio code, then the position in APRS Mic-E encoding (APRS101 ch. 10): six
     *  "destination" characters for latitude and three for longitude. The layout is from MMDVM's YSF gateway (read for
     *  facts); the Mic-E decoding is ours from APRS101. */
    void tryGps(Entry* e) {
        for (int i = std::min(gpsLen_, 99) - 1; i > 0; --i) {
            if (gps_[i] != 0x03) continue;
            unsigned sum = 0; for (int k = 0; k <= i; ++k) sum += gps_[k];
            if (i + 1 >= gpsLen_ || uint8_t(sum) != gps_[i + 1]) return;
            if (!((gps_[1] == 0x22 && gps_[2] == 0x62) || (gps_[1] == 0x47 && gps_[2] == 0x64)) || i < 14) return;
            double lat, lon;
            if (micE(gps_ + 5, gps_ + 11, &lat, &lon)) { e->lat = lat; e->lon = lon; e->hasGps = true; }
            return;
        }
    }
public:
    /** APRS Mic-E: `d` = the 6 destination characters, `i` = the information field's d+28, m+28, h+28. */
    static bool micE(const uint8_t* d, const uint8_t* i, double* lat, double* lon) {
        int dig[6]; bool hi[6];
        for (int k = 0; k < 6; ++k) {
            const uint8_t c = d[k];
            if (c >= '0' && c <= '9') { dig[k] = c - '0'; hi[k] = false; }
            else if (c >= 'A' && c <= 'J') { dig[k] = c - 'A'; hi[k] = true; }
            else if (c >= 'P' && c <= 'Y') { dig[k] = c - 'P'; hi[k] = true; }
            else if (c == 'K' || c == 'L' || c == 'Z') { dig[k] = 0; hi[k] = c != 'L'; }
            else return false;
        }
        const double la = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
        const bool north = hi[3], plus100 = hi[4], west = hi[5];
        int deg = int(i[0]) - 28; if (plus100) deg += 100;
        if (deg >= 180 && deg <= 189) deg -= 80; else if (deg >= 190 && deg <= 199) deg -= 190;
        int mn = int(i[1]) - 28; if (mn >= 60) mn -= 60;
        const int hu = int(i[2]) - 28;
        if (deg < 0 || deg > 179 || mn < 0 || mn > 59 || hu < 0 || hu > 99 || la > 90) return false;
        *lat = north ? la : -la;
        const double lo = deg + (mn + hu / 100.0) / 60.0;
        *lon = west ? -lo : lo;
        return true;
    }
private:
    void finish() { out_.close(Sys::Ysf, 0); good_ = 0; gpsHave_ = 0; gpsLen_ = 0; std::memset(gps_, 0, sizeof gps_); }
    Path& p_;
    Sink& out_;
    std::deque<double> pend_;
    bool locked_ = false;
    double next_ = 0;
    int misses_ = 0, good_ = 0;
    Levels lv_;
    uint8_t gps_[100] = {}; uint8_t gpsHave_ = 0; int gpsLen_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// D-STAR — GMSK 4800 bit/s, LSB first. Frame sync (its 24-bit pattern ends at 0) · header 660 bits (1…660) · then voice
// frames of 96 bits (72 voice, never read, + 24 data). The data field of frame j ends at 756 + 96j; frame 0 of every 21
// carries the data sync in it, the others pair up into 6-octet slow-data blocks.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class DstarSys {
public:
    DstarSys(Path& p, Sink& s) : p_(p), out_(s) {}
    void cand(int pat, double pos, float rho) {
        if (rho < 0) return;
        if (pat == kDsEnd) { if (state_ != Idle && pos > lastData_ - 200) endPending_ = pos; return; }
        if (state_ == Voice) return;
        if (pend_.size() >= 16) pend_.pop_front();
        pend_.push_back({pat, pos});
    }
    void service() {
        const long end = p_.ring.end();
        while (state_ != Voice && !pend_.empty()) {
            const P c = pend_.front();
            const double need = c.pat == kDsFs ? 662.0 : 2.0;
            if (!(c.pos + need * Path::kSps < double(end))) break;
            pend_.pop_front();
            if (c.pat == kDsFs) { if (header(c.pos)) { state_ = Voice; dataEnd0_ = c.pos + 756.0 * Path::kSps; frame_ = 0; misses_ = 0; } }
            else if (c.pat == kDsData) lateSync(c.pos);
        }
        while (state_ == Voice) {
            const double de = dataEnd0_ + double(frame_) * 96.0 * Path::kSps;
            if (!(de + 4.0 * Path::kSps < double(end))) break;
            if (endPending_ > 0 && endPending_ < de + 48.0 * Path::kSps) { finish(); break; }
            dataFrame(de);
            ++frame_;
            if (misses_ >= 2) { finish(); break; }
        }
        if (state_ == Voice) endPending_ = -1;
    }
    bool tracking() const { return state_ == Voice; }
    void reset() { finish(); pend_.clear(); }
    long headersOk = 0;

private:
    enum State { Idle, Voice };
    struct P { int pat; double pos; };
    static int8_t softBit(float z) { const float x = z * 64.f; return int8_t(std::max(-127.f, std::min(127.f, x))); }
    bool fit2(double pos, int pat, Levels& lv) {
        const auto& s = patsA()[size_t(pat)].s;
        const int L = int(s.size());
        std::vector<int> ks(static_cast<size_t>(L)); for (int i = 0; i < L; ++i) ks[size_t(i)] = i - (L - 1);
        return fitLevels(p_.ring, pos, ks.data(), s.data(), L, lv);
    }
    bool header(double pos) {
        Levels lv; if (!fit2(pos, kDsFs, lv)) return false;
        int8_t s[660];
        for (int i = 0; i < 660; ++i) s[i] = softBit(symZ(p_.ring, pos, 1 + i, lv));
        uint8_t h[41]; int errs = 0;
        if (!dstarHeaderDecode(s, h, &errs)) return false;
        ++headersOk;
        lv_ = lv;
        Entry* e = out_.entry(Sys::Dstar, 0, true);
        applyHeader(e, h);
        e->ber = float(errs) * 100.f / 660.f;
        out_.changed();
        return true;
    }
    void applyHeader(Entry* e, const uint8_t* h) {
        e->via2 = cleanText(h + 3, 8); e->via1 = cleanText(h + 11, 8); e->to = cleanText(h + 19, 8);
        const std::string my1 = cleanText(h + 27, 8), my2 = cleanText(h + 35, 4);
        e->from = my2.empty() ? my1 : my1 + "/" + my2;
        e->ids = !my1.empty();
        e->kind = (h[0] & 0x80) ? "data" : "voice";
        e->mode = (h[0] & 0x40) ? "via repeater" : "direct";
        if (h[0] & 0x08) e->mode += ", urgent";
        e->t1 = out_.nowMs();
    }
    /** A data sync without a header (a late join): believed when the next one arrives exactly 21 frames on. */
    void lateSync(double pos) {
        if (lateAt_ > 0 && std::fabs(pos - (lateAt_ + 21.0 * 96.0 * Path::kSps)) < 4.0) {
            state_ = Voice; dataEnd0_ = pos; frame_ = 0; misses_ = 0;
            Levels lv; if (fit2(pos, kDsData, lv)) lv_ = lv;
            Entry* e = out_.entry(Sys::Dstar, 0, true);
            e->kind = "voice"; e->t1 = out_.nowMs();
            out_.changed();
            lateAt_ = -1;
            return;
        }
        lateAt_ = pos;
    }
    void dataFrame(double de) {
        if (frame_ % 21 == 0) {
            float r = 0;
            const double rp = refineSync(p_, kDsData, de, 3, 0.5f, &r, +1);
            if (rp >= 0) { dataEnd0_ += rp - de; misses_ = 0; Levels lv; if (fit2(rp, kDsData, lv)) lv_ = lv; }
            else ++misses_;
            half_ = 0;
            hdrLen_ = 0;   // ★ a header repeated in slow data starts right after a data sync (STD7.0 §6.4)
            lastData_ = de;
            return;
        }
        lastData_ = de;
        // the 24 data bits end at `de`: symbols −23…0 from it
        uint8_t b[24];
        for (int i = 0; i < 24; ++i) b[i] = symZ(p_.ring, de, i - 23, lv_) > 0 ? 1 : 0;
        const Bits& pn = dstarPn();
        uint8_t oct[3] = {0, 0, 0};
        for (int i = 0; i < 24; ++i) if (b[i] ^ pn[size_t(i)]) oct[i >> 3] |= uint8_t(1u << (i & 7));
        if (half_ == 0) { std::memcpy(blk_, oct, 3); half_ = 1; return; }
        std::memcpy(blk_ + 3, oct, 3); half_ = 0;
        slowBlock();
    }
    void slowBlock() {
        Entry* e = out_.entry(Sys::Dstar, 0, false);
        if (!e) return;
        e->t1 = out_.nowMs();
        const uint8_t mh = blk_[0], type = mh >> 4, n = mh & 0x0F;
        if (type == 0x4 && n <= 3) {   // message: 4 blocks of 5 characters
            std::memcpy(msg_ + 5 * n, blk_ + 1, 5); msgHave_ |= uint8_t(1u << n);
            if (msgHave_ == 0x0F) { const std::string t = cleanText(msg_, 20); if (!t.empty() && t != e->text) { e->text = t; out_.changed(); } msgHave_ = 0; }
        } else if (type == 0x5 && n >= 1 && n <= 5) {   // header retransmitted (a late joiner's callsigns)
            for (int i = 0; i < n && hdrLen_ < 41; ++i) hdr_[hdrLen_++] = blk_[1 + i];
            if (hdrLen_ >= 41) {
                const uint16_t c = crcX25(hdr_, 39);
                if (hdr_[39] == uint8_t(c) && hdr_[40] == uint8_t(c >> 8) && !e->ids) { applyHeader(e, hdr_); out_.changed(); }
                hdrLen_ = 0;
            }
        } else if (type == 0x3 && n >= 1 && n <= 5) {   // simple data: D-PRS (APRS text) or NMEA
            for (int i = 0; i < n; ++i) {
                const char ch = char(blk_[1 + i]);
                if (ch == '\r' || ch == '\n') { line(e, gpsLine_); gpsLine_.clear(); }
                else if (gpsLine_.size() < 200) gpsLine_ += ch;
            }
        } else if (type != 0x6 || mh != 0x66) { hdrLen_ = type == 0x5 ? hdrLen_ : hdrLen_; }
    }
    /** One line of slow data: D-PRS ("$$CRCxxxx,CALL>AP…,DSTAR*:!DDMM.mmN/DDDMM.mmW…") or an NMEA RMC / GGA sentence. */
    void line(Entry* e, const std::string& s) {
        double la, lo;
        if (parseLine(s, &la, &lo)) { e->lat = la; e->lon = lo; e->hasGps = true; out_.changed(); }
    }
public:
    static bool parseLine(const std::string& s, double* la, double* lo) {
        auto dm = [](const std::string& v, int degDigits, double* out) {
            if (v.size() < size_t(degDigits + 2)) return false;
            for (int i = 0; i < degDigits + 2; ++i) if (!std::isdigit(uint8_t(v[size_t(i)]))) return false;
            const double d = std::atof(v.substr(0, size_t(degDigits)).c_str()), m = std::atof(v.substr(size_t(degDigits)).c_str());
            if (m >= 60) return false;
            *out = d + m / 60.0; return true;
        };
        if (s.rfind("$$CRC", 0) == 0) {
            const size_t comma = s.find(','), colon = s.find(':');
            if (comma == std::string::npos || colon == std::string::npos || colon + 1 >= s.size()) return false;
            // ★ the "$$CRC" value: CRC-16 X.25 over what follows the comma (JARL STD7.0 §6.8.1; our reading of Ap2)
            const std::string body = s.substr(comma + 1);
            const uint16_t c = crcX25(reinterpret_cast<const uint8_t*>(body.data()), body.size());
            char hex[8]; std::snprintf(hex, sizeof hex, "%04X", c);
            const bool crcOk = s.compare(5, 4, hex) == 0;
            std::string info = s.substr(colon + 1);
            size_t k = 0;
            if (info[0] == '!' || info[0] == '=') k = 1;
            else if (info[0] == '/' || info[0] == '@') k = 8;
            else return false;
            if (info.size() < k + 19) return false;
            const std::string lat = info.substr(k, 7), lon = info.substr(k + 9, 8);
            const char ns = info[k + 7], ew = info[k + 17];
            double a, b;
            if (!dm(lat, 2, &a) || !dm(lon, 3, &b) || (ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W')) return false;
            if (!crcOk && lat[4] != '.') return false;
            *la = ns == 'S' ? -a : a; *lo = ew == 'W' ? -b : b;
            return *la >= -90 && *la <= 90 && *lo >= -180 && *lo <= 180;
        }
        if (s.size() > 10 && s[0] == '$' && (s.compare(3, 3, "RMC") == 0 || s.compare(3, 3, "GGA") == 0)) {
            // NMEA: the checksum must match
            const size_t star = s.find('*');
            if (star == std::string::npos || star + 3 > s.size()) return false;
            uint8_t x = 0; for (size_t i = 1; i < star; ++i) x ^= uint8_t(s[i]);
            if (std::strtoul(s.substr(star + 1, 2).c_str(), nullptr, 16) != x) return false;
            std::vector<std::string> f; size_t a = 0;
            for (size_t i = 0; i <= star; ++i) if (i == star || s[i] == ',') { f.push_back(s.substr(a, i - a)); a = i + 1; }
            const bool rmc = s.compare(3, 3, "RMC") == 0;
            const size_t li = rmc ? 3 : 2;
            if (f.size() < li + 4) return false;
            if (rmc && f[2] != "A") return false;
            double a1, b1;
            if (!dm(f[li], 2, &a1) || !dm(f[li + 2], 3, &b1)) return false;
            *la = f[li + 1] == "S" ? -a1 : a1; *lo = f[li + 3] == "W" ? -b1 : b1;
            return true;
        }
        return false;
    }
private:
    void finish() {
        if (state_ != Idle || out_.entry(Sys::Dstar, 0, false)) out_.close(Sys::Dstar, 0);
        state_ = Idle; endPending_ = -1; half_ = 0; msgHave_ = 0; hdrLen_ = 0; gpsLine_.clear(); lateAt_ = -1;
    }
    Path& p_;
    Sink& out_;
    std::deque<P> pend_;
    State state_ = Idle;
    double dataEnd0_ = 0, endPending_ = -1, lateAt_ = -1, lastData_ = 0;
    long frame_ = 0;
    int misses_ = 0, half_ = 0;
    Levels lv_;
    uint8_t blk_[6] = {}, msg_[20] = {}, msgHave_ = 0, hdr_[41] = {};
    int hdrLen_ = 0;
    std::string gpsLine_;
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// NXDN — frames of 192 symbols (80 ms at 2400 Bd "NXDN48", 40 ms at 4800 Bd "NXDN96"): FSW 10 (−9…0) · LICH 8 (1…8) ·
// SACCH 30 (9…38) · 144 symbols of FACCH1 ×2 or voice (39…182). Everything after the FSW is scrambled: a PN9 bit per
// symbol inverts it.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class NxdnSys {
public:
    NxdnSys(Path& p, Sink& s, int pat, int key) : p_(p), out_(s), pat_(pat), key_(key) {}
    void cand(int pat, double pos, float rho) {
        if (pat != pat_ || locked_ || rho < 0) return;
        if (pend_.size() >= 16) pend_.pop_front();
        pend_.push_back(pos);
    }
    void service() {
        const long end = p_.ring.end();
        while (!locked_ && !pend_.empty()) {
            if (!(pend_.front() + 185.0 * Path::kSps < double(end))) break;
            const double pos = pend_.front(); pend_.pop_front();
            int ran = -1;
            const int r = frame(pos, &ran);
            if (r == 2) { lock(pos); }
            else if (r == 1) {   // a SACCH alone: believed when the next frame agrees
                if (prevAt_ > 0 && std::fabs(pos - (prevAt_ + 960.0)) < 4.0 && prevRan_ == ran) lock(pos);
                else { prevAt_ = pos; prevRan_ = ran; }
            }
        }
        while (locked_ && next_ + 185.0 * Path::kSps < double(end)) {
            float rr = 0;
            const double rp = refineSync(p_, pat_, next_, 3, 0.55f, &rr, +1);
            const double pos = rp >= 0 ? rp : next_;
            int ran = -1;
            if (frame(pos, &ran) > 0) misses_ = 0; else if (++misses_ >= 4) { finish(); break; }
            next_ = pos + 960.0;
        }
    }
    bool tracking() const { return locked_; }
    void reset() { finish(); pend_.clear(); }
    long sacchOk = 0, facchOk = 0;

private:
    void lock(double pos) {
        locked_ = true; next_ = pos + 960.0; misses_ = 0;
        Entry* e = out_.entry(Sys::Nxdn, key_, true);
        e->t1 = out_.nowMs(); if (e->cc < 0) e->cc = ran_;
        e->mode = pat_ == kNx48 ? "NXDN48" : "NXDN96";
        if (haveCall_) applyCall(e);
        out_.changed();
    }
    /** 0 = nothing, 1 = a SACCH passed, 2 = a FACCH1 passed. */
    int frame(double pos, int* ranOut) {
        FrameReader fr{p_.ring, pos, {}};
        if (!fitFrame(p_.ring, pos, patsB()[kNx48].s, 1, 182, fr.lv)) return 0;
        const Bits& pn = nxdnPn();
        auto zz = [&](int k) { const float z = fr.z(k); return pn[size_t(k + 9)] ? -z : z; };
        uint8_t lich = 0;
        for (int i = 0; i < 8; ++i) lich = uint8_t((lich << 1) | (zz(1 + i) < 0 ? 1 : 0));
        const int par = ((lich >> 7) ^ (lich >> 6) ^ (lich >> 5) ^ (lich >> 4)) & 1;
        if ((lich & 1) != par) return 0;
        const int rfct = lich >> 6, opt = (lich >> 2) & 3;
        int8_t sa[60];
        for (int i = 0; i < 30; ++i) softDibit(zz(9 + i), sa + 2 * i);
        uint8_t si[32];
        int res = 0;
        const int se = nxdnDecode(sa, 32, false, si);
        if (se <= 8 && nxdnCrc6(si, 26) == takeBits(si, 26, 6)) {
            ++sacchOk; res = 1;
            const int sr = int(takeBits(si, 0, 2)), ran = int(takeBits(si, 2, 6));
            *ranOut = ran; ran_ = ran;
            if (rfct == 1 || rfct == 3) {   // RTCH: the superframe's four 18-bit fragments make the first 72 bits of a message
                const int idx = 3 - sr;
                std::memcpy(l3s_ + 18 * idx, si + 8, 18); have_ |= uint8_t(1u << idx);
                if (have_ == 0x0F) { have_ = 0; message(l3s_, 72, false); }
            }
        }
        // FACCH1 halves (option = steal: 0 both, 1 first, 2 second … as LICH "steal" says)
        for (int h = 0; h < 2; ++h) {
            const bool steal = opt == 0 || (opt == 1 && h == 0) || (opt == 2 && h == 1);
            if (!steal) continue;
            int8_t fa[144];
            for (int i = 0; i < 72; ++i) softDibit(zz(39 + 72 * h + i), fa + 2 * i);
            uint8_t fi[92];
            const int fe = nxdnDecode(fa, 92, true, fi);
            if (fe <= 20 && nxdnCrc12(fi, 80) == takeBits(fi, 80, 12)) { ++facchOk; res = 2; message(fi, 80, true); }
        }
        return res;
    }
    void message(const uint8_t* b, int n, bool facch) {
        (void)facch; (void)n;
        uint8_t o[10]; bitsToBytesMsb(b, 72, o);
        const int type = o[0] & 0x3F;
        if (type == 0x01) {   // VCALL
            callSrc_ = (uint32_t(o[3]) << 8) | o[4]; callDst_ = (uint32_t(o[5]) << 8) | o[6];
            callGroup_ = (o[2] & 0x80) == 0; callEnc_ = (o[7] >> 6) != 0; haveCall_ = true;
            if (Entry* e = out_.entry(Sys::Nxdn, key_, false)) { applyCall(e); out_.changed(); }
        } else if (type == 0x08) {   // TX_REL
            finish();
        }
    }
    void applyCall(Entry* e) {
        if (e->ids && (e->src != callSrc_ || e->dst != callDst_)) { out_.close(Sys::Nxdn, key_); e = out_.entry(Sys::Nxdn, key_, true); e->mode = pat_ == kNx48 ? "NXDN48" : "NXDN96"; }
        e->src = callSrc_; e->dst = callDst_; e->group = callGroup_; e->enc = e->enc || callEnc_; e->ids = true; e->cc = ran_;
        e->t1 = out_.nowMs();
    }
    void finish() {
        if (locked_) out_.close(Sys::Nxdn, key_);
        locked_ = false; have_ = 0; haveCall_ = false; prevAt_ = -1;
    }
    Path& p_;
    Sink& out_;
    int pat_, key_;
    std::deque<double> pend_;
    bool locked_ = false;
    double next_ = 0, prevAt_ = -1;
    int misses_ = 0, prevRan_ = -2, ran_ = -1;
    uint8_t l3s_[72] = {}, have_ = 0;
    bool haveCall_ = false, callGroup_ = true, callEnc_ = false;
    uint32_t callSrc_ = 0, callDst_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// dPMR — 2400 Bd. Header: FS1 (−23…0) · HI0 60 symbols (1…60) · channel code 12 (61…72) · HI1 60 (73…132); the first
// payload frame's FS2 ends at 144. Payload frames of 192 symbols: [FS2 | CC] 12 · CCH 36 · TCH 144 (voice, never read).
// End frame: FS3 · END 36.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class DpmrSys {
public:
    DpmrSys(Path& p, Sink& s) : p_(p), out_(s) {}
    void cand(int pat, double pos, float rho) {
        if (rho < 0) return;
        if (pat == kPmFs3) { if (locked_) endAt_ = pos; return; }
        if (locked_) return;
        if (pat != kPmFs1 && pat != kPmFs2) return;
        if (pend_.size() >= 16) pend_.pop_front();
        pend_.push_back({pat, pos});
    }
    void service() {
        const long end = p_.ring.end();
        while (!locked_ && !pend_.empty()) {
            const P c = pend_.front();
            if (!(c.pos + 134.0 * Path::kSps < double(end))) break;
            pend_.pop_front();
            if (c.pat == kPmFs1) { if (header(c.pos)) { locked_ = true; next_ = c.pos + 144.0 * Path::kSps; frameNo_ = 0; misses_ = 0; } }
            else {
                if (cch(c.pos, nullptr)) {
                    if (prevAt_ > 0 && std::fabs(c.pos - (prevAt_ + 384.0 * Path::kSps)) < 4.0) { locked_ = true; next_ = c.pos + 192.0 * Path::kSps; frameNo_ = 1; misses_ = 0; open(); }
                    else prevAt_ = c.pos;
                }
            }
        }
        while (locked_ && next_ + 38.0 * Path::kSps < double(end)) {
            if (endAt_ > 0 && endAt_ < next_ + 4) { finish(); break; }
            double pos = next_;
            if ((frameNo_ & 1) == 0) { float r = 0; const double rp = refineSync(p_, kPmFs2, next_, 3, 0.5f, &r, +1); if (rp >= 0) pos = rp; }
            if (cch(pos, &frameNo_)) misses_ = 0; else if (++misses_ >= 4) { finish(); break; }
            ++frameNo_;
            next_ = pos + 192.0 * Path::kSps;
        }
    }
    bool tracking() const { return locked_; }
    void reset() { finish(); pend_.clear(); }
    long hdrOk = 0, cchOk = 0;

private:
    struct P { int pat; double pos; };
    static int ccBits(const FrameReader& fr, int k0) { int v = 0; for (int i = 0; i < 12; ++i) v = (v << 1) | (fr.z(k0 + i) < 0 ? 1 : 0); return v; }
    bool header(double pos) {
        FrameReader fr{p_.ring, pos, {}};
        if (!fitFrame(p_.ring, pos, patsB()[kPmFs1].s, 1, 132, fr.lv)) return false;
        for (int copy = 0; copy < 2; ++copy) {
            uint8_t hb[120], bits[80]; int fx = 0;
            fr.hard(copy == 0 ? 1 : 73, 60, hb);
            if (!dpmrBlockDecode(hb, 10, bits, &fx)) continue;
            if (dpmrCrc8(bits, 72) != takeBits(bits, 72, 8)) continue;
            ++hdrOk;
            lv_ = fr.lv;
            Entry* e = out_.entry(Sys::Dpmr, 0, true);
            const int ht = int(takeBits(bits, 0, 4)), m = int(takeBits(bits, 52, 3));
            e->dst = takeBits(bits, 4, 24); e->src = takeBits(bits, 28, 24); e->ids = true;
            e->kind = m <= 1 || m == 5 ? "voice" : "data";
            e->group = takeBits(bits, 57, 2) == 0;   // F: 00 = call ALL
            e->cc = ccBits(fr, 61);
            e->mode = ht == 0 ? "" : "header type " + std::to_string(ht);
            e->ber = float(fx) * 100.f / 120.f; e->t1 = out_.nowMs();
            out_.changed();
            return true;
        }
        return false;
    }
    /** A payload frame's CCH at `pos` (the end of its FS2 or channel code). */
    bool cch(double pos, long* frameNo) {
        FrameReader fr{p_.ring, pos, {}};
        if (!frameNo || (*frameNo & 1) == 0) { if (!fitFrame(p_.ring, pos, patsB()[kPmFs2].s, 1, 180, fr.lv)) return false; lv_ = fr.lv; }
        else { if (!(lv_.gain > 0)) return false; fr.lv = lv_; }
        uint8_t cb[72], bits[48];
        fr.hard(1, 36, cb);
        if (!dpmrBlockDecode(cb, 6, bits, nullptr)) return false;
        if (dpmrCrc7(bits, 41) != takeBits(bits, 41, 7)) return false;
        ++cchOk;
        const int fn = int(takeBits(bits, 0, 2));
        const uint32_t id = takeBits(bits, 2, 12);
        idPart_[fn] = id; idHave_ |= uint8_t(1u << fn);
        if (frameNo && (*frameNo & 1) == 1) cc_ = ccBits(fr, -11);
        if (Entry* e = out_.entry(Sys::Dpmr, 0, false)) {
            e->t1 = out_.nowMs();
            if ((idHave_ & 3) == 3 && !e->ids) e->dst = (idPart_[0] << 12) | idPart_[1];
            if ((idHave_ & 12) == 12 && !e->ids) { e->src = (idPart_[2] << 12) | idPart_[3]; e->ids = (idHave_ & 3) == 3; }
            if (cc_ >= 0 && e->cc < 0) e->cc = cc_;
            out_.changed();
        }
        return true;
    }
    void open() { Entry* e = out_.entry(Sys::Dpmr, 0, true); e->kind = "voice"; e->t1 = out_.nowMs(); out_.changed(); }
    void finish() { if (locked_) out_.close(Sys::Dpmr, 0); locked_ = false; idHave_ = 0; endAt_ = -1; prevAt_ = -1; cc_ = -1; }
    Path& p_;
    Sink& out_;
    std::deque<P> pend_;
    bool locked_ = false;
    double next_ = 0, endAt_ = -1, prevAt_ = -1;
    long frameNo_ = 0;
    int misses_ = 0, cc_ = -1;
    Levels lv_;
    uint32_t idPart_[4] = {}; uint8_t idHave_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The receiver: the front end, the five systems, AUTO or a pin, the log, the report.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class Receiver : public Sink {
public:
    static constexpr size_t kLogMax = 60;
    Receiver() : dmr_(fe_.pa, *this), ysf_(fe_.pa, *this), ds_(fe_.pa, *this), nx48_(fe_.pb, *this, kNx48, 0), nx96_(fe_.pa, *this, kNx96, 1), pm_(fe_.pb, *this) {
        t0_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    /** AUTO (Sys::None) or one system: the others' correlators are switched off. */
    void setPin(Sys s) {
        pin_ = s;
        auto on = [&](Sys x) { return s == Sys::None || s == x; };
        for (int q : {kDmrBs, kDmrMs, kDmrDm1, kDmrDm2}) fe_.pa.setActive(q, on(Sys::Dmr));
        fe_.pa.setActive(kYsf, on(Sys::Ysf));
        for (int q : {kDsFs, kDsData, kDsEnd}) fe_.pa.setActive(q, on(Sys::Dstar));
        fe_.pa.setActive(kNx96, on(Sys::Nxdn)); fe_.pb.setActive(kNx48, on(Sys::Nxdn));
        for (int q : {kPmFs1, kPmFs2, kPmFs3, kPmFs4}) fe_.pb.setActive(q, on(Sys::Dpmr));
        if (!on(Sys::Dmr)) dmr_.reset(); if (!on(Sys::Ysf)) ysf_.reset(); if (!on(Sys::Dstar)) ds_.reset();
        if (!on(Sys::Nxdn)) { nx48_.reset(); nx96_.reset(); } if (!on(Sys::Dpmr)) pm_.reset();
        changed();
    }
    Sys pin() const { return pin_; }
    void setKernel(Kern k) { fe_.pa.kern = k; fe_.pb.kern = k; }
    /** Test hook: time from the sample clock instead of the wall (deterministic logs). */
    void setClock(int64_t t0ms) { t0_ = t0ms; }
    void feedIq(const cf32* x, int n, double rate) { fe_.feedIq(x, n, rate); process(); }
    void feed24k(const cf32* x, int n) { fe_.feed24k(x, n); process(); }
    bool busy() const { return dmr_.tracking() || ysf_.tracking() || ds_.tracking() || nx48_.tracking() || nx96_.tracking() || pm_.tracking(); }
    const std::deque<Entry>& log() const { return log_; }
    uint64_t version() const { return ver_; }
    long candidates() const { return fe_.pa.candidates + fe_.pb.candidates; }
    struct Counts { long dmrBursts, dmrData, dmrEmb, dmrLc, ysfFich, dsHdr, nxSacch, nxFacch, pmHdr, pmCch; };
    Counts counts() const { return {dmr_.stats.bursts, dmr_.stats.dataOk, dmr_.stats.embOk, dmr_.stats.lcOk, ysf_.fichOk, ds_.headersOk,
                                    nx48_.sacchOk + nx96_.sacchOk, nx48_.facchOk + nx96_.facchOk, pm_.hdrOk, pm_.cchOk}; }

    /** The report. `ids` = the identities copy (callsigns, IDs, alias, text, GPS); otherwise only what was heard. */
    std::string json(bool ids, const std::string& band) const {
        std::string j = "{\"type\":\"dvsig\",\"state\":\"";
        j += busy() ? "sync" : "search";
        j += "\",\"pin\":\""; j += sysKey(pin_); j += "\",\"amateur\":"; j += band.empty() ? "false" : "true";
        j += ",\"band\":\""; j += band; j += "\",\"ids\":"; j += ids ? "true" : "false";
        if (!last_.empty()) { j += ",\"sys\":\""; j += last_; j += "\""; }
        j += ",\"log\":[";
        bool first = true;
        for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
            const Entry& e = *it;
            if (!first) j += ","; first = false;
            char b[160];
            std::snprintf(b, sizeof b, "{\"id\":%llu,\"t\":%lld,\"t1\":%lld,\"sys\":\"%s\",\"kind\":\"%s\",\"open\":%s,\"enc\":%s",
                          (unsigned long long)e.id, (long long)e.t0, (long long)e.t1, sysName(e.sys), e.kind.c_str(), e.open ? "true" : "false", e.enc ? "true" : "false");
            j += b;
            if (e.slot) { j += ",\"slot\":"; j += std::to_string(e.slot); }
            if (e.cc >= 0) { j += e.sys == Sys::Nxdn ? ",\"ran\":" : ",\"cc\":"; j += std::to_string(e.cc); }
            if (e.sys == Sys::Ysf || e.sys == Sys::Nxdn) { j += ",\"mode\":\""; j += esc(e.mode); j += "\""; }
            if (ids) {
                if (e.ids && (e.sys == Sys::Dmr || e.sys == Sys::Nxdn || e.sys == Sys::Dpmr)) {
                    j += ",\"src\":" + std::to_string(e.src) + ",\"dst\":" + std::to_string(e.dst) + ",\"grp\":" + (e.group ? "true" : "false");
                }
                if (!e.from.empty()) j += ",\"from\":\"" + esc(e.from) + "\"";
                if (!e.to.empty()) j += ",\"to\":\"" + esc(e.to) + "\"";
                if (!e.via1.empty()) j += ",\"via1\":\"" + esc(e.via1) + "\"";
                if (!e.via2.empty()) j += ",\"via2\":\"" + esc(e.via2) + "\"";
                if (!e.alias.empty()) j += ",\"alias\":\"" + esc(e.alias) + "\"";
                if (!e.text.empty()) j += ",\"text\":\"" + esc(e.text) + "\"";
                if (e.sys == Sys::Dstar && !e.mode.empty()) j += ",\"mode\":\"" + esc(e.mode) + "\"";
                if (e.dg >= 0) j += ",\"dg\":" + std::to_string(e.dg);
                if (e.hasGps) { char g[80]; std::snprintf(g, sizeof g, ",\"gps\":{\"lat\":%.5f,\"lon\":%.5f}", e.lat, e.lon); j += g; }
                if (e.ber > 0) { char g[24]; std::snprintf(g, sizeof g, ",\"ber\":%.1f", double(e.ber)); j += g; }
            }
            j += "}";
        }
        j += "]}";
        return j;
    }

    // ── Sink ──
    int64_t nowMs() const override { return t0_ + int64_t(double(fe_.pa.ring.end()) / 24.0); }
    Entry* entry(Sys s, int key, bool create) override {
        for (auto& o : open_) if (o.sys == s && o.key == key) for (auto& e : log_) if (e.id == o.id) return &e;
        if (!create) return nullptr;
        Entry e; e.id = ++nextId_; e.sys = s; e.t0 = e.t1 = nowMs();
        log_.push_back(e);
        while (log_.size() > kLogMax) {
            const uint64_t gone = log_.front().id;
            log_.pop_front();
            open_.erase(std::remove_if(open_.begin(), open_.end(), [&](const OpenKey& o) { return o.id == gone; }), open_.end());
        }
        open_.push_back({s, key, e.id});
        last_ = sysName(s);
        changed();
        return &log_.back();
    }
    void changed() override { ++ver_; }
    void close(Sys s, int key) override {
        for (auto it = open_.begin(); it != open_.end(); ++it) if (it->sys == s && it->key == key) {
            for (auto& e : log_) if (e.id == it->id) { e.open = false; e.t1 = nowMs(); }
            open_.erase(it); changed(); return;
        }
    }

private:
    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (uint8_t(c) < 0x20 || uint8_t(c) >= 0x7F) o += '?'; else o += c; }
        return o;
    }
    void process() {
        auto ca = [this](int pat, double pos, float rho) {
            if (pat <= kDmrDm2) dmr_.cand(pat, pos, rho);
            else if (pat == kYsf) ysf_.cand(pat, pos, rho);
            else if (pat == kDsFs || pat == kDsData || pat == kDsEnd) ds_.cand(pat, pos, rho);
            else if (pat == kNx96) nx96_.cand(pat, pos, rho);
        };
        auto cb = [this](int pat, double pos, float rho) {
            if (pat <= kPmFs4) pm_.cand(pat, pos, rho);
            else if (pat == kNx48) nx48_.cand(pat, pos, rho);
        };
        fe_.pa.run(ca); fe_.pb.run(cb);
        dmr_.service(); ysf_.service(); ds_.service(); nx96_.service(); nx48_.service(); pm_.service();
        fe_.trim();
    }
    struct OpenKey { Sys sys; int key; uint64_t id; };
    Front fe_;
    DmrSys dmr_;
    YsfSys ysf_;
    DstarSys ds_;
    NxdnSys nx48_, nx96_;
    DpmrSys pm_;
    Sys pin_ = Sys::None;
    std::deque<Entry> log_;
    std::vector<OpenKey> open_;
    uint64_t nextId_ = 0, ver_ = 1;
    int64_t t0_ = 0;
    std::string last_;
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// One listener's receiver on its own thread (M17's pattern; decoder priority — Network > Audio > Spectrum > Decoders).
// The pipeline thread only COPIES its IQ into a bounded queue (2 s, the oldest dropped and counted).
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
class Listener {
public:
    explicit Listener(Sys pin = Sys::None) : pin_(pin) { th_ = std::thread([this] { loop(); }); }
    ~Listener() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    void feedIq(const cf32* x, int n, double rateHz) {
        if (n <= 0 || !(rateHz > 0)) return;
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(Block{std::vector<cf32>(x, x + n), rateHz});
        queued_ += size_t(n);
        while (queued_ > size_t(rateHz * 2.0) && q_.size() > 1) { queued_ -= q_.front().iq.size(); q_.pop_front(); ++dropped_; }
        cv_.notify_one();
    }
    void setPin(Sys s) { std::lock_guard<std::mutex> lk(m_); pinWant_ = int(s); cv_.notify_one(); }
    /** The two copies of the report (identities / public) for `band` (empty = not amateur), and the report's version. */
    void status(const std::string& band, std::string* idsCopy, std::string* publicCopy, uint64_t* ver) const {
        std::lock_guard<std::mutex> lk(sm_);
        if (band != band_) { band_ = band; ++statusVer_; dirty_ = true; }
        if (idsCopy) *idsCopy = ids_.empty() ? std::string("{\"type\":\"dvsig\",\"state\":\"search\",\"log\":[]}") : rewriteBand(ids_, band);
        // ★★ V12 beta90 DMR ID → CALLSIGN (vibe_dmr_id.h): only the identities copy, only on an amateur band (the rule
        //    above), only with the owner's switch on — each DMR entry's source (and a private call's destination) gains
        //    "srcCall" / "dstCall" once RadioID has answered; an answer landing later bumps the version so the box redraws.
        if (idsCopy && !band.empty() && !ids_.empty()) *idsCopy = vibe::dmrid::Lookup::shared().annotate(*idsCopy);
        if (publicCopy) *publicCopy = pub_.empty() ? std::string("{\"type\":\"dvsig\",\"state\":\"search\",\"log\":[]}") : rewriteBand(pub_, band);
        if (ver) *ver = statusVer_ + (band.empty() ? 0 : vibe::dmrid::Lookup::shared().generation());
    }
    bool busy() const { return busy_.load(std::memory_order_relaxed); }
    uint64_t droppedBlocks() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    size_t queuedFrames() const { std::lock_guard<std::mutex> lk(m_); return queued_ + (working_ ? 1 : 0); }

private:
    struct Block { std::vector<cf32> iq; double rate = 0; };
    /** The receiver writes "band":"" and "amateur":false; the caller's band goes in here (the JSON is ours, fixed form). */
    static std::string rewriteBand(const std::string& j, const std::string& band) {
        std::string o = j;
        const std::string a = "\"amateur\":false,\"band\":\"\"";
        const size_t p = o.find(a);
        if (p != std::string::npos) o.replace(p, a.size(), std::string("\"amateur\":") + (band.empty() ? "false" : "true") + ",\"band\":\"" + band + "\"");
        return o;
    }
    void loop() {
        vibeDecoderThread("vibe-dvsig");
        Receiver rx;
        rx.setPin(pin_);
        uint64_t seen = 0;
        for (;;) {
            Block b;
            int pin = -1;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty() || pinWant_ >= 0; });
                if (stop_) break;
                if (pinWant_ >= 0) { pin = pinWant_; pinWant_ = -1; }
                if (!q_.empty()) { b = std::move(q_.front()); q_.pop_front(); queued_ -= b.iq.size(); }
                working_ = true;
            }
            if (pin >= 0) rx.setPin(Sys(pin));
            if (!b.iq.empty()) rx.feedIq(b.iq.data(), int(b.iq.size()), b.rate);
            busy_.store(rx.busy(), std::memory_order_relaxed);
            if (rx.version() != seen) {
                seen = rx.version();
                const std::string i = rx.json(true, ""), p = rx.json(false, "");
                std::lock_guard<std::mutex> lk(sm_);
                ids_ = i; pub_ = p; ++statusVer_;
            }
            { std::lock_guard<std::mutex> lk(m_); working_ = false; }
        }
    }
    Sys pin_;
    std::atomic<bool> busy_{false};
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> q_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0;
    bool stop_ = false, working_ = false;
    int pinWant_ = -1;
    mutable std::mutex sm_;
    std::string ids_, pub_;
    mutable std::string band_;
    mutable uint64_t statusVer_ = 1;
    mutable bool dirty_ = false;
    std::thread th_;
};

}  // namespace dvsig
}  // namespace vibe
