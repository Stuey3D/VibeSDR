// vibe_adsb_data.h — what an aircraft IS: registration, type, operator, country, military, route. The reader for
// the compact binary tables the ADS-B data builder writes (vibe_adsb_data_build.h), and the enrichment the
// decoder service attaches to each aircraft once (vibe_adsb_service.h). Design: docs/v12/ADSB-DATA-RESEARCH.md.
//
// ★★★ LOW RAM BY CONSTRUCTION — the floor is a Pi 2 and a 1 GB, 32-bit Android phone (memory
//     android_potato_phone_users, minimum_confirmed_spec). The tables are sorted fixed-size records on disk,
//     MEMORY-MAPPED and binary-searched: resident memory is the pages a lookup touched, never a heap map of
//     450 000 aircraft. The starter copy compiled into the binary (vibe_adsb_starter.h) is .rodata — also paged in
//     only when read.
// ★★★ NEVER ON THE DSP THREAD. A first lookup can fault a page in from an SD card. The service calls this from its
//     own decoder thread with its tracker lock RELEASED (Network > Audio > Spectrum > Decoders — memory
//     feedback_thread_priority_order), and only once per aircraft, or when its callsign changes.
// ★★ TWO FILES, TWO SOURCES, TWO LICENCES, each replaced atomically by the weekly update (vibe_adsb_data_update.h):
//    - `vrs.vad`  — Virtual Radar Server standing data (CC0 1.0): routes, airports, airlines, model types, code
//                   blocks (military sub-blocks + territories), countries, and VRS users' aircraft overlay.
//    - `mic.vad`  — the Mictronics aircraft database (ODC-By 1.0): 450 000 aircraft by ICAO address, its type and
//                   operator tables. ODC-By asks for a notice wherever the output is shown: every client carries it.
//    The starter (no network) is a CC0-only subset of vrs.vad, so the package itself conveys no ODC-By data.
// ★★ PRIVACY (research §3.4): no table here holds a private individual's name. An aircraft VRS marks as privately
//    operated shows "Private", never its owner.
// ★ Little-endian on disk, read with memcpy: every target we build (x86, arm, arm64) is little-endian.
#pragma once
#include "vibe_adsb_blocks.h"
#include "vibe_adsb_starter.h"   // the CC0 starter tables compiled in (generated)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace vibeadsbdata {

// ── the container ────────────────────────────────────────────────────────────────────────────────────
// Header (32 bytes): "VADB", u32 version, u32 kind, u32 sections, u32 built (unix), 3 × u32 reserved;
// then `sections` × {u32 id, u32 offset, u32 size}. Each section is a packed array of fixed-size records,
// except STRS (NUL-terminated UTF-8, offset 0 = "") and META ("key=value\n" lines: source, etag, licence…).
constexpr uint32_t kVersion = 1;
constexpr uint32_t kKindVrs = 1, kKindMic = 2;
constexpr uint32_t fourcc(const char (&s)[5]) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; }

// Record sizes — the builder writes exactly these (static layout, see vibe_adsb_data_build.h)
constexpr size_t kAirlRec = 16;   // code[4] icao[4] iata[4] u32 name
constexpr size_t kIataRec = 8;    // iata[4] icao[4]   — only IATA codes held by ONE airline with an ICAO code
constexpr size_t kModlRec = 16;   // icao[4] u32 manufacturer u32 model, engines, engineType, species, wtc
constexpr size_t kBlkRec  = 12;   // u32 first u32 last iso[2] u8 mil u8 0
constexpr size_t kCtryRec = 8;    // iso[2] 0 0 u32 name
constexpr size_t kAptRec  = 28;   // code[4] iata[4] f32 lat f32 lon u32 name u32 location iso[2] 0 0
constexpr size_t kAcovRec = 24;   // u32 hex|flags<<24 u32 reg u32 type u32 model u32 operator airline[4]
constexpr size_t kAcftRec = 9;    // hex(3) flags(1) reg(3) type(2)   — packed, 450 000 of them
constexpr size_t kTypeRec = 12;   // icao[4] u32 desc, species, engines, engineType, wtc
constexpr size_t kOperRec = 16;   // code[4] u32 name u32 country u32 telephony

inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t rd24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
inline float    rdf(const uint8_t* p)  { float v; std::memcpy(&v, p, 4); return v; }

/** A 4-byte, NUL-padded code key (an ICAO designator, an airport code). */
inline void key4(const std::string& s, char k[4]) { std::memset(k, 0, 4); std::memcpy(k, s.data(), std::min<size_t>(4, s.size())); }
inline std::string unkey(const uint8_t* p, size_t n = 4) { size_t l = 0; while (l < n && p[l]) ++l; return std::string((const char*)p, l); }

