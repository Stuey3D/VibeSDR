// dab-unitlog.cpp — run a captured multiplex (bench-clip .vbu8 layout: "VIBEBU81", double rate, double centre, uint32 sid,
// then u8 I/Q) through the REAL DabService in real time and write a UNIT LOG of one DAB+ service: every access-unit slot
// of every super frame on the programme clock, CRC-failed slots marked, lost super frames left as gaps. test-dab-faad
// replays the log through each decoder, so FFmpeg and FAAD2 are compared on exactly the units — and exactly the errors —
// the receiver produced off air. V12, 2026-10-26.
//   dab-unitlog <clip.vbu8> list                      — the services on the multiplex (after the FIC has been read)
//   dab-unitlog <clip.vbu8> <sid hex> <out.dabu>      — the log for one service, the whole clip
//   dab-unitlog <clip.vbu8> <sid hex> --pcm <out.wav> — what the SERVICE delivers (takePcm, 48 kHz stereo) — the stream a
//                                                       listener gets, whichever decoder VIBE_DAB_AAC / the switch picks;
//                                                       VIBE_UNITLOG_SWITCH_AT=<s> flips the owner's switch at that second
// Log: magic "VIBEDABU", then per slot: uint32 sf, uint8 slot, uint8 slots, uint8 ps, uint8 pad, uint16 len, ADTS bytes.
// ★ The clip is STREAMED (u8 → float per 50 ms block): a 60 s 2.4 MS/s capture is 1.15 GB as float.
// ★★ Real broadcasts are other people's programmes: logs made from them live in ~/VibeSDR-test-audio/dab/ only.
#include "vibe_dab_service.h"
#include "vibe_dab_channels.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: dab-unitlog <clip.vbu8> list | <sid hex> <out.dabu>\n"); return 2; }
    FILE* fp = std::fopen(argv[1], "rb");
    char magic[8]; double rate = 0, centre = 0; uint32_t sid0 = 0;
    if (!fp || std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "VIBEBU81", 8) != 0 || std::fread(&rate, 8, 1, fp) != 1
        || std::fread(&centre, 8, 1, fp) != 1 || std::fread(&sid0, 4, 1, fp) != 1) { std::printf("not a VIBEBU81 clip\n"); return 2; }
    const bool list = std::string(argv[2]) == "list";
    const bool pcmMode = argc >= 5 && std::string(argv[3]) == "--pcm";
    const uint32_t sid = list ? 0 : uint32_t(std::strtoul(argv[2], nullptr, 16));
    FILE* out = nullptr;
    if (!list && !pcmMode) { if (argc < 4 || !(out = std::fopen(argv[3], "wb"))) { std::printf("cannot write the log\n"); return 2; } std::fwrite("VIBEDABU", 1, 8, out); }
    std::vector<float> heard;   // --pcm: everything takePcm returned

    vibedab::DabService svc;
    svc.setRfRate(rate); svc.setRfCentre(centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - centre) < 1000.0) { svc.setChannel(i); break; }
    std::mutex m; uint64_t slots = 0, bad = 0; int64_t sfFirst = -1, sfLast = -1;
    if (!list) svc.setService(sid);
    if (!list && !pcmMode) {
        svc.onAuSlotForTest = [&](uint64_t sf, int slot, int n, bool ps, const uint8_t* p, size_t len) {
            std::lock_guard<std::mutex> lk(m);
            const uint32_t s32 = uint32_t(sf); const uint8_t h[4] = { uint8_t(slot), uint8_t(n), uint8_t(ps ? 1 : 0), 0 };
            const uint16_t l16 = uint16_t(len);
            std::fwrite(&s32, 4, 1, out); std::fwrite(h, 1, 4, out); std::fwrite(&l16, 2, 1, out); if (len) std::fwrite(p, 1, len, out);
            ++slots; if (!len) ++bad; if (sfFirst < 0) sfFirst = int64_t(sf); sfLast = int64_t(sf);
        };
    }
    const size_t blk = size_t(rate / 20);              // 50 ms of complex samples
    std::vector<uint8_t> u(blk * 2); std::vector<float> f(blk * 2), pcm(48000);
    const auto t0 = steady_clock::now(); double fed = 0;
    while (std::fread(u.data(), 1, u.size(), fp) == u.size()) {
        for (size_t i = 0; i < u.size(); ++i) f[i] = (float(u[i]) - 127.5f) / 127.5f;
        svc.feed(f.data(), blk); fed += 0.05;
        { const size_t got = svc.takePcm(pcm.data(), pcm.size() / 2); if (pcmMode) heard.insert(heard.end(), pcm.begin(), pcm.begin() + long(2 * got)); }
        if (std::getenv("VIBE_UNITLOG_JSON") && std::fabs(fed - 10.0) < 0.026) { std::string j; svc.jsonTry(j); }   // ★ primes the CPU window
        if (const char* sw = std::getenv("VIBE_UNITLOG_SWITCH_AT")) {
            static bool flipped = false;
            if (!flipped && fed >= std::atof(sw)) { flipped = true; vibedab::dabAacChoice().store(1 - vibedab::dabAacChoice().load()); std::printf("   [%.1f s] switch flipped\n", fed); }
        }
        std::this_thread::sleep_until(t0 + microseconds((long long)(fed * 1e6)));
        if (list && fed > (std::getenv("VIBE_LIST_SECS") ? std::atof(std::getenv("VIBE_LIST_SECS")) : 12.0)) break;
    }
    std::fclose(fp);
    std::this_thread::sleep_for(milliseconds(500));
    if (std::getenv("VIBE_UNITLOG_JSON")) { std::string j; for (int i = 0; i < 50 && !svc.jsonTry(j); ++i) std::this_thread::sleep_for(milliseconds(20)); std::printf("%s\n", j.c_str()); }
    if (list) { for (auto& r : svc.learnable()) std::printf("%04X  %s\n", r.sid, r.label.c_str()); return 0; }
    if (pcmMode) {
        FILE* w = std::fopen(argv[4], "wb"); if (!w) return 2;
        const uint32_t n = uint32_t(heard.size()), data = n * 2, rate = 48000;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, w); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, w); };
        std::fwrite("RIFF", 1, 4, w); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, w); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
        std::fwrite("data", 1, 4, w); u32(data);
        for (float x : heard) { const float c = x > 1.f ? 1.f : (x < -1.f ? -1.f : x); u16(uint16_t(int16_t(std::lrint(c * 32767.f)))); }
        std::fclose(w);
        std::printf("%04X: %.1f s fed, %.1f s of audio delivered\n", sid, fed, double(heard.size() / 2) / 48000.0);
        return 0;
    }
    { std::lock_guard<std::mutex> lk(m); svc.onAuSlotForTest = nullptr; }
    std::fclose(out);
    std::printf("%04X: %.1f s fed, %llu unit slots, %llu failed their CRC, super frames %lld..%lld (%lld due)\n",
                sid, fed, (unsigned long long)slots, (unsigned long long)bad, (long long)sfFirst, (long long)sfLast,
                (long long)(sfLast - sfFirst + 1));
    return 0;
}
