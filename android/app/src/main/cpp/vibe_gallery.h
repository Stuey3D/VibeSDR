// vibe_gallery.h — THE GALLERY: what the server's decoders heard, kept on disk for anyone to read back (V12, 2026-10-22).
//
// ★★★ WHY (Stuart, 2026-10-22, the night Storm Austen came in): "lets get it up and running so nick can use Navtex
//     overnight". Background decoding (vibe_bgdecode.h) decodes while nobody listens — so its output has to land
//     somewhere a person can read in the morning. And a LISTENER who ran a decoder by hand gets the same: "decodes from a
//     listener who manually ran a decoder are saved to the gallery too" — each item says which it was.
//
// ── What is kept, and when it is written ───────────────────────────────────────────────────────────────────────
//  • NAVTEX: one .txt per MESSAGE, written when it COMPLETES (ZCZC … NNNN) — the same message boundaries the web client
//    and the app draw (src/utils/navtex.ts, ported below as NavtexAsm), so a saved file is what the box showed.
//  • RTTY (continuous): held in RAM and APPENDED every minute or 8 KB, and at once when the decoder stops (a profile
//    switch, a listener arriving, a clean shutdown). One file per UTC day per run, and never more than kRunMaxBytes,
//    so none grows for ever.
//  ★★ NEVER A WRITE PER CHARACTER. A Pi 2's SD card is the target hardware; a decoder printing 6 characters a second
//     must cost a write a minute, not six a second.
//
// ── Bounded ───────────────────────────────────────────────────────────────────────────────────────────────────
//  ★★ The owner sets the size (Setup → Server, beside the free space on the disk) and THAT is the limit: everything is
//     kept until the space is full, then the OLDEST items go first — the newest night's NAVTEX always fits. No item count
//     evicts anything (2026-11-01; the old 5,000-item cap did). The index is append-only and counted inside the space —
//     see "The store" below.
//  ★ Every radio on a machine is its own process (main.cpp), and they share ONE gallery directory — so every change to
//    the index is under an flock() on <dir>/.lock, the same kernel-released lock the decoder slots use.
//
// ── Read-only over HTTP, and path-traversal-proof ─────────────────────────────────────────────────────────────
//  ★★★ A name from the network is only ever looked up: it must be [A-Za-z0-9._-] with no leading dot, AND it must be in
//      the index, AND it is opened O_NOFOLLOW. Three independent refusals; any one of them stops "../config.json".
//  ★ No location beyond what the decoders printed (a NAVTEX message names its own sea area); the server's own position
//    is never written into an item.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "vibe_navtex_stations.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vibe {
namespace gallery {

// ── tiny helpers ────────────────────────────────────────────────────────────────────────────────────────────────
inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
/** Printable ASCII only, capped — for anything off the air that goes into a TITLE or a file NAME. */
inline std::string printable(const std::string& s, size_t cap) {
    // ★ Bytes ≥ 0x80 pass (our titles carry '·'; an owner's radio label may be in any script) — control characters,
    //   which would break a line of the index or a JSON reader, never do.
    std::string o;
    for (unsigned char c : s) { if (o.size() >= cap) break; if (c >= 0x20 && c != 0x7f) o += (char)c; }
    return o;
}
inline std::string utcStamp(int64_t t, const char* fmt) {
    std::time_t tt = (std::time_t)t; std::tm g{}; gmtime_r(&tt, &g);
    char b[48]; std::strftime(b, sizeof b, fmt, &g); return b;
}
inline int64_t nowUtc() { return (int64_t)std::time(nullptr); }
inline int64_t nowUtcMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
/** "518.000 kHz" / "14.0740 MHz" — how the item names its frequency. */
inline std::string freqText(double hz) {
    char b[32];
    if (hz < 30e6) std::snprintf(b, sizeof b, "%.3f kHz", hz / 1e3);
    else std::snprintf(b, sizeof b, "%.4f MHz", hz / 1e6);
    return b;
}
/** The value of "key":"…" or "key":number in ONE index line (our own writer's shape — not a general parser). */
inline std::string lineStr(const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    size_t p = l.find(k); if (p == std::string::npos) return "";
    p += k.size();
    std::string o;
    for (size_t i = p; i < l.size(); i++) {
        if (l[i] == '\\' && i + 1 < l.size()) { o += l[i]; o += l[++i]; continue; }
        if (l[i] == '"') return o;
        o += l[i];
    }
    return "";
}
inline double lineNum(const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t p = l.find(k); if (p == std::string::npos) return 0;
    return std::strtod(l.c_str() + p + k.size(), nullptr);
}

/** ★★★ A RADIO'S SERIAL NEVER REACHES THE GALLERY (Lenovo, 2026-10-22: every item said "SDRplay RSP1A 2235030199" — the
 *  gallery is public, and serials are kept out of public view; the radio URLs were changed to hide them). Each token is
 *  removed wherever it appears, and the spaces it leaves are tidied. */
inline std::string stripTokens(std::string s, const std::vector<std::string>& tokens) {
    for (const auto& t : tokens) {
        if (t.size() < 3) continue;
        for (size_t p; (p = s.find(t)) != std::string::npos;) s.erase(p, t.size());
    }
    std::string o;
    for (char c : s) { if (c == ' ' && (o.empty() || o.back() == ' ')) continue; o += c; }
    while (!o.empty() && (o.back() == ' ' || o.back() == '-' || o.back() == '(')) o.pop_back();
    for (size_t p; (p = o.find(" ()")) != std::string::npos;) o.erase(p, 3);
    return o;
}

// ── What an item is ─────────────────────────────────────────────────────────────────────────────────────────────
struct Meta {
    std::string kind;        ///< "navtex" | "rtty" (phase 2: "wefax", "sstv", "ft8")
    std::string origin;      ///< "background" | "live"
    double      freqHz = 0;  ///< the DIAL the decoder listened through
    std::string mode;        ///< "usb" | "lsb" | …
    std::string decoder;     ///< what ran: "navtex", "rtty", "rtty-auto" …
    std::string profile;     ///< "Profile 1" for background decoding, "" for a listener's
    std::string radio;       ///< this radio's label, for a machine with several
    std::string title;       ///< what the list shows ("Station G · Met warning · #98")
    bool        partial = false;   ///< cut off (a listener arrived, the profile changed) — said, never hidden
    int64_t     utc = 0;     ///< when it began, seconds
    /** ★ Background decoding that ran ON while a listener was here (Stuart, 2026-10-22: "Keep decoding stations still in
     *  range while listeners are on") — read when the item is written, so a message that saw one is marked. */
    std::shared_ptr<std::atomic<bool>> listenerOn;
    /** ★ V12 beta 4: THIS server's position (NaN = none set) — with the dial, what names a NAVTEX station
     *  (vibe_navtex_stations.h). Not written to the index. */
    double      lat = NAN, lon = NAN;
    /** ★ V12 beta 4: stamped (UTC seconds) whenever this meta's decoder writes a new item — the admin page's "last
     *  message 22:53" for a background VFO. */
    std::shared_ptr<std::atomic<int64_t>> lastItemAt;
    /** ★ V12 beta 4: an FT8/FT4 summary's running count — spots and countries so far — for the admin page's live line. */
    std::shared_ptr<std::atomic<uint64_t>> liveSpots, liveCountries;
    /** ★ 2026-10-23: a STANDING card ("last10" — the FT8 summary's rolling last 10 minutes): one per radio, origin,
     *  profile and dial, found again by the next run (Store::adoptSlot). Written to the index as "s". */
    std::string slot;
    /** ★ V12 2026-10-25 (ISM's daily items): the name a DOWNLOAD is saved as — "ISM 433.92 2026-10-25.txt" — written to
     *  the index as "dn". Letters, digits, spaces, '.', '-', '_' and '(' ')' only (downloadSafe), so it cannot break a
     *  Content-Disposition header or name a path. Empty = the item's own file name. */
    std::string dlName;
    /** ★ V12 2026-10-26 (DSC): a position the item is ABOUT — a ship's distress position — for the gallery's map pin
     *  ("plat"/"plon"); NaN = none. And how urgent it is ("lv": 3 distress, 2 urgency, 1 safety; -1 = not said). */
    double pinLat = NAN, pinLon = NAN;
    int level = -1;
};
/** What a download name may hold (Meta::dlName). Anything else is dropped. */
inline std::string downloadSafe(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (o.size() >= 100) break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-'
            || c == '_' || c == '(' || c == ')') o += (char)c;
    }
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    return o;
}

