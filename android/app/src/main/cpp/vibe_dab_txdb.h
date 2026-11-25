// vibe_dab_txdb.h — the DAB transmitter directory: a TII code becomes a place and a distance.
//
// ★★★ WHAT THIS IS FOR. The null symbol says "Main Id 17, Sub Id 02" (vibe_dab_tii.h). A DX-er
//     wants "Daventry, 12 miles". Regulators publish the mapping — Ofcom's transmitter
//     parameters for the UK, with the TII codes and coordinates of every site — and this is that
//     table, keyed by the ensemble's ECC (country) and EId, then Main/Sub.
// ★★ ONE COUNTRY PER TABLE, KEYED BY ECC (Stuart, 2026-09-07): the UK (E1) is compiled in from
//    tools/gen-dab-txdb.py; any other country's list can be compiled the same way, or dropped
//    into the data directory as dab-tii-<ecc>.csv (eid,main,sub,site,area,lat,lon[,erp_kw,height_m])
//    and loaded at start without a rebuild. Where no directory covers the ensemble the codes stand alone —
//    the fallback is the bare TII, never a wrong name.
// ★★ MORE THAN ONE REGULATOR, AND NOT EVERY ONE PUBLISHES TII (2026-10-06). Stuart asked for the
//    world's DAB sites "without breaking any licencing agreements" — so only a regulator's own
//    list, under a licence that lets a public repository carry it, is compiled in
//    (docs/DAB-SITES-SOURCES.md has the survey and why each other country is left out). Every
//    row carries its SOURCE, so the panel says whose record it is ("Ofcom record", "BAKOM
//    record") and never passes one regulator's data off as another's. Only Ofcom publishes TII
//    codes; ČTÚ (Czechia) publishes EIds, so its rows match like the UK's minus the TII. A source
//    with neither (BAKOM, Switzerland: site, block, ensemble name; RDI, Netherlands: site and
//    block) is matched by COUNTRY + BLOCK — the EId's country nibble and the block we are tuned
//    to (kDabTxByBlock). Such a row can name a licensed site, never a TII hit: it has no code.
// ★ TII codes are REUSED across a country: the same Main/Sub on the same EId can name two sites
//   a few hundred kilometres apart. When the receiver's position is known the nearest match is
//   the answer; when it is not, the first is returned and marked ambiguous.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace vibedab {

/** Where a row came from. The licence and attribution of each are in docs/DAB-SITES-SOURCES.md
 *  and the About credits; the panel's wording is dabTxSourceLabel(). */
enum : uint8_t {
    kDabSrcOfcom = 0,     ///< UK — Ofcom technical parameters, Open Government Licence v3.0
    kDabSrcBakom = 1,     ///< Switzerland — BAKOM via opendata.swiss / geo.admin.ch, "Open use"
    kDabSrcCtu   = 2,     ///< Czechia — ČTÚ, data.gov.cz terms (no copyright, no database right; CC0)
    kDabSrcRdi   = 3,     ///< Netherlands — RDI, rdi.nl CC0 1.0
    kDabSrcUser  = 255,   ///< the server owner's own file in the data directory — never shipped by us
};
inline const char* dabTxSourceLabel(uint8_t src) {
    switch (src) {
        case kDabSrcOfcom: return "Ofcom record";
        case kDabSrcBakom: return "BAKOM record";
        case kDabSrcCtu:   return "\xC4\x8CT\xC3\x9A record";   // ČTÚ
        case kDabSrcRdi:   return "RDI record";
        case kDabSrcUser:  return "your list";
        default:           return "licence record";
    }
}

/** Row flags. */
enum : uint8_t {
    /** No EId and no TII in the source: `eid` holds only the country nibble (top 4 bits) and the
     *  row belongs to whichever ensemble of that country is on `block`. */
    kDabTxByBlock = 0x01,
    /** The EId must match AND, when we know it, the block. ČTÚ lists one placeholder EId
     *  (0x211B) against several regional multiplexes on different blocks; the block keeps a
     *  listener on R1 from being shown R8's sites. */
    kDabTxEidAndBlock = 0x02,
};

/** ★★ WHAT A ROW'S HEIGHT IS MEASURED FROM (2026-11-25). Regulators do not publish the same height, and
 *  the panel must say which one it is showing — a site 600 m above the sea and an aerial 106 m above
 *  the ground are not comparable numbers. */
