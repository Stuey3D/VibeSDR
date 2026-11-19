// vibe_pktlink.h — V12: what sits on one slicer's bit stream for packet radio — the G3RUH descrambler, the AX.25 HDLC
// deframer, FX.25 and IL2P side by side — and the merger that keeps one copy of a frame several slicers found.
// docs/v12/AX25-9600-FX25-IL2P.md.
//
// ★★ ONE STREAM OF LINE BITS FEEDS THREE RECEIVERS, each where its standard puts it:
//      line bit ─┬─────────────────────────────────────────────► IL2P (no NRZI; for 9600 BEFORE the G3RUH descrambler)
//                └─ [G3RUH descrambler 1+x^12+x^17, 9600 only] ─┬─► HDLC deframer (does NRZI itself, vibe_hdlc.h)
//                                                              └─ NRZI decode ─► FX.25 correlation tag + codeblock
//    The G3RUH descrambler is the self-synchronising one of James Miller's 9600 modem (G3RUH, "9600 baud packet radio
//    modem design", AMSAT): out[n] = in[n] ⊕ in[n−12] ⊕ in[n−17]. It needs 17 bits to settle and then follows any
//    transmission, so nothing has to find its phase.
// ★★ A SCRAMBLED STREAM'S ONE WRONG LINE BIT IS THREE WRONG LEVELS (the descrambler's three taps), six wrong data bits:
//    the one-level repair of vibe_hdlc.h cannot mend that, so it is off at 9600. FX.25 and IL2P are the repair there.
// ★ Polarity: AX.25/FX.25 do not care (NRZI; the descrambler turns an inverted stream into an inverted one). IL2P looks
//   for its sync word both ways round (vibe_il2p.h).
#pragma once
#include "vibe_fx25.h"
#include "vibe_hdlc.h"
#include "vibe_il2p.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace vibepkt {

enum Fec { kAx25 = 0, kFx25 = 1, kIl2p = 2 };
inline const char* fecName(int f) { return f == kFx25 ? "fx25" : f == kIl2p ? "il2p" : "ax25"; }

struct PktFrame {
    std::vector<uint8_t> bytes;   ///< the AX.25 frame WITHOUT its FCS
    int fec = kAx25;              ///< which receiver produced it
    int fixed = 0;                ///< bytes the RS code corrected (FX.25 / IL2P)
    int repaired = 0;             ///< AX.25: one level flipped (vibe_hdlc.h repair)
    bool crc = false;             ///< IL2P: the trailing CRC was present and matched
    float quality = 0;
    int slicer = 0;
};

struct LinkOpts {
    bool fx25 = false, il2p = false;
    bool g3ruh = false;                  ///< 9600: descramble before the HDLC deframer and FX.25
    vibehdlc::DeframerOptions hdlc;      ///< the AX.25 deframer's own options
};

/** The G3RUH self-synchronising descrambler: one line bit in, one descrambled bit out. */
struct Descrambler {
    uint32_t sr = 0;
    int bit(int in) { const int o = (in ^ int(sr >> 11) ^ int(sr >> 16)) & 1; sr = ((sr << 1) | uint32_t(in & 1)) & 0x1FFFF; return o; }
};
/** …and the transmit side for the test generators: s[n] = d[n] ⊕ s[n−12] ⊕ s[n−17]. */
struct Scrambler {
    uint32_t sr = 0;
    int bit(int d) { const int s = (d ^ int(sr >> 11) ^ int(sr >> 16)) & 1; sr = ((sr << 1) | uint32_t(s)) & 0x1FFFF; return s; }
};