class Vad {
public:
    struct Sec { const uint8_t* p = nullptr; size_t n = 0; size_t count(size_t rec) const { return rec ? n / rec : 0; } };

    bool open(const uint8_t* p, size_t n, std::string* err = nullptr) {
        p_ = nullptr; n_ = 0; secs_.clear();
        auto fail = [&](const char* w) { if (err) *err = w; return false; };
        if (!p || n < 32 || std::memcmp(p, "VADB", 4) != 0) return fail("not a VibeSDR ADS-B data file");
        if (rd32(p + 4) != kVersion) return fail("unknown data file version");
        kind_ = rd32(p + 8);
        built_ = rd32(p + 16);
        const uint32_t ns = rd32(p + 12);
        if (ns > 64 || 32 + size_t(ns) * 12 > n) return fail("bad section table");
        for (uint32_t i = 0; i < ns; ++i) {
            const uint8_t* e = p + 32 + i * 12;
            const uint32_t id = rd32(e), off = rd32(e + 4), sz = rd32(e + 8);
            if (size_t(off) + sz > n) return fail("section past the end of the file");
            secs_[id] = Sec{p + off, sz};
        }
        p_ = p; n_ = n;
        strs_ = sec(fourcc("STRS"));
        return true;
    }
    bool ok() const { return p_ != nullptr; }
    uint32_t kind() const { return kind_; }
    uint32_t built() const { return built_; }
    Sec sec(uint32_t id) const { auto it = secs_.find(id); return it == secs_.end() ? Sec{} : it->second; }
    bool has(uint32_t id) const { return sec(id).n > 0; }
    /** A string from the pool. ★ Bounds-checked: a corrupt offset gives "", never a read past the mapping. */
    std::string str(uint32_t off) const {
        if (!off || off >= strs_.n) return {};
        const uint8_t* s = strs_.p + off;
        const size_t max = strs_.n - off;
        const size_t l = strnlen((const char*)s, max);
        return std::string((const char*)s, l);
    }
    std::string meta(const std::string& key) const {
        const Sec m = sec(fourcc("META"));
        const std::string t((const char*)m.p, m.n), k = key + "=";
        size_t o = 0;
        while (o < t.size()) {
            size_t e = t.find('\n', o); if (e == std::string::npos) e = t.size();
            if (t.compare(o, k.size(), k) == 0) return t.substr(o + k.size(), e - o - k.size());
            o = e + 1;
        }
        return {};
    }

    /** Binary search a section of `rec`-byte records sorted by a memcmp key of `klen` bytes at offset 0. */
    const uint8_t* find(uint32_t id, size_t rec, const void* key, size_t klen) const {
        const Sec s = sec(id);
        size_t lo = 0, hi = s.count(rec);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const int c = std::memcmp(s.p + mid * rec, key, klen);
            if (c == 0) return s.p + mid * rec;
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return nullptr;
    }

private:
    const uint8_t* p_ = nullptr; size_t n_ = 0;
    uint32_t kind_ = 0, built_ = 0;
    std::map<uint32_t, Sec> secs_;
    Sec strs_;
};

/** A read-only mapping of a data file. ★ The old mapping stays valid after the updater renames a new file over
 *  the path (the inode lives while it is mapped), so a reload can never pull pages out from under a lookup. */
