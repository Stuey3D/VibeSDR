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
#include "vibe_pktlink.h"   // ★ V12 FX.25 / IL2P on the same slicers (docs/v12/AX25-9600-FX25-IL2P.md)
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"

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
/** ★ V12 (docs/v12/AX25-9600-FX25-IL2P.md): ANY AX.25 frame — I, S, U, UI with any PID — for the frame log (cubesat
 *  telemetry, connected-mode traffic, NET/ROM). The addresses are checked as strictly as parseAx25; `hasPid` says
 *  whether the frame carries a PID (I and UI frames); `info` is what follows (empty for S frames and most U frames). */
inline bool parseAx25Frame(const std::vector<uint8_t>& b, Ax25& f, bool& hasPid) {
    if (b.size() < 15) return false;
    size_t p = 0;
    std::vector<Addr> as;
    for (;;) {
        if (p + 7 > b.size() || as.size() >= 10) return false;
        Addr a;
        // ★ a satellite's "callsign" may hold a space inside it ("TRON 4" — a SatNOGS observation, 2026-11-19): every
        //   character is still a capital, digit or space, the first is not a space, trailing spaces are padding
        for (int i = 0; i < 6; ++i) {
            if (!addrByteOk(b[p + size_t(i)])) return false;
            const char c = char(b[p + size_t(i)] >> 1);
            if (i == 0 && c == ' ') return false;
            a.call += c;
        }
        while (!a.call.empty() && a.call.back() == ' ') a.call.pop_back();
        const uint8_t s = b[p + 6];
        a.ssid = (s >> 1) & 0x0F;
        a.h = (s & 0x80) != 0;
        as.push_back(a);
        p += 7;
        if (s & 1) break;
    }
    if (as.size() < 2 || p + 1 > b.size()) return false;
    f.dest = as[0]; f.src = as[1];
    f.digis.assign(as.begin() + 2, as.end());
    f.control = b[p];
    const bool iFrame = (f.control & 1) == 0, uiFrame = (f.control & ~0x10) == 0x03;
    hasPid = iFrame || uiFrame;
    if (hasPid) { if (p + 2 > b.size()) return false; f.pid = b[p + 1]; f.info.assign(b.begin() + long(p) + 2, b.end()); }
    else { f.pid = 0; f.info.assign(b.begin() + long(p) + 1, b.end()); }
    return true;
}
inline bool ax25FrameOk(const std::vector<uint8_t>& b) { Ax25 f; bool hp = false; return parseAx25Frame(b, f, hp); }
/** The control field in words, for the log: "UI", "I 3/5", "RR 2", "SABM", … (AX.25 v2.2 §4.3). */
inline std::string controlName(uint8_t c) {
    if ((c & 1) == 0) return "I " + std::to_string((c >> 1) & 7) + "/" + std::to_string(c >> 5);
    if ((c & 3) == 1) { static const char* n[4] = { "RR", "RNR", "REJ", "SREJ" }; return std::string(n[(c >> 2) & 3]) + " " + std::to_string(c >> 5); }
    switch (c & ~0x10) { case 0x03: return "UI"; case 0x2F: return "SABM"; case 0x6F: return "SABME"; case 0x43: return "DISC"; case 0x0F: return "DM";
                         case 0x63: return "UA"; case 0x87: return "FRMR"; case 0xAF: return "XID"; case 0xE3: return "TEST"; }
    return "U";
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
struct Frame { std::vector<uint8_t> bytes; int repaired = 0; float quality = 0; int slicer = 0;
               float votes = 0;   ///< ★ 2026-11-24 how many slicers / paths heard exactly these bytes (resolveCandidates)
               int fec = 0; int fixed = 0; bool crc = false; };   ///< ★ V12: fec = vibepkt::Fec (AX.25 / FX.25 / IL2P)

inline bool aprsFrameOk(const std::vector<uint8_t>& b) { Ax25 f; return parseAx25(b, f); }

inline uint32_t frameHash(const std::vector<uint8_t>& b) { uint32_t h = 2166136261u; for (uint8_t c : b) h = (h ^ c) * 16777619u; return h; }
/** ★ Rank of a copy: a clean AX.25 copy first, then a repaired one, then an FEC copy (fewest fixes first). */
inline int frameRank(const Frame& f) { return f.fec ? 2 : f.repaired ? 1 : 0; }
inline bool frameBetter(const Frame& a, const Frame& b) {
    return frameRank(a) < frameRank(b) || (frameRank(a) == frameRank(b) && (a.fixed < b.fixed || (a.fixed == b.fixed && a.quality > b.quality)));
}
/** One copy into a list of candidates heard together: the same bytes again add a vote and keep the better copy's
 *  details. (Weighting repairs lower, or letting a lone repair out only when "sure" — the flipped level ≤ 0.3 of the mean,
 *  ≤ 2 weak levels — was measured and dropped: test-aprs-onair --curve 100 7 10 ab, 2 400 packets × 3 runs, it lost the
 *  audio path 123 packets AND let one wrong frame through that the plain vote stops: a chance-pass CLEAN copy that a
 *  correct repair from another slicer outvotes.) */
inline void offerCandidate(std::vector<Frame>& c, Frame f) {
    if (f.votes <= 0) f.votes = 1.f;
    for (Frame& x : c) if (x.bytes == f.bytes) { const float v = x.votes + f.votes; if (frameBetter(f, x)) x = f; x.votes = v; return; }
    c.push_back(f);
}
/* ★★★ 2026-11-24 COPIES THAT DISAGREE (docs/v12/APRS-WEAK.md). Every slicer, filter and path decodes the same transmission,
 *  so several copies of one packet arrive together. Now and then one of them passes the FCS ON THE WRONG BITS — 16 bits
 *  of FCS against many slicers' tries: test-aprs-onair --curve found two such frames in 100 packets at 9 dB with the
 *  shipping decoder ("WIDE28-1 … 854.Y8W", once clean and once repaired, from the slicers either side of the right ones)
 *  — and it arrives BESIDE the right copy, a few bytes different. Two candidates of the same length that differ in ≤ 8
 *  bytes are one transmission, and the copies VOTE: the variant most slicers / paths heard wins if it has at least
 *  1.5 × the votes of any other; otherwise nothing of that transmission is trusted. Returns how many were dropped. */
inline int resolveCandidates(std::vector<Frame>& c) {
    const size_t n = c.size();
    if (n < 2) return 0;
    std::vector<int> grp(n); for (size_t i = 0; i < n; ++i) grp[i] = int(i);
    auto root = [&](int i) { while (grp[size_t(i)] != i) i = grp[size_t(i)] = grp[size_t(grp[size_t(i)])]; return i; };
    for (size_t i = 0; i < n; ++i) for (size_t j = i + 1; j < n; ++j) {
        if (c[i].bytes.size() != c[j].bytes.size()) continue;
        int d = 0; for (size_t k = 0; k < c[i].bytes.size() && d <= 8; ++k) d += c[i].bytes[k] != c[j].bytes[k];
        if (d <= 8) grp[size_t(root(int(i)))] = root(int(j));
    }
    std::vector<Frame> keep; int dropped = 0;
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const int r = root(int(i));
        if (done[size_t(r)]) continue;
        done[size_t(r)] = 1;
        int best = -1, members = 0; float second = 0;
        for (size_t j = 0; j < n; ++j) if (root(int(j)) == r) {
            ++members;
            if (best < 0 || c[j].votes > c[size_t(best)].votes) { if (best >= 0) second = std::max(second, c[size_t(best)].votes); best = int(j); }
            else second = std::max(second, c[j].votes);
        }
        if (members > 1 && c[size_t(best)].votes < 1.5f * second) { dropped += members; continue; }
        keep.push_back(c[size_t(best)]); dropped += members - 1;
    }
    c.swap(keep);
    return dropped;
}

