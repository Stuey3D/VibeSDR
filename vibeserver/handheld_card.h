// handheld_card.h — the R36S handheld's card: names, FAT-safe text and the one way anything is written there
// (docs/v12/R36S-HANDHELD-IMAGE.md §0.1 "The card", §0.2 "Files the two halves share").
//
// ★★★ EVERY WRITE IS A TEMP FILE AND A RENAME (Stuart, 2026-11-05: "the safest option"). The card is exFAT with no
//     journal, in a device whose battery can reach the floor mid-write and whose owner can pull the radio — or the
//     card — at any moment. A file written in place and cut short is a file that LOOKS saved and is not; a temp file
//     cut short is a stray ".part" the next start sweeps up, and the name the owner sees only ever points at a whole
//     file. fsync before the rename, so the rename can never land ahead of the data.
// ★★ NAMES A PERSON READS ON A PC: "YYYY-MM-DD HHMM - <kHz to 3 dp> kHz <MODE detail>.ext", in UTC (the radio's clock,
//    not the owner's time zone — a log of what was heard is compared with other people's logs), and FAT-safe: none of
//    : \ / * ? " < > | and no control characters, no trailing dot or space (Windows refuses those).
// ★ Header-only and free of the shim, so test-handheld-card.cpp can drive every rule on the Mac.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace hhcard {

/** ★ FAT-safe text: the forbidden characters become a space-free stand-in, runs of spaces collapse, and a trailing dot or
 *  space is dropped. `cap` bytes at most (a UTF-8 sequence is never cut in half). */
inline std::string fatSafe(const std::string& in, size_t cap = 120) {
    std::string o;
    for (size_t i = 0; i < in.size(); i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F) continue;
        char r = (char)c;
        switch (c) {
            case ':': r = '.'; break;                         // ★ "12:30" reads as "12.30", not "1230"
            case '/': case '\\': case '|': r = '-'; break;
            case '*': case '?': case '"': case '<': case '>': continue;
            default: break;
        }
        if (r == ' ' && (o.empty() || o.back() == ' ')) continue;
        o += r;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    while (!o.empty() && (o.front() == ' ' || o.front() == '.')) o.erase(0, 1);
    if (o.size() > cap) {
        size_t cut = cap;
        while (cut > 0 && ((unsigned char)o[cut] & 0xC0) == 0x80) cut--;   // ★ never half a UTF-8 character
        o.resize(cut);
        while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    }
    return o;
}

/** "2026-11-05 1423" for a UTC time in seconds. */
inline std::string utcStamp(int64_t utcSec) {
    std::time_t t = (std::time_t)utcSec; std::tm g{}; gmtime_r(&t, &g);
    char b[32]; std::strftime(b, sizeof b, "%Y-%m-%d %H%M", &g);
    return b;
}
/** "2026-11-05 142301" — for the files where two in one minute is normal (diagnostics, exports). */
inline std::string utcStampSec(int64_t utcSec) {
    std::time_t t = (std::time_t)utcSec; std::tm g{}; gmtime_r(&t, &g);
    char b[32]; std::strftime(b, sizeof b, "%Y-%m-%d %H%M%S", &g);
    return b;
}

/** "518.000" — kHz to three decimal places (1 Hz), as the spec names it. Empty for no frequency. */
inline std::string khz3(double hz) {
    if (!std::isfinite(hz) || hz <= 0) return "";
    char b[32]; std::snprintf(b, sizeof b, "%.3f", hz / 1000.0);
    return b;
}

/** ★ The card's file name for one recording or decoded item:
 *  "2026-11-05 1423 - 518.000 kHz NAVTEX.txt". No frequency → "2026-11-05 1423 - NAVTEX.txt". */
inline std::string itemName(int64_t utcSec, double hz, const std::string& modeDetail, const std::string& ext) {
    std::string e;
    for (char c : ext) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) e += c;
    if (e.empty()) e = "bin";
    std::string mid;
    const std::string k = khz3(hz);
    if (!k.empty()) mid = k + " kHz";
    const std::string md = fatSafe(modeDetail, 60);
    if (!md.empty()) mid += (mid.empty() ? "" : " ") + md;
    std::string n = utcStamp(utcSec) + (mid.empty() ? std::string() : " - " + mid);
    return fatSafe(n, 150) + "." + e;
}

