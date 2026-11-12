// vibe_ism_fobs.h — V12 ISM decoder, second pass (2026-11-12): CAR KEY FOBS and CAR ALARMS, GARAGE / GATE OPENERS, TV
// remotes and other REMOTES with a known frame. The first pass is vibe_ism_remotes.h (remoteReading, the same shapes).
//
// ★★★ SOURCES: frame FACTS only — from the protocol notes at the head of rtl_433's device files and conf/ descriptions
//     (GPL: read for the frame layout, timing and check, nothing copied — every line here is our own), datasheets and
//     the rtl_433_tests READMEs. Each family says where its facts came from.
// ★★★ PRIVACY (Stuart, 2026-10-25): a remote's code IS the key of a car, a gate, a garage — SECRET: Reading::secret +
//     secretId (hashed by the Tracker), never pubId. A rolling / hopping code is never decoded beyond what identifies the
//     family, and never shown; the fixed part (serial) only ever becomes the hashed secret id. Shown: make, button, battery.
// ★ Secret-id family tags here: 0xD0… (the remotes use 20–43, TPMS 0x40…, vibe_ism_security2.h 0xC0…).
#pragma once
#include "vibe_ism_remotes.h"

namespace vibeism {

/** The second-pass remote / fob families, strongest checks first. */
inline bool decodeFobs(const Burst& b, std::vector<Reading>& out) {
    (void)b; (void)out;
    return false;
}

} // namespace vibeism