class Mapped {
public:
    Mapped() = default;
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
    Mapped(Mapped&& o) noexcept { *this = std::move(o); }
    Mapped& operator=(Mapped&& o) noexcept {
        if (this != &o) { close(); p_ = o.p_; n_ = o.n_; ino_ = o.ino_; mtime_ = o.mtime_; o.p_ = nullptr; o.n_ = 0; }
        return *this;
    }
    ~Mapped() { close(); }
    bool open(const std::string& path) {
        close();
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat sb{};
        if (::fstat(fd, &sb) != 0 || sb.st_size < 32) { ::close(fd); return false; }
        void* m = ::mmap(nullptr, size_t(sb.st_size), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return false;
        p_ = (const uint8_t*)m; n_ = size_t(sb.st_size);
        ino_ = uint64_t(sb.st_ino); mtime_ = int64_t(sb.st_mtime);
        return true;
    }
    void close() { if (p_) ::munmap((void*)p_, n_); p_ = nullptr; n_ = 0; ino_ = 0; mtime_ = 0; }
    const uint8_t* data() const { return p_; }
    size_t size() const { return n_; }
    /** Has the file at `path` been replaced since it was mapped (or appeared, or gone)? */
    bool stale(const std::string& path) const {
        struct stat sb{};
        if (::stat(path.c_str(), &sb) != 0) return p_ != nullptr;
        return !p_ || uint64_t(sb.st_ino) != ino_ || int64_t(sb.st_mtime) != mtime_ || size_t(sb.st_size) != n_;
    }
private:
    const uint8_t* p_ = nullptr; size_t n_ = 0; uint64_t ino_ = 0; int64_t mtime_ = 0;
};

// ── callsigns ────────────────────────────────────────────────────────────────────────────────────────
/** The VRS normalisation (routes/schema-01/README.md, CC0 — our own implementation of the documented rule):
 *  split code + number, strip the number's leading zeros (a lone "0" stays), and accept only n, nn, nA, nnn, nnA,
 *  nAA, nnnn, nnnA, nnAA. `iataToIcao` swaps a 2-character IATA code for the airline's ICAO code. */
template <class IataFn>
inline bool normaliseCallsign(const std::string& in, std::string& code, std::string& number, IataFn iataToIcao) {
    std::string cs;
    for (char c : in) if (c != ' ') cs += char(std::toupper((unsigned char)c));
    auto L = [&](size_t i) { return i < cs.size() && cs[i] >= 'A' && cs[i] <= 'Z'; };
    auto D = [&](size_t i) { return i < cs.size() && cs[i] >= '0' && cs[i] <= '9'; };
    size_t cl = 0;
    if (L(0) && L(1) && L(2) && D(3)) cl = 3;
    else if (L(0) && L(1) && D(2)) cl = 2;
    else if (L(0) && D(1) && D(2)) cl = 2;          // "U2" + digits
    else if (D(0) && L(1) && D(2)) cl = 2;          // "9W" + digits
    if (!cl) return false;
    for (size_t i = cl; i < cs.size(); ++i) if (!L(i) && !D(i)) return false;
    code = cs.substr(0, cl);
    std::string num = cs.substr(cl);
    size_t z = 0; while (z < num.size() && num[z] == '0') ++z;
    num = num.substr(z);
    if (num.empty() || !(num[0] >= '0' && num[0] <= '9')) num = "0" + num;
    if (num.size() > 4) return false;
    size_t letters = 0;
    for (size_t i = 0; i < num.size(); ++i) {
        const bool isL = num[i] >= 'A' && num[i] <= 'Z';
        if (isL) ++letters; else if (letters) return false;   // a digit after a letter
    }
    if (letters > 2 || (letters && num.size() - letters < 1)) return false;
    if (cl == 2) { const std::string icao = iataToIcao(code); if (!icao.empty()) code = icao; }
    number = num;
    return true;
}

/** 7 characters × 6 bits, left-aligned in 42 bits: sorts like the strings. 0 = padding, '0'..'9' = 1..10,
 *  'A'..'Z' = 11..36. Returns 0 for a callsign this cannot hold. */
inline uint64_t packCallsign(const std::string& s) {
    if (s.empty() || s.size() > 7) return 0;
    uint64_t k = 0;
    for (size_t i = 0; i < 7; ++i) {
        uint64_t v = 0;
        if (i < s.size()) {
            const char c = s[i];
            if (c >= '0' && c <= '9') v = uint64_t(c - '0') + 1;
            else if (c >= 'A' && c <= 'Z') v = uint64_t(c - 'A') + 11;
            else return 0;
        }
        k = (k << 6) | v;
    }
    return k;
}
constexpr int kSeqBits = 22;     // ROUT entries: packed callsign << 22 | sequence index

// ── geometry (for the route check) ───────────────────────────────────────────────────────────────────
inline double rad(double d) { return d * 3.14159265358979323846 / 180.0; }
inline double gcNm(double la1, double lo1, double la2, double lo2) {
    const double a = std::sin(rad(la2 - la1) / 2), b = std::sin(rad(lo2 - lo1) / 2);
    const double h = a * a + std::cos(rad(la1)) * std::cos(rad(la2)) * b * b;
    return 2 * 3440.065 * std::asin(std::min(1.0, std::sqrt(h)));
}
inline double bearing(double la1, double lo1, double la2, double lo2) {
    const double y = std::sin(rad(lo2 - lo1)) * std::cos(rad(la2));
    const double x = std::cos(rad(la1)) * std::sin(rad(la2)) - std::sin(rad(la1)) * std::cos(rad(la2)) * std::cos(rad(lo2 - lo1));
    return std::atan2(y, x);
}
/** How far (NM) P is from the great-circle SEGMENT A→B: the cross-track distance while P is alongside the segment,
 *  else the distance to the nearer end. */
inline double segmentNm(double pla, double plo, double ala, double alo, double bla, double blo) {
    const double R = 3440.065;
    const double dap = gcNm(ala, alo, pla, plo) / R;
    const double xt = std::asin(std::sin(dap) * std::sin(bearing(ala, alo, pla, plo) - bearing(ala, alo, bla, blo)));
    const double at = std::acos(std::max(-1.0, std::min(1.0, std::cos(dap) / std::max(1e-12, std::cos(xt)))));
    const double len = gcNm(ala, alo, bla, blo) / R;
    const double ahead = std::cos(bearing(ala, alo, pla, plo) - bearing(ala, alo, bla, blo));
    if (ahead < 0 || at > len) return std::min(gcNm(pla, plo, ala, alo), gcNm(pla, plo, bla, blo));
    return std::fabs(xt) * R;
}

// ── what an aircraft is ──────────────────────────────────────────────────────────────────────────────
struct Airport { std::string code, iata, name, location, iso; double lat = 0, lon = 0; };

struct Info {
    std::string r, t, desc, wtc, eng, species, country, countryName, op, opName, logo, sil;
    bool mil = false;
    std::vector<Airport> route;   ///< every airport of the filed route, in flight order (empty = none known)
    int leg = 0;                  ///< the leg the aircraft looks to be on (route[leg] → route[leg + 1])
    int plausible = -1;           ///< -1 = not judged yet (no position), 0 = the aircraft is far off it, 1 = on it