/** ★ A decoder's folder under Gallery/: upper-case letters, digits and '-' only ("SSTV", "ADS-B", "DAB slides" → "DAB-SLIDES"). */
inline std::string folderName(const std::string& decoder) {
    std::string o;
    for (unsigned char c : decoder) {
        if (c >= 'a' && c <= 'z') o += (char)(c - 32);
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') o += (char)c;
        else if ((c == ' ' || c == '_') && !o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    if (o.size() > 24) o.resize(24);
    return o.empty() ? "OTHER" : o;
}

/** ★ What a filename the NETWORK names may be, here: one path segment of our own making — the FAT-safe set, no
 *  leading dot, no "..", no separator. Every reader checks this before touching the disk. */
inline bool safeSegment(const std::string& n) {
    if (n.empty() || n.size() > 200 || n[0] == '.' || n.find("..") != std::string::npos) return false;
    for (unsigned char c : n) {
        if (c < 0x20 || c == 0x7F) return false;
        switch (c) { case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|': return false; default: break; }
    }
    return n.back() != ' ' && n.back() != '.';
}

/** A serial as a file name: [A-Za-z0-9._-], anything else '_' ("IC-7300 03001234" → "IC-7300_03001234"). */
inline std::string serialFile(const std::string& serial) {
    std::string o;
    for (unsigned char c : serial)
        o += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') ? (char)c : '_';
    if (o.empty() || o[0] == '.') o = "_" + o;
    return o.substr(0, 64);
}

inline bool isDir(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
inline bool exists(const std::string& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0; }

/** mkdir -p, one level at a time. */
inline bool ensureDir(const std::string& p) {
    if (p.empty()) return false;
    if (isDir(p)) return true;
    const size_t sl = p.find_last_of('/');
    if (sl != std::string::npos && sl > 0 && !ensureDir(p.substr(0, sl))) return false;
    return ::mkdir(p.c_str(), 0755) == 0 || isDir(p);
}

/** ★ A name not yet taken in `dir`: "x.wav", then "x (2).wav", "x (3).wav" … */
inline std::string uniqueIn(const std::string& dir, const std::string& name) {
    if (!exists(dir + "/" + name)) return name;
    const size_t dot = name.rfind('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext  = dot == std::string::npos ? "" : name.substr(dot);
    for (int k = 2; k < 1000; k++) {
        const std::string n = stem + " (" + std::to_string(k) + ")" + ext;
        if (!exists(dir + "/" + n)) return n;
    }
    return stem + " (" + std::to_string((long long)::time(nullptr)) + ")" + ext;
}

/** The temp name a write goes through: hidden, and ending ".part" so a sweep can find what a cut-off write left. */
inline std::string tempFor(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    const std::string dir = sl == std::string::npos ? "." : path.substr(0, sl);
    const std::string base = sl == std::string::npos ? path : path.substr(sl + 1);
    return dir + "/." + base + "." + std::to_string((long long)::getpid()) + ".part";
}

/** ★★★ THE ONE WRITE: data to a temp file beside `path`, fsync, rename over `path`, fsync the directory. */
inline bool atomicWrite(const std::string& path, const std::string& data, std::string* err = nullptr) {
    const std::string tmp = tempFor(path);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) { if (err) *err = std::string("cannot create ") + tmp + ": " + std::strerror(errno); return false; }
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) { if (errno == EINTR) continue; if (err) *err = std::string("write: ") + std::strerror(errno); ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += (size_t)n;
    }
    if (::fsync(fd) != 0 && errno != EINVAL) { if (err) *err = std::string("fsync: ") + std::strerror(errno); ::close(fd); ::unlink(tmp.c_str()); return false; }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { if (err) *err = std::string("rename: ") + std::strerror(errno); ::unlink(tmp.c_str()); return false; }
    const size_t sl = path.find_last_of('/');
    const std::string dir = sl == std::string::npos ? "." : (sl == 0 ? "/" : path.substr(0, sl));
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

/** ★ A finished temp file (written by a stream, e.g. an upload) made real the same way. */
inline bool commitTemp(const std::string& tmp, const std::string& path, std::string* err = nullptr) {
    const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { if (err) *err = std::string("rename: ") + std::strerror(errno); ::unlink(tmp.c_str()); return false; }
    const size_t sl = path.find_last_of('/');
    const std::string dir = sl == std::string::npos ? "." : (sl == 0 ? "/" : path.substr(0, sl));
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

/** ★ What a cut-off write left behind: every ".*.part" file under `dir` (one level of sub-folders), removed. */
inline int sweepParts(const std::string& dir) {
    int n = 0;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* e = ::readdir(d)) {
        const std::string nm = e->d_name;
        if (nm == "." || nm == "..") continue;
        const std::string p = dir + "/" + nm;
        if (nm[0] == '.' && nm.size() > 5 && nm.compare(nm.size() - 5, 5, ".part") == 0) { if (::unlink(p.c_str()) == 0) n++; }
        else if (nm[0] != '.' && isDir(p)) {
            DIR* d2 = ::opendir(p.c_str());
            if (!d2) continue;
            while (dirent* e2 = ::readdir(d2)) {
                const std::string n2 = e2->d_name;
                if (n2.size() > 5 && n2[0] == '.' && n2.compare(n2.size() - 5, 5, ".part") == 0
                    && ::unlink((p + "/" + n2).c_str()) == 0) n++;
            }
            ::closedir(d2);
        }
    }
    ::closedir(d);
    return n;
}

struct Space { uint64_t freeBytes = 0, totalBytes = 0; bool ok = false; };
inline Space spaceAt(const std::string& path) {
    Space s; struct statvfs v{};
    if (path.empty() || ::statvfs(path.c_str(), &v) != 0) return s;
    s.freeBytes = (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
    s.totalBytes = (uint64_t)v.f_blocks * (uint64_t)v.f_frsize;
    s.ok = true;
    return s;
}

struct Entry { std::string name; uint64_t bytes = 0; int64_t mtime = 0; };
/** The plain files in `dir` that a person put there or we made — no dot files, no folders — newest first. */
inline std::vector<Entry> listFiles(const std::string& dir) {
    std::vector<Entry> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string nm = e->d_name;
        if (nm.empty() || nm[0] == '.') continue;
        struct stat st{};
        if (::stat((dir + "/" + nm).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back({ nm, (uint64_t)st.st_size, (int64_t)st.st_mtime });
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.name > b.name; });
    return out;
}

/** Read a whole (small) file. False if missing or over `cap`. */
inline bool readFile(const std::string& path, std::string& out, size_t cap = 8u << 20) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > cap) { ::close(fd); return false; }
    out.resize((size_t)st.st_size);
    size_t got = 0;
    while (got < out.size()) { const ssize_t n = ::read(fd, &out[got], out.size() - got); if (n <= 0) break; got += (size_t)n; }
    ::close(fd);
    out.resize(got);
    return true;
}

}  // namespace hhcard
