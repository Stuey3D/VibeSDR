// vibe_log_radio.h — V12 beta92 WHICH RADIO HEARD IT: the radio tag every server-wide log entry carries, so the log views
// can offer a RADIO filter (docs/v12/LOG-RADIO-FILTER.md).
//
// ★★★ WHY (Stuart, 2026-11-22): "we dont separate the digital spots, navtex etc. The only one that should be separate logs
//     per radio on the server should be the DX log" — and "in the logs themselves we could filter by sdr if multiple picked
//     up the same signal". So the logs stay ONE PER SERVER (every radio process feeds the same files); each entry only
//     REMEMBERS which radio(s) heard it, and the filter is a view. Nothing is split, nothing is deleted.
//
// ── What a radio is called here ──────────────────────────────────────────────────────────────────────────────────
//   Its ID — vsconfig::radioId(serial), eight hex characters, the same id the radio's links and the landing list use.
//   ★★ Never the serial (the gallery is public; serials are kept out of public view — vibe_gallery.h stripTokens).
//   ★ "" = this process is not one of a machine's radios (the Android app's own radio, a test, an older build) — its
//     entries carry no tag and every view shows them under every radio ("unknown").
//
// ── Two ways an entry carries it ────────────────────────────────────────────────────────────────────────────────
//   • The GALLERY's index lines (vibe_gallery.h): "ri":"<id>[,<id>…]" — text, at most kMaxIdsPerEntry ids.
//   • The STATION STORE's fixed 64-byte records (vibe_station_store.h): a 32-bit MASK, bit n = the radio on line n of
//     <gallery>/radios.idx — the machine's REGISTRY, append-only, one id a line, at most kMaxRadios lines, under flock,
//     so every radio process gives the same id the same bit. Merged across processes by OR, like the counts by delta.
//
// ── The names ──────────────────────────────────────────────────────────────────────────────────────────────────
//   setMachineRadios(fn): the radios on the machine and their public names as the landing page lists them (main.cpp
//   reads its config). The log APIs answer "radios":[{"id","name","n"}] — only the radios the log actually holds — and
//   the clients draw the filter only when there are two or more.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibe {
namespace logradio {

static constexpr size_t kMaxRadios = 32;        ///< bits in a station record's mask; a machine has a handful
static constexpr size_t kMaxIdsPerEntry = 8;    ///< ids on one gallery line (one item is heard by a handful at most)

/** A radio id as we accept it: 1–16 of [0-9a-z]. Anything else is not one of ours (and never reaches a file or JSON). */
inline bool validId(const std::string& id) {
    if (id.empty() || id.size() > 16) return false;
    for (unsigned char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return false;
    return true;
}

// ── this process's radio ──────────────────────────────────────────────────────────────────────────────────────
struct Self { std::mutex m; std::string id; };
inline Self& self() { static Self s; return s; }
inline void setMine(const std::string& id) { std::lock_guard<std::mutex> lk(self().m); self().id = validId(id) ? id : std::string(); }
inline std::string mine() { std::lock_guard<std::mutex> lk(self().m); return self().id; }

// ── the machine's radios and their names ──────────────────────────────────────────────────────────────────────
struct Named { std::string id, name; };
using RadiosFn = std::function<std::vector<Named>()>;
struct Names { std::mutex m; RadiosFn fn; };
inline Names& names() { static Names n; return n; }
inline void setMachineRadios(RadiosFn fn) { std::lock_guard<std::mutex> lk(names().m); names().fn = std::move(fn); }
inline std::vector<Named> machineRadios() {
    RadiosFn fn;
    { std::lock_guard<std::mutex> lk(names().m); fn = names().fn; }
    std::vector<Named> v = fn ? fn() : std::vector<Named>{};
    v.erase(std::remove_if(v.begin(), v.end(), [](const Named& r) { return !validId(r.id); }), v.end());
    return v;
}

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

/** ★ The "radios" array a log API answers: each radio the log HOLDS (`counts`: id → entries), in the machine's order,
 *  named as the landing page names it. A radio no longer on the machine keeps its id as its name ("a radio removed
 *  since"), so its entries can still be picked out. `unknown`: entries with no tag (older files) — said, never hidden. */
inline std::string radiosJson(const std::vector<std::pair<std::string, uint64_t>>& counts, uint64_t unknown = 0) {
    const std::vector<Named> mach = machineRadios();
    std::vector<std::pair<std::string, uint64_t>> left = counts;
    std::string o = "[";
    bool first = true;
    auto put = [&](const std::string& id, const std::string& name, uint64_t n) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":\"" + jsonEsc(id) + "\",\"name\":\"" + jsonEsc(name) + "\",\"n\":" + std::to_string((unsigned long long)n) + "}";
    };
    for (const auto& r : mach) {
        auto it = std::find_if(left.begin(), left.end(), [&](const std::pair<std::string, uint64_t>& p) { return p.first == r.id; });
        if (it == left.end() || !it->second) continue;
        put(r.id, r.name.empty() ? r.id : r.name, it->second);
        left.erase(it);
    }
    for (const auto& p : left) if (p.second && validId(p.first)) put(p.first, p.first, p.second);
    o += "]";
    if (unknown) o += ",\"radioUnknown\":" + std::to_string((unsigned long long)unknown);
    return o;
}

// ── the "ri" text: "a1b2c3d4,0f0e0d0c" ──────────────────────────────────────────────────────────────────────────
inline std::vector<std::string> splitIds(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    auto take = [&] { if (validId(cur) && v.size() < kMaxIdsPerEntry && std::find(v.begin(), v.end(), cur) == v.end()) v.push_back(cur); cur.clear(); };
    for (char c : s) { if (c == ',') take(); else cur += c; }
    take();
    return v;
}
inline std::string joinIds(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& id : v) { if (!o.empty()) o += ','; o += id; }
    return o;
}
/** `a` ∪ `b`, a's order first, bounded. */
inline std::string unionIds(const std::string& a, const std::string& b) {
    std::vector<std::string> v = splitIds(a);
    for (const auto& id : splitIds(b)) if (v.size() < kMaxIdsPerEntry && std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
    return joinIds(v);
}

// ── THE REGISTRY: <dir>/radios.idx — id → bit, the same on every process of the machine ──────────────────────────
/** The ids on file, line n = bit n (at most kMaxRadios). */
inline std::vector<std::string> readRegistry(const std::string& dir) {
    std::vector<std::string> v;
    if (dir.empty()) return v;
    const int fd = ::open((dir + "/radios.idx").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return v;
    ::flock(fd, LOCK_SH);
    char buf[kMaxRadios * 18];
    const ssize_t n = ::pread(fd, buf, sizeof buf, 0);
    ::flock(fd, LOCK_UN);
    ::close(fd);
    std::string cur;
    for (ssize_t i = 0; i < n && v.size() < kMaxRadios; i++) {
        if (buf[i] == '\n') { v.push_back(validId(cur) ? cur : std::string()); cur.clear(); }   // ★ a bad line keeps its bit
        else if (cur.size() < 17) cur += buf[i];
    }
    return v;
}
/** The bit for `id`, registering it (appended under the file's lock) the first time. -1 = none (no dir, no id, full). */
inline int bitFor(const std::string& dir, const std::string& id) {
    if (dir.empty() || !validId(id)) return -1;
    ::mkdir(dir.c_str(), 0755);
    const int fd = ::open((dir + "/radios.idx").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    ::flock(fd, LOCK_EX);
    char buf[kMaxRadios * 18];
    const ssize_t n = ::pread(fd, buf, sizeof buf, 0);
    int bit = -1, line = 0;
    std::string cur;
    for (ssize_t i = 0; i < n && line < (int)kMaxRadios; i++) {
        if (buf[i] == '\n') { if (cur == id) { bit = line; break; } ++line; cur.clear(); }
        else if (cur.size() < 17) cur += buf[i];
    }
    if (bit < 0 && line < (int)kMaxRadios) {
        const std::string l = id + "\n";
        struct stat st{};
        // ★ a torn last line (no newline) is closed first, so the new id gets a line of its own
        const bool torn = ::fstat(fd, &st) == 0 && st.st_size > 0 && n > 0 && buf[std::min<ssize_t>(n, st.st_size) - 1] != '\n';
        if (torn) { if (::pwrite(fd, "\n", 1, st.st_size) == 1) { ++line; st.st_size += 1; } }
        if (line < (int)kMaxRadios && ::pwrite(fd, l.data(), l.size(), st.st_size) == (ssize_t)l.size()) bit = line;
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);
    return bit;
}
/** The ids a mask names (unknown bits — a registry line lost — are left out). */
inline std::vector<std::string> idsOf(const std::vector<std::string>& registry, uint32_t mask) {
    std::vector<std::string> v;
    for (size_t b = 0; b < registry.size() && b < kMaxRadios; b++) if ((mask >> b) & 1u) if (!registry[b].empty()) v.push_back(registry[b]);
    return v;
}

}  // namespace logradio
}  // namespace vibe