    bool empty() const { return r.empty() && t.empty() && country.empty() && op.empty() && route.empty() && !mil; }
    bool operator==(const Info& o) const { return json() == o.json(); }
    bool operator!=(const Info& o) const { return !(*this == o); }

    static void esc(std::string& o, const std::string& s) {
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    /** The fields, readsb's names where readsb has one (r, t, desc) — ADSB.md. Absent = unknown. */
    std::string json() const {
        std::string o = "{";
        auto add = [&](const char* k, const std::string& v) {
            if (v.empty()) return;
            if (o.size() > 1) o += ',';
            o += '"'; o += k; o += "\":\""; esc(o, v); o += '"';
        };
        add("r", r); add("t", t); add("desc", desc); add("wtc", wtc); add("eng", eng); add("species", species);
        add("country", country); add("countryName", countryName);
        if (mil) { if (o.size() > 1) o += ','; o += "\"mil\":1"; }
        add("op", op); add("opName", opName); add("logo", logo); add("sil", sil);
        if (route.size() >= 2) {
            const Airport& a = route[size_t(leg)];
            const Airport& b = route[size_t(leg) + 1];
            if (o.size() > 1) o += ',';
            o += "\"route\":{";
            std::string ro;
            auto radd = [&](const char* k, const std::string& v) {
                if (v.empty()) return;
                if (!ro.empty()) ro += ',';
                ro += '"'; ro += k; ro += "\":\""; esc(ro, v); ro += '"';
            };
            radd("from", a.code); radd("to", b.code); radd("fromIata", a.iata); radd("toIata", b.iata);
            radd("fromName", a.name); radd("toName", b.name); radd("fromPlace", a.location); radd("toPlace", b.location);
            if (route.size() > 2) {
                ro += ",\"stops\":[";
                for (size_t i = 0; i < route.size(); ++i) { if (i) ro += ','; ro += '"'; esc(ro, route[i].code); ro += '"'; }
                ro += ']';
            }
            if (plausible >= 0) ro += std::string(",\"plausible\":") + (plausible ? "true" : "false");
            o += ro + "}";
        }
        return o + "}";
    }
};

/** Owner-supplied operator logos and silhouettes (research §1.4): VRS-compatible names — `<hex>`, `<registration>`,
 *  `<operator>-<type>`, `<operator>` for logos, `<hex>`, `<type>` for silhouettes — .png, .bmp, .svg, .gif, .jpg.
 *  ★★ WE SHIP NONE, DOWNLOAD NONE, LINK TO NONE: they are airlines' trademarks. The owner's own folders only.
 *  ★★ PATHS COME FROM readdir(), NEVER FROM A REQUEST: a request names a KEY, and only a key that is in this index
 *     has a file — so "../" or "%2e%2e" cannot reach anything, by construction (test-adsb-data). */
class Images {
public:
    void setDir(const std::string& d) { std::lock_guard<std::mutex> lk(m_); dir_ = d; scanned_ = 0; }
    std::string dir() { std::lock_guard<std::mutex> lk(m_); return dir_; }
    /** A key a request may name: letters, digits, '-' and '_', up to 40 — anything else is refused outright. */
    static bool validKey(const std::string& k) {
        if (k.empty() || k.size() > 40) return false;
        for (char c : k) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
        return true;
    }
    static std::string upper(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; }
    /** kind 0 = logo (OperatorFlags), 1 = silhouette (Silhouettes). Fills `path` + `mime` when the key has a file. */
    bool find(int kind, const std::string& key, std::string* path = nullptr, std::string* mime = nullptr) {
        if (!validKey(key)) return false;
        std::lock_guard<std::mutex> lk(m_);
        rescanLocked();
        const auto& idx = kind ? sil_ : logo_;
        auto it = idx.find(upper(key));
        if (it == idx.end()) return false;
        if (path) *path = dir_ + (kind ? "/Silhouettes/" : "/OperatorFlags/") + it->second;
        if (mime) {
            const std::string& f = it->second;
            const std::string ext = upper(f.substr(f.rfind('.') + 1));
            *mime = ext == "PNG" ? "image/png" : ext == "SVG" ? "image/svg+xml" : ext == "GIF" ? "image/gif"
                  : ext == "JPG" ? "image/jpeg" : "image/bmp";
        }
        return true;
    }
    bool any(int kind) { std::lock_guard<std::mutex> lk(m_); rescanLocked(); return !(kind ? sil_ : logo_).empty(); }
    size_t count(int kind) { std::lock_guard<std::mutex> lk(m_); rescanLocked(); return (kind ? sil_ : logo_).size(); }

private:
    void rescanLocked() {
        // ★ Every five minutes at most: an owner dropping a pack in sees it within minutes, and a readdir of
        //   6 500 names is a few milliseconds on a Pi, off every hot path.
        const time_t now = time(nullptr);
        if (scanned_ && now - scanned_ < 300) return;
        scanned_ = now;
        logo_ = scan(dir_ + "/OperatorFlags");
        sil_ = scan(dir_ + "/Silhouettes");
    }
    static std::unordered_map<std::string, std::string> scan(const std::string& d) {
        std::unordered_map<std::string, std::string> m;
        if (d.size() < 2) return m;
        DIR* dp = ::opendir(d.c_str());
        if (!dp) return m;
        static const char* pref[] = {"PNG", "BMP", "SVG", "GIF", "JPG"};   // ★ one key, several files: the first wins
        auto rank = [](const std::string& ext) { for (int i = 0; i < 5; ++i) if (ext == pref[i]) return i; return 9; };
        while (dirent* e = ::readdir(dp)) {
            const std::string f = e->d_name;
            const size_t dot = f.rfind('.');
            if (dot == std::string::npos || dot == 0 || f[0] == '.') continue;
            const std::string base = f.substr(0, dot), ext = upper(f.substr(dot + 1));
            if (rank(ext) > 4 || !validKey(base)) continue;
            const std::string k = upper(base);
            auto it = m.find(k);
            if (it == m.end() || rank(ext) < rank(upper(it->second.substr(it->second.rfind('.') + 1)))) m[k] = f;
        }
        ::closedir(dp);
        return m;
    }
    std::mutex m_;
    std::string dir_;
    time_t scanned_ = 0;
    std::unordered_map<std::string, std::string> logo_, sil_;
};

class Db {
public:
    static Db& get() { static Db d; return d; }
    static Images& images() { static Images i; return i; }

