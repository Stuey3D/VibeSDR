// test-dxscan-fm.cpp — THE DX SCANNER'S FM HOP (vibe_dxscan_fm.h): several RDS stations decoded from ONE wide capture at once.
// V12, 2026-11-04. docs/v12/DX-SCANNER.md.
//
// A synthetic 2.4 MS/s capture (an RTL's) holding broadcast stations at chosen offsets and levels — each a real MPX (pilot,
// stereo, RDS group 0A with its PI and PS, proper checkwords, differentially encoded, biphase on 57 kHz locked to the pilot:
// the benchmark's encoder) — plus noise. Checks: every station found on the raster, its PI and PS decoded, CRC-valid
// groups counted, no RDS → no PI, a carrier-free channel → no peak, abort → nothing; and the cost (CPU per hop).
// ★ Silent: no audio device anywhere.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include "vibe_dxscan_fm.h"

using namespace vibe::dx;
using vibedsp::cf32;
static int pass = 0, fail = 0;
static void ok(bool c, const std::string& what) { c ? ++pass : ++fail; std::printf("   %s %s\n", c ? "ok  " : "FAIL", what.c_str()); }

struct Station { double offHz; uint16_t pi; const char* ps; double amp; bool rds; };

/** The RDS bit stream (differential) for PI + PS over group 0A, repeated. */
static std::vector<int> rdsBits(uint16_t pi, const char* ps) {
    using vibedsp::RdsDecoder;
    auto enc = [](uint16_t data, int off) { const uint16_t cw = RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[off]; return ((uint32_t)data << 10) | cw; };
    std::vector<int> bits;
    for (int addr = 0; addr < 4; ++addr) {
        const uint32_t blk[4] = { enc(pi, 0), enc((uint16_t)(addr & 3), 1), enc(0xE0CD, 2),
                                  enc((uint16_t)(((uint8_t)ps[addr * 2] << 8) | (uint8_t)ps[addr * 2 + 1]), 4) };
        for (int b = 0; b < 4; ++b) for (int k = 25; k >= 0; --k) bits.push_back((blk[b] >> k) & 1);
    }
    std::vector<int> m(bits.size()); int prev = 0;
    for (size_t k = 0; k < bits.size(); ++k) { m[k] = prev ^ bits[k]; prev = m[k]; }
    return m;
}

/** `seconds` of capture at `fs` holding `st`, plus complex noise of RMS `noise` per component. */
static std::vector<cf32> capture(double fs, double seconds, const std::vector<Station>& st, double noise, unsigned seed) {
    const size_t n = (size_t)(fs * seconds);
    std::vector<cf32> iq(n, cf32(0, 0));
    std::mt19937 g(seed); std::normal_distribution<float> d(0.f, (float)noise);
    for (auto& v : iq) v = cf32(d(g), d(g));
    for (size_t s = 0; s < st.size(); ++s) {
        const auto& S = st[s];
        const auto m = rdsBits(S.pi, S.ps);
        double ph = 0;
        const double t0 = 0.0137 * (double)s;   // each station's programme at its own phase
        for (size_t i = 0; i < n; ++i) {
            const double t = i / fs + t0;
            const double L = 0.25 * std::cos(2 * M_PI * (700.0 + 150 * s) * t), R = 0.25 * std::cos(2 * M_PI * (3100.0 - 90 * s) * t);
            const double pilot = 0.09 * std::cos(2 * M_PI * 19000.0 * t);
            const double stereo = (L - R) * std::cos(2 * M_PI * 38000.0 * t);
            double rds = 0;
            if (S.rds) {
                const long kb = (long)std::floor(t * 1187.5);
                const double phInBit = (t * 1187.5 - kb) * 2 * M_PI;
                const double manch = ((phInBit < M_PI) ? 1.0 : -1.0) * (m[(size_t)kb % m.size()] ? 1.0 : -1.0);
                rds = 0.045 * manch * std::cos(2 * M_PI * 57000.0 * t);
            }
            const double mpx = (L + R) + pilot + stereo + rds;
            ph += 2 * M_PI * (S.offHz + 75000.0 * mpx) / fs;
            if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            iq[i] += cf32((float)(S.amp * std::cos(ph)), (float)(S.amp * std::sin(ph)));
        }
    }
    return iq;
}

