// test_spectrum_processor — the C++ SpectrumProcessor must draw what src/assets/signalProcessor.ts draws.
//   node gen_spectrum_vectors.mjs build && ./build/vibedsp_spectrum_processor_tests build
// Replays the TS run's inputs (spectrum_in.bin) and compares against its outputs (spectrum_out.bin):
// the waterfall bytes and the dB window EXACTLY; the trace and peak within float rounding (std::exp may differ
// from V8's by an ulp, and the EMA carries it).
#include "spectrum_processor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace vibedsp;

static std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Reader {
    const std::vector<char>& b; size_t o = 0;
    explicit Reader(const std::vector<char>& v) : b(v) {}
    bool more() const { return o < b.size(); }
    template <typename T> T get() { T v; std::memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T); return v; }
    const char* take(size_t n) { const char* p = b.data() + o; o += n; return p; }
};

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const auto in = slurp(dir + "/spectrum_in.bin");
    const auto out = slurp(dir + "/spectrum_out.bin");
    if (in.empty() || out.empty()) { std::printf("FAIL: no vectors in %s (run gen_spectrum_vectors.mjs)\n", dir.c_str()); return 1; }
    Reader ri(in), ro(out);
    SpectrumProcessor p;
    SpectrumProcessorSettings s;
    int frames = 0, rowDiff = 0, rangeDiff = 0, traceDiff = 0;
    double worstTrace = 0;
    while (ri.more()) {
        const uint8_t ev = ri.get<uint8_t>();
        if (ev == 2) {
            s.autoContrast = ri.get<double>(); s.minRangeDb = ri.get<double>();
            s.manual = ri.get<uint8_t>(); s.manualMinDb = ri.get<double>(); s.manualMaxDb = ri.get<double>();
            s.specFloor = ri.get<double>(); s.specPeakScale = ri.get<double>();
            s.smoothingFrames = ri.get<double>(); s.avgFrames = ri.get<double>();
            s.spatialSmooth = ri.get<uint8_t>(); s.peakHold = ri.get<uint8_t>();
            s.wfBrightness = ri.get<double>(); s.wfContrast = ri.get<double>(); s.wfSharpness = ri.get<double>();
            p.applySettings(s);
        } else if (ev == 3) {
            p.resetPeakHold();
        } else if (ev == 4) {
            p.reset();
        } else if (ev == 1) {
            const int n = ri.get<int32_t>();
            const double c = ri.get<double>(), bw = ri.get<double>();
            const bool inter = ri.get<uint8_t>();
            const double now = ri.get<double>();
            std::vector<float> bins(n);
            std::memcpy(bins.data(), ri.take(sizeof(float) * n), sizeof(float) * n);
            const SpectrumFrameOut r = p.process(bins.data(), n, c, bw, inter, now);

            const int en = ro.get<int32_t>();
            const double eMin = ro.get<double>(), eMax = ro.get<double>();
            const uint8_t* eRow = (const uint8_t*)ro.take(en);
            std::vector<float> eSpec(en), ePeak(en);
            std::memcpy(eSpec.data(), ro.take(4 * en), 4 * en);
            std::memcpy(ePeak.data(), ro.take(4 * en), 4 * en);
            if (en != n || r.n != n) { std::printf("FAIL frame %d: length %d vs %d\n", frames, r.n, en); return 1; }
            if (r.dbMin != eMin || r.dbMax != eMax) {
                if (rangeDiff++ < 5) std::printf("range diff frame %d: %.17g..%.17g vs %.17g..%.17g\n", frames, r.dbMin, r.dbMax, eMin, eMax);
            }
            int rd = 0;
            for (int i = 0; i < n; i++) if (r.row[i] != eRow[i]) rd++;
            if (rd) { if (rowDiff++ < 5) std::printf("row diff frame %d: %d bytes\n", frames, rd); }
            for (int i = 0; i < n; i++) {
                const double d1 = std::fabs((double)r.spec[i] - eSpec[i]), d2 = std::fabs((double)r.peak[i] - ePeak[i]);
                const double d = d1 > d2 ? d1 : d2;
                if (d > worstTrace) worstTrace = d;
                if (d > 1e-5) { if (traceDiff++ < 5) std::printf("trace diff frame %d bin %d: %g\n", frames, i, d); }
            }
            frames++;
        } else {
            std::printf("FAIL: bad event %d\n", ev); return 1;
        }
    }
    std::printf("%d frames · row diffs %d · range diffs %d · trace diffs %d (worst %.3g)\n",
                frames, rowDiff, rangeDiff, traceDiff, worstTrace);
    const bool ok = frames > 500 && !rowDiff && !rangeDiff && !traceDiff;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
