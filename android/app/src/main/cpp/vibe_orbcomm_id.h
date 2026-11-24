// vibe_orbcomm_id.h — WHICH ORBCOMM satellite a carrier is (V12, 2026-11-24): from the satellite's own position report,
// else its channel + Doppler; and where it was in the sky at each decode (the sky plot's dot).
//
// ★★ WHY NOT THE SYNC ID ALONE: the sync packet's one-byte spacecraft ID (Kenny: FM-1 … FM-36 = 01 … 24 hex) is not a
//    catalogue number and no public table maps the second-generation (OG2) birds' IDs. So the ID is LEARNT: an ephemeris
//    packet (every 4 s) gives the transmitting satellite's own GPS position — the bird whose SGP4 position (CelesTrak
//    elements) is nearest, within kPosMatchKm, is that ID's satellite for the rest of the run.
//    Before an ephemeris, the sync packet's downlink channel (the transmitter's NOMINAL frequency) and the carrier's
//    received frequency give a measured Doppler; the bird above the horizon whose predicted Doppler is within
//    kDopplerMatchHz names the carrier ("by Doppler"). Neither = unknown (the decodes count, no dot is drawn).
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "vibe_orbcomm.h"
#include "vibe_sat_orbit.h"

namespace vibeorb {

constexpr double kPosMatchKm = 400;        ///< ephemeris vs SGP4 (elements a day old are ~1–10 km out; the next bird is 1000s)
constexpr double kDopplerMatchHz = 900;    ///< a dongle's ppm at 137 MHz (±1 ppm = ±137 Hz) + the elements' age, with room

struct Bird {
    long long norad = 0;
    std::string name;
    vibe::sat::Sgp4 sgp;
    bool ok = false;
};

/** Earth-fixed position (km) of a bird at `unix` (the same TEME → pseudo-Earth-fixed rotation as vibe::sat::look). */
inline bool ecefKm(vibe::sat::Sgp4& s, double unix, double re[3]) {
    double r[3], v[3];
    if (s.propagateUnix(unix, r, v) != 0) return false;
    const double th = vibe::sat::gmst(vibe::sat::jdFromUnix(unix));
    const double c = std::cos(th), sn = std::sin(th);
    re[0] = c * r[0] + sn * r[1]; re[1] = -sn * r[0] + c * r[1]; re[2] = r[2];
    return true;
}

class Ident {
public:
    struct Answer {
        long long norad = 0;          ///< 0 = not known
        std::string name, how;        ///< how: "position" (its own report) | "doppler" | "id" (learnt earlier) | ""
        double azDeg = 0, elDeg = -90, dopplerHz = 0, predictedHz = 0;
        bool sky = false;             ///< az/el valid (a location and the bird's elements)
    };
    void setBirds(std::vector<Bird> b) { birds_ = std::move(b); }
    void setObserver(const vibe::sat::Observer& o) { obs_ = o; }
    bool hasObserver() const { return obs_.valid; }
    const std::vector<Bird>& birds() const { return birds_; }

    /** One good packet on carrier `chan` (received at `rfHz`, at `t` UTC). Learns from sync and ephemeris packets. */
    Answer onPacket(const Rx& r, double t) {
        if (carriers_.size() > 64 && !carriers_.count(r.chan)) carriers_.erase(carriers_.begin());   // ★ bounded: carriers come and go
        Carrier& c = carriers_[r.chan];
        SyncInfo si;
        if (decodeSync(r.p, si)) { c.scId = si.id; c.dcn = si.channel; }
        Ephemeris e;
        if (decodeEphemeris(r.p, e, t)) {
            c.scId = e.id;
            const double tq = e.unix > 0 && std::fabs(e.unix - t) < 120 ? e.unix : t;
            double best = 1e18; long long bn = 0;
            for (Bird& b : birds_) {
                if (!b.ok) continue;
                double p[3];
                if (!ecefKm(b.sgp, tq, p)) continue;
                const double d = std::sqrt(sq(p[0] - e.x / 1000) + sq(p[1] - e.y / 1000) + sq(p[2] - e.z / 1000));
                if (d < best) { best = d; bn = b.norad; }
            }
            lastEphKm_ = best;
            if (bn && best < kPosMatchKm) { idMap_[e.id] = bn; c.norad = bn; c.how = "position"; }
        }
        if (!c.norad && c.scId >= 0) {
            auto it = idMap_.find(c.scId);
            if (it != idMap_.end()) { c.norad = it->second; c.how = "id"; }
        }
        Answer a;
        if (!c.norad && c.dcn > 0 && obs_.valid) {   // by Doppler: the measured shift vs each bird's prediction
            const double measured = r.rfHz - downlinkHz(c.dcn);
            double best = 1e18; long long bn = 0;
            for (Bird& b : birds_) {
                if (!b.ok) continue;
                const vibe::sat::Look L = vibe::sat::look(b.sgp, obs_, t);
                if (!L.ok || L.elDeg < -2) continue;
                const double d = std::fabs(L.dopplerHz(downlinkHz(c.dcn)) - measured);
                if (d < best) { best = d; bn = b.norad; }
            }
            if (bn && best < kDopplerMatchHz) { a.norad = bn; a.how = "doppler"; }   // per packet: not remembered (two birds can share)
        }
        if (c.norad) { a.norad = c.norad; a.how = c.how; }
        if (a.norad) {
            for (Bird& b : birds_) if (b.norad == a.norad) {
                a.name = b.name;
                if (obs_.valid && b.ok) {
                    const vibe::sat::Look L = vibe::sat::look(b.sgp, obs_, t);
                    if (L.ok) {
                        a.sky = true; a.azDeg = L.azDeg; a.elDeg = L.elDeg;
                        const double nominal = c.dcn > 0 ? downlinkHz(c.dcn) : r.rfHz;
                        a.dopplerHz = r.rfHz - nominal; a.predictedHz = L.dopplerHz(nominal);
                    }
                }
                break;
            }
        }
        return a;
    }
    /** The carrier's spacecraft ID / downlink channel as last heard (−1 / 0 = not yet). */
    int scIdOf(int chan) const { auto it = carriers_.find(chan); return it == carriers_.end() ? -1 : it->second.scId; }
    int channelOf(int chan) const { auto it = carriers_.find(chan); return it == carriers_.end() ? 0 : it->second.dcn; }
    double lastEphemerisMatchKm() const { return lastEphKm_; }
    const std::map<int, long long>& learntIds() const { return idMap_; }
private:
    static double sq(double v) { return v * v; }
    struct Carrier { int scId = -1, dcn = 0; long long norad = 0; std::string how; };
    std::vector<Bird> birds_;
    vibe::sat::Observer obs_;
    std::map<int, Carrier> carriers_;
    std::map<int, long long> idMap_;
    double lastEphKm_ = -1;
};

}  // namespace vibeorb