    /** `<data dir>/adsb` — where vrs.vad and mic.vad live (written by the updater). */
    void setDir(const std::string& d) { std::lock_guard<std::mutex> lk(m_); dir_ = d; checked_ = 0; }
    std::string dir() { std::lock_guard<std::mutex> lk(m_); return dir_; }

    /** Re-map a file the updater has replaced. Cheap (two stat()s), and at most once a minute unless forced. */
    void refresh(bool force = false) {
        std::lock_guard<std::mutex> lk(m_);
        refreshLocked(force);
    }

    /** Everything known from the address alone: country, military, registration, type and its description. */
    void lookupHex(uint32_t hex, Info& i) {
        std::lock_guard<std::mutex> lk(m_);
        refreshLocked(false);
        // country: our Annex 10 table, unless VRS has a SMALLER block (a territory: Cayman in the UK's block)
        const char* st = stateFor(hex);
        if (st) i.country = st;
        // ★ VRS: "look for the smallest range that contains" the address (code-blocks/schema-01/README.md)
        uint32_t best = 0xFFFFFFFF;
        const uint8_t* bestRec = nullptr;
        const Vad& v = vrs();
        const Vad::Sec b = v.sec(fourcc("BLKS"));
        for (size_t k = 0; k < b.count(kBlkRec); ++k) {
            const uint8_t* r = b.p + k * kBlkRec;
            const uint32_t f = rd32(r), l = rd32(r + 4);
            if (f > hex) break;                                  // sorted by first
            if (hex > l || l - f >= best) continue;
            best = l - f; bestRec = r;
        }
        if (bestRec) {
            const std::string iso = unkey(bestRec + 8, 2);
            if (bestRec[10]) i.mil = true;
            // ★ a military block inside a state's (UK 43C000–43FFFF) keeps the state's country; a SMALL civil block
            //   that disagrees with Annex 10 is a territory the state sub-allocated (Cayman, Aruba, Hong Kong)
            else if (!iso.empty() && iso != "ZZ" && (!st || best < 0x400)) i.country = iso;
        }
        if (!i.country.empty()) i.countryName = countryNameLocked(i.country);
        // the Mictronics aircraft (registration, type, military flag)
        if (mic_.ok()) {
            const Vad::Sec a = mic_.sec(fourcc("ACFT"));
            size_t lo = 0, hi = a.count(kAcftRec);
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                const uint32_t h = rd24(a.p + mid * kAcftRec);
                if (h == hex) {
                    const uint8_t* r = a.p + mid * kAcftRec;
                    if (r[3] & 1) i.mil = true;
                    i.r = mic_.str(rd24(r + 4));
                    const uint16_t ti = rd16(r + 7);
                    const Vad::Sec ts = mic_.sec(fourcc("TYPE"));
                    if (ti != 0xFFFF && size_t(ti) < ts.count(kTypeRec)) i.t = unkey(ts.p + size_t(ti) * kTypeRec);
                    break;
                }
                if (h < hex) lo = mid + 1; else hi = mid;
            }
        }
        // VRS users' overlay: fills what Mictronics lacks, and knows a PRIVATE operator from a company
        if (const uint8_t* r = findHexLocked(v, hex)) {
            const uint32_t w = rd32(r);
            if (i.r.empty()) i.r = v.str(rd32(r + 4));
            if (i.t.empty()) i.t = v.str(rd32(r + 8));
            (void)w;
            if (i.desc.empty()) i.desc = v.str(rd32(r + 12));
        }
        if (!i.t.empty()) typeLocked(i.t, i);
    }

