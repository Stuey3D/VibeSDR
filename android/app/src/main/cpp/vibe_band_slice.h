// vibe_band_slice.h — Band spots' (and the CW/PSK skimmers') IQ SLICES off the channeliser: how wide, how flat, and
// where the extra ones go (V12, 2026-10-27).
//
// ★★ ONE RULE, THREE READERS: the per-listener extract (local_sdr_shim.cpp feedOneClient), the one-pipeline tap
//    (vibe_wide_tap.h) and the plan (digi::DigiTuner::fits). They had a copy each, and the copies disagreed: the slice was
//    sized as if only its outer 4 % rolled off (w / 0.92) while Channelizer::extract rolled off the outer QUARTER on each
//    side — so 40 m's WSPR/PSK/JS8 sat 2–3 dB down the taper at 960 kS/s and JTTY 8–17 dB down at 2.4 MS/s.
//    ★ The slice now takes a narrow roll-off (kTaperDiv: a sixteenth a side → flat over 7/8 of its rate) and is sized
//      for that flat part. Measured (test-band-slice): every mode's passband edge within 0.5 dB at every capture rate,
//      block-rate spurs below −90 dB — the same cost as before (the bins a slice needs did not change on any band at
//      2.4 MS/s; at 960 kS/s 40 m goes 30 → 60 kHz — one inverse FFT of 64 → 128 points per block, nothing).
#pragma once
#include "decoders/digi_bands.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace vibe {

/** The slice's roll-off: chanBins / kTaperDiv bins at each edge (Channelizer::extract's `taperDiv`). */
constexpr int kBandTaperDiv = 16;
/** What part of the slice's rate is flat: 1 − 2/kTaperDiv, less a cushion for the raised cosine's first bin. */
constexpr double kBandFlatFrac = 1.0 - 2.0 / kBandTaperDiv - 0.01;
static_assert(kBandTaperDiv == 16, "digi::DigiTuner::kFlatHalfFrac assumes a sixteenth a side");

/** The slice's size in bins (a power of two, at most the FFT) for a wanted (flat) width. */
inline int bandSliceBins(double sampleRate, int fftSize, double widthHz) {
    int b = 64;
    while (b < fftSize && sampleRate * (double)b / (double)fftSize * kBandFlatFrac < widthHz) b <<= 1;
    return std::min(b, fftSize);
}

/** ★ One extra slice: its RF centre and the flat width it needs. */
struct ExtraSlice { double centreHz = 0, widthHz = 0; };
constexpr int kMaxExtraSlices = 3;

/** The extra slices a Band run on `dialHz` wants (the band's clusters beyond its main one — digi::Span::extras). */
inline std::vector<ExtraSlice> bandExtraSlices(double dialHz) {
    std::vector<ExtraSlice> o;
    const digi::Band* b = digi::bandAt(dialHz);
    if (!b) return o;
    const digi::Span sp = digi::spanOf(*b, digi::modes());
    if (!sp.ok) return o;
    for (const digi::SubSpan& e : sp.extras) {
        if ((int)o.size() >= kMaxExtraSlices) break;
        o.push_back({ e.centreHz, e.widthHz() });
    }
    return o;
}

/** ★ Can a capture centred on `hwCentreHz` at `sampleRate` hold a slice centred on `centreHz` with `rate`? Its whole width
 *  inside the inner 90 % of the capture (an RTL's and an Airspy's edges are filter skirt). */
inline bool captureHolds(double sampleRate, double hwCentreHz, double centreHz, double rate) {
    return std::fabs(centreHz - hwCentreHz) + rate / 2.0 <= sampleRate * 0.45;
}

} // namespace vibe

/** The IQ width Band mode wants for a dial (DecoderHost::bandWidthFor): the registered modes' main span of the band the
 *  dial is on, wide enough either side of the dial to hold it all. Off any band: 24 kHz. */
inline double DecoderHostBandWidth(double dialHz) {
    using namespace vibe;
    const digi::Band* b = digi::bandAt(dialHz);
    if (!b) return 24000.0;
    const digi::Span sp = digi::spanOf(*b, digi::modes());
    if (!sp.ok) return 24000.0;
    return std::max(4000.0, 2.0 * std::max(std::fabs(sp.hiHz - dialHz), std::fabs(dialHz - sp.loHz)));
}

