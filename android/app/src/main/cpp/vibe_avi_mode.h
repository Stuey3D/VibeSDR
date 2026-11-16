// vibe_avi_mode.h — the "Aircraft datalink" whole-radio mode's BANDS (V12, docs/v12/AVIATION.md §Decision): ACARS and
// VDL2 are 5 MHz apart, more than any capture here, so the mode has two bands and the tune keys step between them
// (ISM's pattern). Each band: where the radio sits, the channels decoded, and what the capture must hold.
// ★ The radio's DC remnant sits at the band's centre, so each centre is BETWEEN channels: 131.675 (ACARS: 50 kHz from
//   131.725), 136.8625 (VDL2: 12.5 kHz from 136.850 and 136.875 — outside both channels' ±5 kHz signal).
// ★ A channel outside the capture is simply not decoded (vibe_avi_front.h MultiChannel::inside); the rate rule below
//   keeps every channel of the band inside.
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
    double lo() const { double v = chans.front(); for (double c : chans) v = std::min(v, c); return v - 12.5e3; }
    double hi() const { double v = chans.front(); for (double c : chans) v = std::max(v, c); return v + 12.5e3; }
    /** The narrowest capture that holds every channel about the centre, with the channeliser's 10 % edge. */
    double minRateHz() const { return 2.0 * std::max(centreHz - lo(), hi() - centreHz) / 0.9; }
};

inline const std::vector<Band>& bands() {
    static const std::vector<Band> v = {
        // ACARS (VHF, AM-MSK 2400 bit/s): 131.725 / 131.825 Europe, 131.525 Europe, 131.550 the worldwide primary
        { "acars", "ACARS", "Europe and the worldwide 131.550", 131.675e6, { 131.525e6, 131.550e6, 131.725e6, 131.825e6 } },
        // VDL Mode 2 (D8PSK 10.5 ksym/s): every 25 kHz channel 136.700–136.975 (136.975 is worldwide)
        { "vdl2", "VDL2", "Worldwide 136.975 and Europe 136.700\xe2\x80\x93" "136.950", 136.8625e6, vibevdl2::defaultChannels() },
    };
    return v;
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
