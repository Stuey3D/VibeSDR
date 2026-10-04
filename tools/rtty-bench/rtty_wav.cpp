// rtty_wav — run a WAV through the server's RTTY decoder (decoders/fsk_decoder.cpp), exactly as the decoder host
// does (48 kHz mono int16), and print the text and the decoder's resync count. For before/after measurements on a
// real recording: build once against the old decoder and once against the new, run both on the same file.
//
//   rtty_wav in.wav [centre=1000] [shift=450] [baud=50] [framing=5N1.5] [inverted=1]
//   rtty_wav in.wav auto        — RttyAuto: finds shift, centre, baud and polarity itself
//   rtty_wav in.wav [centre] [shift] [baud] 4/7 [inverted] — NAVTEX: CCIR476 (SITOR-B FEC); the framing "4/7" picks it
//   rtty_wav in.wav CCIR476 [centre=500] [shift=170] [baud=100] [inverted=0] — the same, with NAVTEX's defaults
//   (★ 2026-10-04: the encoding argument was added for NAVTEX; the ITA2 and auto forms above are unchanged.)
//   (DWD weather RTTY: 450 Hz shift, 50 baud, inverted — the app's 'weather' preset.)
//   Convert first if needed:  ffmpeg -i rec.m4a -ac 1 -ar 48000 -c:a pcm_s16le in.wav
#include "decoders/fsk_decoder.h"
#include "decoders/rtty_auto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int runFsk(const std::vector<int16_t>& mono, size_t frames, double cf, double sh, double baud,
                  const std::string& fr, const std::string& enc, bool inv) {
    vibe::FskDecoder d(48000, cf, sh, baud, fr, enc, inv);
    std::string out; long chars = 0;
    d.onChar = [&](char32_t c) { chars++; if (c == U'\r') return; out += c < 128 ? (char)c : '?'; };
    for (size_t i = 0; i < mono.size(); i += 960) d.process(&mono[i], (int)std::min<size_t>(960, mono.size() - i));
    std::printf("%s\n", out.c_str());
    std::fprintf(stderr, "── %.1f s, %ld chars, resyncs %lu\n", frames / 48000.0, chars, d.resyncs());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s in.wav [centre] [shift] [baud] [framing] [inverted]\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror("open"); return 1; }
    std::vector<uint8_t> b; { uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n); }
    std::fclose(f);
    // Minimal RIFF walk: find "fmt " and "data".
    if (b.size() < 44 || std::memcmp(&b[0], "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) { std::fprintf(stderr, "not a WAV\n"); return 1; }
    size_t p = 12; int ch = 0, rate = 0, bits = 0; const int16_t* pcm = nullptr; size_t frames = 0;
    while (p + 8 <= b.size()) {
        uint32_t sz; std::memcpy(&sz, &b[p + 4], 4);
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = b[p + 10] | b[p + 11] << 8; std::memcpy(&rate, &b[p + 12], 4); bits = b[p + 22] | b[p + 23] << 8; }
        if (!std::memcmp(&b[p], "data", 4)) { pcm = (const int16_t*)&b[p + 8]; frames = std::min<size_t>(sz, b.size() - p - 8) / 2 / (ch ? ch : 1); break; }
        p += 8 + sz + (sz & 1);
    }
    if (!pcm || bits != 16 || rate != 48000) { std::fprintf(stderr, "need 16-bit 48 kHz PCM (got %d-bit %d Hz) — convert with ffmpeg\n", bits, rate); return 1; }
    std::vector<int16_t> mono(frames);
    for (size_t i = 0; i < frames; i++) mono[i] = pcm[i * ch];
    if (argc > 2 && std::string(argv[2]) == "auto") {
        vibe::RttyAuto a(48000);
        std::string out;
        a.onChar = [&](char32_t c) { if (c == U'\r') return; out += c < 128 ? (char)c : '?'; };
        for (size_t i = 0; i < mono.size(); i += 960) a.process(&mono[i], (int)std::min<size_t>(960, mono.size() - i));
        std::printf("%s\n", out.c_str());
        std::fprintf(stderr, "── auto chose: %s", a.chosen().empty() ? "nothing\n" : a.chosen().c_str());
        return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "CCIR476") {
        const double cf = argc > 3 ? std::atof(argv[3]) : 500, sh = argc > 4 ? std::atof(argv[4]) : 170,
                     baud = argc > 5 ? std::atof(argv[5]) : 100;
        const bool inv = argc > 6 ? std::atoi(argv[6]) != 0 : false;
        return runFsk(mono, frames, cf, sh, baud, "4/7", "CCIR476", inv);
    }
    const double cf = argc > 2 ? std::atof(argv[2]) : 1000, sh = argc > 3 ? std::atof(argv[3]) : 450, baud = argc > 4 ? std::atof(argv[4]) : 50;
    const std::string fr = argc > 5 ? argv[5] : "5N1.5"; const bool inv = argc > 6 ? std::atoi(argv[6]) != 0 : true;
    return runFsk(mono, frames, cf, sh, baud, fr, fr == "4/7" ? "CCIR476" : "ITA2", inv);
}