class AfskDemod {
public:
    /** `fsIn` the audio rate (48000 from the decoder host); `baud` 1200 (Bell 202) or 300 (HF). `single`: one slicer
     *  only (balanced, on tune) — the baseline test-aprs measures the extra slicers against. */
    /** ★ V12 `fx25` / `il2p`: also run those receivers on every slicer's bits (docs/v12/AX25-9600-FX25-IL2P.md). */
    AfskDemod(double fsIn, int baud, bool single = false, bool fx25 = false, bool il2p = false, bool tones = false) : baud_(baud) {
        decim_ = tones ? 1 : std::max(1, int(std::lround(fsIn / 12000.0)));
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
            if (!tones) tableGram_(p, mark + o, space + o, fs_, L_);
            pairs_.push_back(std::move(p));
        }
        /* ★★ 2026-11-24 ONE-TONE SLICERS (docs/v12/APRS-WEAK.md): with the tones 9–12 dB or more apart (a pre-emphasised
         *  TX through a flat receiver plus a radio's own response; a de-emphasised one the other way) the weak tone is
         *  near the hiss, but the strong one still switches cleanly ON and OFF. Two more slicers read ONE tone each as
         *  on/off keying against half its own peak (mark − ½, ½ − space). Not for the tone bank (its levels are not
         *  peak-normalised) and not 300 Bd (five tunings × slicers already). */
        struct SlSpec { float a, g, c; bool ls; };
        std::vector<SlSpec> specs;
        for (float g : gains) specs.push_back({ 1.f, g, 0.f, false });
        /* ★★ and the same balances again on the FITTED levels (Pair: the two tones least-squares fitted together). Both
         *  sets, not one: the fit wins by far with the tones far apart (test-aprs: 2200 Hz 9 dB down 4 → 39 of 40) but
         *  loses a little on balanced tones through a listener's NFM chain near the threshold (test-aprs-onair --curve,
         *  ±4.5 kHz +1 kHz, audio alone at 9 dB: plain 85, fitted alone 77 of 100); the copies' vote takes both. */
        sep_ = !single && !tones;
        if (sep_) for (float g : gains) specs.push_back({ 1.f, g, 0.f, true });
        if (!single && !tones && baud != 300) { specs.push_back({ 1.f, 0.f, 0.5f, false }); specs.push_back({ 0.f, 1.f, -0.5f, false }); }
        for (size_t pi = 0; pi < pairs_.size(); ++pi)
            for (const SlSpec& sp : specs) {
                Slicer s;
                s.pair = int(pi); s.gain = sp.g; s.a = sp.a; s.c = sp.c; s.ls = sp.ls;
                s.clk.reset(new vibehdlc::BitClock(fs_, baud, 0));
                vibehdlc::Deframer::Options o;
                o.minBytes = 18; o.maxBytes = 340;
                // ★ V12: a REPAIRED frame must still be APRS (the old rule, three-in-a-million); a CLEAN one need only be
                //   any well-formed AX.25 frame (offer_ checks) — cubesat telemetry and other traffic reach the frame log.
                o.repairMax = 8; o.repairConf = 0.5f;
 o.validate = aprsFrameOk; o.validateClean = false;
                s.df.reset(new vibehdlc::Deframer(o));
                if (fx25) s.fx.reset(new vibefx25::Rx());
                if (il2p) s.il.reset(new vibeil2p::Rx());
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
    /** ★ 2026-11-24 TONE LEVELS IN (docs/v12/APRS-WEAK.md): the mark and space levels already measured elsewhere (the IQ
     *  receiver's FM tone bank, FmToneBank) at this demodulator's own rate — `fsIn` of a demodulator built with `tones`.
     *  Everything after the correlators is the same: per-tone peaks, the slicers, PLLs, deframers, FX.25 / IL2P. */
    template <class Out> void feedTones(const float* m, const float* s, int n, Out&& out) {
        // ★ RAW levels — no per-tone peak normalisation. The bank's two levels are already on one scale (an FM carrier
        //   has a constant envelope: the matched tone gives the same |sum| whichever it is), and a peak tracker's noise
        //   peaks cost 1.4 dB (test-aprs-onair --curve: 50 % point 5.06 → 3.63 dB CNR).
        for (int i = 0; i < n; ++i) { pairs_[0].mN = m[i]; pairs_[0].sN = s[i]; slice_(out); }
    }
    uint64_t repaired() const { uint64_t r = 0; for (auto& s : sl_) r += s.df->framesRepaired(); return r; }
    uint64_t fecDupes() const { return fecDupes_; }

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
    /* ★★★ 2026-11-24 THE TWO TONES SEPARATED (docs/v12/APRS-WEAK.md; Stuart's XCover + HF+ recording: 2200 Hz 13–14 dB
     *  under 1200 Hz in some packets, 7.5 dB over it in others — 0 decoded). Over one bit, 1200 and 2200 Hz are NOT
     *  orthogonal (1000 Hz apart, the window's bins are 1200 Hz): a 1200 Hz bit leaks about −14 dB into the 2200 Hz
     *  correlator. With 2200 Hz 14 dB down, the leak IS the space tone's level — every bit looks like space, whatever the
     *  slicer's balance. So the two tones are FITTED TOGETHER: the window is least-squares fitted by both tones (cos and
     *  sin of each — four basis functions; their 4 × 4 Gram matrix kept as running sums of the tones' products, exact for
     *  any rate and tuning offset) and each tone's level is its own fitted amplitude, the other tone's leak removed. A pure
     *  1200 Hz bit then gives a 2200 Hz level of ~0 however loud it is. */
    struct Gram {
        std::vector<std::complex<float>> ring;   // per sample: um², us², um·us, um·conj(us)
        std::complex<double> a2m{0, 0}, a2s{0, 0}, ap{0, 0}, ad{0, 0};
        int rp = 0, renorm = 0;
    };
    struct Pair { Tone m, s; float mN = 0, sN = 0; Gram g;
                  float mN2 = 0, sN2 = 0, pm2 = 1e-6f, ps2 = 1e-6f;   ///< ★ the FITTED levels, normalised by their own peaks
                  std::vector<float> ginv; int P = 0, nph = 0; };   ///< ★ G⁻¹ by sample index mod P, where the tones repeat
    /** ★ The Gram matrix depends only on WHERE the window sits (the tones' phases at its samples), never on the signal. Where
     *  both tones repeat within P ≤ 1200 samples (1200 / 2200 Hz at 12 kS/s: P = 60; the IQ paths' 13.3 kS/s: 200) its
     *  inverse is tabled once, and a sample costs a 4 × 4 multiply instead of a Cholesky solve. */
    static void tableGram_(Pair& p, double fm, double fsHz, double fsr, int L) {
        int P = 0;
        for (int q = 1; q <= 1200 && !P; ++q) {
            const double a = fm * q / fsr, b = fsHz * q / fsr;
            if (std::fabs(a - std::round(a)) < 1e-7 && std::fabs(b - std::round(b)) < 1e-7) P = q;
        }
        if (!P) return;
        p.P = P; p.nph = 0; p.ginv.assign(size_t(P) * 16, 0.f);
        const double wm = 2 * M_PI * fm / fsr, ws = 2 * M_PI * fsHz / fsr, ridge = 1e-3 * L;
        for (int n0 = 0; n0 < P; ++n0) {
            double G[4][8] = {};
            for (int i = 0; i < L; ++i) {
                const double n = double(n0 - i);
                const double f[4] = { std::cos(wm * n), std::sin(wm * n), std::cos(ws * n), std::sin(ws * n) };
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) G[r][c] += f[r] * f[c];
            }
            for (int r = 0; r < 4; ++r) { G[r][r] += ridge; G[r][4 + r] = 1; }
            for (int c = 0; c < 4; ++c) {   // Gauss-Jordan (SPD: no pivoting needed)
                const double d = G[c][c];
                for (int k = 0; k < 8; ++k) G[c][k] /= d;
                for (int r = 0; r < 4; ++r) if (r != c) { const double m = G[r][c]; for (int k = 0; k < 8; ++k) G[r][k] -= m * G[c][k]; }
            }
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p.ginv[size_t(n0) * 16 + size_t(r * 4 + c)] = float(G[r][4 + c]);
        }
    }
    /** One sample through a pair: the tones' correlators, then the least-squares amplitudes of both. */
    void pairStep_(Pair& p, float y, float& lm, float& ls) {
        const std::complex<float> um(p.m.ph), us(p.s.ph);   // e^{−jωn} at this sample (before the correlators advance)
        p.m.step(y); p.s.step(y);
        if (!sep_) { lm = float(std::abs(p.m.acc)); ls = float(std::abs(p.s.acc)); return; }
        const int L = int(p.m.ring.size());
        if (p.P) {
            const float* gi = &p.ginv[size_t(p.nph) * 16];
            if (++p.nph >= p.P) p.nph = 0;
            const float b[4] = { float(p.m.acc.real()), float(-p.m.acc.imag()), float(p.s.acc.real()), float(-p.s.acc.imag()) };
            float c[4];
            for (int r = 0; r < 4; ++r) c[r] = gi[r * 4] * b[0] + gi[r * 4 + 1] * b[1] + gi[r * 4 + 2] * b[2] + gi[r * 4 + 3] * b[3];
            const float h = 0.5f * float(L);
            lm = std::sqrt(c[0] * c[0] + c[1] * c[1]) * h; ls = std::sqrt(c[2] * c[2] + c[3] * c[3]) * h;
            return;
        }
        Gram& g = p.g;
        if (g.ring.empty()) g.ring.assign(size_t(L) * 4, { 0, 0 });
        const std::complex<float> v[4] = { um * um, us * us, um * us, um * std::conj(us) };
        std::complex<float>* r = &g.ring[size_t(g.rp) * 4];
        g.a2m += std::complex<double>(v[0]) - std::complex<double>(r[0]); g.a2s += std::complex<double>(v[1]) - std::complex<double>(r[1]);
        g.ap += std::complex<double>(v[2]) - std::complex<double>(r[2]); g.ad += std::complex<double>(v[3]) - std::complex<double>(r[3]);
        for (int k = 0; k < 4; ++k) r[k] = v[k];
        g.rp = (g.rp + 1) % L;
        if (++g.renorm >= 1024) {
            g.renorm = 0; g.a2m = g.a2s = g.ap = g.ad = 0;
            for (int i = 0; i < L; ++i) { const std::complex<float>* q = &g.ring[size_t(i) * 4]; g.a2m += std::complex<double>(q[0]); g.a2s += std::complex<double>(q[1]); g.ap += std::complex<double>(q[2]); g.ad += std::complex<double>(q[3]); }
        }
        // basis: cos ωm n = Re um, sin ωm n = −Im um (and the same for s); Σ over the window via the sums above
        const double h = 0.5 * L, ridge = 1e-3 * L;
        double G[4][4];
        G[0][0] = h + 0.5 * g.a2m.real() + ridge; G[1][1] = h - 0.5 * g.a2m.real() + ridge; G[0][1] = -0.5 * g.a2m.imag();
        G[2][2] = h + 0.5 * g.a2s.real() + ridge; G[3][3] = h - 0.5 * g.a2s.real() + ridge; G[2][3] = -0.5 * g.a2s.imag();
        G[0][2] = 0.5 * (g.ad.real() + g.ap.real());   // cos·cos
        G[1][3] = 0.5 * (g.ad.real() - g.ap.real());   // sin·sin
        G[0][3] = 0.5 * (g.ad.imag() - g.ap.imag());   // cos_m·sin_s
        G[1][2] = -0.5 * (g.ap.imag() + g.ad.imag());  // sin_m·cos_s
        for (int i = 0; i < 4; ++i) for (int j = 0; j < i; ++j) G[i][j] = G[j][i];
        double b[4] = { p.m.acc.real(), -p.m.acc.imag(), p.s.acc.real(), -p.s.acc.imag() };
        // Cholesky solve (G is symmetric positive definite: four independent functions over a bit, plus the ridge)
        double Lc[4][4] = {};
        for (int i = 0; i < 4; ++i) for (int j = 0; j <= i; ++j) {
            double sum = G[i][j];
            for (int k = 0; k < j; ++k) sum -= Lc[i][k] * Lc[j][k];
            if (i == j) Lc[i][i] = std::sqrt(std::max(sum, 1e-12)); else Lc[i][j] = sum / Lc[j][j];
        }
        double z[4], c[4];
        for (int i = 0; i < 4; ++i) { double sum = b[i]; for (int k = 0; k < i; ++k) sum -= Lc[i][k] * z[k]; z[i] = sum / Lc[i][i]; }
        for (int i = 3; i >= 0; --i) { double sum = z[i]; for (int k = i + 1; k < 4; ++k) sum -= Lc[k][i] * c[k]; c[i] = sum / Lc[i][i]; }
        lm = float(std::sqrt(c[0] * c[0] + c[1] * c[1]) * h); ls = float(std::sqrt(c[2] * c[2] + c[3] * c[3]) * h);
    }
    struct Slicer { int pair = 0; float gain = 1, a = 1, c = 0; bool ls = false; std::unique_ptr<vibehdlc::BitClock> clk; std::unique_ptr<vibehdlc::Deframer> df;
                    std::unique_ptr<vibefx25::Rx> fx; std::unique_ptr<vibeil2p::Rx> il; int prev = 0; };

