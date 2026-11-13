// search_page.h — the SEARCH page: web/client/src/search.ts search() and main.ts initSearch, ported (inventory §2.5).
//
// WHAT IT SEARCHES, IN THE WEB'S ORDER (search.ts: "server first, EiBi as fallback, band plan ALWAYS included"):
//   · a typed FREQUENCY is a result of its own — "Tune 7.150 MHz" (parseFreq: "100.7", "7150k", "14.074 MHz"; a bare
//     number < 30 is MHz, < 30000 kHz, else Hz)
//   · the RECEIVER's bookmarks (GET /bookmarks): name — "Saved on the receiver" / "Heard by this receiver"
//   · the EiBi schedule the engine holds (GET /stations): name, comment (time · language)
//   · the BAND PLAN (bandplan_gen.h): name, type (ham / broadcast / utility), short label ("40m") — tuning to a band
//     parks in its middle with its mode
//   ★ The web's YOUR BOOKMARKS source (browser-local) has no native counterpart: this UI's bookmarks ARE the receiver's
//     (bm_page.h), so they are searched once, as the receiver's.
// RANKING: case-blind substring match; the sources' order is the rank (user · server · EiBi · band — stable, so within a
//   source the order found), at most 200 results; EiBi's scan stops past 800 hits (search.ts "bound the scan").
// ★ FILTERED LAST to what this radio can reach (search.ts tunableWindow): a result outside the radio's ranges is not
//   offered — the web applies the same rule to a locked receiver's window, for the same reason ("offering results you
//   cannot tune to is the same lie"); a band that merely overlaps is kept and re-pointed inside. Native difference: the
//   ranges here are hwinfo's (radio.allowed, else radio.ranges), so an RTL without HF sees no shortwave rows.
// ★ The band plan is searched for THIS ITU region only (the web lists every region's copy of a band, so "AM Broadcast"
//   came back twice with different edges).
// ★ DAB rows are left out (no DAB here — bm_page.h).
// SELECTING one = main.ts tuneTo (bmTuneTo): its mode, else the band's, else the current one; its passband; the band step.
// LIVE AS YOU TYPE: the keyboard shows the count and the first results under the field (drawSearchLive), and the page
//   behind it is already showing them when ↵ closes it.
// STICKY (main.ts SEARCH_STICKY_MS): the results survive the station you just tried, for 90 s after you last used them.
#pragma once
#include "bm_page.h"

