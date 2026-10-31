// VibeSDR — the benchmark's DECODER rows, and how many decoders this box can run at once (B6).
//
// ★★★ WHY (Stuart, 2026-09-30): "as part of the benchmark test advise how many simultaneous
//     decoders can be run — it may be that with this current load another 4 users could have
//     decoders but not all 10 at once." Every decoder the server has is timed HERE, on this box,
//     on a synthetic signal it actually decodes, and the figure the benchmark recommends becomes
//     the owner's default decoder limit (vibe_setup_page.h benchApplyDefaults and
//     ServerModeScreen.tsx — the two readers).
//  ★ The first rows (here): RTTY and NAVTEX (one FSK decoder, two alphabets and rates), WEFAX, SSTV, the time
//    signals (MSF/DCF77/RWM/WWV/WWVB/JJY — one decoder), FT8+FT4 (the digital spots, always run together), Band spots
//    and LoRa. ★★ EVERY OTHER DECODER (2026-10-31, Stuart: "all these decoders need their own lines in the initial
//    setup benchmark too") is in vibe_benchmark_more.h — and a NEW decoder adds its row there, in the same change.
//    Advanced RDS is not a decoder slot — it has its own row (rdsx_*) and its own switch.
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
#include "decoders/digi_spotter.h"   // ★ V12: the Band spots row
#include "decoders/digi_bands.h"
extern "C" {
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
}
#include "vibe_clock.h"
#include "vibe_lora_service.h"   // ★ V12: the LoRa row (ISM mode's 868 default channels)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <complex>
#include <cstdint>
#include <functional>
#include <mutex>
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

/** ★★ V12: a busy BAND as Band spots receives it — the IQ slice of `span` at `rate`, every registered mode's dial in it
 *  carrying `perDial` real transmissions of its own (FT8-family modes are encoded by ft8_lib: FT8 at 6.25 Hz / 0.16 s,
 *  FT4 at 20.833 Hz / 0.048 s), plus noise. Modes ft8_lib cannot encode are left quiet: their engine still searches. */
