// sec_audio.h — the AUDIO sheet (#audioPanel) (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
// Spec: docs/v12/R36S-NATIVE-MENU-INVENTORY.md §3 (every row), §1.7 (SQL on the reading chip), §5.2 (Recordings).
//
// ★★ SPEAKER MODE (§3.0): every row here is an ENGINE message on the spectrum socket — the engine plays the audio, so
//    squelch, NR, notch, NB, de-emphasis and the broadcast-FM processing are all heard on the R36S speaker. The two
//    things the web page did itself are ported here: the AUTO SQUELCH tracker (it steers the engine's `squelch`) and
//    the RECORDER (the engine's speaker tap → a WAV → POST /handheld/recordings, audio_rec.h).
// ★★ TWO VOLUMES IN SERIES (§3.5): this sheet's VOLUME is the ENGINE's speaker level ({"type":"speaker","level"}),
//    linear, before ALSA. R36S HARDWARE → VOLUME is the codec (App::volume_ / muted_, the volume keys). Never mixed.
// ★★ THE ENGINE IS THE TRUTH: every DSP row paints the radio's hwinfo report, with the web's 4 s press guard
//    (setToggleTo) so a stale report never flips a button just pressed. Where the web has only a PREF (squelch, the
//    auto-squelch margin, volume, mute, REC BUFFER, NFM AUDIO's choice) the value lives here for this run — there is
//    no per-radio prefs store in the native UI yet.
// ★ AGENTS.md — a control that does nothing here is not drawn: SQUELCH is not on an Icom; AUTO SQUELCH needs the
//   server's `sig`; NOISE BLANKER is not for broadcast FM; NFM AUDIO is NFM only; the BROADCAST FM rows are WFM only;
//   AUTO BW only where hwinfo autoBwModes lists wfm. VFO B rows (§3.3, §3.4, §3.12) are left out: the native UI has
//   no VFO B yet.
#pragma once
// (included by app.h after App is declared)
#include "audio_rec.h"