    void level_(Pair& p, float m, float s) {
        // ★ each tone's own level: a fast-rising, slowly-falling peak — the tilt of pre-/de-emphasis divided out
        p.m.peak += (m > p.m.peak ? 0.3f : peakA_) * (m - p.m.peak);
        p.s.peak += (s > p.s.peak ? 0.3f : peakA_) * (s - p.s.peak);
        p.mN = m / std::max(p.m.peak, 1e-9f); p.sN = s / std::max(p.s.peak, 1e-9f);
    }
    template <class Out> void sample_(float y, Out& out) {
        for (auto& p : pairs_) {
            float m, sp; pairStep_(p, y, m, sp);
            level_(p, float(std::abs(p.m.acc)), float(std::abs(p.s.acc)));   // the plain correlators
            if (sep_) {                                                       // the fitted levels, their own peaks
                p.pm2 += (m > p.pm2 ? 0.3f : peakA_) * (m - p.pm2); p.ps2 += (sp > p.ps2 ? 0.3f : peakA_) * (sp - p.ps2);
                p.mN2 = m / std::max(p.pm2, 1e-9f); p.sN2 = sp / std::max(p.ps2, 1e-9f);
            }
        }
        slice_(out);
    }
    template <class Out> void slice_(Out& out) {
        for (size_t k = 0; k < sl_.size(); ++k) {
            Slicer& s = sl_[k];
            const Pair& p = pairs_[size_t(s.pair)];
            const float d = s.ls ? p.mN2 - s.gain * p.sN2 : s.a * p.mN - s.gain * p.sN - s.c;
            const bool busy = s.df->inFrame() || (s.fx && s.fx->busy()) || (s.il && s.il->busy());
            s.clk->push(d, busy ? 0.75f : 0.45f, [&](float lvl) {
                s.df->push(lvl, [&](const vibehdlc::Frame& fr) {
                    if (!fr.repaired && !ax25FrameOk(fr.bytes)) return;
                    Frame f; f.bytes = fr.bytes; f.repaired = fr.repaired; f.quality = fr.quality;
                    offer_(f, int(k), out);
                });
                const int raw = lvl >= 0 ? 1 : 0;   // mark (1200 Hz) = 1, as IL2P's AFSK symbol map says
                if (s.fx) s.fx->bit(raw == s.prev ? 1 : 0, [&](const vibefx25::Result& r) {
                    if (!ax25FrameOk(r.bytes)) return;
                    Frame f; f.bytes = r.bytes; f.fec = vibepkt::kFx25; f.fixed = r.fixed; f.quality = 0.5f;
                    offer_(f, int(k), out);
                });
                if (s.il) s.il->bit(raw, [&](const vibeil2p::Result& r) {
                    if (!ax25FrameOk(r.bytes)) return;
                    Frame f; f.bytes = r.bytes; f.fec = vibepkt::kIl2p; f.fixed = r.fixed; f.crc = r.crc; f.quality = 0.5f;
                    offer_(f, int(k), out);
                });
                s.prev = raw;
            });
        }
        ++t_;
        if (!pend_.empty() && t_ >= pendDue_) flush_(out);
    }
    template <class Out> void offer_(const Frame& fr, int slicer, Out& out) {
        const uint32_t h = frameHash(fr.bytes);
        // ★ FX.25's check bytes arrive after the frame's own end flag, so its copy of a frame a slicer already passed
        //   lands here; 1.5 s covers the longest code block at 1200 Bd (255 bytes ≈ 1.7 s at 300 Bd is HF, no FX.25).
        for (auto& r : recent_) if (r.h == h && t_ - r.t < uint64_t(fs_ * (baud_ >= 1200 ? 2.0 : 0.5))) { if (fr.fec) ++fecDupes_; return; }
        Frame f = fr; f.slicer = slicer;
        if (pend_.empty()) pendDue_ = t_ + uint64_t(fs_ / baud_ * 3) + 1;
        offerCandidate(pend_, f);
    }
    template <class Out> void flush_(Out& out) {
        std::vector<Frame> v; v.swap(pend_);
        conflicts_ += resolveCandidates(v);
        for (const Frame& f : v) { recent_[rp_] = { frameHash(f.bytes), t_ }; rp_ = (rp_ + 1) % kRecent; out(f); }
    }
public:
    /** ★ 2026-11-24: frames dropped because copies of one transmission disagreed (resolveCandidates). */
    uint64_t conflicts() const { return conflicts_; }
private:
    int baud_, decim_ = 4, dph_ = 0, L_ = 10;
    double fs_ = 12000;
    std::vector<float> lp_, hist_;
    int hp_ = 0;
    std::vector<Pair> pairs_;
    std::vector<Slicer> sl_;
    float peakA_ = 0.001f;
    bool sep_ = true;   ///< ★ the fitted slicers run too (not for the tone bank, nor `single`)
    uint64_t t_ = 0;
    std::vector<Frame> pend_; uint64_t pendDue_ = 0;
    uint64_t fecDupes_ = 0, conflicts_ = 0;
    struct Rec { uint32_t h = 0; uint64_t t = 0; };
    static constexpr int kRecent = 8;
    Rec recent_[kRecent];
    int rp_ = 0;
};

