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
    std::string kind;     ///< weather | tpms | keyfob | suspectfob | doorbell | sensor | energy | meter
    std::string name;     ///< "Oregon Scientific THGR810" / "TPMS #2"
    std::string line;     ///< the list's readable line, the server's units (metric, PSI) — what every client shows by default
    double freqHz = 0;    ///< the burst's frequency
    bool secret = false;  ///< a kind whose identity never leaves the Tracker (its name is the nickname)
    /** ★ V12 meters (kind "meter"): the line WITHOUT its signal and interval — the meter's own news. The day's log writes a
     *  meter only when this changes, or once per 15 minutes (vibe_ism_gallery.h). Empty for every other kind. */
    std::string core;
};

/** ★ V12 2026-11-01 (the ISM map, "stations received" in the gallery): one packet from a MESH NODE WITH A POSITION.
 *  ★★★ The position is already the PUBLISHED one (vibe_lora_track.h publishedPos: a person's node rounded to ~1 km and
 *      `approx`); there is no field a message's text could travel in. */
struct NodeFix {
    std::string net;      ///< "meshcore" | "meshtastic"
    std::string type;     ///< "repeater" | "room" | "sensor" | "companion"
    std::string name;     ///< the node's advertised name (or its id)
    std::string node;     ///< its id ("!a1b2c3d4" / a MeshCore key prefix) — what makes two names one node
    double lat = 0, lon = 0;
    bool approx = false;
    int hops = -1;
    float snr = 0;
    double freqHz = 0;
};

/** ★ Called on the ISM decoder's OWN thread (never the DSP thread): heard() for each new reading, tick() about once a
 *  second, finish() once when the service stops (a band change, leaving ISM, a clean shutdown). */
class Sink {
public:
    virtual ~Sink() = default;
    virtual void heard(const Heard& h) = 0;
    /** ★ V12: a mesh node with a position was heard (every packet, not only news — the card counts them). */
    virtual void node(const NodeFix&) {}
    virtual void tick() {}
    virtual void finish() {}
};

} // namespace vibeism