namespace hh {

struct SearchHit {
    std::string name, detail, itu, mode;
    double frequency = 0;
    int source = 3;               // 0 user · 1 server · 2 eibi · 3 band (search.ts ResultSource rank)
    bool heard = false;           // server: heard over RDS (else saved)
    bool hasBw = false; int bwLo = 0, bwHi = 0;
    double bandLo = 0, bandHi = 0;   // band: its edges (for the reach filter)
};

struct SearchUi {
    std::string query;            // what the page's field holds
    std::vector<SearchHit> hits;
    std::string hitsFor; uint64_t bmSeq = ~0ull, stSeq = ~0ull; size_t rangesN = ~(size_t)0;
    double usedAt = 0;            // last tuned from (0 = never: still the search you are typing)
    Scroller scroll;
    // the keyboard's live preview
    std::string liveFor; std::vector<SearchHit> live; size_t liveN = 0;
};
inline SearchUi& searchUi() { static SearchUi u; return u; }

/** search.ts parseFreq. 0 when not a frequency. */
inline double searchParseFreq(const std::string& qIn) {
    std::string q = hwTrim(qIn);
    size_t i = 0;
    while (i < q.size() && (std::isdigit((unsigned char)q[i]) || q[i] == '.')) i++;
    if (i == 0) return 0;
    const std::string num = q.substr(0, i);
    std::string unit = q.substr(i);
    unit.erase(0, unit.find_first_not_of(" \t"));
    unit = bmLower(unit);
    if (!(unit.empty() || unit == "k" || unit == "khz" || unit == "m" || unit == "mhz" || unit == "hz")) return 0;
    if (std::count(num.begin(), num.end(), '.') > 1) return 0;   // parseFloat("1.2.3") is 1.2 — the web would take it;
    const double n = std::atof(num.c_str());                    // a mistyped dot is better refused than guessed
    if (!(n > 0) || !std::isfinite(n)) return 0;
    if (unit[0] == 'k') return std::round(n * 1e3);
    if (unit[0] == 'm') return std::round(n * 1e6);
    if (unit == "hz") return std::round(n);
    if (n < 30) return std::round(n * 1e6);
    if (n < 30000) return std::round(n * 1e3);
    return std::round(n);
}

/** search.ts search(query, 200), on this UI's sources. */
inline std::vector<SearchHit> searchRun(const std::string& query, const std::vector<std::pair<double, double>>& ranges, int region,
                                        size_t limit = 200) {
    std::vector<SearchHit> out;
    const std::string q = bmLower(hwTrim(query));
    if (q.empty()) return out;
    if (const double hz = searchParseFreq(q)) {
        SearchHit h; h.name = "Tune " + bmMhz(hz) + " MHz"; h.frequency = hz; h.source = 3; h.detail = "Direct entry";
        out.push_back(h);
    }
    auto hit = [&](const std::string& lower) { return !lower.empty() && lower.find(q) != std::string::npos; };
    {
        BmShared& b = bmShared();
        std::lock_guard<std::mutex> lk(b.m);
        // 2. the receiver's bookmarks — what this receiver can actually RECEIVE, so above EiBi
        for (auto& r : b.rows) {
            if (r.mode == "dab" || !hit(r.lname)) continue;
            SearchHit h; h.name = r.name; h.frequency = r.frequency; h.mode = r.mode; h.source = 1; h.heard = !r.manual;
            h.detail = r.manual ? "Saved on the receiver" : "Heard by this receiver";
            h.hasBw = r.hasBw; h.bwLo = r.bwLo; h.bwHi = r.bwHi;
            out.push_back(std::move(h));
        }
        // 3. the EiBi schedule — what EXISTS
        for (auto& s : b.stations) {
            if (!hit(s.lname) && !hit(s.lcomment)) continue;
            SearchHit h; h.name = s.name; h.frequency = s.frequency; h.mode = s.mode; h.source = 2; h.detail = s.comment; h.itu = s.itu;
            out.push_back(std::move(h));
            if (out.size() > limit * 4) break;                       // bound the scan; EiBi is ~10k rows
        }
    }
    // 4. the band plan — ALWAYS searched
    static const char* kType[] = {"ham", "broadcast", "utility"};
    for (int i = 0; i < kBandPlanN; i++) {
        const BandDef& b = kBandPlan[i];
        if (b.regions && region && !(b.regions & (1 << region))) continue;
        const char* ty = b.type >= 0 && b.type < 3 ? kType[b.type] : "";
        if (!hit(bmLower(b.name)) && !hit(ty) && !hit(bmLower(b.label))) continue;
        SearchHit h; h.name = b.name; h.frequency = std::round((b.lo + b.hi) / 2); h.mode = b.mode; h.source = 3;
        h.detail = std::string(ty) + " · " + bmMhz(b.lo) + "–" + bmMhz(b.hi) + " MHz";
        h.bandLo = b.lo; h.bandHi = b.hi;
        out.push_back(std::move(h));
    }
    std::stable_sort(out.begin(), out.end(), [](const SearchHit& a, const SearchHit& b) { return a.source < b.source; });
    if (ranges.empty()) { if (out.size() > limit) out.resize(limit); return out; }
    // ★ filter LAST (search.ts): every source narrowed by the same rule
    std::vector<SearchHit> kept;
    for (auto& r : out) {
        bool in = false;
        for (auto& w : ranges) if (r.frequency >= w.first && r.frequency <= w.second) in = true;
        if (in) { kept.push_back(std::move(r)); }
        else if (r.bandHi > 0) {
            for (auto& w : ranges) if (r.bandHi >= w.first && r.bandLo <= w.second) {
                r.frequency = std::round((std::max(r.bandLo, w.first) + std::min(r.bandHi, w.second)) / 2);
                kept.push_back(std::move(r));
                break;
            }
        }
        if (kept.size() >= limit) break;
    }
    return kept;
}

inline const char* searchSrcLabel(const SearchHit& h) {
    return h.source == 1 ? (h.heard ? "HEARD" : "SAVED") : h.source == 2 ? "EiBi" : h.source == 3 ? "BAND" : "";
}

/** Recompute when the query, a source or the reach changed. */
inline void searchRefresh(SearchUi& u, const std::string& q, const RadioState& s, int region, std::vector<SearchHit>& into,
                          std::string& doneFor) {
    uint64_t bs, ss;
    { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); bs = b.seq; ss = b.stationsSeq; }
    if (doneFor == q && u.bmSeq == bs && u.stSeq == ss && u.rangesN == s.ranges.size()) return;
    into = searchRun(q, s.ranges, region);
    doneFor = q;
    u.bmSeq = bs; u.stSeq = ss; u.rangesN = s.ranges.size();
}

