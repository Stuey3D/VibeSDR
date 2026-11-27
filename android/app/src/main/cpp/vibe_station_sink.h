// vibe_station_sink.h — what AIS and APRS hand to whoever keeps their station lines (V12: the gallery's day log and summary
// card, vibe_station_log.h). Its own tiny header — as ISM's vibe_ism_sink.h — so the decoders know nothing about files.
#pragma once
#include <cmath>
#include <string>
#include <algorithm>

namespace vibestation {

/** One new line for the log: the station's key (MMSI, callsign), what it is, its name and its readable line. */
struct Heard {
    std::string key;
    std::string kind;     ///< "A", "B", "base", "aton", "sar" (AIS) · "position", "weather", "object", "message" … (APRS)
    std::string name;
    std::string line;
    double freqHz = 0;
    /** ★ beta96 (Stuart 11-26: "yes save snr history") the message's SNR, dB (its decoder's measure) — the day log writes it
     *  as " · SNR 18 dB" at the line's end (snrToken), which the gallery viewers draw as the colour-coded chip. NAN = none. */
    double snr = NAN;
};
/** " · SNR 18 dB" (whole dB, U+2212 for a minus, as the clients' chips) — or "" for none. Bounded: −99…199. */
inline std::string snrToken(double snr) {
    if (!(snr == snr)) return std::string();
    const long v = std::lround(std::max(-99.0, std::min(199.0, snr)));
    return std::string(" \xc2\xb7 SNR ") + (v < 0 ? "\xe2\x88\x92" : "") + std::to_string(v < 0 ? -v : v) + " dB";
}
/** ★ Called on the decoder's OWN thread: heard() per new line, tick() about once a second, finish() once at the stop. */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const Heard& h) = 0;
    virtual void tick() {}
    virtual void finish() {}
};

} // namespace vibestation