namespace hh {

namespace aud {

// ── the squelch's scale and the auto tracker's constants (web/client/src/main.ts, squelchNeighbours.ts) ──
constexpr double SQL_OFF = -999;               // vibeSquelch.ts VS_SQL_OFF
constexpr double SQL_LEGACY_OFF_DB = -100;     // what goes out as `db` for OFF (an older server reads it the same)
constexpr double SCALE_MIN = -121, SCALE_MAX = -13;   // METER_MIN_DBFS … METER_MAX_DBFS
constexpr double AUTO_MARGIN_DEFAULT = 12, AUTO_HYST_DB = 3, AUTO_HANG_S = 0.7;
constexpr double AUTO_FALL = 0.20, AUTO_RISE = 0.002;  // per `sig` sample (20 a second)
constexpr double NEAR_CEIL_DB = 3, NEAR_SMOOTH = 0.1;
constexpr double PRESS_GUARD_S = 4;            // setToggleTo: a report is ignored for 4 s after a press of that control

inline bool sqlOn(double db) { return std::isfinite(db) && db > SQL_OFF; }

/** recordBuffer.ts retuneAllowanceHz. */
inline double retuneAllowanceHz(const std::string& m) {
    if (m == "cw" || m == "cwu" || m == "cwl") return 1000;
    if (m == "usb" || m == "lsb") return 3000;
    if (m == "am" || m == "sam" || m == "sau" || m == "sal" || m == "sas" || m == "drm") return 10000;
    if (m == "nfm" || m == "fm") return 15000;
    if (m == "wfm" || m == "wbfm") return 150000;
    if (m == "dab") return 0;
    return 5000;
}

/** recordBuffer.ts RecordBufferAnchor: has the held audio stopped being this audio (a retune beyond the mode's
 *  allowance from where the dial was when the oldest held audio began, or a mode change)?
 *  ★ The window is the buffer's length in SECONDS → ms. (The web constructs it with the seconds as the ms argument —
 *    `new RecordBufferAnchor(recBufSec)` against `constructor(windowMs)` — so there the anchor is ~30 ms old.) */
struct Anchor {
    std::vector<std::pair<double, double>> hist;   // (t s, Hz)
    std::string mode;
    bool started = false;
    double windowS = 0;
    void reset(double hz, const std::string& m, double t) { hist = {{t, hz}}; mode = m; started = true; }
    double anchorHz(double t) {
        const double edge = t - windowS;
        size_t i = 0;
        while (i + 1 < hist.size() && hist[i + 1].first <= edge) i++;
        if (i) hist.erase(hist.begin(), hist.begin() + (long)i);
        return hist.empty() ? NAN : hist[0].second;
    }
    bool update(double hz, const std::string& m, double t) {
        if (!started || m != mode) { reset(hz, m, t); return true; }
        const double a = anchorHz(t);
        if (std::fabs(hz - a) > retuneAllowanceHz(m)) { reset(hz, m, t); return true; }
        if (hist.empty() || hist.back().second != hz) {
            hist.push_back({t, hz});
            if (hist.size() > 512) hist.erase(hist.begin() + 1, hist.begin() + (long)(hist.size() - 256));
        }
        return false;
    }
};

/** The sheet's own state (the App is one per process). */
struct State {
    // squelch (VFO A)
    double applied = SQL_OFF;      // what the engine was last told (the web's squelchDb)
    double manual = SQL_OFF;       // the person's own threshold (the web's pref `squelch` + `squelchOn`)
    bool autoOn = false;
    double margin = AUTO_MARGIN_DEFAULT;
    double floorEwma = NAN, nearExcess = NAN, nearAt = 0, nearRef = NAN, openAt = -1e9, shown = NAN;
    std::string seedKey, nearKey;
    double lastTickAt = 0, lastChan = 1e9, lastFloor = 1e9;
    double lastAdjAt = 0;
    // the speaker
    int vol = 80;                  // main.ts:9806 — pref `volume` × 100, else 80
    bool mute = false;             // not persisted (as the web)
    bool wasConnected = false;
    int sentTap = -1;
    // the recorder
    arec::Recorder rec;
    int rbSec = 0;                 // ★ the handheld's default is OFF (main.ts:16787), and it is DRAWN off
    Anchor rbAnchor;
    double recPressAt = 0;
    // DSP presses (the 4 s guard): key → (time, value)
    std::map<std::string, std::pair<double, double>> press;
    int nfmPref = -1;              // NFM AUDIO: the listener's choice wins over the report (main.ts:2043-2052)
    double nfmResentAt = 0;
    // the RECORDINGS view
    bool recsView = false;
    bool recsLoaded = false;
    std::vector<Json> recs;
    std::string armedDel; double armedAt = 0;
    int uploadingSeen = 0;
    // ★ the sheet's own UP / DOWN order: rows of (id, centre x), top to bottom. ui.h's nearest-box rule is made for
    //   scattered controls; down a column of rows with controls at either end it skips rows (a centred slider over a
    //   left / right pair over a right-hand toggle). So UP / DOWN go to the next ROW, to its control nearest across;
    //   the correction is applied at the start of the next frame, before anything is drawn (no flicker).
    std::vector<std::vector<std::pair<std::string, float>>> navRows;
    std::string navFix;
};
inline State& st() { static State s; return s; }

inline std::string fmtDb(double v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v); return b; }
inline const char* jb(bool b) { return b ? "true" : "false"; }

/** vibeSquelch.ts vsSquelchMsg. */
inline void sendSquelch(App& a, double db) {
    const bool on = sqlOn(db);
    a.eng_->send(std::string("{\"type\":\"squelch\",\"db\":") + fmtDb(on ? db : SQL_LEGACY_OFF_DB) + ",\"on\":" + jb(on) + "}");
}
/** main.ts applySquelch. `persist` = it is the person's own threshold (an automatic one never overwrites it). */
inline void applySquelch(App& a, double db, bool persist) {
    State& S = st();
    S.applied = db;
    sendSquelch(a, db);
    if (persist) S.manual = db;
    a.num_["audio.sqlDb"] = S.applied;     // ★ for the deck: the SQL chip + #mSqlLine read the same figure (§1.4, §1.7)
}

/** main.ts sqlChannelKey: frequency | mode | passband. */
inline std::string channelKey(const RadioState& s) {
    char b[96];
    std::snprintf(b, sizeof b, "%lld|%s|%d|%d", (long long)std::llround(s.vfoHz), s.mode.c_str(), s.bwLow, s.bwHigh);
    return b;
}

/** main.ts autoSquelchTick — once per `sig` sample. */
inline void autoSquelchTick(App& a, const RadioState& s) {
    State& S = st();
    if (!S.autoOn || !s.last.count("sig")) return;
    const double now = nowSec();
    const double chan = s.chanDb;
    const std::string key = channelKey(s);
    if (key != S.seedKey) { S.seedKey = key; S.floorEwma = NAN; S.nearRef = NAN; }
    const double nearInst = (std::isfinite(S.nearExcess) && S.nearKey == key && now - S.nearAt < 1.0) ? chan - S.nearExcess : NAN;
    if (std::isfinite(nearInst)) {
        S.nearRef = std::isfinite(S.nearRef) ? S.nearRef + (nearInst - S.nearRef) * NEAR_SMOOTH : nearInst;
        if (!std::isfinite(S.floorEwma)) S.floorEwma = nearInst;
    }
    const bool openish = now - S.openAt < AUTO_HANG_S;
    const double k = chan < S.floorEwma ? AUTO_FALL : (openish ? 0 : AUTO_RISE);
    S.floorEwma = std::isfinite(S.floorEwma) ? S.floorEwma + (chan - S.floorEwma) * k : chan;
    if (std::isfinite(S.nearRef) && S.floorEwma > S.nearRef + NEAR_CEIL_DB) S.floorEwma = S.nearRef + NEAR_CEIL_DB;
    const double decide = S.floorEwma + S.margin - (openish ? AUTO_HYST_DB : 0);
    if (chan >= decide) S.openAt = now;
    const bool gateOpen = now - S.openAt < AUTO_HANG_S;
    S.shown = decide;
    a.num_["audio.sqlShown"] = decide;
    const double want = gateOpen ? SQL_OFF : std::round(std::min(SCALE_MAX, decide));
    if (std::isfinite(want) && want != S.applied) applySquelch(a, want, false);
}

/** main.ts setSquelchAuto. Off restores the remembered MANUAL threshold. */
inline void setSquelchAuto(App& a, const RadioState& s, bool on) {
    State& S = st();
    S.autoOn = on;
    a.num_["audio.sqlAuto"] = on ? 1 : 0;
    if (on) { S.floorEwma = NAN; S.nearRef = NAN; S.seedKey = channelKey(s); autoSquelchTick(a, s); }
    else { applySquelch(a, S.manual, false); a.num_.erase("audio.sqlShown"); }
}

/** The tap and the level, to the engine (main.ts sendSpeaker). */
inline void sendLevel(App& a) {
    const State& S = st();
    a.eng_->speakerLevel(S.mute ? 0.0 : S.vol / 100.0);
}
inline void syncTap(App& a, const RadioState& s) {
    State& S = st();
    const int want = S.rec.wantsTap() ? 1 : 0;
    if (!s.connected) { S.sentTap = -1; return; }
    if (want != S.sentTap) { a.eng_->send(std::string("{\"type\":\"speaker\",\"tap\":") + (want ? "1" : "0") + "}"); S.sentTap = want; }
}

/** Stop the recording and send it to the card. */
inline void stopRecording(App& a, const RadioState& s) {
    State& S = st();
    arec::Done d;
    const bool got = S.rec.stop(d);
    a.num_.erase("audio.recSec");
    // ★ the record buffer starts again at the stop (main.ts initRecorder): the next recording never repeats this one
    S.rec.clearBuffer();
    S.rbAnchor = Anchor{}; S.rbAnchor.windowS = S.rbSec;
    syncTap(a, s);
    if (got) arec::upload(a.eng_->port(), d, s.vfoHz, upper(s.mode));
}
inline void startRecording(App& a, const RadioState& s) {
    State& S = st();
    if (!S.rec.start()) { a.toast("Recording NOT saved · cannot write the temp file"); return; }
    a.num_["audio.recSec"] = S.rec.seconds();
    syncTap(a, s);
}

/** A DSP switch as the radio runs it: a press in the last 4 s wins, then the hwinfo report, then `def`. */
inline bool dspBool(const RadioState& s, const std::string& k, bool def) {
    const State& S = st();
    auto it = S.press.find(k);
    if (it != S.press.end() && nowSec() - it->second.first < PRESS_GUARD_S) return it->second.second != 0;
    const Json& h = s.msg("hwinfo");
    return h.has(k) && (h[k].kind == Json::Bool || h[k].kind == Json::Num) ? h[k].boolean() : def;
}
inline void pressed(const std::string& k, double v) { st().press[k] = {nowSec(), v}; }

/** main.ts nrStrength / nrPercent — exact inverses (the echo must never ratchet). */
inline double nrStrength(double pct) { const double p = std::max(0.0, std::min(100.0, pct)); return p <= 80 ? p / 80 : 1 + (p - 80) / 20 * 0.4; }
inline double nrPercent(double str) { const double v = std::max(0.0, std::min(1.4, str)); return v <= 1 ? v * 80 : 80 + (v - 1) / 0.4 * 20; }
inline int nrNow(const RadioState& s) {
    const State& S = st();
    auto it = S.press.find("nr");
    if (it != S.press.end() && nowSec() - it->second.first < PRESS_GUARD_S) return (int)it->second.second;
    const Json& h = s.msg("hwinfo");
    if (!h["nr"].boolean()) return 0;
    if (h["nrStrength"].kind == Json::Num) return std::max(1, (int)std::lround(nrPercent(h["nrStrength"].num())));
    return 50;    // (the web's saved strength, else 50 — there is no saved strength here)
}
inline int deemphNow(const RadioState& s) {
    const State& S = st();
    auto it = S.press.find("deemph");
    if (it != S.press.end() && nowSec() - it->second.first < PRESS_GUARD_S) return (int)it->second.second;
    const Json& h = s.msg("hwinfo");
    if (h["deemph"].kind == Json::Num) return (int)std::lround(h["deemph"].num() * 1e6);
    return 50;    // the markup's `on` segment
}
inline bool autoBwForWfm(const RadioState& s) {
    for (auto& m : s.msg("hwinfo")["autoBwModes"].a) if (m.str() == "wfm") return true;
    return false;
}

// ── drawing bits ──
inline void wrapText(Ctx& c, const std::string& t, float x, float& y, float w, Rgba col) {
    for (auto& l : c.tc.wrap(c.f.small, t, (int)w)) { c.tc.draw(c.g, c.f.small, l, x, y, col); y += c.f.small.lineHeight(); }
}
inline float wrapHeight(Ctx& c, const std::string& t, float w) { return (float)c.tc.wrap(c.f.small, t, (int)w).size() * c.f.small.lineHeight(); }
inline void rowLabel(Ctx& c, const std::string& t, float x, float cy) { c.tc.draw(c.g, c.f.bodyB, t, x, cy - c.f.bodyB.lineHeight() / 2.f, th::btnText); }
inline void speakerGlyph(Ctx& c, float cx, float cy, Rgba col, bool muted) {
    c.g.rect(cx - 9, cy - 4, 5, 8, col);
    c.g.triangle(cx - 4, cy - 4, cx + 3, cy - 10, cx + 3, cy + 10, col);
    c.g.triangle(cx - 4, cy - 4, cx + 3, cy + 10, cx - 4, cy + 4, col);
    if (muted) { c.g.line(cx + 6, cy - 5, cx + 14, cy + 5, 2, col); c.g.line(cx + 6, cy + 5, cx + 14, cy - 5, 2, col); }
    else for (int k = 0; k < 2; k++) {
        const float rr = 6 + k * 5;
        for (int i = -3; i < 3; i++) {
            const float a0 = i * 0.25f, a1 = (i + 1) * 0.25f;
            c.g.line(cx + 4 + std::cos(a0) * rr, cy + std::sin(a0) * rr, cx + 4 + std::cos(a1) * rr, cy + std::sin(a1) * rr, 1.6f, col);
        }
    }
}
inline void crossGlyph(Ctx& c, float cx, float cy, float r, Rgba col) {
    c.g.line(cx - r, cy - r, cx + r, cy + r, 2.2f, col);
    c.g.line(cx - r, cy + r, cx + r, cy - r, 2.2f, col);
}

}  // namespace aud

