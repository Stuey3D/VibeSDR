// IQ REPLAY — a captured rtl_tcp (u8) IQ file through the real RxPipeline in WFM, with the stereo
// decoder's state printed twice a second and the stereo audio written to a WAV.
//
// ★ Made for the RSP1A's "mud in the width" on 96.1 (2026-10-30): the captures come off the server's
//   full-rate raw IQ tap (after the capture cleaner and the gain-step smoother, rotated to the VFO),
//   so the station is at 0 Hz.
//
//   vibedsp_iq_replay <file.u8> <rate> [offset Hz] [out.wav] [autoBw Hz, 0 = wide] [chanBins] [fftSize] [bw Hz]
//
// ★ Taken from v12-rsp-spur (5779e9cab) WITHOUT the spur canceller's argument — that stage is not on this branch.
//
// ★ chanBins > 0 runs the SERVER'S LISTENER PATH: a Channelizer of fftSize over the capture, extract()
//   at the station, and the RxPipeline at chanRate = rate x chanBins / fftSize — what a listener on a
//   shared dial actually hears. chanBins 0 feeds the pipeline the raw capture (the direct path).
#include "../vibedsp.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

using namespace vibedsp;

struct Cap { std::vector<float> lr; int rate = 48000; };
static void audioCb(void* ctx, const float* pcm, int frames, int ch, int rate) {
    auto* c = (Cap*)ctx; c->rate = rate;
    for (int i = 0; i < frames; ++i) { c->lr.push_back(pcm[i * ch]); c->lr.push_back(pcm[i * ch + (ch > 1 ? 1 : 0)]); }
}

static void writeWav(const char* path, const std::vector<float>& lr, int rate) {
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    const unsigned n = (unsigned)lr.size(), bytes = n * 2;
    auto u32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
    u32((unsigned)rate); u32((unsigned)rate * 4); u16(4); u16(16); std::fwrite("data", 1, 4, f); u32(bytes);
    for (float v : lr) { const float c = std::max(-1.f, std::min(1.f, v)); const short s = (short)std::lrint(c * 32767.f); std::fwrite(&s, 2, 1, f); }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s file.u8 rate [offset] [out.wav] [autoBw]\n", argv[0]); return 2; }
    const double fs = std::atof(argv[2]), off = argc > 3 ? std::atof(argv[3]) : 0.0;
    const char* wav = argc > 4 ? argv[4] : nullptr;
    const double autoBw = argc > 5 ? std::atof(argv[5]) : 0.0;
    const int chanBins = argc > 6 ? std::atoi(argv[6]) : 0, fftN = argc > 7 ? std::atoi(argv[7]) : 32768;
    const double bwHz = argc > 8 ? std::atof(argv[8]) : 200000.0;
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror(argv[1]); return 1; }
    std::vector<unsigned char> u; { unsigned char b[1 << 16]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) u.insert(u.end(), b, b + r); }
    std::fclose(f);
    const int N = (int)(u.size() / 2);
    std::vector<cf32> iq(N);
    for (int i = 0; i < N; ++i) iq[i] = cf32((u[2 * i] - 127.5f) / 127.5f, (u[2 * i + 1] - 127.5f) / 127.5f);

    Cap cap; RxPipeline::Callbacks cb; cb.ctx = &cap; cb.audio = audioCb;
    RxPipeline pipe;
    const double rate = chanBins > 0 ? fs * chanBins / fftN : fs;
    const int centreBin = (int)std::lround(off / (fs / fftN));
    const double resid = chanBins > 0 ? off - centreBin * (fs / fftN) : off;
    pipe.start(rate, 1024, 20.0, 48000, cb);
    pipe.setDeemphasis(50e-6);
    pipe.setTune(resid, RxPipeline::Mode::WFM, bwHz);
    pipe.setAutoBandwidth(autoBw);
    Channelizer chz(chanBins > 0 ? fftN : 4096);
    Channelizer::ExtractCtx ectx;
    std::vector<cf32> slice((size_t)std::max(chanBins, 1));
    if (chanBins > 0) std::printf("server path: fft %d, chanBins %d -> channel %.0f Hz, bw %.0f\n", fftN, chanBins, rate, bwHz);
    const int blk = (int)(fs * 0.5);
    for (int o = 0, k = 0; o < N; o += blk, ++k) {
        const int n = std::min(blk, N - o);
        if (chanBins > 0) {
            chz.feed(iq.data() + o, n, [&](const cf32* bins, int) {
                const int got = chz.extract(bins, centreBin, chanBins, slice.data(), ectx, chz.blockIndex());
                if (got > 0) pipe.feed(slice.data(), got);
            });
        } else
            for (int s = 0; s < n; s += 65536) pipe.feed(iq.data() + o + s, std::min(65536, n - s));
        std::printf("%5.1f s  pilot %s  dev %.2f kHz  blend %.2f  S/N %5.1f  lmrCut %5.0f Hz  wideCost %.1f\n", (o + n) / fs,
                    pipe.pilotLocked() ? "LOCK" : "----", std::fabs(pipe.pilotLockAmp()) * 75.f,
                    pipe.stereoBlend(), pipe.blendSnrDb(), pipe.lmrHiCutHz(), pipe.ifGainDb());
    }
    pipe.stop();
    if (wav) writeWav(wav, cap.lr, cap.rate);
    return 0;
}
