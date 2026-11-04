// vibe_ndb_gallery_clean.h — THE ONE-TIME CLEAN-UP OF NDB CARDS ALREADY ON DISK (V12, 2026-11-04).
//
// ★★ WHY: the NDB skimmer confirmed false idents made only of dots or only of dashes (EEI, IEE, EIE, ES, SE, IS, SI,
//    EHE …) — marine DGPS / data carriers read as Morse, clustered on 283.5–290 and 348.5–350 kHz. The skimmer no longer
//    votes them in without the carrier's repeat cycle, but the cards written before that still list them. Stuart: "can
//    we make it scan back and remove the garbage from the list too?"
//
// ★★ WHAT IT DOES, once per server (a marker file in the gallery folder, written when a pass completes):
//    every NDB .txt card — the day's heard log ("Each beacon, most recently heard first …"), each hourly card and the
//    rolling "NDB last 10 min" card ("Each beacon, lowest frequency first …") — has its beacon lines whose ident is
//    uniform (vibendb::uniformIdent) AND that say "(not in the published list)" AND have no distance MOVED (never deleted) to a section at the
//    bottom: kSetAsideHeading. ✗ Never by the list alone: SND, EMW, NWI, CDF are real beacons the list lacks — untouched.
//    ✗ Never by the ident alone: a PUBLISHED beacon with such an ident stays where it is.
//    What the card derives from its list is recomputed the way its writer (vibe_ndb_gallery.h) words it: "Beacons heard",
//    "Furthest", "Best SNR" (only re-chosen when the one named was moved), the day's "N beacons identified · furthest …"
//    line, and the TITLE in the index (title() / headline()).
// ★ A card that does not change is not rewritten (no SD wear). One card in RAM at a time, names only for the list — a
//   256 MB board with thousands of cards copes. Each rewrite is Store::rewrite: the file tmp+renamed where it stands
//   (its time, order, slot, download name and radio kept) and one index line appended.
// ★ It runs when the gallery is configured (LocalSdrShim::setGallery), before background decoding starts any NDB
//   writer. A card this process holds open (a writer already live) is skipped — and then the marker is NOT written, so
//   the next start finishes the job. Several radio processes on one folder: an flock() lets one run it; the others skip.
// ★ The day's MAP picture is not touched: its dots are only beacons with a published position, which these never have.
//   (Its title and caption count every row, so a past day's map may say a few more beacons than its card.)
#pragma once
#include "vibe_gallery.h"
#include "vibe_ndb_db.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vibe {
namespace ndbgal {

/** The section the moved lines go to — at the bottom of the card. The day's writer reads it back (DayLog::restore) and
 *  keeps it, so a restart never puts them back in the list. */
static const char* const kSetAsideHeading =
    "Probably data or noise, not beacons (all dots or all dashes, not in the published list \xe2\x80\x94 kept for the record):";
/** The marker: this server's cards have been through the clean-up. */
static const char* const kCleanMarker = ".ndb-uniform-clean-1";

namespace clean {

static const char* const kSep = " \xc2\xb7 ";   // " · "
static const char* const kDash = " \xe2\x80\x94 ";   // " — "
static const char* const kUnlisted = "(not in the published list)";

inline std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> f; size_t q = 0;
    for (;;) {
        const size_t d = line.find(kSep, q);
        if (d == std::string::npos) { f.push_back(line.substr(q)); break; }
        f.push_back(line.substr(q, d - q)); q = d + std::strlen(kSep);
    }
    return f;
}
inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}
inline bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