    /** Operator and route from the callsign. `reg` keeps a registration-as-callsign ("GABCD") from reading as
     *  an airline. `hex` lets VRS's overlay name the operator when the callsign does not. */
    void lookupCallsign(const std::string& cs, const std::string& reg, uint32_t hex, Info& i) {
        std::lock_guard<std::mutex> lk(m_);
        refreshLocked(false);
        i.op.clear(); i.opName.clear(); i.route.clear(); i.leg = 0; i.plausible = -1;
        std::string regPlain;
        for (char c : reg) if (c != '-') regPlain += char(std::toupper((unsigned char)c));
        std::string csPlain;
        for (char c : cs) if (c != ' ') csPlain += char(std::toupper((unsigned char)c));
        const bool isReg = !regPlain.empty() && csPlain == regPlain;
        std::string code, number;
        const bool flight = !isReg && normaliseCallsign(csPlain, code, number, [&](const std::string& ia) { return iataLocked(ia); });
        if (flight && code.size() == 3) {
            std::string name;
            if (operatorLocked(code, name)) { i.op = code; i.opName = name; }
        }
        // ★ No airline callsign: VRS users' overlay may know the operator — a COMPANY's name, or "Private" (never
        //   an individual's name: research §3.4)
        if (i.op.empty()) if (const uint8_t* r = findHexLocked(vrs(), hex)) {
            const std::string al = unkey(r + 20);
            std::string name;
            if (rd32(r) >> 24 & 1) i.opName = "Private";
            else if (!al.empty() && operatorLocked(al, name)) { i.op = al; i.opName = name; }
            else i.opName = vrs().str(rd32(r + 16));
        }
        if (flight) routeLocked(code + number, i);
    }

