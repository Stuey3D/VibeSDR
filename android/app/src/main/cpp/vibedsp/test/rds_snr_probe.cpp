// VibeSDR — RDS weak-signal probe (host only).
//
// The existing RDS tests use a clean, generously-modulated signal, so they answer
// "does the chain work" and NOT "how weak a station can it read". That gap matters:
// RDS rides at 57 kHz, the very top of the MPX, where FM's noise triangle is worst,
// so it fails long before the audio does. Reported on-air 2026-07-25: no RDS on a
// station at S9+12 that used to give it.
//
// This sweeps carrier-to-noise and reports, for each level, whether the PS name was
// recovered and how many group callbacks got through. Run the SAME binary against two
// checkouts to compare — that is the only way to tell a real regression from a signal
// that was always marginal.
//
// ★ RESULT 2026-07-25. Run against 1eb8752 (RDS at the full channel rate) and 723cef6
// (decimated): the threshold is IDENTICAL — both decode at noise 1.20 and both fail at
// 1.60. The decimation cost ~5% of the decoded groups at moderate noise and moved the
// limit not at all. So weak-signal RDS was always this marginal; it is not a regression.
//
// It IS poor in absolute terms though (no RDS at S9+12 where a car radio manages it), and
// the two reasons are visible in rds.cpp:
//   1. Detection is REAL-ONLY (mpx * ref57), so any phase error between our pilot-derived
//      57 kHz and the station's subcarrier scales the output by cos(theta) — zero at 90
//      degrees. Multipath rotates it. A complex I/Q demod with a phase estimate fixes it.
//   2. Block sync needs an EXACT syndrome match, so one bad bit discards the block. The
//      10-bit CRC can correct bursts up to 5 bits; that is standard and unimplemented.
// Either would raise the threshold. Keep this probe as the way to prove it.
//
//   ./build/vibedsp_rds_snr_probe
#include "../vibedsp.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibedsp;

static uint32_t encodeBlock(uint16_t data, int offsetIdx) {
    uint16_t cw = RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[offsetIdx];
    return ((uint32_t)data << 10) | cw;
}

struct Cap { uint16_t pi = 0; char ps[9] = {0}; int psCalls = 0; };

static std::vector<int> buildDiffBits(uint16_t PI, const char* PS, int reps) {
    std::vector<int> bits;
    for (int rep = 0; rep < reps; ++rep)
        for (int addr = 0; addr < 4; ++addr) {
            const uint16_t A = PI, B = (0 << 12) | (addr & 0x3), C = 0x1234;
            const uint16_t D = ((uint8_t)PS[addr * 2] << 8) | (uint8_t)PS[addr * 2 + 1];
            const uint32_t blk[4] = { encodeBlock(A,0), encodeBlock(B,1),
                                      encodeBlock(C,2), encodeBlock(D,4) };
            for (int b = 0; b < 4; ++b)
                for (int i = 25; i >= 0; --i) bits.push_back((blk[b] >> i) & 1);
        }
    std::vector<int> m(bits.size());
    int prev = 0;
    for (size_t k = 0; k < bits.size(); ++k) { m[k] = prev ^ bits[k]; prev = m[k]; }
    return m;
}

// One run at a given noise level. `noise` is the per-component Gaussian-ish sigma
// added to the IQ, with the carrier at unit amplitude.
static Cap runAt(double noise, double rdsLevel, bool stereoOn) {
    const uint16_t PI = 0xC79F;
    const char* PS = "RDSTEST!";
    auto m = buildDiffBits(PI, PS, 60);

    const double fs = 1920000.0, fc = 300000.0;
    const int Ni = 1 << 22;
    std::vector<cf32> iq(Ni);

    unsigned s = 22222;
    auto rnd = [&s]() {           // cheap sum-of-uniforms, near enough to Gaussian
        float a = 0.0f;
        for (int k = 0; k < 4; ++k) {
            s = s * 1664525u + 1013904223u;
            a += (float)((int)(s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }
        return a * 0.5f;
    };

    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double pilot = 0.1 * std::cos(2.0 * M_PI * 19000.0 * t);
        const double mono  = 0.2 * std::cos(2.0 * M_PI * 1000.0 * t);
        // A real stereo station puts the L-R difference on 38 kHz, which sits right
        // next to RDS in the MPX. Leaving it out makes RDS look easier than it is.
        const double diff  = stereoOn
            ? 0.2 * std::cos(2.0 * M_PI * 3000.0 * t) * std::sin(2.0 * M_PI * 38000.0 * t)
            : 0.0;
        const int k = (int)std::floor(t * 1187.5);
        double rds = 0.0;
        if (k >= 0 && k < (int)m.size()) {
            const double phInBit = (t * 1187.5 - k) * 2.0 * M_PI;
            const double manch = ((phInBit < M_PI) ? 1.0 : -1.0) * (m[k] ? 1.0 : -1.0);
            rds = rdsLevel * manch * std::cos(2.0 * M_PI * 57000.0 * t);
        }
        const double mpx = mono + pilot + diff + rds;
        ph += 2.0 * M_PI * (fc + 75000.0 * mpx) / fs;
        iq[i] = cf32((float)std::cos(ph) + (float)(rnd() * noise),
                     (float)std::sin(ph) + (float)(rnd() * noise));
    }

    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap;
    cb.rdsPs = [](void* c, uint16_t pi, const char* ps) {
        auto* p = (Cap*)c; p->pi = pi; std::strncpy(p->ps, ps, 8); p->psCalls++;
    };
    cb.audio = [](void*, const float*, int, int, int) {};
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fc, RxPipeline::Mode::WFM, 200000.0);
    for (int o = 0; o < Ni; o += 65536)
        pipe.feed(iq.data() + o, std::min(65536, Ni - o));
    return cap;
}

int main() {
    std::printf("\nRDS weak-signal probe — 2.18 s of WFM per point, PS=\"RDSTEST!\"\n");
    std::printf("stereo L-R present, RDS at 5%% MPX (a typical real injection level)\n\n");
    std::printf("  %-10s %-8s %-10s %s\n", "IQ noise", "PS ok?", "callbacks", "recovered");
    const double levels[] = { 0.0, 0.10, 0.30, 0.60, 0.90, 1.20, 1.60, 2.00 };
    for (double nz : levels) {
        const Cap c = runAt(nz, 0.05, true);
        const bool ok = (c.psCalls > 0 && std::strcmp(c.ps, "RDSTEST!") == 0);
        std::printf("  %-10.2f %-8s %-10d \"%s\"\n", nz, ok ? "yes" : "NO", c.psCalls, c.ps);
    }
    std::printf("\n");
    return 0;
}