enum : uint8_t {
    kDabHtNone      = 0,   ///< no height published: heightM means nothing
    kDabHtAerialAgl = 1,   ///< the aerial above the ground at the site (Ofcom "In-Use Ae Ht"; an owner's list)
    kDabHtSiteAsl   = 2,   ///< the ground at the site above sea level (ČTÚ "Výška nad mořem")
    kDabHtEffective = 3,   ///< the effective aerial height, as the regulator calls it (RDI "Eff.Hoogte")
};
/** The JSON spelling of a height reference — "agl", "asl", "eff" — and the clients' wording keys off it. */
inline const char* dabHeightRefKey(uint8_t ref) {
    switch (ref) {
        case kDabHtAerialAgl: return "agl";
        case kDabHtSiteAsl:   return "asl";
        case kDabHtEffective: return "eff";
        default:              return "";
    }
}

struct DabTx {
    uint16_t    eid;
    uint8_t     mainId, subId;      ///< 0/0 when the source publishes no TII
    const char* site;
    const char* area;               ///< the area served — or, on a by-block row, the ensemble's name
    float       lat, lon;
    const char* block;
    uint8_t     src   = kDabSrcOfcom;
    uint8_t     flags = 0;
    /** ★★ POWER AND HEIGHT (Stuart, 2026-11-25: why is a farther site stronger?). The regulator's own
     *  figures, never estimated: 0 kW = not published, kDabHtNone = no height. Ofcom gives the in-use
     *  ERP and the aerial's height above ground; BAKOM the ERP (one figure per site, so only where the
     *  site carries one service); ČTÚ the ERP and the site's altitude; RDI the ERP and an effective
     *  height. docs/DAB-SITES-SOURCES.md has the columns. */
    float       erpKw     = 0;
    int16_t     heightM   = 0;
    uint8_t     heightRef = kDabHtNone;
};

/** ★ ISO 3166 alpha-2 → ECC, for the per-row country column of an owner's own TII list (the
 *  FMLIST / AbracaDABra "dab-tx-list.csv" layout). ETSI TS 101 756 table D.1; -1 when unknown,
 *  and such a row then matches on the EId alone. */
inline int dabEccForIso(const char* iso) {
    static const struct { char c[3]; uint8_t ecc; } k[] = {
        {"AD",0xE0},{"AT",0xE0},{"AU",0xF0},{"BE",0xE0},{"BG",0xE1},{"CH",0xE1},{"CY",0xE1},{"CZ",0xE2},
        {"DE",0xE0},{"DK",0xE1},{"EE",0xE4},{"ES",0xE2},{"FI",0xE1},{"FR",0xE1},{"GB",0xE1},{"GI",0xE1},
        {"GR",0xE1},{"HR",0xE3},{"HU",0xE0},{"IE",0xE3},{"IT",0xE0},{"KW",0xF2},{"LI",0xE2},{"LT",0xE2},
        {"LU",0xE1},{"LV",0xE3},{"MC",0xE2},{"ME",0xE3},{"MK",0xE4},{"MT",0xE0},{"NL",0xE3},{"NO",0xE2},
        {"PL",0xE2},{"PT",0xE4},{"RO",0xE1},{"RS",0xE2},{"SE",0xE3},{"SI",0xE4},{"SK",0xE2},{"SM",0xE1},
        {"TN",0xE2},{"TR",0xE3},{"UA",0xE4},{"VA",0xE2},{"ZA",0xD0},
    };
    if (!iso || !iso[0] || !iso[1]) return -1;
    const char a = char(std::toupper((unsigned char)iso[0])), b = char(std::toupper((unsigned char)iso[1]));
    for (const auto& e : k) if (e.c[0] == a && e.c[1] == b) return e.ecc;
    return -1;
}

/** A resolved transmitter: what to print beside the TII code. */
struct DabTxMatch {
    bool        found     = false;
    bool        ambiguous = false;   ///< several sites share the code and no position to choose by
    std::string site, area;
    double      lat = 0, lon = 0;
    double      km  = -1;            ///< from the receiver, or -1 when its position is unknown
    float       erpKw = 0;           ///< 0 = not published
    int         heightM = 0;
    uint8_t     heightRef = kDabHtNone;
};

/** ★ The power/height fields of one JSON object — `,"erpKw":9.55,"htM":106,"htRef":"agl"` — or nothing
 *  for what the source does not publish. One copy for the TII lines and the licensed sites. */