// ── ★★★ THE DONGLE'S DC, KEPT OFF BAND'S PASSBANDS (V12, 2026-10-27, branch v12-band-dc) ─────────────────────────────────
//
// ★★★ THE FAULT. A zero-IF radio's DC sits at the capture's PHYSICAL centre: rtlCenter + hwOffsetHz() (the R820T/R828D's
//     15 kHz offset tuning, the HackRF's 250 kHz, an RSP's 15 kHz — or rtlCenter itself on an rtl_tcp dongle, which gets no
//     offset). Single mode never meets it: the offset exists precisely to keep it off the listener's channel. Band mode does,
//     because the dial IS the slice's centre and the slice is up to 75 kHz wide: on Stuart's Pi 2 (V4, 2.048 MS/s) 40 m's
//     dial 7.0606 put the DC at 7.0756 — in the middle of FT8 (7.0742–7.0770), where every station is.
// ★★ THE FIX MOVES THE CAPTURE, NOT THE OFFSET. hwOffsetHz() is read in ~60 places and copied into half a dozen (the raw-IQ
//    rotation, the background runner, a pending pan, DAB's RF centre …) — the one number that once shipped 15.00 kHz wrong
//    because two readers disagreed (Radio Caroline on 648 reading 663). Changing it for Band would re-open exactly that. The
//    capture's LOGICAL centre (rtlCenter), on the other hand, already moves away from the dial on every minimal-movement pan,
//    and every consumer derives what it needs from rtlCenter + hwOffsetHz() at the moment it uses it. So in Band the tuner is
//    placed — by the same placement code a pan uses — where its DC falls OUTSIDE the band's slices (main and extras), and
//    hwOffsetHz() never changes. Band off → the ordinary placement again (in auto IF, the tuner follows the dial).
// ★ PREFERENCE ORDER: outside every slice (the channeliser then never sees the spike at all) → else outside every mode's
//   passband (the tuners filter it out) → else where it was. Each with kBandDcGuardHz of clearance, the smallest move, and
//   every slice still inside the capture (captureHolds).
// ★ MEASURED, AND SAID PLAINLY: Stuart's V4 at 7.150 MHz (~/VibeSDR-test-audio/iq/v4l_40m_7150000_2400k.cu8) shows a DC of
//   −45 dBFS one FFT bin wide (±50 Hz), constant to 0.0001 over 30 s — the capture cleaner's slow mean removes it, and the
//   40 m recording through the slice route with that DC on FT8 lost no FT8 calls (80 of 80; 78 of 80 at −25 dBFS). So a CLEAN
//   spike costs little; a dongle whose DC region is dirty (1/f skirt, a DC that steps with every gain change) costs the whole
//   sub-band. Moving it out of the slice costs one retune at Band start, on a band step, and at Band stop.

