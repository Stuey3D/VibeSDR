// VibeSDR — the benchmark's DECODER rows, and how many decoders this box can run at once (B6).
//
// ★★★ WHY (Stuart, 2026-09-30): "as part of the benchmark test advise how many simultaneous
//     decoders can be run — it may be that with this current load another 4 users could have
//     decoders but not all 10 at once." Every decoder the server has is timed HERE, on this box,
//     on a synthetic signal it actually decodes, and the figure the benchmark recommends becomes
//     the owner's default decoder limit (vibe_setup_page.h benchApplyDefaults and
//     ServerModeScreen.tsx — the two readers).
//  ★ The decoders the server has (local_sdr_shim.cpp, vibe_decoder_host.h): RTTY and NAVTEX (one
//    FSK decoder, two alphabets and rates), WEFAX, SSTV, the time signals (MSF/DCF77/RWM/WWV/WWVB —
//    one decoder), and FT8+FT4 (the digital spots, always run together). Advanced RDS is not a
//    decoder slot — it has its own row (rdsx_*) and its own switch.
//  ★★ AVERAGE CPU, NOT PEAK. Decoders run at the LOWEST priority behind a drop-never-wait queue
//     (vibe_thread.h's order), so a burst is absorbed — what must fit is the sustained load. FT8
//     is the bursty one: nothing for 14 s, then a whole slot's decode; its row is the slot's total
//     CPU over the slot's 15 s.
//  ★ Measured as PROCESS CPU (getrusage) across the row, because SSTV and FT8 decode on threads of
//    their own. The radio is stopped while the benchmark runs, so nothing else of ours is counted.
#pragma once
#include "decoders/fsk_decoder.h"
#include "decoders/wefax_decoder.h"
#include "decoders/sstv_decoder.h"
#include "decoders/time_decoder.h"
#include "decoders/ft8_decoder.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include "vibe_clock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>

namespace vibe {
namespace benchdec {

inline double processCpu() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}
/** Continuous-phase FSK at 48 kHz: `bits` at `baud`, mark/space either side of `cf`. */
inline std::vector<int16_t> fsk(double seconds, double cf, double shift, double baud, unsigned seed) {
    const double fs = 48000, spb = fs / baud;
    std::vector<int16_t> out((size_t)(fs * seconds));
    double ph = 0; uint32_t r = seed;
    bool bit = true; double left = spb;
    for (auto& v : out) {
        if ((left -= 1.0) <= 0) { left += spb; r = r * 1664525u + 1013904223u; bit = (r >> 28) & 1; }
        ph += 2 * M_PI * (cf + (bit ? shift / 2 : -shift / 2)) / fs;
        v = (int16_t)(12000 * std::sin(ph));
    }
    return out;
}
/** WEFAX: an FM subcarrier 1500-2300 Hz carrying a picture. */
inline std::vector<int16_t> wefax(double seconds) {
    const double fs = 48000;
    std::vector<int16_t> out((size_t)(fs * seconds));
    double ph = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = i / fs, grey = 0.5 + 0.5 * std::sin(2 * M_PI * 7.0 * t) * std::cos(2 * M_PI * 0.3 * t);
        ph += 2 * M_PI * (1500.0 + 800.0 * grey) / fs;
        out[i] = (int16_t)(12000 * std::sin(ph));
    }
    return out;
}
/** SSTV at 12 kHz: a Robot 36 VIS header (so the decoder commits to a picture and its video thread
 *  runs), then lines — a 1200 Hz sync and a varying luminance — for the rest. */
