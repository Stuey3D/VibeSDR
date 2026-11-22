// wefax_bg_runner — replay a recorded WEFAX audio WAV through the WHOLE BACKGROUND PATH, not just the decoder: the audio is
// put back on the air as USB IQ (a 4 MHz-ish capture), and a real bg::BgRunner cuts it with its channeliser, demodulates it
// with the server's RxPipeline (USB, 48 kHz), runs the DecoderHost's WEFAX with the background options (auto_start +
// auto_stop) and files the charts with the gallery tap — exactly what an unattended WEFAX VFO does on a server
// (2026-11-22: 8 h on Stuart's Pi 500 with no chart saved — is the start missed anywhere in that chain?).
//
//   ffmpeg -i rec.wav -ar 48000 -ac 1 -c:a pcm_s16le rec48.wav
//   build: as vibeserver/test-bgdecode (run-tests.sh flags_for/deps_for test-bgdecode), this file in its place
//   ./wefax_bg_runner rec48.wav galleryDir [dialHz=4608100] [stations]
//
// `stations`: run it as a WEFAX STATIONS VFO instead (the dial's centre frequency ticked) — the same audio, the new path.
// Prints the server's own log lines (wefax: … diag, the hourly lines every 10 min of audio) and the gallery's items.
// ★ SILENT: nothing is played.
#include "vibe_bgdecode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;

static std::vector<int16_t> readWav(const char* path, int& rate) {
    std::vector<int16_t> pcm; rate = 0;
    FILE* f = std::fopen(path, "rb"); if (!f) return pcm;
    char id[4]; uint32_t sz; uint8_t hdr[12]; if (std::fread(hdr, 1, 12, f) != 12) { std::fclose(f); return pcm; }
    long dataLen = 0; int ch = 1;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&sz, 4, 1, f) == 1) {
        if (!std::memcmp(id, "fmt ", 4)) { std::vector<uint8_t> b(sz); if (std::fread(b.data(), 1, sz, f) != sz) break; ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; }
        else if (!std::memcmp(id, "data", 4)) { dataLen = sz; break; }
        else std::fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    if (ch != 1) { std::fclose(f); return pcm; }
    pcm.resize((size_t)dataLen / 2);
    pcm.resize(std::fread(pcm.data(), 2, pcm.size(), f));
    std::fclose(f);
    return pcm;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: wefax_bg_runner in48k.wav galleryDir [dialHz] [stations]\n"); return 2; }
    int ar = 0;
    const std::vector<int16_t> a = readWav(argv[1], ar);
    if (a.empty() || ar != 48000) { std::fprintf(stderr, "need a 48 kHz mono s16 WAV\n"); return 1; }
    const double dial = argc > 3 ? std::atof(argv[3]) : 4608100.0;
    const bool stations = argc > 4 && !std::strcmp(argv[4], "stations");
    const double fs = 240000, centre = dial + 40000;   // ★ the signal 40 kHz below the capture's centre, as a VFO usually is
    const int up = (int)(fs / ar);                     // 5

    // ── the audio → USB IQ: shift the passband's middle (1900 Hz) to 0, low-pass ±1.4 kHz (drops the mirror image at
    //    −3.8 kHz), then up to the capture rate and out to (dial + 1900 − centre). The fax tones are 1500–2300 Hz. ──
    const int NT = 255; std::vector<double> h(NT);
    { const double fc = 1400.0 / ar; double s = 0;
      for (int i = 0; i < NT; i++) { const double m = i - (NT - 1) / 2.0; const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (NT - 1)) + 0.08 * std::cos(4 * M_PI * i / (NT - 1));
        h[i] = (m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m)) * w; s += h[i]; }
      for (auto& x : h) x /= s; }

    gallery::Store store; store.setDir(argv[2]);
    FileSlots slots("", [] { return 4; });
    std::mutex lm;
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &store;
    p.profile.centreHz = centre;
    bg::Vfo v; v.mode = "usb";
    if (stations) { v.dec = "wefax-stations"; v.hz = 0; v.fax = { dial + 1900 }; } else { v.dec = "wefax"; v.hz = dial; }
    p.profile.vfos = { v };
    double audioT = 0;
    p.log = [&](const std::string& t) {
        std::lock_guard<std::mutex> g(lm);
        if (t.find("wefax") != std::string::npos || t.find("background decoding") != std::string::npos)
            std::printf("%8.1fs  %s\n", audioT, t.c_str());
    };
    p.wefaxHourSecs = 60;    // ★ the "hour" is a wall-clock minute here (the replay runs ~80× real time); the part-hour at the end
    r.start(p);

    std::vector<double> zr(NT, 0), zi(NT, 0); int zp = 0;
    double ph1 = 0, ph2 = 0;
    const double w1 = -2 * M_PI * 1900.0 / ar, w2 = 2 * M_PI * (dial + 1900.0 - centre) / fs;
    std::vector<vibedsp::cf32> iq; iq.reserve(16384);
    double pr = 0, pi = 0;   // the previous baseband sample (linear interpolation up)
    for (size_t n = 0; n < a.size(); n++) {
        const double x = a[n] / 32768.0;
        zr[zp] = x * std::cos(ph1); zi[zp] = x * std::sin(ph1);
        ph1 += w1; if (ph1 < -M_PI) ph1 += 2 * M_PI;
        double br = 0, bi = 0;
        for (int k = 0, q = zp; k < NT; k++) { br += h[k] * zr[q]; bi += h[k] * zi[q]; if (--q < 0) q = NT - 1; }
        zp = (zp + 1) % NT;
        for (int u = 1; u <= up; u++) {
            const double t = (double)u / up, cr = pr + (br - pr) * t, ci = pi + (bi - pi) * t;
            const double c = std::cos(ph2), s = std::sin(ph2);
            ph2 += w2; if (ph2 > M_PI) ph2 -= 2 * M_PI; if (ph2 < -M_PI) ph2 += 2 * M_PI;
            iq.push_back(vibedsp::cf32((float)(2.0 * (cr * c - ci * s)), (float)(2.0 * (cr * s + ci * c))));
        }
        pr = br; pi = bi;
        if (iq.size() >= 16384) {
            while (r.queuedForTest() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            { std::lock_guard<std::mutex> g(lm); audioT = (double)n / ar; }
            r.feedIq(iq.data(), (int)iq.size()); iq.clear();
        }
    }
    if (!iq.empty()) r.feedIq(iq.data(), (int)iq.size());
    while (r.queuedForTest() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    std::printf("dropped IQ samples: %llu\n", (unsigned long long)r.droppedSamples());
    r.stop();
    std::printf("gallery: %s\n", store.listJson("wefax", 50).c_str());
    return 0;
}
