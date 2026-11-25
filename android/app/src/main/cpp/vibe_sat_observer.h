// vibe_sat_observer.h — ★★ beta94 WHERE THE SATELLITE ENGINE'S SKY IS COMPUTED FROM (tester Mac Ha, Stuart 11-25).
//
// Two sources, one rule:
//   OWNER  — the server's published location (LocalSdrShim::setLocationJson: the setup page's locator, the app's home or
//            moving place). It is what listeners are told; it always wins.
//   DEVICE — Local Hardware only: the phone's own position (its GPS, or a town / locator the user entered in the app),
//            snapped to its grid-square centre by the app. It goes to the satellite engine and NOWHERE ELSE — not
//            /location, not vibeserver.json, not the directory, not the DAB receiver position or the ITU region.
//            It fills in only while the owner has none.
//
// Pure (no engine, no locks) so test-sat-observer can prove the precedence without the whole shim. The shim holds one
// under its own mutex and applies each Apply to satEngine().
#pragma once

namespace vibe { namespace sat {

class ObserverSource {
public:
    /** What the engine's observer should become after a change: nothing, set (lat, lon), or cleared. */
    struct Apply { enum Kind { None, Set, Clear } kind = None; double lat = 0, lon = 0; };

    /** The owner's published location changed (have = false: the owner has none now). */
    Apply owner(bool have, double lat, double lon) {
        ownerHave_ = have; oLat_ = lat; oLon_ = lon;
        if (have) { fromDevice_ = false; return set(lat, lon); }
        // ★ No owner position any more: the device's fills in where there is one. Otherwise the engine keeps what it had
        //   (the behaviour before beta94 — an owner turning location off never cleared the sky).
        if (deviceHave_) { fromDevice_ = true; return set(dLat_, dLon_); }
        return Apply();
    }
    /** The phone's own position (Local Hardware) changed (have = false: CLEAR, no position on this phone). */
    Apply device(bool have, double lat, double lon) {
        deviceHave_ = have; dLat_ = lat; dLon_ = lon;
        if (ownerHave_) return Apply();   // ★ the owner's location rules; the phone's never replaces it
        if (have) { fromDevice_ = true; return set(lat, lon); }
        if (fromDevice_) { fromDevice_ = false; Apply a; a.kind = Apply::Clear; return a; }
        return Apply();
    }
    bool ownerSet() const { return ownerHave_; }
    bool deviceInUse() const { return fromDevice_; }

private:
    static Apply set(double lat, double lon) { Apply a; a.kind = Apply::Set; a.lat = lat; a.lon = lon; return a; }
    bool ownerHave_ = false, deviceHave_ = false, fromDevice_ = false;
    double oLat_ = 0, oLon_ = 0, dLat_ = 0, dLon_ = 0;
};

}}  // namespace vibe::sat
