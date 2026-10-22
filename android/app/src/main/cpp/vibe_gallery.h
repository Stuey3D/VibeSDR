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
//  ★★ The owner sets the size (Setup → Server, beside the free space on the disk). When a write takes the gallery past
//     it, the OLDEST items go first — the newest night's NAVTEX always fits. The index is one JSON line per item and is
//     rewritten (never grown) when items are evicted, and it is capped at kMaxItems lines whatever the size limit says.
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
};

// ── The store ───────────────────────────────────────────────────────────────────────────────────────────────────
class Store {
public:
    static constexpr size_t   kMaxItems    = 5000;              // ★ the index never passes this, whatever the size says
    static constexpr uint64_t kRunMaxBytes = 1024 * 1024;       // ★ one RTTY run file — then a new one
    static constexpr uint64_t kDefaultLimit = 100ull * 1024 * 1024;

    void setDir(const std::string& d) {
        std::lock_guard<std::mutex> lk(m_);
        dir_ = d;
        if (!dir_.empty()) { ::mkdir(dir_.c_str(), 0755); if (::access(dir_.c_str(), W_OK | X_OK) != 0) dir_.clear(); }
    }
    std::string dir() const { std::lock_guard<std::mutex> lk(m_); return dir_; }
    /** 0 = the gallery keeps nothing (saving is off); the owner's figure otherwise. */
    void setLimitBytes(uint64_t b) { std::lock_guard<std::mutex> lk(m_); limit_ = b; }
    uint64_t limitBytes() const { std::lock_guard<std::mutex> lk(m_); return limit_; }
    void setRadioLabel(const std::string& r) { std::lock_guard<std::mutex> lk(m_); radio_ = stripTokens(printable(r, 60), redact_); }
    /** ★★★ The machine's radio SERIALS: never written into an item, and stripped from any item an older build wrote
     *  (the index is rewritten once, here, under the lock). */
    void setRedact(const std::vector<std::string>& serials) {
        std::string dir;
        { std::lock_guard<std::mutex> lk(m_); redact_.clear(); for (auto& t : serials) if (t.size() >= 3) redact_.push_back(t);
          radio_ = stripTokens(radio_, redact_); dir = dir_; }
        if (dir.empty() || serials.empty()) return;
        Lock fl(dir);
        std::vector<std::string> lines = readIndex(dir);
        bool changed = false;
        for (auto& l : lines) { const std::string c = redactLine(l); if (c != l) { l = c; changed = true; } }
        if (!changed) return;
        std::string body; for (auto& l : lines) body += l + "\n";
        const std::string tmp = dir + "/index.jsonl.tmp";
        if (writeFile(tmp, body, false)) ::rename(tmp.c_str(), (dir + "/index.jsonl").c_str());
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
        evictLocked(dir, limit, openNames());
        return true;
    }
    /** The run is finished: it may now be evicted like anything else. */
    void close(const std::string& name) { std::lock_guard<std::mutex> lk(m_); open_.erase(name); }
    /** Bytes in one item, 0 if absent. */
    uint64_t sizeOf(const std::string& name) const {
        if (!safeName(name)) return 0;
        struct stat st{};
        return ::stat((dir() + "/" + name).c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
    }

    /** ★ The list, NEWEST FIRST, for /vibeserver/gallery.json. `kind` filters ("" = all); at most `limit` items. */
    std::string listJson(const std::string& kind, size_t limit) const {
        const std::string dir = this->dir();
        std::string items;
        uint64_t used = 0; size_t total = 0;
        if (!dir.empty()) {
            Lock fl(dir, /*shared=*/true);
            std::vector<std::string> lines = readIndex(dir);
            for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                const std::string f = lineStr(*it, "f");
                if (!safeName(f)) continue;
                struct stat st{};
                if (::stat((dir + "/" + f).c_str(), &st) != 0) continue;
                used += (uint64_t)st.st_size; ++total;
                if (!kind.empty() && lineStr(*it, "k") != kind) continue;
                if (limit == 0) continue;
                --limit;
                std::string l = redactLine(*it);
                if (!l.empty() && l.back() == '}') l.pop_back();
                if (!items.empty()) items += ',';
                items += l + ",\"bytes\":" + std::to_string((unsigned long long)st.st_size) + "}";
            }
        }
        return "{\"items\":[" + items + "],\"count\":" + std::to_string(total) + ",\"usedBytes\":"
             + std::to_string((unsigned long long)used) + ",\"limitBytes\":" + std::to_string((unsigned long long)limitBytes()) + "}";
    }
    /** ★★★ A NAME FROM THE NETWORK → a path, only if it is one of ours. */
    bool resolve(const std::string& name, std::string& path, std::string& ctype) const {
        if (!safeName(name)) return false;
        const std::string dir = this->dir();
        if (dir.empty()) return false;
        bool listed = false;
        { Lock fl(dir, true);
          for (const auto& l : readIndex(dir)) if (lineStr(l, "f") == name) { listed = true; break; } }
        if (!listed) return false;
        path = dir + "/" + name;
        const size_t dot = name.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        ctype = ext == "txt" ? "text/plain; charset=utf-8" : ext == "png" ? "image/png" : ext == "json" ? "application/json"
              : "application/octet-stream";
        return true;
    }
    /** Bytes the gallery holds now (every indexed file that exists). */
    uint64_t usedBytes() const {
        const std::string dir = this->dir();
        if (dir.empty()) return 0;
        Lock fl(dir, true);
        uint64_t n = 0;
        for (const auto& l : readIndex(dir)) {
            struct stat st{};
            const std::string f = lineStr(l, "f");
            if (safeName(f) && ::stat((dir + "/" + f).c_str(), &st) == 0) n += (uint64_t)st.st_size;
        }
        return n;
    }
    /** Space left on the disk the gallery is on — shown beside the size setting. 0 when unknown. */
    static uint64_t freeBytesAt(const std::string& path) {
        struct statvfs v{};
        if (path.empty() || ::statvfs(path.c_str(), &v) != 0) return 0;
        return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
    }

private:
    /** The machine-wide lock: every process on the box that touches this gallery takes it (flock — released by the
     *  kernel if a radio process dies holding it). */
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
    static std::vector<std::string> readIndex(const std::string& dir) {
        std::vector<std::string> out;
        FILE* f = std::fopen((dir + "/index.jsonl").c_str(), "r");
        if (!f) return out;
        char buf[4096];
        std::string cur;
        while (std::fgets(buf, sizeof buf, f)) {
            cur += buf;
            if (!cur.empty() && cur.back() == '\n') { cur.pop_back(); if (!cur.empty() && cur[0] == '{') out.push_back(cur); cur.clear(); }
        }
        if (!cur.empty() && cur[0] == '{') out.push_back(cur);
        std::fclose(f);
        return out;
    }
    static void appendIndex(const std::string& dir, const Meta& m, const std::string& name, const std::string& radio) {
        char hz[32]; std::snprintf(hz, sizeof hz, "%.0f", m.freqHz);
        std::string l = "{\"f\":\"" + jsonEsc(name) + "\",\"k\":\"" + jsonEsc(m.kind) + "\",\"o\":\"" + jsonEsc(m.origin)
            + "\",\"t\":" + std::to_string((long long)(m.utc ? m.utc : nowUtc())) + ",\"hz\":" + hz
            + ",\"m\":\"" + jsonEsc(m.mode) + "\",\"d\":\"" + jsonEsc(m.decoder) + "\",\"p\":\"" + jsonEsc(printable(m.profile, 40))
            + "\",\"r\":\"" + jsonEsc(radio) + "\",\"title\":\"" + jsonEsc(printable(m.title, 120)) + "\""
            + (m.partial ? ",\"partial\":true" : "") + (m.listenerOn && m.listenerOn->load() ? ",\"wl\":true" : "") + "}\n";
        writeFile(dir + "/index.jsonl", l, true) || writeFile(dir + "/index.jsonl", l, false);
    }
    /** ★★ OLDEST OUT until the gallery fits — never one of `keep` (the item just written, an RTTY run still open). The
     *  index is REWRITTEN (tmp + rename) without what went, and lines whose file has vanished are dropped with them. */
    static void evictLocked(const std::string& dir, uint64_t limit, const std::set<std::string>& keep) {
        std::vector<std::string> lines = readIndex(dir);
        std::vector<uint64_t> sizes(lines.size(), 0);
        std::vector<bool> gone(lines.size(), false);
        uint64_t total = 0;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string f = lineStr(lines[i], "f");
            struct stat st{};
            if (!safeName(f) || ::stat((dir + "/" + f).c_str(), &st) != 0) { gone[i] = true; continue; }
            sizes[i] = (uint64_t)st.st_size; total += sizes[i];
        }
        size_t live = 0; for (size_t i = 0; i < lines.size(); i++) if (!gone[i]) ++live;
        bool changed = live != lines.size();
        for (size_t i = 0; i < lines.size() && (total > limit || live > kMaxItems); i++) {
            if (gone[i]) continue;
            const std::string f = lineStr(lines[i], "f");
            if (keep.count(f)) continue;
            ::unlink((dir + "/" + f).c_str());
            gone[i] = true; total -= sizes[i]; --live; changed = true;
        }
        if (!changed) return;
        std::string body;
        for (size_t i = 0; i < lines.size(); i++) if (!gone[i]) body += lines[i] + "\n";
        const std::string tmp = dir + "/index.jsonl.tmp";
        if (writeFile(tmp, body, false)) ::rename(tmp.c_str(), (dir + "/index.jsonl").c_str());
    }

    mutable std::mutex m_;
    std::string dir_, radio_;
    std::vector<std::string> redact_;
    uint64_t limit_ = kDefaultLimit;
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
    /** "Station G · Met warning · #98 · 2 repaired" — navtexTitle(). */
    static std::string title(const NavtexMsg& m) {
        if (m.startLost) return "Start lost";
        const std::string mid = "\xc2\xb7";
        std::string t = std::string("Station ") + (m.id.size() == 4 && m.id[0] >= 'A' && m.id[0] <= 'Z' ? std::string(1, m.id[0]) : "?");
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