    /** Judge the route against the aircraft's position: on it if within max(60 NM, a quarter of the leg) of any
     *  leg's great circle — our own rule. Picks the leg it is nearest. */
    static void judge(Info& i, double lat, double lon) {
        if (i.route.size() < 2) { i.plausible = -1; return; }
        double bestScore = 1e18; int bestLeg = 0; bool on = false;
        for (size_t k = 0; k + 1 < i.route.size(); ++k) {
            const Airport& a = i.route[k];
            const Airport& b = i.route[k + 1];
            const double len = gcNm(a.lat, a.lon, b.lat, b.lon);
            const double d = segmentNm(lat, lon, a.lat, a.lon, b.lat, b.lon);
            const double allow = std::max(60.0, 0.25 * len);
            const double score = d / allow;
            if (score < bestScore) { bestScore = score; bestLeg = int(k); }
            if (d <= allow) on = true;
        }
        i.leg = bestLeg;
        i.plausible = on ? 1 : 0;
    }

    /** Owner logos/silhouettes for this aircraft, in VRS's candidate order. */
    static void resolveImages(const std::string& hexUpper, Info& i) {
        Images& im = images();
        i.logo.clear(); i.sil.clear();
        std::vector<std::string> lc = {hexUpper, i.r};
        if (!i.op.empty() && !i.t.empty()) lc.push_back(i.op + "-" + i.t);
        lc.push_back(i.op);
        for (const auto& k : lc) if (!k.empty() && im.find(0, k)) { i.logo = Images::upper(k); break; }
        for (const auto& k : {hexUpper, i.t}) if (!k.empty() && im.find(1, k)) { i.sil = Images::upper(k); break; }
    }

    // ── status, for the setup page ──
    struct Status { bool vrsFile = false, micFile = false; uint32_t vrsBuilt = 0, micBuilt = 0, starterBuilt = 0;
                    size_t aircraft = 0, routes = 0, airports = 0, operators = 0, types = 0; std::string vrsSource, micSource; };
    Status status() {
        std::lock_guard<std::mutex> lk(m_);
        refreshLocked(false);
        Status s;
        s.vrsFile = vrsFile_.data() != nullptr; s.micFile = mic_.ok();
        s.vrsBuilt = s.vrsFile ? vrsF_.built() : 0; s.micBuilt = s.micFile ? mic_.built() : 0;
        s.starterBuilt = starter_.ok() ? starter_.built() : 0;
        const Vad& v = vrs();
        s.routes = v.sec(fourcc("ROUT")).count(8);
        s.airports = v.sec(fourcc("APRT")).count(kAptRec);
        s.aircraft = mic_.sec(fourcc("ACFT")).count(kAcftRec);
        s.operators = std::max(mic_.sec(fourcc("OPER")).count(kOperRec), v.sec(fourcc("AIRL")).count(kAirlRec));
        s.types = std::max(mic_.sec(fourcc("TYPE")).count(kTypeRec), v.sec(fourcc("MODL")).count(kModlRec));
        s.vrsSource = v.meta("source"); s.micSource = mic_.meta("source");
        return s;
    }

private:
    Db() {
        size_t n = 0;
        const uint8_t* p = starterVrs(n);
        if (p && n) starter_.open(p, n);
    }
    const Vad& vrs() const { return vrsF_.ok() ? vrsF_ : starter_; }

    void refreshLocked(bool force) {
        const time_t now = time(nullptr);
        if (!force && checked_ && now - checked_ < 60) return;
        checked_ = now;
        if (dir_.empty()) return;
        const std::string vp = dir_ + "/vrs.vad", mp = dir_ + "/mic.vad";
        if (vrsFile_.stale(vp)) {
            Vad nv; Mapped nm;
            if (nm.open(vp) && nv.open(nm.data(), nm.size()) && nv.kind() == kKindVrs) { vrsFile_ = std::move(nm); vrsF_.open(vrsFile_.data(), vrsFile_.size()); }
            else if (!nm.data()) { vrsFile_.close(); vrsF_ = Vad(); }
        }
        if (micFile_.stale(mp)) {
            Vad nv; Mapped nm;
            if (nm.open(mp) && nv.open(nm.data(), nm.size()) && nv.kind() == kKindMic) { micFile_ = std::move(nm); mic_.open(micFile_.data(), micFile_.size()); }
            else if (!nm.data()) { micFile_.close(); mic_ = Vad(); }
        }
    }

    const uint8_t* findHexLocked(const Vad& v, uint32_t hex) const {
        const Vad::Sec s = v.sec(fourcc("ACOV"));
        size_t lo = 0, hi = s.count(kAcovRec);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const uint32_t h = rd32(s.p + mid * kAcovRec) & 0xFFFFFF;
            if (h == hex) return s.p + mid * kAcovRec;
            if (h < hex) lo = mid + 1; else hi = mid;
        }
        return nullptr;
    }

