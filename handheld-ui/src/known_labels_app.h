// known_labels_app.h — the SIGNAL LABELS wired into the main screen (known_labels.h is the port; this is the R36S's
// plumbing): where the list comes from, when it is rebuilt, and the one call drawOverlays makes.
//
// ★★ THE LIST is rebuilt only when one of its sources changes — the receiver's bookmarks (bmShared().seq), the EiBi
//    schedule (stationsSeq, e.g. after IMPORT EIBI) or the ten-minute EiBi clock (the app re-reads which utility rows
//    are on air every ten minutes too) — and it is built on a WORKER (hwAsync): the 9000-row schedule is filtered there,
//    never in the draw loop. The UI thread only swaps the finished list in.
// ★ THE SOURCES are fetched only while the labels are ON: GET /bookmarks at most once a minute (FM names are learnt
//   while you listen), GET /stations once — after that only when SEARCH or IMPORT EIBI fetches it anyway. Off = nothing
//   fetched, built, laid out or drawn.
#pragma once
#include "bm_state.h"
#include "disp_settings.h"
#include "known_labels.h"
#include "tuned_label.h"

#include <ctime>
#include <memory>
#include <mutex>

namespace hh {

struct KnownLabelsUi {
    KnownLabels k;
    bool on = false;
    uint64_t bmSeq = ~0ull, stSeq = ~0ull;
    int bucket = -1;                 // UTC minutes / 10 the list was built for
    double fetchAt = -1e9;
    // the worker's hand-over
    std::mutex m;
    bool busy = false, ready = false;
    std::vector<KnownSig> built;
    // the worker's own (one worker at a time — `busy`): EiBi's labels, kept while the schedule and the ten minutes stand,
    // so a new bookmark list (every GET /bookmarks bumps its seq) re-does only the cheap half
    std::vector<KnownSig> eibiCache;
    uint64_t eibiSeq = ~0ull;
    int eibiBucket = -1;
    // ★★ V12 THE DX CATCHES ON THE DIAL (known_labels.h dxDialSignals — the app's and the web's rule): the engine's
    //    GET /vibeserver/dxlog.json?dial=1 — fetched on a worker while the labels are on, once and then every ten minutes
    //    (the scanner sweeps only while nobody listens); ≤ DX_DIAL_MAX entries kept. A 403 (the owner's "admin lock on
    //    viewing") or an engine without a DX Log (404) = none. Under `m`.
    std::vector<DxDialIn> dx;
    double dxNow = 0;                // the engine's clock in its answer (unix s) …
    std::time_t dxAt = 0;            // … and this clock then — the labels' "2 h ago" runs on the engine's clock
    uint64_t dxSeq = 0, dxBuiltSeq = ~0ull;
    double dxFetchAt = -1e9;
    bool dxBusy = false;
    int dxMinute = -1;               // the minute the list's "… ago" was worked out for
};
inline KnownLabelsUi& knownLabelsUi() { static KnownLabelsUi u; return u; }

/** Fetch the DX catches (a worker). */
inline void knownLabelsFetchDx(int port) {
    KnownLabelsUi& u = knownLabelsUi();
    { std::lock_guard<std::mutex> lk(u.m); if (u.dxBusy) return; u.dxBusy = true; }
    hwAsync([port] {
        KnownLabelsUi& u = knownLabelsUi();
        const HttpResult r = httpRequest(port, "GET", "/vibeserver/dxlog.json?dial=1&limit=100", "", 10);
        double pn = 0;
        std::vector<DxDialIn> v;
        if (r.status == 200) v = dxDialParse(r.body, &pn);
        std::lock_guard<std::mutex> lk(u.m);
        u.dxBusy = false;
        if (r.status == 0) return;   // no answer: keep what we had, try at the next round
        u.dx = std::move(v); u.dxNow = pn; u.dxAt = std::time(nullptr); u.dxSeq++;
    });
}

/** ★★ V12 the tuned station (tuned_label_app.h, after this file): which placed label names it, −1 = none. */
inline int tunedPickNow(Ctx& c, const RadioState& s, const std::vector<PlacedLabel>& placed);

inline int knownUtcMinutes() {
    const std::time_t t = std::time(nullptr);
    std::tm g{};
    gmtime_r(&t, &g);
    return g.tm_hour * 60 + g.tm_min;
}

/** Build the list on a worker from the shared bookmark + schedule state (copied under its lock, filtered first).
 *  False = a build is still running (the caller tries again next frame). */
inline bool knownLabelsBuild(int nowMin, uint64_t stSeq) {
    KnownLabelsUi& u = knownLabelsUi();
    { std::lock_guard<std::mutex> lk(u.m); if (u.busy) return false; u.busy = true; }
    hwAsync([nowMin, stSeq] {
        KnownLabelsUi& u = knownLabelsUi();
        const bool redoEibi = stSeq != u.eibiSeq || nowMin / 10 != u.eibiBucket;
        std::vector<EibiIn> eibi;
        std::vector<BookmarkIn> bms;
        {
            BmShared& b = bmShared();
            std::lock_guard<std::mutex> lk(b.m);
            // ★ only the rows that can be labels leave the lock (a few hundred of ~9000): utility rows, not broadcasts
            if (redoEibi) for (const StationRow& r : b.stations)
                if (eibiClass(r.name, r.comment).dec) eibi.push_back({r.name, r.comment, r.frequency});
            bms.reserve(b.rows.size());
            for (const BmRow& r : b.rows) {
                BookmarkIn o;
                o.name = r.name; o.mode = r.mode; o.frequency = r.frequency; o.manual = r.manual ? 1 : 0;
                o.nameSrc = r.nameSrc; o.sid = r.sid; o.repeater = r.repeater;
                bms.push_back(std::move(o));
            }
        }
        if (redoEibi) { u.eibiCache = eibiKnownSignals(eibi, nowMin); u.eibiSeq = stSeq; u.eibiBucket = nowMin / 10; }
        const std::vector<KnownSig> bmSigs = bookmarkSignals(bms);
        std::vector<KnownSig> list = knownSignalList(u.eibiCache, bmSigs);
        // ★★ V12 + the DX catches, clear of every channel those labels — or any bookmark at all — hold
        std::vector<DxDialIn> dx; double dxNow;
        { std::lock_guard<std::mutex> lk(u.m); dx = u.dx; dxNow = u.dxNow > 0 ? u.dxNow + (double)(std::time(nullptr) - u.dxAt) : (double)std::time(nullptr); }
        if (!dx.empty()) {
            std::vector<double> bmHz;
            for (const BookmarkIn& b : bms) bmHz.push_back(b.frequency);
            const std::vector<KnownSig> dxs = dxDialSignals(std::move(dx), dxNow, list, bmHz);
            if (!dxs.empty()) list = knownSignalList(u.eibiCache, bmSigs, dxs);
        }
        std::lock_guard<std::mutex> lk(u.m);
        u.built = std::move(list); u.ready = true; u.busy = false;
    });
    return true;
}

/** The labels for this frame: called from drawOverlays with the spectrum's rect. */
inline void knownLabelsDraw(App& a, Ctx& c, const RadioState& s, Rect spec) {
    KnownLabelsUi& u = knownLabelsUi();
    // ★ off, or the DAB page's thin strip (no room for two rows — as the dB grid), or no view: nothing at all
    if (!dispSet().knownLabels || spec.h < 60 || !(s.spanHz > 0)) {
        if (u.on) { u.k.clear(); u.on = false; u.bmSeq = u.stSeq = ~0ull; u.bucket = -1; u.dxFetchAt = -1e9; u.dxBuiltSeq = ~0ull; }
        return;
    }
    u.on = true;
    const double now = nowSec();
    if (a.eng_ && now - u.fetchAt > 60) {
        u.fetchAt = now;
        const int port = a.eng_->port();
        bmRefresh(port);
        bool haveStations;
        { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); haveStations = b.stationsSeq > 0; }
        if (!haveStations) bmLoadStations(port);
    }
    if (a.eng_ && now - u.dxFetchAt > 600) { u.dxFetchAt = now; knownLabelsFetchDx(a.eng_->port()); }   // ★★ V12 the DX catches
    // a source changed → rebuild (on the worker)
    uint64_t bs, ss;
    { BmShared& b = bmShared(); std::lock_guard<std::mutex> lk(b.m); bs = b.seq; ss = b.stationsSeq; }
    const int nowMin = knownUtcMinutes(), bucket = nowMin / 10;
    // ★★ V12 a new DX list, or (while any are held) a new minute for their "… ago"
    uint64_t dxSeq; bool dxAny;
    { std::lock_guard<std::mutex> lk(u.m); dxSeq = u.dxSeq; dxAny = !u.dx.empty(); }
    const bool dxDue = dxSeq != u.dxBuiltSeq || (dxAny && nowMin != u.dxMinute);
    if ((bs != u.bmSeq || ss != u.stSeq || bucket != u.bucket || dxDue) && knownLabelsBuild(nowMin, ss)) {
        u.bmSeq = bs; u.stSeq = ss; u.bucket = bucket; u.dxBuiltSeq = dxSeq; u.dxMinute = nowMin;
    }
    {
        std::lock_guard<std::mutex> lk(u.m);
        if (u.ready) { u.k.setList(std::move(u.built)); u.built.clear(); u.ready = false; }
    }
    // until the first build lands, the curated table alone (it needs no data)
    if (u.k.list().empty()) u.k.setList(std::vector<KnownSig>(knownTable()));
    const double lo = s.viewCentreHz - s.spanHz / 2;
    const float nx = spec.x + (float)((s.vfoHz - lo) / s.spanHz) * spec.w;
    const float needle = s.vfoHz > 0 && nx >= spec.x && nx <= spec.r() ? nx : -1;
    u.k.layout(c.tc, c.f.tiny, spec, lo, s.spanHz, s.vfoHz, needle);
    // ★★ V12 the label naming the station you are tuned to is drawn in the tuned label's colour (tuned_label_app.h)
    const int hi = tunedPickNow(c, s, u.k.placed());
    u.k.draw(c.g, c.tc, c.f.tiny, spec, lo, s.spanHz, hi, hi >= 0 ? rgb(tl::stampColour(dispSet().needle, a.wfp_.palette().rgba)) : Rgba{});
}

}  // namespace hh
