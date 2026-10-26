// vibe_station_sink.h — what AIS and APRS hand to whoever keeps their station lines (V12: the gallery's day log and summary
// card, vibe_station_log.h). Its own tiny header — as ISM's vibe_ism_sink.h — so the decoders know nothing about files.
#pragma once
#include <string>

namespace vibestation {

/** One new line for the log: the station's key (MMSI, callsign), what it is, its name and its readable line. */
struct Heard {
    std::string key;
    std::string kind;     ///< "A", "B", "base", "aton", "sar" (AIS) · "position", "weather", "object", "message" … (APRS)
    std::string name;
    std::string line;
    double freqHz = 0;
};
/** ★ Called on the decoder's OWN thread: heard() per new line, tick() about once a second, finish() once at the stop. */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const Heard& h) = 0;
    virtual void tick() {}
    virtual void finish() {}
};

} // namespace vibestation
