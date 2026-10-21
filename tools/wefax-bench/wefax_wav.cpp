// wefax_wav — replay a recorded WAV through the SHIPPING WEFAX decoder (android/app/src/main/cpp/decoders/wefax_decoder.cpp)
// with the options the app sends (DecoderClient.ts: phasing on, NO auto_start / auto_stop) at the 48 kHz the decoder host
// feeds it. Writes one PGM per chart (a new chart starts on each START event, as the clients roll to PREV) and logs every
// phase change / START / STOP with its time in the recording.
//
//   ffmpeg -i rec.wav -ar 48000 -ac 1 -c:a pcm_s16le rec48.wav
//   c++ -O2 -std=c++17 -I android/app/src/main/cpp tools/wefax-bench/wefax_wav.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp -o wefax_wav
//   ./wefax_wav rec48.wav outprefix [lpm=120] [skipSeconds=0] [carrierHz=1900]
//
// skipSeconds drops the start of the recording: the "late join" variant (no start tone, no phasing).
#include "decoders/wefax_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace vibe;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: wefax_wav in48k.wav outprefix [lpm] [skipSeconds]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    int lpm = argc > 3 ? atoi(argv[3]) : 120;
    double skipS = argc > 4 ? atof(argv[4]) : 0;
    double carrier = argc > 5 ? atof(argv[5]) : 1900.0;   // only for an off-centre recording; the app always sends 1900
    // Minimal RIFF walk: find fmt + data.
    char id[4]; uint32_t sz; uint8_t hdr[12]; fread(hdr, 1, 12, f);
    int rate = 0, chans = 1; long dataLen = 0;
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) { std::vector<uint8_t> b(sz); fread(b.data(), 1, sz, f); chans = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; }
        else if (!memcmp(id, "data", 4)) { dataLen = sz; break; }
        else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    if (chans != 1) { fprintf(stderr, "need mono s16 (got x%d)\n", chans); return 1; }
    if (rate != 48000) fprintf(stderr, "note: %d Hz — the shipping host always feeds 48000\n", rate);
    std::vector<int16_t> pcm(dataLen / 2); pcm.resize(fread(pcm.data(), 2, pcm.size(), f)); fclose(f);
    size_t start = (size_t)(skipS * rate); if (start > pcm.size()) start = pcm.size();

    WefaxDecoder::Config cfg; cfg.lpm = lpm; cfg.usePhasing = true; cfg.autoStart = false; cfg.autoStop = false; cfg.carrier = carrier;
    WefaxDecoder d(rate, cfg);
    std::vector<std::vector<uint8_t>> img; int chart = 0; size_t pos = start;
    auto t = [&]() { return (double)pos / rate; };
    auto flush = [&]() {
        if (img.size() < 50) { img.clear(); return; }
        char name[512]; snprintf(name, sizeof name, "%s_c%d.pgm", argv[2], chart);
        FILE* o = fopen(name, "wb"); fprintf(o, "P5\n%d %zu\n255\n", d.width(), img.size());
        for (auto& r : img) fwrite(r.data(), 1, r.size(), o); fclose(o);
        printf("wrote %s  %zu lines\n", name, img.size()); chart++; img.clear();
    };
    d.onLine = [&](uint32_t, uint32_t w, const uint8_t* px) { img.emplace_back(px, px + w); };
    d.onStart = [&]() { printf("%8.1fs START (new chart)\n", t()); flush(); };
    d.onStop  = [&]() { printf("%8.1fs STOP\n", t()); };
    const char* names[] = { "standing by", "start tone", "phasing", "image", "stop tone" };
    d.onDiag = [&](const std::string& m) { printf("%8.1fs DIAG %s\n", t(), m.c_str()); };
    d.onPhase = [&](int p) { printf("%8.1fs phase %d %s\n", t(), p, (p >= 0 && p <= 4) ? names[p] : "?"); };
    const size_t CH = 4800;
    for (; pos < pcm.size(); pos += CH) d.process(pcm.data() + pos, (int)std::min(CH, pcm.size() - pos));
    flush();
    return 0;
}