/** One beacon line of a card, as the card wrote it. */
struct Beacon {
    std::string ident, name, compass;
    double hz = 0, km = -1, best = -99;
    std::string key() const { return ident + "@" + std::to_string((long long)std::lround(hz / 500.0)); }   // ★ the writer's map key
};
/** `day`: the day's log (9 fields), else a summary card (8). False: not a beacon line. */
inline bool parse(const std::string& line, bool day, Beacon& b) {
    b = Beacon();
    const std::vector<std::string> f = fields(line);
    if (f.size() != (day ? 9u : 8u)) return false;
    const size_t o = day ? 2 : 0;   // ident's field
    b.ident = trim(f[o]);
    b.hz = std::atof(f[o + 1].c_str()) * 1e3;
    if (b.ident.empty() || b.hz <= 0) return false;
    const std::string best = day ? f[4] : (startsWith(f[3], "best ") ? f[3].substr(5) : f[3]);
    b.best = std::atof(best.c_str());
    const std::string& dist = day ? f[5] : f[4];
    const std::string& brg = day ? f[6] : f[5];
    const std::string& st = day ? f[8] : f[7];
    if (dist != "-") { b.km = std::atof(dist.c_str()); const size_t sp = brg.find(' '); if (sp != std::string::npos) b.compass = brg.substr(sp + 1); }
    b.name = startsWith(st, "(") ? std::string() : st;   // "(not in the published list)", "(published, no name)"
    return true;
}
inline bool garbage(const std::string& line, bool day, Beacon& b) {
    if (!parse(line, day, b)) return false;
    const std::vector<std::string> f = fields(line);
    // ★ A line with a DISTANCE matched the published list (whatever its station field says — an older writer printed a
    //   published beacon with no name as "(not in the published list)"): never moved.
    return vibendb::uniformIdent(b.ident) && f.back() == kUnlisted && b.km < 0;
}
/** The writer's pick: the first in key order that is strictly greater. */
inline const Beacon* furthest(const std::vector<Beacon>& v) {
    std::map<std::string, const Beacon*> m; for (const auto& b : v) m.emplace(b.key(), &b);
    const Beacon* f = nullptr; for (const auto& kv : m) if (kv.second->km >= 0 && (!f || kv.second->km > f->km)) f = kv.second;
    return f;
}
inline const Beacon* bestOf(const std::vector<Beacon>& v) {
    std::map<std::string, const Beacon*> m; for (const auto& b : v) m.emplace(b.key(), &b);
    const Beacon* f = nullptr; for (const auto& kv : m) if (!f || kv.second->best > f->best) f = kv.second;
    return f;
}
inline std::string jsonUnesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c; }
        else o += s[i];
    }
    return o;
}

/** What one card becomes. */
struct Result {
    bool changed = false;
    std::string body, title;
    std::vector<std::string> moved;   // the idents moved, one per line
};

