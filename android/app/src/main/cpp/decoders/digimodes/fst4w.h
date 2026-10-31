// decoders/digimodes/fst4w.h — FST4W (the beacon mode, WSPR's successor) as a digital-spots mode: our own
// fst4::LiveDecoder in FST4W mode (digimodes/fst4.h has the engine). V12, 2026-10-31.
// ★ Periods 120, 300, 900 and 1800 s; WSPR's sub-band (1400–1600 Hz above WSPR's dials — digi_bands.h's "FST4W" rows),
//   so on Band (and the background Band profile) it runs beside WSPR on LF, MF and HF.
#pragma once
#include "fst4.h"

namespace vibe {
namespace digi {

inline ModeDef vibeDigiMode_fst4w() {
    ModeDef m;
    m.name = "FST4W";
    m.slotSec = 120.0;
    m.sampleRate = 12000;
    m.audioLoHz = 1350; m.audioHiHz = 1650;     // ★ as WSPR's: the search 1400–1600 is flat passband
    m.costVsFt8 = 0.25; m.lightCostVsFt8 = 0.2; // ★ Mac ≈ 10 ms a minute (a 1.5 kHz mix, 4 resamplers, rare decodes)
    m.priority = 100;
    m.singleDialTolHz = 150;                    // ★ Single mode: only on a WSPR/FST4W dial (as WSPR)
    m.make = [] { return std::unique_ptr<Engine>(new Fst4Engine(true)); };
    return m;
}

} // namespace digi
} // namespace vibe