inline std::vector<int16_t> sstv(double seconds) {
    const double fs = 12000;
    std::vector<int16_t> out((size_t)(fs * seconds));
    size_t i = 0; double ph = 0;
    auto tone = [&](double hz, double sec) {
        for (int k = 0; k < (int)(sec * fs) && i < out.size(); ++k, ++i) { ph += 2 * M_PI * hz / fs; out[i] = (int16_t)(12000 * std::sin(ph)); }
    };
    tone(1900, 0.3); tone(1200, 0.01); tone(1900, 0.3);
    tone(1200, 0.03);                                     // start bit
    const int vis = 8;                                    // Robot 36
    int parity = 0;
    for (int b = 0; b < 7; ++b) { const int bit = (vis >> b) & 1; parity ^= bit; tone(bit ? 1100 : 1300, 0.03); }
    tone(parity ? 1100 : 1300, 0.03);
    tone(1200, 0.03);                                     // stop bit
    for (int line = 0; i < out.size(); ++line) {
        tone(1200, 0.009); tone(1500, 0.003);
        for (int px = 0; px < 32 && i < out.size(); ++px) tone(1500 + 800 * (0.5 + 0.5 * std::sin(px * 0.3 + line * 0.1)), 0.088 / 32);
        tone(line % 2 ? 2300 : 1500, 0.0045); tone(1900, 0.0015);
        for (int px = 0; px < 16 && i < out.size(); ++px) tone(1900 + 200 * std::sin(px * 0.5), 0.044 / 16);
    }
    return out;
}
/** A time-signal carrier as the decoder hears it in CW: a 600 Hz beat keyed off 100/200 ms a second. */
inline std::vector<int16_t> timeSignal(double seconds) {
    const double fs = 48000;
    std::vector<int16_t> out((size_t)(fs * seconds));
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = i / fs, frac = t - std::floor(t);
        const bool off = frac < (((long)t % 3) ? 0.1 : 0.2);
        out[i] = off ? 0 : (int16_t)(12000 * std::sin(2 * M_PI * 600.0 * t));
    }
    return out;
}
/** A busy FT8 band at 12 kHz: `n` REAL FT8 transmissions (CQ calls, encoded by ft8_lib), each on
 *  its own offset, plus noise — the candidate search AND the LDPC decode a busy 20 m evening causes.
 *  ★ They must decode: a band of random tones costs only the search, and the row would read low. */
inline std::vector<int16_t> ft8Band(int n, double seconds) {
    const double fs = 12000;
    std::vector<double> acc((size_t)(fs * seconds), 0.0);
    for (int s = 0; s < n; ++s) {
        char msg[32];
        std::snprintf(msg, sizeof msg, "CQ G4A%c%c IO9%d", 'A' + (s / 26) % 26, 'A' + s % 26, s % 10);
        ftx_message_t m; ftx_message_init(&m);
        if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
        uint8_t tones[FT8_NN]; ft8_encode(m.payload, tones);
        double ph = 0; const double f0 = 300.0 + s * 120.0;
        const size_t start = (size_t)(0.5 * fs), spsym = (size_t)(0.16 * fs);
        for (int k = 0; k < FT8_NN; ++k)
            for (size_t j = 0; j < spsym; ++j) {
                const size_t i = start + (size_t)k * spsym + j;
                if (i >= acc.size()) break;
                ph += 2 * M_PI * (f0 + tones[k] * 6.25) / fs;
                acc[i] += std::sin(ph);
            }
    }
    std::vector<int16_t> out(acc.size());
    uint32_t r = 12345;
    for (size_t i = 0; i < acc.size(); ++i) {
        r = r * 1664525u + 1013904223u;
        const double v = acc[i] / std::max(1, n) * 0.6 + ((double)(r >> 8) / 16777216.0 - 0.5) * 0.05;
        out[i] = (int16_t)(20000 * v);
    }
    return out;
}

struct Row { std::string id, label; double pct = -1; int decoded = -1; };

/** Time one decoder: `seconds` of its signal, fed as fast as it will take it; `settle` waits for
 *  work a decoder does on a thread of its own to finish before the clock stops. */
template <class Feed, class Settle>
inline double timeIt(double audioSeconds, Feed feed, Settle settle) {
    const double c0 = processCpu();
    feed();
    settle();
    const double used = processCpu() - c0;
    return audioSeconds > 0 ? 100.0 * used / audioSeconds : -1;
}
/** Wait until this process stops spending CPU (a decoder thread has finished), at most `capSec`. */
inline void settleCpu(double capSec) {
    const auto t0 = std::chrono::steady_clock::now();
    double last = processCpu(); int quiet = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < capSec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double now = processCpu();
        quiet = (now - last) < 0.005 ? quiet + 1 : 0;
        last = now;
        if (quiet >= 3) break;
    }
}

