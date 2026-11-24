// vibe_avi_mode.h — the "Aircraft datalink" whole-radio mode's BANDS (V12, docs/v12/AVIATION.md §Decision): ACARS and
// VDL2 are 5 MHz apart, more than any capture here, so the mode has two bands and the tune keys step between them
// (ISM's pattern). Each band: where the radio sits, the channels decoded, and what the capture must hold.
// ★ The radio's DC remnant sits at the band's centre, so each centre is BETWEEN channels: 131.675 (ACARS: 50 kHz from
//   131.725), 136.8625 (VDL2: 12.5 kHz from 136.850 and 136.875 — outside both channels' ±5 kHz signal).
// ★ A channel outside the capture is simply not decoded (vibe_avi_front.h MultiChannel::inside); the rate rule below
//   keeps every channel of the band inside.
// ★★ V12 2026-11-24 ACARS + VDL2 TOGETHER (Stuart: "on a radio wide enough we should allow ACARS and VDL2 together"): a third
//   band holding both sets, centred at 134.25 MHz — the middle of 131.5125…136.9875, so the DC is 2.4 MHz from the
//   nearest channel of either set. It needs ~6.1 MS/s (minRateHz), so it is `wide`: OFFERED only where the radio's RUNNING
//   capture already holds it (an RSP1B at 8–10 MS/s, an Airspy R2 at 10, a HackRF, a USRP), or where the owner's 8 MS/s
//   switch will raise it there (offered() below) — never on an RTL's 2.4 MS/s or the HF+ (no dead choice). The service
//   runs both decoders, each on its own thread (vibe_avi_service.h).
//   ★ 6.0 MS/s is just short (6.083 needed): an RSP at 6 MS/s needs the switch (or the owner's 8 MS/s).
// ✗ Not yet: the US ACARS set (129.125–131.550 MHz, 2.4 MHz wide — its own centre and a heavier front end).
#pragma once
#include "vibe_vdl2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibeavi {

struct Band {
    const char* id;          ///< "acars" | "vdl2" — also the Service's kind
    const char* name;        ///< "ACARS"
    const char* region;      ///< the BANDS list's second line
    double centreHz;         ///< where the radio sits (its DC between channels)
    std::vector<double> chans;
    bool wide = false;       ///< ★ offered only where the running capture already holds it (offered())
    double lo() const { double v = chans.front(); for (double c : chans) v = std::min(v, c); return v - 12.5e3; }
    double hi() const { double v = chans.front(); for (double c : chans) v = std::max(v, c); return v + 12.5e3; }
    /** The narrowest capture that holds every channel about the centre, with the channeliser's 10 % edge. */
    double minRateHz() const { return 2.0 * std::max(centreHz - lo(), hi() - centreHz) / 0.9; }
};

/** The ACARS channels (VHF, AM-MSK 2400 bit/s): 131.725 / 131.825 Europe, 131.525 Europe, 131.550 the worldwide primary. */
inline std::vector<double> acarsChannels() { return { 131.525e6, 131.550e6, 131.725e6, 131.825e6 }; }