inline void searchAskQuery(App& a) {
    SearchUi& u = searchUi();
    KbRequest r;
    r.title = "SEARCH";
    r.opts.mode = kb::KbMode::search;
    r.opts.value = u.query;
    r.placeholder = "Search stations, bands, frequency…";
    r.commit = [&a](const kb::KbState& st) -> std::string {
        SearchUi& su = searchUi();
        su.query = bmClean(st.valueUtf8(), 80);
        su.usedAt = 0;                                    // a new query is a new hunt, not a stale one
        su.scroll = {};
        a.nav_.setFocus("search", "s.r.0");               // the first result (Nav falls back to the field if none)
        return "";
    };
    a.openKeyboard(r);
}

inline void searchOpen(App& a) {
    SearchUi& u = searchUi();
    // ★ main.ts SEARCH_STICKY_MS: a list you tuned from is kept for 90 s after you last used it; one never tuned from is
    //   still the search you are typing
    if (u.usedAt > 0 && nowSec() - u.usedAt > 90) { u.query.clear(); u.hits.clear(); u.hitsFor.clear(); u.usedAt = 0; }
    else if (u.usedAt > 0) u.usedAt = nowSec();
    u.scroll = {};
    a.nav_.forget("search");
    a.open(App::S_SEARCH);
    if (a.eng_) { bmRefresh(a.eng_->port()); bmLoadStations(a.eng_->port()); }
    if (u.query.empty()) { a.nav_.setFocus("search", "s.field"); searchAskQuery(a); }
    else a.nav_.setFocus("search", "s.r.0");
}

/** One result row (the web's .sres: frequency · flag + name + detail · source). */
inline void searchRow(Ctx& c, const SearchHit& h, Rect row, bool focused, bool compact) {
    if (!compact) {
        c.g.rrect(row.x, row.y, row.w, row.h, 7, focused ? th::activeBg : th::btnBg);
        c.g.rframe(row.x, row.y, row.w, row.h, 7, 1, focused ? th::activeBdr : th::btnBorder);
    }
    const Font& f = compact ? c.f.small : c.f.body;
    const float fw = (float)c.tc.widthTab(f, "8888.888") + 10, srcW = compact ? 50 : 64;
    const float ty = row.cy() - f.lineHeight() / 2.f;
    c.tc.drawTab(c.g, f, bmMhz(h.frequency), row.x + (compact ? 0 : 10), ty, th::white);
    float x = row.x + (compact ? 0 : 10) + fw;
    const float right = row.r() - srcW - 8;
    const int nameMax = (int)(right - x);
    const std::string nm = bmFit(f, h.name, nameMax);
    x += c.tc.draw(c.g, f, nm, x, ty, th::white) + 10;
    if (!h.detail.empty() && right - x > 40) c.tc.draw(c.g, c.f.small, bmFit(c.f.small, h.detail, (int)(right - x)), x, row.cy() - c.f.small.lineHeight() / 2.f, th::dim);
    c.tc.draw(c.g, c.f.small, searchSrcLabel(h), row.r() - (compact ? 0 : 10), row.cy() - c.f.small.lineHeight() / 2.f, h.source == 2 ? th::dim : th::btnText, 2);
}

