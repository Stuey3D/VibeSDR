// dab-unitlog.cpp — run a captured multiplex (bench-clip .vbu8 layout: "VIBEBU81", double rate, double centre, uint32 sid,
// then u8 I/Q) through the REAL DabService in real time and write a UNIT LOG of one DAB+ service: every access-unit slot
// of every super frame on the programme clock, CRC-failed slots marked, lost super frames left as gaps. test-dab-faad
// replays the log through each decoder, so FFmpeg and FAAD2 are compared on exactly the units — and exactly the errors —
// the receiver produced off air. V12, 2026-10-26.
//   dab-unitlog <clip.vbu8> list                      — the services on the multiplex (after the FIC has been read)
//   dab-unitlog <clip.vbu8> <sid hex> <out.dabu>      — the log for one service, the whole clip
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
    const uint32_t sid = list ? 0 : uint32_t(std::strtoul(argv[2], nullptr, 16));
    FILE* out = nullptr;
    if (!list) { if (argc < 4 || !(out = std::fopen(argv[3], "wb"))) { std::printf("cannot write the log\n"); return 2; } std::fwrite("VIBEDABU", 1, 8, out); }

    vibedab::DabService svc;
    svc.setRfRate(rate); svc.setRfCentre(centre);
    for (int i = 0; i < int(vibedab::kBandIIICount); ++i)
        if (std::fabs(double(vibedab::kBandIII[i].centreHz) - centre) < 1000.0) { svc.setChannel(i); break; }
    std::mutex m; uint64_t slots = 0, bad = 0; int64_t sfFirst = -1, sfLast = -1;
    if (!list) {
        svc.setService(sid);
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
        svc.takePcm(pcm.data(), pcm.size() / 2);
        std::this_thread::sleep_until(t0 + microseconds((long long)(fed * 1e6)));
        if (list && fed > (std::getenv("VIBE_LIST_SECS") ? std::atof(std::getenv("VIBE_LIST_SECS")) : 12.0)) break;
    }
    std::fclose(fp);
    std::this_thread::sleep_for(milliseconds(500));
    if (std::getenv("VIBE_UNITLOG_JSON")) { std::string j; for (int i = 0; i < 50 && !svc.jsonTry(j); ++i) std::this_thread::sleep_for(milliseconds(20)); std::printf("%s\n", j.c_str()); }
    if (list) { for (auto& r : svc.learnable()) std::printf("%04X  %s\n", r.sid, r.label.c_str()); return 0; }
    { std::lock_guard<std::mutex> lk(m); svc.onAuSlotForTest = nullptr; }
    std::fclose(out);
    std::printf("%04X: %.1f s fed, %llu unit slots, %llu failed their CRC, super frames %lld..%lld (%lld due)\n",
                sid, fed, (unsigned long long)slots, (unsigned long long)bad, (long long)sfFirst, (long long)sfLast,
                (long long)(sfLast - sfFirst + 1));
    return 0;
}
