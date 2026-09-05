// dab-offline.cpp — replay a CAPTURED multiplex through the real receiver, off the air.
//
// ★★★ WHY THIS EXISTS. Every wrong theory about the DAB breakup was overturned by real captured
//     data and not one by reasoning, and each hypothesis cost a ten-minute deploy to a live
//     receiver. This runs the SAME chain over the SAME samples in seconds, so the three stages
//     that can still be at fault — the 16-CIF time deinterleaver, the UEP depuncturing and CU
//     extraction — can be changed and measured without touching anything on air.
//
// ★★ It is NOT a substitute for the radio. It is a way to stop guessing between visits to it.
//
// Input: the VIBEIQ16 files written by VIBE_IQ_DUMP (see vibe_dab_service.h).
//   header: magic[8]="VIBEIQ16", double sampleRate, double centreHz, then int16 I/Q pairs.
#include "vibe_dab_receiver.h"
#include "vibe_dab_resample.h"
#include "vibe_dab_mp2.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>

using namespace vibedab;

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: dab-offline <capture.raw> [sid]\n"); return 2; }
    FILE* fp = std::fopen(argv[1], "rb");
    if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    char magic[8] = {0}; double rate = 0, centre = 0;
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "VIBEIQ16", 8) != 0) {
        printf("not a VIBEIQ16 capture\n"); return 2;
    }
    if (std::fread(&rate, sizeof rate, 1, fp) != 1) return 2;
    if (std::fread(&centre, sizeof centre, 1, fp) != 1) return 2;
    printf("capture: %.0f MS/s, centre %.3f MHz\n", rate, centre / 1e6);

    DabReceiver rx;
    Resample24to2048 rs;
    Mp2Decoder mp2;
    const bool needResample = (rate > 2.2e6);

    /* ★★★ MIRROR THE LIVE workerLoop EXACTLY, or this harness measures a receiver nobody runs:
     *  accumulate to a whole frame, push, tell the sync what we consumed (or throw it away, which
     *  is what the live code does today — that is the A/B), then drop one frame from the front. */
    const bool track = (argc > 2 && std::string(argv[2]) == "track");
    printf("sync mode: %s\n", track ? "TRACK (syncConsumed)" : "RESET (resetSync, as shipped)");
    std::vector<float> acc;
    std::vector<int16_t> raw(size_t(rate) * 2 / 10);      // 100 ms at a time
    std::vector<float>   fl, res;
    std::vector<Cplx>    iq;
    uint32_t sid = argc > 3 ? uint32_t(atoi(argv[3])) : 0;
    bool selected = false;
    int mp2In = 0, mp2Bad = 0, mp2Out = 0;
    long long blocks = 0;

    while (true) {
        const size_t got = std::fread(raw.data(), sizeof(int16_t), raw.size(), fp);
        if (got < 2) break;
        fl.resize(got);
        for (size_t i = 0; i < got; ++i) fl[i] = float(raw[i]) / 32767.0f;
        const std::vector<float>* use = &fl;
        // ★ process() APPENDS — clear it, or the buffer grows quadratically and the harness
        //   measures a receiver fed the same samples over and over. (Cost me one run.)
        if (needResample) { res.clear(); rs.process(fl.data(), got / 2, res); use = &res; }
        acc.insert(acc.end(), use->begin(), use->end());
        /* ★ TWO frames in the window, ONE consumed — exactly as workerLoop does. The frame start
         *  may land anywhere in the first half and a whole frame must still follow it. */
        const size_t one  = size_t(modeI().frameSamples);
        const size_t need = one * 2;
        while (acc.size() / 2 >= need) {
            iq.resize(need);
            for (size_t i = 0; i < need; ++i) iq[i] = Cplx{ acc[2*i], acc[2*i+1] };
            rx.push(iq.data(), need);
            ++blocks;
            if (track) rx.syncConsumed(one); else rx.resetSync();
            acc.erase(acc.begin(), acc.begin() + long(one * 2));

        if (!selected) {
            const Ensemble& e = rx.ensemble();
            if (!e.services.empty()) {
                for (const auto& kv : e.services) {
                    if (sid && kv.second.sid != sid) continue;
                    if (kv.second.label.empty()) continue;
                    for (const auto& c : kv.second.components) {
                        printf("service %u %-18s scType %d subch %d\n",
                               unsigned(kv.second.sid), kv.second.label.c_str(), c.scType, c.subChId);
                    }
                    if (!sid) sid = kv.second.sid;
                    break;
                }
                if (sid && rx.selectService(sid)) {
                    selected = true;
                    printf("selected sid %u — bitrate %d, %s\n", unsigned(sid), rx.serviceBitrate(),
                           rx.uepProf().valid ? "UEP" : (rx.profile().valid ? "EEP" : "no profile"));
                }
            }
        }
        if (selected) {
            for (const auto& f : rx.audioFrames()) {
                ++mp2In;
                std::vector<float> out;
                if (mp2.decode(f.data(), f.size(), out) <= 0) ++mp2Bad; else ++mp2Out;
            }
        }
        }   // while (acc >= one frame)
    }
    const DabStats& s = rx.stats();
    printf("\npushes %lld  frames %d  locked %s  FIB %d/%d = %.4f  offset %.0f Hz (%.2f ppm) carrierShift %d\n",
           blocks, s.framesSeen, s.locked ? "yes" : "no", s.fibsOk, s.fibsTotal, s.fibRate,
           s.freqOffsetHz, s.freqOffsetPpm, s.intOffsetCarriers);
    printf("MP2: in %d  bad %d (%.1f%%)  out %d\n",
           mp2In, mp2Bad, mp2In ? 100.0 * mp2Bad / mp2In : 0.0, mp2Out);
    std::fclose(fp);
    return 0;
}