inline void App::drawSearch(Ctx& c, const RadioState& s) {
    SearchUi& u = searchUi();
    bool closed = false;
    const Rect panel{12, 10, 616, 460};
    const Rect area = sheetPanel(c, panel, "SEARCH", "s.close", closed);
    if (closed) { closeTop(); return; }
    searchRefresh(u, u.query, s, region(), u.hits, u.hitsFor);
    float y = area.y;
    // the field (hw_pump.h's hwRowField look: black, a white hairline, the placeholder dimmed)
    {
        const Rect fr{area.x, y, area.w, 48};
        const Nav::Hit h = c.nav.item("s.field", fr);
        c.g.rrect(fr.x, fr.y, fr.w, fr.h, 7, rgb(0x000000, 0.55f));
        c.g.rframe(fr.x, fr.y, fr.w, fr.h, 7, 1, rgb(0xffffff, 0.45f));
        c.g.circle(fr.x + 22, fr.cy() - 3, 8, 2.2f, th::dim);
        c.g.line(fr.x + 28, fr.cy() + 3, fr.x + 34, fr.cy() + 10, 2.6f, th::dim);
        textIn(c, c.f.body, u.query.empty() ? "Search stations, bands, frequency…" : bmFit(c.f.body, u.query, (int)fr.w - 60), fr,
               u.query.empty() ? th::dimmer : th::white, 0, 46);
        if (h.focused) focusRing(c, fr, 7);
        if (h.pressed) { searchAskQuery(*this); return; }
        y += 56;
    }
    // what there is to search
    int eibiN = -1; bool stLoading = false; size_t stN = 0;
    { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); eibiN = b.eibiEntries; stLoading = b.stationsLoading; stN = b.stations.size(); }
    {
        std::string line;
        if (u.query.empty()) line = "Stations, bookmarks, bands (\"40m\", \"broadcast\") or a frequency (\"7.150\", \"7150k\")";
        else line = std::to_string(u.hits.size()) + (u.hits.size() == 1 ? " result" : " results") + (u.hits.size() >= 200 ? " (the first 200)" : "");
        c.tc.draw(c.g, c.f.small, bmFit(c.f.small, line, (int)area.w - 4), area.x + 2, y, th::dim);
        y += c.f.small.lineHeight();
        std::string ei;
        if (stLoading && stN == 0) ei = "Loading the EiBi schedule…";
        else if (stN > 0) ei = "EiBi schedule: " + std::to_string(stN) + " stations";
        else if (eibiN == 0) ei = "No EiBi schedule — put " + eibiSeasonFile() + " (eibispace.de) in the card's EiBi folder, then BOOKMARKS → IMPORT EIBI";
        if (!ei.empty()) c.tc.draw(c.g, c.f.small, bmFit(c.f.small, ei, (int)area.w - 4), area.x + 2, y, stN > 0 ? th::dimmer : th::amber);
        y += c.f.small.lineHeight() + 8;
    }
    // the results
    const Rect view{area.x - 8, y, area.w + 16, panel.b() - 8 - y};
    c.g.clip(view.x, view.y, view.w, view.h);
    const float rowH = 46, gap = 4;
    float ly = view.y - u.scroll.y;
    for (size_t i = 0; i < u.hits.size(); i++, ly += rowH + gap) {
        if (ly + rowH < view.y - 2 * rowH || ly > view.b() + 2 * rowH) continue;
        const Rect row{area.x, ly, area.w, rowH};
        const Nav::Hit h = c.nav.item("s.r." + std::to_string(i), row);
        if (ly + rowH > view.y && ly < view.b()) {
            searchRow(c, u.hits[i], row, h.focused, false);
            if (h.focused) focusRing(c, row, 7);
        }
        if (h.pressed) {
            const SearchHit r = u.hits[i];
            u.usedAt = nowSec();
            c.g.unclip();
            bmTuneTo(*this, r.frequency, r.mode, r.hasBw, r.bwLo, r.bwHi);
            closeTop();
            return;
        }
    }
    c.g.unclip();
    u.scroll.contentH = (float)u.hits.size() * (rowH + gap);
    u.scroll.follow(c.nav.focusedRect(), view);
    if (nav_.focus("search").empty()) nav_.setFocus("search", "s.field");
}

/** Under the keyboard's field while SEARCH is being typed: the count and the first three, live (the web's results list
 *  opens under the box on every key). Draws nothing for any other keyboard. */
inline void App::drawSearchLive(Ctx& c, Rect r) {
    if (kbReq_.title != "SEARCH" || kbReq_.opts.mode != kb::KbMode::search) return;
    SearchUi& u = searchUi();
    const std::string q = bmClean(kbd_.state().valueUtf8(), 80);
    {
        static uint64_t bs0 = ~0ull, ss0 = ~0ull;
        uint64_t bs, ss;
        { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); bs = b.seq; ss = b.stationsSeq; }
        if (q != u.liveFor || bs != bs0 || ss != ss0) {
            u.live = searchRun(q, eng_->state().ranges, region());
            u.liveFor = q; bs0 = bs; ss0 = ss;
        }
    }
    if (q.empty()) return;
    const float lh = (float)c.f.small.lineHeight() + 2;
    float y = r.y;
    std::string head = u.live.empty() ? "No results" : std::to_string(u.live.size()) + (u.live.size() == 1 ? " result" : " results") + " — enter or START to see them";
    c.tc.draw(c.g, c.f.small, head, r.x, y, th::dim);
    y += lh;
    for (size_t i = 0; i < u.live.size() && y + lh <= r.b() + 1; i++, y += lh) searchRow(c, u.live[i], {r.x, y, r.w, lh}, false, true);
}

}  // namespace hh
