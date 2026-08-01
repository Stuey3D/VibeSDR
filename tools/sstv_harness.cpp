// sstv_harness — run REAL SSTV audio through the shipping decoder, on the Mac.
//
//   clang++ -std=c++17 -O2 tools/sstv_harness.cpp \
//       android/app/src/main/cpp/decoders/sstv_decoder.cpp \
//       android/app/src/main/cpp/fft/kiss_fftr.c android/app/src/main/cpp/fft/kiss_fft.c \
//       -Iandroid/app/src/main/cpp -o /tmp/sstv_harness
//   /tmp/sstv_harness in.wav out.ppm
//
// ★★★ WHY THIS EXISTS. This bug has been "fixed" three times, each time verified against
// whatever happened to be on air that night — once. A frame that came through whole was written
// up as proof, and a different ENCODING had been hiding the fault (Stuart, 2026-08-01). One good
// frame is not a test.
//
// ★★ AND WHY IT TAKES A WAV RATHER THAN SYNTHESISING ONE. Stuart: "I don't want to prove it with
// fake audio in a simulator just to have the real deal break" — which is technically right: slant
// is a clock/sample-rate artefact, and audio generated with a perfect clock would flatten out the
// very thing under test. So this replays REAL recordings. Clean MP3s are the first gate, not the
// last word: passing them proves the decoder handles a good signal, and off-air remains the proof.
#include "decoders/sstv_decoder.h"
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// Minimal WAV reader: 16-bit PCM, mono or stereo (stereo is mixed down).
static bool readWav(const char* path, std::vector<int16_t>& out, int& rate) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) {
        fprintf(stderr, "%s is not a RIFF/WAVE file\n", path); fclose(f); return false;
    }
    int channels = 1, bits = 16;
    rate = 0;
    while (!feof(f)) {
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
        if (!memcmp(id, "fmt ", 4)) {
            uint16_t fmt, ch, blockAlign, bps; uint32_t sr, byteRate;
            fread(&fmt, 2, 1, f); fread(&ch, 2, 1, f); fread(&sr, 4, 1, f);
            fread(&byteRate, 4, 1, f); fread(&blockAlign, 2, 1, f); fread(&bps, 2, 1, f);
            channels = ch; rate = (int)sr; bits = bps;
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            const size_t frames = sz / (channels * (bits / 8));
            std::vector<int16_t> raw(frames * channels);
            fread(raw.data(), 2, raw.size(), f);
            out.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                int acc = 0;
                for (int c = 0; c < channels; c++) acc += raw[i * channels + c];
                out[i] = (int16_t)(acc / channels);
            }
            fclose(f);
            return bits == 16;
        } else {
            fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
        }
    }
    fclose(f);
    fprintf(stderr, "%s has no data chunk\n", path);
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: sstv_harness in.wav out.ppm [autosync=0|1]\n"); return 2; }
    // ★★★ MATCH WHAT THE RADIO ACTUALLY RUNS. local_sdr_shim.cpp:3361 constructs the decoder with
    //     autoSync=FALSE — the post-reception cleanup was switched off on 2026-08-01 because it was
    //     the thing breaking the picture. A harness that leaves the default ON tests a code path the
    //     product no longer executes, which is exactly how a "fix" gets verified against nothing.
    const bool autoSync = (argc > 3) ? (atoi(argv[3]) != 0) : false;

    std::vector<int16_t> pcm; int rate = 0;
    if (!readWav(argv[1], pcm, rate)) return 1;
    fprintf(stderr, "%s: %zu samples @ %d Hz (%.1fs)\n", argv[1], pcm.size(), rate,
            pcm.size() / (double)rate);

    vibe::SstvDecoder dec((double)rate, autoSync);

    int W = 0, H = 0;
    std::vector<uint8_t> img;
    std::string modeName = "(none)";
    int linesSeen = 0, lastY = -1;
    bool redrew = false, completed = false;

    dec.onMode = [&](uint8_t idx, const std::string& name) {
        modeName = name;
        fprintf(stderr, "  mode: %s (vis idx %u)\n", name.c_str(), idx);
    };
    dec.onImageStart = [&](int w, int h) {
        W = w; H = h; img.assign((size_t)w * h * 3, 0);
        fprintf(stderr, "  image: %dx%d\n", w, h);
    };
    dec.onLine = [&](int y, int w, const uint8_t* rgb) {
        linesSeen++;
        if (y > lastY) lastY = y;
        if (!img.empty() && y >= 0 && y < H && w == W) memcpy(&img[(size_t)y * W * 3], rgb, (size_t)w * 3);
    };
    // ★ The two events that matter for THIS bug: the post-reception redraw is where the slant
    //   correction is applied, and the symptom is that it repaints only part of the image.
    dec.onRedrawStart = [&]() { redrew = true; fprintf(stderr, "  redraw: STARTED\n"); };
    dec.onComplete    = [&]() { completed = true; fprintf(stderr, "  complete\n"); };
    dec.onStatus      = [&](const std::string& s) { fprintf(stderr, "  status: %s\n", s.c_str()); };

    // Feed it the way the radio does — small blocks, not one giant buffer.
    const int BLOCK = 1024;
    for (size_t i = 0; i < pcm.size(); i += BLOCK) {
        const int n = (int)std::min((size_t)BLOCK, pcm.size() - i);
        dec.process(&pcm[i], n);
    }
    // The decode runs on its own thread; give it time to drain and run any redraw.
    for (int i = 0; i < 200 && !completed; i++) {
        struct timespec ts{0, 50 * 1000 * 1000}; nanosleep(&ts, nullptr);
    }

    printf("RESULT autosync=%d mode=%s size=%dx%d lines=%d lastY=%d redraw=%s complete=%s\n",
           autoSync?1:0, modeName.c_str(), W, H, linesSeen, lastY,
           redrew ? "yes" : "no", completed ? "yes" : "no");

    if (img.empty()) { fprintf(stderr, "no image decoded\n"); return 1; }
    FILE* o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    fwrite(img.data(), 1, img.size(), o);
    fclose(o);
    return 0;
}
