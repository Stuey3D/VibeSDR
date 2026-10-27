// test-band-dc.cpp — BAND SPOTS AND THE DONGLE'S DC (V12, 2026-10-27, branch v12-band-dc; vibe_band_slice.h bandDcCentre).
//
// A zero-IF radio's DC sits at rtlCenter + hwOffsetHz(). With Band's dial on the slice centre and the tuner following the
// dial (auto IF), an RTL's 15 kHz put 40 m's DC on FT8 (Stuart's Pi 2, 7.0756). This test:
//   • prints, for every band at five capture rates and three offsets (RTL/RSP 15 kHz, HackRF 250 kHz, rtl_tcp 0), where the
//     DC landed BEFORE (tuner on the dial) — and lists every band where it fell on a mode;
//   • checks the placement AFTER: never on a mode, outside every slice wherever the capture allows, every slice still inside
//     the capture, idempotent, the smallest move, the caller's bounds honoured when they can be, nothing off a band.
//   test-band-dc            the checks (and the table)
//   test-band-dc where <rate> <dialHz> <dcHz>   prints where a DC sits: 0 outside every slice, 1 inside one, 2 <MODE> on a mode
//                                                (scripts/test-server-band-dc.mjs reads it, so the e2e and the rule agree)
#include "vibe_band_slice.h"
#include "decoders/digi_bands.h"
#include "decoders/digi_mode.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0, checks = 0;
static void ok(bool c, const std::string& what) { ++checks; if (!c) { ++fails; std::printf("  [FAIL] %s\n", what.c_str()); } }
static void okv(bool c, const std::string& what) { ++checks; std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) ++fails; }
// ★ the shim's own rule (local_sdr_shim.cpp fftSizeForRate)
static int fftFor(double rate) { double want = rate / 75.0; int s = 4096; while (s < (int)want && s < 32768) s *= 2; return s; }

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "where") {
        const double r = std::atof(argv[2]);
        std::string m;
        const int w = bandDcWhere(r, fftFor(r), std::atof(argv[3]), std::atof(argv[4]), &m);
        std::printf("%d %s\n", w, w == 2 ? m.c_str() : "-");
        return 0;
    }
    const double rates[] = { 2400000, 2048000, 1024000, 960000, 300000 };
    struct Off { const char* who; double hz; double minRate; } offs[] = { { "RTL/RSP 15 kHz", 15000, 0 }, { "HackRF 250 kHz", 250000, 2e6 },
                                                                         { "rtl_tcp 0", 0, 0 } };
    std::printf("── where the DC landed BEFORE (tuner on the dial), every band ──\n");
    std::set<std::string> onMode;
    for (const digi::Band& b : digi::bands()) {
        const digi::Span sp = digi::spanOf(b, digi::modes());
        if (!sp.ok) continue;
        for (const Off& o : offs) {
            std::string row;
            for (double r : rates) {
                if (r < o.minRate) continue;
                std::string m;
                const int w = bandDcWhere(r, fftFor(r), sp.centreHz, sp.centreHz + o.hz, &m);
                char cell[96];
                std::snprintf(cell, sizeof cell, " %5.3gM:%s", r / 1e6, w == 2 ? ("ON " + m).c_str() : w == 1 ? "in-slice" : "clear");
                row += cell;
                if (w == 2) onMode.insert(std::string(b.id) + " " + o.who + " → " + m + " at " + std::to_string((long long)(sp.centreHz + o.hz)));
            }
            std::printf("  %-5s dial %10.1f kHz  %-15s DC %10.1f kHz |%s\n", b.id, sp.centreHz / 1e3, o.who, (sp.centreHz + o.hz) / 1e3, row.c_str());
        }
    }
    std::printf("── bands where the DC fell ON A MODE before ──\n");
    for (const std::string& s : onMode) std::printf("  %s\n", s.c_str());
    okv(onMode.count(std::string("40m RTL/RSP 15 kHz → FT8 at 7075600")) == 1, "the Pi 2's case is in the list: 40 m, 15 kHz, DC 7.0756 on FT8");

    std::printf("── AFTER: bandDcCentre on every band, rate and offset ──\n");
    int moved = 0, total = 0, inSlice = 0;
    double worstMove = 0;
    for (const digi::Band& b : digi::bands()) {
        const digi::Span sp = digi::spanOf(b, digi::modes());
        if (!sp.ok) continue;
        for (const Off& o : offs) for (double r : rates) {
            if (r < o.minRate) continue;
            const int fft = fftFor(r);
            const double dial = sp.centreHz;
            char tag[160]; std::snprintf(tag, sizeof tag, "%s %s at %.3g MS/s", b.id, o.who, r / 1e6);
            // the slices must fit the capture at all (a 300 kS/s radio cannot hold 6 m's JTTY: that is the "out" chip, not ours)
            const std::vector<BandDcZone> sl = bandDcSlices(r, fft, dial);
            const double c = bandDcCentre(r, fft, dial, dial, o.hz);
            const double dc = c + o.hz;
            std::string m;
            const int where = bandDcWhere(r, fft, dial, dc, &m);
            ++total; if (std::fabs(c - dial) > 0.5) ++moved;
            worstMove = std::max(worstMove, std::fabs(c - dial));
            ok(where != 2, std::string(tag) + ": the DC is never on a mode (on " + m + ")");
            // within the guard of a passband is "on" too
            // ★ within the guard of a passband: allowed only where the capture is too narrow to go anywhere else (300 kS/s)
            const bool guarded = bandDcWhere(r, fft, dial, dc, nullptr, kBandDcGuardHz - 1) != 2;
            if (!guarded && r <= 300000) std::printf("  (%s: DC %.1f kHz is between passbands but within %.0f kHz of one — a 300 kS/s capture has no room)\n", tag, dc / 1e3, kBandDcGuardHz / 1e3);
            else ok(guarded, std::string(tag) + ": the DC keeps its guard from every passband");
            if (where == 1) { ++inSlice; std::printf("  (%s: DC inside a slice, clear of every mode — the capture is too narrow to leave the slices)\n", tag); }
            ok(std::fabs(bandDcCentre(r, fft, dial, c, o.hz) - c) < 0.5, std::string(tag) + ": idempotent");
            // the main slice still inside the capture whenever it was before
            const BandDcZone& mz = sl[0];
            const bool heldBefore = captureHolds(r, dial + o.hz, 0.5 * (mz.loHz + mz.hiHz), mz.hiHz - mz.loHz);
            ok(!heldBefore || captureHolds(r, dc, 0.5 * (mz.loHz + mz.hiHz), mz.hiHz - mz.loHz), std::string(tag) + ": the main slice is still inside the capture");
            if (std::fabs(c - dial) > 0.5) ok(std::fmod(std::fabs(dc), 100.0) < 1e-6, std::string(tag) + ": the moved DC is on a whole 100 Hz");
        }
    }
    okv(fails == 0, "every band, rate and offset: never on a mode, guard kept, idempotent, slices captured (" + std::to_string(checks) + " checks)");
    std::printf("  %d of %d placements moved the tuner, the largest by %.1f kHz; %d left the DC inside a slice (clear of every mode)\n",
                moved, total, worstMove / 1e3, inSlice);

    std::printf("── the Pi 2's case, in numbers ──\n");
    {
        const double r = 2048000, dial = 7060600, off = 15000; const int fft = fftFor(r);
        const double c = bandDcCentre(r, fft, dial, dial, off);
        std::string m; const int before = bandDcWhere(r, fft, dial, dial + off, &m);
        std::printf("  40 m, 2.048 MS/s, 15 kHz: tuner %.1f → %.1f kHz, DC %.1f → %.1f kHz (%s → %s)\n", dial / 1e3, c / 1e3, (dial + off) / 1e3,
                    (c + off) / 1e3, before == 2 ? ("on " + m).c_str() : "clear", bandDcWhere(r, fft, dial, c + off) == 0 ? "outside every slice" : "inside a slice");
        okv(before == 2 && m == "FT8", "before: 40 m's DC on FT8");
        okv(bandDcWhere(r, fft, dial, c + off) == 0, "after: outside every slice (main and JTTY's extra)");
        // the smallest way out: the slices' union is 7.0256…7.1108 with the guard; up is 35.2 kHz, down 50 kHz
        okv(c + off > dial && std::fabs((c + off) - (dial + off)) < 40000, "after: the nearer side (up, < 40 kHz of travel)");
        // the vfo stays well inside the capture: Band's slices sit within ±0.45 of the rate
        for (const BandDcZone& s : bandDcSlices(r, fft, dial))
            okv(captureHolds(r, c + off, 0.5 * (s.loHz + s.hiHz), s.hiHz - s.loHz), "after: slice " + std::to_string((long long)s.loHz) + "–" + std::to_string((long long)s.hiHz) + " inside the capture");
    }
    std::printf("── the rule's edges ──\n");
    {
        const double r = 2048000; const int fft = fftFor(r);
        okv(bandDcCentre(r, fft, 9000000, 9000000, 15000) == 9000000, "off every band: unchanged");
        okv(bandDcCentre(r, fft, 7060600, 6900000, 15000) == 6900000, "a DC already outside every slice: unchanged (no needless retune)");
        // bounds: the caller allows the centre only below the dial — the move goes down (50 kHz) rather than up
        const double cDown = bandDcCentre(r, fft, 7060600, 7060600, 15000, -INFINITY, 7060600);
        okv(cDown + 15000 < 7030000 && bandDcWhere(r, fft, 7060600, cDown + 15000) == 0, "bounds honoured: the centre stays below the dial when asked (DC " + std::to_string((long long)(cDown + 15000)) + ")");
        // bounds that cannot be met are waived — the DC matters more than a view edge
        const double cAny = bandDcCentre(r, fft, 7060600, 7060600, 15000, 7060000, 7061000);
        okv(bandDcWhere(r, fft, 7060600, cAny + 15000) == 0, "impossible bounds are waived: the DC still leaves the slices");
        // rtl_tcp (offset 0): the DC at the dial is inside the main slice on every band — moved out
        const double ct = bandDcCentre(r, fft, 14083700, 14083700, 0);
        okv(bandDcWhere(r, fft, 14083700, ct) == 0, "rtl_tcp on 20 m: the DC (at the dial) leaves the slice");
        // HackRF on 2 m: its 250 kHz lands between 2 m's clusters — clear already, so nothing moves
        const digi::Span s2 = digi::spanOf(*digi::bandById("2m"), digi::modes());
        const double ch = bandDcCentre(2e6, fftFor(2e6), s2.centreHz, s2.centreHz, 250000);
        std::printf("  HackRF 2 m: dial %.1f, DC %.1f → %.1f kHz\n", s2.centreHz / 1e3, (s2.centreHz + 250000) / 1e3, (ch + 250000) / 1e3);
        okv(bandDcWhere(2e6, fftFor(2e6), s2.centreHz, ch + 250000) != 2, "HackRF on 2 m: never on a mode");
        // a band step: the next band's own rule, from wherever the tuner was
        const double c30 = bandDcCentre(r, fft, 10137700, 10137700, 15000);
        okv(bandDcWhere(r, fft, 10137700, c30 + 15000) == 0, "30 m (a step from 40 m): outside 30 m's slices");
    }
    std::printf("── Band on ONE VFO, somebody listening on the OTHER (V12 beta24, VFO A/B) ──\n");
    {
        // ★ Band on B's 40 m (dial 7.074), A listening on AM 7.100 MHz (±5 kHz + 2.5 kHz guard). The plain rule would take
        //   the DC up out of the slices — and could land it on A. With A's zone as `also`, it never does.
        const double r = 2048000, off = 15000, bDial = 7074000; const int fft = fftFor(r);
        const double aLo = 7100000 - 7500, aHi = 7100000 + 7500;
        int bad = 0, onA = 0;
        for (double centre = 7000000; centre <= 7150000; centre += 500) {
            const double c = bandDcCentre(r, fft, bDial, centre, off, -INFINITY, INFINITY, aLo, aHi);
            const double dc = c + off;
            if (bandDcWhere(r, fft, bDial, dc) != 0) bad++;
            if (dc > aLo && dc < aHi) onA++;
        }
        okv(bad == 0, "every starting centre 7.000–7.150 MHz: the DC ends outside B's band's slices (" + std::to_string(bad) + " not)");
        okv(onA == 0, "…and never on A's passband + guard (" + std::to_string(onA) + " on it)");
        // a DC outside the slices but ON A's passband is moved as well
        const double c = bandDcCentre(r, fft, bDial, 7100000 - off, off, -INFINITY, INFINITY, aLo, aHi);
        okv(c + off <= aLo || c + off >= aHi, "a DC on A (outside B's slices) is moved off A");
        okv(bandDcWhere(r, fft, bDial, c + off) == 0, "…and not into B's slices");
        // no `also`: exactly the old placement
        okv(bandDcCentre(r, fft, bDial, 7074000, off) == bandDcCentre(r, fft, bDial, 7074000, off, -INFINITY, INFINITY, 0, 0),
            "with no other VFO: the placement as before");
    }
    std::printf("\n%s — %d checks, %d failed\n", fails ? "FAILED" : "all passed", checks, fails);
    return fails ? 1 : 0;
}
