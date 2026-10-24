// vibe_adsb_data_build.h — turns the two upstream downloads into the compact tables vibe_adsb_data.h reads.
//
// ★★★ ONE BUILDER, TWO CALLERS. The server's weekly update (vibe_adsb_data_update.h) and the repository's
//     reproducible build (scripts/build-adsb-data.sh → vibeserver's adsb-data-tool, which also writes the starter
//     compiled into every server) run THIS code, so the tables a server makes for itself and the ones we ship can
//     never drift apart (memory recurring_fault_shapes: one rule, two readers).
// ★★★ THE LICENCES ARE CHECKED BEFORE ANYTHING IS BUILT, AND A CHANGE STOPS THE BUILD. The research
//     (docs/v12/ADSB-DATA-RESEARCH.md §3) cleared these sources AS LICENSED ON 2026-10-24: VRS standing data under
//     CC0 1.0, the Mictronics database under ODC-By 1.0. If either licence file changes, the new data is NOT
//     used — the old tables stay, and the setup page says why — until a person has read the new terms and moved
//     the pins below. Data under terms nobody has read is not data we may serve.
// ★★ INSIDE A 1 GB PHONE'S BUDGET. The inputs are read whole (6 MB + 3.4 MB, mapped by the caller); the 25 MB of
//    CSV and 15 MB of JSON inside them are STREAMED through vibe_inflate.h and never held at once. The largest
//    working set is the route table (≈ 621 000 × 8 bytes + the distinct airport sequences): ~15 MB, briefly, on
//    the updater's own lowest-priority thread.
#pragma once
#include "vibe_adsb_data.h"
#include "vibe_inflate.h"
#include <cerrno>
#include <cstdlib>
#include <set>

namespace vibeadsbdata {

// ── the licence pins ─────────────────────────────────────────────────────────────────────────────────
// FNV-1a 64 of each licence file's exact bytes, as read on 2026-10-24, plus a phrase that must be in it.
// ★ To move a pin: READ the new licence, decide it still allows what ADSB-DATA-RESEARCH.md §3 says we do, update
//   that document, then these. tools: `adsb-data-tool licence-hash <file>`.
constexpr uint64_t kVrsLicenceFnv = 0xf92ec4039367c961ULL;   // standing-data/LICENSE (CC0 1.0 Universal)
constexpr uint64_t kMicLicenceFnv = 0x3898d7b82cfaf922ULL;   // aircraft-database/LICENSE (ODC-By 1.0)
inline const char* kVrsLicencePhrase = "CC0 1.0 Universal";
inline const char* kMicLicencePhrase = "Open Data Commons Attribution License";

inline uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

/** "" when the licence is the one we cleared; otherwise why not. */
inline std::string checkLicence(const std::string& text, uint64_t pin, const char* phrase, const char* what) {
    if (text.empty()) return std::string(what) + ": the licence file is missing from the download";
    if (text.find(phrase) == std::string::npos || fnv1a64(text) != pin) {
        char b[64]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)fnv1a64(text));
        return std::string(what) + ": the licence file has CHANGED upstream (fnv " + b +
               ") — the update is held until someone reads the new terms";
    }
    return {};
}

// ── CSV (RFC 4180-ish, as VRS writes it: optional quotes, "" inside quotes, a UTF-8 BOM on line 1) ──────
struct Csv {
    std::vector<std::string> head;
    std::vector<std::vector<std::string>> rows;
    int col(const char* name) const { for (size_t i = 0; i < head.size(); ++i) if (head[i] == name) return int(i); return -1; }
};
inline Csv parseCsv(const std::string& body) {
    Csv c;
    size_t i = 0;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    std::vector<std::string> row; std::string f; bool q = false, any = false;
    auto endRow = [&]() {
        row.push_back(f); f.clear();
        if (c.head.empty()) c.head = row; else if (row.size() > 1 || !row[0].empty()) c.rows.push_back(row);
        row.clear(); any = false;
    };
    for (; i < body.size(); ++i) {
        const char ch = body[i];
        if (q) {
            if (ch == '"') { if (i + 1 < body.size() && body[i + 1] == '"') { f += '"'; ++i; } else q = false; }
            else f += ch;
            continue;
        }
        if (ch == '"') { q = true; any = true; }
        else if (ch == ',') { row.push_back(f); f.clear(); any = true; }
        else if (ch == '\n') endRow();
        else if (ch != '\r') { f += ch; any = true; }
    }
    if (any || !f.empty() || !row.empty()) endRow();
    return c;
}