/* ★★★ 2026-11-24 THE FM TONE BANK — Bell 202 detected IN THE IQ, below the FM threshold (docs/v12/APRS-WEAK.md).
 *  A discriminator turns weak FM into clicks below ~9 dB carrier to noise and nothing after it can undo that. But an
 *  AFSK-over-FM bit is a KNOWN WAVEFORM in the IQ: one bit of a tone f at deviation D is the carrier's phase swinging
 *  β·sin(2πft + θ), β = D / f, for 1/1200 s. So correlate the IQ itself against exp(jβ·sin(2πft + θ)) over exactly one
 *  bit, for each tone, a few β (the TX's deviation and tilt are unknown) and K tone phases θ (AFSK keeps its phase
 *  across bits, so any θ can start a bit), and take the magnitude (non-coherent: the carrier's phase is unknown). The
 *  largest magnitude per tone is that tone's level. It is the textbook matched filter for a known signal in white noise,
 *  so it is not bounded by the FM threshold; Eb/N0 at 6 dB CNR in 12.5 kHz is ≈ 16 dB.
 *  ★ Costs: per sample and hypothesis two complex multiply-adds (the new sample in, the one a bit ago out — running sums,
 *    refreshed exactly every 1024 samples against float drift) and a |·|². Templates from a 256-step table per β. */