// ── The store ───────────────────────────────────────────────────────────────────────────────────────────────────
// ★★★ THE INDEX IS APPEND-ONLY, AND NEVER HELD WHOLE IN RAM (2026-11-01, the Lenovo: "5000 items on this server · 9.7 MB"
//     whatever the filter — the old 5,000-item cap was throwing NAVTEX history away long before the owner's 100 MB).
//     index.jsonl is one JSON line per change, only ever APPENDED:
//       • an item:        {"f":"…","k":"navtex",…}                  (add / create)
//       • a replacement:  the item's whole line again, ending ,"re":1}  — the item's NEW line where it stands (retag,
//                         replace) — or ,"re":2}  — and it moves to the newest (replace toNewest, the rolling cards)
//       • a removal:      {"x":"<file name>"}                        (eviction)
//     What is live is worked out by reading the file through ONCE, in 64 KB pieces, into a COMPACT TABLE (Cache): 16
//     bytes an item — the line's offset and length, a hash of the name, its kind and its bytes — never the lines. A
//     page is then the table walked newest first and ONE pread() per item shown. A change by any process on the box is
//     an append, so the table reads just the new tail; a rewrite (compaction, the serial redaction) is a new inode, so
//     it reads the file again. And every kRefreshSec it reads it all again anyway, for files grown or deleted behind it.
//     ★ Compaction: when the dead lines (old versions, removals, removed items' lines) pass reserve() — an eighth of
//       the live index — the file is rewritten, streamed line by line under the lock, with only the live lines.
//     ★ Measured (vibeserver/bench-gallery.cpp, Mac, -O2; a Pi 2 is perhaps 10–20× slower): ~217 bytes an item on
//       disk, 16 in RAM. 5k / 50k / 200k items: peak RSS 3.2 / 4.9 / 10.3 MB; the first list (the whole index read)
//       10 / 104 / 385 ms, then a page 0.07 ms at any size; a full gallery's add (evicting one) 0.2 / 0.3 / 0.9 ms.
//       The old store at its 5k cap: 15 MB, and 10.6 ms an add (the index read, every file stat'ed, rewritten).
//  ★★★ THE OWNER'S SIZE IS THE LIMIT (Stuart, 2026-11-01: "we dont want a hard limit of 5000 items, we want the limit
//      to be the assigned space"). The gallery keeps everything until the assigned space is full, then the oldest go
//      first — and the space counts the index file itself, not just the items. kMaxItems (1,000,000) is NOT a limit:
//      it is a backstop against a corrupt or runaway index that normal use never reaches (a 100 MB gallery of ~2 KB
//      NAVTEX texts is ~50,000 items), and hitting it is logged loudly.
//  ★★ FAIR EVICTION: oldest first, but never a kind below its newest kKindFloor items while anything else can go — a
//     night of NAVTEX texts cannot push out the last WEFAX charts. Only when everything left is at its floor does the
//     oldest go regardless (the size limit is the owner's, and it holds).
class Store {
public:
    static constexpr size_t   kMaxItems    = 1000000;           // ★ a BACKSTOP only (logged if hit): the SIZE governs
    static constexpr size_t   kKindFloor   = 50;                // ★ each kind keeps its newest 50 while anything else can go
    static constexpr uint64_t kRunMaxBytes = 1024 * 1024;       // ★ one RTTY run file — then a new one
    static constexpr uint64_t kDefaultLimit = 100ull * 1024 * 1024;
    static constexpr int      kRefreshSec  = 3600;              // ★ the whole index re-read at most this often

    void setDir(const std::string& d) {
        std::lock_guard<std::mutex> lk(m_);
        dir_ = d;
        if (!dir_.empty()) { ::mkdir(dir_.c_str(), 0755); if (::access(dir_.c_str(), W_OK | X_OK) != 0) dir_.clear(); }
    }
    std::string dir() const { std::lock_guard<std::mutex> lk(m_); return dir_; }
    /** 0 = the gallery keeps nothing (saving is off); the owner's figure otherwise. */
    void setLimitBytes(uint64_t b) { std::lock_guard<std::mutex> lk(m_); limit_ = b; }
    uint64_t limitBytes() const { std::lock_guard<std::mutex> lk(m_); return limit_; }
    /** ★ Test seam: the backstop (kMaxItems unless a test lowers it). */
    void setMaxItems(size_t n) { std::lock_guard<std::mutex> lk(m_); maxItems_ = n ? n : kMaxItems; }
    void setRadioLabel(const std::string& r) { std::lock_guard<std::mutex> lk(m_); radio_ = stripTokens(printable(r, 60), redact_); }
    /** ★★★ The machine's radio SERIALS: never written into an item, and stripped from any item an older build wrote
     *  (the index is rewritten once, here, under the lock — streamed line by line, never read whole). */
    void setRedact(const std::vector<std::string>& serials) {
        std::string dir;
        { std::lock_guard<std::mutex> lk(m_); redact_.clear(); for (auto& t : serials) if (t.size() >= 3) redact_.push_back(t);
          radio_ = stripTokens(radio_, redact_); dir = dir_; }
        if (dir.empty() || serials.empty()) return;
        Lock fl(dir);
        const std::string tmp = dir + "/index.jsonl.tmp";
        FILE* out = std::fopen(tmp.c_str(), "w");
        if (!out) return;
        bool changed = false, okw = true;
        eachLine(dir, [&](uint64_t, const std::string& l) {
            const std::string c = redactLine(l);
            if (c != l) changed = true;
            okw = okw && std::fwrite(c.data(), 1, c.size(), out) == c.size() && std::fputc('\n', out) != EOF;
        });
        okw = (std::fclose(out) == 0) && okw;
        if (changed && okw) ::rename(tmp.c_str(), (dir + "/index.jsonl").c_str());
        else ::unlink(tmp.c_str());
    }
    std::string radioLabel() const { std::lock_guard<std::mutex> lk(m_); return radio_; }
    bool enabled() const { std::lock_guard<std::mutex> lk(m_); return !dir_.empty() && limit_ > 0; }

    /** ★★★ THE ONLY NAMES THIS WILL EVER SERVE OR TOUCH. Our writer makes nothing else. */
    static bool safeName(const std::string& n) {
        if (n.empty() || n.size() > 128 || n[0] == '.' || n == "index.jsonl") return false;
        for (unsigned char c : n)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return false;
        return n.find("..") == std::string::npos;
    }

