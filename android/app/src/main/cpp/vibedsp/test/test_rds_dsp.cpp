// VibeSDR V5 host test — full RDS chain through RxPipeline. Synthesises a WFM
// broadcast (pilot + mono tone + biphase RDS on 57 kHz) carrying a known PI/PS,
// FM-modulates it, runs the whole native pipeline, and checks the PS name is
// recovered. Exercises: PLL lock, coherent 57 kHz demod, biphase + differential
// decode, block sync, group-0A parsing. Self-consistent (encoder matches the
// decoder's conventions) — real-station validation is the on-device step.
#include "../vibedsp.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static uint32_t encodeBlock(uint16_t data, int offsetIdx) {
    uint16_t cw = RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[offsetIdx];
    return ((uint32_t)data << 10) | cw;
}

struct Cap { uint16_t pi = 0; char ps[9] = {0}; int psCalls = 0; };
static void onPs(void* c, uint16_t pi, const char* ps) {
    auto* p = (Cap*)c; p->pi = pi; std::strncpy(p->ps, ps, 8); p->psCalls++;
}

// Build the differentially-encoded bit stream for a PS over many 0A groups.
static std::vector<int> buildDiffBits(uint16_t PI, const char* PS, int reps) {
    std::vector<int> bits;
    for (int rep = 0; rep < reps; ++rep)
        for (int addr = 0; addr < 4; ++addr) {
            const uint16_t A = PI;
            const uint16_t B = (0 << 12) | (addr & 0x3);
            const uint16_t C = 0x1234;
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

// Direct RdsDemod unit test with IDEAL baseband (no PLL / FM), to isolate the
// DSP front-end from the rest of the pipeline.
static void testDirect() {
    std::printf("-- RdsDemod direct (ideal inputs) --\n");
    const double R = 640000.0;          // a typical WFM channel rate
    auto m = buildDiffBits(0xABCD, "DIRECT12", 20);
    const int N = (int)((m.size() + 4) / 1187.5 * R);
    std::vector<float> mpx(N), ref57(N), ref57q(N), bitClk(N);
    for (int i = 0; i < N; ++i) {
        const double t = i / R;
        const int k = (int)std::floor(t * 1187.5);
        double manch = 0.0;
        if (k >= 0 && k < (int)m.size()) {
            const double phInBit = (t * 1187.5 - k) * 2.0 * M_PI;
            manch = ((phInBit < M_PI) ? 1.0 : -1.0) * (m[k] ? 1.0 : -1.0);
        }
        const double car = std::cos(2.0 * M_PI * 57000.0 * t);
        mpx[i]    = (float)(0.05 * manch * car);
        ref57[i]  = (float)car;
        ref57q[i] = (float)std::sin(2.0 * M_PI * 57000.0 * t);
        bitClk[i] = (float)std::fmod(2.0 * M_PI * 1187.5 * t, 2.0 * M_PI);
    }
    Cap cap;
    RdsDemod demod;
    RdsDecoder::Callbacks cb; cb.ctx = &cap; cb.ps = onPs;
    demod.configure(R, cb);
    for (int o = 0; o < N; o += 8192)
        demod.process(mpx.data() + o, ref57.data() + o, ref57q.data() + o,
                      bitClk.data() + o, std::min(8192, N - o));
    std::printf("  direct PS=\"%s\" (calls=%d)\n", cap.ps, cap.psCalls);
    check(std::strcmp(cap.ps, "DIRECT12") == 0, "direct RdsDemod recovers PS");
}

int main() {
    std::printf("== vibedsp RDS-through-pipeline host test ==\n");
    testDirect();

    const uint16_t PI = 0xC79F;
    const char* PS = "RDSTEST!";

    // Build the transmitted (pre-differential) bit stream: many 0A groups.
    std::vector<int> bits;
    for (int rep = 0; rep < 20; ++rep) {
        for (int addr = 0; addr < 4; ++addr) {
            const uint16_t A = PI;
            const uint16_t B = (0 << 12) | (addr & 0x3);   // group 0A, segment addr
            const uint16_t C = 0x1234;
            const uint16_t D = ((uint8_t)PS[addr * 2] << 8) | (uint8_t)PS[addr * 2 + 1];
            const uint32_t blk[4] = { encodeBlock(A,0), encodeBlock(B,1),
                                      encodeBlock(C,2), encodeBlock(D,4) };
            for (int b = 0; b < 4; ++b)
                for (int i = 25; i >= 0; --i) bits.push_back((blk[b] >> i) & 1);
        }
    }
    // Differential encode: m[k] = m[k-1] ^ bits[k].
    std::vector<int> m(bits.size());
    int prev = 0;
    for (size_t k = 0; k < bits.size(); ++k) { m[k] = prev ^ bits[k]; prev = m[k]; }

    // Synthesise the WFM IQ.
    const double fs = 1920000.0, fc = 300000.0;
    const int Ni = 1 << 22;
    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double pilot = 0.1 * std::cos(2.0 * M_PI * 19000.0 * t);
        const double mono  = 0.2 * std::cos(2.0 * M_PI * 1000.0 * t);   // L+R audio
        const int k = (int)std::floor(t * 1187.5);
        double rds = 0.0;
        if (k >= 0 && k < (int)m.size()) {
            const double phInBit = (t * 1187.5 - k) * 2.0 * M_PI;
            const double manch = ((phInBit < M_PI) ? 1.0 : -1.0) * (m[k] ? 1.0 : -1.0);
            rds = 0.05 * manch * std::cos(2.0 * M_PI * 57000.0 * t);
        }
        const double mpx = mono + pilot + rds;
        ph += 2.0 * M_PI * (fc + 75000.0 * mpx) / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }

    struct Cap2 { Cap rds; bool stereo = false; long audio = 0; } cap2;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap2;
    cb.rdsPs = [](void* c, uint16_t pi, const char* ps){ onPs(&((Cap2*)c)->rds, pi, ps); };
    cb.stereo = [](void* c, bool lk){ ((Cap2*)c)->stereo = lk; };
    cb.audio = [](void* c, const float*, int frames, int, int){ ((Cap2*)c)->audio += frames; };
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fc, RxPipeline::Mode::WFM, 200000.0);
    for (int o = 0; o < Ni; o += 65536)
        pipe.feed(iq.data() + o, std::min(65536, Ni - o));
    std::printf("  [diag] pilot locked=%d, audio frames=%ld\n", (int)cap2.stereo, cap2.audio);
    Cap& cap = cap2.rds;

    std::printf("  PI=0x%04X  PS=\"%s\"  (ps callbacks=%d)\n", cap.pi, cap.ps, cap.psCalls);
    check(cap.psCalls > 0, "RDS PS callback fired through pipeline");
    check(cap.pi == PI, "PI recovered");
    check(std::strcmp(cap.ps, PS) == 0, "PS name recovered exactly");

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