// ── the writer ───────────────────────────────────────────────────────────────────────────────────────
class Writer {
public:
    Writer() { strs_.push_back('\0'); }
    /** A pooled string. `dedupe` for short repeated ones (names, descriptions) — NOT for 450 000 registrations,
     *  whose hash map would cost more RAM than the strings. */
    uint32_t str(const std::string& s, bool dedupe = true) {
        if (s.empty()) return 0;
        if (dedupe) { auto it = pool_.find(s); if (it != pool_.end()) return it->second; }
        const uint32_t o = uint32_t(strs_.size());
        strs_.append(s); strs_.push_back('\0');
        if (dedupe) pool_[s] = o;
        return o;
    }
    void section(const char (&id)[5], std::string bytes) { secs_.push_back({fourcc(id), std::move(bytes)}); }
    void meta(const std::string& k, const std::string& v) { meta_ += k + "=" + v + "\n"; }
    size_t poolSize() const { return strs_.size(); }

    /** The file. ★ MOVES the sections out (the writer is spent): copying them would double the peak. */
    std::string bytes(uint32_t kind, uint32_t built) {
        std::vector<std::pair<uint32_t, std::string>> all = std::move(secs_);
        all.push_back({fourcc("STRS"), std::move(strs_)});
        all.push_back({fourcc("META"), meta_});
        pool_.clear();
        size_t total = 32 + all.size() * 12;
        for (const auto& a : all) total += a.second.size() + 8;
        std::string o(32 + all.size() * 12, '\0');
        o.reserve(total);
        auto w32 = [&](size_t at, uint32_t v) { std::memcpy(&o[at], &v, 4); };
        std::memcpy(&o[0], "VADB", 4);
        w32(4, kVersion); w32(8, kind); w32(12, uint32_t(all.size())); w32(16, built);
        for (size_t i = 0; i < all.size(); ++i) {
            while (o.size() % 8) o.push_back('\0');                      // ★ 8-aligned: ROUT is read as u64s
            w32(32 + i * 12, all[i].first); w32(36 + i * 12, uint32_t(o.size())); w32(40 + i * 12, uint32_t(all[i].second.size()));
            o += all[i].second;
            std::string().swap(all[i].second);
        }
        return o;
    }

private:
    std::string strs_, meta_;
    std::unordered_map<std::string, uint32_t> pool_;
    std::vector<std::pair<uint32_t, std::string>> secs_;
};

inline void put32(std::string& s, uint32_t v) { s.append((const char*)&v, 4); }
inline void put16(std::string& s, uint16_t v) { s.append((const char*)&v, 2); }
inline void put24(std::string& s, uint32_t v) { s.push_back(char(v & 0xFF)); s.push_back(char(v >> 8 & 0xFF)); s.push_back(char(v >> 16 & 0xFF)); }
inline void putKey(std::string& s, const std::string& k, size_t n = 4) { char b[8] = {0}; std::memcpy(b, k.data(), std::min(n, k.size())); s.append(b, n); }
inline void putF(std::string& s, float v) { s.append((const char*)&v, 4); }

/** Write `bytes` to `path` ATOMICALLY: a .part beside it, fsync, rename. A reader sees the old file or the new
 *  one, never half of either; a failure leaves the old file as it was. */