    /** A complete item (a NAVTEX message, later a picture). @return its file name, or "" when not saved. */
    std::string add(const Meta& m, const std::string& ext, const std::string& body) {
        std::string dir; uint64_t limit; std::string radio;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; limit = limit_; radio = radio_; }
        if (dir.empty() || limit == 0) return "";
        Lock fl(dir);
        const std::string name = newName(m, ext);
        if (!writeFile(dir + "/" + name, body, false)) return "";
        appendIndex(dir, m, name, radio);
        evictLocked(dir, limit, std::set<std::string>{ name });
        return name;
    }
    /** An item written in pieces (an RTTY run): created with its first text. */
    std::string create(const Meta& m, const std::string& ext, const std::string& first) {
        std::string dir; uint64_t limit; std::string radio;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; limit = limit_; radio = radio_; }
        if (dir.empty() || limit == 0) return "";
        Lock fl(dir);
        const std::string name = newName(m, ext);
        if (!writeFile(dir + "/" + name, first, false)) return "";
        appendIndex(dir, m, name, radio);
        { std::lock_guard<std::mutex> lk(m_); open_.insert(name); }
        evictLocked(dir, limit, openNames());
        return name;
    }
    /** More of an item made by create(). False when it is gone (evicted, or the owner cleared the folder) — the caller
     *  starts a new one. */
    bool append(const std::string& name, const std::string& text) {
        if (!safeName(name)) return false;
        std::string dir; uint64_t limit;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; limit = limit_; }
        if (dir.empty() || limit == 0) return false;
        Lock fl(dir);
        struct stat st{};
        if (::stat((dir + "/" + name).c_str(), &st) != 0) return false;
        if (!writeFile(dir + "/" + name, text, true)) return false;
        {   // ★ the table's bytes follow the run as it grows (no index line is written for an append)
            std::lock_guard<std::mutex> ck(cacheMtx_);
            if (refresh(dir)) {
                const long i = findLive(dir, name, /*fromNewest=*/true);
                if (i >= 0) { Row& r = cache_.rows[(size_t)i]; const uint64_t nb = (uint64_t)st.st_size + text.size();
                              setBytes(r, nb); }
            }
        }
        evictLocked(dir, limit, openNames());
        return true;
    }
    /** ★★ V12 beta 4: an item REWRITTEN IN PLACE (the FT8 summary's last-10-minutes card: one card, not one per
     *  rewrite). The file is replaced whole (tmp + rename, so a reader never sees half of it) and its new index line is
     *  APPENDED (",\"re\":1" — it takes the item's place in the list). False when it is gone (evicted, the folder
     *  cleared) — the caller starts a new one. */
    /** `toNewest`: the item also MOVES to the newest (",\"re\":2"), so the card lists first (the rolling FT8 card).
     *  ★ The cost of a replace: the file, and ONE index line appended (~300 bytes) — never the index rewritten. */
    bool replace(const std::string& name, const Meta& m, const std::string& body, bool toNewest = false) {
        if (!safeName(name)) return false;
        std::string dir; uint64_t limit; std::string radio;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; limit = limit_; radio = radio_; }
        if (dir.empty() || limit == 0) return false;
        Lock fl(dir);
        bool listed;
        { std::lock_guard<std::mutex> ck(cacheMtx_); listed = refresh(dir) && findLive(dir, name, true) >= 0; }
        struct stat st{};
        if (!listed || ::stat((dir + "/" + name).c_str(), &st) != 0) return false;
        const std::string tmpf = dir + "/." + name + ".tmp";
        if (!writeFile(tmpf, body, false) || ::rename(tmpf.c_str(), (dir + "/" + name).c_str()) != 0) { ::unlink(tmpf.c_str()); return false; }
        appendLine(dir, indexLine(m, name, radio, toNewest ? 2 : 1));
        if (m.lastItemAt) m.lastItemAt->store(nowUtc(), std::memory_order_relaxed);
        evictLocked(dir, limit, openNames());
        return true;
    }
    /** The run is finished: it may now be evicted like anything else. */
    void close(const std::string& name) { std::lock_guard<std::mutex> lk(m_); open_.erase(name); }
    /** ★ 2026-10-23: the standing card for `m.slot` on this radio, origin, profile and dial — the newest if a crash left
     *  more than one — HELD OPEN (never evicted) until close(). "" when there is none (the caller creates it). */
    /** `anyOrigin` (ISM's daily log and summary, 2026-10-25: "one file per UTC day per band"): background decoding and
     *  a listener write into the SAME day's item, so its origin is not part of what finds it. */
    std::string adoptSlot(const Meta& m, bool anyOrigin = false) {
        if (m.slot.empty()) return "";
        std::string dir, radio;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; radio = radio_; }
        if (dir.empty()) return "";
        std::string found;
        {
            Lock fl(dir, /*shared=*/true);
            std::lock_guard<std::mutex> ck(cacheMtx_);
            if (!refresh(dir)) return "";
            char hz[32]; std::snprintf(hz, sizeof hz, "%.0f", m.freqHz);
            const int fd = openIndex(dir);
            std::string l;
            for (size_t i = cache_.rows.size(); i-- > cache_.firstLive && fd >= 0;) {   // ★ newest first: the first match
                const Row& r = cache_.rows[i];
                if ((r.flags & (kDead | kSlot)) != kSlot || !lineAt(fd, r, l)) continue;
                if (lineStr(l, "s") == jsonEsc(m.slot) && lineStr(l, "k") == jsonEsc(m.kind) && (anyOrigin || lineStr(l, "o") == jsonEsc(m.origin))
                    && lineNum(l, "hz") == std::strtod(hz, nullptr) && lineStr(l, "p") == jsonEsc(printable(m.profile, 40))
                    && lineStr(l, "r") == jsonEsc(radio)) { found = lineStr(l, "f"); break; }
            }
            if (fd >= 0) ::close(fd);
            struct stat st{};
            if (!found.empty() && (!safeName(found) || ::stat((dir + "/" + found).c_str(), &st) != 0)) found.clear();
        }
        if (!found.empty()) { std::lock_guard<std::mutex> lk(m_); open_.insert(found); }
        return found;
    }
    /** ★ One field of an item's index line ("o", "title" …), as the index holds it (JSON-escaped). "" if absent. */
    std::string field(const std::string& name, const char* key) const {
        std::string l;
        return liveLine(name, l) ? lineStr(l, key) : "";
    }
    /** ★ An item's index line rewritten from `m` where it stands — its file untouched (ISM's daily log, whose origin
     *  becomes "both" the first time the other side writes into it). False when it is gone. One line appended. */
    bool retag(const std::string& name, const Meta& m) {
        if (!safeName(name)) return false;
        std::string dir, radio;
        { std::lock_guard<std::mutex> lk(m_); dir = dir_; radio = radio_; }
        if (dir.empty()) return false;
        Lock fl(dir);
        { std::lock_guard<std::mutex> ck(cacheMtx_); if (!refresh(dir) || findLive(dir, name, true) < 0) return false; }
        return appendLine(dir, indexLine(m, name, radio, 1));
    }
    /** ★ An item's whole text, for a writer that rebuilds its state from what it wrote (ISM's summary after a restart).
     *  Capped at `cap` bytes. False when it is not ours or not there. */
    bool readItem(const std::string& name, std::string& out, size_t cap = 4u << 20) const {
        std::string path, ctype;
        if (!resolve(name, path, ctype)) return false;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return false;
        out.clear();
        char b[8192];
        for (;;) {
            const ssize_t n = ::read(fd, b, sizeof b);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            out.append(b, (size_t)n);
            if (out.size() >= cap) { out.resize(cap); break; }
        }
        ::close(fd);
        return true;
    }
    /** Bytes in one item, 0 if absent. */
    uint64_t sizeOf(const std::string& name) const {
        if (!safeName(name)) return 0;
        struct stat st{};
        return ::stat((dir() + "/" + name).c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
    }

    /** ★ The list, NEWEST FIRST, for /vibeserver/gallery.json. `kind` filters ("" = all); at most `limit` items.
     *  ★★ V12 beta 4 — PAGES: `offset` ≥ 0 skips that many of the FILTERED list and adds "total" (how many match the
     *  filter) and "offset", so a client draws "Page 2 of 14". offset < 0 is the old request, answered exactly as before
     *  (an older app asks for limit=300 and gets the newest 300).
     *  ★★ 2026-11-01: "kinds" — each kind's count and bytes ({"navtex":{"n":1234,"b":2202009},…}), so the footer can say
     *  "1,234 NAVTEX items · 2.1 MB — 5,000 items on this server · 9.7 MB". From the table, no extra file scan.
     *  ★ From the compact table (Cache): the page costs a pread() and a stat() per item SHOWN, never one per item kept. */
    std::string listJson(const std::string& kind, size_t limit, long offset = -1) const {
        const std::string dir = this->dir();
        std::string items, kinds;
        uint64_t used = 0, indexBytes = 0; size_t total = 0, matched = 0;
        if (!dir.empty()) {
            Lock fl(dir, /*shared=*/true);
            std::lock_guard<std::mutex> ck(cacheMtx_);
            if (refresh(dir)) {
                const Cache& c = cache_;
                indexBytes = c.parsed;
                total = c.live; used = c.used + c.parsed;   // ★ the index counts inside the assigned space
                int want = -2;                                                // -2 = all
                if (!kind.empty()) { want = -1; for (size_t k = 0; k < c.kinds.size(); k++) if (c.kinds[k] == kind) want = (int)k; }
                matched = want == -2 ? c.live : want < 0 ? 0 : c.kindN[(size_t)want];
                for (size_t k = 0; k < c.kinds.size(); k++) {
                    if (!c.kindN[k]) continue;
                    if (!kinds.empty()) kinds += ',';
                    kinds += "\"" + jsonEsc(c.kinds[k]) + "\":{\"n\":" + std::to_string(c.kindN[k]) + ",\"b\":"
                           + std::to_string((unsigned long long)c.kindB[k]) + "}";
                }
                size_t skip = offset > 0 ? (size_t)offset : 0;
                const int fd = (want == -1 || limit == 0) ? -1 : openIndex(dir);
                std::string l;
                for (size_t i = c.rows.size(); fd >= 0 && limit > 0 && i-- > c.firstLive;) {
                    const Row& r = c.rows[i];
                    if ((r.flags & kDead) || (want >= 0 && r.kind != want)) continue;
                    if (skip) { --skip; continue; }
                    if (!lineAt(fd, r, l)) continue;
                    const std::string f = lineStr(l, "f");
                    struct stat st{};
                    if (!safeName(f) || ::stat((dir + "/" + f).c_str(), &st) != 0) continue;   // ★ gone since the table was read
                    --limit;
                    l = redactLine(stripRe(l));
                    if (!l.empty() && l.back() == '}') l.pop_back();
                    if (!items.empty()) items += ',';
                    items += l + ",\"bytes\":" + std::to_string((unsigned long long)st.st_size) + "}";
                }
                if (fd >= 0) ::close(fd);
            }
        }
        std::string j = "{\"items\":[" + items + "],\"count\":" + std::to_string(total) + ",\"usedBytes\":"
             + std::to_string((unsigned long long)used) + ",\"limitBytes\":" + std::to_string((unsigned long long)limitBytes())
             + ",\"indexBytes\":" + std::to_string((unsigned long long)indexBytes) + ",\"kinds\":{" + kinds + "}";
        if (offset >= 0) j += ",\"total\":" + std::to_string(matched) + ",\"offset\":" + std::to_string(offset);
        return j + "}";
    }
    /** ★ Test seam: how many times the index has been read (whole, or its new tail) into the table. */
    unsigned long cacheBuilds() const { return cacheBuilds_.load(); }
    /** ★ Test seam: how many times the whole index has been read. */
    unsigned long fullReads() const { return fullReads_.load(); }
    /** ★ The live index as text, oldest first — each item's CURRENT line (no replacements or removals): what the old
     *  rewritten-in-place index.jsonl held. For tests and diagnostics. */
    std::string indexText() const {
        const std::string dir = this->dir();
        std::string out;
        if (dir.empty()) return out;
        Lock fl(dir, true);
        std::lock_guard<std::mutex> ck(cacheMtx_);
        if (!refresh(dir)) return out;
        const int fd = openIndex(dir);
        std::string l;
        for (size_t i = cache_.firstLive; fd >= 0 && i < cache_.rows.size(); i++)
            if (!(cache_.rows[i].flags & kDead) && lineAt(fd, cache_.rows[i], l)) out += stripRe(l) + "\n";
        if (fd >= 0) ::close(fd);
        return out;
    }
    /** ★★ V12 2026-11-01 (the NAVTEX map): every LIVE item of `kind` ("" = all), oldest first, as its CURRENT line —
     *  replacements and removals already applied (the append-only index read through the table, never raw). Streamed:
     *  one pread an item, under the read lock — so `fn` must NOT call back into the Store (resolve, field …); collect
     *  what needs it and ask after. Returns false when there is no gallery. */
    template <class Fn> bool forEachLive(const std::string& kind, Fn&& fn) const {
        const std::string dir = this->dir();
        if (dir.empty()) return false;
        Lock fl(dir, true);
        std::lock_guard<std::mutex> ck(cacheMtx_);
        if (!refresh(dir)) return false;
        int want = -2;
        if (!kind.empty()) { want = -1; for (size_t k = 0; k < cache_.kinds.size(); k++) if (cache_.kinds[k] == kind) want = (int)k; }
        if (want == -1) return true;
        const int fd = openIndex(dir);
        std::string l;
        for (size_t i = cache_.firstLive; fd >= 0 && i < cache_.rows.size(); i++) {
            const Row& r = cache_.rows[i];
            if ((r.flags & kDead) || (want >= 0 && r.kind != want)) continue;
            if (lineAt(fd, r, l)) fn(stripRe(l));
        }
        if (fd >= 0) ::close(fd);
        return true;
    }
    /** ★★★ A NAME FROM THE NETWORK → a path, only if it is one of ours. */
    bool resolve(const std::string& name, std::string& path, std::string& ctype) const {
        std::string l;
        if (!liveLine(name, l)) return false;
        path = dir() + "/" + name;
        const size_t dot = name.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        ctype = ext == "txt" ? "text/plain; charset=utf-8" : ext == "png" ? "image/png" : ext == "json" ? "application/json"
              : "application/octet-stream";
        return true;
    }
    /** Bytes the gallery holds now: every indexed file that exists, AND the index itself (it is in the space too). */
    uint64_t usedBytes() const {
        const std::string dir = this->dir();
        if (dir.empty()) return 0;
        Lock fl(dir, true);
        std::lock_guard<std::mutex> ck(cacheMtx_);
        return refresh(dir) ? cache_.used + cache_.parsed : 0;
    }
    /** Items the gallery holds now. */
    size_t itemCount() const {
        const std::string dir = this->dir();
        if (dir.empty()) return 0;
        Lock fl(dir, true);
        std::lock_guard<std::mutex> ck(cacheMtx_);
        return refresh(dir) ? cache_.live : 0;
    }
    /** Space left on the disk the gallery is on — shown beside the size setting. 0 when unknown. */
    static uint64_t freeBytesAt(const std::string& path) {
        struct statvfs v{};
        if (path.empty() || ::statvfs(path.c_str(), &v) != 0) return 0;
        return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
    }

private:
    /** The machine-wide lock: every process on the box that touches this gallery takes it (flock — released by the
     *  kernel if a radio process dies holding it). ★ Taken BEFORE cacheMtx_, always. */
    struct Lock {
        int fd = -1;
        Lock(const std::string& dir, bool shared = false) {
            fd = ::open((dir + "/.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0) ::flock(fd, shared ? LOCK_SH : LOCK_EX);
        }
        ~Lock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    };
    std::set<std::string> openNames() const { std::lock_guard<std::mutex> lk(m_); return open_; }
    /** One index line with every serial removed from its radio and title. */
    std::string redactLine(const std::string& l) const {
        std::vector<std::string> red; { std::lock_guard<std::mutex> lk(m_); red = redact_; }
        if (red.empty()) return l;
        bool any = false; for (auto& t : red) if (l.find(t) != std::string::npos) { any = true; break; }
        if (!any) return l;
        std::string o = l;
        for (const char* key : { "r", "title" }) {
            const std::string k = std::string("\"") + key + "\":\"";
            const size_t p = o.find(k); if (p == std::string::npos) continue;
            const size_t a = p + k.size();
            size_t e = a; while (e < o.size() && !(o[e] == '"' && o[e - 1] != '\\')) e++;
            o.replace(a, e - a, stripTokens(o.substr(a, e - a), red));
        }
        return o;
    }
    static std::string kindToken(const std::string& k) {
        std::string o; for (char c : k) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
        return o.empty() ? "item" : o.substr(0, 12);
    }
    std::string newName(const Meta& m, const std::string& ext) {
        // 20261022-231500-navtex-518k0-3f2a.txt — sortable, readable, and nothing a path could hide in.
        char f[24]; std::snprintf(f, sizeof f, "%.1f", m.freqHz / 1e3);
        std::string fk = f; for (auto& c : fk) if (c == '.') c = 'k';
        static thread_local std::mt19937 rng{ std::random_device{}() ^ (unsigned)::getpid() };
        char r[8]; std::snprintf(r, sizeof r, "%04x", (unsigned)(rng() & 0xffff));
        std::string e; for (char c : ext) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) e += c;
        return utcStamp(m.utc ? m.utc : nowUtc(), "%Y%m%d-%H%M%S") + "-" + kindToken(m.kind) + "-" + fk + "-" + r + "." + (e.empty() ? "txt" : e);
    }
    static bool writeFile(const std::string& path, const std::string& body, bool append) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | (append ? O_APPEND : (O_CREAT | O_TRUNC)), 0644);
        if (fd < 0) return false;
        size_t off = 0;
        while (off < body.size()) {
            const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
            if (n < 0) { if (errno == EINTR) continue; ::close(fd); return false; }
            off += (size_t)n;
        }
        ::close(fd);
        return true;
    }
    /** ★ Lines appended to the index (each ends "\n"). If a crash left the last line without its newline, one is put
     *  first, so the half line stays alone (and is skipped) instead of swallowing this one. Caller holds the lock. */
    static bool appendLine(const std::string& dir, std::string l) {
        const std::string p = dir + "/index.jsonl";
        const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            struct stat st{}; char last = '\n';
            if (::fstat(fd, &st) == 0 && st.st_size > 0 && ::pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n') l = "\n" + l;
            ::close(fd);
        }
        return writeFile(p, l, true) || writeFile(p, l, false);
    }
    static void appendIndex(const std::string& dir, const Meta& m, const std::string& name, const std::string& radio) {
        if (m.lastItemAt) m.lastItemAt->store(nowUtc(), std::memory_order_relaxed);
        appendLine(dir, indexLine(m, name, radio));
    }
    static std::string pinText(const Meta& m) {
        if (!std::isfinite(m.pinLat) || !std::isfinite(m.pinLon) || std::fabs(m.pinLat) > 90 || std::fabs(m.pinLon) > 180) return "";
        char b[64]; std::snprintf(b, sizeof b, ",\"plat\":%.5f,\"plon\":%.5f", m.pinLat, m.pinLon);
        return b;
    }
    /** `re`: 0 = a new item; 1 = the item's new line where it stands; 2 = …and it moves to the newest. ★ Always LAST in
     *  the line, so stripRe() can take it off what is served. */
    static std::string indexLine(const Meta& m, const std::string& name, const std::string& radio, int re = 0) {
        char hz[32]; std::snprintf(hz, sizeof hz, "%.0f", m.freqHz);
        std::string l = "{\"f\":\"" + jsonEsc(name) + "\",\"k\":\"" + jsonEsc(m.kind) + "\",\"o\":\"" + jsonEsc(m.origin)
            + "\",\"t\":" + std::to_string((long long)(m.utc ? m.utc : nowUtc())) + ",\"hz\":" + hz
            + ",\"m\":\"" + jsonEsc(m.mode) + "\",\"d\":\"" + jsonEsc(m.decoder) + "\",\"p\":\"" + jsonEsc(printable(m.profile, 40))
            + "\",\"r\":\"" + jsonEsc(radio) + "\",\"title\":\"" + jsonEsc(printable(m.title, 120)) + "\""
            + (m.partial ? ",\"partial\":true" : "") + (m.listenerOn && m.listenerOn->load() ? ",\"wl\":true" : "")
            + (m.slot.empty() ? std::string() : ",\"s\":\"" + jsonEsc(m.slot) + "\"")
            + (m.dlName.empty() ? std::string() : ",\"dn\":\"" + jsonEsc(downloadSafe(m.dlName)) + "\"")
            + pinText(m) + (m.level >= 0 ? ",\"lv\":" + std::to_string(m.level) : std::string())
            + (re ? ",\"re\":" + std::to_string(re) : std::string()) + "}\n";
        return l;
    }
    static std::string stripRe(const std::string& l) {
        if (l.size() > 8 && l.compare(l.size() - 8, 7, ",\"re\":1") == 0) return l.substr(0, l.size() - 8) + "}";
        if (l.size() > 8 && l.compare(l.size() - 8, 7, ",\"re\":2") == 0) return l.substr(0, l.size() - 8) + "}";
        return l;
    }

    // ── the compact table ─────────────────────────────────────────────────────────────────────────────────────────
    enum : uint8_t { kDead = 1, kSlot = 2 };
    /** ★ 16 bytes an item: where its CURRENT line is in index.jsonl, a hash of its name (a match is always confirmed by
     *  reading the line), its kind, its bytes. Never the line itself. */
    struct Row { uint32_t h = 0, off = 0, bytes = 0; uint16_t len = 0; uint8_t kind = 0, flags = 0; };
    struct Cache {
        std::string dir;
        bool valid = false;
        dev_t dev = 0; ino_t ino = 0;
        uint64_t parsed = 0;                   ///< the index is read up to here (the end of its last whole line)
        std::chrono::steady_clock::time_point built;
        std::vector<Row> rows;                 ///< oldest first; dead rows stay until compaction or a re-read
        size_t live = 0, firstLive = 0;
        uint64_t liveIdx = 0;                  ///< bytes of the live items' current lines; parsed − liveIdx = dead lines
        uint64_t used = 0;
        std::vector<std::string> kinds; std::vector<size_t> kindN; std::vector<uint64_t> kindB;
    };
    static uint32_t nameHash(const std::string& s) {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) { h ^= c; h *= 16777619u; }
        return h;
    }
    static int openIndex(const std::string& dir) { return ::open((dir + "/index.jsonl").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); }
    static bool lineAt(int fd, const Row& r, std::string& out) {
        out.resize(r.len);
        size_t got = 0;
        while (got < r.len) {
            const ssize_t n = ::pread(fd, &out[got], r.len - got, (off_t)r.off + (off_t)got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            got += (size_t)n;
        }
        return !out.empty() && out[0] == '{';
    }
    /** ★ Every whole line of the index from `from`, in 64 KB pieces — never the file in RAM. @return the offset after
     *  the last whole line. */
    static uint64_t eachLine(const std::string& dir, const std::function<void(uint64_t, const std::string&)>& fn, uint64_t from = 0) {
        const int fd = openIndex(dir);
        if (fd < 0) return from;
        std::vector<char> buf(65536);
        std::string cur; uint64_t at = from, lineStart = from;
        for (;;) {
            const ssize_t n = ::pread(fd, buf.data(), buf.size(), (off_t)at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            size_t s = 0;
            for (size_t i = 0; i < (size_t)n; i++) {
                if (buf[i] != '\n') continue;
                cur.append(buf.data() + s, i - s);
                fn(lineStart, cur);
                cur.clear(); s = i + 1; lineStart = at + i + 1;
            }
            cur.append(buf.data() + s, (size_t)n - s);
            if (cur.size() > (1u << 20)) cur.clear();   // ★ not a line of ours: never let one grow without bound
            at += (uint64_t)n;
        }
        ::close(fd);
        return lineStart;
    }
    uint8_t kindId(const std::string& k) const {
        Cache& c = cache_;
        for (size_t i = 0; i < c.kinds.size(); i++) if (c.kinds[i] == k) return (uint8_t)i;
        if (c.kinds.size() >= 255) return 254;                       // ★ more kinds than any build has: lumped together
        c.kinds.push_back(k); c.kindN.push_back(0); c.kindB.push_back(0);
        return (uint8_t)(c.kinds.size() - 1);
    }
    void setBytes(Row& r, uint64_t nb) const {
        Cache& c = cache_;
        const uint32_t b = (uint32_t)std::min<uint64_t>(nb, 0xffffffffu);
        if (!(r.flags & kDead)) { c.used = c.used - r.bytes + b; c.kindB[r.kind] = c.kindB[r.kind] - r.bytes + b; }
        r.bytes = b;
    }
    void kill(size_t i) const {
        Cache& c = cache_;
        Row& r = c.rows[i];
        if (r.flags & kDead) return;
        r.flags |= kDead;
        c.live--; c.used -= r.bytes; c.kindN[r.kind]--; c.kindB[r.kind] -= r.bytes; c.liveIdx -= (uint64_t)r.len + 1;
        while (c.firstLive < c.rows.size() && (c.rows[c.firstLive].flags & kDead)) c.firstLive++;
    }
    /** The live row for `name`, or -1. `fromNewest`: replacements are found near the newest, removals near the oldest. */
    long findLive(const std::string& dir, const std::string& name, bool fromNewest) const {
        const Cache& c = cache_;
        const uint32_t h = nameHash(name);
        int fd = -1; std::string l; long hit = -1;
        auto check = [&](size_t i) {
            const Row& r = c.rows[i];
            if ((r.flags & kDead) || r.h != h) return false;
            if (fd < 0) fd = openIndex(dir);
            return fd >= 0 && lineAt(fd, r, l) && lineStr(l, "f") == name;
        };
        if (fromNewest) { for (size_t i = c.rows.size(); i-- > c.firstLive;) if (check(i)) { hit = (long)i; break; } }
        else { for (size_t i = c.firstLive; i < c.rows.size(); i++) if (check(i)) { hit = (long)i; break; } }
        if (fd >= 0) ::close(fd);
        return hit;
    }
    /** One line of the index, at `off`, into the table. */
    void ingest(const std::string& dir, uint64_t off, const std::string& l) const {
        Cache& c = cache_;
        if (l.empty() || l[0] != '{') return;
        // ★ Our writer's shape first ({"f":"…","k":"…" — names are the safe alphabet, so no escapes), the general
        //   lookup only for anything else: the whole-index read is ~1/3 parsing, so it matters at 100,000 lines.
        std::string f, k;
        if (l.compare(0, 6, "{\"f\":\"") == 0) {
            const size_t e = l.find('"', 6);
            if (e != std::string::npos) {
                f = l.substr(6, e - 6);
                if (l.compare(e, 8, "\",\"k\":\"") == 0) { const size_t ke = l.find('"', e + 8); if (ke != std::string::npos) k = l.substr(e + 8, ke - e - 8); }
            }
        } else f = lineStr(l, "f");
        if (f.empty()) {
            const std::string x = l.compare(0, 6, "{\"x\":\"") == 0 ? lineStr(l, "x") : std::string();
            if (!x.empty()) { const long i = findLive(dir, x, false); if (i >= 0) kill((size_t)i); }
            return;
        }
        if (!safeName(f) || l.size() > 0xffff || off > 0xffffffffull) return;
        if (k.empty()) k = lineStr(l, "k");
        const int re = l.size() > 8 && l.compare(l.size() - 8, 6, ",\"re\":") == 0 ? l[l.size() - 2] - '0' : 0;
        const long prev = re ? findLive(dir, f, true) : -1;
        struct stat st{};
        if (::stat((dir + "/" + f).c_str(), &st) != 0) { if (prev >= 0) kill((size_t)prev); return; }
        Row n; n.h = nameHash(f); n.off = (uint32_t)off; n.len = (uint16_t)l.size();
        n.kind = kindId(k); n.flags = l.find(",\"s\":\"") == std::string::npos ? 0 : kSlot;
        n.bytes = (uint32_t)std::min<uint64_t>((uint64_t)st.st_size, 0xffffffffu);
        if (prev >= 0 && re == 1) {                       // ★ the item's new line, where it stands
            Row& r = c.rows[(size_t)prev];
            c.used -= r.bytes; c.kindN[r.kind]--; c.kindB[r.kind] -= r.bytes; c.liveIdx -= (uint64_t)r.len + 1;
            r = n;
            c.used += r.bytes; c.kindN[r.kind]++; c.kindB[r.kind] += r.bytes; c.liveIdx += (uint64_t)r.len + 1;
            return;
        }
        if (prev >= 0) kill((size_t)prev);                // ★ re 2: it moves to the newest
        c.rows.push_back(n);
        c.live++; c.used += n.bytes; c.kindN[n.kind]++; c.kindB[n.kind] += n.bytes; c.liveIdx += (uint64_t)n.len + 1;
    }
    /** ★ The table brought up to date with index.jsonl. Caller holds the directory lock (shared is enough) and
     *  cacheMtx_. A grown file: just its new tail. A new inode, a shorter file, another directory, or kRefreshSec
     *  since the whole was read: the whole again. */
    bool refresh(const std::string& dir) const {
        Cache& c = cache_;
        struct stat st{};
        const bool have = ::stat((dir + "/index.jsonl").c_str(), &st) == 0;
        const auto now = std::chrono::steady_clock::now();
        const bool whole = !c.valid || c.dir != dir || (have && (c.dev != st.st_dev || c.ino != st.st_ino))
            || (!have && c.ino != 0) || (have && (uint64_t)st.st_size < c.parsed) || now - c.built > std::chrono::seconds(kRefreshSec);
        if (whole) {
            c = Cache();
            c.dir = dir; c.valid = true; c.built = now;
            if (have) { c.dev = st.st_dev; c.ino = st.st_ino; c.rows.reserve((size_t)std::min<uint64_t>((uint64_t)st.st_size / 200 + 64, 2000000)); }
            ++fullReads_;
        }
        if (have && (uint64_t)st.st_size > c.parsed) {
            c.parsed = eachLine(dir, [&](uint64_t off, const std::string& l) { ingest(dir, off, l); }, c.parsed);
            ++cacheBuilds_;
        } else if (whole) ++cacheBuilds_;
        // ★ dead rows dropped from RAM once they are a quarter of the table (their lines stay in the file till compaction)
        if (c.rows.size() - c.live > std::max<size_t>(256, c.live / 4)) {
            c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return (r.flags & kDead) != 0; }), c.rows.end());
            c.firstLive = 0;
        }
        return true;
    }
    /** The live line for `name` (a shared lock, the table, ONE pread). */
    bool liveLine(const std::string& name, std::string& l) const {
        if (!safeName(name)) return false;
        const std::string dir = this->dir();
        if (dir.empty()) return false;
        Lock fl(dir, true);
        std::lock_guard<std::mutex> ck(cacheMtx_);
        if (!refresh(dir)) return false;
        const long i = findLive(dir, name, true);
        if (i < 0) return false;
        const int fd = openIndex(dir);
        const bool got = fd >= 0 && lineAt(fd, cache_.rows[(size_t)i], l);
        if (fd >= 0) ::close(fd);
        return got;
    }

    /** ★★ OLDEST OUT until the gallery — its items AND its index — fits the assigned space; never one of `keep` (the item just written, an RTTY run still open), and
     *  never a kind below its newest kKindFloor while anything else can go. Each removal is ONE line appended
     *  ({"x":name}); the index is compacted (streamed) once its dead lines pass reserve() bytes.
     *  ★★ THE INDEX IS INSIDE THE SPACE: the live lines AND a reserve for dead lines (an eighth of the live index) are
     *     counted, and the file is compacted before its dead lines outgrow that reserve — so items + index file never
     *     pass the owner's size, at any item count. */
    void evictLocked(const std::string& dir, uint64_t limit, const std::set<std::string>& keep) {
        size_t maxItems; { std::lock_guard<std::mutex> lk(m_); maxItems = maxItems_; }
        std::lock_guard<std::mutex> ck(cacheMtx_);
        if (!refresh(dir)) return;
        Cache& c = cache_;
        std::set<uint32_t> keepH; for (auto& k : keep) keepH.insert(nameHash(k));
        std::string tomb;
        auto over = [&] { return c.used + c.liveIdx + reserve(limit) > limit || c.live > maxItems; };
        if (c.live > maxItems && !backstopLogged_.exchange(true))
            std::fprintf(stderr, "[gallery] !!!! BACKSTOP HIT: %zu items in %s (backstop %zu) — the index is corrupt or a writer is"
                                 " running away; the oldest are being removed. Normal use never gets here: the size limit governs.\n",
                         c.live, dir.c_str(), maxItems);
        const int fd = openIndex(dir);
        std::string l;
        for (int pass = 0; pass < 2 && fd >= 0 && over(); pass++) {
            for (size_t i = c.firstLive; i < c.rows.size() && over(); i++) {
                const Row& r = c.rows[i];
                if (r.flags & kDead) continue;
                if (pass == 0 && c.kindN[r.kind] <= kKindFloor) continue;   // ★ fair: this kind keeps its newest
                if (!lineAt(fd, r, l)) continue;
                const std::string f = lineStr(l, "f");
                if (keepH.count(r.h) && keep.count(f)) continue;
                if (safeName(f)) ::unlink((dir + "/" + f).c_str());
                tomb += "{\"x\":\"" + jsonEsc(f) + "\"}\n";
                kill(i);
                if (tomb.size() >= 65536) { appendTombs(dir, tomb); tomb.clear(); if (!c.valid) break; }   // ★ RAM bounded
            }
            if (!c.valid) break;
        }
        if (fd >= 0) ::close(fd);
        if (!tomb.empty()) appendTombs(dir, tomb);
        if (c.valid && c.parsed > c.liveIdx + reserve(limit)) compactLocked(dir);
    }
    /** ★ Bytes kept for dead lines in the index (old versions, removals) between compactions: an eighth of the live
     *  index, at least min(64 KB, a sixteenth of the space). Caller holds cacheMtx_. */
    uint64_t reserve(uint64_t limit) const { return std::max<uint64_t>(cache_.liveIdx / 8, std::min<uint64_t>(65536, limit / 16)); }
    /** Removal lines appended. ★ We hold the exclusive lock, so what we appended is exactly the new tail — the table
     *  (which already killed those rows) just moves past it. */
    void appendTombs(const std::string& dir, const std::string& tomb) {
        Cache& c = cache_;
        struct stat before{}, after{};
        const bool had = ::stat((dir + "/index.jsonl").c_str(), &before) == 0;
        appendLine(dir, tomb);
        if (had && (uint64_t)before.st_size == c.parsed && ::stat((dir + "/index.jsonl").c_str(), &after) == 0
            && (uint64_t)after.st_size == c.parsed + tomb.size()) c.parsed = (uint64_t)after.st_size;
        else c.valid = false;
    }
    /** ★ The index rewritten with only its live lines, in list order — streamed (a pread per line, buffered writes),
     *  tmp + rename. Caller holds the EXCLUSIVE lock and cacheMtx_. */
    void compactLocked(const std::string& dir) {
        Cache& c = cache_;
        const std::string tmp = dir + "/index.jsonl.tmp";
        const int fd = openIndex(dir);
        FILE* out = fd >= 0 ? std::fopen(tmp.c_str(), "w") : nullptr;
        if (!out) { if (fd >= 0) ::close(fd); return; }
        std::vector<Row> rows; rows.reserve(c.live);
        uint64_t at = 0; bool okw = true; std::string l;
        for (size_t i = c.firstLive; i < c.rows.size() && okw; i++) {
            Row r = c.rows[i];
            if (r.flags & kDead) continue;
            if (!lineAt(fd, r, l)) { okw = false; break; }
            l = stripRe(l);
            okw = std::fwrite(l.data(), 1, l.size(), out) == l.size() && std::fputc('\n', out) != EOF;
            r.off = (uint32_t)at; r.len = (uint16_t)l.size();
            at += l.size() + 1;
            rows.push_back(r);
        }
        ::close(fd);
        okw = (std::fclose(out) == 0) && okw;
        struct stat st{};
        if (!okw || ::rename(tmp.c_str(), (dir + "/index.jsonl").c_str()) != 0 || ::stat((dir + "/index.jsonl").c_str(), &st) != 0) {
            ::unlink(tmp.c_str()); c.valid = false; return;
        }
        c.rows.swap(rows); c.firstLive = 0; c.parsed = at; c.liveIdx = at; c.dev = st.st_dev; c.ino = st.st_ino;
        ++compactions_;
    }
