// vibe_sat_offer.h — ★★ beta94 IS SATELLITE MODE OFFERED, FOR THE PUBLISHED `modes` (Stuart 11-25: "the directory page
// should have a little satellite icon for servers that have satellite mode enabled").
//
// "sat" goes into the radio's published `modes` (vibeserver.json → the directory listing, as "adsb" does) only where a
// listener could actually use SATELLITE mode on this radio:
//   · it receives at least one listed downlink — the same rule as hwinfo's `doppler` (vsSatAnyReach: never the R36S,
//     never a radio below every downlink, never a locked window holding none);
//   · the server knows where it is — no observer = no pass can be predicted, and the button stays greyed until the
//     OWNER sets a locator; a listing must not promise that;
//   · a pick has a demodulator to land in — nearly every downlink is NFM, USB or LSB (vibe::sat::listenModeOf), so all
//     three switched off is no (that also covers every "only mode" receiver: onlyModeNow needs every demodulator off);
//   · "sat" itself in the owner's blocked list is honoured, should it ever be written there.
// ★ NOT gated on the orbit data or the clock: both arrive by themselves minutes after a start, and a listing sent in
//   those minutes would drop the badge until the next ping (the DAB+ badge's old after-a-reboot fault, directory.cpp).
//
// Pure, so test-sat-offer can hold the rule without the whole shim; the shim passes vsSatAnyReach, the engine's
// haveObserver and vsModeBlocked.
#pragma once

namespace vibe { namespace sat {

template <class Blocked>
inline bool offeredInModes(bool reachesADownlink, bool located, const Blocked& blocked) {
    if (!reachesADownlink || !located || blocked("sat")) return false;
    return !blocked("nfm") || !blocked("usb") || !blocked("lsb");
}

} }  // namespace vibe::sat
