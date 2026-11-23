// vibe_gallery_shared.h — V12 beta92 ONE DAY ITEM PER SERVER, WHICHEVER RADIO HEARD IT (docs/v12/LOG-RADIO-FILTER.md
// § Shared day items). The tools the AIS / APRS, DSC and NDB day items use to be written by every radio process of the
// machine into the SAME item, as FUNcube's day log already is (vibe_fctlm_gallery.h FcLock).
//
// ★★★ WHY (Stuart, 2026-11-22): "we dont separate the digital spots, navtex etc. The only one that should be separate logs
//     per radio on the server should be the DX log" — "all radios on same server should feed the same log". Each radio is
//     its own process (vibeserver@<serial>); before this, these day items were found again by Store::adoptSlot WITH the
//     radio's label, profile and dial in the match, so two radios on one channel wrote two logs of the same day.
//
// ── The three tools ─────────────────────────────────────────────────────────────────────────────────────────────
//  • Lock — an exclusive flock on <gallery>/.shared.lock (the kernel releases it if a process dies). EVERY find-or-create,
//    dedupe, append, merge and rewrite of a shared item is inside it, so two processes never make two items or interleave.
//    Taken BEFORE the gallery's own lock (Store takes <dir>/.lock inside each call) — always in that order.
//  • adopt(): the item by its SLOT alone (Store::adoptSlot anyWriter — never the radio, profile, origin or dial).
//  • dedupeLines(): a day log's lines are "HH:MM:SS  <text>". A line is the SAME RECEPTION as one already in the file
//    (another radio heard the same transmission) when its text is identical and its time is within `windowSec` — then it
//    is dropped. A repeat later than that is a new reception and is kept (an AIS ship's unchanged line an hour on).
//    Only the file's TAIL is read for it (kTailBytes): a duplicate is always recent, since every radio flushes at least
//    once a minute — never the whole day's file once a minute on a Pi 2's SD card.
//  • State (for a summary card): <gallery>/.shared/<slot>.state — the card's ROWS as text, one per station / device,
//    merged under the Lock by every radio and the card drawn from it, so the card is the whole server's. Bounded by the
//    caller's row cap; the day's files are removed two days on (pruneStates). Counted in the gallery's space (sideBytes).
// ★ After any append by this process the item's index line is retagged, so its "ri" lists this radio too
//   (vibe_log_radio.h) — an append alone writes no index line.
#pragma once
#include "vibe_gallery.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibe {
namespace gallery {
namespace sharedday {

static constexpr size_t kTailBytes = 256 * 1024;   ///< the part of a day log read for the dedupe
static constexpr int kDedupeSec = 20;              ///< the same text within this = the same reception

struct Lock {
    int fd = -1;
    explicit Lock(const std::string& dir) {
        if (dir.empty()) return;
        fd = ::open((dir + "/.shared.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) while (::flock(fd, LOCK_EX) != 0 && errno == EINTR) {}
    }
    ~Lock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    Lock(const Lock&) = delete; Lock& operator=(const Lock&) = delete;
};

/** The server's item for `m.slot` (kind + slot only). "" = none yet. Caller holds the Lock. */
inline std::string adopt(Store& st, const Meta& m) { return st.adoptSlot(m, /*anyOrigin=*/true, /*anyWriter=*/true); }

/** The last `bytes` of an item's text, from a line boundary. False when it is not there. */
inline bool readTail(const Store& st, const std::string& name, std::string& out, size_t bytes = kTailBytes) {
    std::string path, ctype;
    if (!st.resolve(name, path, ctype)) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat sb{};
    if (::fstat(fd, &sb) != 0) { ::close(fd); return false; }
    const off_t size = sb.st_size, from = size > (off_t)bytes ? size - (off_t)bytes : 0;
    out.assign((size_t)(size - from), '\0');
    size_t got = 0;
    while (got < out.size()) {
        const ssize_t n = ::pread(fd, &out[got], out.size() - got, from + (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    ::close(fd);
    out.resize(got);
    if (from > 0) { const size_t nl = out.find('\n'); out = nl == std::string::npos ? std::string() : out.substr(nl + 1); }
    return true;
}

/** "HH:MM:SS" at the start of a line → seconds of the day; -1 when the line is not one. */
inline int lineSecs(const std::string& l) {
    if (l.size() < 10 || l[2] != ':' || l[5] != ':') return -1;
    for (int i : { 0, 1, 3, 4, 6, 7 }) if (l[i] < '0' || l[i] > '9') return -1;
    return ((l[0] - '0') * 10 + (l[1] - '0')) * 3600 + ((l[3] - '0') * 10 + (l[4] - '0')) * 60 + (l[6] - '0') * 10 + (l[7] - '0');
}
/** The text after the time ("HH:MM:SS  "). */
inline std::string lineText(const std::string& l) { size_t p = 8; while (p < l.size() && l[p] == ' ') ++p; return l.substr(p); }

/** Which of `lines` are NEW against `tail` (and earlier entries of `lines`): the same text within `windowSec` = the
 *  same reception, false. Lines with no time are always new. */
inline std::vector<bool> newLines(const std::string& tail, const std::vector<std::string>& lines, int windowSec = kDedupeSec) {
    std::unordered_map<std::string, std::vector<int>> seen;
    size_t p = 0;
    while (p < tail.size()) {
        size_t e = tail.find('\n', p); if (e == std::string::npos) e = tail.size();
        const std::string l = tail.substr(p, e - p); p = e + 1;
        const int t = lineSecs(l); if (t >= 0) seen[lineText(l)].push_back(t);
    }
    std::vector<bool> keep(lines.size(), true);
    for (size_t i = 0; i < lines.size(); i++) {
        const int t = lineSecs(lines[i]);
        if (t < 0) continue;
        auto& v = seen[lineText(lines[i])];
        for (int u : v) if (std::abs(u - t) <= windowSec) { keep[i] = false; break; }
        if (keep[i]) v.push_back(t);
    }
    return keep;
}
/** `fresh` without the lines `tail` (or an earlier line of `fresh`) already has — the same text within `windowSec`.
 *  Lines with no time are always kept. `dropped`: how many went. */
inline std::string dedupeLines(const std::string& tail, const std::string& fresh, int windowSec = kDedupeSec, size_t* dropped = nullptr) {
    std::unordered_map<std::string, std::vector<int>> seen;
    auto each = [](const std::string& s, auto fn) {
        size_t p = 0;
        while (p < s.size()) { size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size(); fn(s.substr(p, e - p)); p = e + 1; }
    };
    each(tail, [&](const std::string& l) { const int t = lineSecs(l); if (t >= 0) seen[lineText(l)].push_back(t); });
    std::string out;
    size_t gone = 0;
    each(fresh, [&](const std::string& l) {
        if (l.empty()) return;
        const int t = lineSecs(l);
        if (t >= 0) {
            auto& v = seen[lineText(l)];
            for (int u : v) if (std::abs(u - t) <= windowSec) { ++gone; return; }   // ★ another radio heard this one
            v.push_back(t);
        }
        out += l; out += '\n';
    });
    if (dropped) *dropped = gone;
    return out;
}

/** Append `lines` (whole lines) to the shared item `name`, deduped against its tail, and retag it with `m` (this radio
 *  joins its "ri"). @return false when the item is gone (the caller creates it). `kept`: the lines written. Caller holds
 *  the Lock. */
inline bool appendDeduped(Store& st, const std::string& name, const Meta& m, const std::string& lines, std::string* kept = nullptr,
                          int windowSec = kDedupeSec) {
    std::string tail;
    if (name.empty() || !readTail(st, name, tail)) return false;
    const std::string add = dedupeLines(tail, lines, windowSec);
    if (!add.empty() && !st.append(name, add)) return false;
    if (kept) *kept = add;
    st.retag(name, m);
    return true;
}

// ── State: a summary card's rows, shared by every radio process ─────────────────────────────────────────────────
inline std::string stateDir(const Store& st) { const std::string d = st.dir(); return d.empty() ? d : d + "/.shared"; }
inline std::string statePath(const Store& st, const std::string& slot) {
    std::string s;
    for (char c : slot) s += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') ? c : '_';
    const std::string d = stateDir(st);
    return d.empty() ? d : d + "/" + s.substr(0, 96) + ".state";
}
/** The state's text ("" when none). Caller holds the Lock. Capped at `cap` bytes. */
inline std::string readState(const Store& st, const std::string& slot, size_t cap = 4u << 20) {
    const std::string p = statePath(st, slot);
    std::string o;
    if (p.empty()) return o;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return o;
    char b[8192];
    for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0 && o.size() < cap;) o.append(b, n);
    std::fclose(f);
    return o;
}
/** Write the state (tmp + rename). Caller holds the Lock. */
inline bool writeState(const Store& st, const std::string& slot, const std::string& text) {
    const std::string p = statePath(st, slot);
    if (p.empty()) return false;
    ::mkdir(stateDir(st).c_str(), 0755);
    const std::string tmp = p + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) { ::unlink(tmp.c_str()); return false; }
    return ::rename(tmp.c_str(), p.c_str()) == 0;
}
/** States not written for `keepSec` go (a finished day's). At most once an hour per process is plenty. */
inline void pruneStates(const Store& st, int64_t now, int64_t keepSec = 2 * 86400) {
    const std::string d = stateDir(st);
    if (d.empty()) return;
    DIR* dp = ::opendir(d.c_str());
    if (!dp) return;
    while (dirent* e = ::readdir(dp)) {
        const std::string n = e->d_name;
        if (n.size() < 7 || n.compare(n.size() - 6, 6, ".state") != 0) continue;
        struct stat sb{};
        const std::string p = d + "/" + n;
        if (::stat(p.c_str(), &sb) == 0 && S_ISREG(sb.st_mode) && now - (int64_t)sb.st_mtime > keepSec) ::unlink(p.c_str());
    }
    ::closedir(dp);
}

/** A field-separated row: tabs and newlines in a value become spaces, so a row is always one line of N fields. */
inline std::string field(const std::string& s) { std::string o = s; for (char& c : o) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return o; }
inline std::vector<std::string> splitTabs(const std::string& l) {
    std::vector<std::string> f; size_t p = 0;
    for (;;) { const size_t t = l.find('\t', p); if (t == std::string::npos) { f.push_back(l.substr(p)); break; } f.push_back(l.substr(p, t - p)); p = t + 1; }
    return f;
}

}  // namespace sharedday
}  // namespace gallery
}  // namespace vibe