public:
    /** ★ Test seam: compactions this Store has done. */
    unsigned long compactions() const { return compactions_.load(); }
private:
    mutable std::mutex cacheMtx_;
    mutable Cache cache_;
    mutable std::atomic<unsigned long> cacheBuilds_{0}, fullReads_{0};
    std::atomic<unsigned long> compactions_{0};
    std::atomic<bool> backstopLogged_{false};

    mutable std::mutex m_;
    std::string dir_, radio_;
    std::vector<std::string> redact_;
    uint64_t limit_ = kDefaultLimit;
    size_t maxItems_ = kMaxItems;
    std::set<std::string> open_;
};

/** The one store this process writes to (the shim's background decoding and its listeners' decoders). */
inline Store& shared() { static Store s; return s; }

// ── NAVTEX messages, as the web client and the app cut them ─────────────────────────────────────────────────────
// ★★ A PORT OF src/utils/navtex.ts's NavtexAssembler (2026-10-05) — same matching rules, so the server saves exactly the
//    messages the box shows: ZCZC with ≥ 3 of 4 letters, a 4-character id with ≤ 2 lost; NNNN with ≥ 3 N's and the
//    4th LOST ('_'); no NNNN → closed "[end lost]" by the next header, a lost lock, or 75 s of quiet; text before any
//    header is a "[start lost]" message once it holds 16 letters/digits.
// ★ Bounded: a segment that never meets a boundary is closed at kSegMax (a stuck decoder printing noise all night).
struct NavtexMsg {
    std::string id;           // B1B2B3B4, '_' where lost; empty = start lost
    std::string text;
    bool startLost = false, endLost = false;
    int64_t startedAt = 0;    // UTC seconds
    int lost = 0, chars = 0;
    bool haveFec = false; unsigned long fecClean = 0, fecRepaired = 0, fecLost = 0;
    int gaps = 0; int64_t gapMs = 0;   // ★ fades survived (navtex.ts navtexGapMarker) — shown in the text and the title
    /** ★ V12 beta 4: "B · Oostende, Belgium 🇧🇪" when the station could be named (navtex::label), else empty → "Station B". */
    std::string station;
    /** ★ 2026-11-01: why that station — plain English, this message's evidence (navtex::Resolution::why), in the file. */
    std::vector<std::string> why;
};

