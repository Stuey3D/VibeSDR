// sitor_wav — run a WAV (any rate, mono or the first channel; 16-bit PCM) through the SITOR-A listener
// (decoders/sitor_a.cpp) and print what it printed. Each change of the sending station starts a new line "[1] " / "[2] ".
//
//   sitor_wav in.wav [centre=1000] [options]
//   options (env SITOR_OPTS, comma list): combine=0, afc=0, timing=0, atc=0
//   stderr: the protocol events, the final status and the CPU time per second of audio.
#include "decoders/sitor_a.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s in.wav [centre=1000]\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror("open"); return 1; }
    std::vector<uint8_t> b; { uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n); }
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(&b[0], "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) { std::fprintf(stderr, "not a WAV\n"); return 1; }
    size_t p = 12; int ch = 1, rate = 0, bits = 0; const int16_t* pcm = nullptr; size_t frames = 0;
    while (p + 8 <= b.size()) {
        uint32_t sz; std::memcpy(&sz, &b[p + 4], 4);
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = b[p + 10] | b[p + 11] << 8; std::memcpy(&rate, &b[p + 12], 4); bits = b[p + 22] | b[p + 23] << 8; }
        if (!std::memcmp(&b[p], "data", 4)) { pcm = (const int16_t*)&b[p + 8]; frames = std::min<size_t>(sz, b.size() - p - 8) / 2 / (ch ? ch : 1); break; }
        p += 8 + sz + (sz & 1);
    }
    if (!pcm || bits != 16) { std::fprintf(stderr, "need 16-bit PCM\n"); return 1; }
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; i++) mono[i] = pcm[i * ch] / 32768.0f;
    const double cf = argc > 2 ? std::atof(argv[2]) : 1000;
    vibe::sitora::Options o;
    if (const char* e = std::getenv("SITOR_OPTS")) {
        std::string s = e; s += ',';
        for (size_t a = 0, q; (q = s.find(',', a)) != std::string::npos; a = q + 1) {
            const std::string kv = s.substr(a, q - a); const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq); const bool v = kv.substr(eq + 1) != "0";
            if (k == "combine") o.combine = v; else if (k == "afc") o.afc = v; else if (k == "timing") o.fineTiming = v; else if (k == "atc") o.atc = v;
        }
    }
    vibe::sitora::Receiver rx(rate, cf, o);
    std::string out; int side = -1; double tNow = 0;
    const bool quiet = std::getenv("SITOR_QUIET") != nullptr;
    rx.onChar = [&](int s, char32_t c) {
        if (s != side) { if (!out.empty()) out += '\n'; out += "[" + std::to_string(s + 1) + "] "; side = s; }
        out += c < 128 ? (char)c : '?';
    };
    rx.onEvent = [&](const std::string& e) { if (!quiet) std::fprintf(stderr, "%8.1f s  %s\n", tNow, e.c_str()); };
    vibe::sitora::Link last = vibe::sitora::Link::NoSignal;
    rx.onStatus = [&](const vibe::sitora::Status& s) {
        if (s.link != last && !quiet) std::fprintf(stderr, "%8.1f s  [%s]\n", tNow, vibe::sitora::linkName(s.link));
        last = s.link;
    };
    const auto t0 = std::chrono::steady_clock::now();
    const int chunk = rate / 50;
    for (size_t i = 0; i < frames; i += chunk) {
        tNow = (double)i / rate;
        rx.process(&mono[i], (int)std::min<size_t>(chunk, frames - i));
    }
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s\n", out.c_str());
    const auto& s = rx.status();
    std::fprintf(stderr, "── %.1f s audio, %.3f s CPU (%.2f ms per second of audio)\n", frames / (double)rate, cpu, 1000 * cpu / (frames / (double)rate));
    std::fprintf(stderr, "── blocks %lu, repeats %lu, combined %lu, lost chars %lu; master %d; ids [%s|%s] numbers [%s|%s]%s; snr %.1f/%.1f dB; offset %+.1f/%+.1f Hz%s\n",
                 s.blocks, s.repeats, s.combined, s.lost, s.master + 1, s.ids[0].c_str(), s.ids[1].c_str(), s.numbers[0].c_str(),
                 s.numbers[1].c_str(), s.idChecked ? " (checked)" : "", s.snrDb[0], s.snrDb[1], s.offsetHz[0], s.offsetHz[1], s.inverted ? " inverted" : "");
    return 0;
}
