// decoders/digi_mode.h — THE INTERFACE A DIGITAL-SPOTS MODE IMPLEMENTS (V12, 2026-10-26). FT8 and FT4 today; WSPR, JS8,
// FT2 (and later Q65 / FST4 / FST4W / MSK144) plug in here without the host, the shim or the clients changing.
//
// ★★★ WHAT A MODE IS, TO THE SPOTTER. A slotted (or free-running) decoder that is handed REAL, USB AUDIO at its own
//     sample rate with ITS DIAL at audio 0 Hz — exactly what WSJT-X hands its decoders off a radio set to that dial in
//     USB — and reports each decoded transmission as a Spot. It never sees IQ, a socket, a band or another mode:
//       • Single mode ("Digital spots: Single mode"): the listener's own audio, decimated to the mode's rate; their dial
//         is the mode's dial. Every registered mode whose `single` is true runs on it (FT8 + FT4 today, as before).
//       • Band mode ("Digital spots: Band"): ONE wide IQ slice of the band, and for every (mode, dial) inside it a
//         DigiTuner (decoders/digi_tuner.h) cuts that dial's audio passband out of the slice — so every mode reads its
//         own slice of the band at once, from one channeliser extract.
//
// ★★ ADDING A MODE IS ONE FILE: decoders/digimodes/<name>.h, holding `inline ModeDef vibeDigiMode_<name>()` (and the
//    engine's .cpp files beside it in decoders/digimodes/, which every build globs). The registry
//    (decoders/digi_modes.cpp) already includes WSPR, JS8, FT2, Q65, FST4, FST4W and MSK144 by __has_include, so for
//    those the header IS the whole change. The dial frequencies come from decoders/digi_bands.h by the mode's NAME
//    (published dials, already typed in); a mode with dials of its own lists them in ModeDef::dials instead.
//    docs/v12/DIGITAL-SPOTS.md is the long form, with a worked example.
//
// ★ THREADS. process() is called on the host's DECODE thread (vibe-decode — lowest priority, behind a drop-never-wait
//   queue), in order, never concurrently with itself. It must return quickly: a slot's decode belongs on the engine's
//   OWN worker thread (as Ft8Decoder does) — and if the previous slot is still decoding when the next is complete, the
//   new slot is SKIPPED AND COUNTED (slotsSkipped), never waited for. onSpot may be called on the engine's thread.
// ★ OFF-AIR TEXT. Anything a transmitter sent is untrusted: the host whitelists `call` and `grid` again before they go
//   anywhere, but an engine must not hand over a hashed/unresolved call ("<...>") as a callsign — report only calls it
//   actually decoded.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace digi {

/** One decoded transmission. */
struct Spot {
    std::string mode;     ///< "FT8", "FT4", "WSPR", "JS8", "FT2", "Q65-60A", … — the ModeDef's name unless a submode
    std::string call;     ///< the SENDER's callsign as decoded (no brackets, no hash placeholders)
    std::string grid;     ///< the sender's OWN 4- or 6-character locator, or "" (never a report such as "-12" or "RR73")
    std::string text;     ///< the decoded message, if the engine has it ("" = none)
    /** ★ V12 (2026-10-27): what the station was doing, for the modes that say — "CQ" (calling), "QSO" (in an exchange),
     *  "DE" / "TEST" (identifying) — JTTY and PSK. "" for the slotted modes, whose text already says it. */
    std::string how;
    int    snr = 0;       ///< dB in 2500 Hz (WSJT-X's convention)
    float  audioHz = 0;   ///< audio frequency above the DIAL (USB) — RF = dial + audioHz
    float  dt = 0;        ///< seconds from the slot's nominal start (0 for a free-running mode)
    /** ★ UTC (s) of the START of the slot this transmission was heard in — what WSJT-X's time column shows, and what the
     *  spot is stamped with (M7EIW: HH:MM:SS tells which transmission block it was). NaN = unknown: stamped now. */
    double slotUtc = NAN;
};

/** One running decoder of one mode on one dial. Made by ModeDef::make; owned by the host. */
class Engine {
public:
    virtual ~Engine() = default;
    /** `n` samples of real USB audio at the ModeDef's sampleRate, ±1.0 full scale, the dial at 0 Hz. `captureUtc` is the
     *  corrected UTC (vibeUtcNow) at which the LAST sample was captured — cut slots by it, not by the decode thread's
     *  clock (the host's queue may run seconds behind). Return quickly; see the thread note above. */
    virtual void process(const float* audio, int n, double captureUtc) = 0;
    /** Slots skipped because the previous one was still decoding — the health pill's "TOO HEAVY" reads it. */
    virtual uint64_t slotsSkipped() const { return 0; }
    /** Slots decoded so far (tests, the admin page). */
    virtual uint64_t slotsDecoded() const { return 0; }