class NavtexAsm {
public:
    static constexpr int64_t kEndLostMs = 75000;
    /** ★★ A LOCK LOST FOR LESS THAN THIS IS A BLIP, NOT AN END — navtex.ts LOCK_BLIP_MS, the same rule (2026-10-22: NickB's
     *  clean 490 kHz messages came out as "[end lost]" + "[start lost]" pieces, each relock ~1 s after a resync). */
    static constexpr int64_t kLockBlipMs = 1500;
    static constexpr size_t  kOrphanMin = 16, kSegMax = 16384;
    std::function<void(const NavtexMsg&)> onMessage;

    /** All times are UTC MILLISECONDS (the message's startedAt is stored in seconds). */
    void push(const std::string& chunk, int64_t now) {
        if (chunk.empty()) return;
        if (seg_.empty()) segStartedAt_ = now;
        seg_ += chunk; lastTextAt_ = now;
        scan(now);
        if (seg_.size() > kSegMax) {      // ★ the bound: whatever this is, it is not a message any more
            if (inMsg_ || alnum(seg_) >= kOrphanMin) finish(build(seg_, true));
            seg_.clear(); inMsg_ = false; scanFrom_ = 0; segStartedAt_ = now; cut_ = Cut();
        }
    }
    /** 75 s of nothing: an unfinished message ends "[end lost]". A lock lost and not regained within kLockBlipMs: the
     *  message ends where it was lost. */
    void tick(int64_t now) {
        if (!lastTextAt_ || now - lastTextAt_ < kEndLostMs) return;
        if (cut_.on && !cut_.relocked) { closeAtCut(now); return; }   // ★ silent for good while unlocked: ends at the loss
        if (inMsg_ || alnum(seg_) >= kOrphanMin) finish(build(seg_, true));
        seg_.clear(); inMsg_ = false; scanFrom_ = 0; lastTextAt_ = 0; cut_ = Cut();
    }
    /** The decoder's lock (op 0x03, state 3 = reading). A loss is not an end until it outlasts a blip (see navtex.ts). */
    void lockState(int st, int64_t now) {
        const bool was = locked_;
        locked_ = st >= 3;
        if (was && !locked_) {
            lockLosses_++;
            if (inMsg_ || alnum(seg_) >= kOrphanMin) { if (!cut_.on || cut_.relocked) cut_ = Cut{ true, seg_.size(), now, false }; return; }
            seg_.clear(); inMsg_ = false; scanFrom_ = 0; segStartedAt_ = now;
            return;
        }
        // ★★★ A FADE IS NOT AN END (Lenovo, beta3: 517.5 kHz lost lock 5–15 s at a time): carry on in the same message, the
        //     gap shown where it was ("[… signal lost 12 s …]") and the hunting garbage dropped. Only NNNN, a new ZCZC or
        //     75 s of silence close a message — navtex.ts, the same rule.
        if (!was && locked_ && cut_.on && !cut_.relocked) {
            const int64_t lost = now - cut_.atMs;
            if (lost >= kLockBlipMs) {
                const size_t at = std::min(cut_.at, seg_.size());
                char m[64]; std::snprintf(m, sizeof m, "\n[\xe2\x80\xa6 signal lost %lld s \xe2\x80\xa6]\n", (long long)std::max<int64_t>(1, (lost + 500) / 1000));
                seg_ = seg_.substr(0, at) + m;
                gaps_++; gapMs_ += lost;
                scanFrom_ = std::min(scanFrom_, at);
            }
            cut_.relocked = true;
        }
    }
    /** The decoder stopped: what was arriving is saved as far as it got (end lost). */
    void flush() {
        if (inMsg_ || alnum(seg_) >= kOrphanMin) finish(build(seg_, true));
        seg_.clear(); inMsg_ = false; scanFrom_ = 0; cut_ = Cut();
    }
    /** A retune: the text from here is another transmission — drop what was arriving (as the client's clearLive). */
    void clearLive() { seg_.clear(); inMsg_ = false; scanFrom_ = 0; lastTextAt_ = 0; locked_ = false; cut_ = Cut(); gaps_ = 0; gapMs_ = 0; }
    /** Lock losses seen (blips included) — for the log. */
    unsigned long lockLosses() const { return lockLosses_; }