/** One slicer's receivers. Push one soft LINE level per bit (sign = the bit, |v| = confidence). */
class LinkRx {
public:
    explicit LinkRx(const LinkOpts& o) : o_(o), df_(o.hdlc) {
        if (o_.fx25) fx_.reset(new vibefx25::Rx());
        if (o_.il2p) il_.reset(new vibeil2p::Rx());
    }
    template <class Out> void push(float soft, Out&& out) {
        const int raw = soft >= 0 ? 1 : 0;
        if (il_) il_->bit(raw, [&](const vibeil2p::Result& r) {
            PktFrame f; f.bytes = r.bytes; f.fec = kIl2p; f.fixed = r.fixed; f.crc = r.crc; f.quality = q_;
            out(f);
        });
        int lvl = raw;
        if (o_.g3ruh) lvl = ds_.bit(raw);
        const float s = lvl ? std::fabs(soft) : -std::fabs(soft);
        q_ += 0.02f * (std::fabs(soft) - q_);
        df_.push(s, [&](const vibehdlc::Frame& fr) {
            PktFrame f; f.bytes = fr.bytes; f.repaired = fr.repaired; f.quality = fr.quality;
            out(f);
        });
        if (fx_) {
            const int dbit = lvl == prev_ ? 1 : 0;   // NRZI: no change = 1
            fx_->bit(dbit, [&](const vibefx25::Result& r) {
                PktFrame f; f.bytes = r.bytes; f.fec = kFx25; f.fixed = r.fixed; f.quality = q_;
                out(f);
            });
        }
        prev_ = lvl;
    }
    /** Inside something (an HDLC frame, an FX.25 codeblock, an IL2P packet): the bit clock may hold firmer. */
    bool inFrame() const { return df_.inFrame() || (fx_ && fx_->busy()) || (il_ && il_->busy()); }
    bool hdlcInFrame() const { return df_.inFrame(); }
    const vibehdlc::Deframer& hdlc() const { return df_; }
    const vibefx25::Rx* fx25() const { return fx_.get(); }
    const vibeil2p::Rx* il2p() const { return il_.get(); }
private:
    LinkOpts o_;
    vibehdlc::Deframer df_;
    std::unique_ptr<vibefx25::Rx> fx_;
    std::unique_ptr<vibeil2p::Rx> il_;
    Descrambler ds_;
    int prev_ = 0;
    float q_ = 0;
};

/** ★ The same frame from several slicers (and from the plain deframer AND FX.25) is one frame: held `holdBits` bits
 *  for the cleanest copy (FEC-free first, then unrepaired, then fewest fixes, then the best eye), then passed on once;
 *  any copy within `recentBits` after that only counts. FX.25's own copy of a frame a slicer already passed is
 *  normal (its check bytes arrive after the frame's end flag) — counted as `fecDupes`. */
class Merger {
public:
    Merger(double bitsPerSecond, double holdBits = 3, double recentSec = 0.6) : hold_(uint64_t(holdBits) + 1), recent_(uint64_t(bitsPerSecond * recentSec)) {}
    /** `t` = the current time in bits (any monotonic counter of bits). */
    template <class Out> void offer(const PktFrame& f, uint64_t t, Out&& out) {
        uint32_t h = 2166136261u;
        for (uint8_t c : f.bytes) h = (h ^ c) * 16777619u;
        for (auto& r : rec_) if (r.h == h && t - r.t < recent_) { if (f.fec != kAx25) ++fecDupes_; ++dupes_; return; }
        if (has_ && ph_ == h) { if (better_(f, p_)) p_ = f; return; }
        if (has_) flush_(t, out);
        p_ = f; ph_ = h; has_ = true; due_ = t + hold_;
    }
    template <class Out> void tick(uint64_t t, Out&& out) { if (has_ && t >= due_) flush_(t, out); }
    uint64_t dupes() const { return dupes_; }
    uint64_t fecDupes() const { return fecDupes_; }
private:
    static int rank_(const PktFrame& f) { return f.fec == kAx25 ? (f.repaired ? 1 : 0) : 2; }
    static bool better_(const PktFrame& a, const PktFrame& b) {
        if (rank_(a) != rank_(b)) return rank_(a) < rank_(b);
        if (a.fixed != b.fixed) return a.fixed < b.fixed;
        return a.quality > b.quality;
    }
    template <class Out> void flush_(uint64_t t, Out& out) {
        has_ = false;
        rec_[rp_] = { ph_, t }; rp_ = (rp_ + 1) % kRecent;
        out(p_);
    }
    struct Rec { uint32_t h = 0; uint64_t t = 0; };
    static constexpr int kRecent = 16;
    Rec rec_[kRecent];
    int rp_ = 0;
    uint64_t hold_, recent_;
    PktFrame p_; uint32_t ph_ = 0; bool has_ = false; uint64_t due_ = 0;
    uint64_t dupes_ = 0, fecDupes_ = 0;
};

} // namespace vibepkt