inline bool writeAtomic(const std::string& path, const std::string& bytes, std::string& err) {
    const std::string tmp = path + ".part";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { err = "cannot write " + tmp + ": " + std::strerror(errno); return false; }
    size_t o = 0;
    while (o < bytes.size()) {
        const ssize_t w = ::write(fd, bytes.data() + o, bytes.size() - o);
        if (w <= 0) { if (errno == EINTR) continue; err = "write failed: " + std::string(std::strerror(errno)); ::close(fd); ::unlink(tmp.c_str()); return false; }
        o += size_t(w);
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { err = "rename failed: " + std::string(std::strerror(errno)); ::unlink(tmp.c_str()); return false; }
    return true;
}

struct BuildResult {
    std::string error;            ///< "" = built
    std::string licenceError;     ///< set when a licence pin failed (the error says the same)
    std::string bytes;            ///< the .vad file
    size_t aircraft = 0, routes = 0, airports = 0, airlines = 0, types = 0, operators = 0, blocks = 0, overlay = 0;
    std::string version;          ///< VRS: the commit (from the tarball); Mictronics: dbversion
    std::string licenceText;      ///< the upstream licence file, kept beside the tables (ODC-By §4.2: a copy travels with it)
};

// ── VRS standing data (the GitHub tarball of vradarserver/standing-data) ────────────────────────────
/** `starter`: only the small CC0 tables (airlines, model types, code blocks, countries) — the copy compiled into
 *  the server so a fresh install shows operator names, types, country and military with no network at all. */
inline BuildResult buildVrs(const uint8_t* tgz, size_t n, bool starter, const std::map<std::string, std::string>& meta,
                            uint32_t built = uint32_t(time(nullptr))) {
    BuildResult res;
    std::string licence, commit;
    // the tables as they arrive (airports by code, routes as packed key + airport sequence)
    struct Apt { std::string iata, name, loc, iso; float lat = 0, lon = 0; };
    std::map<std::string, Apt> airports;
    std::vector<uint64_t> routes;                                 // packed key << 22 | first-seen sequence id
    std::unordered_map<std::string, uint32_t> seqId;
    std::vector<std::string> seqs;
    struct Airl { std::string icao, iata, name; };
    std::map<std::string, Airl> airlines;
    struct Modl { std::string mf, model; char eng = 0, et = 0, sp = 0, wtc = 0; bool active = false; };
    std::map<std::string, Modl> models;
    struct Blk { uint32_t f, l; std::string iso; bool mil; };
    std::vector<Blk> blocks;
    std::map<std::string, std::string> countries;
    struct Ov { uint32_t hex; bool priv; std::string reg, type, model, oper, airline; };
    std::vector<Ov> overlay;
    size_t badRows = 0;

    auto kindOf = [](const std::string& name) -> std::string {
        // "standing-data-main/routes/schema-01/B/BAW-all.csv" → "routes"
        const size_t a = name.find('/');
        if (a == std::string::npos) return {};
        const size_t b = name.find('/', a + 1);
        if (b == std::string::npos) return name.substr(a + 1) == "LICENSE" ? "LICENSE" : std::string();
        if (name.compare(b, 11, "/schema-01/") != 0) return {};
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".csv") != 0) return {};
        return name.substr(a + 1, b - a - 1);
    };
    vibeinflate::TarReader tar;
    tar.want = [&](const std::string& name) {
        const std::string k = kindOf(name);
        if (k.empty()) return false;
        if (starter && (k == "routes" || k == "airports" || k == "aircraft")) return false;
        return k == "LICENSE" || k == "routes" || k == "airports" || k == "airlines" || k == "model-type" ||
               k == "code-blocks" || k == "countries" || k == "aircraft";
    };
    tar.global = [&](const std::string& k, const std::string& v) { if (k == "comment") commit = v; };
    tar.file = [&](const std::string& name, const std::string& body) {
        const std::string k = kindOf(name);
        if (k == "LICENSE") { licence = body; return; }
        const Csv c = parseCsv(body);
        auto get = [&](const std::vector<std::string>& r, int i) { return i >= 0 && size_t(i) < r.size() ? r[size_t(i)] : std::string(); };
        if (k == "routes") {
            const int cs = c.col("Callsign"), ap = c.col("AirportCodes");
            for (const auto& r : c.rows) {
                const uint64_t key = packCallsign(get(r, cs));
                const std::string seq = get(r, ap);
                if (!key || seq.find('-') == std::string::npos) { ++badRows; continue; }
                auto it = seqId.find(seq);
                uint32_t id;
                if (it == seqId.end()) { id = uint32_t(seqs.size()); seqId.emplace(seq, id); seqs.push_back(seq); }
                else id = it->second;
                routes.push_back(key << kSeqBits | id);
            }
        } else if (k == "airports") {
            const int co = c.col("Code"), nm = c.col("Name"), ia = c.col("IATA"), lo = c.col("Location"), is = c.col("CountryISO2"),
                      la = c.col("Latitude"), ln = c.col("Longitude");
            for (const auto& r : c.rows) {
                const std::string code = get(r, co);
                if (code.empty() || code.size() > 4) { ++badRows; continue; }
                Apt a; a.iata = get(r, ia); a.name = get(r, nm); a.loc = get(r, lo); a.iso = get(r, is);
                a.lat = std::strtof(get(r, la).c_str(), nullptr); a.lon = std::strtof(get(r, ln).c_str(), nullptr);
                airports[code] = a;
            }
        } else if (k == "airlines") {
            const int co = c.col("Code"), nm = c.col("Name"), ic = c.col("ICAO"), ia = c.col("IATA");
            for (const auto& r : c.rows) {
                const std::string code = get(r, co);
                if (code.empty() || code.size() > 4) { ++badRows; continue; }
                airlines[code] = Airl{get(r, ic), get(r, ia), get(r, nm)};
            }
        } else if (k == "model-type") {
            const int ic = c.col("ICAO"), mf = c.col("Manufacturer"), md = c.col("Model"), en = c.col("Engines"),
                      et = c.col("EngineTypeCode"), sp = c.col("SpeciesCode"), wt = c.col("WakeTurbulenceCode"), ac = c.col("IsActive");
            for (const auto& r : c.rows) {
                const std::string t = get(r, ic);
                if (t.empty() || t.size() > 4) continue;
                const bool active = get(r, ac) == "1";
                auto it = models.find(t);
                // ★ several models share a designator; the first ACTIVE one names it (the type table from Mictronics,
                //   when present, is preferred anyway — one description per designator)
                if (it != models.end() && (it->second.active || !active)) continue;
                Modl m; m.mf = get(r, mf); m.model = get(r, md); m.active = active;
                const std::string e = get(r, en), x = get(r, et), s = get(r, sp), w = get(r, wt);
                m.eng = e.empty() ? 0 : e[0]; m.et = x.empty() ? 0 : x[0]; m.sp = s.empty() ? 0 : s[0];
                m.wtc = w.empty() ? 0 : w.back();                           // "L/M" → M: the heavier, as a separation would
                models[t] = m;
            }
        } else if (k == "code-blocks") {
            const int st = c.col("Start"), fi = c.col("Finish"), mi = c.col("IsMilitary"), is = c.col("CountryISO2");
            for (const auto& r : c.rows) {
                const std::string a = get(r, st), b = get(r, fi);
                if (a.empty() || b.empty()) continue;
                blocks.push_back({uint32_t(std::strtoul(a.c_str(), nullptr, 16)), uint32_t(std::strtoul(b.c_str(), nullptr, 16)), get(r, is), get(r, mi) == "1"});
            }
        } else if (k == "countries") {
            const int is = c.col("ISO"), nm = c.col("Name");
            for (const auto& r : c.rows) if (get(r, is).size() == 2) countries[get(r, is)] = get(r, nm);
        } else if (k == "aircraft") {
            const int ic = c.col("ICAO"), rg = c.col("Registration"), mt = c.col("ModelICAO"), mm = c.col("ManufacturerAndModel"),
                      pr = c.col("IsPrivateOperator"), op = c.col("Operator"), al = c.col("AirlineCode");
            for (const auto& r : c.rows) {
                const std::string h = get(r, ic);
                if (h.size() != 6) { ++badRows; continue; }
                Ov o; o.hex = uint32_t(std::strtoul(h.c_str(), nullptr, 16)); o.priv = get(r, pr) == "1";
                o.reg = get(r, rg); o.type = get(r, mt); o.model = get(r, mm); o.airline = get(r, al);
                // ★★ a PRIVATE operator's name is never kept — it may be a person's (research §3.4)
                o.oper = o.priv ? std::string() : get(r, op);
                overlay.push_back(std::move(o));
            }
        }
    };
    const std::string e = vibeinflate::gunzip(tgz, n, [&](const uint8_t* b, size_t k) { return tar.feed(b, k); });
    if (!e.empty()) { res.error = "VRS standing data: " + e; return res; }
    res.licenceError = checkLicence(licence, kVrsLicenceFnv, kVrsLicencePhrase, "VRS standing data");
    res.licenceText = licence;
    if (!res.licenceError.empty()) { res.error = res.licenceError; return res; }
    if (airlines.empty() || models.empty() || blocks.empty()) { res.error = "VRS standing data: the tarball is missing its tables"; return res; }
    if (!starter && (routes.empty() || airports.empty())) { res.error = "VRS standing data: no routes or airports in the tarball"; return res; }

    Writer w;
    {   // airlines + the unambiguous IATA → ICAO map for callsign normalisation
        std::string s, ia;
        std::map<std::string, std::vector<std::string>> byIata;
        for (const auto& kv : airlines) {
            putKey(s, kv.first); putKey(s, kv.second.icao); putKey(s, kv.second.iata); put32(s, w.str(kv.second.name));
            if (!kv.second.iata.empty() && kv.second.icao.size() == 3) byIata[kv.second.iata].push_back(kv.second.icao);
        }
        // ★ an IATA code held by SEVERAL airlines (codes are reused) maps to none: a wrong airline is worse than none
        for (const auto& kv : byIata) if (kv.second.size() == 1) { putKey(ia, kv.first); putKey(ia, kv.second[0]); }
        w.section("AIRL", s); w.section("IATA", ia);
        res.airlines = airlines.size();
    }
    {
        std::string s;
        for (const auto& kv : models) {
            putKey(s, kv.first); put32(s, w.str(kv.second.mf)); put32(s, w.str(kv.second.model));
            s.push_back(kv.second.eng); s.push_back(kv.second.et); s.push_back(kv.second.sp); s.push_back(kv.second.wtc);
        }
        w.section("MODL", s);
        res.types = models.size();
    }
    {
        std::sort(blocks.begin(), blocks.end(), [](const Blk& a, const Blk& b) { return a.f < b.f || (a.f == b.f && a.l > b.l); });
        std::string s;
        for (const auto& b : blocks) { put32(s, b.f); put32(s, b.l); putKey(s, b.iso, 2); s.push_back(b.mil ? 1 : 0); s.push_back(0); }
        w.section("BLKS", s);
        res.blocks = blocks.size();
    }
    {
        std::string s;
        for (const auto& kv : countries) { putKey(s, kv.first, 2); s.push_back(0); s.push_back(0); put32(s, w.str(kv.second)); }
        w.section("CTRY", s);
    }
    if (!starter) {
        // ★ only the airports some route flies to — 3 800 of 34 000: the card names a route's ends, nothing more
        std::map<std::string, uint16_t> idx;
        for (const auto& sq : seqs) {
            size_t o = 0;
            while (o <= sq.size()) {
                size_t e2 = sq.find('-', o); if (e2 == std::string::npos) e2 = sq.size();
                const std::string c = sq.substr(o, e2 - o);
                if (airports.count(c)) idx[c] = 0;
                o = e2 + 1;
            }
        }
        if (idx.size() >= 0xFFFF) { res.error = "VRS standing data: more airports than the table can index"; return res; }
        std::string ap;
        uint16_t k = 0;
        for (auto& kv : idx) {
            kv.second = k++;
            const Apt& a = airports[kv.first];
            putKey(ap, kv.first); putKey(ap, a.iata); putF(ap, a.lat); putF(ap, a.lon);
            put32(ap, w.str(a.name)); put32(ap, w.str(a.loc)); putKey(ap, a.iso, 2); ap.push_back(0); ap.push_back(0);
        }
        w.section("APRT", ap);
        res.airports = idx.size();
        // sequences: u32 count, u32 offsets[count + 1], u16 airport indices
        std::vector<uint32_t> remap(seqs.size(), 0xFFFFFFFF);
        std::string offs, ids;
        uint32_t nseq = 0, nids = 0;
        for (size_t si = 0; si < seqs.size(); ++si) {
            std::vector<uint16_t> v;
            size_t o = 0; bool ok = true;
            const std::string& sq = seqs[si];
            while (o <= sq.size()) {
                size_t e2 = sq.find('-', o); if (e2 == std::string::npos) e2 = sq.size();
                auto it = idx.find(sq.substr(o, e2 - o));
                if (it == idx.end()) { ok = false; break; }
                v.push_back(it->second);
                o = e2 + 1;
            }
            if (!ok || v.size() < 2) continue;
            remap[si] = nseq++;
            put32(offs, nids);
            for (uint16_t x : v) put16(ids, x);
            nids += uint32_t(v.size());
        }
        put32(offs, nids);
        std::string sq; put32(sq, nseq); sq += offs; sq += ids;
        w.section("RSEQ", sq);
        if (nseq >= (1u << kSeqBits)) { res.error = "VRS standing data: too many distinct routes"; return res; }
        if (seqs.size() >= (1u << kSeqBits)) { res.error = "VRS standing data: too many distinct routes"; return res; }
        // ★ in place: the low bits become the written sequence's index, routes whose airports are unknown go
        std::vector<uint64_t>& keys = routes;
        size_t kept = 0;
        const uint64_t lowMask = (1ull << kSeqBits) - 1;
        for (uint64_t r : keys) { const uint32_t m = remap[size_t(r & lowMask)]; if (m != 0xFFFFFFFF) keys[kept++] = (r & ~lowMask) | m; }
        keys.resize(kept);
        seqId.clear(); seqs.clear(); seqs.shrink_to_fit(); airports.clear();
        std::sort(keys.begin(), keys.end());
        // a callsign twice (should not happen): the first wins, the rest go
        std::string rs; rs.reserve(keys.size() * 8);
        uint64_t prev = ~0ULL;
        for (uint64_t x : keys) { if ((x >> kSeqBits) == prev) continue; prev = x >> kSeqBits; rs.append((const char*)&x, 8); ++res.routes; }
        w.section("ROUT", rs);
        // the users' aircraft overlay
        std::sort(overlay.begin(), overlay.end(), [](const Ov& a, const Ov& b) { return a.hex < b.hex; });
        std::string ov;
        uint32_t last = 0xFFFFFFFF;
        for (const auto& o : overlay) {
            if (o.hex == last) continue; last = o.hex;
            put32(ov, (o.hex & 0xFFFFFF) | (o.priv ? 1u << 24 : 0));
            put32(ov, w.str(o.reg, false)); put32(ov, w.str(o.type)); put32(ov, w.str(o.model)); put32(ov, w.str(o.oper)); putKey(ov, o.airline);
            ++res.overlay;
        }
        w.section("ACOV", ov);
    }
    w.meta("kind", "vrs");
    w.meta("source", "Virtual Radar Server standing data (github.com/vradarserver/standing-data), CC0 1.0");
    w.meta("licence", "CC0-1.0");
    if (!commit.empty()) w.meta("commit", commit);
    for (const auto& kv : meta) w.meta(kv.first, kv.second);
    res.version = commit;
    res.bytes = w.bytes(kKindVrs, built);
    (void)badRows;
    return res;
}