/** ★ The tone bank's kernel: K hypotheses (a multiple of 4) of one β — each running sum takes the new sample times its
 *  template and gives back the sample a bit ago times its own; returns the largest |sum|². SoA rows: the K templates at
 *  one table step lie side by side, so NEON / SSE2 take four hypotheses an instruction. Scalar is the reference. */
inline float toneBankRow(float* aRe, float* aIm, const float* nRe, const float* nIm, const float* oRe, const float* oIm, int K,
                         float zr, float zi, float wr, float wi) {
#if VIBE_NEON
    const float32x4_t vzr = vdupq_n_f32(zr), vzi = vdupq_n_f32(zi), vwr = vdupq_n_f32(wr), vwi = vdupq_n_f32(wi);
    float32x4_t mx = vdupq_n_f32(0.f);
    for (int k = 0; k < K; k += 4) {
        const float32x4_t nr = vld1q_f32(nRe + k), ni = vld1q_f32(nIm + k), or_ = vld1q_f32(oRe + k), oi = vld1q_f32(oIm + k);
        float32x4_t ar = vld1q_f32(aRe + k), ai = vld1q_f32(aIm + k);
        ar = vaddq_f32(ar, vsubq_f32(vmulq_f32(vzr, nr), vmulq_f32(vzi, ni)));
        ar = vsubq_f32(ar, vsubq_f32(vmulq_f32(vwr, or_), vmulq_f32(vwi, oi)));
        ai = vaddq_f32(ai, vaddq_f32(vmulq_f32(vzr, ni), vmulq_f32(vzi, nr)));
        ai = vsubq_f32(ai, vaddq_f32(vmulq_f32(vwr, oi), vmulq_f32(vwi, or_)));
        vst1q_f32(aRe + k, ar); vst1q_f32(aIm + k, ai);
        mx = vmaxq_f32(mx, vaddq_f32(vmulq_f32(ar, ar), vmulq_f32(ai, ai)));
    }
    float m4[4]; vst1q_f32(m4, mx);
    return std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
#elif VIBE_SSE
    const __m128 vzr = _mm_set1_ps(zr), vzi = _mm_set1_ps(zi), vwr = _mm_set1_ps(wr), vwi = _mm_set1_ps(wi);
    __m128 mx = _mm_setzero_ps();
    for (int k = 0; k < K; k += 4) {
        const __m128 nr = _mm_loadu_ps(nRe + k), ni = _mm_loadu_ps(nIm + k), or_ = _mm_loadu_ps(oRe + k), oi = _mm_loadu_ps(oIm + k);
        __m128 ar = _mm_loadu_ps(aRe + k), ai = _mm_loadu_ps(aIm + k);
        ar = _mm_add_ps(ar, _mm_sub_ps(_mm_mul_ps(vzr, nr), _mm_mul_ps(vzi, ni)));
        ar = _mm_sub_ps(ar, _mm_sub_ps(_mm_mul_ps(vwr, or_), _mm_mul_ps(vwi, oi)));
        ai = _mm_add_ps(ai, _mm_add_ps(_mm_mul_ps(vzr, ni), _mm_mul_ps(vzi, nr)));
        ai = _mm_sub_ps(ai, _mm_add_ps(_mm_mul_ps(vwr, oi), _mm_mul_ps(vwi, or_)));
        _mm_storeu_ps(aRe + k, ar); _mm_storeu_ps(aIm + k, ai);
        mx = _mm_max_ps(mx, _mm_add_ps(_mm_mul_ps(ar, ar), _mm_mul_ps(ai, ai)));
    }
    float m4[4]; _mm_storeu_ps(m4, mx);
    return std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
#else
    float mx = 0;
    for (int k = 0; k < K; ++k) {
        aRe[k] += (zr * nRe[k] - zi * nIm[k]) - (wr * oRe[k] - wi * oIm[k]);
        aIm[k] += (zr * nIm[k] + zi * nRe[k]) - (wr * oIm[k] + wi * oRe[k]);
        mx = std::max(mx, aRe[k] * aRe[k] + aIm[k] * aIm[k]);
    }
    return mx;
#endif
}

