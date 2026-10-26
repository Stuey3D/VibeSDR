// js8_wav — run VibeSDR's JS8 slot decoder (decoders/js8_decoder.cpp) over a recording, slot by slot, every submode.
//
//   js8_wav FILE.wav START_UTC [--sub ABCE] [--passes N] [--osd N] [--cands N] [--minscore N] [--iters N] [--coherent N]
//     START_UTC  UTC (unix seconds, fractional) of the file's first sample (ubersdr-rec prints it as start_utc=)
//     --sub      the submodes to run, by JS8Call's letters (A Normal, B Fast, C Turbo, E Slow); default all four
//   Output: one line per decode "D <slot> <letter> <pass> <snr> <dt> <hz> <kind> <message>", and on stderr each
//   submode's slots and mean / worst ms per slot.
//   Build: tools/js8-bench/build.sh (-O2, like the server).
#include "decoders/js8_decoder.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vibe;

static bool readWav(const char* path, std::vector<int16_t>& out, int& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::fclose(f); return false; }
    int ch = 1, bits = 16; rate = 0;
    for (;;) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(sz); if (std::fread(b.data(), 1, sz, f) != sz) break;
            ch = b[2] | b[3] << 8; rate = (int)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24); bits = b[14] | b[15] << 8;
        } else if (!std::memcmp(id, "data", 4)) {
            if (ch != 1 || bits != 16) { std::fclose(f); return false; }
            // a recorder that never finalised its header writes 0 or 0xFFFFFFFF: read to the end
            std::vector<int16_t> buf(1 << 16); size_t got;
            while ((got = std::fread(buf.data(), 2, buf.size(), f)) > 0) out.insert(out.end(), buf.begin(), buf.begin() + (long)got);
            std::fclose(f);
            return rate > 0;
        } else {
            std::fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    std::fclose(f);
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: js8_wav FILE.wav START_UTC [options]\n"); return 2; }
    std::vector<int16_t> pcm; int rate = 0;
    if (!readWav(argv[1], pcm, rate)) { std::fprintf(stderr, "cannot read %s (mono s16 WAV)\n", argv[1]); return 2; }
    const double fileStart = std::atof(argv[2]);
    std::string subs = "ABCE";
    Js8SlotOptions o; o.maxPasses = 2; o.osdMaxErrors = 26; o.coherentMax = 40;   // the live decoder's full depth
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto nxt = [&] { return i + 1 < argc ? std::atof(argv[++i]) : 0.0; };
        if (a == "--sub" && i + 1 < argc) subs = argv[++i];
        else if (a == "--passes") o.maxPasses = (int)nxt();
        else if (a == "--osd") o.osdMaxErrors = (int)nxt();
        else if (a == "--cands") o.maxCandidates = (int)nxt();
        else if (a == "--minscore") o.minScore = (int)nxt();
        else if (a == "--iters") o.ldpcIters = (int)nxt();
        else if (a == "--coherent") o.coherentMax = (int)nxt();
    }
    const double fileEnd = fileStart + (double)pcm.size() / rate;
    for (Js8Submode s : kJs8Submodes) {
        const Js8SubmodeInfo& in = js8Info(s);
        if (subs.find(in.letter) == std::string::npos) continue;
        Js8SlotDecoder dec(rate, s);
        const int win = dec.windowSamples();
        std::vector<float> w((size_t)win);
        double tot = 0, worst = 0; int slots = 0, decodes = 0;
        double stg[5] = {0, 0, 0, 0, 0}, coh = 0; long cands = 0, osdN = 0, cohN = 0, cohD = 0;
        for (double S = std::ceil(fileStart / in.period) * in.period; S + (double)win / rate <= fileEnd; S += in.period) {
            const long i0 = std::lround((S - fileStart) * rate);
            for (int i = 0; i < win; ++i) w[(size_t)i] = pcm[(size_t)(i0 + i)] / 32768.0f;
            std::vector<Js8Result> res; Js8SlotStats st;
            const auto t0 = std::chrono::steady_clock::now();
            dec.decode(w.data(), win, o, res, st);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            tot += ms; worst = std::max(worst, ms); ++slots;
            coh += st.coherentMs; cohN += st.coherentTried[0] + st.coherentTried[1]; cohD += st.coherentDecodes[0] + st.coherentDecodes[1];
            stg[0] += st.waterfallMs; stg[1] += st.searchMs; stg[2] += st.ldpcMs; stg[3] += st.osdMs; stg[4] += st.subtractMs;
            cands += st.candidates[0] + st.candidates[1]; osdN += st.osdTried[0] + st.osdTried[1];
            for (const Js8Result& r : res) {
                ++decodes;
                std::printf("D %.1f %c %d%s %d %.2f %.1f %s %s%s\n", S, in.letter, r.pass, r.osd ? "o" : "", r.snr, r.dt, r.hz,
                            js8::frameKindName(r.frame.kind), r.frame.textKnown ? r.frame.text.c_str() : "[compressed text]",
                            (r.frame.flags & js8::kFlagLast) ? " <EOM>" : "");
            }
        }
        const double k = slots ? 1.0 / slots : 0.0;
        std::fprintf(stderr, "%-6s %4d slots, %4d decodes, %.1f ms/slot mean, %.1f worst | waterfall %.2f search %.2f "
                     "bp %.2f coherent %.2f osd %.2f subtract %.2f | cands %.0f coherent-tried %.0f osd-tried %.0f | coherent decodes %ld\n",
                     in.name, slots, decodes, tot * k, worst, stg[0] * k, stg[1] * k, stg[2] * k, coh * k, stg[3] * k, stg[4] * k,
                     cands * k, cohN * k, osdN * k, cohD);
    }
    return 0;
}