inline std::vector<std::complex<float>> busyBandSlice(const digi::Span& span, double rate, double seconds, int perDial) {
    std::vector<std::complex<float>> x((size_t)(rate * seconds), std::complex<float>(0, 0));
    auto put = [&](double offHz, double startSec, const uint8_t* tones, int nn, double sym) {
        double ph = 0; const size_t s0 = (size_t)(startSec * rate), ns = (size_t)std::llround(sym * rate);
        for (int k = 0; k < nn; ++k) for (size_t j = 0; j < ns; ++j) {
            const size_t i = s0 + (size_t)k * ns + j; if (i >= x.size()) return;
            ph += 2 * M_PI * (offHz + tones[k] / sym) / rate;
            x[i] += std::complex<float>((float)(0.01 * std::cos(ph)), (float)(0.01 * std::sin(ph)));
        }
    };
    int seq = 0;
    for (const auto& m : digi::modes()) {
        const bool ft4 = m.name == "FT4";
        if (m.name != "FT8" && !ft4) continue;
        for (double d : digi::dialsOf(m, span.band->id)) {
            if (d + m.audioLoHz < span.loHz || d + m.audioHiHz > span.hiHz) continue;
            for (int k = 0; k < perDial; ++k, ++seq) {
                char msg[32];
                std::snprintf(msg, sizeof msg, "CQ G4%c%c%c IO9%d", 'A' + (seq / 676) % 26, 'A' + (seq / 26) % 26, 'A' + seq % 26, seq % 10);
                ftx_message_t mm; ftx_message_init(&mm);
                if (ftx_message_encode(&mm, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
                uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
                if (ft4) ft4_encode(mm.payload, tones); else ft8_encode(mm.payload, tones);
                const double f = d + 300.0 + k * (3000.0 / std::max(1, perDial)) - span.centreHz;
                put(f, 0.5, tones, ft4 ? FT4_NN : FT8_NN, ft4 ? 0.048 : 0.16);
            }
        }
    }
    uint32_t r = 777;
    for (auto& v : x) {
        r = r * 1664525u + 1013904223u; const float a = ((float)(r >> 8) / 16777216.0f - 0.5f) * 0.03f;
        r = r * 1664525u + 1013904223u; const float b = ((float)(r >> 8) / 16777216.0f - 0.5f) * 0.03f;
        v += std::complex<float>(a, b);
    }
    return x;
}

/** One row. `decoded`: what it got out of its signal (−1 = not counted); `ms`: the wall time the row took (the total is
 *  shown to the owner — vibe_benchmark.h "benchSec"). */
struct Row { std::string id, label; double pct = -1; int decoded = -1; int ms = -1;
             /** ★ Rows scored by their hottest thread (AIS, ISM): which thread, and every thread's % plus "process". */
             std::string hottest; std::vector<std::pair<std::string, double>> threads; };
inline double rowClock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
/** ★ LoRa's rows (dec_lora, dec_lora_all): the threaded engine on the 868 plan in real time, scored by its hottest thread.
 *  Defined in vibe_benchmark_more.h (it needs the per-thread clock), which every user of this header also includes. */
inline Row loraRealTimeRow(const char* id, const char* label, double budgetUnits);

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
    double t0 = rowClock();
    const double s = std::max(4.0, seconds * 2);
    auto chunked = [](const std::vector<int16_t>& v, int blk, const std::function<void(const int16_t*, int)>& f) {
        for (size_t o = 0; o < v.size(); o += (size_t)blk) f(v.data() + o, (int)std::min<size_t>((size_t)blk, v.size() - o));
    };
    {
        t0 = rowClock(); if (step) step("Decoder: RTTY");
        const auto sig = fsk(s, 1000, 170, 45.45, 1);
        FskDecoder d(48000, 1000, 170, 45.45, "5N1.5", "ITA2", false);
        rows.push_back({ "dec_rtty", "Decoder: RTTY", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        t0 = rowClock(); if (step) step("Decoder: NAVTEX");
        const auto sig = fsk(s, 500, 170, 100, 2);
        FskDecoder d(48000, 500, 170, 100, "4/7", "CCIR476", false);
        rows.push_back({ "dec_navtex", "Decoder: NAVTEX", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        t0 = rowClock(); if (step) step("Decoder: WEFAX");
        const auto sig = wefax(s);
        WefaxDecoder::Config c; c.usePhasing = false; c.autoStart = false; c.autoStop = false;
        WefaxDecoder d(48000, c);
        rows.push_back({ "dec_wefax", "Decoder: WEFAX", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        t0 = rowClock(); if (step) step("Decoder: SSTV");
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
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        t0 = rowClock(); if (step) step("Decoder: time signals");
        const auto sig = timeSignal(s);
        TimeDecoder d(48000, TimeDecoder::Station::MSF);
        rows.push_back({ "dec_time", "Decoder: time signals (MSF, DCF77, RWM, WWV, WWVB, JJY)", timeIt(s, [&] { chunked(sig, 960, [&](const int16_t* p, int n) { d.process(p, n); }); }, [] {}) });
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        t0 = rowClock(); if (step) step("Decoder: FT8 + FT4");
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
        rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
    }
    {
        /* ★★ V12 DIGITAL SPOTS: BAND — its own row, because it is its own DEFAULT (vibe_setup_page.h benchApplyDefaults,
         *  ServerModeScreen.tsx applyBench: Band starts OFF where this row is not green; the owner can always switch it
         *  on). The busiest band the registry has — the one with the most dials in its passband — every dial carrying 15
         *  stations, one pass (what a loaded box pays), from a 96 kHz slice through the real tuners and engines.
         *  ★ Not counted as the HEAVIEST decoder for the decoder limit (vibe_benchmark.h): Band is one slot, and where it
         *    is too heavy it is off by default — letting it lower every other decoder's limit would punish the boxes it
         *    is not running on. */
        t0 = rowClock(); if (step) step("Decoder: digital spots, Band");
        const digi::Span* best = nullptr; digi::Span keep; size_t most = 0;
        for (const auto& b : digi::bands()) {
            digi::Span sp = digi::spanOf(b, digi::modes());
            if (sp.ok && sp.modes.size() > most) { most = sp.modes.size(); keep = sp; best = &keep; }
        }
        double pct = -1; std::atomic<int> got{0};
        if (best) {
            const double rate = 96000.0, slot0 = std::floor(vibeUtcNow() / 15.0) * 15.0;
            const auto x = busyBandSlice(*best, rate, 15.5, 15);
            {
                digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(),
                                 [&](const digi::Spot&, double) { got.fetch_add(1); }, nullptr, [] { return true; });
                size_t fed = 0;
                pct = timeIt(15.0, [&] {
                    for (size_t o = 0; o < x.size(); o += 2048) {
                        const int n = (int)std::min<size_t>(2048, x.size() - o);
                        fed += (size_t)n;
                        sp.feedIq(&x[o], n, rate, best->centreHz, slot0 + (double)fed / rate);
                    }
                }, [] { settleCpu(20.0); });
            }
            rows.push_back({ "dec_ft8band", std::string("Decoder: digital spots, Band (") + best->band->name + ")", pct, got.load() });
            rows.back().ms = (int)std::lround(1000.0 * (rowClock() - t0));
        }
    }
    {
        /* ★★ V12 LoRa (vibe_lora_service.h): ISM mode's 868 default channels (LoRaWAN 868.1/.3/.5 at SF7-12, Meshtastic
         *  LongFast, MeshCore) through the THREADED engine, fed in real time — vibe_benchmark_more.h loraRealTimeRow.
         *  ★★ SCORED BY ITS HOTTEST THREAD since LoRa runs on three (vibe-lora / lora2 / lora3, 2026-10-31), as ISM and AIS
         *     are: the inline total it used to report (~110 % on a Pi 2) read as "too heavy" for a set that keeps up there
         *     with no thread over ~55 %. The engine's own total (the three threads) stays in the row's detail, and THAT is
         *     what sets how many more channels run (vibelora::budgetFromBench via the shim's vsLoraBenchPct) — the same
         *     quantity the inline figure was, so the governor's arithmetic is unchanged. The default set is never hidden. */
        if (step) step("Decoder: LoRa (868 default channels)");
        rows.push_back(loraRealTimeRow("dec_lora", "Decoder: LoRa (868 default channels)", 0.0));
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
