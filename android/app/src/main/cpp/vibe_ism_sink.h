// vibe_ism_sink.h — what ISM mode hands to whoever keeps its readings (V12, 2026-10-25: the gallery's daily ISM log and
// summary card, vibe_ism_gallery.h). Its own tiny header so the decoder service (vibe_ism_service.h) knows nothing about
// files, and the gallery knows nothing about the decoders.
//
// ★★★ PRIVACY (Stuart, 2026-10-25): a Heard carries ONLY what the device list already shows — the kind, the device's
//     name and its readable line, exactly as vibeism::Tracker built them. A TPMS / key fob / fixed-code remote /
//     "Suspected key fob" is its anonymous per-server name ("TPMS #2"); there is no field here an ID, a code or a bit
//     could travel in.
#pragma once
#include <string>

namespace vibeism {

/** One reading the device list counted as NEW (Tracker::accept returned true — a repeat of the same line inside 3 s is
 *  not one). */
struct Heard {
    std::string kind;     ///< weather | tpms | keyfob | suspectfob | doorbell | sensor | energy
    std::string name;     ///< "Oregon Scientific THGR810" / "TPMS #2"
    std::string line;     ///< the list's readable line, the server's units (metric, PSI) — what every client shows by default
    double freqHz = 0;    ///< the burst's frequency
    bool secret = false;  ///< a kind whose identity never leaves the Tracker (its name is the nickname)
};

/** ★ Called on the ISM decoder's OWN thread (never the DSP thread): heard() for each new reading, tick() about once a
 *  second, finish() once when the service stops (a band change, leaving ISM, a clean shutdown). */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const Heard& h) = 0;
    virtual void tick() {}
    virtual void finish() {}
};

} // namespace vibeism
