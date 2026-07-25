// VibeSDR — offline IQ probe: run the REAL pipeline over a recorded capture.
//
// The app's own recorder saves DECODED AUDIO, which is useless for diagnosing RDS: the
// subcarrier lives at 57 kHz in the MPX and is thrown away by the 15 kHz audio filter
// long before anything is written. To debug a real station you need the IQ.
//
// Capture (VibeServer must be closed — it holds the dongle):
//   rtl_sdr -f 90100000 -s 1024000 -g 30 -n 20480000 cap.iq      # ~20 s
//   (-g 0 selects the tuner's automatic gain; try several values, that is the point)
//
// Analyse:
//   ./build/vibedsp_iq_probe cap.iq 1024000
//
// Reports pilot lock, stereo blend, and every RDS group as it arrives, so a station that
// gives "no RDS" can be told apart from a station whose RDS simply is not decodable.
#include "../vibedsp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace vibedsp;

struct Cap {
    long audioFrames = 0;
    int  psCalls = 0, rtCalls = 0;
    uint16_t pi = 0;
    char ps[9] = {0};
    char rt[65] = {0};
    double firstPsSec = -1.0, firstRtSec = -1.0;
    double nowSec = 0.0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <cap.iq> [sampleRate=1024000] [freqOffsetHz=0]\n", argv[0]);
        std::printf("  cap.iq is rtl_sdr's raw format: interleaved unsigned 8-bit I/Q.\n");
        return 2;
    }
    const char* path = argv[1];
    const double fs  = (argc > 2) ? std::atof(argv[2]) : 1024000.0;
    const double off = (argc > 3) ? std::atof(argv[3]) : 0.0;

    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::printf("\n%s: %.1f MB = %.1f s at %.3f MSPS\n\n",
                path, bytes / 1e6, bytes / 2.0 / fs, fs / 1e6);

    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb;
    cb.ctx = &cap;
    cb.audio = [](void* c, const float*, int n, int, int) { ((Cap*)c)->audioFrames += n; };
    cb.stereo = [](void* c, bool lk) {
        std::printf("  [%6.2fs] stereo pilot %s\n", ((Cap*)c)->nowSec, lk ? "LOCKED" : "lost");
    };
    cb.rdsPs = [](void* c, uint16_t pi, const char* ps) {
        auto* x = (Cap*)c;
        if (x->firstPsSec < 0) { x->firstPsSec = x->nowSec;
            std::printf("  [%6.2fs] first PS  PI=0x%04X \"%s\"\n", x->nowSec, pi, ps); }
        x->pi = pi; std::strncpy(x->ps, ps, 8); x->psCalls++;
    };
    cb.rdsText = [](void* c, const char* rt) {
        auto* x = (Cap*)c;
        if (x->firstRtSec < 0) { x->firstRtSec = x->nowSec;
            std::printf("  [%6.2fs] first RT  \"%s\"\n", x->nowSec, rt); }
        std::strncpy(x->rt, rt, 64); x->rtCalls++;
    };

    pipe.start(fs, 1024, 10.0, 48000, cb);
    pipe.setTune(off, RxPipeline::Mode::WFM, 200000.0);

    const int block = 65536;
    std::vector<uint8_t> raw(block * 2);
    std::vector<cf32> iq(block);
    long long done = 0;
    size_t got;
    while ((got = std::fread(raw.data(), 1, raw.size(), f)) >= 2) {
        const int n = (int)(got / 2);
        // rtl_sdr writes unsigned 8-bit centred on 127.5.
        for (int i = 0; i < n; ++i)
            iq[i] = cf32((raw[2*i]     - 127.5f) / 127.5f,
                         (raw[2*i + 1] - 127.5f) / 127.5f);
        pipe.feed(iq.data(), n);
        done += n;
        cap.nowSec = (double)done / fs;
    }
    std::fclose(f);

    std::printf("\n  audio produced   : %.1f s\n", cap.audioFrames / 48000.0);
    std::printf("  pilot lock amp   : %.4f  (engage 0.060 / release 0.035)\n", pipe.pilotLockAmp());
    std::printf("  stereo blend     : %.2f\n", pipe.stereoBlend());
    std::printf("  RDS PS callbacks : %d%s\n", cap.psCalls,
                cap.firstPsSec >= 0 ? "" : "   <-- NO RDS AT ALL");
    if (cap.psCalls) std::printf("  PI / PS          : 0x%04X \"%s\"  (first at %.2fs)\n",
                                 cap.pi, cap.ps, cap.firstPsSec);
    std::printf("  RDS RT callbacks : %d\n", cap.rtCalls);
    if (cap.rtCalls) std::printf("  RadioText        : \"%s\"  (first at %.2fs)\n",
                                 cap.rt, cap.firstRtSec);
    std::printf("\n");
    return 0;
}
