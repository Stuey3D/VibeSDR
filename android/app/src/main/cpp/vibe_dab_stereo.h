// vibe_dab_stereo.h — is the PLAYING DAB service stereo? What the stereo light beside the mode says.
//
// ★★★ STUART, 2026-10-01: "the stereo icon from WFM also is stuck when in DAB mode even when on Mono
//     stations." The light was the FM PILOT's — a 19 kHz tone DAB does not have — so in DAB it showed
//     whatever the last FM station left it at. A DAB service says what it is in its own audio headers,
//     and this is the reading of them. Sent in the `dab` state as "stereo" (see DabService::jsonLocked_),
//     beside the codecDetail text a person reads.
// ★ Pure, so vibeserver/test-dab-stereo.cpp holds the table down.
#pragma once

namespace vibedab {

/** DAB+ (HE-AAC, TS 102 563): the super frame header's aac_channel_mode (the CORE is two channels) and
 *  ps_flag (HE-AAC v2 PARAMETRIC STEREO — a mono core with the second channel rebuilt from side
 *  information: what comes out, and what a listener hears, is stereo). */
inline bool dabAacIsStereo(bool coreStereo, bool ps) { return coreStereo || ps; }

/** DAB (MPEG-1/2 Layer II, EN 300 401 7.3): the frame header's mode field.
 *    0 stereo ............ stereo
 *    1 joint stereo ...... stereo (intensity-coded above the bound — still a stereo programme)
 *    2 dual channel ...... NOT stereo: two independent mono programmes (e.g. two languages)
 *    3 single channel .... mono */
inline bool dabMp2ModeIsStereo(int mode) { return mode == 0 || mode == 1; }

}  // namespace vibedab