/** ★ One card's text (and its index title) → the cleaned card. Unchanged (changed = false) when nothing is moved. */
inline Result card(const std::string& text, const std::string& oldTitle) {
    Result r;
    std::vector<std::string> lines; {
        size_t p = 0;
        while (p < text.size()) { const size_t e = text.find('\n', p); if (e == std::string::npos) { lines.push_back(text.substr(p)); break; } lines.push_back(text.substr(p, e - p)); p = e + 1; }
    }
    const bool endsNl = !text.empty() && text.back() == '\n';
    long head = -1; bool day = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (startsWith(lines[i], "Each beacon, most recently heard first")) { head = (long)i; day = true; break; }
        if (startsWith(lines[i], "Each beacon, lowest frequency first")) { head = (long)i; day = false; break; }
    }
    if (head < 0) return r;
    size_t aside = lines.size();
    for (size_t i = (size_t)head + 1; i < lines.size(); i++) if (lines[i] == kSetAsideHeading) { aside = i; break; }
    std::vector<std::string> kept, moved; std::vector<Beacon> keptB; std::set<std::string> movedIds;
    for (size_t i = (size_t)head + 1; i < aside; i++) {
        Beacon b;
        if (garbage(lines[i], day, b)) { moved.push_back(lines[i]); movedIds.insert(b.ident); r.moved.push_back(b.ident); continue; }
        kept.push_back(lines[i]);
        if (parse(lines[i], day, b)) keptB.push_back(b);
    }
    if (moved.empty()) return r;
    while (!kept.empty() && kept.back().empty()) kept.pop_back();   // the blank line before an existing section

    std::vector<std::string> pre(lines.begin(), lines.begin() + head + 1);
    const size_t n = keptB.size();
    char b[400];
    std::string farIdent; double farKm = -1;
    std::string bestIdent; double bestDb = 0; bool haveBest = false;
    if (day) {
        // "128 beacons identified · furthest: PIS Pisarovina 1425 km"
        std::string date;
        if (!pre.empty() && startsWith(pre[0], "NDB heard log")) { const auto f = fields(pre[0]); if (f.size() >= 2) date = f[1].substr(0, f[1].find(' ')); }
        for (auto& l : pre) {
            const size_t at = l.find(" identified");
            if (at == std::string::npos || l.empty() || l[0] < '0' || l[0] > '9') continue;
            std::string fIdent, fName; double fKm = -1;
            const std::string fk = std::string(kSep) + "furthest: ";
            const size_t fp = l.find(fk);
            if (fp != std::string::npos) {
                const std::string rest = l.substr(fp + fk.size());
                const size_t sp = rest.find(' ');
                fIdent = rest.substr(0, sp);
                const size_t kmAt = rest.rfind(" km");
                const size_t numAt = kmAt == std::string::npos ? std::string::npos : rest.rfind(' ', kmAt - 1);
                if (sp != std::string::npos && numAt != std::string::npos && numAt >= sp) { fName = rest.substr(sp + 1, numAt - sp - 1); fKm = std::atof(rest.c_str() + numAt + 1); }
            }
            if (!fIdent.empty() && movedIds.count(fIdent)) {
                fIdent.clear(); fName.clear(); fKm = -1;
                if (const Beacon* f = furthest(keptB)) { fIdent = f->ident; fName = f->name; fKm = f->km; }
            }
            l = std::to_string(n) + (n == 1 ? " beacon" : " beacons") + " identified";
            if (!fIdent.empty() && fKm >= 0) { std::snprintf(b, sizeof b, "%sfurthest: %s %s %.0f km", kSep, fIdent.c_str(), fName.c_str(), fKm); l += b; }
            farIdent = fIdent; farKm = fKm;
            break;
        }
        r.title = "NDB" + std::string(kSep) + date + kSep + std::to_string(n) + (n == 1 ? " beacon heard" : " beacons heard");
        if (!farIdent.empty() && farKm >= 0) { std::snprintf(b, sizeof b, "%sfurthest %s %.0f km", kSep, farIdent.c_str(), farKm); r.title += b; }
        if (date.empty()) r.title = oldTitle;   // ★ not the writer's shape: the title is left as it was
    } else {
        std::vector<std::string> out;
        for (const auto& l : pre) {
            if (startsWith(l, "Beacons heard:")) { std::snprintf(b, sizeof b, "Beacons heard:     %zu", n); out.push_back(b); continue; }
            if (startsWith(l, "Furthest:")) {
                const std::string rest = trim(l.substr(9));
                if (startsWith(rest, "(")) { out.push_back(l); continue; }   // "(this server has no position set …)"
                const std::string id = rest.substr(0, rest.find(' '));
                if (!movedIds.count(id)) {
                    farIdent = id;
                    const size_t d = rest.rfind(kDash);
                    if (d != std::string::npos) farKm = std::atof(rest.c_str() + d + std::strlen(kDash));
                    out.push_back(l); continue;
                }
                if (const Beacon* f = furthest(keptB)) {
                    farIdent = f->ident; farKm = f->km;
                    std::snprintf(b, sizeof b, "Furthest:          %s %s%s%.0f km %s", f->ident.c_str(), f->name.c_str(), kDash, f->km, f->compass.c_str());
                    out.push_back(b);
                }
                continue;   // ★ no beacon left with a distance: the writer would have written no line
            }
            if (startsWith(l, "Best SNR:")) {
                const std::string rest = trim(l.substr(9));
                const std::string id = rest.substr(0, rest.find(' '));
                if (!movedIds.count(id)) {
                    bestIdent = id; haveBest = true;
                    const size_t sp = rest.find(' ');
                    if (sp != std::string::npos) bestDb = std::atof(rest.c_str() + sp + 1);
                    out.push_back(l); continue;
                }
                if (const Beacon* f = bestOf(keptB)) {
                    bestIdent = f->ident; bestDb = f->best; haveBest = true;
                    std::snprintf(b, sizeof b, "Best SNR:          %s %.0f dB", f->ident.c_str(), f->best);
                    out.push_back(b);
                }
                continue;
            }
            out.push_back(l);
        }
        pre.swap(out);
        // the title: its own beginning ("NDB 19:00–20:00 UTC", "NDB last 10 min to 20:20 UTC") + the writer's headline()
        const size_t cut = oldTitle.find(kSep);
        if (cut == std::string::npos) r.title = oldTitle;
        else {
            r.title = oldTitle.substr(0, cut) + kSep + std::to_string(n) + (n == 1 ? " beacon" : " beacons");
            if (!farIdent.empty() && farKm >= 0) { std::snprintf(b, sizeof b, "%sfurthest %s %.0f km", kSep, farIdent.c_str(), farKm); r.title += b; }
            if (haveBest) { std::snprintf(b, sizeof b, "%sbest %s %.0f dB", kSep, bestIdent.c_str(), bestDb); r.title += b; }
        }
    }
    std::string o;
    for (const auto& l : pre) { o += l; o += '\n'; }
    for (const auto& l : kept) { o += l; o += '\n'; }
    o += '\n'; o += kSetAsideHeading; o += '\n';
    for (size_t i = aside + 1; i < lines.size(); i++) { if (i + 1 == lines.size() && lines[i].empty() && endsNl) break; if (!lines[i].empty()) { o += lines[i]; o += '\n'; } }
    for (const auto& l : moved) { o += l; o += '\n'; }
    r.body = o; r.changed = true;
    return r;
}

} // namespace clean