    /** Set by the host before audio flows. May be called from any thread. */
    std::function<void(const Spot&)> onSpot;
    /** ★ The server's load, asked before any OPTIONAL extra work (a second decode pass, OSD): true = loaded, do the
     *  cheapest decode. Empty = never loaded. */
    std::function<bool()> loaded;
};

/** Everything the spotter needs to know about a mode. Plain data and a factory — no global state. */
struct ModeDef {
    std::string name;           ///< "FT8" — the name on the wire, in the list's Mode column and on the map's legend
    double slotSec = 15.0;      ///< T/R period (15 FT8, 7.5 FT4, 120 WSPR, …); 0 = free-running (MSK144)
    int    sampleRate = 12000;  ///< the audio rate process() is fed at
    float  audioLoHz = 200;     ///< the audio passband the engine searches, above the dial (USB)…
    float  audioHiHz = 3000;    ///< …and its top. Band mode cuts exactly this (plus a guard) out of the slice.
    bool   single = true;       ///< also run in Single mode on the listener's own audio (false: Band mode only)
    /** ★ Single mode runs a mode only at its own dial (Spotter::atOwnDial). > 0: "at" = the listener's dial within this
     *  many Hz of one of its dials (a narrow mode on a dial of its own — WSPR's 200 Hz at 1500 Hz above 7.0386 …).
     *  0: "at" = half the listener's USB window inside this mode's audio window above one of its dials (FT8, JS8 …). */
    double singleDialTolHz = 0;
    /** ★ Single mode, tuned to NO mode's dial: the modes that run anyway — FT8 and FT4, what Single always ran, so it
     *  never goes silent off the table. */
    bool singleFallback = false;
    /** Relative CPU of one engine, FT8 = 1.0 (one decode per slot, averaged over the slot). The benchmark's Band row and
     *  the docs use it to cost a band; it never gates anything by itself. */
    double costVsFt8 = 1.0;
    /** ★★ V12 BAND GOVERNOR (decoders/digi_governor.h, 2026-10-27). The same cost when the engine is told it is `loaded`
     *  (one pass, no OSD / AP / coherent stage) — the depth the governor drops BEFORE it drops a mode. < 0 = the same as
     *  costVsFt8 (an engine with no lighter setting). */
    double lightCostVsFt8 = -1.0;
    /** ★★ The governor's PRIORITY: lower runs first, and when a band is over budget the HIGHEST number is shed first.
     *  FT8 10 · FT4 20 · WSPR 30 · JS8 40 · FT2 50 · JTTY 60 · PSK 70 · a mode that says nothing (Q65, FST4, FST4W,
     *  MSK144 as their engines land) 100 — docs/v12/DIGITAL-SPOTS.md "The Band governor" says why. It also orders the
     *  band's span: a mode never pushes a higher-priority one out of the 48 kHz cluster. */
    int priority = 100;
    /** ★ The activity gate's period (s): how long a window's energy is judged over before its audio is decoded or
     *  dropped. 0 = the slot (FT8 15, FT4 7.5, WSPR 120); a free-running mode (slotSec 0) says its own (JTTY, PSK 4). */
    double gateSec = 0;
    double lightCost() const { return lightCostVsFt8 >= 0 ? lightCostVsFt8 : costVsFt8; }
    /** ★ Own dials, { band id ("20m"), dial Hz }. EMPTY = use the published table in digi_bands.h for `name`. */
    std::vector<std::pair<std::string, double>> dials;
    /** A fresh engine. Called on a socket thread when spots start or the band changes; may throw (the host catches). */
    std::function<std::unique_ptr<Engine>()> make;
    /** ★ Optional: a fresh engine for ONE dial (Hz) — a mode whose set-up depends on where it listens (Q65: its 15 s
     *  period only on 6 m and 2 m). The spotter prefers it to `make` when set. `variantAt` says which set-up a dial gets,
     *  so Single remakes the engine only when the listener's dial moves to a dial that needs a different one. */
    std::function<std::unique_ptr<Engine>(double dialHz)> makeAt;
    std::function<int(double dialHz)> variantAt;
    std::unique_ptr<Engine> makeFor(double dialHz) const { return makeAt ? makeAt(dialHz) : make(); }
};

/** The registered modes, in registration order (FT8 first). Built once, never changed after — read freely. */
const std::vector<ModeDef>& modes();
/** A registered mode by name (case-insensitive), or null. */
const ModeDef* modeByName(const std::string& name);

} // namespace digi
} // namespace vibe