/** ★★ CALL ONCE A FRAME, sheet open or not (the auto squelch must keep tracking, the recorder must keep receiving, a
 *  reconnect must get the speaker level / tap / squelch again — main.ts pushSettingsToServer). */
inline void audioTick(App& a, const RadioState& s) {
    using namespace aud;
    State& S = st();
    if (!a.eng_) return;
    if (!a.eng_->onAudio) {
        // ★ the recording tap: every audio-socket message reaches here on the UI thread (Engine::pump)
        a.eng_->onAudio = [&a](bool text, const std::string& d) {
            if (text) return;
            if (st().rec.pcm(d)) { stopRecording(a, a.eng_->state()); a.toast("Recording stopped at 2 GB"); }
        };
    }
    // a (re)connect: the engine keeps no per-client state — tell it the level, the tap and the squelch again
    if (s.connected && !S.wasConnected) {
        sendLevel(a);
        S.sentTap = -1;
        if (sqlOn(S.applied)) sendSquelch(a, S.applied);
        S.lastTickAt = 0;
    }
    S.wasConnected = s.connected;
    syncTap(a, s);
    // the auto squelch, once per `sig` sample (≈ 20 a second): a new figure, or 50 ms since the last (a repeat)
    if (S.autoOn && s.last.count("sig")) {
        const double now = nowSec();
        if (s.chanDb != S.lastChan || s.floorDb != S.lastFloor || now - S.lastTickAt >= 0.05) {
            S.lastChan = s.chanDb; S.lastFloor = s.floorDb; S.lastTickAt = now;
            autoSquelchTick(a, s);
        }
    }
    // the record buffer forgets on a retune beyond the mode's allowance (main.ts recBufCheck)
    if (S.rbSec > 0 && s.vfoHz > 0 && S.rbAnchor.update(s.vfoHz, s.mode, nowSec())) S.rec.clearBuffer();
    // NFM AUDIO: the listener's choice wins over a report that disagrees (re-stated, at most every 2 s)
    if (S.nfmPref >= 0 && s.msg("hwinfo").has("nfmvoice") && s.msg("hwinfo")["nfmvoice"].boolean() != (S.nfmPref == 1)
        && nowSec() - S.nfmResentAt > 2 && nowSec() - (S.press.count("nfmvoice") ? S.press["nfmvoice"].first : 0) > PRESS_GUARD_S) {
        a.eng_->send(std::string("{\"type\":\"nfmvoice\",\"on\":") + jb(S.nfmPref == 1) + "}");
        S.nfmResentAt = nowSec();
    }
    if (S.rec.recording()) a.num_["audio.recSec"] = S.rec.seconds();   // ★ for the deck's status line (#mRecTime)
    a.num_["audio.mute"] = S.mute ? 1 : 0;   // ★ for the deck's speaker key (#mAudio's muted glyph follows THIS mute)
    a.num_["audio.vol"] = S.vol;
    // what the workers handed back
    {
        std::lock_guard<std::mutex> lk(arec::mail().m);
        auto& mb = arec::mail();
        while (!mb.toasts.empty()) { a.toast(mb.toasts.front()); mb.toasts.pop_front(); }
        if (mb.listReady) { S.recs = mb.list; S.recsLoaded = true; mb.listReady = false; }
        if (mb.uploading < S.uploadingSeen && S.recsView) arec::fetchList(a.eng_->port());   // a save landed: show it
        S.uploadingSeen = mb.uploading;
    }
}