class FmToneBank {
public:
    /** `fs` the IQ rate here; `betaM` / `betaS` the modulation indices tried for mark (1200 Hz) and space (2200 Hz);
     *  `phases` the tone phases tried (rounded up to a multiple of 4 — the SIMD width). */
    FmToneBank(double fs, int baud, const std::vector<float>& betaM, const std::vector<float>& betaS, int phases = 12)
        : K_((std::max(4, phases) + 3) / 4 * 4) {
        L_ = std::max(4, int(std::lround(fs / baud)));
        const double f[2] = { baud == 300 ? 1600.0 : 1200.0, baud == 300 ? 1800.0 : 2200.0 };
        const std::vector<float>* bs[2] = { &betaM, &betaS };
        for (int t = 0; t < 2; ++t) {
            Tone& T = tone_[t];
            T.inc = uint32_t(std::llround(f[t] / fs * 4294967296.0));
            T.nb = int(bs[t]->size());
            // row m, hypothesis k: exp(−jβ·sin(2π(m/kTab + k/K)))
            T.re.assign(size_t(T.nb) * kTab * size_t(K_), 0.f); T.im.assign(T.re.size(), 0.f);
            for (int b = 0; b < T.nb; ++b)
                for (int m = 0; m < kTab; ++m)
                    for (int k = 0; k < K_; ++k) {
                        const double ph = -double((*bs[t])[size_t(b)]) * std::sin(2 * M_PI * (double(m) / kTab + double(k) / K_));
                        const size_t i = (size_t(b) * kTab + size_t(m)) * size_t(K_) + size_t(k);
                        T.re[i] = float(std::cos(ph)); T.im[i] = float(std::sin(ph));
                    }
            T.aRe.assign(size_t(T.nb) * size_t(K_), 0.f); T.aIm.assign(T.aRe.size(), 0.f);
            T.ph.assign(size_t(L_), 0);
        }
        zr_.assign(size_t(L_), { 0, 0 });
    }
    int length() const { return L_; }
    /** One IQ sample → the mark and space levels (the best hypothesis's |sum| of each). */
    void step(std::complex<float> z, float& mark, float& space) {
        const std::complex<float> zo = zr_[size_t(rp_)];
        zr_[size_t(rp_)] = z;
        float best[2];
        for (int t = 0; t < 2; ++t) {
            Tone& T = tone_[t];
            const uint32_t po = T.ph[size_t(rp_)], pn = T.cur;
            T.ph[size_t(rp_)] = pn; T.cur += T.inc;
            const size_t rn = size_t(pn >> kShift) * size_t(K_), ro = size_t(po >> kShift) * size_t(K_);
            float b = 0;
            for (int bi = 0; bi < T.nb; ++bi) {
                const size_t base = size_t(bi) * kTab * size_t(K_);
                b = std::max(b, toneBankRow(&T.aRe[size_t(bi) * size_t(K_)], &T.aIm[size_t(bi) * size_t(K_)],
                                            &T.re[base + rn], &T.im[base + rn], &T.re[base + ro], &T.im[base + ro], K_,
                                            z.real(), z.imag(), zo.real(), zo.imag()));
            }
            best[t] = b;
        }
        rp_ = (rp_ + 1) % L_;
        if (++n_ >= 1024) refresh_();
        mark = std::sqrt(best[0]); space = std::sqrt(best[1]);
    }
private:
    static constexpr int kTab = 256, kShift = 24;   // 256 template steps a cycle (≤ 0.06 rad of phase error at β 5)
    struct Tone { uint32_t inc = 0, cur = 0; int nb = 0; std::vector<float> re, im, aRe, aIm; std::vector<uint32_t> ph; };
    /** The running sums again from the ring, every 1024 samples (float sums drift). */
    void refresh_() {
        n_ = 0;
        for (auto& T : tone_)
            for (int bi = 0; bi < T.nb; ++bi)
                for (int k = 0; k < K_; ++k) {
                    double sr = 0, si = 0;
                    for (int i = 0; i < L_; ++i) {
                        const size_t j = (size_t(bi) * kTab + size_t(T.ph[size_t(i)] >> kShift)) * size_t(K_) + size_t(k);
                        const std::complex<float> z = zr_[size_t(i)];
                        sr += double(z.real() * T.re[j] - z.imag() * T.im[j]); si += double(z.real() * T.im[j] + z.imag() * T.re[j]);
                    }
                    T.aRe[size_t(bi) * size_t(K_) + size_t(k)] = float(sr); T.aIm[size_t(bi) * size_t(K_) + size_t(k)] = float(si);
                }
    }
    int K_, L_ = 10, rp_ = 0, n_ = 0;
    Tone tone_[2];
    std::vector<std::complex<float>> zr_;
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
/* ★★ V12 beta95 AN ALTITUDE BEYOND PHYSICAL SENSE FOR THE SYMBOL (Stuart 11-25: G8TMV-9, a car, sent /A=301169 —
 *  91 796 m — and the list showed it as real). The ceiling per symbol: things on the ground (cars, houses, joggers, ships,
 *  digipeaters, weather stations…) above Mount Everest, 9 000 m; aircraft above 25 000 m; anything else above a balloon's
 *  record, 60 000 m; a satellite / shuttle, none. Below the Dead Sea shore, −500 m, for anything. The figure is still
 *  shown — marked "implausible", never presented as real. ONE RULE, TWO READERS: src/utils/aprsSymbols.ts aprsAltCeiling. */
inline double altCeilingM(char table, char code) {
    const bool alt = table != '/';
    if (code == 'S') return INFINITY;                                       // shuttle / satellite
    if (code == 'O') return 60000;                                          // balloon (either table)
    if (!alt && (code == '^' || code == '\'' || code == 'X' || code == 'g')) return 25000;   // aircraft, helicopter, glider
    if (alt && code == '^') return 25000;                                   // aircraft (the alternate table's)
    static const char* ground = ">jkuvRUaf-KH#&r_WysCY[b<=+h;nP!F";      // the primary table's ground things
    static const char* groundAlt = "#&->_kuvjs";                            // the alternate codes that mean the same kind
    if (std::strchr(alt ? groundAlt : ground, code) && code != ' ') return 9000;
    return 60000;
}
inline bool altPlausible(char table, char code, double metres) {
    return !(metres == metres) || (metres >= -500 && metres <= altCeilingM(table, code));
}
/** "car" → "for a car"; '' → "here" — the word after "implausible". */
inline std::string altWhy(char table, char code) {
    const std::string n = symbolName(table, code);
    return n.empty() ? std::string() : " for a " + n;
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
    if (p.altFt == p.altFt) {
        s += dot + fmt(p.altFt * 0.3048, 0) + " m";
        if (!altPlausible(p.symTable, p.symCode, p.altFt * 0.3048)) s += " (implausible" + altWhy(p.symTable, p.symCode) + ")";   // ★ beta95
    }
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
