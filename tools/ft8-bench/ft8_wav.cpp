// ft8_wav — run VibeSDR's FT8/FT4 slot decoder (decoders/ft8_decoder.cpp) over a recording, slot by slot.
//
// ★ WHY (2026-10-05): every change to the FT8 decoder is judged on REAL audio against a WSJT-X-quality reference
//   (UberSDR's jt9 on the same receiver, same hour), not on a synthetic band — and on its CPU per slot, because the
//   decoder shares a Pi 2 with the radio. This drives the same Ft8SlotDecoder the server runs, with the same options.
//
//   ft8_wav FILE.wav START_UTC [--ft4] [--passes N] [--iters N] [--cands N] [--minscore N] [--lead S] [--len S]
//                              [--from UTC] [--to UTC]
//     START_UTC  UTC (unix seconds, fractional) of the file's first sample.
//     --lead     window start relative to the slot boundary (default: the decoder's own leadSec)
//     --len      seconds of the window to decode (default: the whole window) — --lead 0.8 --len 14.6 is the OLD decoder
//   Output: one line per decode "D <slot> <pass> <snr> <dt> <hz> <message>", one per slot
//           "S <slot> <passes> <ms1> <ms2> <ms3> <n1> <n2> <n3>".
//
//   Build: tools/ft8-bench/build.sh (-O2, like the server).
#include "decoders/ft8_decoder.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "ft8/message.h"
}

using namespace vibe;

// ★ The decoder's own shared table, so a hashed call resolves here exactly as it would in the server.
static bool htLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* call) {
    const int bits = type == FTX_CALLSIGN_HASH_10_BITS ? 10 : type == FTX_CALLSIGN_HASH_12_BITS ? 12 : 22;
    return ft8CallHashes().lookup(bits, hash, call);
}
static void htSave(const char* call, uint32_t n22) { ft8CallHashes().save(call, n22); }
static ftx_callsign_hash_interface_t g_if = { htLookup, htSave };

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
    if (argc < 3) { std::fprintf(stderr, "usage: ft8_wav FILE.wav START_UTC [options]\n"); return 2; }
    std::vector<int16_t> pcm; int rate = 0;
    if (!readWav(argv[1], pcm, rate)) { std::fprintf(stderr, "cannot read %s (mono s16 WAV)\n", argv[1]); return 2; }
    const double fileStart = std::atof(argv[2]);
    bool ft4 = false; double lead = NAN, len = 0, from = 0, to = 1e18;
    Ft8SlotOptions o;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto nxt = [&] { return i + 1 < argc ? std::atof(argv[++i]) : 0.0; };
        if (a == "--ft4") ft4 = true;
        else if (a == "--passes") o.maxPasses = (int)nxt();
        else if (a == "--iters") o.ldpcIters = (int)nxt();
        else if (a == "--cands") o.maxCandidates = (int)nxt();
        else if (a == "--minscore") o.minScore = (int)nxt();
        else if (a == "--osd") o.osdMaxErrors = (int)nxt();
        else if (a == "--osdscore") o.osdMinScore = (int)nxt();
        else if (a == "--lead") lead = nxt();
        else if (a == "--len") len = nxt();
        else if (a == "--from") from = nxt();
        else if (a == "--to") to = nxt();
    }
    Ft8SlotDecoder dec(rate, ft4);
    if (std::isnan(lead)) lead = Ft8SlotDecoder::leadSec(ft4);
    const double period = ft4 ? 7.5 : 15.0;
    const int win = dec.windowSamples();
    const int use = len > 0 ? std::min(win, (int)std::lround(len * rate)) : win;
    std::vector<float> w((size_t)win);
    const double fileEnd = fileStart + (double)pcm.size() / rate;
    double totMs = 0; int slots = 0;
    for (double S = std::ceil((fileStart - lead) / period) * period; S + lead + (double)use / rate <= fileEnd; S += period) {
        if (S < from || S >= to) continue;
        const long i0 = std::lround((S + lead - fileStart) * rate);
        std::fill(w.begin(), w.end(), 0.0f);
        for (int i = 0; i < use; ++i) w[(size_t)i] = pcm[(size_t)(i0 + i)] / 32768.0f;
        std::vector<Ft8Result> res; Ft8SlotStats st;
        dec.decode(w.data(), win, o, res, st);
        for (const Ft8Result& r : res) {
            char text[64] = {};
            ftx_message_t m = r.msg;
            ftx_message_offsets_t offs;
            if (ftx_message_decode(&m, &g_if, text, &offs) != FTX_MESSAGE_RC_OK) std::snprintf(text, sizeof text, "?");
            std::printf("D %.1f %d%s %d %.2f %.1f %s\n", S, r.pass, r.osd ? "o" : "", r.snr, r.dt + (lead - Ft8SlotDecoder::leadSec(ft4)), r.hz, text);
        }
        std::printf("S %.1f %d %.2f %.2f %.2f %d %d %d\n", S, st.passes, st.passMs[0], st.passMs[1], st.passMs[2],
                    st.passDecodes[0], st.passDecodes[1], st.passDecodes[2]);
        totMs += st.passMs[0] + st.passMs[1] + st.passMs[2]; ++slots;
    }
    std::fprintf(stderr, "%d slots, %.1f ms/slot mean\n", slots, slots ? totMs / slots : 0.0);
    return 0;
}