/** ★ CALL ON EVERY SPEC FRAME (beside updateSignal, bins low → high): the auto squelch's reference — how far the
 *  channel stands above its empty neighbours on this view (src/services/squelchNeighbours.ts channelExcessDb). Only
 *  worked out while AUTO SQUELCH is on. Without it the tracker still runs, on the channel's own baseline. */
inline void audioSpecFrame(App& a, const uint8_t* bins, int n, double centreHz, double spanHz) {
    using namespace aud;
    State& S = st();
    if (!S.autoOn || !a.eng_) return;
    const RadioState& s = a.eng_->state();
    S.nearExcess = NAN;
    S.nearAt = nowSec();
    S.nearKey = channelKey(s);
    if (n < 16 || !(spanHz > 0)) return;
    const double binHz = spanHz / n, widthHz = (double)s.bwHigh - s.bwLow;
    if (!(widthHz > 0) || widthHz / binHz < 4) return;
    const double startHz = centreHz - spanHz / 2;
    const int edge = (int)std::ceil(n * 0.02);
    auto peakIn = [&](double loHz) -> double {
        const int i0 = (int)std::floor((loHz - startHz) / binHz), i1 = (int)std::ceil((loHz + widthHz - startHz) / binHz);
        if (i0 < edge || i1 > n - edge) return NAN;
        double pk = -1e9;
        for (int i = i0; i < i1; i++) pk = std::max(pk, (double)bins[i] - 256);   // (the engine's binary8: dB = byte − 256)
        return pk;
    };
    const double chan = peakIn(s.vfoHz + s.bwLow);
    if (!std::isfinite(chan)) return;
    std::vector<double> nb;
    for (int m = 2; m <= 6; m++) for (int sg : {-1, 1}) { const double v = peakIn(s.vfoHz + s.bwLow + sg * m * widthHz); if (std::isfinite(v)) nb.push_back(v); }
    if (nb.size() < 3) return;
    std::sort(nb.begin(), nb.end());
    S.nearExcess = chan - nb[(size_t)((nb.size() - 1) * 0.25)];
}

