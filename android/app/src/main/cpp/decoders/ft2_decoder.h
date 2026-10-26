// VibeSDR V12 — FT2 decoder (2026-10-26): IU8LMC's fast mode, on the FT8/FT4 core.
//
// ★★★ FT2 IS FT4 AT TWICE THE SPEED. Decodium (IU8LMC's WSJT-X fork, GPL-3, where FT2 was defined in Feb 2026) builds
//  FT2 from FT4's frame unchanged — 4-GFSK BT 1.0, modulation index 1; 105 symbols = ramp + four 4-symbol Costas
//  groups (0,1,3,2 · 1,0,2,3 · 2,3,1,0 · 3,2,0,1) with 29 data symbols after each of the first three + ramp;
//  LDPC(174,91) + CRC-14; the 77-bit WSJT-X message XORed with FT4's sequence; FT4's Gray map — and only halves
//  the symbol: 288 samples at 12 kHz (24 ms, 41.67 baud, 41.67 Hz tones, ~167 Hz wide), the T/R period 3.75 s,
//  each transmission 2.52 s. Its stations spot on the same dials across the bands (FT2 dials, docs/v12/JS8-FT2.md).
//  ★ So FT2 here is FtxMode::FT2 of the FT8/FT4 decoder (ft8_decoder.h): the same STFT, Costas search (NEON/SSE2),
//    belief propagation, OSD and subtraction, at FT2's symbol length, with a 5 s window and a -0.53 … +1.54 s time
//    search (Decodium starts 0, 100 or 300 ms into the slot by version and platform). Facts only were taken from
//    Decodium's source; nothing of it is copied. Checked on real FT2 off the air (~/VibeSDR-test-audio/ft2/).
// ★ Messages are standard 77-bit WSJT-X messages: ftx_message_decode_std reads them exactly as for FT8/FT4, and the
//   call-hash table is the one FT8 and FT4 share.
#pragma once
#include "ft8_decoder.h"

namespace vibe {

/** The live FT2 decoder: Ft8Decoder in FT2 mode — slots every 3.75 s cut by the capture clock, its own worker thread,
 *  a slot still decoding when the next is ready SKIPPED and counted, onSpot(callTo, callDe, grid, snr, audioHz). */
class Ft2Decoder : public Ft8Decoder {
public:
    explicit Ft2Decoder(int sampleRate) : Ft8Decoder(sampleRate, FtxMode::FT2) {}
};

} // namespace vibe