// ── the Mictronics aircraft database (indexedDB.zip) ─────────────────────────────────────────────────
/** A streaming reader for aircrafts.json's one shape: {"HEX":["reg","type","flags"(,"desc")],…}. */
class AircraftJson {
public:
    std::function<void(const std::string& hex, const std::vector<std::string>& f)> row;
    void feed(const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const char c = char(b[i]);
            if (inStr_) {
                if (esc_) {
                    esc_ = false;
                    if (c == 'u') { uni_ = 4; code_ = 0; continue; }
                    cur_ += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
                } else if (uni_) {
                    code_ = code_ * 16 + uint32_t(std::isdigit((unsigned char)c) ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10));
                    if (--uni_ == 0) utf8(code_);
                } else if (c == '\\') esc_ = true;
                else if (c == '"') { inStr_ = false; if (depth_ == 1) key_ = cur_; else if (depth_ == 2) f_.push_back(cur_); cur_.clear(); }
                else cur_ += c;
                continue;
            }
            if (c == '"') { inStr_ = true; cur_.clear(); }
            else if (c == '{' || c == '[') { ++depth_; if (depth_ == 2) f_.clear(); }
            else if (c == '}' || c == ']') { if (depth_ == 2 && row) row(key_, f_); --depth_; }
        }
    }