/** What a pass did — for the log line and the tests. */
struct CleanStats {
    bool ran = false;        ///< false: already done (the marker), another process is on it, or no gallery
    size_t cards = 0, changed = 0, lines = 0, skipped = 0, failed = 0;
    std::map<std::string, size_t> idents;   ///< each ident moved, and how many lines of it
};

/** ★★ The pass. Cheap when done (one stat of the marker); `force` ignores the marker (tests, a support request). */
inline CleanStats cleanUniformIdents(gallery::Store& s, bool force = false) {
    CleanStats st;
    const std::string dir = s.dir();
    if (dir.empty() || !s.enabled()) return st;   // ★ saving is off: nothing may be rewritten — and not marked done
    const std::string marker = dir + "/" + kCleanMarker;
    struct stat sb{};
    if (!force && ::stat(marker.c_str(), &sb) == 0) return st;
    const int lk = ::open((dir + "/.ndb-uniform-clean.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0) return st;
    if (::flock(lk, LOCK_EX | LOCK_NB) != 0) { ::close(lk); return st; }   // ★ another radio's process is doing it
    if (!force && ::stat(marker.c_str(), &sb) == 0) { ::flock(lk, LOCK_UN); ::close(lk); return st; }
    st.ran = true;
    const std::set<std::string> open = s.heldOpen();
    for (const std::string& name : s.names("ndb")) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) continue;   // ★ the maps are pictures
        if (open.count(name)) { st.skipped++; continue; }
        std::string text;
        if (!s.readItem(name, text, 1u << 20)) continue;
        st.cards++;
        const clean::Result r = clean::card(text, clean::jsonUnesc(s.field(name, "title")));
        if (!r.changed) continue;
        if (!s.rewrite(name, r.title, r.body)) { st.failed++; continue; }
        st.changed++; st.lines += r.moved.size();
        for (const auto& id : r.moved) st.idents[id]++;
    }
    if (st.skipped == 0 && st.failed == 0) {   // ★ done: the marker (else the next start finishes the job)
        const int m = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (m >= 0) {
            char b[160]; const int k = std::snprintf(b, sizeof b, "done %lld: %zu cards read, %zu cleaned, %zu lines set aside\n", (long long)gallery::nowUtc(), st.cards, st.changed, st.lines);
            if (k > 0 && ::write(m, b, (size_t)k) < 0) {}
            ::close(m);
        }
    }
    std::fprintf(stderr, "[gallery] NDB clean-up: %zu cards read, %zu cleaned, %zu lines of all-dot / all-dash unlisted idents set aside%s\n",
                 st.cards, st.changed, st.lines, st.skipped || st.failed ? " (some cards busy — again at the next start)" : "");
    ::flock(lk, LOCK_UN); ::close(lk);
    return st;
}

} // namespace ndbgal
} // namespace vibe