inline const std::vector<Band>& bands() {
    static const std::vector<Band> v = [] {
        std::vector<Band> b = {
            { "acars", "ACARS", "Europe and the worldwide 131.550", 131.675e6, acarsChannels() },
            // VDL Mode 2 (D8PSK 10.5 ksym/s): every 25 kHz channel 136.700–136.975 (136.975 is worldwide)
            { "vdl2", "VDL2", "Worldwide 136.975 and Europe 136.700\xe2\x80\x93" "136.950", 136.8625e6, vibevdl2::defaultChannels() },
        };
        // ★ both sets from one wide capture: the DC at 134.25 MHz, between the two sets and 2.4 MHz from either
        Band both{ "acars+vdl2", "ACARS + VDL2", "Both bands at once (a wide capture: 6.1 MS/s or more)", 134.25e6, acarsChannels() };
        for (double h : vibevdl2::defaultChannels()) both.chans.push_back(h);
        both.wide = true;
        b.push_back(both);
        return b;
    }();
    return v;
}
/* ★★ THE OWNER'S 8 MS/s SWITCH (Stuart 2026-11-24: "maybe a settings toggle to allow 8 MS/s (for SDRplay, to compensate
 *  for the roll-off) for the aircraft datalink to capture both ACARS and VDL2 together") — "avdata+8m", an option under
 *  the aircraft datalink, IN the modes CSV when ticked (the "lora+all" convention). With it, picking ACARS + VDL2 raises
 *  the capture to kBoostRateHz (or the radio's next listed rate above it: an Airspy R2's 10) and leaving gives the owner's
 *  rate back; an RSP's IF filter is then 8 MHz wide, so both sets (±2.74 MHz of 134.25) sit well inside its flat part.
 *  Never where the owner PINNED the rate, never after the rate guard recorded a fall-over at a boosted rate
 *  (vibe_rate_guard.h), never on a radio whose list stops short of it (an RTL, the HF+ — the switch is not drawn there). */
static constexpr double kBoostRateHz = 8.0e6;
/** The rate the switch would take the radio to: the smallest of `rates` at or above 8 MS/s — 0 = none (no switch here). */
inline double boostRateFrom(const std::vector<double>& rates) {
    double best = 0;
    for (double r : rates) if (r >= kBoostRateHz - 1000.0 && (best == 0 || r < best)) best = r;
    return best;
}
/** The rate entering the combined band would raise the capture to, or 0 when it will not (switch off, rate pinned, the
 *  guard holding it, no such rate on this radio). */
inline double wideBoostRate(bool ownerSwitch, bool ratePinned, bool guardBlocked, const std::vector<double>& rates) {
    if (!ownerSwitch || ratePinned || guardBlocked) return 0;
    return boostRateFrom(rates);
}
/** ★ Is `b` offered on a radio whose running capture (its USABLE span) is `runningHz`, where entering it could raise the
 *  capture to `boostHz` (0 = it cannot: wideBoostRate)? A narrow band always is (where it cannot run, the mode says why);
 *  a WIDE band only where the capture already holds it, or the owner's switch will raise it there — otherwise it would be
 *  a choice that can only fail (AGENTS.md: no dead controls). `runningHz` 0 = not known yet. */
inline bool offered(const Band& b, double runningHz, double boostHz = 0) {
    if (!b.wide) return true;
    if (runningHz > 0 && runningHz >= b.minRateHz()) return true;
    return boostHz >= b.minRateHz();
}
/** ★ Does entering `b` on a capture of `runningHz` raise the rate (to `boostHz`)? Only the wide band, only when the running
 *  capture does not already hold it. */
inline bool needsBoost(const Band& b, double runningHz, double boostHz) {
    return b.wide && !(runningHz > 0 && runningHz >= b.minRateHz()) && boostHz >= b.minRateHz();
}
inline const Band* bandById(const std::string& id) {
    for (const auto& b : bands()) if (id == b.id) return &b;
    return nullptr;
}

/** {"type":"avdata", band, bandName, hz, …the Tracker's own fields (msgs, aircraft)…, dropped} from a Tracker JSON. */
inline std::string report(const Band& b, const std::string& trackerJson, unsigned long long dropped) {
    std::string o = "{\"type\":\"avdata\",\"band\":\"";
    o += b.id; o += "\",\"bandName\":\""; o += b.name; o += "\",";
    char h[64]; std::snprintf(h, sizeof h, "\"hz\":%.0f,\"dropped\":%llu,", b.centreHz, dropped); o += h;
    if (trackerJson.size() >= 2 && trackerJson.front() == '{') o += trackerJson.substr(1);
    else o += "\"msgs\":0,\"aircraft\":[]}";
    return o;
}

}  // namespace vibeavi
