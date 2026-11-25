// vibe_fctlm_gallery.h — V12 beta91 TELEMETRY CHARTS: FUNcube telemetry KEPT in the gallery, per pass and per day.
//
// ★★★ WHY (Stuart, 2026-11-22): "how will the funcube telemetry display? will it be saved as a text file to the gallery?"
//     → "that could be good to have" → "especially for those who background decode it every pass". The decoder
//     (vibe_fctlm.h) put ONE readable line a frame into an RTTY-style run — the battery's raw count only, a new file each
//     time the decoder attached. Nothing a chart could read, and nothing that built history across passes.
//
// ── What is written ────────────────────────────────────────────────────────────────────────────────────────────
//  • A PASS — one .csv item (kind "fctlm"), every channel the frame carries, a row a frame (one every 5 s):
//        # FUNcube telemetry · AO-73 · 145.9350 MHz USB
//        # Pass from 2026-11-22 14:02:16 UTC · AOS 14:01 · LOS 14:14 UTC
//        # Units from AMSAT-UK's published telemetry equations (…). [raw] = that channel has no published conversion. …
//        utc,unix,frame,seq,mode,elevation [deg],rs fixed,Photo voltage 1 [V],…,Reboot count,…
//        2026-11-22T14:02:16Z,1795442536,WO1,123456,Sunlight,23.4,0,3.963,…
//    A column's heading is the channel's NAME, then "[unit]" (★ beta94: V, mA, °C, mW, lux — AMSAT-UK's published
//    equations, vibe_fctlm.h), "[raw]" only where no conversion is published, or nothing for a count / flag. A board whose
//    data-valid flag is off in a frame leaves its cells BLANK (never 0). Names carry no commas, so it opens as-is in a spreadsheet; the
//    charts (src/utils/tlmCharts.ts, both clients) read the same headings.
//    Titled "AO-73 telemetry · pass 14:02 UTC · 23 frames" (the clients find a satellite's passes by that prefix), and
//    downloaded as "AO-73 telemetry 2026-11-22 1402 UTC.csv".
//    ★ THE PASS: the satellite engine's AOS when the lock knows it (a SATELLITE pick, a background pass) — a new AOS is a
//      new pass; without one (a hand-tuned listener) a gap over kPassGapSec. A change of satellite or of the frame's
//      channel set starts a new file too, and so does kPassMaxBytes ("part 2") — none grows for ever.
//  • THE DAY — ONE .txt item per satellite per UTC day, a line a frame, whoever was decoding (Stuart 11-22: a background
//    profile's pass and a listener's pick of the same satellite write into the SAME day's log — Store::adoptSlot with
//    anyWriter — and every RADIO on the server too: the slot "day-YYYYMMDD-<sat>" alone finds it, never the radio, the
//    profile, the origin or the dial):
//        14:02:16  AO-73  WO1 · seq 123456 · Sunlight · battery 7.876 V · el 23° · RS fixed 0   (or "no EPS data")
//    Titled "AO-73 telemetry log · 22 Nov · 180 frames" — the count is the FILE's lines after each append, so two writers
//    keep it right; a restart reads the day back and counts on.
//    ★★ TWO WRITERS, ONE FILE, NO HALF LINES: every tap in the process writing a satellite's day shares ONE DayFile (below),
//       which holds the lines and writes them — whole lines only, ONE O_APPEND write under the gallery's exclusive flock
//       (Store::append). A PASS is shared the same way (PassFile): a background decode and a listener's pick of the same
//       pass make ONE .csv, each frame once. ★★★ AND ACROSS RADIO PROCESSES (Stuart 11-22: "all radios on same server
//       should feed the same log for the satellite"): see FcLock below — flock, slot lookup without the radio, dedupe
//       against the file's own rows.
//
// ★★ NEVER A WRITE PER FRAME (the RTTY rule, a Pi 2's SD card): both are held in RAM and appended once a minute or 8 KB,
//    when a pass ends, at a UTC midnight and when the decoder stops. A 12-minute AO-73 pass is ~150 frames ≈ 60 KB of
//    CSV and ~15 KB of log — inside the owner's gallery space, whose eviction (the oldest first) bounds the whole.
// ★ No position of the SERVER is written (the elevation is the bird's, as seen from it — the same the panel shows).
#pragma once
#include "vibe_gallery.h"
#include "vibe_decoder_host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace vibe {
namespace bg {
namespace fctlmlog {

inline constexpr int64_t  kPassGapSec   = 20 * 60;      ///< no AOS known: frames this far apart are another pass
inline constexpr uint64_t kPassMaxBytes = 256 * 1024;   ///< one pass's CSV — then "part 2"
inline constexpr size_t   kFlushBytes   = 8192;
inline constexpr int64_t  kFlushSec     = 60;

/** A channel's name as a CSV heading: printable, no commas or quotes, then "[raw]" / "[unit]" / nothing. */
inline std::string column(const vibefc::Value& v) {
    std::string n;
    for (unsigned char c : v.name) { if (n.size() >= 48) break; if (c >= 0x20 && c < 0x7F && c != ',' && c != '"' && c != '[' && c != ']') n += char(c); }
    if (n.empty()) n = v.key;
    if (v.raw) return n + " [raw]";
    if (!v.unit.empty()) {
        // ★ beta94 UTF-8 kept ("°C"); ASCII controls, commas, quotes and brackets dropped
        std::string u; for (unsigned char c : v.unit) if (c >= 0x20 && c != 0x7F && c != ',' && c != '"' && c != '[' && c != ']') u += char(c);
        if (!u.empty()) return n + " [" + u + "]";
    }
    return n;
}
/** The fixed columns, then one per value. */
inline std::string csvHeader(const vibefc::Decoded& d) {
    std::string h = "utc,unix,frame,seq,mode,elevation [deg],rs fixed";
    for (const auto& v : d.values) h += "," + column(v);
    return h + "\n";
}
inline std::string cell(const std::string& s) {   // a text cell: printable, no commas / quotes (frame names, modes)
    std::string o; for (unsigned char c : s) if (c >= 0x20 && c < 0x7F && c != ',' && c != '"') o += char(c);
    return o.substr(0, 24);
}
inline std::string csvRow(const vibefc::Decoded& d, const vibefc::TlmCtx& c, int64_t t) {
    std::string r = gallery::utcStamp(t, "%Y-%m-%dT%H:%M:%SZ") + "," + std::to_string((long long)t) + "," + cell(d.frameName) + ","
                  + (d.seq >= 0 ? std::to_string(d.seq) : std::string()) + "," + cell(d.mode) + ",";
    if (std::isfinite(c.elDeg)) { char b[16]; std::snprintf(b, sizeof b, "%.1f", c.elDeg); r += b; }
    r += "," + std::to_string(d.rsFixed);
    // ★★ beta94 a board whose data-valid flag is off: a BLANK cell (never a 0 to chart); readings to their decimals
    for (const auto& v : d.values) r += "," + (v.valid ? vibefc::num(v.v, v.dp) : std::string());
    return r + "\n";
}
/** What the columns are: a frame whose channel set differs starts a new file (its header would not fit). */
inline std::string signature(const vibefc::Decoded& d) { std::string s; for (const auto& v : d.values) s += v.key + ";"; return s; }
/** "AO-73" → "AO-73"; anything else a file name or a slot would not like is dropped. */
inline std::string satToken(const std::string& sat) {
    std::string o; for (unsigned char c : sat) if (std::isalnum(c) || c == '-' || c == '_') o += char(c);
    return o.empty() ? "SAT" : o.substr(0, 24);
}
/** The day log's readable line (its time added by the writer). */
inline std::string dayLine(const vibefc::Decoded& d, const vibefc::TlmCtx& c, const std::string& sat) {
    std::string l = gallery::printable(sat, 24) + "  " + cell(d.frameName) + " \xc2\xb7 seq " + std::to_string(d.seq) + " \xc2\xb7 " + cell(d.mode);
    const std::string bat = vibefc::batteryText(d);   // ★ beta94 "battery 7.876 V" / "no EPS data"
    if (!bat.empty()) l += " \xc2\xb7 " + bat;
    if (std::isfinite(c.elDeg)) { char b[24]; std::snprintf(b, sizeof b, " \xc2\xb7 el %.0f\xc2\xb0", c.elDeg); l += b; }
    l += " \xc2\xb7 RS fixed " + std::to_string(d.rsFixed);
    if (!d.fitter.empty()) l += " \xc2\xb7 \"" + d.fitter.substr(0, 200) + "\"";
    return l;
}

// ── ★★★ ONE PASS, ONE DAY — whoever decodes it (Stuart 11-22: "only one chart needed regardless of background or user
//     listened"). A background VFO and a listener's SATELLITE pick may decode the SAME pass of the same bird at once (two
//     decoder hosts, one process). They share ONE PassFile (found by satellite + AOS, or — no AOS known — a frame within
//     kPassGapSec of its last) and ONE DayFile (satellite + UTC day): one .csv, one log, each frame ONCE (by its sequence
//     number, else its time). The file is written by whichever tap flushes; the last tap to leave writes the rest and
//     closes it. Different radios are different processes and never decode one bird's same pass on the same box. ────────
struct PassFile {
    std::mutex m;
    gallery::Store* store = nullptr;
    gallery::Meta meta;                 ///< the first writer's (dial, mode, origin, profile)
    std::string sat, sig, header, rows, name;
    double aos = 0, los = 0, maxEl = -90;
    int64_t start = 0, last = 0, lastFlush = 0;
    int frames = 0, partFrames = 0, part = 1, users = 0;
    std::set<long long> seen;           ///< the frames written (a pass is ~150 — bounded by the pass itself)
};
struct DayFile {
    std::mutex m;
    gallery::Store* store = nullptr;
    gallery::Meta meta;
    std::string sat, name, pending;
    int64_t day = -1, lastFlush = 0;
    long frames = 0;
    int users = 0;
    std::deque<long long> seenQ; std::set<long long> seen;   ///< frames logged today, the newest kSeenMax
};
inline constexpr size_t kSeenMax = 4096;
inline bool markSeen(std::set<long long>& s, std::deque<long long>* q, long long k) {
    if (!s.insert(k).second) return false;
    if (q) { q->push_back(k); if (q->size() > kSeenMax) { s.erase(q->front()); q->pop_front(); } }
    return true;
}
/** A frame's identity: its sequence number (one a frame, from the satellite), else its time. */
inline long long frameKey(const vibefc::Decoded& d, int64_t t) { return d.seq >= 0 ? d.seq : -1 - (long long)t; }

class Registry {
public:
    static Registry& get() { static Registry r; return r; }
    /** The pass this frame belongs to — an open one another tap is writing, or a new one. Caller then holds a use. */
    std::shared_ptr<PassFile> pass(gallery::Store& st, const gallery::Meta& meta, const std::string& sat, const std::string& sig,
                                   const vibefc::TlmCtx& c, int64_t t, const std::shared_ptr<PassFile>& mine) {
        std::lock_guard<std::mutex> lk(m_);
        prune();
        auto same = [&](PassFile& p) {
            if (p.store != &st || p.sat != sat || p.sig != sig) return false;
            if (c.aos > 0 && p.aos > 0) return std::fabs(c.aos - p.aos) <= 120;
            return t - p.last <= kPassGapSec && t >= p.start - kPassGapSec;
        };
        if (mine) { std::lock_guard<std::mutex> pl(mine->m); if (same(*mine)) return mine; }
        for (auto& w : passes_) if (auto p = w.lock()) { std::lock_guard<std::mutex> pl(p->m); if (p->users > 0 && same(*p)) { ++p->users; return p; } }
        auto p = std::make_shared<PassFile>();
        p->store = &st; p->meta = meta; p->sat = sat; p->sig = sig; p->aos = c.aos; p->los = c.los; p->start = t; p->last = t;
        p->lastFlush = t; p->users = 1;
        passes_.push_back(p);
        return p;
    }
    std::shared_ptr<DayFile> day(gallery::Store& st, const gallery::Meta& meta, const std::string& sat, int64_t day, int64_t t,
                                 const std::shared_ptr<DayFile>& mine) {
        std::lock_guard<std::mutex> lk(m_);
        prune();
        if (mine && mine->store == &st && mine->sat == sat && mine->day == day) return mine;
        for (auto& w : days_) if (auto d = w.lock()) { std::lock_guard<std::mutex> dl(d->m); if (d->users > 0 && d->store == &st && d->sat == sat && d->day == day) { ++d->users; return d; } }
        auto d = std::make_shared<DayFile>();
        d->store = &st; d->meta = meta; d->sat = sat; d->day = day; d->lastFlush = t; d->users = 1;
        days_.push_back(d);
        return d;
    }
private:
    void prune() {
        passes_.erase(std::remove_if(passes_.begin(), passes_.end(), [](const std::weak_ptr<PassFile>& w) { return w.expired(); }), passes_.end());
        days_.erase(std::remove_if(days_.begin(), days_.end(), [](const std::weak_ptr<DayFile>& w) { return w.expired(); }), days_.end());
    }
    std::mutex m_;
    std::vector<std::weak_ptr<PassFile>> passes_;
    std::vector<std::weak_ptr<DayFile>> days_;
};

/** The frame lines a day log holds ("hh:mm:ss  …"). */
inline long countLines(const std::string& body) {
    long n = 0; size_t p = 0;
    while (p < body.size()) {
        const size_t e = body.find('\n', p);
        if (e != std::string::npos && e - p > 10 && body[p + 2] == ':' && body[p + 5] == ':') ++n;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return n;
}
inline std::string upperOf(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// ── A pass's CSV (caller holds p.m) ──
inline gallery::Meta passMeta(const PassFile& p) {
    gallery::Meta m = p.meta;
    m.kind = "fctlm"; m.utc = p.start;
    std::string t = p.sat + " telemetry \xc2\xb7 pass " + gallery::utcStamp(p.start, "%H:%M") + " UTC";
    if (p.part > 1) t += " \xc2\xb7 part " + std::to_string(p.part);
    t += " \xc2\xb7 " + std::to_string(p.partFrames) + (p.partFrames == 1 ? " frame" : " frames");
    if (p.maxEl > -90) { char b[24]; std::snprintf(b, sizeof b, " \xc2\xb7 max el %.0f\xc2\xb0", p.maxEl); t += b; }
    m.title = t;
    m.dlName = gallery::downloadSafe(p.sat + " telemetry " + gallery::utcStamp(p.start, "%Y-%m-%d %H%M") + " UTC"
                                     + (p.part > 1 ? " part " + std::to_string(p.part) : std::string()) + ".csv");
    return m;
}
inline std::string passHead(const PassFile& p) {
    std::string h = "# FUNcube telemetry \xc2\xb7 " + p.sat;
    if (p.meta.freqHz > 0) h += " \xc2\xb7 " + gallery::freqText(p.meta.freqHz) + (p.meta.mode.empty() ? std::string() : " " + upperOf(p.meta.mode));
    h += "\n# Pass from " + gallery::utcStamp(p.start, "%Y-%m-%d %H:%M:%S") + " UTC";
    if (p.aos > 0) h += " \xc2\xb7 AOS " + gallery::utcStamp((int64_t)p.aos, "%H:%M");
    if (p.los > 0) h += " \xc2\xb7 LOS " + gallery::utcStamp((int64_t)p.los, "%H:%M") + " UTC";
    if (p.part > 1) h += " \xc2\xb7 part " + std::to_string(p.part);
    h += "\n# Units from AMSAT-UK's published telemetry equations (FUNcube-1 telemetry_equations_b1; JY1Sat data budget 2F)."
         " [raw] = that channel has no published conversion. A blank cell = the frame's data-valid flag for that board was off."
         " Times are UTC.\n";
    return h + p.header;
}
/** @return true when it CREATED an item. */
// ── ★★★ ACROSS PROCESSES (Stuart: "all radios on same server should feed the same log for the satellite, it is the same
//     satellite after all"). Every radio is its own process (vibeserver@<serial>), all on ONE gallery directory. So a
//     flush, in ANY process, runs under an exclusive flock on <gallery>/.fctlm.lock (released by the kernel if a process
//     dies), and inside it: find the item by its SLOT alone (Store::adoptSlot anyWriter — no radio, profile, origin or
//     dial), read the frames it already holds (the CSV's seq column / the log's "seq N"), drop ours that are there,
//     append the rest as whole lines in one O_APPEND write, and count the file for the title. Two processes therefore
//     make ONE item, each frame ONCE, and nothing interleaves. The flock also serialises threads in one process (each
//     flush opens its own descriptor). Cost: one read of a ≤ 256 KB pass file / a day log once a minute. ──────────────
struct FcLock {
    int fd = -1;
    explicit FcLock(const std::string& dir) {
        if (dir.empty()) return;
        fd = ::open((dir + "/.fctlm.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) while (::flock(fd, LOCK_EX) != 0 && errno == EINTR) {}
    }
    ~FcLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    FcLock(const FcLock&) = delete; FcLock& operator=(const FcLock&) = delete;
};
/** A CSV row's frame key: its seq column (the 4th), else -1 - its unix time; LLONG_MIN = not a row. */
inline long long csvKey(const std::string& l) {
    if (l.empty() || l[0] == '#' || !std::isdigit((unsigned char)l[0])) return LLONG_MIN;
    size_t c1 = l.find(','), c2 = c1 == std::string::npos ? c1 : l.find(',', c1 + 1), c3 = c2 == std::string::npos ? c2 : l.find(',', c2 + 1),
           c4 = c3 == std::string::npos ? c3 : l.find(',', c3 + 1);
    if (c4 == std::string::npos) return LLONG_MIN;
    if (c4 > c3 + 1) return std::strtoll(l.c_str() + c3 + 1, nullptr, 10);
    return -1 - std::strtoll(l.c_str() + c1 + 1, nullptr, 10);
}
/** A day-log line's frame key ("… · seq 123 · …"), LLONG_MIN = not a frame line. */
inline long long dayKey(const std::string& l) {
    if (l.size() < 10 || l[2] != ':' || l[5] != ':') return LLONG_MIN;
    const size_t p = l.find(" seq ");
    return p == std::string::npos ? LLONG_MIN : std::strtoll(l.c_str() + p + 5, nullptr, 10);
}
/** `fresh`'s lines whose key is not in `body` (keyed by `key`); counts what the file then holds. */
template <class K>
inline std::string unseen(const std::string& body, const std::string& fresh, K key, long& total, double* maxEl = nullptr) {
    std::set<long long> have; total = 0;
    auto eachLine = [](const std::string& s, auto fn) { size_t p = 0; while (p < s.size()) { size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size(); fn(s.substr(p, e - p)); p = e + 1; } };
    eachLine(body, [&](const std::string& l) {
        const long long k = key(l); if (k == LLONG_MIN) return;
        if (have.insert(k).second) ++total;
        if (maxEl) { size_t c = 0; for (int i = 0; i < 5 && c != std::string::npos; ++i) c = l.find(',', c + (i ? 1 : 0)); if (c != std::string::npos) { char* e; const double v = std::strtod(l.c_str() + c + 1, &e); if (e != l.c_str() + c + 1 && v > *maxEl) *maxEl = v; } }
    });
    std::string out;
    eachLine(fresh, [&](const std::string& l) {
        const long long k = key(l);
        if (k != LLONG_MIN && !have.insert(k).second) return;   // ★ another process wrote this frame
        if (k != LLONG_MIN) ++total;
        if (maxEl) { size_t c = 0; for (int i = 0; i < 5 && c != std::string::npos; ++i) c = l.find(',', c + (i ? 1 : 0)); if (c != std::string::npos) { char* e; const double v = std::strtod(l.c_str() + c + 1, &e); if (e != l.c_str() + c + 1 && v > *maxEl) *maxEl = v; } }
        out += l + "\n";
    });
    return out;
}
/** The pass's slot: "pass-<sat>-<AOS minute>" (or, with no AOS, "pass-<sat>-t<start minute>") — found by every process.
 *  Failing that, a pass of the satellite on disk whose last row is within kPassGapSec is joined (one radio knowing the
 *  AOS and another not), unless both know the AOS and it differs. */
inline std::string passSlot(const PassFile& p) {
    return "pass-" + satToken(p.sat) + "-" + (p.aos > 0 ? std::to_string((long long)std::llround(p.aos / 60.0))
                                                        : "t" + std::to_string((long long)(p.start / 60)))
         + (p.part > 1 ? "-p" + std::to_string(p.part) : std::string());
}
inline std::string recentPassOnDisk(gallery::Store& st, const std::string& sat, int64_t t, double aos) {
    const std::vector<std::string> all = st.names("fctlm");
    const std::string pre = "pass-" + satToken(sat) + "-";
    int looked = 0;
    for (size_t i = all.size(); i-- > 0 && looked < 24;) {
        const std::string& n = all[i];
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".csv") != 0) continue;
        ++looked;
        const std::string slot = st.field(n, "s");
        if (slot.rfind(pre, 0) != 0) continue;
        if (slot.find("-p", pre.size()) != std::string::npos) continue;   // ★ a later part is found by its own slot
        // ★ both know the AOS and they differ: another pass
        if (aos > 0 && slot.size() > pre.size() && std::isdigit((unsigned char)slot[pre.size()])
            && std::llabs(std::strtoll(slot.c_str() + pre.size(), nullptr, 10) - (long long)std::llround(aos / 60.0)) > 2) continue;
        std::string body; if (!st.readItem(n, body)) continue;
        const size_t e = body.find_last_not_of('\n'); if (e == std::string::npos) continue;
        const size_t b = body.rfind('\n', e);
        const std::string last = body.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
        const size_t c1 = last.find(',');
        if (c1 == std::string::npos) continue;
        const long long lt = std::strtoll(last.c_str() + c1 + 1, nullptr, 10);
        if (lt > 0 && t - lt <= kPassGapSec && t + kPassGapSec >= lt) return n;
    }
    return "";
}
/** @return true when it CREATED an item. Caller holds p.m. */
inline bool flushPass(PassFile& p, int64_t t) {
    p.lastFlush = t;
    if (p.rows.empty()) return false;
    gallery::Store& st = *p.store;
    FcLock fl(st.dir());
    gallery::Meta m = passMeta(p);
    m.slot = passSlot(p);
    if (p.name.empty()) {
        p.name = st.adoptSlot(m, /*anyOrigin=*/true, /*anyWriter=*/true);   // ★ another radio's file for this pass
        // ★ one radio knows the AOS and another does not (a hand-tuned listener): the pass on disk whose last frame is recent
        if (p.name.empty() && p.part == 1) p.name = recentPassOnDisk(st, p.sat, t, p.aos);
    }
    bool made = false;
    std::string body;
    if (!p.name.empty() && st.readItem(p.name, body)) {
        long total = 0; double mx = p.maxEl;
        const std::string add = unseen(body, p.rows, csvKey, total, &mx);
        if ((add.empty() || st.append(p.name, add))) {
            p.partFrames = (int)total; p.maxEl = mx;
            m = passMeta(p); { const std::string own = st.field(p.name, "s"); m.slot = own.empty() ? passSlot(p) : own; }
            st.retag(p.name, m);
            p.rows.clear();
            return false;
        }
        st.close(p.name);   // ★ gone (evicted / cleared): a new one below
    }
    long total = 0; double mx = p.maxEl;
    const std::string rows = unseen(std::string(), p.rows, csvKey, total, &mx);
    p.partFrames = (int)total; p.maxEl = mx;
    m = passMeta(p); m.slot = passSlot(p);
    p.name = st.create(m, "csv", passHead(p) + rows);
    made = !p.name.empty();
    p.rows.clear();
    return made;
}

// ── The day's log (caller holds d.m) ──
inline gallery::Meta dayMeta(const DayFile& d, int64_t now) {
    gallery::Meta m = d.meta;
    m.kind = "fctlm";
    m.slot = "day-" + gallery::utcStamp(now, "%Y%m%d") + "-" + satToken(d.sat);
    m.utc = (now / 86400) * 86400;
    m.title = d.sat + " telemetry log \xc2\xb7 " + gallery::utcStamp(now, "%d %b") + " \xc2\xb7 " + std::to_string(d.frames)
            + (d.frames == 1 ? " frame" : " frames");
    m.dlName = gallery::downloadSafe(d.sat + " telemetry " + gallery::utcStamp(now, "%Y-%m-%d") + ".txt");
    return m;
}
inline bool flushDay(DayFile& d, int64_t now) {
    d.lastFlush = now;
    if (d.pending.empty()) return false;
    gallery::Store& st = *d.store;
    const int64_t at = std::max<int64_t>(d.day * 86400, std::min<int64_t>(now, d.day * 86400 + 86399));
    FcLock fl(st.dir());   // ★ every process and thread: find-or-create, dedupe and append, one at a time
    // ★ ONE log per satellite per day on this SERVER: the slot alone finds it — never the radio, profile, origin or dial
    if (d.name.empty()) d.name = st.adoptSlot(dayMeta(d, at), /*anyOrigin=*/true, /*anyWriter=*/true);
    std::string body;
    if (!d.name.empty() && st.readItem(d.name, body)) {
        long total = 0;
        const std::string add = unseen(body, d.pending, dayKey, total);
        if (add.empty() || st.append(d.name, add)) {
            d.frames = total;   // ★ the count is the FILE's (other radios' and a restart's lines included)
            st.retag(d.name, dayMeta(d, at));
            d.pending.clear();
            return false;
        }
        st.close(d.name);
    }
    long total = 0;
    const std::string lines = unseen(std::string(), d.pending, dayKey, total);
    d.frames = total;
    const std::string head = d.sat + " \xc2\xb7 FUNcube telemetry \xc2\xb7 " + gallery::utcStamp(at, "%Y-%m-%d") + " (UTC)\n"
        + "One line a good frame, every pass this server decoded (every radio, background passes and listeners alike, each frame once). "
          "Each pass's every channel is its own .csv item, in the units of AMSAT-UK's published equations.\n\n";
    d.name = st.create(dayMeta(d, at), "txt", head + lines);
    d.pending.clear();
    return !d.name.empty();
}

} // namespace fctlmlog

class FcTlmGalleryTap : public DecoderTap {
public:
    FcTlmGalleryTap(gallery::Store& store, gallery::Meta meta, std::function<int64_t()> now = nullptr)
        : store_(store), meta_(std::move(meta)), now_(now ? std::move(now) : std::function<int64_t()>([] { return gallery::nowUtc(); })) {
        meta_.kind = "fctlm";
    }
    ~FcTlmGalleryTap() override { finish(true); }

    void satTlm(const vibefc::Decoded& d, const vibefc::TlmCtx& c) override {
        using namespace fctlmlog;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = c.unix > 0 ? (int64_t)c.unix : now_();
        const std::string sat = c.sat.empty() ? std::string("AO-73") : gallery::printable(c.sat, 24);
        const std::string sig = signature(d);
        // ★ the pass: the one this tap is on, one another tap is writing, or a new one
        auto p = Registry::get().pass(store_, meta_, sat, sig, c, t, pass_);
        if (p != pass_) { leavePass(t); pass_ = p; }
        bool fresh = false;
        {
            std::lock_guard<std::mutex> pl(p->m);
            if (p->header.empty()) p->header = csvHeader(d);
            fresh = markSeen(p->seen, nullptr, frameKey(d, t));   // ★ a frame another tap already wrote: not again
            if (fresh) {
                if ((p->name.empty() ? 0 : store_.sizeOf(p->name)) + p->rows.size() > kPassMaxBytes) {
                    if (flushPass(*p, t)) ++saved_;   // ★ the part so far, then a new file for the rest of the pass
                    if (!p->name.empty()) store_.close(p->name);
                    p->name.clear(); ++p->part; p->partFrames = 0;
                }
                p->rows += csvRow(d, c, t);
                ++p->frames; ++p->partFrames;
                if (t > p->last) p->last = t;
                if (std::isfinite(c.elDeg) && c.elDeg > p->maxEl) p->maxEl = c.elDeg;
            }
            if (p->rows.size() >= kFlushBytes || t - p->lastFlush >= kFlushSec) if (flushPass(*p, t)) ++saved_;
        }
        // ★ the day's log: the same frame once, whichever tap heard it first
        auto dy = Registry::get().day(store_, meta_, sat, t / 86400, t, day_);
        if (dy != day_) { leaveDay(t - 1); day_ = dy; }
        std::lock_guard<std::mutex> dl(dy->m);
        if (fresh && markSeen(dy->seen, &dy->seenQ, frameKey(d, t)))
            dy->pending += gallery::utcStamp(t, "%H:%M:%S") + "  " + dayLine(d, c, sat) + "\n";
        if (dy->pending.size() >= kFlushBytes || t - dy->lastFlush >= kFlushSec) if (flushDay(*dy, t)) ++saved_;
    }
    void tick() override {
        using namespace fctlmlog;
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        const int64_t t = now_();
        if (pass_) { std::lock_guard<std::mutex> pl(pass_->m); if (!pass_->rows.empty() && t - pass_->lastFlush >= kFlushSec) if (flushPass(*pass_, t)) ++saved_; }
        if (day_) { std::lock_guard<std::mutex> dl(day_->m); if (!day_->pending.empty() && t - day_->lastFlush >= kFlushSec) if (flushDay(*day_, t)) ++saved_; }
    }
    void finish(bool) override {
        std::lock_guard<std::mutex> lk(m_);
        if (done_) return;
        done_ = true;
        int64_t t = now_();
        if (pass_) { std::lock_guard<std::mutex> pl(pass_->m); if (pass_->last > 0) t = pass_->last; }
        leavePass(t);
        leaveDay(t);
    }
    size_t savedCount() const override { std::lock_guard<std::mutex> lk(m_); return saved_; }
    /** For the tests: the current pass's file ("" before its first flush) and the day log's. */
    std::string passName() const { std::lock_guard<std::mutex> lk(m_); if (!pass_) return ""; std::lock_guard<std::mutex> pl(pass_->m); return pass_->name; }
    std::string dayName() const { std::lock_guard<std::mutex> lk(m_); if (!day_) return ""; std::lock_guard<std::mutex> dl(day_->m); return day_->name; }

private:
    /** This tap leaves its pass: what it holds is written now; the LAST tap out closes the file. */
    void leavePass(int64_t t) {
        if (!pass_) return;
        { std::lock_guard<std::mutex> pl(pass_->m);
          if (fctlmlog::flushPass(*pass_, t)) ++saved_;
          if (--pass_->users <= 0 && !pass_->name.empty()) { store_.close(pass_->name); pass_->name.clear(); } }
        pass_.reset();
    }
    void leaveDay(int64_t t) {
        if (!day_) return;
        { std::lock_guard<std::mutex> dl(day_->m);
          if (fctlmlog::flushDay(*day_, t)) ++saved_;
          if (--day_->users <= 0 && !day_->name.empty()) { store_.close(day_->name); day_->name.clear(); } }
        day_.reset();
    }

    gallery::Store& store_;
    gallery::Meta meta_;
    std::function<int64_t()> now_;
    mutable std::mutex m_;
    bool done_ = false;
    std::shared_ptr<fctlmlog::PassFile> pass_;
    std::shared_ptr<fctlmlog::DayFile> day_;
    size_t saved_ = 0;
};

} // namespace bg
} // namespace vibe
