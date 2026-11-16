// balloon_tx.h — V12 balloons: OUR OWN TRANSMITTER, from the formats (docs/v12/BALLOONS.md) — what the tests, the
// benchmark row and the end-to-end clip send. Nothing here runs in a receiver.
//   • rtty — bytes as asynchronous RTTY: start bit, 7 or 8 data bits LSB first, 1 / 1.5 / 2 stop bits, idle on mark;
//     mark the HIGHER tone (UKHAS practice in USB), continuous phase.
//   • horus — Horus Binary frames (balloon_horus.h's coder and modulator).
//   • ssdv — a made-up picture's quantised blocks (a gradient with some detail) for encodeImage.
#pragma once
#include "balloon_horus.h"
#include "balloon_ssdv.h"
#include "balloon_ukhas.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vibe {
namespace balloon {
namespace tx {

/** ★ A transmission as KEYING: (frequency in Hz, seconds) segments — rendered as real audio (render) or as the complex
 *  baseband an SSB receiver would turn into that audio (renderIq), at any rate. */
struct Seg { double hz, sec; };
using Keying = std::vector<Seg>;

struct Rtty { double baud = 300, shift = 425, centre = 1500, stop = 2; int bits = 8; bool reverse = false; };
/** `bytes` as RTTY keying, after `idleSec` of mark. */
inline void rtty(const std::vector<uint8_t>& bytes, const Rtty& r, Keying& k, double idleSec = 0.3) {
    const double T = 1.0 / r.baud;
    auto tone = [&](bool mark, double nbits) {
        const bool hi = mark != r.reverse;
        const double f = r.centre + (hi ? r.shift / 2 : -r.shift / 2);
        if (!k.empty() && k.back().hz == f) k.back().sec += nbits * T; else k.push_back({ f, nbits * T });
    };
    if (idleSec > 0) tone(true, idleSec * r.baud);
    for (uint8_t b : bytes) {
        tone(false, 1);
        for (int i = 0; i < r.bits; ++i) tone((b >> i) & 1, 1);
        tone(true, r.stop);
    }
}
inline void silence(Keying& k, double sec) { k.push_back({ NAN, sec }); }
/** Horus: our preamble + the frame's 4-FSK symbols, tone k at f0 + k·spacing (a negative spacing = the tones reversed). */
inline void horus(const std::vector<uint8_t>& packet, double f0, double spacing, Keying& k, double baud = 100) {
    for (uint8_t b : horus::transmission(packet))
        for (int i = 0; i < 4; ++i) k.push_back({ f0 + ((b >> (6 - 2 * i)) & 3) * spacing, 1.0 / baud });
}
/** Real audio at `fs` (amplitude `amp`; a NaN segment is silence), continuous phase, appended. */
inline void render(const Keying& k, double fs, float amp, std::vector<float>& out, double& phase) {
    double acc = 0;
    for (const Seg& s : k) {
        acc += s.sec * fs;
        const long n = std::lround(acc); acc -= (double)n;
        if (std::isnan(s.hz)) { out.insert(out.end(), (size_t)n, 0.f); continue; }
        const double w = 2 * M_PI * s.hz / fs;
        for (long i = 0; i < n; ++i) { out.push_back(amp * (float)std::cos(phase)); phase += w; if (phase > M_PI) phase -= 2 * M_PI; }
    }
}
/** The same keying as complex baseband `offHz` above a dial (USB: audio f lands at dial + f), `fs` samples a second;
 *  calls `put(re, im)` per sample (so a long clip is never held). */
template <class Put> inline void renderIq(const Keying& k, double fs, double offHz, float amp, double& phase, Put&& put) {
    double acc = 0;
    for (const Seg& s : k) {
        acc += s.sec * fs;
        const long n = std::lround(acc); acc -= (double)n;
        const bool off = std::isnan(s.hz);
        const double w = off ? 0 : 2 * M_PI * (offHz + s.hz) / fs;
        for (long i = 0; i < n; ++i) {
            if (off) { put(0.f, 0.f); continue; }
            put(amp * (float)std::cos(phase), amp * (float)std::sin(phase));
            phase += w; if (phase > M_PI) phase -= 2 * M_PI; else if (phase < -M_PI) phase += 2 * M_PI;
        }
    }
}
inline std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

/** A UKHAS sentence for a made-up payload: "$$CALL,count,hh:mm:ss,lat,lon,alt,sats,temp*CRC\n". */
inline std::string sentence(const std::string& call, int count, int h, int m, int s, double lat, double lon, int alt, int sats = 9, int temp = -12) {
    char b[160];
    std::snprintf(b, sizeof b, "%s,%d,%02d:%02d:%02d,%.5f,%.5f,%d,%d,%d", call.c_str(), count, h, m, s, lat, lon, alt, sats, temp);
    return makeSentence(b);
}

/** A Horus v2 (or v1) packet for a made-up payload. */
inline std::vector<uint8_t> horusPacket(int version, int id, int count, int h, int m, int s, float lat, float lon, int alt) {
    horus::Fields f;
    f.id = id; f.count = count; f.h = h; f.m = m; f.s = s; f.lat = lat; f.lon = lon; f.alt = alt;
    f.speed = 42; f.sats = 11; f.temp = -31; f.batt = 150;
    for (int i = 0; i < 9; ++i) f.custom[i] = (uint8_t)(0x10 + i);
    return horus::makePacket(version, f);
}
/** ★ A made-up picture's blocks, w16 × h16 MCU columns/rows (subsampling `mode`): a gradient in the DC, a little detail in
 *  the first AC terms, colour that changes across it — the coefficient levels a real camera's JPEG at quality 4 has. */
inline std::vector<std::vector<int16_t>> picture(int w16, int h16, int mode, unsigned seed = 7) {
    const int yc = mode == 0 ? 4 : mode == 3 ? 1 : 2;
    const int mcus = w16 * h16 * (mode == 0 ? 1 : mode == 3 ? 4 : 2);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> small(-3, 3);
    std::vector<std::vector<int16_t>> b;
    b.reserve((size_t)mcus * (size_t)(yc + 2));
    for (int m = 0; m < mcus; ++m) {
        for (int p = 0; p < yc + 2; ++p) {
            std::vector<int16_t> c(64, 0);
            const double t = (double)m / mcus;
            if (p < yc) c[0] = (int16_t)std::lround(-40 + 80 * t + 6 * std::sin(m * 0.37 + p));
            else c[0] = (int16_t)std::lround((p == yc ? 12 : -9) * std::cos(t * 6.28));
            for (int k = 1; k < 6; ++k) c[(size_t)k] = (int16_t)small(rng);
            if (m % 7 == 0) c[20] = (int16_t)(small(rng) * 4);
            if (m % 11 == 0) c[63] = 1;
            b.push_back(c);
        }
    }
    return b;
}

} // namespace tx
} // namespace balloon
} // namespace vibe