    std::string countryNameLocked(const std::string& iso) const {
        char k[2] = {iso.size() > 0 ? iso[0] : '\0', iso.size() > 1 ? iso[1] : '\0'};
        if (const uint8_t* r = vrs().find(fourcc("CTRY"), kCtryRec, k, 2)) return vrs().str(rd32(r + 4));
        return {};
    }

    void typeLocked(const std::string& t, Info& i) const {
        char k[4]; key4(t, k);
        if (const uint8_t* r = mic_.ok() ? mic_.find(fourcc("TYPE"), kTypeRec, k, 4) : nullptr) {
            if (i.desc.empty()) i.desc = mic_.str(rd32(r + 4));
            if (r[8] && r[8] != '-') i.species = std::string(1, char(r[8]));
            if (r[9] && r[9] != '-') i.eng = std::string(1, char(r[9])) + (r[10] && r[10] != '-' ? std::string(1, char(r[10])) : "");
            if (r[11] && r[11] != '-') i.wtc = std::string(1, char(r[11]));
        }
        if (const uint8_t* r = vrs().find(fourcc("MODL"), kModlRec, k, 4)) {
            if (i.desc.empty()) {
                const std::string mf = vrs().str(rd32(r + 4)), md = vrs().str(rd32(r + 8));
                i.desc = mf.empty() ? md : md.empty() ? mf : mf + " " + md;
            }
            if (i.species.empty() && r[14]) i.species = std::string(1, char(r[14]));
            if (i.eng.empty() && r[12]) i.eng = std::string(1, char(r[12])) + (r[13] ? std::string(1, char(r[13])) : "");
            if (i.wtc.empty() && r[15]) i.wtc = std::string(1, char(r[15]));
        }
    }

    std::string iataLocked(const std::string& ia) const {
        char k[4]; key4(ia, k);
        if (const uint8_t* r = vrs().find(fourcc("IATA"), kIataRec, k, 4)) return unkey(r + 4);
        return {};
    }

    bool operatorLocked(const std::string& code, std::string& name) const {
        char k[4]; key4(code, k);
        if (mic_.ok()) if (const uint8_t* r = mic_.find(fourcc("OPER"), kOperRec, k, 4)) { name = mic_.str(rd32(r + 4)); if (!name.empty()) return true; }
        if (const uint8_t* r = vrs().find(fourcc("AIRL"), kAirlRec, k, 4)) { name = vrs().str(rd32(r + 12)); return !name.empty(); }
        return false;
    }

    void routeLocked(const std::string& cs, Info& i) const {
        const Vad& v = vrs();
        const uint64_t key = packCallsign(cs);
        if (!key) return;
        const Vad::Sec rs = v.sec(fourcc("ROUT"));
        size_t lo = 0, hi = rs.count(8);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            uint64_t e; std::memcpy(&e, rs.p + mid * 8, 8);
            const uint64_t k = e >> kSeqBits;
            if (k == key) {
                const uint32_t seq = uint32_t(e & ((1u << kSeqBits) - 1));
                const Vad::Sec sq = v.sec(fourcc("RSEQ"));
                if (sq.n < 4) return;
                const uint32_t ns = rd32(sq.p);
                if (seq >= ns || 4 + (size_t(ns) + 1) * 4 > sq.n) return;
                const uint32_t a = rd32(sq.p + 4 + size_t(seq) * 4), b = rd32(sq.p + 4 + size_t(seq + 1) * 4);
                const uint8_t* idx = sq.p + 4 + (size_t(ns) + 1) * 4;
                if (b < a || idx + size_t(b) * 2 > sq.p + sq.n) return;
                const Vad::Sec ap = v.sec(fourcc("APRT"));
                for (uint32_t j = a; j < b; ++j) {
                    const uint16_t ai = rd16(idx + size_t(j) * 2);
                    if (size_t(ai) >= ap.count(kAptRec)) { i.route.clear(); return; }
                    const uint8_t* r = ap.p + size_t(ai) * kAptRec;
                    Airport x;
                    x.code = unkey(r); x.iata = unkey(r + 4); x.lat = rdf(r + 8); x.lon = rdf(r + 12);
                    x.name = v.str(rd32(r + 16)); x.location = v.str(rd32(r + 20)); x.iso = unkey(r + 24, 2);
                    i.route.push_back(std::move(x));
                }
                return;
            }
            if (k < key) lo = mid + 1; else hi = mid;
        }
    }

    std::mutex m_;
    std::string dir_;
    time_t checked_ = 0;
    Vad starter_, vrsF_, mic_;
    Mapped vrsFile_, micFile_;
};

} // namespace vibeadsbdata
