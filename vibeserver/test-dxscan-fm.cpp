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

    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