/** Every decoder row. `seconds` scales the text/image rows; FT8 always runs one whole slot pair. */
inline std::vector<Row> runDecoderRows(double seconds, const std::function<void(const std::string&)>& step) {
    std::vector<Row> rows;
    const double s = std::max(4.0, seconds * 2);
    auto chunked = [](const std::vector<int16_t>& v, int blk, const std::function<void(const int16_t*, int)>& f) {
        for (size_t o = 0; o < v.size(); o += (size_t)blk) f(v.data() + o, (int)std::min<size_t>((size_t)blk, v.size() - o));
    };
    {
        if (step) step("Decoder: RTTY");
        const auto sig = fsk(s, 1000, 170, 45.45, 1);
        FskDecoder d(48000, 1000, 170, 45.45, "5N1.5", "ITA2", false);
        rows.push_back({ "dec_rtty", "Decoder: RTTY", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
    }
    {
        if (step) step("Decoder: NAVTEX");
        const auto sig = fsk(s, 500, 170, 100, 2);
        FskDecoder d(48000, 500, 170, 100, "4/7", "CCIR476", false);
        rows.push_back({ "dec_navtex", "Decoder: NAVTEX", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
    }
    {
        if (step) step("Decoder: WEFAX");
        const auto sig = wefax(s);
        WefaxDecoder::Config c; c.usePhasing = false; c.autoStart = false; c.autoStop = false;
        WefaxDecoder d(48000, c);
        rows.push_back({ "dec_wefax", "Decoder: WEFAX", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
    }
    {
        if (step) step("Decoder: SSTV");
        const double ss = std::max(s, 8.0);
        const auto sig = sstv(ss);
        std::atomic<int> modes{0};
        double pct;
        {
            SstvDecoder d(12000, true);
            d.onMode = [&](uint8_t, const std::string&) { modes.fetch_add(1); };
            pct = timeIt(ss, [&] { chunked(sig, 240, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] { settleCpu(10.0); });
        }
        rows.push_back({ "dec_sstv", modes.load() ? "Decoder: SSTV" : "Decoder: SSTV (listening)", pct });
    }
    {
        if (step) step("Decoder: time signals");
        const auto sig = timeSignal(s);
        TimeDecoder d(48000, TimeDecoder::Station::MSF);
        rows.push_back({ "dec_time", "Decoder: time signals", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
    }
    {
        if (step) step("Decoder: FT8 + FT4");
        /* ★ The spotter cuts UTC slots by each block's CAPTURE time (2026-10-05), so the band is fed with
         *  stamps of its own: sample 0 on a slot boundary, as if heard in real time. No clock is touched.
         *  ★★ ONE PASS: the extra passes only ever run on spare CPU and stop when the server is loaded, so the
         *     cost a decoder limit must budget for is the single pass — what a loaded box actually pays. */
        const double slot0 = std::floor(vibeUtcNow() / 15.0) * 15.0;
        const auto band = ft8Band(20, 15.5);
        double pct;
        std::atomic<int> got{0};
        {
            Ft8Decoder ft8(12000, false), ft4(12000, true);
            ft8.maxPasses = 1; ft4.maxPasses = 1;
            ft8.onSpot = [&](const std::string&, const std::string&, const std::string&, int, float) { got.fetch_add(1); };
            size_t fed = 0;
            pct = timeIt(15.0, [&] { chunked(band, 240, [&](const int16_t* p, int n) {
                                         fed += (size_t)n;
                                         const double utc = slot0 + (double)fed / 12000.0;
                                         ft8.process(p, n, utc); ft4.process(p, n, utc); }); },
                         [] { settleCpu(20.0); });
        }
        rows.push_back({ "dec_ft8", "Decoder: FT8 + FT4 spots", pct, got.load() });
    }
    return rows;
}

/** ★★★ HOW MANY AT ONCE. The spare cores' green budget (70 % each, all but the one the shared
 *  front holds — the same budget as the locked-range listener estimate) MINUS a full house of
 *  listeners, divided by the HEAVIEST decoder: a limit must hold whatever mix people pick, and
 *  they pick FT8. `plannedUsers` is the listener load the decoders must leave room for.
 *  ★ At least 1 — a box that can run a listener can run one decoder at the lowest priority; at
 *    most 20, past which the uplink and the listener cap decide, not the CPU. */
inline int recommendDecoders(long cores, double perListenerPct, int plannedUsers, double heaviestPct) {
    const double budget = 70.0 * std::max(1L, cores - 1);
    const double left = budget - std::max(0.0, perListenerPct) * std::max(0, plannedUsers);
    if (heaviestPct <= 0) return 1;
    return std::max(1, std::min(20, (int)std::floor(left / heaviestPct)));
}

} // namespace benchdec
} // namespace vibe
