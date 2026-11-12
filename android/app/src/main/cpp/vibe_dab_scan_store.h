// vibe_dab_scan_store.h — THE LAST DAB BAND SCAN, ON DISK (V12, 2026-11-12).
//
// ★★★ WHY. The band scan's list — what MULTIPLEXES shows, what dab_scan_get answers — lived in the radio process's memory
//     only (g_dabBand in local_sdr_shim.cpp). Stuart, 2026-11-12: "after a DAB scan on the R36S if I power off and on
//     again the DAB multiplexes are forgotten and the scan button is present again". The handheld powers off; a server
//     restarts on every settings save, an update, a reboot. Every one of them forgot the scan. The bookmarks the scan
//     made, the ensemble names and the per-block gains were already on disk; the list itself was not.
// ★★ PER RADIO. The caller names the file (one per radio serial on a multi-radio box, one per serial on the handheld);
//    this header only reads and writes it, so one radio's scan cannot land in another's list.
// ★★ BOUNDED. 41 blocks at most (Band III), 64 services per ensemble, 64-byte labels — the file is a few KB, and a
//    file past kMaxFileBytes is DAMAGE and is not read. Blocks are stored by NAME ("11D"), never by index, so a table
//    change can never shift a list onto the wrong blocks.
// ★★ ATOMIC. temp file + fsync + rename (Stuart: "the safest option"), then the directory is synced, so a power cut
//    leaves the old list or the new one, never half of either.
// ★ A MEMORY, NOT A READING. The live ensemble outranks it, and a rescan replaces it whole — exactly as in memory.
//
// Format (text, one record per line, '\n' and '\r' stripped from every label on the way IN):
//   VIBE-DAB-SCAN 1
//   at <unix seconds> secs <whole seconds> cancelled <0|1> by <word>
//   why <text>                                   (optional)
//   H <block name> <complete 0|1> <fib> <mer> <ensemble label>
//   S <sid> <label>                              (the services of the H line above it)
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

#include "vibe_dab_channels.h"

namespace vibedabscan {

/** One ensemble the band scan found — the shim's DabBandScanHit IS this type. */
struct Hit {
    int idx = -1;                 // index into vibedab::kBandIII
    std::string ensemble;
    std::vector<std::pair<uint32_t, std::string>> services;   // playable audio, labelled
    float fibRate = 0.0f, merDb = 0.0f;
    bool complete = false;
};

/** What is kept of a finished scan. */
struct Saved {
    int64_t at = 0;               // when it ended (unix seconds)
    int secs = 0;                 // how long it took
    bool cancelled = false;
    std::string by;               // "setup", "listener", "admin"
    std::string why;
    std::vector<Hit> hits;        // in block order
};

constexpr size_t kMaxServices  = 64;
constexpr size_t kMaxLabel     = 64;
constexpr size_t kMaxFileBytes = 256 * 1024;

inline std::string clean(const std::string& s, size_t cap = kMaxLabel) {
    std::string o;
    for (char c : s) { if (c == '\n' || c == '\r' || c == '\0') continue; o += c; if (o.size() >= cap) break; }
    return o;
}

inline int blockIndex(const std::string& name) {
    for (size_t i = 0; i < vibedab::kBandIIICount; ++i) if (name == vibedab::kBandIII[i].name) return int(i);
    return -1;
}

inline std::string serialise(const Saved& s) {
    std::string o = "VIBE-DAB-SCAN 1\n";
    std::string by;
    for (char c : s.by) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) by += c;
    if (by.empty()) by = "scan";
    o += "at " + std::to_string((long long)s.at) + " secs " + std::to_string(s.secs) + " cancelled " + (s.cancelled ? "1" : "0")
       + " by " + by + "\n";
    if (!s.why.empty()) o += "why " + clean(s.why, 200) + "\n";
    size_t n = 0;
    for (const auto& h : s.hits) {
        if (h.idx < 0 || (size_t)h.idx >= vibedab::kBandIIICount) continue;
        if (++n > vibedab::kBandIIICount) break;
        char q[64];
        std::snprintf(q, sizeof q, " %d %.2f %.1f ", h.complete ? 1 : 0, (double)h.fibRate, (double)h.merDb);
        o += std::string("H ") + vibedab::kBandIII[h.idx].name + q + clean(h.ensemble) + "\n";
        size_t k = 0;
        for (const auto& sv : h.services) {
            if (++k > kMaxServices) break;
            o += "S " + std::to_string(sv.first) + " " + clean(sv.second) + "\n";
        }
    }
    return o;
}

