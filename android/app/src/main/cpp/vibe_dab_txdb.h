// vibe_dab_txdb.h — the DAB transmitter directory: a TII code becomes a place and a distance.
//
// ★★★ WHAT THIS IS FOR. The null symbol says "Main Id 17, Sub Id 02" (vibe_dab_tii.h). A DX-er
//     wants "Daventry, 12 miles". Regulators publish the mapping — Ofcom's transmitter
//     parameters for the UK, with the TII codes and coordinates of every site — and this is that
//     table, keyed by the ensemble's ECC (country) and EId, then Main/Sub.
// ★★ ONE COUNTRY PER TABLE, KEYED BY ECC (Stuart, 2026-09-07): the UK (E1) is compiled in from
//    tools/gen-dab-txdb.py; any other country's list can be compiled the same way, or dropped
//    into the data directory as dab-tii-<ecc>.csv (eid,main,sub,site,area,lat,lon) and loaded at
//    start without a rebuild. Where no directory covers the ensemble the codes stand alone —
//    the fallback is the bare TII, never a wrong name.
// ★ TII codes are REUSED across a country: the same Main/Sub on the same EId can name two sites
//   a few hundred kilometres apart. When the receiver's position is known the nearest match is
//   the answer; when it is not, the first is returned and marked ambiguous.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibedab {

struct DabTx {
    uint16_t    eid;
    uint8_t     mainId, subId;
    const char* site;
    const char* area;
    float       lat, lon;
    const char* block;
};

/** A resolved transmitter: what to print beside the TII code. */
struct DabTxMatch {
    bool        found     = false;
    bool        ambiguous = false;   ///< several sites share the code and no position to choose by
    std::string site, area;
    double      lat = 0, lon = 0;
    double      km  = -1;            ///< from the receiver, or -1 when its position is unknown
};

inline double dabHaversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0088, d2r = M_PI / 180.0;
    const double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2)
                   + std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * R * std::asin(std::sqrt(a));
}

class DabTxDb {
public:
    /** Register a compiled-in country table. */
    void addBuiltin(int ecc, const DabTx* rows, size_t n) { builtin_.push_back({ ecc, rows, n }); }

    /** ★ Load <dir>/dab-tii-<ecc>.csv if present: eid,main,sub,site,area,lat,lon (hex for the
     *  first three, a '#' line is a comment). Rows here take precedence over the built-in table
     *  for that country, so a corrected or newer list needs no rebuild. */
    int loadCountryFile(const std::string& dir, int ecc) {
        char name[64]; std::snprintf(name, sizeof name, "/dab-tii-%02x.csv", ecc & 0xFF);
        FILE* f = std::fopen((dir + name).c_str(), "r");
        if (!f) return 0;
        char line[512]; int n = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            unsigned eid = 0, m = 0, s = 0; char site[128] = {0}, area[128] = {0}; double lat = 0, lon = 0;
            // site/area may not contain commas in this simple format
            if (std::sscanf(line, "%x,%x,%x,%127[^,],%127[^,],%lf,%lf", &eid, &m, &s, site, area, &lat, &lon) == 7) {
                loaded_.push_back({ ecc, uint16_t(eid), uint8_t(m), uint8_t(s), site, area, lat, lon });
                ++n;
            }
        }
        std::fclose(f);
        return n;
    }

    /** Resolve one TII hit. rxLat/rxLon NaN when the receiver's position is unknown. */
    DabTxMatch lookup(int ecc, uint16_t eid, int mainId, int subId, double rxLat, double rxLon) const {
        DabTxMatch best; int nMatch = 0;
        const bool havePos = !std::isnan(rxLat) && !std::isnan(rxLon);
        auto consider = [&](const std::string& site, const std::string& area, double lat, double lon) {
            ++nMatch;
            const double km = havePos ? dabHaversineKm(rxLat, rxLon, lat, lon) : -1;
            if (!best.found || (havePos && km < best.km)) {
                best.found = true; best.site = site; best.area = area; best.lat = lat; best.lon = lon; best.km = km;
            }
        };
        for (const auto& r : loaded_)
            if (r.ecc == ecc && r.eid == eid && r.mainId == mainId && r.subId == subId) consider(r.site, r.area, r.lat, r.lon);
        if (!best.found)
            for (const auto& t : builtin_)
                if (t.ecc == ecc)
                    for (size_t i = 0; i < t.n; ++i) {
                        const DabTx& r = t.rows[i];
                        if (r.eid == eid && r.mainId == mainId && r.subId == subId) consider(r.site, r.area, r.lat, r.lon);
                    }
        best.ambiguous = nMatch > 1 && !havePos;
        return best;
    }
    bool hasCountry(int ecc) const {
        for (const auto& t : builtin_) if (t.ecc == ecc) return true;
        for (const auto& r : loaded_) if (r.ecc == ecc) return true;
        return false;
    }

private:
    struct Table { int ecc; const DabTx* rows; size_t n; };
    struct Row { int ecc; uint16_t eid; uint8_t mainId, subId; std::string site, area; double lat, lon; };
    std::vector<Table> builtin_;
    std::vector<Row>   loaded_;
};

}  // namespace vibedab