    static std::string subject(char b2) {
        switch (b2) {
            case 'A': return "Nav warning"; case 'B': return "Met warning"; case 'C': return "Ice report";
            case 'D': return "SAR / piracy"; case 'E': return "Weather forecast"; case 'F': return "Pilot / VTS";
            case 'G': return "AIS"; case 'H': return "LORAN"; case 'J': return "SATNAV"; case 'K': return "Other navaid";
            case 'L': return "Nav warning (more)"; case 'T': return "Test"; case 'Z': return "No messages on hand";
            case 'V': case 'W': case 'X': case 'Y': return "Special service";
            default: return b2 >= 'A' && b2 <= 'Z' ? std::string("Subject ") + b2 : "Subject ?";
        }
    }
    /** "Station G · Met warning · #98 · 2 repaired" — navtexTitle(); "B · Oostende, Belgium 🇧🇪 · …" once m.station is set. */
    static std::string title(const NavtexMsg& m) {
        const std::string mid = "\xc2\xb7";
        // ★ 2026-11-01: a start-lost message its own text names (navtex::startLostLabel) — "Start lost · N · Suspected Piombino…"
        if (m.startLost) return m.station.empty() ? std::string("Start lost") : "Start lost " + mid + " " + m.station;
        std::string t = !m.station.empty() ? m.station
                      : std::string("Station ") + (m.id.size() == 4 && m.id[0] >= 'A' && m.id[0] <= 'Z' ? std::string(1, m.id[0]) : "?");
        t += " " + mid + " " + subject(m.id.size() == 4 ? m.id[1] : '?');
        const bool serial = m.id.size() == 4 && std::isdigit((unsigned char)m.id[2]) && std::isdigit((unsigned char)m.id[3]);
        t += " " + mid + " #" + (serial ? m.id.substr(2) : "??");
        if (m.gaps) t += " " + mid + " gaps: " + std::to_string(m.gaps) + " (" + std::to_string((long long)((m.gapMs + 500) / 1000)) + " s)";
        if (m.haveFec) {
            if (m.fecRepaired) t += " " + mid + " " + std::to_string(m.fecRepaired) + " repaired";
            if (m.fecLost) t += " " + mid + " " + std::to_string(m.fecLost) + " lost";
            if (!m.fecRepaired && !m.fecLost) t += " " + mid + " no errors";
        }
        return t;
    }
    /** navtexBody() + navtexFileText()'s head — the file a listener's SAVE would have written. */
    static std::string fileText(const NavtexMsg& m, const std::string& where) {
        std::string head = "NAVTEX \xc2\xb7 " + title(m) + "\nReceived " + utcStamp(m.startedAt, "%Y-%m-%d %H:%M UTC");
        if (!m.haveFec && m.chars > 0) head += " \xc2\xb7 " + std::to_string((int)std::lround(100.0 * m.lost / m.chars)) + "% of characters lost";
        if (!where.empty()) head += "\n" + where;
        if (!m.why.empty()) { head += "\nWhy: "; for (size_t i = 0; i < m.why.size(); i++) head += (i ? "; " : "") + m.why[i]; }
        std::string body = m.startLost ? "[start lost]" : "ZCZC " + (m.id.empty() ? std::string("????") : m.id);
        if (!m.text.empty()) body += "\n" + m.text;
        body += m.endLost ? "\n[end lost]" : "\nNNNN";
        return head + "\n\n" + body + "\n";
    }

private:
    static bool upperOrLost(char c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool digitOrLost(char c) { return (c >= '0' && c <= '9') || c == '_'; }
    static bool spaceCh(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    static size_t alnum(const std::string& s) { size_t n = 0; for (char c : s) if (std::isalnum((unsigned char)c)) ++n; return n; }
    static bool zczcAt(const std::string& s, size_t i) {
        int hit = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i + k];
            if (c == "ZCZC"[k]) hit++;
            else if (!(c == '_' || (c >= 'A' && c <= 'Z'))) return false;
        }
        return hit >= 3;
    }
    /** 1 = header (at..end, id), 0 = none, -1 = wait (the stream ends inside it). */
    static int headerAt(const std::string& s, size_t i, size_t& end, std::string& id) {
        if (i + 4 > s.size() || !zczcAt(s, i)) return 0;
        size_t j = i + 4; int sp = 0;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t') && sp < 3) { j++; sp++; }
        if (j + 4 > s.size()) {
            for (size_t k = j; k < s.size(); k++) if (!(k - j < 2 ? upperOrLost(s[k]) : digitOrLost(s[k]))) return 0;
            return -1;
        }
        id = s.substr(j, 4);
        if (!upperOrLost(id[0]) || !upperOrLost(id[1]) || !digitOrLost(id[2]) || !digitOrLost(id[3])) return 0;
        int lostInId = 0; for (char c : id) if (c == '_') lostInId++;
        if (lostInId > 2) return 0;
        if (s.compare(i, 4, "ZCZC") != 0) {
            if (j + 4 >= s.size()) return -1;
            const char after = s[j + 4];
            if (!spaceCh(after) && after != '_') return 0;
        }
        end = j + 4;
        return 1;
    }
    static bool nnnnAt(const std::string& s, size_t i) {
        if (i + 4 > s.size()) return false;
        int n = 0;
        for (int k = 0; k < 4; k++) { const char c = s[i + k]; if (c == 'N') n++; else if (c != '_') return false; }
        return n >= 3;
    }
    static std::string cleanText(const std::string& raw) {
        std::string o;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\r') { while (i + 1 < raw.size() && raw[i + 1] == '\r') i++; if (i + 1 < raw.size() && raw[i + 1] == '\n') i++; o += '\n'; }
            else o += raw[i];
        }
        size_t a = 0;
        // leading blank lines (spaces/tabs then a newline) trimmed
        for (;;) {
            size_t k = a; while (k < o.size() && (o[k] == ' ' || o[k] == '\t')) k++;
            if (k < o.size() && o[k] == '\n') a = k + 1; else break;
        }
        size_t b = o.size(); while (b > a && std::isspace((unsigned char)o[b - 1])) b--;
        return o.substr(a, b - a);
    }
    NavtexMsg build(const std::string& body, bool endLost) {
        NavtexMsg m;
        m.text = cleanText(body);
        for (char c : m.text) { if (c == '_') { m.lost++; m.chars++; } else if (std::isalnum((unsigned char)c)) m.chars++; }
        m.endLost = endLost;
        m.gaps = gaps_; m.gapMs = gapMs_;
        if (inMsg_) { m.id = hdrId_; m.startedAt = hdrAt_ / 1000; }
        else { m.startLost = true; m.startedAt = segStartedAt_ / 1000; }
        return m;
    }
    void closeAtCut(int64_t now) {
        const std::string body = seg_.substr(0, std::min(cut_.at, seg_.size()));
        cut_ = Cut();
        if (inMsg_ || alnum(body) >= kOrphanMin) finish(build(body, true));
        seg_.clear(); inMsg_ = false; scanFrom_ = 0; segStartedAt_ = now;
    }
    void finish(const NavtexMsg& m) { gaps_ = 0; gapMs_ = 0; if (onMessage) onMessage(m); }
    void scan(int64_t now) {
        for (;;) {
            int kind = 0; size_t at = 0, end = 0; std::string id; long waitAt = -1;
            for (size_t i = scanFrom_; i + 4 <= seg_.size(); i++) {
                size_t e = 0; std::string hid;
                const int h = headerAt(seg_, i, e, hid);
                if (h < 0) { waitAt = (long)i; break; }
                if (h > 0) { kind = 1; at = i; end = e; id = hid; break; }
                if (nnnnAt(seg_, i)) { kind = 2; at = i; end = i + 4; break; }
            }
            if (!kind) { scanFrom_ = waitAt >= 0 ? (size_t)waitAt : (seg_.size() > 3 ? seg_.size() - 3 : 0); return; }
            // ★ A lock lost in this segment: a new HEADER = a takeover, the message ends at the cut; NNNN = it was a blip.
            const size_t upto = (cut_.on && kind == 1) ? std::min(cut_.at, at) : at;
            cut_ = Cut();
            const std::string before = seg_.substr(0, upto);
            if (kind == 2) {
                if (inMsg_ || alnum(before) >= kOrphanMin) finish(build(before, false));
                inMsg_ = false;
            } else {
                if (inMsg_ || alnum(before) >= kOrphanMin) finish(build(before, true));
                inMsg_ = true; hdrId_ = id; hdrAt_ = now; gaps_ = 0; gapMs_ = 0;
            }
            seg_.erase(0, end);
            segStartedAt_ = now; scanFrom_ = 0;
        }
    }
    std::string seg_;
    bool inMsg_ = false, locked_ = false;
    std::string hdrId_;
    int64_t hdrAt_ = 0, segStartedAt_ = 0, lastTextAt_ = 0;
    size_t scanFrom_ = 0;
    struct Cut { bool on = false; size_t at = 0; int64_t atMs = 0; bool relocked = false; } cut_;
    unsigned long lockLosses_ = 0;
    int gaps_ = 0; int64_t gapMs_ = 0;
};

} // namespace gallery
} // namespace vibe
