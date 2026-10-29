// iq_channel — cut one channel out of a stereo IQ recording (I left, Q right; tools/ubersdr-rec iq48) as the USB audio
// a receiver would give, with the chosen RF frequency at a chosen audio frequency — the decoders' input.
//
//   iq_channel in.wav centre_hz signal_hz out.wav [audio_centre=1000] [rate=12000] [start_s=0] [secs=all]
//
// USB: higher RF = higher audio. The passband is ±1.2 kHz around 1500 Hz audio (an SSB filter, roughly 300–2700 Hz).
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: %s in.wav centre_hz signal_hz out.wav [audio_centre=1000] [rate=12000] [start_s] [secs]\n", argv[0]); return 2; }
    const double fc = std::atof(argv[2]), fsig = std::atof(argv[3]), ac = argc > 5 ? std::atof(argv[5]) : 1000;
    const int orate = argc > 6 ? std::atoi(argv[6]) : 12000;
    const double t0 = argc > 7 ? std::atof(argv[7]) : 0, dur = argc > 8 ? std::atof(argv[8]) : 1e12;
    FILE* f = std::fopen(argv[1], "rb"); if (!f) { std::perror("open"); return 1; }
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4)) { std::fprintf(stderr, "not a WAV\n"); return 1; }
    int ch = 2, fs = 48000, bits = 16; long dataLen = 0;
    for (;;) {
        char id[4]; uint32_t sz; if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 4, 1, f) != 1) return 1;
        if (!std::memcmp(id, "fmt ", 4)) { std::vector<uint8_t> b(sz); if (std::fread(b.data(), 1, sz, f) != sz) return 1; ch = b[2] | b[3] << 8; std::memcpy(&fs, &b[4], 4); bits = b[14] | b[15] << 8; continue; }
        if (!std::memcmp(id, "data", 4)) { dataLen = sz; break; }
        std::fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    if (ch != 2 || bits != 16 || fs % orate) { std::fprintf(stderr, "need 16-bit stereo IQ at a multiple of %d Hz\n", orate); return 1; }
    const int dec = fs / orate;
    long frames = dataLen / 4; const long skip = (long)(t0 * fs);
    std::fseek(f, skip * 4, SEEK_CUR); frames = std::min(frames - skip, (long)(dur * fs));
    const double dial = fsig - ac, shift = dial + 1500.0 - fc;
    const int N = 8 * dec * 16 + 1;   // lowpass ±1.2 kHz, Blackman sinc
    std::vector<double> h(N); double sum = 0;
    for (int i = 0; i < N; i++) { const double m = i - (N - 1) / 2.0, x = 2 * 1200.0 / fs * m;
        const double s = std::fabs(m) < 1e-9 ? 1 : std::sin(M_PI * x) / (M_PI * x);
        h[i] = s * (0.42 - 0.5 * std::cos(2 * M_PI * i / (N - 1)) + 0.08 * std::cos(4 * M_PI * i / (N - 1))); sum += h[i]; }
    for (double& v : h) v /= sum;
    std::vector<std::complex<double>> line(2 * N);
    FILE* o = std::fopen(argv[4], "wb");
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, o); }; auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, o); };
    std::fwrite("RIFF", 1, 4, o); w32(0); std::fwrite("WAVEfmt ", 1, 8, o); w32(16); w16(1); w16(1); w32(orate); w32(orate * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, o); w32(0);
    const std::complex<double> st = std::polar(1.0, -2 * M_PI * shift / fs), su = std::polar(1.0, 2 * M_PI * 1500.0 / orate);
    std::complex<double> ph(1, 0), pu(1, 0);
    int pos = 0; long outN = 0; std::vector<int16_t> buf(4096 * 2); std::vector<int16_t> ob;
    for (long done = 0; done < frames;) {
        const long m = std::min<long>(4096, frames - done);
        if ((long)std::fread(buf.data(), 4, m, f) != m) break;
        for (long k = 0; k < m; k++) {
            const std::complex<double> z(buf[2 * k] / 32768.0, buf[2 * k + 1] / 32768.0);
            line[pos] = line[pos + N] = z * ph; ph *= st;
            pos = (pos + 1) % N;
            if (((done + k) % dec) == 0) {
                std::complex<double> y(0, 0);
                for (int i = 0; i < N; i++) y += h[i] * line[pos + i];
                const double a = std::real(y * pu); pu *= su;
                ob.push_back((int16_t)std::lround(std::max(-1.0, std::min(1.0, 4 * a)) * 32767));
                if ((++outN & 1023) == 0) { ph /= std::abs(ph); pu /= std::abs(pu); }
            }
        }
        done += m;
    }
    std::fwrite(ob.data(), 2, ob.size(), o);
    const uint32_t dl = (uint32_t)(ob.size() * 2), rl = 36 + dl;
    std::fseek(o, 4, SEEK_SET); w32(rl); std::fseek(o, 40, SEEK_SET); w32(dl); std::fclose(o);
    std::printf("wrote %s: %.0f s, dial %.3f kHz USB, %.3f kHz at %.0f Hz audio\n", argv[4], ob.size() / (double)orate, dial / 1000, fsig / 1000, ac);
    return 0;
}
