// drm-au-dump.cpp — runs a DRM IQ recording (48 kHz stereo-IQ .wav) through VibeServer's DRM receiver and writes every
// AAC access unit of service 0 exactly as DrmFaad hands it to FAAD2 (the super frame's CRC-8 byte, then the frame) to
// a .drmau file, so FAAD2's DRM build can be benchmarked and compared on its own (faad2-drm-bench.c), on any machine.
//   drm-au-dump <in.wav> <out.drmau>
// Build (from android/app/src/main/cpp): c++ -std=c++17 -O2 -I. -Ivibedsp ../../../../../vibeserver/faad2-drm-bench/drm-au-dump.cpp
//   decoders/drm/drm_receiver.cpp <every vibedsp/SOURCES .cpp> + the kissfft and pffft C objects (as scripts/run-tests.sh).
// .drmau: "DRMAU1\0\0", u32 core rate Hz, u8 DRMCH_* channel mode, 3 pad; then per unit u16 length (little-endian) +
// bytes. A change of audio configuration mid-recording ends the dump (the bench opens one decoder).
#include "decoders/drm/drm_audio.h"
#include "decoders/drm/drm_receiver.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace vibedrm;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: drm-au-dump <in.wav> <out.drmau>\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    uint8_t h[44]; if (std::fread(h, 1, 44, f) != 44) return 1;
    int rate; std::memcpy(&rate, h + 24, 4);
    Receiver rx; AudioFramer fr;
    std::vector<uint8_t> out; bool cfgd = false, stop = false; long units = 0, crcBad = 0;
    AudioInfo cur;
    rx.onLogicalFrame = [&](const LogicalFrame& lf) {
        if (stop) return;
        const ServiceSdc& sv = rx.status().sdc.svc[0];
        if (!sv.audio.present || sv.audio.streamId != lf.stream || sv.audio.coding != 0) return;
        if (fr.configure(sv.audio)) {
            if (cfgd) { stop = true; return; }
            cfgd = true; cur = sv.audio;
            // DRMCH_* (neaacdec.h): MONO 1, STEREO 2, SBR_MONO 3, SBR_STEREO 4, SBR_PS_STEREO 5 — as DrmFaad::open
            uint8_t ch = 1;
            if (cur.audioMode == 2) ch = cur.sbr ? 4 : 2;
            else if (cur.audioMode == 1) ch = 5;
            else ch = cur.sbr ? 3 : 1;
            const uint32_t r = uint32_t(cur.sampleRateHz());
            const char magic[8] = {'D', 'R', 'M', 'A', 'U', '1', 0, 0};
            out.insert(out.end(), magic, magic + 8);
            for (int i = 0; i < 4; ++i) out.push_back(uint8_t(r >> (8 * i)));
            out.push_back(ch); out.push_back(0); out.push_back(0); out.push_back(0);
            std::fprintf(stderr, "%s: %s %s, core %u Hz, DRMCH %d\n", argv[1], cur.codecName(), cur.modeName(), r, ch);
        }
        std::vector<AccessUnit> aus;
        fr.push(lf.bytes.data(), lf.bytes.size() - (sv.audio.text ? 4 : 0), aus);
        for (auto& au : aus) {
            if (au.data.empty()) continue;
            const size_t n = au.data.size() + 1;
            out.push_back(uint8_t(n)); out.push_back(uint8_t(n >> 8));
            out.push_back(au.crc);
            out.insert(out.end(), au.data.begin(), au.data.end());
            ++units; if (!au.crcOk) ++crcBad;
        }
    };
    std::vector<int16_t> b(9600); std::vector<cf> x(4800); size_t n;
    while ((n = std::fread(b.data(), 4, 4800, f)) > 0) {
        for (size_t i = 0; i < n; ++i) x[i] = cf(b[2 * i], b[2 * i + 1]);
        rx.feed(x.data(), int(n), rate);
    }
    std::fclose(f);
    {   const RxStatus& st = rx.status(); const AudioInfo& a = st.sdc.svc[0].audio;
        std::fprintf(stderr, "%s: FAC %lld/%lld SDC %lld/%lld, label \"%s\", audio present %d coding %d sbr %d mode %d sr %d, "
                     "SNR %.1f dB\n", argv[1], (long long)st.facGood, (long long)(st.facGood + st.facBad), (long long)st.sdcGood,
                     (long long)(st.sdcGood + st.sdcBad), st.sdc.svc[0].label.c_str(), a.present, a.coding, a.sbr, a.audioMode,
                     a.srCode, st.wmerDb); }
    if (!cfgd) { std::fprintf(stderr, "%s: no AAC service decoded\n", argv[1]); return 1; }
    FILE* o = std::fopen(argv[2], "wb");
    std::fwrite(out.data(), 1, out.size(), o); std::fclose(o);
    std::fprintf(stderr, "%s: %ld units (%ld failed their CRC-16)\n", argv[2], units, crcBad);
    return 0;
}