private:
    void utf8(uint32_t u) {
        if (u < 0x80) cur_ += char(u);
        else if (u < 0x800) { cur_ += char(0xC0 | (u >> 6)); cur_ += char(0x80 | (u & 0x3F)); }
        else { cur_ += char(0xE0 | (u >> 12)); cur_ += char(0x80 | ((u >> 6) & 0x3F)); cur_ += char(0x80 | (u & 0x3F)); }
    }
    bool inStr_ = false, esc_ = false; int uni_ = 0; uint32_t code_ = 0; int depth_ = 0;
    std::string cur_, key_; std::vector<std::string> f_;
};

/** The small whole-file JSON tables (types.json, operators.json): {"KEY":["a","b","c"],…} → key → strings. */
inline std::map<std::string, std::vector<std::string>> smallJson(const std::string& body) {
    std::map<std::string, std::vector<std::string>> m;
    AircraftJson p;
    p.row = [&](const std::string& k, const std::vector<std::string>& f) { m[k] = f; };
    p.feed((const uint8_t*)body.data(), body.size());
    return m;
}

inline BuildResult buildMic(const uint8_t* zip, size_t n, const std::map<std::string, std::string>& meta,
                            uint32_t built = uint32_t(time(nullptr))) {
    BuildResult res;
    auto whole = [&](const char* name, std::string& out) {
        bool found = false;
        const std::string e = vibeinflate::unzipEntry(zip, n, name, [&](const uint8_t* b, size_t k) { out.append((const char*)b, k); return true; }, &found);
        return e.empty() ? (found ? std::string() : std::string(name) + " is missing") : e;
    };
    std::string licence, typesJ, opsJ, ver, comment, e;
    // ★ the zip's own comment says the licence too ("Made available under the Open Data Commons Attribution License")
    if (!(e = whole("LICENSE", licence)).empty()) { res.error = "Mictronics database: " + e; return res; }
    vibeinflate::unzipEntry(zip, n, "", [](const uint8_t*, size_t) { return true; }, nullptr, &comment);
    res.licenceError = checkLicence(licence, kMicLicenceFnv, kMicLicencePhrase, "Mictronics aircraft database");
    res.licenceText = licence;
    if (res.licenceError.empty() && comment.find(kMicLicencePhrase) == std::string::npos)
        res.licenceError = "Mictronics aircraft database: the archive no longer says it is under the ODC Attribution License — the update is held";
    if (!res.licenceError.empty()) { res.error = res.licenceError; return res; }
    if (!(e = whole("types.json", typesJ)).empty() || !(e = whole("operators.json", opsJ)).empty()) { res.error = "Mictronics database: " + e; return res; }
    whole("dbversion.json", ver);
    {
        const size_t c = ver.find(':');
        if (c != std::string::npos) { for (size_t i = c + 1; i < ver.size(); ++i) if (std::isdigit((unsigned char)ver[i])) res.version += ver[i]; }
    }
    Writer w;
    // types: every designator of types.json, plus any an aircraft names that types.json lacks (no description)
    auto types = smallJson(typesJ);
    typesJ.clear(); typesJ.shrink_to_fit();
    // ★ 12 bytes an aircraft (450 000 of them): the type as a provisional id, renumbered once types are sorted
    struct Ac { uint32_t hex; uint32_t reg; uint16_t type; uint8_t flags; };
    std::unordered_map<std::string, uint16_t> provisional;
    std::vector<std::string> provNames;
    std::vector<Ac> acs;
    acs.reserve(470000);
    std::string regs;                    // ★ registrations go straight to the pool, undeduplicated
    AircraftJson aj;
    aj.row = [&](const std::string& hex, const std::vector<std::string>& f) {
        if (hex.size() != 6 || f.empty()) return;
        char* end = nullptr;
        const unsigned long h = std::strtoul(hex.c_str(), &end, 16);
        if (!end || *end) return;
        Ac a; a.hex = uint32_t(h);
        a.reg = w.str(f[0], false);
        const std::string t = f.size() > 1 && f[1].size() <= 4 ? f[1] : std::string();
        a.type = 0xFFFF;
        if (!t.empty()) {
            auto it = provisional.find(t);
            if (it != provisional.end()) a.type = it->second;
            else if (provNames.size() < 0xFFFF) { a.type = uint16_t(provNames.size()); provisional.emplace(t, a.type); provNames.push_back(t); }
        }
        const std::string fl = f.size() > 2 ? f[2] : std::string();
        a.flags = uint8_t((fl.size() > 0 && fl[0] == '1' ? 1 : 0) | (fl.size() > 1 && fl[1] == '1' ? 2 : 0));   // military, interesting
        acs.push_back(a);
    };
    bool found = false;
    e = vibeinflate::unzipEntry(zip, n, "aircrafts.json", [&](const uint8_t* b, size_t k) { aj.feed(b, k); return true; }, &found);
    if (!e.empty() || !found) { res.error = "Mictronics database: " + (e.empty() ? std::string("aircrafts.json is missing") : e); return res; }
    if (acs.size() < 1000) { res.error = "Mictronics database: aircrafts.json is implausibly small"; return res; }
    if (w.poolSize() >= (1u << 24)) { res.error = "Mictronics database: registrations overflow the table"; return res; }
    for (const auto& t : provNames) if (!types.count(t)) types[t] = {};
    std::map<std::string, uint16_t> tidx;
    {
        std::string s;
        uint16_t k = 0;
        for (const auto& kv : types) {
            if (kv.first.empty() || kv.first.size() > 4) continue;
            tidx[kv.first] = k++;
            const auto& f = kv.second;
            const std::string d = f.size() > 0 ? f[0] : "", cls = f.size() > 1 ? f[1] : "", wt = f.size() > 2 ? f[2] : "";
            putKey(s, kv.first); put32(s, w.str(d));
            // "L4J" = species L(and)/H(eli)/S(ea)/A(mphibian)/G(yro)/T(iltrotor), engine count, engine type
            s.push_back(cls.size() > 0 ? cls[0] : 0); s.push_back(cls.size() > 1 ? cls[1] : 0); s.push_back(cls.size() > 2 ? cls[2] : 0);
            s.push_back(wt.empty() ? 0 : wt[0]);
        }
        w.section("TYPE", s);
        res.types = tidx.size();
    }
    std::sort(acs.begin(), acs.end(), [](const Ac& a, const Ac& b) { return a.hex < b.hex; });
    {
        std::string s; s.reserve(acs.size() * kAcftRec);
        uint32_t last = 0xFFFFFFFF;
        for (const auto& a : acs) {
            if (a.hex == last) continue; last = a.hex;
            put24(s, a.hex); s.push_back(char(a.flags)); put24(s, a.reg);
            uint16_t ti = 0xFFFF;
            if (a.type != 0xFFFF) { auto it = tidx.find(provNames[a.type]); if (it != tidx.end()) ti = it->second; }
            put16(s, ti);
            ++res.aircraft;
        }
        acs.clear(); acs.shrink_to_fit();
        w.section("ACFT", s);
    }
    {
        auto ops = smallJson(opsJ);
        std::string s;
        for (const auto& kv : ops) {
            if (kv.first.size() != 3) continue;      // ★ ICAO designators are three letters; the odd longer keys are not
            const auto& f = kv.second;
            putKey(s, kv.first); put32(s, w.str(f.size() > 0 ? f[0] : "")); put32(s, w.str(f.size() > 1 ? f[1] : "")); put32(s, w.str(f.size() > 2 ? f[2] : ""));
            ++res.operators;
        }
        w.section("OPER", s);
    }
    w.meta("kind", "mic");
    w.meta("source", "Mictronics aircraft database (github.com/Mictronics/aircraft-database), ODC-By 1.0");
    w.meta("licence", "ODC-By-1.0");
    w.meta("notice", "Contains information from Mictronics aircraft database, which is made available under the ODC Attribution License.");
    if (!res.version.empty()) w.meta("dbversion", res.version);
    for (const auto& kv : meta) w.meta(kv.first, kv.second);
    res.bytes = w.bytes(kKindMic, built);
    return res;
}

} // namespace vibeadsbdata