static std::vector<FmHit> runHop(FmHopEngine& e, const FmHopEngine::Params& p, const std::vector<cf32>& iq, double* wall = nullptr,
                                 double abortAfter = -1) {
    e.start(p);
    const auto w0 = std::chrono::steady_clock::now();
    const size_t blk = (size_t)(p.rateHz / 100);   // 10 ms pieces, as a radio delivers them
    size_t pos = 0; double fed = 0;
    while (e.running()) {
        if (abortAfter >= 0 && fed >= abortAfter) { e.abort(); break; }
        while (e.running() && e.queuedSec() > 0.2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const size_t n = std::min(blk, iq.size() - pos);
        e.feed(iq.data() + pos, (int)n);
        pos = (pos + n) % iq.size(); fed += n / p.rateHz;
        if (fed > 60) break;
    }
    e.abort();   // joins (a finished hop keeps its results)
    if (wall) *wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    return e.results();
}

static int64_t T0dx() { return 1800000000; }

int main() {
    const double fs = 2.4e6, centre = 98.95e6;   // a hop centre half a raster step off the raster
    // stations on the 100 kHz raster at offsets from the centre (98.950): 98.1, 98.4, 98.8, 99.2, 99.6
    const std::vector<Station> st = {
        { -850e3, 0xC201, "BBC R4  ", 0.30, true },
        { -550e3, 0xC479, "HEART   ", 0.10, true },
        { -150e3, 0x5201, "RAI R1  ", 0.02, true },    // weaker (Es-ish)
        { +250e3, 0xD3C1, "  NO RDS", 0.10, false },   // a carrier with no RDS
        { +650e3, 0xF201, "FR INTER", 0.05, true },
    };
    std::printf("── synthesising 5 s of a 2.4 MS/s capture with %zu stations ──\n", st.size());
    const auto iq = capture(fs, 5.0, st, 0.002, 7);

    FmHopEngine eng;
    FmHopEngine::Params p;
    p.rateHz = fs; p.centreHz = centre; p.dcHz = centre; p.rasterKhz = 100;
    const auto plan = planFm(fs, 100);
    p.loHz = centre - fmHalfUse(fs); p.hiHz = centre + fmHalfUse(fs);
    p.loHz = std::ceil((p.loHz - 87.5e6) / 1e5) * 1e5 + 87.5e6; p.hiHz = std::floor((p.hiHz - 87.5e6) / 1e5) * 1e5 + 87.5e6;
    p.dwellSec = 4.0;
    double wall = 0;
    const auto hits = runHop(eng, p, iq, &wall);
    const auto s = eng.stats();
    std::printf("   hop: %d peaks, %d round(s), %d worker(s), floor %.1f dBFS, %.2f s wall, %.2f s CPU (%.0f %% of a core over the dwell)\n",
                s.peaks, s.rounds, s.workers, s.floorDb, wall, s.cpuSec, 100.0 * s.cpuSec / std::max(0.1, wall));
    auto find = [&](int khz) -> const FmHit* { for (const auto& h : hits) if (h.khz == khz) return &h; return nullptr; };
    for (const auto& S : st) {
        const int khz = (int)std::lround((centre + S.offHz) / 1e3);
        const FmHit* h = find(khz);
        char b[200];
        if (!S.rds) {
            std::snprintf(b, sizeof b, "%.1f MHz (no RDS): found as a peak, and NO PI (%s)", khz / 1e3, h ? (h->pi ? "pi!" : "none") : "not found");
            ok(h && h->pi == 0 && !fmConfirmed(h->groups, h->psComplete), b);
            continue;
        }
        std::snprintf(b, sizeof b, "%.1f MHz: PI %04X (want %04X), %d clean groups, PS \"%s\"%s, SNR %.0f dB", khz / 1e3,
                      h ? h->pi : 0, S.pi, h ? h->groups : 0, h ? h->ps.c_str() : "", h && h->psComplete ? " (complete)" : "", h ? h->snrDb : 0);
        ok(h && h->pi == S.pi && fmConfirmed(h->groups, h->psComplete) && trimRight(h->ps) == trimRight(S.ps), b);
    }
    int stray = 0; for (const auto& h : hits) { bool known = false; for (const auto& S : st) if (h.khz == (int)std::lround((centre + S.offHz) / 1e3)) known = true; if (!known && h.pi) ++stray; }
    ok(stray == 0, "no PI reported on a channel with no station (" + std::to_string(stray) + ")");
    ok(s.peaks >= 5 && s.peaks <= 8, "the peak-picker found the stations, not their skirts (" + std::to_string(s.peaks) + " peaks)");

    std::printf("── abort ──\n");
    {
        FmHopEngine e2;
        const auto h2 = runHop(e2, p, iq, nullptr, 1.0);
        ok(h2.empty(), "★ a hop cut short returns NOTHING (a listener arrived: the visit is never logged)");
    }

    std::printf("── rounds (a weak box's budget) ──\n");
    {
        FmHopEngine e3; FmHopEngine::Params q = p; q.peaksPerRound = 2; q.dwellSec = 3.0;
        double w3 = 0;
        const auto h3 = runHop(e3, q, iq, &w3);
        int ids = 0; for (const auto& h : h3) if (h.pi && fmConfirmed(h.groups, h.psComplete)) ++ids;
        ok(ids == 4 && e3.stats().rounds >= 3, "two peaks a round: every RDS station still identified, in " + std::to_string(e3.stats().rounds) + " rounds");
    }

    std::printf("── noise only ──\n");
    {
        const auto quiet = capture(fs, 2.0, {}, 0.002, 9);
        FmHopEngine e4;
        const auto h4 = runHop(e4, p, quiet);
        ok(h4.empty(), "a capture of noise has no peaks and no stations (" + std::to_string(h4.size()) + ")");
    }

    std::printf("── 50 kHz raster ──\n");
    {
        FmHopEngine e5; FmHopEngine::Params q = p; q.rasterKhz = 50;
        const auto h5 = runHop(e5, q, iq);
        int ids = 0; for (const auto& h : h5) if (h.pi && fmConfirmed(h.groups, h.psComplete)) ++ids;
        ok(ids == 4, "the 50 kHz raster finds the same four RDS stations (" + std::to_string(ids) + ")");
    }

    std::printf("── the Sony's 104 MHz hop (2026-11-14): BBC Northampton just below it, Horizon 150 kHz under the DC ──\n");
    {
        const double fs2 = 2.048e6, c2 = 104.85e6;   // the TV's rate; the 1400 kHz-IF plan's hop 104.3–105.4
        auto iq2 = capture(fs2, 5.0, { { -650e3, 0xCF11, "BBC Nhtn", 0.30, true },     // 104.2: outside the hop, strong
                                       { -150e3, 0xC0BB, "Horizon ", 0.10, true } },   // 104.7
                           0.002, 11);
        for (auto& v : iq2) v += cf32(0.08f, 0.06f);   // ★ the RTL's DC spike at the hop's centre, as strong as a station (an RTL at scan gain)
        FmHopEngine e6; FmHopEngine::Params q;
        q.rateHz = fs2; q.centreHz = c2; q.dcHz = c2; q.rasterKhz = 100; q.loHz = 104.3e6; q.hiHz = 105.4e6; q.dwellSec = 4.0;
        const auto h6 = runHop(e6, q, iq2);
        const FmHit* hz = nullptr; bool edge = false, dc = false;
        for (const auto& h : h6) { if (h.khz == 104700) hz = &h; if (h.khz == 104300) edge = true; if (h.khz == 104800) dc = true; }
        ok(!edge, "★ no peak on the hop's first channel (104.3) from BBC 104.2's skirt below it");
        ok(!dc, "★ no peak on 104.8 from the capture's DC spike");
        ok(hz && hz->pi == 0xC0BB && fmConfirmed(hz->groups, hz->psComplete), "★ Horizon is found on 104.7 and named (C0BB)");
    }

    std::printf("── weak stereo stations beside a strong one (the G35's 107.25 hop, 2026-11-16) ──\n");
    {
        // NLive 106.9 strong with RDS; 107.4 and 107.8 weak, pilot only (no RDS); 107.6 weak too — on 106.9's mirror about the DC
        const double c3 = 107.25e6;
        const auto iq3 = capture(fs, 9.0, { { -350e3, 0xCA89, "NLive   ", 0.30, true },
                                            { +150e3, 0x0001, "        ", 0.0025, false },   // ~10 dB channel SNR
                                            { +350e3, 0x0002, "        ", 0.0020, false },
                                            { +550e3, 0x0003, "        ", 0.0016, false } },   // ~6 dB: under the peak bar
                                 0.002, 13);
        FmHopEngine e7; FmHopEngine::Params q;
        q.rateHz = fs; q.centreHz = c3; q.dcHz = c3; q.rasterKhz = 100; q.loHz = 106.7e6; q.hiHz = 107.8e6; q.dwellSec = 4.0;
        const auto h7 = runHop(e7, q, iq3);
        auto at = [&](int khz) -> const FmHit* { for (const auto& h : h7) if (h.khz == khz) return &h; return nullptr; };
        for (int k : { 106900, 107400, 107600, 107800 }) {
            const FmHit* h = at(k);
            std::printf("      %.1f MHz: %s\n", k / 1e3, h ? (std::string("SNR ") + std::to_string((int)std::lround(h->snrDb)) + " dB, pilot " + (h->pilot ? "locked" : "no")
                                                           + (h->pi ? ", PI" : "")).c_str() : "not reported");
        }
        const FmHit* w4 = at(107400); const FmHit* w8 = at(107800);
        ok(at(106900) && at(106900)->pi == 0xCA89, "NLive 106.9 identified");
        ok(w4 && w4->pilot && w4->snrDb < 12.0f, "★ 107.4: a weak pilot-only station (under the old 12 dB bar) is reported with its pilot locked");
        ok(w8 && w8->pilot && w8->snrDb < q.minSnrDb, "★ 107.8: under the peak bar too — a WEAK CANDIDATE given a dwell, pilot locked");
        ok(e7.stats().weak >= 1, "the weak candidates are counted (" + std::to_string(e7.stats().weak) + ")");
        // …and through the log
        Log lg(T0dx());
        lg.commitFm(106700, 107800, 107250, 100, h7, T0dx() + 86400 * 2);
        const Entry* e4 = lg.findKey(fmKey(0, 107400));
        const Entry* e8 = lg.findKey(fmKey(0, 107800));
        const Entry* e6 = lg.findKey(fmKey(0, 107600));
        ok(e4 && e4->stereo && e4->cls != Cls::Suspect, "★ 107.4 is LOGGED (no RDS, stereo), not suspect");
        ok(e8 && e8->stereo && e8->cls != Cls::Suspect, "★ 107.8 is LOGGED (no RDS, stereo), not suspect");
        ok(!e6 || e6->cls == Cls::Suspect, "107.6, on 106.9's mirror about the hop's DC and far weaker, is kept SUSPECT (the image rule unchanged)"
                                           + std::string(e6 ? " — " + e6->suspectWhy : " — not reported"));
        // a weak candidate of plain noise is never reported
        const auto quiet = capture(fs, 6.0, {}, 0.002, 17);
        FmHopEngine e8n; const auto hq = runHop(e8n, q, quiet);
        ok(hq.empty(), "noise only: no weak candidate is reported (" + std::to_string(hq.size()) + ")");
    }

    std::printf("── OIRT: a 30 kHz raster hop (2026-11-16) ──\n");
    {
        // two OIRT stations, 69.80 (strong) and 70.31 (weaker), in a hop centred half a raster off: 69.995
        const double c4 = 69.995e6;
        const auto iq4 = capture(fs, 5.0, { { -195e3, 0xE201, "RADIO 1 ", 0.10, true }, { +315e3, 0xE202, "MAYAK   ", 0.02, true } }, 0.002, 19);
        FmHopEngine e9; FmHopEngine::Params q;
        q.rateHz = fs; q.centreHz = c4; q.dcHz = c4; q.rasterKhz = 30; q.loHz = 69.38e6; q.hiHz = 70.58e6; q.dwellSec = 4.0;
        const auto h9 = runHop(e9, q, iq4);
        const FmHit* a1 = nullptr; const FmHit* a2 = nullptr; int others = 0;
        for (const auto& h : h9) { if (h.khz == 69800) a1 = &h; else if (h.khz == 70310) a2 = &h; else ++others; }
        ok(a1 && a1->pi == 0xE201 && fmConfirmed(a1->groups, a1->psComplete), "★ 69.80 found on the 30 kHz raster and identified");
        ok(a2 && a2->pi == 0xE202 && fmConfirmed(a2->groups, a2->psComplete), "★ 70.31 too");
        ok(others == 0, "★ each station is ONE peak, not one per 30 kHz channel across its width (" + std::to_string(others) + " others)");
    }

    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