namespace vibe {

/** Clearance the DC keeps from a slice's edge or a mode's passband edge. */
constexpr double kBandDcGuardHz = 3000.0;

struct BandDcZone { double loHz = 0, hiHz = 0; };

/** The RF extents of the slices a Band run on `dialHz` cuts (main, then the extras), as the tap and the per-listener
 *  extract size them (bandSliceBins of their flat width) — or nothing off a band. */
inline std::vector<BandDcZone> bandDcSlices(double sampleRate, int fftSize, double dialHz) {
    std::vector<BandDcZone> z;
    if (!(sampleRate > 0) || fftSize < 64 || !vibe::digi::bandAt(dialHz)) return z;
    const double w = std::min(::DecoderHostBandWidth(dialHz), sampleRate * 0.8);
    const double r = sampleRate * vibe::bandSliceBins(sampleRate, fftSize, w) / fftSize;
    z.push_back({ dialHz - r / 2, dialHz + r / 2 });
    for (const vibe::ExtraSlice& e : vibe::bandExtraSlices(dialHz)) {
        const double er = sampleRate * vibe::bandSliceBins(sampleRate, fftSize, e.widthHz) / fftSize;
        z.push_back({ e.centreHz - er / 2, e.centreHz + er / 2 });
    }
    return z;
}

/** Every registered mode's passband on the band `dialHz` is on (main slice and extras), with the mode's name. */
struct BandDcPassband { double loHz = 0, hiHz = 0; std::string mode; };
inline std::vector<BandDcPassband> bandDcPassbands(double dialHz) {
    std::vector<BandDcPassband> o;
    const vibe::digi::Band* b = vibe::digi::bandAt(dialHz);
    if (!b) return o;
    for (const vibe::digi::ModeDef& m : vibe::digi::modes())
        for (double d : vibe::digi::dialsOf(m, b->id)) o.push_back({ d + m.audioLoHz, d + m.audioHiHz, m.name });
    return o;
}

/** Where a DC at `dcHz` sits for a Band run on `dialHz`: 0 = outside every slice, 1 = inside a slice but clear of every
 *  passband (by `guardHz`), 2 = on a mode (its name in `*onMode`). */
inline int bandDcWhere(double sampleRate, int fftSize, double dialHz, double dcHz, std::string* onMode = nullptr,
                       double guardHz = 0.0) {
    for (const BandDcPassband& p : bandDcPassbands(dialHz))
        if (dcHz >= p.loHz - guardHz && dcHz <= p.hiHz + guardHz) { if (onMode) *onMode = p.mode; return 2; }
    for (const BandDcZone& s : bandDcSlices(sampleRate, fftSize, dialHz))
        if (dcHz >= s.loHz && dcHz <= s.hiHz) return 1;
    return 0;
}

/** ★★ THE PLACEMENT: the logical centre (rtlCenter) to use instead of `centreHz` while Band runs on `dialHz`, so that the DC
 *  (centre + `hwOffsetHz`) is clear — see the note above. `loHz`/`hiHz`: where the caller's own rules allow the centre to be
 *  (the VFO captured, the view inside the capture); honoured if any clear place lies within them, else waived (the DC
 *  matters more than a view edge). Returns `centreHz` unchanged off a band, or when it is already clear. Idempotent.
 *  ★★ `alsoLoHz`..`alsoHiHz` (V12 beta24, VFO A/B): ONE MORE place the DC must not be — the OTHER VFO's passband + guard
 *  (absolute Hz; empty when lo >= hi) when Band runs on one VFO and somebody listens on the other. It joins every tier, so a
 *  DC on that passband is moved as a DC on a slice is, and no way out lands on it. */
inline double bandDcCentre(double sampleRate, int fftSize, double dialHz, double centreHz, double hwOffsetHz,
                           double loHz = -INFINITY, double hiHz = INFINITY, double alsoLoHz = 0.0, double alsoHiHz = 0.0) {
    if (!(sampleRate > 0) || !vibe::digi::bandAt(dialHz)) return centreHz;
    const std::vector<BandDcZone> slices = bandDcSlices(sampleRate, fftSize, dialHz);
    if (slices.empty()) return centreHz;
    std::vector<BandDcZone> modes;
    for (const BandDcPassband& p : bandDcPassbands(dialHz)) modes.push_back({ p.loHz, p.hiHz });
    std::vector<BandDcZone> cut = slices;
    if (alsoLoHz < alsoHiHz) { cut.push_back({ alsoLoHz, alsoHiHz }); modes.push_back({ alsoLoHz, alsoHiHz }); }
    const double dc = centreHz + hwOffsetHz;
    // the union of the zones, each widened by the guard, merged
    auto merged = [](std::vector<BandDcZone> z) {
        for (BandDcZone& x : z) { x.loHz -= kBandDcGuardHz; x.hiHz += kBandDcGuardHz; }
        std::sort(z.begin(), z.end(), [](const BandDcZone& a, const BandDcZone& b) { return a.loHz < b.loHz; });
        std::vector<BandDcZone> m;
        for (const BandDcZone& x : z) {
            if (!m.empty() && x.loHz <= m.back().hiHz) m.back().hiHz = std::max(m.back().hiHz, x.hiHz);
            else m.push_back(x);
        }
        return m;
    };
    auto inside = [](const std::vector<BandDcZone>& m, double f) {
        for (const BandDcZone& x : m) if (f > x.loHz && f < x.hiHz) return &x;
        return (const BandDcZone*)nullptr;
    };
    // a DC at `p` must leave every slice inside the capture (the slices are cut from it)
    // ★★ …every slice it CAN hold (strict = false). A narrow capture cannot hold a band's far extras at all wherever it sits
    //    (300 kS/s on 2 m: the main slice at 144.16 and the WSPR / MSK144 extras at 144.49 / 144.36, 400 kHz apart; 70 cm's
    //    Q65 and MSK144 extras since 8fa07987b / f8e691200) — that is the mode's "out" chip, not ours. Demanding them too
    //    made EVERY way out fail, and the DC was left ON FT8 (test-band-dc). So: first every slice (strict, as before);
    //    when no place does that, the slices the capture holds where it is now — none of those is ever given up.
    auto holds = [&](double p, bool strict) {
        for (const BandDcZone& s : slices) {
            const double sc = 0.5 * (s.loHz + s.hiHz), sw = s.hiHz - s.loHz;
            if (!strict && !captureHolds(sampleRate, dc, sc, sw)) continue;
            if (!captureHolds(sampleRate, p, sc, sw)) return false;
        }
        return true;
    };
    const std::vector<BandDcZone> tiers[2] = { merged(cut), merged(modes) };
    for (int strict = 1; strict >= 0; --strict)
    for (int pass = 0; pass < 2; ++pass) {              // ★ pass 0 inside the caller's bounds, pass 1 without them
        for (const std::vector<BandDcZone>& z : tiers) {
            const BandDcZone* in = inside(z, dc);
            if (!in) return centreHz;                    // ★ already clear at this tier
            // the two ways out of the zone the DC is in, on whole 100 Hz (outward, so the rounding never steps back in)
            const double cand[2] = { std::floor(in->loHz / 100.0) * 100.0, std::ceil(in->hiHz / 100.0) * 100.0 };
            double best = NAN;
            for (double p : cand) {
                const double c = p - hwOffsetHz;
                if (pass == 0 && (c < loHz || c > hiHz)) continue;
                if (!holds(p, strict != 0)) continue;
                if (!std::isfinite(best) || std::fabs(p - dc) < std::fabs(best - dc)) best = p;
            }
            if (std::isfinite(best)) return best - hwOffsetHz;
        }
    }
    return centreHz;
}

} // namespace vibe
