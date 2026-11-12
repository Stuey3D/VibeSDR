// vibe_dab_last_service.h — WHICH STATION TO PLAY WHEN A MULTIPLEX IS CHOSEN WITHOUT ONE (V12, 2026-11-12).
//
// ★★★ WHY. A block step named no service, and the server chose none: the multiplex sat silent with "Pick a station"
//     (and, until the same day, a stale sub-channel squealing under it). Stuart, 2026-11-12: "last on MUX is good, but
//     with one extra rule … in the case where a station is on multiple multiplexes then switch to that one instead"
//     (Nation 80s on 10C and 10D). The rule, in order — chooseService() below:
//       1. the SAME STATION: the SId that was playing, if this ensemble carries it; failing that, ONE service whose
//          label is exactly the old one's (case and spaces aside) — conservative: two matches, or none, is no match;
//       2. the LAST ONE LISTENED TO on this multiplex (this store: by EId, the block's name as the fallback);
//       3. the FIRST audio service, in the order the clients list them (the report's order — by SId).
//     A request that names a service always wins; the DX sweep and the band scans never arm a choice (the shim).
// ★★ PER RADIO, like the band scan's file (main.cpp names it beside dab-scan; Start+Select's reset unlinks it).
// ★★ BOUNDED: kMaxEntries multiplexes (Band III is 41 blocks; a DXer meets more EIds than blocks, so 64), the oldest
//    use goes first; a file past kMaxFileBytes is damage and is not read. ATOMIC: temp + fsync + rename.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace vibedablast {

constexpr size_t kMaxEntries   = 64;
constexpr size_t kMaxFileBytes = 16 * 1024;

struct Entry { uint64_t seq = 0; uint32_t eid = 0; std::string block; uint32_t sid = 0; };

/** The store: in memory, with a file behind it. Not thread-safe — the owner (DabService) holds its own lock. */
class Store {
public:
    const std::vector<Entry>& entries() const { return e_; }
    void clear() { e_.clear(); seq_ = 0; block_.clear(); }

    /** ★★ THE BLOCK THIS RADIO WAS LAST LISTENED TO ON (V12, 2026-11-12 — vibe_dab_landing.h rule 1). Kept in the same
     *  file as the per-multiplex stations, so Start+Select's reset forgets both. "" = none. A listener's choice only: the
     *  DX sweep, the band scans and background decoding never reach this (the shim records it). */
    const std::string& lastBlock() const { return block_; }
    /** True if it changed (the caller saves). */
    bool setLastBlock(const std::string& b) {
        if (b.empty() || b.size() > 4 || b == block_) return false;
        block_ = b;
        return true;
    }

    /** The service last played on this multiplex: by EId when known, else by block name. 0 = none remembered. */
    uint32_t lookup(uint32_t eid, const std::string& block) const {
        if (eid) for (const auto& x : e_) if (x.eid == eid) return x.sid;
        // ★ The block's name, the fallback: the newest entry for it (an EId not met before, or none read yet).
        const Entry* best = nullptr;
        if (!block.empty()) for (const auto& x : e_) if (x.block == block && (!best || x.seq > best->seq)) best = &x;
        return best ? best->sid : 0;
    }
    /** Remember `sid` for this multiplex. True if anything changed (the caller saves). */
    bool remember(uint32_t eid, const std::string& block, uint32_t sid) {
        if (!sid || (eid == 0 && block.empty())) return false;
        Entry* hit = nullptr;
        for (auto& x : e_) if (eid ? x.eid == eid : (x.eid == 0 && x.block == block)) { hit = &x; break; }
        if (hit && hit->sid == sid && hit->block == block) { hit->seq = ++seq_; return false; }
        if (!hit) {
            if (e_.size() >= kMaxEntries) {
                auto oldest = std::min_element(e_.begin(), e_.end(), [](const Entry& a, const Entry& b) { return a.seq < b.seq; });
                e_.erase(oldest);
            }
            e_.push_back({});
            hit = &e_.back();
        }
        hit->seq = ++seq_; hit->eid = eid; hit->block = block; hit->sid = sid;
        return true;
    }