inline void App::drawAudioSheet(Ctx& c, const RadioState& s) {
    using namespace aud;
    State& S = st();
    audioTick(*this, s);     // (idempotent within a frame — the main loop should call it every frame as well)
    const bool recs = S.recsView;
    bool closed = false;
    const Rect area = sheetPanel(c, {12, 10, 616, 460}, recs ? "RECORDINGS" : "AUDIO", recs ? "r.close" : "a.close", closed);
    if (closed) {
        if (recs) { S.recsView = false; audioScroll_ = {}; nav_.setFocus("audio", "a.recs"); }
        else { S.navFix.clear(); closeTop(); }
        return;
    }
    const Rect view{area.x - 8, area.y, area.w + 16, area.h};
    c.g.clip(view.x, view.y, view.w, view.h);
    float y = area.y - audioScroll_.y;
    const float x0 = area.x + 6, xr = area.r() - 6, w = xr - x0;
    const double now = nowSec();
    BtnStyle bst; bst.font = &c.f.btn;

    if (recs) {
        // ════ RECORDINGS (§5.2) — the card's Recordings/, newest first; ✕ twice within 3 s deletes ════
        // ★ No playback: the web's is an HTML <audio> element (browser audio, not the engine's speaker), and the
        //   engine has no "play a file" path yet. Listed and deleted only.
        if (S.recsLoaded && S.recs.empty()) {
            const std::string t1 = "No recordings on the card yet — press ", t2 = " REC.";
            const float ty = y + 10;
            float tx = x0 + c.tc.draw(c.g, c.f.body, t1, x0, ty, th::dim);
            c.g.circle(tx + 7, ty + c.f.body.lineHeight() / 2.f, 5, 0, th::dim);   // ● (not in the font)
            c.tc.draw(c.g, c.f.body, t2, tx + 14, ty, th::dim);
            y += 50;
        }
        for (size_t i = 0; i < S.recs.size(); i++) {
            const Json& r = S.recs[i];
            std::string name = r["name"].str();
            if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".wav") == 0 || name.compare(name.size() - 4, 4, ".WAV") == 0)) name.resize(name.size() - 4);
            const std::string id = "r.del#" + r["name"].str();
            const bool armed = S.armedDel == r["name"].str() && now - S.armedAt < 3;
            const float bw = armed ? 120 : 56, rh = 48;
            const Rect del{xr - bw, y + 2, bw, rh - 4};
            c.g.rrect(x0 - 4, y, w + 8, rh, 8, th::btnBg);
            float tx = x0 + 8;
            const std::string size = " · " + arec::formatSize(r["bytes"].num());
            const float maxName = del.x - 12 - tx - (float)c.tc.width(c.f.small, size);
            std::string shown = name;
            while (shown.size() > 4 && c.tc.width(c.f.body, shown) > maxName) {
                shown.pop_back();
                while (!shown.empty() && ((unsigned char)shown.back() & 0xC0) == 0x80) shown.pop_back();   // whole UTF-8 characters
                if (c.tc.width(c.f.body, shown + "…") <= maxName) { shown += "…"; break; }
            }
            tx += c.tc.draw(c.g, c.f.body, shown, tx, y + rh / 2 - c.f.body.lineHeight() / 2.f, th::btnText);
            c.tc.draw(c.g, c.f.small, size, tx, y + rh / 2 - c.f.small.lineHeight() / 2.f, th::dim);
            BtnStyle ds; ds.font = &c.f.bodyB; ds.on = armed; ds.radius = 7;
            if (button(c, id, del, armed ? "DELETE?" : "", ds)) {
                if (armed) { arec::remove(eng_->port(), r["name"].str()); S.armedDel.clear(); }
                else { S.armedDel = r["name"].str(); S.armedAt = now; }
            }
            if (!armed) crossGlyph(c, del.cx(), del.cy(), 8, th::btnText);   // ✕ (not in the font)
            y += rh + 8;
        }
        c.g.unclip();
        audioScroll_.contentH = y + audioScroll_.y - area.y;
        audioScroll_.follow(c.nav.focusedRect(), view);
        // the back button returns to the AUDIO sheet (the web's recordings panel has its own CLOSE)
        if (c.nav.input().back && c.nav.backPressed()) { S.recsView = false; audioScroll_ = {}; nav_.setFocus("audio", "a.recs"); }
        if (nav_.focus("audio").empty() || nav_.focus("audio") == "a.close") nav_.setFocus("audio", "r.close");
        return;
    }

    Rect procView{0, 0, 0, 0};     // the highlighted PROCESSING switch with its help, when one is highlighted
    if (!S.navFix.empty()) { nav_.setFocus("audio", S.navFix); S.navFix.clear(); }
    S.navRows.clear();
    auto navRow = [&](std::initializer_list<std::pair<std::string, float>> items) { S.navRows.emplace_back(items); };
    navRow({{"a.close", area.r() - 50}});
    // ════ SQUELCH (VFO A) — §3.1 — never on an Icom ════
    const bool icom = s.hwDriver == "icom";
    if (!icom) {
        rowLabel(c, "SQUELCH", x0, y + 16);
        {
            std::string rd = S.autoOn ? "AUTO" : sqlOn(S.applied) ? fmtDb(S.applied) + " dBFS" : "OFF";
            if (S.autoOn && std::isfinite(S.shown)) rd = "AUTO · " + fmtDb(S.shown) + " dBFS";
            c.tc.draw(c.g, c.f.body, rd, xr, y + 16 - c.f.body.lineHeight() / 2.f, th::dim, 2);
        }
        y += 36;
        const Rect bar{x0, y, w, 32};
        const Nav::Hit h = c.nav.item("a.sql", bar, true);
        navRow({{"a.sql", bar.cx()}});
        if (h.adjust) {
            // ★ the web's drag, on the D-pad: 1 dB a press (3 dB when pressed in quick succession); fully left = OFF.
            //   From OFF, the first press right puts the threshold on the live level the fill shows (the web's ball
            //   starts at the far left — 100 dB of single presses below anything a channel reads).
            const double step = now - S.lastAdjAt < 0.35 ? 3 : 1;
            S.lastAdjAt = now;
            double base = S.applied;
            if (S.autoOn) { base = std::isfinite(S.shown) ? std::round(S.shown) : S.manual; setSquelchAuto(*this, s, false); }   // a drag while AUTO is on turns AUTO off first
            double db;
            if (!sqlOn(base)) db = h.adjust > 0 ? std::round(SCALE_MIN + std::max(0.0, std::min(1.0, sigSmooth_)) * (SCALE_MAX - SCALE_MIN)) : SQL_OFF;
            else { db = base + h.adjust * step; if (db < SCALE_MIN) db = SQL_OFF; if (db > SCALE_MAX) db = SCALE_MAX; }
            applySquelch(*this, db, true);
        }
        const bool on = sqlOn(S.applied);
        const bool shut = on && lastSigDb_ < S.applied;   // ★ the RAW level (the smoothed fill decays too slowly to judge by)
        c.g.rrect(bar.x, bar.y, bar.w, bar.h, 6, rgb(0x000000, 0.45f));
        const float fw = (float)std::max(0.0, std::min(1.0, sigSmooth_)) * (bar.w - 2);
        if (fw > 2) c.g.rrect(bar.x + 1, bar.y + 1, fw, bar.h - 2, 5, shut ? rgb(0xe05050, 0.85f) : rgb(0xdddddd, 0.88f));
        c.g.rframe(bar.x, bar.y, bar.w, bar.h, 6, 1, th::btnBorder);
        const double shownDb = S.autoOn && std::isfinite(S.shown) ? S.shown : S.applied;
        const float frac = on || (S.autoOn && std::isfinite(S.shown))
            ? (float)std::max(0.0, std::min(1.0, (shownDb - SCALE_MIN) / (SCALE_MAX - SCALE_MIN))) : 0.f;
        const float bx = bar.x + frac * bar.w;
        if (S.autoOn) {
            c.g.rect(bx - 1.5f, bar.y - 4, 3, bar.h + 8, th::red);   // the red line that tracks the noise
            const float tw = (float)c.tc.width(c.f.small, "AUTO SQUELCH ACTIVE") + 16;
            c.g.rrect(bar.cx() - tw / 2, bar.y + 5, tw, bar.h - 10, 5, rgb(0x000000, 0.6f));
            textIn(c, c.f.small, "AUTO SQUELCH ACTIVE", bar, th::white, 1);
        }
        c.g.circle(bx, bar.cy(), 12, 0, rgb(0x2e9d5e));
        c.g.circle(bx, bar.cy(), 12, 1.5f, rgb(0xbff5d2, 0.8f));
        if (h.focused) focusRing(c, bar, 6, true);
        y += 32 + 14;
        // AUTO SQUELCH + ABOVE NOISE (§3.2) — only where the server reports `sig` (nothing to track otherwise)
        if (s.last.count("sig")) {
            BtnStyle as = bst; as.lit = S.autoOn;
            // ★ full width, the margin on its own line under it: the D-pad goes straight down bar → AUTO → ABOVE NOISE
            //   (beside each other, the slider takes LEFT and the switch could only be reached from below)
            if (button(c, "a.sqlAuto", {x0, y, w, 46}, "AUTO SQUELCH", as)) setSquelchAuto(*this, s, !S.autoOn);
            navRow({{"a.sqlAuto", x0 + w / 2}});
            y += 46 + 10;
            if (S.autoOn) {
                rowLabel(c, "ABOVE NOISE", x0, y + 22);
                const float sx = x0 + (float)c.tc.width(c.f.bodyB, "ABOVE NOISE") + 22, sw = xr - 76 - sx;
                double m = S.margin;
                navRow({{"a.sqlMargin", sx + sw / 2}});
                if (slider(c, "a.sqlMargin", {sx, y + 2, sw, 40}, m, 4, 20, 1)) {
                    S.margin = m; S.floorEwma = NAN; S.nearRef = NAN; S.seedKey = channelKey(s);   // re-judge at once
                    autoSquelchTick(*this, s);
                }
                c.tc.draw(c.g, c.f.body, "+" + fmtDb(S.margin) + " dB", xr, y + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
                y += 44 + 10;
            }
        }
        const char* note = S.autoOn
            ? "Auto — the red line tracks the measured noise floor and keeps the gate a few dB above it. Drag the bar to take over manually."
            : on ? "Audio passes only above the ball. The level you set is where it stays — the bar underneath moves with the signal, the threshold does not."
                 : "Off — audio always passes. Drag the ball up from the left to set a threshold.";
        wrapText(c, note, x0, y, w, th::dim);
        if (S.autoOn) {
            y += 4;
            wrapText(c, "How far above the channel's own noise the squelch opens. Lower hears weaker signals and lets more noise through; higher is quieter but can clip the start of a weak over. The red line on the bar moves with it.", x0, y, w, th::dimmer);
        }
        y += 18;
    }

    // ════ VOLUME + mute — §3.5 (the ENGINE's speaker level; R36S HARDWARE → VOLUME is the codec) ════
    {
        rowLabel(c, "VOLUME", x0, y + 24);
        const float vx = x0 + (float)c.tc.width(c.f.bodyB, "VOLUME") + 16;
        const Rect mk{vx, y + 2, 44, 44};
        BtnStyle ms; ms.on = S.mute; ms.radius = 8;
        if (button(c, "a.mute", mk, "", ms)) { S.mute = !S.mute; sendLevel(*this); }
        speakerGlyph(c, mk.cx() - 2, mk.cy(), S.mute ? th::red : th::btnText, S.mute);
        double v = S.vol;
        const float sx = mk.r() + 18, sw = xr - 56 - sx;
        navRow({{"a.mute", mk.cx()}, {"a.vol", sx + sw / 2}});
        if (slider(c, "a.vol", {sx, y + 4, sw, 40}, v, 0, 100, 5)) {
            S.vol = (int)v;
            if (S.vol == 0) S.mute = true;             // setVolumePct(0) = mute (main.ts:9757)
            else if (S.mute) S.mute = false;           // moving the level up hears it again
            sendLevel(*this);
        }
        c.tc.draw(c.g, c.f.body, std::to_string(S.vol), xr, y + 24 - c.f.body.lineHeight() / 2.f, th::dim, 2);
        y += 48 + 18;
    }

    // ════ REC / RECORDINGS — §3.6 ════
    {
        const float bw = (w - 12) / 2;
        const Rect rr{x0, y, bw, 50};
        const bool recNow = S.rec.recording();
        BtnStyle rs = bst; rs.on = recNow; rs.disabled = !recNow && !s.connected;
        std::string lbl = "REC";
        if (recNow) { const int t = (int)S.rec.seconds(); char b[32]; std::snprintf(b, sizeof b, "STOP  %02d:%02d", t / 60, t % 60); lbl = b; }
        if (button(c, "a.rec", rr, "", rs) && now - S.recPressAt > 0.4) {
            S.recPressAt = now;
            if (recNow) stopRecording(*this, s); else startRecording(*this, s);
        }
        {
            // the record glyph: a ringed dot; while recording it BREATHES (#mRecTime's dot), red
            const float tw = (float)c.tc.width(c.f.btn, lbl), gx = rr.cx() - (tw + 26) / 2 + 8;
            const float br = recNow ? 0.55f + 0.45f * (float)std::sin(now * 4.0) : 1.f;
            Rgba dot = recNow ? th::red : th::btnText; dot.a *= rs.disabled ? 0.38f : br;
            c.g.circle(gx, rr.cy(), 8, 1.5f, dot);
            c.g.circle(gx, rr.cy(), 4.5f, 0, dot);
            Rgba tc = recNow ? th::activeTx : th::btnText; if (rs.disabled) tc.a *= 0.38f;
            c.tc.drawTab(c.g, c.f.btn, lbl, gx + 18, rr.cy() - c.f.btn.lineHeight() / 2.f, tc);
        }
        if (rs.disabled) navRow({{"a.recs", x0 + bw + 12 + bw / 2}});
        else navRow({{"a.rec", rr.cx()}, {"a.recs", x0 + bw + 12 + bw / 2}});
        if (button(c, "a.recs", {x0 + bw + 12, y, bw, 50}, "RECORDINGS", bst)) {
            S.recsView = true; S.recsLoaded = false; S.recs.clear(); S.armedDel.clear(); audioScroll_ = {};
            arec::fetchList(eng_->port());
            nav_.setFocus("audio", "r.close");
        }
        y += 50 + 18;
    }

    // ════ REC BUFFER — §3.7 (★ drawn as it IS: off on a fresh handheld, never the markup's lit 30 s) ════
    {
        rowLabel(c, "REC BUFFER", x0, y + 21);
        const int opts[3] = {0, 30, 60};
        int sel = 0;
        for (int i = 0; i < 3; i++) if (opts[i] == S.rbSec) sel = i;
        const int hit = segmented(c, "a.rb", {xr - 198, y, 198, 42}, {"OFF", "30 s", "60 s"}, sel, 6);
        navRow({{"a.rb#0", xr - 198 + 31}, {"a.rb#1", xr - 99}, {"a.rb#2", xr - 31}});
        if (hit >= 0 && opts[hit] != S.rbSec) {
            S.rbSec = opts[hit];
            S.rec.setBuffer(S.rbSec);
            S.rbAnchor = Anchor{}; S.rbAnchor.windowS = S.rbSec;
            syncTap(*this, s);
        }
        y += 42 + 8;
        wrapText(c, "Record Buffer: when enabled, VibeSDR holds a live buffer of up to 30 or 60 seconds of audio, which can allow for recording of easily missed signals. This buffer is cleared on retune, so the previous station's audio is not included.", x0, y, w, th::dim);
        y += 18;
    }

    // ════ NOISE REDUCTION — §3.10 ════
    {
        rowLabel(c, "NOISE REDUCTION", x0, y + 22);
        const int cur = nrNow(s);
        double v = cur;
        const float sx = x0 + (float)c.tc.width(c.f.bodyB, "NOISE REDUCTION") + 22, sw = xr - 64 - sx;
        navRow({{"a.nr", sx + sw / 2}});
        if (slider(c, "a.nr", {sx, y + 2, sw, 40}, v, 0, 100, 5)) {
            const int p = (int)v;
            pressed("nr", p);
            char b[96];
            std::snprintf(b, sizeof b, "{\"type\":\"nr\",\"on\":%s,\"strength\":%.4f}", jb(p > 0), nrStrength(p));
            eng_->send(b);
        }
        c.tc.draw(c.g, c.f.body, cur > 0 ? std::to_string(cur) + "%" : "OFF", xr, y + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
        y += 44 + 18;
    }

    // a switch row: label, ON/OFF chip right, then the help (verbatim)
    auto switchRow = [&](const std::string& id, const std::string& label, const std::string& key, bool def,
                         const std::string& onT, const std::string& offT, const std::string& help) {
        rowLabel(c, label, x0, y + 21);
        const bool on = key == "nfmvoice" && st().nfmPref >= 0 ? st().nfmPref == 1 : dspBool(s, key, def);
        const float tw = std::max(96.f, (float)std::max(c.tc.width(c.f.bodyB, onT), c.tc.width(c.f.bodyB, offT)) + 28);
        navRow({{id, xr - tw / 2}});
        if (toggle(c, id, {xr - tw, y, tw, 42}, on, onT, offT)) {
            pressed(key, on ? 0 : 1);
            if (key == "nfmvoice") st().nfmPref = on ? 0 : 1;
            eng_->send("{\"type\":\"" + key + "\",\"on\":" + jb(!on) + "}");
        }
        y += 42 + 8;
        if (!help.empty()) wrapText(c, help, x0, y, w, th::dim);
        y += 14;
    };

    // ════ AUTO NOTCH — §3.11 ════
    switchRow("a.notch", "AUTO NOTCH", "notch", false, "ON", "OFF", "");
    // ════ NOISE BLANKER — §3.13: once the radio reports it; not for broadcast FM (it has its own NB below) ════
    const bool wfm = s.mode == "wfm";
    if (s.msg("hwinfo").has("nbx") && !wfm)
        switchRow("a.nbx", "NOISE BLANKER", "nbx", false, "ON", "OFF",
                  "Impulse noise blanker on your own channel — the crackle of distant lightning, power-line hash, ignition, thermostats, switch-mode supplies. Removes the moments an impulse occupies rather than filtering continuously; on a clean signal it blanks nothing. Not for broadcast FM, which has its own NB below. The owner may also run a wider one on the whole receiver.");
    // ════ NFM AUDIO — §3.14: NFM only, once the radio reports it ════
    if (s.msg("hwinfo").has("nfmvoice") && (s.mode == "nfm" || s.mode == "fm"))
        switchRow("a.nfmv", "NFM AUDIO", "nfmvoice", true, "VOICE", "RAW",
                  "VOICE: 300 Hz to 3 kHz, as a normal NFM radio sounds — removes the repeater's sub-audible CTCSS tone and softens the hiss. RAW: the flat, unfiltered demodulator output, for piping into an external decoder (pagers, 9600-baud packet) that needs the low frequencies.");

    // ════ BROADCAST FM — §3.15, §3.16: WFM only (they act on nothing else), never an Icom ════
    if (wfm && !icom) {
        y += 4;
        c.g.rect(x0, y, w, 1, th::btnBorder);
        y += 10;
        {
            float tx = x0;
            for (char ch : std::string("BROADCAST FM")) { const std::string one(1, ch); tx += c.tc.draw(c.g, c.f.small, one, tx, y, th::dim) + 3; }
        }
        y += c.f.small.lineHeight() + 10;
        // FM DE-EMPHASIS
        rowLabel(c, "FM DE-EMPHASIS", x0, y + 21);
        {
            const int taus[3] = {0, 50, 75};
            const int cur = deemphNow(s);
            int sel = -1;
            for (int i = 0; i < 3; i++) if (taus[i] == cur) sel = i;
            const int hit = segmented(c, "a.deemph", {xr - 252, y, 252, 42}, {"OFF", "50µs", "75µs"}, sel, 6);
            navRow({{"a.deemph#0", xr - 252 + 40}, {"a.deemph#1", xr - 126}, {"a.deemph#2", xr - 40}});
            if (hit >= 0) {
                pressed("deemph", taus[hit]);
                eng_->send(std::string("{\"type\":\"deemph\",\"tau\":") + (hit == 0 ? "0" : hit == 1 ? "5e-5" : "7.5e-5") + "}");
            }
        }
        y += 42 + 18;
        // PROCESSING — six switches; the highlighted one's help underneath (room kept for the longest, so nothing jumps)
        rowLabel(c, "PROCESSING", x0, y + 14);
        y += 34;
        struct P { const char* key; const char* name; bool def; const char* help; };
        std::vector<P> ps = {
            {"stereo", "ST", true, "Stereo decoding. Off forces mono — which is a legitimate thing to want on a weak signal, though NR now treats mono hiss too, so it is no longer the only way to escape it."},
            {"wsp", "NR", true, "NR — weak-signal noise treatment: stereo high-blend plus an audio high-cut. WORKS IN MONO TOO — the high-cut treats baseband hiss, which is there whether you are in stereo or not; only the stereo half needs L-R. Acts only on a noisy signal: a strong station is bypassed entirely. Off unless you (or the server's owner) switch it on — it is for a weak, hissy station."},
            {"ims", "IMS", true, "IMS — Intelligent Multipath Suppression, as on the TEF6686. Treats a REFLECTION that CEQ cannot correct: below 18 dB MPX S/N a channel equaliser would contort itself around noise, so it stands down, and until now nothing treated multipath as multipath in that range. Multipath is phase distortion and the 38 kHz stereo subcarrier suffers it far worse than the mono sum, so this blends toward mono in proportion to the measured multipath depth — suppressing the distortion specifically rather than retreating from noise. Stands down entirely while CEQ is engaged, because CEQ is correcting the same fault properly. Watch MULTIPATH in the Advanced RDS panel."},
            {"ceq", "CEQ", true, "CEQ — blind channel equaliser. Undoes a REFLECTION (multipath), which is distortion rather than weakness. FM is a constant-amplitude signal, so the equaliser adapts by driving the envelope back toward constant — no training signal needed. It engages only on a good signal with real multipath, because on noise it would amplify what it cannot fix. Watch MULTIPATH in the Advanced RDS panel: it shows the depth before and after."},
            {"autobw", "AUTO BW", false, "AUTO BW — automatic demodulator bandwidth, the TEF6686's trick. A strong station keeps the full width it was transmitted with; a weak one is narrowed toward 134 kHz, because at the edge of the channel there is more noise than signal. Driven by the MPX S/N, and it changes the filter WITHOUT interrupting the audio or the RDS. FM broadcast only. Watch AUTO BW in the Advanced RDS panel to see what it is choosing."},
            {"nb", "NB", true, "Noise blanker — impulse noise: ignition, thermostats, switch-mode supplies, electric fences. A different fault again: impulses are brief and enormous, so the cure is to remove the moments they occupy rather than to filter continuously. On a clean signal it blanks nothing. The BLANKER figure in the Advanced RDS panel shows how much it is actually removing."},
        };
        if (!autoBwForWfm(s)) ps.erase(ps.begin() + 4);   // ★ AUTO BW only where the radio says WFM has one (autoBwModes)
        const int cols = 3;
        const float gw = (w - (cols - 1) * 10) / cols, gh = 44;
        const std::string focus = nav_.focus("audio");
        const char* help = nullptr;
        float reserve = 0;
        for (size_t i = 0; i < ps.size(); i++) {
            const Rect r{x0 + (i % cols) * (gw + 10), y + (i / cols) * (gh + 10), gw, gh};
            const std::string id = std::string("a.p.") + ps[i].key;
            if (i % cols == 0) S.navRows.emplace_back();
            S.navRows.back().push_back({id, r.cx()});
            const bool on = dspBool(s, ps[i].key, ps[i].def);
            const std::string nm = ps[i].name;
            if (toggle(c, id, r, on, nm + " ON", nm + " OFF")) {
                pressed(ps[i].key, on ? 0 : 1);
                eng_->send(std::string("{\"type\":\"") + ps[i].key + "\",\"on\":" + jb(!on) + "}");
            }
            if (focus == id) help = ps[i].help;
            reserve = std::max(reserve, wrapHeight(c, ps[i].help, w));
        }
        const float gridTop = y;
        y += ((ps.size() + cols - 1) / cols) * (gh + 10) + 2;
        float hy = y;
        if (help) wrapText(c, help, x0, hy, w, th::dim);
        else wrapText(c, "Highlight a switch for what it does.", x0, hy, w, th::dimmer);
        if (help) procView = Rect{x0, gridTop, w, hy - gridTop};   // ★ scroll the switch AND its help into view
        y += reserve + 12;
    }

    c.g.unclip();
    audioScroll_.contentH = y + audioScroll_.y - area.y;
    audioScroll_.follow(procView.h > 0 ? &procView : c.nav.focusedRect(), view);
    // UP / DOWN: the next row, its control nearest across; LEFT / RIGHT: the neighbour in the same row, else stay
    // (a slider keeps LEFT / RIGHT for its value — Nav already used them) (see State::navRows)
    const Dir nd = c.nav.input().nav;
    const std::string cur = nav_.focus("audio");
    const bool isSlider = cur == "a.sql" || cur == "a.vol" || cur == "a.nr" || cur == "a.sqlMargin";
    if (c.nav.topLayer() && (nd == D_LEFT || nd == D_RIGHT) && !isSlider) {
        for (auto& row : S.navRows)
            for (size_t k = 0; k < row.size(); k++) {
                if (row[k].first != cur) continue;
                const long to = (long)k + (nd == D_RIGHT ? 1 : -1);
                S.navFix = to >= 0 && to < (long)row.size() ? row[(size_t)to].first : cur;
            }
    }
    if (c.nav.topLayer() && (nd == D_UP || nd == D_DOWN)) {
        for (size_t ri = 0; ri < S.navRows.size(); ri++)
            for (auto& it : S.navRows[ri]) {
                if (it.first != cur) continue;
                const long to = (long)ri + (c.nav.input().nav == D_DOWN ? 1 : -1);
                if (to < 0 || to >= (long)S.navRows.size()) break;
                float best = 1e9f;
                for (auto& cand : S.navRows[(size_t)to])
                    if (std::fabs(cand.second - it.second) < best) { best = std::fabs(cand.second - it.second); S.navFix = cand.first; }
            }
    }
    if (nav_.focus("audio").empty() || nav_.focus("audio") == "r.close") nav_.setFocus("audio", icom ? "a.mute" : "a.sql");
    // tests: VIBE_HHUI_NAVLOG=1 logs every highlight move on this sheet (a scripted walk checks every row is reachable)
    static const bool navLog = std::getenv("VIBE_HHUI_NAVLOG") != nullptr;
    static std::string lastLogged;
    if (navLog && nav_.focus("audio") != lastLogged) { lastLogged = nav_.focus("audio"); std::fprintf(stderr, "vibe-hhui: audio focus %s\n", lastLogged.c_str()); }
}


}  // namespace hh
