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
#include <algorithm>

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
    std::vector<uint8_t> pend;
    std::vector<int> badIdx;            // ★ WHICH frames fail — runs and periods are the diagnosis
    std::vector<long> frameStarts;      // ★ and where each frame was found, to catch skips/dups
    std::vector<int>  fibOkPer, fibTotPer;   // ★ per-FRAME FIC health, to separate signal from us
    std::vector<float> nullDepth, prsCorr;
    std::vector<int>  frameOfMp2;            // which DAB frame each MP2 frame came from

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
            frameStarts.push_back(rx.lastFrameStart());
            fibOkPer.push_back(rx.stats().fibsOk);
            fibTotPer.push_back(rx.stats().fibsTotal);
            nullDepth.push_back(rx.stats().nullDepthDb);
            prsCorr.push_back(rx.stats().prsCorrelation);
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
                for (const auto& kv2 : rx.ensemble().subChannels) {
                    const auto& sc = kv2.second;
                    printf("  subch %2d: startCu %4d sizeCu %4d %s protLevel %d option %d\n",
                           sc.id, sc.startCu, sc.sizeCu, sc.eep ? "EEP" : "UEP",
                           sc.protLevel, sc.option);
                }
                if (sid && rx.selectService(sid)) {
                    selected = true;
                    printf("selected sid %u — bitrate %d, %s\n", unsigned(sid), rx.serviceBitrate(),
                           rx.uepProf().valid ? "UEP" : (rx.profile().valid ? "EEP" : "no profile"));
                }
            }
        }
        if (selected) {
            // ★ TAKE, do not index. audioFrames() is a VIEW of a bounded ring that is only
            //   cleared on service selection, so reading it after every push re-counts every
            //   frame still in it — 19056 "MP2 frames" out of 311 DAB frames (1244 CIFs). The
            //   live path calls takeAudioFrames(); a harness that does not is measuring itself.
            for (const auto& f0 : rx.takeAudioFrames()) {
                /* ★ LSF PAIRING TEST: a 24 kHz Layer II frame is 1152 samples = 48 ms = TWO DAB
                 *  logical frames, so the 192-byte chunks must be joined before decoding. */
                std::vector<uint8_t> f = f0;
                if (!pend.empty()) { pend.insert(pend.end(), f.begin(), f.end()); f = pend; pend.clear(); }
                else {
                    const Mp2Info hi = mp2Header(f.data(), f.size());
                    if (hi.valid && size_t(hi.frameBytes) > f.size()) { pend = f; continue; }
                }
                ++mp2In;
                std::vector<float> out;
                frameOfMp2.push_back(int(blocks) - 1);
                if (mp2.decode(f.data(), f.size(), out) <= 0) {
                    ++mp2Bad; badIdx.push_back(mp2In - 1);
                    if (mp2Bad <= 3) {          // ★ LOOK AT THE BYTES, do not infer from the config
                        printf("  bad frame %d: len %zu  bytes", mp2In - 1, f.size());
                        for (size_t k = 0; k < 6 && k < f.size(); ++k) printf(" %02X", f[k]);
                        if (f.size() >= 4) {
                            const unsigned h = (unsigned(f[0])<<24)|(unsigned(f[1])<<16)|(unsigned(f[2])<<8)|f[3];
                            printf("   sync %03X id %d layer %d prot %d brIdx %d srIdx %d mode %d",
                                   (h>>21)&0x7FF, (h>>19)&1, (h>>17)&3, (h>>16)&1,
                                   (h>>12)&0xF, (h>>10)&3, (h>>6)&3);
                        }
                        printf("\n");
                    }
                }
                else ++mp2Out;
            }
        }
        }   // while (acc >= one frame)
    }
    const DabStats& s = rx.stats();
    printf("\npushes %lld  frames %d  locked %s  FIB %d/%d = %.4f  offset %.0f Hz (%.2f ppm) carrierShift %d\n",
           blocks, s.framesSeen, s.locked ? "yes" : "no", s.fibsOk, s.fibsTotal, s.fibRate,
           s.freqOffsetHz, s.freqOffsetPpm, s.intOffsetCarriers);
    printf("erased frames: %d of %d\n", s.erasedFrames, s.framesSeen);
    printf("MP2: in %d  bad %d (%.1f%%)  out %d\n",
           mp2In, mp2Bad, mp2In ? 100.0 * mp2Bad / mp2In : 0.0, mp2Out);
    {   // ★ the whole ensemble as finally understood, so a service can be picked by hand
        printf("\nservices:\n");
        for (const auto& kv : rx.ensemble().services)
            for (const auto& c : kv.second.components)
                printf("  sid %5u  %-18s scType %2d  subch %d\n",
                       unsigned(kv.second.sid), kv.second.label.c_str(), c.scType, c.subChId);
    }
    // ── where the failures fall ─────────────────────────────────────────────
    if (!badIdx.empty()) {
        printf("\nbad-frame gaps (first 40): ");
        for (size_t i = 1; i < badIdx.size() && i < 41; ++i) printf("%d ", badIdx[i] - badIdx[i-1]);
        printf("\n");
        std::map<int,int> gapHist;
        for (size_t i = 1; i < badIdx.size(); ++i) gapHist[badIdx[i] - badIdx[i-1]]++;
        printf("gap histogram (gap:count, top 12): ");
        std::vector<std::pair<int,int>> gh(gapHist.begin(), gapHist.end());
        std::sort(gh.begin(), gh.end(), [](auto&a, auto&b){ return a.second > b.second; });
        for (size_t i = 0; i < gh.size() && i < 12; ++i) printf("%d:%d  ", gh[i].first, gh[i].second);
        printf("\n");
        int runs = 0, longest = 1, cur = 1;
        for (size_t i = 1; i < badIdx.size(); ++i) {
            if (badIdx[i] == badIdx[i-1] + 1) { ++cur; }
            else { if (cur > 1) ++runs; if (cur > longest) longest = cur; cur = 1; }
        }
        if (cur > longest) longest = cur;
        printf("contiguous runs >1: %d   longest run: %d frames\n", runs, longest);
    }
    // ── WHAT WAS HAPPENING WHEN THE AUDIO BROKE ─────────────────────────────
    if (!badIdx.empty()) {
        const int f0 = frameOfMp2[size_t(badIdx.front())];
        const int f1 = frameOfMp2[size_t(badIdx.back())];
        printf("\nburst spans DAB frames %d..%d — context:\n", f0, f1);
        printf("  frame  start   delta  fibOk/tot  nullDb   prs\n");
        for (int i = std::max(0, f0 - 4); i <= std::min(int(frameStarts.size()) - 1, f1 + 3); ++i) {
            const long d = i > 0 ? frameStarts[size_t(i)] - frameStarts[size_t(i-1)] : 0;
            printf("  %5d  %6ld  %5ld  %5d/%-4d  %6.1f  %5.2f%s\n", i, frameStarts[size_t(i)], d,
                   fibOkPer[size_t(i)], fibTotPer[size_t(i)], nullDepth[size_t(i)], prsCorr[size_t(i)],
                   (i >= f0 && i <= f1) ? "   <-- BAD AUDIO" : "");
        }
    }
    // ── frame-start stability: a skip or a duplicate corrupts 16 CIFs ────────
    {
        std::map<long,int> d;
        for (size_t i = 1; i < frameStarts.size(); ++i)
            if (frameStarts[i] >= 0 && frameStarts[i-1] >= 0) d[frameStarts[i] - frameStarts[i-1]]++;
        printf("frame-start deltas: ");
        for (const auto& kv : d) printf("%ld:%d  ", kv.first, kv.second);
        printf("\n");
    }
    std::fclose(fp);
    return 0;
}
