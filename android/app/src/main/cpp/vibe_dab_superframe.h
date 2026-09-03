// vibe_dab_superframe.h — the DAB+ audio super frame header, and the two bits that decide the
// sample rate.
//
// Source: ETSI TS 102 563 V2.1.1, table 2 (he_aac_super_frame_header).
//
// ★★★ THIS IS THE CHIPMUNK BUG, AND IT IS TWO BITS WIDE.
//
//     Stuart, 2026-09-04, on why we are not shipping what OWRX ships: *"OWRX has mismatched sample
//     rates hence the chipmunks."* Here is exactly where that comes from. `dac_rate` and
//     `sbr_flag` together give FOUR cases:
//
//       dac_rate  sbr_flag   AUs   AAC core rate   OUTPUT rate
//          0         1        2       16 kHz          32 kHz
//          1         1        3       24 kHz          48 kHz
//          0         0        4       32 kHz          32 kHz
//          1         0        6       48 kHz          48 kHz
//
//     There are TWO ways to get this wrong and both are audible as chipmunks:
//       · assume 48 kHz when the service is 32 kHz  -> 1.5x too fast;
//       · take the CORE rate as the output rate when SBR is on -> 2x too fast.
//     Neither fails loudly. Both just sound silly, which is why it survives in shipped software.
//
// ★★ AND IT IS OUR PROBLEM EVEN THOUGH WE DO NOT DECODE DAB+. We hand the bitstream to the
//    browser, but WE synthesise the ADTS/mp4 header it reads — so the sampling-frequency index in
//    that header is ours to get right. Hand over the wrong index and the browser makes the
//    chipmunks on our behalf.
//
// ★ The number of access units is not incidental either: it is how the superframe is carved up,
//   so a wrong rate assumption also mis-slices the audio.
#pragma once

#include <cstdint>

namespace vibedab {

struct AudioFormat {
    int  coreRateHz;    ///< what the AAC core runs at
    int  outputRateHz;  ///< what comes OUT — doubled by SBR. THIS is the playback rate.
    int  accessUnits;   ///< AUs per super frame
    bool sbr;           ///< spectral band replication in use
};

/** Decode the two rate bits of he_aac_super_frame_header(). TS 102 563 table 2. */
inline constexpr AudioFormat dabPlusFormat(bool dacRate, bool sbrFlag) {
    if (!dacRate &&  sbrFlag) return { 16000, 32000, 2, true  };
    if ( dacRate &&  sbrFlag) return { 24000, 48000, 3, true  };
    if (!dacRate && !sbrFlag) return { 32000, 32000, 4, false };
    return                           { 48000, 48000, 6, false };
}

/** The MPEG-4 sampling-frequency index for an ADTS/ASC header.
 *  ★★ IT DESCRIBES THE CORE, NOT THE OUTPUT. With SBR the decoder is told the core rate and does
 *     the doubling itself; writing the OUTPUT rate here is the 2x chipmunk, committed by us in the
 *     one place we still touch the DAB+ path. Returns -1 for a rate MPEG-4 does not enumerate. */
inline constexpr int mpeg4SamplingFrequencyIndex(int rateHz) {
    switch (rateHz) {
        case 96000: return 0;  case 88200: return 1;  case 64000: return 2;
        case 48000: return 3;  case 44100: return 4;  case 32000: return 5;
        case 24000: return 6;  case 22050: return 7;  case 16000: return 8;
        case 12000: return 9;  case 11025: return 10; case 8000:  return 11;
        case 7350:  return 12;
        default:    return -1;
    }
}

/** ★ Plain DAB (MPEG-1/2 Layer II) rates, for completeness: 48 kHz, or 24 kHz for the LSF variant.
 *  This one WE decode (patents expired), so the output rate is whatever the frame header says and
 *  the same rule applies — never assume. */
inline constexpr bool isValidLayerIIRate(int rateHz) {
    return rateHz == 48000 || rateHz == 24000;
}

}  // namespace vibedab