inline std::string dabTxSpecsJson(float erpKw, int heightM, uint8_t heightRef) {
    std::string j; char b[64];
    if (erpKw > 0 && std::isfinite(erpKw)) { std::snprintf(b, sizeof b, ",\"erpKw\":%.4g", double(erpKw)); j += b; }
    if (heightRef != kDabHtNone) {
        std::snprintf(b, sizeof b, ",\"htM\":%d,\"htRef\":\"%s\"", heightM, dabHeightRefKey(heightRef)); j += b;
    }
    return j;
}

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
     *  for that country, so a corrected or newer list needs no rebuild.
     *  ★ Two OPTIONAL columns after lon (2026-11-25): erp_kw and height_m, the aerial's height
     *    above the ground in metres — eid,main,sub,site,area,lat,lon,erp_kw,height_m. Either may be
     *    left empty; a file with seven columns loads exactly as before. */
    int loadCountryFile(const std::string& dir, int ecc) {
        char name[64]; std::snprintf(name, sizeof name, "/dab-tii-%02x.csv", ecc & 0xFF);
        FILE* f = std::fopen((dir + name).c_str(), "r");
        if (!f) return 0;
        char line[512]; int n = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            unsigned eid = 0, m = 0, s = 0; char site[128] = {0}, area[128] = {0}; double lat = 0, lon = 0;
            // site/area may not contain commas in this simple format
            int used = 0;
            if (std::sscanf(line, "%x,%x,%x,%127[^,],%127[^,],%lf,%lf%n", &eid, &m, &s, site, area, &lat, &lon, &used) == 7) {
                Row r; r.ecc = ecc; r.eid = uint16_t(eid); r.mainId = uint8_t(m); r.subId = uint8_t(s);
                r.site = site; r.area = area; r.lat = lat; r.lon = lon;
                parseCountrySpecs(line + used, r);
                loaded_.push_back(r);
                ++n;
            }
        }
        std::fclose(f);
        return n;
    }

    /** ★★ THE OWNER'S OWN TII LIST, in the layout FMLIST exports and AbracaDABra / muxpulse read
     *  ("dab-tx-list.csv": `;`-separated, one header line, then id;country;channel;label;EId;TII;
     *  location;lat;lon;altitude;height;polarisation;MHz;kW). Stuart, 2026-10-06: those apps let a
     *  listener import a list they fetched themselves. FMLIST's data is not ours to redistribute,
     *  so we never ship or fetch it — but a server owner with their own copy can drop it into the
     *  data directory and listeners on THEIR server get the names. Read from disk only; nothing
     *  here sends it anywhere. TII is decimal main×100+sub, the EId hex. Returns the rows read. */
    int loadTxListFile(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return 0;
        char line[1024]; int n = 0; bool first = true;
        while (std::fgets(line, sizeof line, f)) {
            if (first) { first = false; continue; }           // the header
            const DabTxListRow r = parseTxListLine(line);
            if (!r.ok) continue;
            Row x; x.ecc = r.ecc; x.eid = r.eid; x.mainId = r.mainId; x.subId = r.subId;
            x.site = r.site; x.area = r.label; x.lat = r.lat; x.lon = r.lon; x.block = r.block;
            x.erpKw = r.erpKw; x.heightM = r.heightM; x.heightRef = r.heightRef;
            loaded_.push_back(x);
            ++n;
        }
        std::fclose(f);
        return n;
    }

    /** The optional `,erp_kw,height_m` tail of a dab-tii-<ecc>.csv line (after lon). Empty, missing,
     *  non-numeric or non-positive fields are simply not set: never a guessed figure. Public for the test. */
    template <class R> static void parseCountrySpecs(const char* p, R& r) {
        if (!p || *p != ',') return;
        ++p;
        char* end = nullptr;
        const double kw = std::strtod(p, &end);
        if (end != p && kw > 0 && kw < 10000) r.erpKw = float(kw);
        p = std::strchr(p, ',');
        if (!p) return;
        ++p;
        const long h = std::strtol(p, &end, 10);
        if (end != p && h > 0 && h < 2000) { r.heightM = int16_t(h); r.heightRef = kDabHtAerialAgl; }
    }

    /** One line of the dab-tx-list.csv layout, parsed — public so a test can hold it to the format.
     *  ★ alt/height/kW (columns 9, 10, 13) are read when present (2026-11-25): height is the aerial's
     *    above the ground, kW the ERP. Empty or zero = not known. */
    struct DabTxListRow {
        bool ok = false; int ecc = -1; uint16_t eid = 0; uint8_t mainId = 0, subId = 0;
        std::string site, label, block; double lat = 0, lon = 0;
        float erpKw = 0; int16_t heightM = 0; uint8_t heightRef = kDabHtNone;
    };
    static DabTxListRow parseTxListLine(const char* line) {
        DabTxListRow r;
        std::vector<std::string> c; std::string cur;
        for (const char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
            if (*p == ';') { c.push_back(cur); cur.clear(); } else cur += *p;
        }
        c.push_back(cur);
        if (c.size() < 9) return r;
        auto trim = [](const std::string& x) {
            size_t a = 0, b = x.size();
            while (a < b && (x[a] == ' ' || x[a] == '"' || x[a] == '\t')) ++a;
            while (b > a && (x[b - 1] == ' ' || x[b - 1] == '"' || x[b - 1] == '\t')) --b;
            return x.substr(a, b - a);
        };
        const std::string iso = trim(c[1]), eidS = trim(c[4]), tiiS = trim(c[5]);
        const std::string latS = trim(c[7]), lonS = trim(c[8]);
        if (eidS.empty() || tiiS.empty() || latS.empty() || lonS.empty()) return r;
        char* end = nullptr;
        const unsigned long eid = std::strtoul(eidS.c_str(), &end, 16);
        if (*end || eid == 0 || eid > 0xFFFF) return r;
        const long tii = std::strtol(tiiS.c_str(), &end, 10);
        if (*end || tii < 0 || tii > 6999 || tii % 100 == 0 || tii % 100 > 23) return r;   // main 0–69, sub 1–23
        const double lat = std::strtod(latS.c_str(), &end); if (*end) return r;
        const double lon = std::strtod(lonS.c_str(), &end); if (*end) return r;
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) return r;
        r.ecc = dabEccForIso(iso.c_str()); r.eid = uint16_t(eid);
        r.mainId = uint8_t(tii / 100); r.subId = uint8_t(tii % 100);
        r.site = trim(c[6]).substr(0, 127); r.label = trim(c[3]).substr(0, 127); r.block = trim(c[2]).substr(0, 7);
        r.lat = lat; r.lon = lon; r.ok = !r.site.empty();
        if (c.size() > 10) {
            const std::string hS = trim(c[10]);
            const long h = hS.empty() ? 0 : std::strtol(hS.c_str(), &end, 10);
            if (!hS.empty() && h > 0 && h < 2000) { r.heightM = int16_t(h); r.heightRef = kDabHtAerialAgl; }
        }
        if (c.size() > 13) {
            const std::string kS = trim(c[13]);
            const double kw = kS.empty() ? 0 : std::strtod(kS.c_str(), &end);
            if (!kS.empty() && kw > 0 && kw < 10000) r.erpKw = float(kw);
        }
        return r;
    }

    /** Resolve one TII hit. rxLat/rxLon NaN when the receiver's position is unknown. */
    DabTxMatch lookup(int ecc, uint16_t eid, int mainId, int subId, double rxLat, double rxLon) const {
        DabTxMatch best; int nMatch = 0;
        const bool havePos = !std::isnan(rxLat) && !std::isnan(rxLon);
        auto consider = [&](const std::string& site, const std::string& area, double lat, double lon,
                            float erpKw, int heightM, uint8_t heightRef) {
            ++nMatch;
            const double km = havePos ? dabHaversineKm(rxLat, rxLon, lat, lon) : -1;
            if (!best.found || (havePos && km < best.km)) {
                best.found = true; best.site = site; best.area = area; best.lat = lat; best.lon = lon; best.km = km;
                best.erpKw = erpKw; best.heightM = heightM; best.heightRef = heightRef;
            }
        };
        for (const auto& r : loaded_)
            if (eccMatches(r.ecc, ecc) && r.eid == eid && r.mainId == mainId && r.subId == subId)
                consider(r.site, r.area, r.lat, r.lon, r.erpKw, r.heightM, r.heightRef);
        if (!best.found)
            for (const auto& t : builtin_)
                if (t.ecc == ecc)
                    for (size_t i = 0; i < t.n; ++i) {
                        const DabTx& r = t.rows[i];
                        if ((r.flags & kDabTxByBlock) || r.subId == 0) continue;   // no TII in that source: nothing to match
                        if (r.eid == eid && r.mainId == mainId && r.subId == subId)
                            consider(r.site, r.area, r.lat, r.lon, r.erpKw, r.heightM, r.heightRef);
                    }
        best.ambiguous = nMatch > 1 && !havePos;
        return best;
    }
    /** ★ THE LICENSED SITES FOR AN ENSEMBLE — a regulator's record, not a measurement. Shown beside
     *  the air's own TII so a reader can see both: an ensemble whose null symbol carries no TII
     *  (11D was thought to be one, 2026-09-07 — it was the detector; fixed 2026-11-04, vibe_dab_tii.h)
     *  or one sending the generic small-scale code 01/05
     *  still has a known transmitter. Stuart: "is that knowledge something we can add to the panel".
     *  Nearest first when the receiver's position is known.
     *  ★ `block` is the block we are tuned to ("12C"): a by-block source (no EIds) matches on the
     *    EId's country nibble and that block. Null or empty skips those rows. */
    struct Site {
        std::string site, area; int mainId = 0, subId = 0; double km = -1; uint8_t src = kDabSrcOfcom;
        float erpKw = 0; int heightM = 0; uint8_t heightRef = kDabHtNone;
    };
    std::vector<Site> sitesFor(int ecc, uint16_t eid, double rxLat, double rxLon, size_t maxN = 4,
                               const char* block = nullptr) const {
        std::vector<Site> out;
        const bool havePos = !std::isnan(rxLat) && !std::isnan(rxLon);
        auto add = [&](const std::string& site, const std::string& area, int m, int s, double lat, double lon, uint8_t src,
                       float erpKw, int heightM, uint8_t heightRef) {
            for (const auto& o : out) if (o.site == site) return;
            Site x; x.site = site; x.area = area; x.mainId = m; x.subId = s; x.src = src;
            x.erpKw = erpKw; x.heightM = heightM; x.heightRef = heightRef;
            x.km = havePos ? dabHaversineKm(rxLat, rxLon, lat, lon) : -1;
            out.push_back(x);
        };
        const bool haveBlock = block && block[0];
        for (const auto& r : loaded_)
            if (eccMatches(r.ecc, ecc) && r.eid == eid)
                add(r.site, r.area, r.mainId, r.subId, r.lat, r.lon, kDabSrcUser, r.erpKw, r.heightM, r.heightRef);
        for (const auto& t : builtin_) if (t.ecc == ecc)
            for (size_t i = 0; i < t.n; ++i) {
                const DabTx& r = t.rows[i];
                const bool sameBlock = haveBlock && r.block && std::strcmp(r.block, block) == 0;
                const bool hit = (r.flags & kDabTxByBlock)
                    ? sameBlock && (r.eid >> 12) == (eid >> 12)
                    : r.eid == eid && (!(r.flags & kDabTxEidAndBlock) || !haveBlock || sameBlock);
                if (hit) add(r.site, r.area, r.mainId, r.subId, r.lat, r.lon, r.src, r.erpKw, r.heightM, r.heightRef);
            }
        if (havePos) std::sort(out.begin(), out.end(), [](const Site& a, const Site& b) { return a.km < b.km; });
        if (out.size() > maxN) out.resize(maxN);
        return out;
    }
    /** ★ Every TII code the directory holds for this ensemble, as bits main * 24 + sub — what a shared
     *  comb is resolved against (tiiResolveShared, vibe_dab_tii.h). None set = no TII record here:
     *  rows without a code (by-block sources, ČTÚ) never count. One pass; the caller caches it. */
    static constexpr int kTiiCodeBits = 70 * 24;
    std::vector<bool> tiiCodes(int ecc, uint16_t eid) const {
        std::vector<bool> out(kTiiCodeBits, false);
        auto set = [&](int m, int s) { if (m >= 0 && m < 70 && s > 0 && s < 24) out[size_t(m * 24 + s)] = true; };
        for (const auto& r : loaded_)
            if (eccMatches(r.ecc, ecc) && r.eid == eid) set(r.mainId, r.subId);
        for (const auto& t : builtin_) if (t.ecc == ecc)
            for (size_t i = 0; i < t.n; ++i) {
                const DabTx& r = t.rows[i];
                if ((r.flags & kDabTxByBlock) || r.subId == 0) continue;
                if (r.eid == eid) set(r.mainId, r.subId);
            }
        return out;
    }
    bool hasCountry(int ecc) const {
        for (const auto& t : builtin_) if (t.ecc == ecc) return true;
        for (const auto& r : loaded_) if (r.ecc == ecc) return true;
        return false;
    }
    size_t loadedRows() const { return loaded_.size(); }

private:
    /** An owner's row whose country we could not place (ecc -1) matches on the EId alone. */
    static bool eccMatches(int rowEcc, int ecc) { return rowEcc < 0 || rowEcc == ecc; }
    struct Table { int ecc; const DabTx* rows; size_t n; };
    struct Row {
        int ecc = -1; uint16_t eid = 0; uint8_t mainId = 0, subId = 0;
        std::string site, area; double lat = 0, lon = 0; std::string block;
        float erpKw = 0; int16_t heightM = 0; uint8_t heightRef = kDabHtNone;
    };
    std::vector<Table> builtin_;
    std::vector<Row>   loaded_;
};

}  // namespace vibedab