    std::string serialise() const {
        std::string out = "VIBE-DAB-LAST 1\n";
        char b[96];
        // ★ "B <block>" — an older build skips the line it cannot read (parse() below does the same for any unknown line)
        if (!block_.empty()) out += "B " + block_ + "\n";
        for (const auto& x : e_) {
            std::snprintf(b, sizeof b, "L %llu %X %s %X\n", (unsigned long long)x.seq, unsigned(x.eid),
                          x.block.empty() ? "-" : x.block.c_str(), unsigned(x.sid));
            out += b;
        }
        return out;
    }
    /** Replace the store with `text`. False (and the store empty) if it is not ours. */
    bool parse(const std::string& text) {
        clear();
        if (text.size() > kMaxFileBytes || text.rfind("VIBE-DAB-LAST 1\n", 0) != 0) return false;
        size_t p = text.find('\n') + 1;
        while (p < text.size() && e_.size() < kMaxEntries) {
            size_t q = text.find('\n', p);
            if (q == std::string::npos) q = text.size();
            const std::string line = text.substr(p, q - p);
            p = q + 1;
            unsigned long long seq = 0; unsigned eid = 0, sid = 0; char blk[16] = {0};
            if (line.size() > 2 && line[0] == 'B' && line[1] == ' ') {
                if (std::sscanf(line.c_str(), "B %4s", blk) == 1) block_ = blk;
                continue;
            }
            if (std::sscanf(line.c_str(), "L %llu %X %15s %X", &seq, &eid, blk, &sid) != 4 || !sid) continue;
            Entry x; x.seq = seq; x.eid = eid; x.block = std::strcmp(blk, "-") ? blk : ""; x.sid = sid;
            e_.push_back(x);
            seq_ = std::max<uint64_t>(seq_, seq);
        }
        return true;
    }
    bool load(const std::string& path) {
        clear();
        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) return false;
        std::string t; char b[4096]; size_t n;
        while ((n = std::fread(b, 1, sizeof b, fp)) > 0 && t.size() <= kMaxFileBytes) t.append(b, n);
        std::fclose(fp);
        return parse(t);
    }
    bool save(const std::string& path) const {
        if (path.empty()) return false;
        const std::string tmp = path + ".tmp", text = serialise();
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;
        const bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
        return true;
    }
private:
    std::vector<Entry> e_;
    uint64_t seq_ = 0;
    std::string block_;
};

/** Labels compared as a listener reads them: case and runs of spaces aside, ends trimmed. */
inline std::string labelKey(const std::string& s) {
    std::string o; bool sp = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) { sp = !o.empty(); continue; }
        if (sp) { o += ' '; sp = false; }
        o += char(std::tolower(c));
    }
    return o;
}

struct Candidate { uint32_t sid; std::string label; };

/** ★ The rule (see the top of the file). `list` = the playable audio services in the clients' order. 0 = nothing. */
inline uint32_t chooseService(const std::vector<Candidate>& list, uint32_t prevSid, const std::string& prevLabel,
                              uint32_t lastOnMux, const char** why = nullptr) {
    auto has = [&](uint32_t sid) { return sid && std::any_of(list.begin(), list.end(), [&](const Candidate& c) { return c.sid == sid; }); };
    if (list.empty()) return 0;
    if (has(prevSid)) { if (why) *why = "the same station"; return prevSid; }
    const std::string k = labelKey(prevLabel);
    if (k.size() >= 3) {    // ★ "BBC" alone is not a station; a two-letter label proves nothing
        uint32_t m = 0; int n = 0;
        for (const auto& c : list) if (labelKey(c.label) == k) { m = c.sid; ++n; }
        if (n == 1) { if (why) *why = "the same station (by name)"; return m; }
    }
    if (has(lastOnMux)) { if (why) *why = "the last one played here"; return lastOnMux; }
    if (why) *why = "the first in the list";
    return list.front().sid;
}

}  // namespace vibedablast