/** @return false on anything that is not one of our files — the caller then has no saved list (the SCAN key shows). */
inline bool parse(const std::string& text, Saved& out) {
    out = Saved{};
    if (text.size() > kMaxFileBytes || text.compare(0, 16, "VIBE-DAB-SCAN 1\n") != 0) return false;
    size_t p = 16;
    std::vector<bool> seen(vibedab::kBandIIICount, false);
    Hit* cur = nullptr;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("at ", 0) == 0) {
            long long at = 0; int secs = 0, canc = 0; char by[32] = "";
            if (std::sscanf(line.c_str(), "at %lld secs %d cancelled %d by %31s", &at, &secs, &canc, by) >= 3) {
                out.at = at; out.secs = secs < 0 ? 0 : secs; out.cancelled = canc != 0; out.by = by;
            }
        } else if (line.rfind("why ", 0) == 0) {
            out.why = clean(line.substr(4), 200);
        } else if (line.rfind("H ", 0) == 0) {
            cur = nullptr;
            char name[16] = ""; int comp = 0; float fib = 0, mer = 0; int used = 0;
            if (std::sscanf(line.c_str(), "H %15s %d %f %f %n", name, &comp, &fib, &mer, &used) < 4 || used <= 0) continue;
            const int idx = blockIndex(name);
            if (idx < 0 || seen[(size_t)idx]) continue;   // ★ an unknown or repeated block: damage, skipped
            seen[(size_t)idx] = true;
            Hit h;
            h.idx = idx; h.complete = comp != 0; h.fibRate = fib; h.merDb = mer;
            h.ensemble = clean(line.substr((size_t)used));
            out.hits.push_back(std::move(h));
            cur = &out.hits.back();
        } else if (line.rfind("S ", 0) == 0 && cur) {
            char* end = nullptr;
            const unsigned long sid = std::strtoul(line.c_str() + 2, &end, 10);
            if (!end || end == line.c_str() + 2 || sid == 0 || sid > 0xFFFFFFFFul || cur->services.size() >= kMaxServices) continue;
            std::string label = *end == ' ' ? std::string(end + 1) : std::string();
            cur->services.push_back({(uint32_t)sid, clean(label)});
        }
    }
    // ★ block order, as the scan walks the band
    for (size_t i = 1; i < out.hits.size(); ++i)
        for (size_t j = i; j > 0 && out.hits[j - 1].idx > out.hits[j].idx; --j) std::swap(out.hits[j - 1], out.hits[j]);
    return true;
}

/** Read the file. @return false when there is none, it cannot be read, or it is not ours. */
inline bool load(const std::string& path, Saved& out) {
    out = Saved{};
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
        if (text.size() > kMaxFileBytes) { std::fclose(f); return false; }
    }
    std::fclose(f);
    return parse(text, out);
}

/** Write `text` to `path` atomically. @return false (with `err`) when the place is missing or read-only — the caller
 *  carries on with the list in memory. */
inline bool writeAtomic(const std::string& path, const std::string& text, std::string* err = nullptr) {
    if (path.empty()) { if (err) *err = "no file"; return false; }
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { if (err) *err = std::strerror(errno); return false; }
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t w = ::write(fd, text.data() + off, text.size() - off);
        if (w < 0) { if (errno == EINTR) continue; if (err) *err = std::strerror(errno); ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += (size_t)w;
    }
    if (::fsync(fd) != 0) { if (err) *err = std::strerror(errno); ::close(fd); ::unlink(tmp.c_str()); return false; }
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { if (err) *err = std::strerror(errno); ::unlink(tmp.c_str()); return false; }
    const size_t sl = path.find_last_of('/');
    const std::string dir = sl == std::string::npos ? std::string(".") : (sl == 0 ? std::string("/") : path.substr(0, sl));
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }   // ★ the rename itself survives a power cut
    return true;
}

}  // namespace vibedabscan
